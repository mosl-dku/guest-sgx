// SPDX-License-Identifier: (GPL-2.0 OR BSD-3-Clause)
// Copyright(c) 2016-19 Intel Corporation.

#include <asm/mman.h>
#include <linux/delay.h>
#include <linux/file.h>
#include <linux/hashtable.h>
#include <linux/highmem.h>
#include <linux/ratelimit.h>
#include <linux/sched/signal.h>
#include <linux/shmem_fs.h>
#include <linux/slab.h>
#include <linux/suspend.h>
#include "driver.h"

struct sgx_add_page_req {
	struct sgx_encl *encl;
	struct sgx_encl_page *encl_page;
	struct sgx_secinfo secinfo;
	unsigned long mrmask;
	struct list_head list;
};

static int sgx_encl_get(unsigned long addr, struct sgx_encl **encl)
{
	struct mm_struct *mm = current->mm;
	struct vm_area_struct *vma;
	int ret;

	if (addr & (PAGE_SIZE - 1))
		return -EINVAL;

	down_read(&mm->mmap_sem);

	ret = sgx_encl_find(mm, addr, &vma);
	if (!ret) {
		*encl = vma->vm_private_data;

		if ((*encl)->flags & SGX_ENCL_SUSPEND)
			ret = SGX_POWER_LOST_ENCLAVE;
		else
			kref_get(&(*encl)->refcount);
	}

	up_read(&mm->mmap_sem);
	return ret;
}

static bool sgx_process_add_page_req(struct sgx_add_page_req *req,
				     struct sgx_epc_page *epc_page)
{
	struct sgx_encl_page *encl_page = req->encl_page;
	struct sgx_encl *encl = req->encl;
	unsigned long page_index = sgx_encl_get_index(encl, encl_page);
	struct sgx_secinfo secinfo;
	struct sgx_pageinfo pginfo;
	struct page *backing;
	unsigned long addr;
	int ret;
	int i;

	if (encl->flags & (SGX_ENCL_SUSPEND | SGX_ENCL_DEAD))
		return false;

	addr = SGX_ENCL_PAGE_ADDR(encl_page);

	backing = sgx_encl_get_backing_page(encl, page_index);
	if (IS_ERR(backing))
		return false;

	/*
	 * The SECINFO field must be 64-byte aligned, copy it to a local
	 * variable that is guaranteed to be aligned as req->secinfo may
	 * or may not be 64-byte aligned, e.g. req may have been allocated
	 * via kzalloc which is not aware of __aligned attributes.
	 */
	memcpy(&secinfo, &req->secinfo, sizeof(secinfo));

	pginfo.secs = (unsigned long)sgx_epc_addr(encl->secs.epc_page);
	pginfo.addr = addr;
	pginfo.metadata = (unsigned long)&secinfo;
	pginfo.contents = (unsigned long)kmap_atomic(backing);
	ret = __eadd(&pginfo, sgx_epc_addr(epc_page));
	kunmap_atomic((void *)(unsigned long)pginfo.contents);

	put_page(backing);

	if (ret) {
		if (encls_failed(ret))
			ENCLS_WARN(ret, "EADD");
		return false;
	}

	for_each_set_bit(i, &req->mrmask, 16) {
		ret = __eextend(sgx_epc_addr(encl->secs.epc_page),
				sgx_epc_addr(epc_page) + (i * 0x100));
		if (ret) {
			if (encls_failed(ret))
				ENCLS_WARN(ret, "EEXTEND");
			return false;
		}
	}

	encl_page->encl = encl;
	encl_page->epc_page = epc_page;
	encl->secs_child_cnt++;

	return true;
}

static void sgx_add_page_worker(struct work_struct *work)
{
	struct sgx_add_page_req *req;
	bool skip_rest = false;
	bool is_empty = false;
	struct sgx_encl *encl;
	struct sgx_epc_page *epc_page;

	encl = container_of(work, struct sgx_encl, work);

	do {
		schedule();

		mutex_lock(&encl->lock);
		if (encl->flags & SGX_ENCL_DEAD)
			skip_rest = true;

		req = list_first_entry(&encl->add_page_reqs,
				       struct sgx_add_page_req, list);
		list_del(&req->list);
		is_empty = list_empty(&encl->add_page_reqs);
		mutex_unlock(&encl->lock);

		if (skip_rest)
			goto next;

		epc_page = sgx_alloc_page();

		mutex_lock(&encl->lock);

		if (IS_ERR(epc_page)) {
			sgx_encl_destroy(encl);
			skip_rest = true;
		} else if (!sgx_process_add_page_req(req, epc_page)) {
			sgx_free_page(epc_page);
			sgx_encl_destroy(encl);
			skip_rest = true;
		}

		mutex_unlock(&encl->lock);

next:
		kfree(req);
	} while (!kref_put(&encl->refcount, sgx_encl_release) && !is_empty);
}

static u32 sgx_calc_ssaframesize(u32 miscselect, u64 xfrm)
{
	u32 size_max = PAGE_SIZE;
	u32 size;
	int i;

	for (i = 2; i < 64; i++) {
		if (!((1 << i) & xfrm))
			continue;

		size = SGX_SSA_GPRS_SIZE + sgx_xsave_size_tbl[i];
		if (miscselect & SGX_MISC_EXINFO)
			size += SGX_SSA_MISC_EXINFO_SIZE;

		if (size > size_max)
			size_max = size;
	}

	return PFN_UP(size_max);
}

static int sgx_validate_secs(const struct sgx_secs *secs,
			     unsigned long ssaframesize)
{
	if (secs->size < (2 * PAGE_SIZE) || !is_power_of_2(secs->size))
		return -EINVAL;

	if (secs->base & (secs->size - 1))
		return -EINVAL;

	if (secs->miscselect & sgx_misc_reserved_mask ||
	    secs->attributes & sgx_attributes_reserved_mask ||
	    secs->xfrm & sgx_xfrm_reserved_mask)
		return -EINVAL;

	if (secs->attributes & SGX_ATTR_MODE64BIT) {
		if (secs->size > sgx_encl_size_max_64)
			return -EINVAL;
	} else if (secs->size > sgx_encl_size_max_32)
		return -EINVAL;

	if (!(secs->xfrm & XFEATURE_MASK_FP) ||
	    !(secs->xfrm & XFEATURE_MASK_SSE) ||
	    (((secs->xfrm >> XFEATURE_BNDREGS) & 1) !=
	     ((secs->xfrm >> XFEATURE_BNDCSR) & 1)))
		return -EINVAL;

	if (!secs->ssa_frame_size || ssaframesize > secs->ssa_frame_size)
		return -EINVAL;

	if (memchr_inv(secs->reserved1, 0, SGX_SECS_RESERVED1_SIZE) ||
	    memchr_inv(secs->reserved2, 0, SGX_SECS_RESERVED2_SIZE) ||
	    memchr_inv(secs->reserved3, 0, SGX_SECS_RESERVED3_SIZE) ||
	    memchr_inv(secs->reserved4, 0, SGX_SECS_RESERVED4_SIZE))
		return -EINVAL;

	return 0;
}

static struct sgx_encl *sgx_encl_alloc(struct sgx_secs *secs)
{
	unsigned long encl_size = secs->size + PAGE_SIZE;
	unsigned long ssaframesize;
	struct sgx_encl_mm *mm;
	struct sgx_encl *encl;
	struct file *backing;

	ssaframesize = sgx_calc_ssaframesize(secs->miscselect, secs->xfrm);
	if (sgx_validate_secs(secs, ssaframesize))
		return ERR_PTR(-EINVAL);

	backing = shmem_file_setup("SGX backing", encl_size + (encl_size >> 5),
				   VM_NORESERVE);
	if (IS_ERR(backing))
		return ERR_CAST(backing);

	encl = kzalloc(sizeof(*encl), GFP_KERNEL);
	if (!encl) {
		fput(backing);
		return ERR_PTR(-ENOMEM);
	}

	mm = kzalloc(sizeof(*mm), GFP_KERNEL);
	if (!mm) {
		kfree(encl);
		fput(backing);
		return ERR_PTR(-ENOMEM);
	}

	encl->secs_attributes = secs->attributes;
	encl->allowed_attributes = SGX_ATTR_ALLOWED_MASK;
	kref_init(&encl->refcount);
	INIT_LIST_HEAD(&encl->add_page_reqs);
	INIT_RADIX_TREE(&encl->page_tree, GFP_KERNEL);
	mutex_init(&encl->lock);
	INIT_WORK(&encl->work, sgx_add_page_worker);
	INIT_LIST_HEAD(&encl->mm_list);
	list_add(&mm->list, &encl->mm_list);
	kref_init(&mm->refcount);
	mm->mm = current->mm;
	mm->encl = encl;
	spin_lock_init(&encl->mm_lock);
	encl->base = secs->base;
	encl->size = secs->size;
	encl->ssaframesize = secs->ssa_frame_size;
	encl->backing = backing;

	return encl;
}

static struct sgx_encl_page *sgx_encl_page_alloc(struct sgx_encl *encl,
						 unsigned long addr)
{
	struct sgx_encl_page *encl_page;
	int ret;

	if (radix_tree_lookup(&encl->page_tree, PFN_DOWN(addr)))
		return ERR_PTR(-EEXIST);
	encl_page = kzalloc(sizeof(*encl_page), GFP_KERNEL);
	if (!encl_page)
		return ERR_PTR(-ENOMEM);
	encl_page->desc = addr;
	encl_page->encl = encl;
	ret = radix_tree_insert(&encl->page_tree, PFN_DOWN(encl_page->desc),
				encl_page);
	if (ret) {
		kfree(encl_page);
		return ERR_PTR(ret);
	}
	return encl_page;
}

static int sgx_encl_pm_notifier(struct notifier_block *nb,
				unsigned long action, void *data)
{
	struct sgx_encl *encl = container_of(nb, struct sgx_encl, pm_notifier);

	if (action != PM_SUSPEND_PREPARE && action != PM_HIBERNATION_PREPARE)
		return NOTIFY_DONE;

	mutex_lock(&encl->lock);
	sgx_encl_destroy(encl);
	encl->flags |= SGX_ENCL_SUSPEND;
	mutex_unlock(&encl->lock);
	flush_work(&encl->work);
	return NOTIFY_DONE;
}

static int sgx_encl_create(struct sgx_encl *encl, struct sgx_secs *secs)
{
	struct vm_area_struct *vma;
	struct sgx_pageinfo pginfo;
	struct sgx_secinfo secinfo;
	struct sgx_epc_page *secs_epc;
	long ret;

	secs_epc = sgx_alloc_page();
	if (IS_ERR(secs_epc)) {
		ret = PTR_ERR(secs_epc);
		return ret;
	}

	encl->secs.encl = encl;
	encl->secs.epc_page = secs_epc;

	pginfo.addr = 0;
	pginfo.contents = (unsigned long)secs;
	pginfo.metadata = (unsigned long)&secinfo;
	pginfo.secs = 0;
	memset(&secinfo, 0, sizeof(secinfo));
	ret = __ecreate((void *)&pginfo, sgx_epc_addr(secs_epc));

	if (ret) {
		pr_debug("ECREATE returned %ld\n", ret);
		return ret;
	}

	if (secs->attributes & SGX_ATTR_DEBUG)
		encl->flags |= SGX_ENCL_DEBUG;

	encl->pm_notifier.notifier_call = &sgx_encl_pm_notifier;
	ret = register_pm_notifier(&encl->pm_notifier);
	if (ret) {
		encl->pm_notifier.notifier_call = NULL;
		return ret;
	}

	down_read(&current->mm->mmap_sem);
	ret = sgx_encl_find(current->mm, secs->base, &vma);
	if (ret != -ENOENT) {
		if (!ret)
			ret = -EINVAL;
		up_read(&current->mm->mmap_sem);
		return ret;
	}

	if (vma->vm_start != secs->base ||
	    vma->vm_end != (secs->base + secs->size) ||
	    vma->vm_pgoff != 0) {
		ret = -EINVAL;
		up_read(&current->mm->mmap_sem);
		return ret;
	}

	vma->vm_private_data = encl;
	up_read(&current->mm->mmap_sem);
	return 0;
}

/**
 * sgx_ioc_enclave_create - handler for %SGX_IOC_ENCLAVE_CREATE
 * @filep:	open file to /dev/sgx
 * @cmd:	the command value
 * @arg:	pointer to an &sgx_enclave_create instance
 *
 * Validates SECS attributes, allocates an EPC page for the SECS and performs
 * ECREATE.
 *
 * Return:
 *   0 on success,
 *   -errno otherwise
 */
static long sgx_ioc_enclave_create(struct file *filep, unsigned int cmd,
				   unsigned long arg)
{
	struct sgx_enclave_create *createp = (struct sgx_enclave_create *)arg;
	struct page *secs_page;
	struct sgx_secs *secs;
	struct sgx_encl *encl;
	int ret;

	secs_page = alloc_page(GFP_HIGHUSER);
	if (!secs_page)
		return -ENOMEM;

	secs = kmap(secs_page);
	if (copy_from_user(secs, (void __user *)createp->src, sizeof(*secs))) {
		ret = -EFAULT;
		goto out;
	}

	encl = sgx_encl_alloc(secs);
	if (IS_ERR(encl)) {
		ret = PTR_ERR(encl);
		goto out;
	}

	ret = sgx_encl_create(encl, secs);
	if (ret)
		kref_put(&encl->refcount, sgx_encl_release);

out:
	kunmap(secs_page);
	__free_page(secs_page);
	return ret;
}

static int sgx_validate_secinfo(struct sgx_secinfo *secinfo)
{
	u64 page_type = secinfo->flags & SGX_SECINFO_PAGE_TYPE_MASK;
	u64 perm = secinfo->flags & SGX_SECINFO_PERMISSION_MASK;
	int i;

	if ((secinfo->flags & SGX_SECINFO_RESERVED_MASK) ||
	    ((perm & SGX_SECINFO_W) && !(perm & SGX_SECINFO_R)) ||
	    (page_type != SGX_SECINFO_TCS && page_type != SGX_SECINFO_TRIM &&
	     page_type != SGX_SECINFO_REG))
		return -EINVAL;

	for (i = 0; i < SGX_SECINFO_RESERVED_SIZE; i++)
		if (secinfo->reserved[i])
			return -EINVAL;

	return 0;
}

static bool sgx_validate_offset(struct sgx_encl *encl, unsigned long offset)
{
	if (offset & (PAGE_SIZE - 1))
		return false;

	if (offset >= encl->size)
		return false;

	return true;
}

static int sgx_validate_tcs(struct sgx_encl *encl, struct sgx_tcs *tcs)
{
	int i;

	if (tcs->flags & SGX_TCS_RESERVED_MASK)
		return -EINVAL;

	if (tcs->flags & SGX_TCS_DBGOPTIN)
		return -EINVAL;

	if (!sgx_validate_offset(encl, tcs->ssa_offset))
		return -EINVAL;

	if (!sgx_validate_offset(encl, tcs->fs_offset))
		return -EINVAL;

	if (!sgx_validate_offset(encl, tcs->gs_offset))
		return -EINVAL;

	if ((tcs->fs_limit & 0xFFF) != 0xFFF)
		return -EINVAL;

	if ((tcs->gs_limit & 0xFFF) != 0xFFF)
		return -EINVAL;

	for (i = 0; i < SGX_TCS_RESERVED_SIZE; i++)
		if (tcs->reserved[i])
			return -EINVAL;

	return 0;
}

static int __sgx_encl_add_page(struct sgx_encl *encl,
			       struct sgx_encl_page *encl_page,
			       void *data,
			       struct sgx_secinfo *secinfo,
			       unsigned int mrmask)
{
	unsigned long page_index = sgx_encl_get_index(encl, encl_page);
	u64 page_type = secinfo->flags & SGX_SECINFO_PAGE_TYPE_MASK;
	struct sgx_add_page_req *req = NULL;
	struct page *backing;
	void *backing_ptr;
	int empty;

	req = kzalloc(sizeof(*req), GFP_KERNEL);
	if (!req)
		return -ENOMEM;

	backing = sgx_encl_get_backing_page(encl, page_index);
	if (IS_ERR(backing)) {
		kfree(req);
		return PTR_ERR(backing);
	}

	backing_ptr = kmap(backing);
	memcpy(backing_ptr, data, PAGE_SIZE);
	kunmap(backing);
	if (page_type == SGX_SECINFO_TCS)
		encl_page->desc |= SGX_ENCL_PAGE_TCS;
	memcpy(&req->secinfo, secinfo, sizeof(*secinfo));
	req->encl = encl;
	req->encl_page = encl_page;
	req->mrmask = mrmask;
	empty = list_empty(&encl->add_page_reqs);
	kref_get(&encl->refcount);
	list_add_tail(&req->list, &encl->add_page_reqs);
	if (empty)
		queue_work(sgx_encl_wq, &encl->work);
	set_page_dirty(backing);
	put_page(backing);
	return 0;
}

static int sgx_encl_add_page(struct sgx_encl *encl, unsigned long addr,
			     void *data, struct sgx_secinfo *secinfo,
			     unsigned int mrmask)
{
	u64 page_type = secinfo->flags & SGX_SECINFO_PAGE_TYPE_MASK;
	struct sgx_encl_page *encl_page;
	int ret;

	if (sgx_validate_secinfo(secinfo))
		return -EINVAL;
	if (page_type == SGX_SECINFO_TCS) {
		ret = sgx_validate_tcs(encl, data);
		if (ret)
			return ret;
	}

	mutex_lock(&encl->lock);

	if (encl->flags & (SGX_ENCL_INITIALIZED | SGX_ENCL_DEAD)) {
		ret = -EINVAL;
		goto out;
	}

	encl_page = sgx_encl_page_alloc(encl, addr);
	if (IS_ERR(encl_page)) {
		ret = PTR_ERR(encl_page);
		goto out;
	}

	ret = __sgx_encl_add_page(encl, encl_page, data, secinfo, mrmask);
	if (ret) {
		radix_tree_delete(&encl_page->encl->page_tree,
				  PFN_DOWN(encl_page->desc));
		kfree(encl_page);
	}

out:
	mutex_unlock(&encl->lock);
	return ret;
}

/**
 * sgx_ioc_enclave_add_page - handler for %SGX_IOC_ENCLAVE_ADD_PAGE
 *
 * @filep:	open file to /dev/sgx
 * @cmd:	the command value
 * @arg:	pointer to an &sgx_enclave_add_page instance
 *
 * Creates a new enclave page and enqueues an EADD operation that will be
 * processed by a worker thread later on.
 *
 * Return:
 *   0 on success,
 *   -errno otherwise
 */
static long sgx_ioc_enclave_add_page(struct file *filep, unsigned int cmd,
				     unsigned long arg)
{
	struct sgx_enclave_add_page *addp = (void *)arg;
	struct sgx_secinfo secinfo;
	struct sgx_encl *encl;
	struct page *data_page;
	void *data;
	int ret;

	ret = sgx_encl_get(addp->addr, &encl);
	if (ret)
		return ret;

	if (copy_from_user(&secinfo, (void __user *)addp->secinfo,
			   sizeof(secinfo))) {
		kref_put(&encl->refcount, sgx_encl_release);
		return -EFAULT;
	}

	data_page = alloc_page(GFP_HIGHUSER);
	if (!data_page) {
		kref_put(&encl->refcount, sgx_encl_release);
		return -ENOMEM;
	}

	data = kmap(data_page);

	if (copy_from_user((void *)data, (void __user *)addp->src, PAGE_SIZE)) {
		ret = -EFAULT;
		goto out;
	}

	ret = sgx_encl_add_page(encl, addp->addr, data, &secinfo, addp->mrmask);
	if (ret)
		goto out;

out:
	kref_put(&encl->refcount, sgx_encl_release);
	kunmap(data_page);
	__free_page(data_page);
	return ret;
}

static int __sgx_get_key_hash(struct crypto_shash *tfm, const void *modulus,
			      void *hash)
{
	SHASH_DESC_ON_STACK(shash, tfm);

	shash->tfm = tfm;

	return crypto_shash_digest(shash, modulus, SGX_MODULUS_SIZE, hash);
}

static int sgx_get_key_hash(const void *modulus, void *hash)
{
	struct crypto_shash *tfm;
	int ret;

	tfm = crypto_alloc_shash("sha256", 0, CRYPTO_ALG_ASYNC);
	if (IS_ERR(tfm))
		return PTR_ERR(tfm);

	ret = __sgx_get_key_hash(tfm, modulus, hash);

	crypto_free_shash(tfm);
	return ret;
}

static int sgx_encl_init(struct sgx_encl *encl, struct sgx_sigstruct *sigstruct,
			 struct sgx_einittoken *token)
{
	u64 mrsigner[4];
	int ret;
	int i;
	int j;

	/* Check that the required attributes have been authorized. */
	if (encl->secs_attributes & ~encl->allowed_attributes)
		return -EINVAL;

	ret = sgx_get_key_hash(sigstruct->modulus, mrsigner);
	if (ret)
		return ret;

	flush_work(&encl->work);

	mutex_lock(&encl->lock);

	if (encl->flags & SGX_ENCL_INITIALIZED)
		goto err_out;

	if (encl->flags & SGX_ENCL_DEAD) {
		ret = -EFAULT;
		goto err_out;
	}

	for (i = 0; i < SGX_EINIT_SLEEP_COUNT; i++) {
		for (j = 0; j < SGX_EINIT_SPIN_COUNT; j++) {
			ret = sgx_einit(sigstruct, token, encl->secs.epc_page,
					mrsigner);
			if (ret == SGX_UNMASKED_EVENT)
				continue;
			else
				break;
		}

		if (ret != SGX_UNMASKED_EVENT)
			break;

		msleep_interruptible(SGX_EINIT_SLEEP_TIME);

		if (signal_pending(current)) {
			ret = -ERESTARTSYS;
			goto err_out;
		}
	}

	if (encls_faulted(ret)) {
		if (encls_failed(ret))
			ENCLS_WARN(ret, "EINIT");

		sgx_encl_destroy(encl);
		ret = -EFAULT;
	} else if (encls_returned_code(ret)) {
		pr_debug("EINIT returned %d\n", ret);
	} else {
		encl->flags |= SGX_ENCL_INITIALIZED;
	}

err_out:
	mutex_unlock(&encl->lock);
	return ret;
}

/**
 * sgx_ioc_enclave_init - handler for %SGX_IOC_ENCLAVE_INIT
 *
 * @filep:	open file to /dev/sgx
 * @cmd:	the command value
 * @arg:	pointer to an &sgx_enclave_init instance
 *
 * Flushes the remaining enqueued EADD operations and performs EINIT.
 *
 * Return:
 *   0 on success,
 *   SGX error code on EINIT failure,
 *   -errno otherwise
 */
static long sgx_ioc_enclave_init(struct file *filep, unsigned int cmd,
				 unsigned long arg)
{
	struct sgx_enclave_init *initp = (struct sgx_enclave_init *)arg;
	struct sgx_sigstruct *sigstruct;
	struct sgx_einittoken *einittoken;
	struct sgx_encl *encl;
	struct page *initp_page;
	int ret;

	initp_page = alloc_page(GFP_HIGHUSER);
	if (!initp_page)
		return -ENOMEM;

	sigstruct = kmap(initp_page);
	einittoken = (struct sgx_einittoken *)
		((unsigned long)sigstruct + PAGE_SIZE / 2);
	memset(einittoken, 0, sizeof(*einittoken));

	if (copy_from_user(sigstruct, (void __user *)initp->sigstruct,
			   sizeof(*sigstruct))) {
		ret = -EFAULT;
		goto out;
	}

	ret = sgx_encl_get(initp->addr, &encl);
	if (ret)
		goto out;

	ret = sgx_encl_init(encl, sigstruct, einittoken);

	kref_put(&encl->refcount, sgx_encl_release);

out:
	kunmap(initp_page);
	__free_page(initp_page);
	return ret;
}

/**
 * sgx_ioc_enclave_set_attribute - handler for %SGX_IOC_ENCLAVE_SET_ATTRIBUTE
 * @filep:	open file to /dev/sgx
 * @cmd:	the command value
 * @arg:	pointer to a struct sgx_enclave_set_attribute instance
 *
 * Sets an attribute matching the attribute file that is pointed by the
 * parameter structure field attribute_fd.
 *
 * Return: 0 on success, -errno otherwise
 */
static long sgx_ioc_enclave_set_attribute(struct file *filep, unsigned int cmd,
					  unsigned long arg)
{
	struct sgx_enclave_set_attribute *params = (void *)arg;
	struct file *attribute_file;
	struct sgx_encl *encl;
	int ret;

	attribute_file = fget(params->attribute_fd);
	if (!attribute_file->f_op)
		return -EINVAL;

	if (attribute_file->f_op != &sgx_fs_provision_fops) {
		ret = -EINVAL;
		goto out;
	}

	ret = sgx_encl_get(params->addr, &encl);
	if (ret)
		goto out;

	encl->allowed_attributes |= SGX_ATTR_PROVISIONKEY;
	kref_put(&encl->refcount, sgx_encl_release);

out:
	fput(attribute_file);
	return ret;
}

typedef long (*sgx_ioc_t)(struct file *filep, unsigned int cmd,
			  unsigned long arg);

long sgx_ioctl(struct file *filep, unsigned int cmd, unsigned long arg)
{
	char data[256];
	sgx_ioc_t handler = NULL;
	long ret;

	switch (cmd) {
	case SGX_IOC_ENCLAVE_CREATE:
		handler = sgx_ioc_enclave_create;
		break;
	case SGX_IOC_ENCLAVE_ADD_PAGE:
		handler = sgx_ioc_enclave_add_page;
		break;
	case SGX_IOC_ENCLAVE_INIT:
		handler = sgx_ioc_enclave_init;
		break;
	case SGX_IOC_ENCLAVE_SET_ATTRIBUTE:
		handler = sgx_ioc_enclave_set_attribute;
		break;
	default:
		return -ENOIOCTLCMD;
	}

	if (copy_from_user(data, (void __user *)arg, _IOC_SIZE(cmd)))
		return -EFAULT;

	ret = handler(filep, cmd, (unsigned long)((void *)data));
	if (!ret && (cmd & IOC_OUT)) {
		if (copy_to_user((void __user *)arg, data, _IOC_SIZE(cmd)))
			return -EFAULT;
	}

	return ret;
}
