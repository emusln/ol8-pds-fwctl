/*
 * Copyright (c) 2007, 2020 Oracle and/or its affiliates.
 *
 * This software is available to you under a choice of one of two
 * licenses.  You may choose to be licensed under the terms of the GNU
 * General Public License (GPL) Version 2, available from the file
 * COPYING in the main directory of this source tree, or the
 * OpenIB.org BSD license below:
 *
 *     Redistribution and use in source and binary forms, with or
 *     without modification, are permitted provided that the following
 *     conditions are met:
 *
 *      - Redistributions of source code must retain the above
 *        copyright notice, this list of conditions and the following
 *        disclaimer.
 *
 *      - Redistributions in binary form must reproduce the above
 *        copyright notice, this list of conditions and the following
 *        disclaimer in the documentation and/or other materials
 *        provided with the distribution.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
 * BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
 * ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 *
 */
#include <linux/pagemap.h>
#include <linux/rbtree.h>
#include <linux/dma-mapping.h> /* for DMA_*_DEVICE */
#include <linux/sched/mm.h>

#include "trace.h"

#include "rds.h"

/*
 * XXX
 *  - build with sparse
 *  - should we detect duplicate keys on a socket?  hmm.
 *  - an rdma is an mlock, apply rlimit?
 */

/*
 * get the number of pages by looking at the page indices that the start and
 * end addresses fall in.
 *
 * Returns 0 if the vec is invalid.  It is invalid if the number of bytes
 * causes the address to wrap or overflows an unsigned int.  This comes
 * from being stored in the 'length' member of 'struct scatterlist'.
 */
static unsigned int rds_pages_in_vec(struct rds_iovec *vec)
{
	if ((vec->addr + vec->bytes <= vec->addr) ||
	    (vec->bytes > (u64)UINT_MAX))
		return 0;

	return ((vec->addr + vec->bytes + PAGE_SIZE - 1) >> PAGE_SHIFT) -
		(vec->addr >> PAGE_SHIFT);
}

static struct rds_mr *rds_mr_tree_walk(struct rb_root *root, u64 key,
				       struct rds_mr *insert)
{
	struct rb_node **p = &root->rb_node;
	struct rb_node *parent = NULL;
	struct rds_mr *mr;

	while (*p) {
		parent = *p;
		mr = rb_entry(parent, struct rds_mr, r_rb_node);

		if (key < mr->r_key)
			p = &(*p)->rb_left;
		else if (key > mr->r_key)
			p = &(*p)->rb_right;
		else
			return mr;
	}

	if (insert) {
		rb_link_node(&insert->r_rb_node, parent, p);
		rb_insert_color(&insert->r_rb_node, root);
		kref_get(&insert->r_kref);
	}
	return NULL;
}

/*
 * Destroy the transport-specific part of a MR.
 */
static unsigned int nmbr_uaf;
static void rds_destroy_mr(struct rds_mr *mr)
{
	struct rds_sock *rs = mr->r_sock;
	void *trans_private = NULL;
	unsigned long flags;

	if (READ_ONCE(rs->poison) != RED_ACTIVE) {
		++nmbr_uaf;
		pr_err_ratelimited("%s:%d: rs: %p poison: %llx number of UAF: %u\n",
				   __func__, __LINE__, rs, READ_ONCE(rs->poison), nmbr_uaf);
	}

	trace_rds_mr_destroy(rs, rs->rs_conn, mr, kref_read(&mr->r_kref),
			     NULL, 0);

	spin_lock_irqsave(&rs->rs_rdma_lock, flags);
	if (!RB_EMPTY_NODE(&mr->r_rb_node))
		rb_erase(&mr->r_rb_node, &rs->rs_rdma_keys);
	trans_private = mr->r_trans_private;
	mr->r_trans_private = NULL;
	spin_unlock_irqrestore(&rs->rs_rdma_lock, flags);

	if (trans_private)
		mr->r_trans->free_mr(trans_private, mr->r_invalidate);
}

void __rds_put_mr_final(struct kref *kref)
{
	struct rds_mr *mr = container_of(kref, struct rds_mr, r_kref);

	rds_destroy_mr(mr);
	rds_sock_put(mr->r_sock);
	kfree(mr);
}

/*
 * By the time this is called we can't have any more ioctls called on
 * the socket so we don't need to worry about racing with others.
 */
void rds_rdma_drop_keys(struct rds_sock *rs)
{
	struct rds_mr *mr;
	struct rb_node *node;
	unsigned long flags;
	char name[TASK_COMM_LEN];

	/* Release any MRs associated with this socket */
	get_task_comm(name, current);
	spin_lock_irqsave(&rs->rs_rdma_lock, flags);
	while ((node = rb_first(&rs->rs_rdma_keys))) {
		mr = container_of(node, struct rds_mr, r_rb_node);
		if (mr->r_trans == rs->rs_transport)
			mr->r_invalidate = 0;
		rb_erase(&mr->r_rb_node, &rs->rs_rdma_keys);
		RB_CLEAR_NODE(&mr->r_rb_node);
		spin_unlock_irqrestore(&rs->rs_rdma_lock, flags);
		kref_put(&mr->r_kref, __rds_put_mr_final);
		spin_lock_irqsave(&rs->rs_rdma_lock, flags);
	}
	spin_unlock_irqrestore(&rs->rs_rdma_lock, flags);
}
EXPORT_SYMBOL_GPL(rds_rdma_drop_keys);

/*
 * Helper function to pin user pages.
 */
static int rds_pin_pages(unsigned long user_addr, unsigned int nr_pages,
			struct page **pages, int write)
{
	int ret;
	struct mm_struct *mm = current->mm;
	int gup_flags = FOLL_LONGTERM | (write ? FOLL_WRITE : 0);

	mmgrab(mm);
	down_read(&mm->mmap_lock);
	ret = pin_user_pages(user_addr, nr_pages, gup_flags, pages, NULL);

	if (ret >= 0 && (unsigned) ret < nr_pages) {
		unpin_user_pages(pages, ret);
		ret = -EFAULT;
	}
	up_read(&mm->mmap_lock);
	mmdrop(mm);

	return ret;
}

static int __rds_rdma_map(struct rds_sock *rs, struct rds_get_mr_args *args,
			  u64 *cookie_ret, struct rds_mr **mr_ret,
			  struct rds_conn_path *cp)
{
	struct rds_mr *mr = NULL, *found;
	unsigned int nr_pages;
	struct page **pages = NULL;
	struct scatterlist *sg;
	void *trans_private;
	unsigned long flags;
	rds_rdma_cookie_t cookie;
	unsigned int nents;
	char *reason;
	u32 iova;
	long i;
	int ret;

	if (ipv6_addr_any(&rs->rs_bound_addr) || !rs->rs_transport) {
		ret = -ENOTCONN; /* XXX not a great errno */
		reason = "transport not set up";
		goto out;
	}

	if (!rs->rs_transport->get_mr) {
		ret = -EOPNOTSUPP;
		reason = "get_mr not supported";
		goto out;
	}

	/* Restrict the size of mr irrespective of underlying transport */
	if (args->vec.bytes > RDS_MAX_MSG_SIZE) {
		ret = -EMSGSIZE;
		reason = "message too big";
		goto out;
	}

	nr_pages = rds_pages_in_vec(&args->vec);
	if (nr_pages == 0) {
		ret = -EINVAL;
		reason = "no pages in vec";
		goto out;
	}

	rdsdebug("RDS: get_mr addr %llx len %llu nr_pages %u\n",
		args->vec.addr, args->vec.bytes, nr_pages);

	/* RDS_RDMA_INVALIDATE is not allowed when creating an MR */
	if (args->flags & ~(RDS_RDMA_USE_ONCE | RDS_RDMA_READWRITE)) {
		ret = -EINVAL;
		reason = "only USE_ONCE and READWRITE is supported";
		goto out;
	}

	/* XXX clamp nr_pages to limit the size of this alloc? */
	pages = kcalloc(nr_pages, sizeof(struct page *), GFP_KERNEL);
	if (!pages) {
		ret = -ENOMEM;
		reason = "alloc of pages failed";
		goto out;
	}

	mr = kzalloc(sizeof(struct rds_mr), GFP_KERNEL);
	if (!mr) {
		ret = -ENOMEM;
		reason = "alloc of mr failed";
		goto out;
	}

	kref_init(&mr->r_kref);
	RB_CLEAR_NODE(&mr->r_rb_node);
	mr->r_trans = rs->rs_transport;
	mr->r_sock = rs;
	rds_sock_addref(rs);

	if (args->flags & RDS_RDMA_USE_ONCE)
		mr->r_use_once = 1;
	if (args->flags & RDS_RDMA_READWRITE)
		mr->r_write = 1;

	/*
	 * Pin the pages that make up the user buffer and transfer the page
	 * pointers to the mr's sg array.  We check to see if we've mapped
	 * the whole region after transferring the partial page references
	 * to the sg array so that we can have one page ref cleanup path.
	 *
	 * For now we have no flag that tells us whether the mapping is
	 * r/o or r/w. We need to assume r/w, or we'll do a lot of RDMA to
	 * the zero page.
	 */
	ret = rds_pin_pages(args->vec.addr, nr_pages, pages, 1);
	if (ret < 0) {
		reason = "rds_pin_pages failed";
		goto out;
	}

	nents = ret;
	sg = kcalloc(nents, sizeof(*sg), GFP_KERNEL);
	if (!sg) {
		ret = -ENOMEM;
		reason = "alloc of sg failed";
		goto out;
	}
	WARN_ON(!nents);
	sg_init_table(sg, nents);

	/* Stick all pages into the scatterlist */
	for (i = 0 ; i < nents; i++)
		sg_set_page(&sg[i], pages[i], PAGE_SIZE, 0);

	rdsdebug("RDS: trans_private nents is %u\n", nents);

	/* Obtain a transport specific MR. If this succeeds, the
	 * s/g list is now owned by the MR.
	 * Note that dma_map() implies that pending writes are
	 * flushed to RAM, so no dma_sync is needed here.
	 *
	 * Note that the transport specific MR may become invalid after
	 * this get_mr() does not hold a reference on it.
	 */
	trans_private = rs->rs_transport->get_mr(sg, nents, rs,
						 &mr->r_key, &iova,
						 cp ? cp->cp_conn : NULL);

	if (IS_ERR(trans_private)) {
		unpin_user_pages(pages, nents);
		kfree(sg);
		ret = PTR_ERR(trans_private);
		reason = "get_mr failed for transport";
		goto out;
	}

	mr->r_trans_private = trans_private;

	rdsdebug("RDS: get_mr put_user key is %x cookie_addr %p\n",
	       mr->r_key, (void *)(unsigned long) args->cookie_addr);

	/* The user may pass us an unaligned address, but we can only
	 * map page aligned regions. So we keep the offset, and build
	 * a 64bit cookie containing <R_Key, offset> and pass that
	 * around. */
	cookie = rds_rdma_make_cookie(mr->r_key, iova | (args->vec.addr & ~PAGE_MASK));
	if (cookie_ret)
		*cookie_ret = cookie;

	mr->r_iova = iova | (args->vec.addr & ~PAGE_MASK);
	if (args->cookie_addr && put_user(cookie, (u64 __user *)(unsigned long) args->cookie_addr)) {
		ret = -EFAULT;
		reason = "invalid address for cookie";
		goto out;
	}

	/* Inserting the new MR into the rbtree bumps its
	 * reference count. */
	spin_lock_irqsave(&rs->rs_rdma_lock, flags);
	found = rds_mr_tree_walk(&rs->rs_rdma_keys, mr->r_key, mr);
	spin_unlock_irqrestore(&rs->rs_rdma_lock, flags);

	BUG_ON(found && found != mr);

	if (mr_ret) {
		kref_get(&mr->r_kref);
		*mr_ret = mr;
	}

	ret = 0;
out:
	kfree(pages);

	if (ret)
		trace_rds_mr_get_err(rs, rs->rs_conn, mr,
				     mr ? kref_read(&mr->r_kref) : 0,
				     reason, ret);
	else
		trace_rds_mr_get(rs, rs->rs_conn, mr,
				 kref_read(&mr->r_kref), NULL, 0);

	if (mr)
		kref_put(&mr->r_kref, __rds_put_mr_final);
	return ret;
}

int rds_get_mr(struct rds_sock *rs, sockptr_t optval, int optlen)
{
	struct rds_get_mr_args args;

	if (optlen != sizeof(struct rds_get_mr_args))
		return -EINVAL;

        if (copy_from_sockptr(&args, optval, sizeof(struct rds_get_mr_args)))
		return -EFAULT;

	return __rds_rdma_map(rs, &args, NULL, NULL, NULL);
}

int rds_get_mr_for_dest(struct rds_sock *rs, sockptr_t optval, int optlen)
{
	struct rds_get_mr_for_dest_args args;
	struct rds_get_mr_args new_args;

	if (optlen != sizeof(struct rds_get_mr_for_dest_args))
		return -EINVAL;

        if (copy_from_sockptr(&args, optval, sizeof(struct rds_get_mr_for_dest_args)))
		return -EFAULT;

	/*
	 * Initially, just behave like get_mr().
	 * TODO: Implement get_mr as wrapper around this
	 *	 and deprecate it.
	 */
	new_args.vec = args.vec;
	new_args.cookie_addr = args.cookie_addr;
	new_args.flags = args.flags;

	return __rds_rdma_map(rs, &new_args, NULL, NULL, NULL);
}

/*
 * Free the MR indicated by the given R_Key
 */
int rds_free_mr(struct rds_sock *rs, sockptr_t optval, int optlen)
{
	struct rds_free_mr_args args;
	struct rds_mr *mr;
	unsigned long flags;

	if (optlen != sizeof(struct rds_free_mr_args))
		return -EINVAL;

        if (copy_from_sockptr(&args, optval, sizeof(struct rds_free_mr_args)))
		return -EFAULT;

	/*
	 * 20366776 workaround.
	 * Let process spawn quickly by avoiding call to flush_mrs()
	 */
	if (args.cookie == 0 && args.flags == 0) {
		if (!rs->rs_transport || !rs->rs_transport->flush_mrs)
			return -EINVAL;
		return 0;
	}

	/* Special case - a null cookie means flush all unused MRs */
	if (args.cookie == 0) {
		if (!rs->rs_transport || !rs->rs_transport->flush_mrs)
			return -EINVAL;
		rs->rs_transport->flush_mrs();
		return 0;
	}

	/* Look up the MR given its R_key and remove it from the rbtree
	 * so nobody else finds it.
	 * This should also prevent races with rds_rdma_unuse.
	 */
	spin_lock_irqsave(&rs->rs_rdma_lock, flags);
	mr = rds_mr_tree_walk(&rs->rs_rdma_keys, rds_rdma_cookie_key(args.cookie), NULL);
	if (mr) {
		if (rds_rdma_make_cookie(mr->r_key, mr->r_iova) == args.cookie) {
			rb_erase(&mr->r_rb_node, &rs->rs_rdma_keys);
			RB_CLEAR_NODE(&mr->r_rb_node);
			if (args.flags & RDS_RDMA_INVALIDATE)
				mr->r_invalidate = 1;
		} else {
			mr = NULL;
		}
	}
	spin_unlock_irqrestore(&rs->rs_rdma_lock, flags);

	if (!mr)
		return -EINVAL;

	kref_put(&mr->r_kref, __rds_put_mr_final);
	return 0;
}

/*
 * This is called when we receive an extension header that
 * tells us this MR was used. It allows us to implement
 * use_once semantics
 */
void rds_rdma_unuse(struct rds_sock *rs, u32 r_key, int force)
{
	struct rds_mr *mr;
	unsigned long flags;
	int zot_me = 0;

	spin_lock_irqsave(&rs->rs_rdma_lock, flags);
	mr = rds_mr_tree_walk(&rs->rs_rdma_keys, r_key, NULL);
	if (!mr) {
		spin_unlock_irqrestore(&rs->rs_rdma_lock, flags);
		return;
	}

	/* Get a reference so that the MR won't go away before calling
	 * sync_mr() below.
	 */
	kref_get(&mr->r_kref);

	/* If it is going to be freed, remove it from the tree now so
	 * that no other thread can find it and free it.
	 */
	if (mr->r_use_once || force) {
		rb_erase(&mr->r_rb_node, &rs->rs_rdma_keys);
		RB_CLEAR_NODE(&mr->r_rb_node);
		zot_me = 1;
	}
	spin_unlock_irqrestore(&rs->rs_rdma_lock, flags);

	/* May have to issue a dma_sync on this memory region.
	 * Note we could avoid this if the operation was a RDMA READ,
	 * but at this point we can't tell. */
	if (mr->r_trans->sync_mr)
		mr->r_trans->sync_mr(mr->r_trans_private, DMA_FROM_DEVICE);

	/* Release the reference held above. */
	kref_put(&mr->r_kref, __rds_put_mr_final);

	/* If the MR was marked as invalidate, this will
	 * trigger an async flush. */
	if (zot_me)
		kref_put(&mr->r_kref, __rds_put_mr_final);
}

void rds_rdma_free_op(struct rm_rdma_op *ro)
{
	unsigned int i;

	for (i = 0; i < ro->op_nents; i++) {
		struct page *page = sg_page(&ro->op_sg[i]);

		/* Mark page dirty if it was possibly modified, which
		 * is the case for a RDMA_READ which copies from remote
		 * to local memory */
		if (!ro->op_write) {
			WARN_ON_ONCE(!page->mapping && irqs_disabled());
			set_page_dirty(page);
		}
		unpin_user_page(page);
	}

	kfree(ro->op_notifier);
	ro->op_notifier = NULL;
	ro->op_active = 0;
}

void rds_atomic_free_op(struct rm_atomic_op *ao)
{
	struct page *page = sg_page(ao->op_sg);

	/* Mark page dirty if it was possibly modified, which
	 * is the case for a RDMA_READ which copies from remote
	 * to local memory */
	set_page_dirty(page);
	unpin_user_page(page);

	kfree(ao->op_notifier);
	ao->op_notifier = NULL;
	ao->op_active = 0;
}

int rds_rdma_extra_size(struct rds_rdma_args *args, struct rds_iov_vector *iov)
{
	struct rds_iovec *vec;
	struct rds_iovec __user *local_vec;
	int tot_pages = 0;
	int i;

	if (args->nr_local == 0)
		return -EINVAL;

	if (args->nr_local > UIO_MAXIOV)
		return -EMSGSIZE;

	iov->iv_vec = kcalloc(args->nr_local,
			      sizeof(struct rds_iovec),
			      GFP_KERNEL);
	if (!iov->iv_vec)
		return -ENOMEM;
	iov->iv_nr_pages = kcalloc(args->nr_local,
				   sizeof(int),
				   GFP_KERNEL);
	if (!iov->iv_nr_pages)
		return -ENOMEM;

	vec = iov->iv_vec;
	local_vec = (struct rds_iovec __user *)(unsigned long)args->local_vec_addr;

	if (copy_from_user(vec, local_vec, args->nr_local *
			   sizeof(struct rds_iovec)))
		return -EFAULT;

	iov->iv_entries = args->nr_local;
	/* figure out the number of pages in the vector */
	for (i = 0; i < iov->iv_entries; i++, vec++) {
		int nr_pages = rds_pages_in_vec(vec);

		if (nr_pages == 0)
			return -EINVAL;

		iov->iv_nr_pages[i] = nr_pages;
		tot_pages += nr_pages;

		/* nr_pages for one entry is limited to (UINT_MAX>>PAGE_SHIFT)+1,
		 * so tot_pages cannot overflow without first going negative.
		 */
		if (tot_pages < 0)
			return -EINVAL;
	}
	iov->iv_tot_pages = tot_pages;
	return tot_pages * sizeof(struct scatterlist);
}

/*
 * The application asks for a RDMA transfer.
 * Extract all arguments and set up the rdma_op
 */
static int rds_cmsg_rdma_args(struct rds_sock *rs, struct rds_message *rm,
			      struct cmsghdr *cmsg, struct rds_iov_vector *iov)
{
	struct rds_rdma_args *args;
	struct rm_rdma_op *op = &rm->rdma;
	int nr_pages;
	unsigned int nr_bytes;
	struct page **pages = NULL;
	struct rds_iovec *vec;
	struct rds_iovec __user *local_vec;
	unsigned int i, j;
	int ret = 0;

	if (cmsg->cmsg_len < CMSG_LEN(sizeof(struct rds_rdma_args))
	    || rm->rdma.op_active)
		return -EINVAL;

	args = CMSG_DATA(cmsg);

	if (ipv6_addr_any(&rs->rs_bound_addr)) {
		ret = -ENOTCONN; /* XXX not a great errno */
		goto out_ret;
	}

	if (args->nr_local > (u64)UINT_MAX) {
		ret = -EMSGSIZE;
		goto out_ret;
	}
	if (iov->iv_entries != args->nr_local) {
		ret = -EINVAL;
		goto out_ret;
	}

	nr_pages = iov->iv_tot_pages;
	pages = kcalloc(nr_pages, sizeof(struct page *), GFP_KERNEL);
	if (!pages) {
		ret = -ENOMEM;
		goto out_ret;
	}

	op->op_write = !!(args->flags & RDS_RDMA_READWRITE);
	op->op_fence = !!(args->flags & RDS_RDMA_FENCE);
	op->op_notify = !!(args->flags & RDS_RDMA_NOTIFY_ME);
	op->op_silent = !!(args->flags & RDS_RDMA_SILENT);
	op->op_remote_complete = !!(args->flags & RDS_RDMA_REMOTE_COMPLETE);
	op->op_active = 1;
	op->op_recverr = rs->rs_recverr;
	WARN_ON(!nr_pages);
	op->op_sg = rds_message_alloc_sgs(rm, nr_pages);

	if (op->op_notify || op->op_recverr || rds_async_send_enabled) {
		/* We allocate an uninitialized notifier here, because
		 * we don't want to do that in the completion handler. We
		 * would have to use GFP_ATOMIC there, and don't want to deal
		 * with failed allocations.
		 */
		op->op_notifier = kzalloc(sizeof(struct rds_notifier), GFP_KERNEL);
		if (!op->op_notifier) {
			ret = -ENOMEM;
			goto out_pages;
		}
		op->op_notifier->n_user_token = args->user_token;
		op->op_notifier->n_status = RDS_RDMA_SEND_SUCCESS;
	}

	/* The cookie contains the R_Key of the remote memory region, and
	 * optionally an offset into it. This is how we implement RDMA into
	 * unaligned memory.
	 * When setting up the RDMA, we need to add that offset to the
	 * destination address (which is really an offset into the MR)
	 * FIXME: We may want to move this into ib_rdma.c
	 */
	op->op_rkey = rds_rdma_cookie_key(args->cookie);
	op->op_remote_addr = args->remote_vec.addr + rds_rdma_cookie_offset(args->cookie);

	nr_bytes = 0;

	rdsdebug("RDS: rdma prepare nr_local %llu rva %llx rkey %x\n",
	       (unsigned long long)args->nr_local,
	       (unsigned long long)args->remote_vec.addr,
	       op->op_rkey);

	local_vec = (struct rds_iovec __user *)(unsigned long) args->local_vec_addr;
	vec = iov->iv_vec;
	for (i = 0; i < args->nr_local; i++, vec++) {
		/* don't need to check, rds_rdma_pages() verified nr will be +nonzero */
		unsigned int nr = iov->iv_nr_pages[i];

		rs->rs_user_addr  = vec->addr;
		rs->rs_user_bytes = vec->bytes;

		/* If it's a WRITE operation, we want to pin the pages for reading.
		 * If it's a READ operation, we need to pin the pages for writing.
		 */
		ret = rds_pin_pages(vec->addr, nr, pages, !op->op_write);
		if (ret < 0)
			goto out_pages;

		nr_bytes += vec->bytes;

		for (j = 0; j < nr; j++) {
			unsigned int offset = vec->addr & ~PAGE_MASK;
			struct scatterlist *sg;

			sg = &op->op_sg[op->op_nents + j];
			sg_set_page(sg, pages[j],
					min_t(unsigned int, vec->bytes, PAGE_SIZE - offset),
					offset);

			vec->addr  += sg->length;
			vec->bytes -= sg->length;
		}

		op->op_nents += nr;
	}

	if (nr_bytes > args->remote_vec.bytes) {
		rdsdebug("RDS nr_bytes %u remote_bytes %u do not match\n",
				nr_bytes,
				(unsigned int) args->remote_vec.bytes);
		ret = -EINVAL;
		goto out_pages;
	}
	op->op_bytes = nr_bytes;

	ret = 0;
out_pages:
	kfree(pages);
out_ret:
	if (ret)
		rds_rdma_free_op(op);

	rds_stats_inc(s_send_rdma);

	return ret;
}

/*
 * The application wants us to pass an RDMA destination (aka MR)
 * to the remote
 */
static int rds_cmsg_rdma_dest(struct rds_sock *rs, struct rds_message *rm,
			      struct cmsghdr *cmsg)
{
	unsigned long flags;
	struct rds_mr *mr;
	u32 r_key;
	int err = 0;

	if (cmsg->cmsg_len < CMSG_LEN(sizeof(rds_rdma_cookie_t))
	 || rm->m_rdma_cookie != 0)
		return -EINVAL;

	memcpy(&rm->m_rdma_cookie, CMSG_DATA(cmsg), sizeof(rm->m_rdma_cookie));

	/* We are reusing a previously mapped MR here. Most likely, the
	 * application has written to the buffer, so we need to explicitly
	 * flush those writes to RAM. Otherwise the HCA may not see them
	 * when doing a DMA from that buffer.
	 */
	r_key = rds_rdma_cookie_key(rm->m_rdma_cookie);

	spin_lock_irqsave(&rs->rs_rdma_lock, flags);
	mr = rds_mr_tree_walk(&rs->rs_rdma_keys, r_key, NULL);
	if (!mr) {
		printk(KERN_ERR "rds_cmsg_rdma_dest: key %x\n", r_key);
		err = -EINVAL;	/* invalid r_key */
	} else {
		kref_get(&mr->r_kref);
	}
	spin_unlock_irqrestore(&rs->rs_rdma_lock, flags);

	if (mr) {
		mr->r_trans->sync_mr(mr->r_trans_private, DMA_TO_DEVICE);
		rm->rdma.op_rdma_mr = mr;
	}
	return err;
}

static void inc_rdma_map_pending(struct rds_conn_path *cp)
{
	atomic_inc(&cp->cp_rdma_map_pending);
}

static void dec_rdma_map_pending(struct rds_conn_path *cp)
{
	if (atomic_dec_and_test(&cp->cp_rdma_map_pending)) {
		if (waitqueue_active(&cp->cp_waitq))
			wake_up_all(&cp->cp_waitq);
		if (test_bit(RDS_SHUTDOWN_WAITING, &cp->cp_flags))
			mod_delayed_work(cp->cp_wq, &cp->cp_down_wait_w, 0);
	}
}

/*
 * The application passes us an address range it wants to enable RDMA
 * to/from. We map the area, and save the <R_Key,offset> pair
 * in rm->m_rdma_cookie. This causes it to be sent along to the peer
 * in an extension header.
 */
static int rds_cmsg_rdma_map(struct rds_sock *rs, struct rds_message *rm,
			     struct cmsghdr *cmsg)
{
	int ret;
	if (cmsg->cmsg_len < CMSG_LEN(sizeof(struct rds_get_mr_args))
	 || rm->m_rdma_cookie != 0)
		return -EINVAL;

	inc_rdma_map_pending(rm->m_conn_path);
	if (!rds_conn_path_up(rm->m_conn_path)) {
		dec_rdma_map_pending(rm->m_conn_path);
		return -EAGAIN;
	}

	ret = __rds_rdma_map(rs, CMSG_DATA(cmsg), &rm->m_rdma_cookie,
			     &rm->rdma.op_rdma_mr, rm->m_conn_path);
	if (!ret)
		rm->rdma.op_implicit_mr = 1;

	dec_rdma_map_pending(rm->m_conn_path);

	return ret;
}

/*
 * Fill in rds_message for an atomic request.
 */
static int rds_cmsg_atomic(struct rds_sock *rs, struct rds_message *rm,
			   struct cmsghdr *cmsg)
{
	struct page *page = NULL;
	struct rds_atomic_args *args;
	int ret = 0;

	if (cmsg->cmsg_len < CMSG_LEN(sizeof(struct rds_atomic_args))
	 || rm->atomic.op_active)
		return -EINVAL;

	args = CMSG_DATA(cmsg);

	if (cmsg->cmsg_type == RDS_CMSG_ATOMIC_CSWP)
		rm->atomic.op_type = RDS_ATOMIC_TYPE_CSWP;
	else {
		rm->atomic.op_type = RDS_ATOMIC_TYPE_FADD;
		/* compare field should be 0 -- unused for FADD ops */
		if (args->compare) {
			ret = -EINVAL;
			goto err;
		}
	}

	rm->atomic.op_swap_add = args->swap_add;
	rm->atomic.op_compare = args->compare;
	rm->atomic.op_notify = !!(args->flags & RDS_RDMA_NOTIFY_ME);
	rm->atomic.op_silent = !!(args->flags & RDS_RDMA_SILENT);
	rm->atomic.op_active = 1;
	rm->atomic.op_recverr = rs->rs_recverr;
	rm->atomic.op_sg = rds_message_alloc_sgs(rm, 1);

	/* verify 8 byte-aligned */
	if (args->local_addr & 0x7) {
		ret = -EFAULT;
		goto err;
	}

	ret = rds_pin_pages(args->local_addr, 1, &page, 1);
	if (ret != 1)
		goto err;
	ret = 0;

	sg_set_page(rm->atomic.op_sg, page, 8, offset_in_page(args->local_addr));

	if (rm->atomic.op_notify || rm->atomic.op_recverr || rds_async_send_enabled) {
		/* We allocate an uninitialized notifier here, because
		 * we don't want to do that in the completion handler. We
		 * would have to use GFP_ATOMIC there, and don't want to deal
		 * with failed allocations.
		 */
		rm->atomic.op_notifier = kzalloc(sizeof(*rm->atomic.op_notifier), GFP_KERNEL);
		if (!rm->atomic.op_notifier) {
			ret = -ENOMEM;
			goto err;
		}

		rm->atomic.op_notifier->n_user_token = args->user_token;
		rm->atomic.op_notifier->n_status = RDS_RDMA_SEND_SUCCESS;
	}

	rm->atomic.op_rkey = rds_rdma_cookie_key(args->cookie);
	rm->atomic.op_remote_addr = args->remote_addr + rds_rdma_cookie_offset(args->cookie);

	return ret;
err:
	if (page)
		unpin_user_page(page);
	rm->atomic.op_active = 0;
	kfree(rm->atomic.op_notifier);

	return ret;
}

static int rds_cmsg_asend(struct rds_sock *rs, struct rds_message *rm,
			  struct cmsghdr *cmsg)
{
	struct rds_asend_args *args;

	if (!rds_async_send_enabled)
		return -EINVAL;

	if (cmsg->cmsg_len < CMSG_LEN(sizeof(struct rds_asend_args)))
		return -EINVAL;

	args = CMSG_DATA(cmsg);
	rm->data.op_notifier = kzalloc(sizeof(*rm->data.op_notifier),
				       GFP_KERNEL);
	if (!rm->data.op_notifier)
		return -ENOMEM;

	rm->data.op_notify = !!(args->flags & RDS_SEND_NOTIFY_ME);
	rm->data.op_notifier->n_user_token = args->user_token;
	rm->data.op_notifier->n_status = RDS_RDMA_SEND_SUCCESS;
	rm->data.op_async = 1;

	return 0;
}

int rds_rdma_process_send_cmsg(struct rds_sock *rs, struct rds_message *rm,
			       struct cmsghdr *cmsg, int *indp,
			       struct rds_iov_vector_arr *iov_arr)
{
	int ret;

	switch (cmsg->cmsg_type) {
	case RDS_CMSG_RDMA_ARGS:
		if (*indp >= iov_arr->iva_entries_used)
			return -ENOMEM;
		ret = rds_cmsg_rdma_args(rs, rm, cmsg, iov_arr->iva_iov + *indp);
		(*indp)++;
		break;

	case RDS_CMSG_RDMA_DEST:
		ret = rds_cmsg_rdma_dest(rs, rm, cmsg);
		break;

	case RDS_CMSG_RDMA_MAP:
		ret = rds_cmsg_rdma_map(rs, rm, cmsg);
		if (ret == -ENODEV)
			/* Accommodate the get_mr() case which can fail
			 * if connection isn't established yet.
			 */
			ret = -EAGAIN;
		break;
	case RDS_CMSG_ATOMIC_CSWP:
	case RDS_CMSG_ATOMIC_FADD:
		ret = rds_cmsg_atomic(rs, rm, cmsg);
		break;

	case RDS_CMSG_ASYNC_SEND:
		ret = rds_cmsg_asend(rs, rm, cmsg);
		break;

	default:
		ret = -EINVAL;
		break;
	}

	return ret;
}
EXPORT_SYMBOL_GPL(rds_rdma_process_send_cmsg);