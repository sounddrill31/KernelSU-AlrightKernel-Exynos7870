#include <linux/ceph/ceph_debug.h>

#include <linux/file.h>
#include <linux/namei.h>
#include <linux/random.h>

#include "super.h"
#include "mds_client.h"
#include <linux/ceph/pagelist.h>

static u64 lock_secret;

static inline u64 secure_addr(void *addr)
{
	u64 v = lock_secret ^ (u64)(unsigned long)addr;
	/*
	 * Set the most significant bit, so that MDS knows the 'owner'
	 * is sufficient to identify the owner of lock. (old code uses
	 * both 'owner' and 'pid')
	 */
	v |= (1ULL << 63);
	return v;
}

void __init ceph_flock_init(void)
{
	get_random_bytes(&lock_secret, sizeof(lock_secret));
}

/**
 * Implement fcntl and flock locking functions.
 */
static int ceph_lock_message(u8 lock_type, u16 operation, struct file *file,
			     int cmd, u8 wait, struct file_lock *fl)
{
	struct inode *inode = file_inode(file);
	struct ceph_mds_client *mdsc = ceph_sb_to_client(inode->i_sb)->mdsc;
	struct ceph_mds_request *req;
	int err;
	u64 length = 0;
	u64 owner;

	req = ceph_mdsc_create_request(mdsc, operation, USE_AUTH_MDS);
	if (IS_ERR(req))
		return PTR_ERR(req);
	req->r_inode = inode;
	ihold(inode);
	req->r_num_caps = 1;

	/* mds requires start and length rather than start and end */
	if (LLONG_MAX == fl->fl_end)
		length = 0;
	else
		length = fl->fl_end - fl->fl_start + 1;

	owner = secure_addr(fl->fl_owner);

	dout("ceph_lock_message: rule: %d, op: %d, owner: %llx, pid: %llu, "
	     "start: %llu, length: %llu, wait: %d, type: %d", (int)lock_type,
	     (int)operation, owner, (u64)fl->fl_pid, fl->fl_start, length,
	     wait, fl->fl_type);

	req->r_args.filelock_change.rule = lock_type;
	req->r_args.filelock_change.type = cmd;
	req->r_args.filelock_change.owner = cpu_to_le64(owner);
	req->r_args.filelock_change.pid = cpu_to_le64((u64)fl->fl_pid);
	req->r_args.filelock_change.start = cpu_to_le64(fl->fl_start);
	req->r_args.filelock_change.length = cpu_to_le64(length);
	req->r_args.filelock_change.wait = wait;

	err = ceph_mdsc_do_request(mdsc, inode, req);

	if (operation == CEPH_MDS_OP_GETFILELOCK) {
		fl->fl_pid = le64_to_cpu(req->r_reply_info.filelock_reply->pid);
		if (CEPH_LOCK_SHARED == req->r_reply_info.filelock_reply->type)
			fl->fl_type = F_RDLCK;
		else if (CEPH_LOCK_EXCL == req->r_reply_info.filelock_reply->type)
			fl->fl_type = F_WRLCK;
		else
			fl->fl_type = F_UNLCK;

		fl->fl_start = le64_to_cpu(req->r_reply_info.filelock_reply->start);
		length = le64_to_cpu(req->r_reply_info.filelock_reply->start) +
						 le64_to_cpu(req->r_reply_info.filelock_reply->length);
		if (length >= 1)
			fl->fl_end = length -1;
		else
			fl->fl_end = 0;

	}
	ceph_mdsc_put_request(req);
	dout("ceph_lock_message: rule: %d, op: %d, pid: %llu, start: %llu, "
	     "length: %llu, wait: %d, type: %d, err code %d", (int)lock_type,
	     (int)operation, (u64)fl->fl_pid, fl->fl_start,
	     length, wait, fl->fl_type, err);
	return err;
}

/**
 * Attempt to set an fcntl lock.
 * For now, this just goes away to the server. Later it may be more awesome.
 */
int ceph_lock(struct file *file, int cmd, struct file_lock *fl)
{
	u8 lock_cmd;
	int err;
	u8 wait = 0;
	u16 op = CEPH_MDS_OP_SETFILELOCK;

	if (!(fl->fl_flags & FL_POSIX))
		return -ENOLCK;
	/* No mandatory locks */
	if (__mandatory_lock(file->f_mapping->host) && fl->fl_type != F_UNLCK)
		return -ENOLCK;

	dout("ceph_lock, fl_owner: %p", fl->fl_owner);

	/* set wait bit as appropriate, then make command as Ceph expects it*/
	if (IS_GETLK(cmd))
		op = CEPH_MDS_OP_GETFILELOCK;
	else if (IS_SETLKW(cmd))
		wait = 1;

	if (F_RDLCK == fl->fl_type)
		lock_cmd = CEPH_LOCK_SHARED;
	else if (F_WRLCK == fl->fl_type)
		lock_cmd = CEPH_LOCK_EXCL;
	else
		lock_cmd = CEPH_LOCK_UNLOCK;

	err = ceph_lock_message(CEPH_LOCK_FCNTL, op, file, lock_cmd, wait, fl);
	if (!err) {
		if (op != CEPH_MDS_OP_GETFILELOCK) {
			dout("mds locked, locking locally");
			err = posix_lock_file(file, fl, NULL);
			if (err && (CEPH_MDS_OP_SETFILELOCK == op)) {
				/* undo! This should only happen if
				 * the kernel detects local
				 * deadlock. */
				ceph_lock_message(CEPH_LOCK_FCNTL, op, file,
						  CEPH_LOCK_UNLOCK, 0, fl);
				dout("got %d on posix_lock_file, undid lock",
				     err);
			}
		}

	} else if (err == -ERESTARTSYS) {
		dout("undoing lock\n");
		ceph_lock_message(CEPH_LOCK_FCNTL, op, file,
				  CEPH_LOCK_UNLOCK, 0, fl);
	}
	return err;
}

int ceph_flock(struct file *file, int cmd, struct file_lock *fl)
{
	u8 lock_cmd;
	int err;
	u8 wait = 0;

	if (!(fl->fl_flags & FL_FLOCK))
		return -ENOLCK;
	/* No mandatory locks */
	if (__mandatory_lock(file->f_mapping->host) && fl->fl_type != F_UNLCK)
		return -ENOLCK;

	dout("ceph_flock, fl_file: %p", fl->fl_file);

	if (IS_SETLKW(cmd))
		wait = 1;

	if (F_RDLCK == fl->fl_type)
		lock_cmd = CEPH_LOCK_SHARED;
	else if (F_WRLCK == fl->fl_type)
		lock_cmd = CEPH_LOCK_EXCL;
	else
		lock_cmd = CEPH_LOCK_UNLOCK;

	err = ceph_lock_message(CEPH_LOCK_FLOCK, CEPH_MDS_OP_SETFILELOCK,
				file, lock_cmd, wait, fl);
	if (!err) {
		err = flock_lock_file_wait(file, fl);
		if (err) {
			ceph_lock_message(CEPH_LOCK_FLOCK,
					  CEPH_MDS_OP_SETFILELOCK,
					  file, CEPH_LOCK_UNLOCK, 0, fl);
			dout("got %d on flock_lock_file_wait, undid lock", err);
		}
	} else if (err == -ERESTARTSYS) {
		dout("undoing lock\n");
		ceph_lock_message(CEPH_LOCK_FLOCK,
				  CEPH_MDS_OP_SETFILELOCK,
				  file, CEPH_LOCK_UNLOCK, 0, fl);
	}
	return err;
}

/*
 * Fills in the passed counter variables, so you can prepare pagelist metadata
 * before calling ceph_encode_locks.
 *
 * FIXME: add counters to struct file_lock_context so we don't need to do this?
 */
void ceph_count_locks(struct inode *inode, int *fcntl_count, int *flock_count)
{
	struct file_lock *lock;
	struct file_lock_context *ctx;

	*fcntl_count = 0;
	*flock_count = 0;

	for (lock = inode->i_flock; lock != NULL; lock = lock->fl_next) {
		if (lock->fl_flags & FL_POSIX)
			++(*fcntl_count);
	}

	ctx = inode->i_flctx;
	if (ctx) {
		spin_lock(&ctx->flc_lock);
		list_for_each_entry(lock, &ctx->flc_posix, fl_list)
			++(*fcntl_count);
		list_for_each_entry(lock, &ctx->flc_flock, fl_list)
			++(*flock_count);
		spin_unlock(&ctx->flc_lock);
	}
	dout("counted %d flock locks and %d fcntl locks",
	     *flock_count, *fcntl_count);
}

/**
 * Encode the flock and fcntl locks for the given inode into the ceph_filelock
 * array. Must be called with inode->i_lock already held.
 * If we encounter more of a specific lock type than expected, return -ENOSPC.
 */
int ceph_encode_locks_to_buffer(struct inode *inode,
				struct ceph_filelock *flocks,
				int num_fcntl_locks, int num_flock_locks)
{
	struct file_lock *lock;
	struct file_lock_context *ctx = inode->i_flctx;
	int err = 0;
	int seen_fcntl = 0;
	int seen_flock = 0;
	int l = 0;

	dout("encoding %d flock and %d fcntl locks", num_flock_locks,
	     num_fcntl_locks);

	for (lock = inode->i_flock; lock != NULL; lock = lock->fl_next) {
		if (lock->fl_flags & FL_POSIX) {
			++seen_fcntl;
			if (seen_fcntl > num_fcntl_locks) {
				err = -ENOSPC;
				goto fail;
			}
			err = lock_to_ceph_filelock(lock, &flocks[l]);
			if (err)
				goto fail;
			++l;
		}
	}

	spin_lock(&ctx->flc_lock);
	list_for_each_entry(lock, &ctx->flc_flock, fl_list) {
		++seen_fcntl;
		if (seen_fcntl > num_fcntl_locks) {
			err = -ENOSPC;
			goto fail;
		}
		err = lock_to_ceph_filelock(lock, &flocks[l]);
		if (err)
			goto fail;
		++l;
	}
	list_for_each_entry(lock, &ctx->flc_flock, fl_list) {
		++seen_flock;
		if (seen_flock > num_flock_locks) {
			err = -ENOSPC;
			goto fail;
		}
		err = lock_to_ceph_filelock(lock, &flocks[l]);
		if (err)
			goto fail;
		++l;
	}
fail:
	spin_unlock(&ctx->flc_lock);
	return err;
}

/**
 * Copy the encoded flock and fcntl locks into the pagelist.
 * Format is: #fcntl locks, sequential fcntl locks, #flock locks,
 * sequential flock locks.
 * Returns zero on success.
 */
int ceph_locks_to_pagelist(struct ceph_filelock *flocks,
			   struct ceph_pagelist *pagelist,
			   int num_fcntl_locks, int num_flock_locks)
{
	int err = 0;
	__le32 nlocks;

	nlocks = cpu_to_le32(num_fcntl_locks);
	err = ceph_pagelist_append(pagelist, &nlocks, sizeof(nlocks));
	if (err)
		goto out_fail;

	err = ceph_pagelist_append(pagelist, flocks,
				   num_fcntl_locks * sizeof(*flocks));
	if (err)
		goto out_fail;

	nlocks = cpu_to_le32(num_flock_locks);
	err = ceph_pagelist_append(pagelist, &nlocks, sizeof(nlocks));
	if (err)
		goto out_fail;

	err = ceph_pagelist_append(pagelist,
				   &flocks[num_fcntl_locks],
				   num_flock_locks * sizeof(*flocks));
out_fail:
	return err;
}

/*
 * Given a pointer to a lock, convert it to a ceph filelock
 */
int lock_to_ceph_filelock(struct file_lock *lock,
			  struct ceph_filelock *cephlock)
{
	int err = 0;
	cephlock->start = cpu_to_le64(lock->fl_start);
	cephlock->length = cpu_to_le64(lock->fl_end - lock->fl_start + 1);
	cephlock->client = cpu_to_le64(0);
	cephlock->pid = cpu_to_le64((u64)lock->fl_pid);
	cephlock->owner = cpu_to_le64(secure_addr(lock->fl_owner));

	switch (lock->fl_type) {
	case F_RDLCK:
		cephlock->type = CEPH_LOCK_SHARED;
		break;
	case F_WRLCK:
		cephlock->type = CEPH_LOCK_EXCL;
		break;
	case F_UNLCK:
		cephlock->type = CEPH_LOCK_UNLOCK;
		break;
	default:
		dout("Have unknown lock type %d", lock->fl_type);
		err = -EINVAL;
	}

	return err;
}
