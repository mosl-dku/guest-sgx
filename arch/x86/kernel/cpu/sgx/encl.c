// SPDX-License-Identifier: (GPL-2.0 OR BSD-3-Clause)
// Copyright(c) 2016-18 Intel Corporation.

#include <linux/mm.h>
#include <linux/shmem_fs.h>
#include <linux/suspend.h>
#include <linux/sched/mm.h>
#include "arch.h"
#include "encl.h"
#include "sgx.h"

static struct sgx_encl_page *sgx_encl_load_page(struct sgx_encl *encl,
						unsigned long addr)
{
	struct sgx_encl_page *entry;

	/* If process was forked, VMA is still there but vm_private_data is set
	 * to NULL.
	 */
	if (!encl)
		return ERR_PTR(-EFAULT);

	if ((encl->flags & SGX_ENCL_DEAD) ||
	    !(encl->flags & SGX_ENCL_INITIALIZED))
		return ERR_PTR(-EFAULT);

	entry = radix_tree_lookup(&encl->page_tree, addr >> PAGE_SHIFT);
	if (!entry)
		return ERR_PTR(-EFAULT);

	/* Page is already resident in the EPC. */
	if (entry->epc_page)
		return entry;

	return ERR_PTR(-EFAULT);
}

static struct sgx_encl_mm *sgx_encl_get_mm(struct sgx_encl *encl,
					   struct mm_struct *mm)
{
	struct sgx_encl_mm *next_mm = NULL;
	struct sgx_encl_mm *prev_mm = NULL;
	int iter;

	while (true) {
		next_mm = sgx_encl_next_mm(encl, prev_mm, &iter);
		if (prev_mm) {
			mmdrop(prev_mm->mm);
			kref_put(&prev_mm->refcount, sgx_encl_release_mm);
		}
		prev_mm = next_mm;

		if (iter == SGX_ENCL_MM_ITER_DONE)
			break;

		if (iter == SGX_ENCL_MM_ITER_RESTART)
			continue;

		if (mm == next_mm->mm)
			return next_mm;
	}

	return NULL;
}

static void sgx_vma_open(struct vm_area_struct *vma)
{
	struct sgx_encl *encl = vma->vm_private_data;
	struct sgx_encl_mm *mm;

	if (!encl)
		return;

	if (encl->flags & SGX_ENCL_DEAD)
		goto out;

	mm = sgx_encl_get_mm(encl, vma->vm_mm);
	if (!mm) {
		mm = kzalloc(sizeof(*mm), GFP_KERNEL);
		if (!mm) {
			encl->flags |= SGX_ENCL_DEAD;
			goto out;
		}

		spin_lock(&encl->mm_lock);
		mm->encl = encl;
		mm->mm = vma->vm_mm;
		list_add(&mm->list, &encl->mm_list);
		kref_init(&mm->refcount);
		spin_unlock(&encl->mm_lock);
	} else {
		mmdrop(mm->mm);
	}

out:
	kref_get(&encl->refcount);
}

static void sgx_vma_close(struct vm_area_struct *vma)
{
	struct sgx_encl *encl = vma->vm_private_data;
	struct sgx_encl_mm *mm;

	if (!encl)
		return;

	mm = sgx_encl_get_mm(encl, vma->vm_mm);
	if (mm) {
		mmdrop(mm->mm);
		kref_put(&mm->refcount, sgx_encl_release_mm);

		/* Release kref for the VMA. */
		kref_put(&mm->refcount, sgx_encl_release_mm);
	}

	kref_put(&encl->refcount, sgx_encl_release);
}

static unsigned int sgx_vma_fault(struct vm_fault *vmf)
{
	unsigned long addr = (unsigned long)vmf->address;
	struct vm_area_struct *vma = vmf->vma;
	struct sgx_encl *encl = vma->vm_private_data;
	struct sgx_encl_page *entry;
	int ret = VM_FAULT_NOPAGE;
	struct sgx_encl_mm *mm;
	unsigned long pfn;

	mm = sgx_encl_get_mm(encl, vma->vm_mm);
	if (!mm)
		return VM_FAULT_SIGBUS;

	mmdrop(mm->mm);
	kref_put(&mm->refcount, sgx_encl_release_mm);

	mutex_lock(&encl->lock);

	entry = sgx_encl_load_page(encl, addr);
	if (IS_ERR(entry)) {
		if (unlikely(PTR_ERR(entry) != -EBUSY))
			ret = VM_FAULT_SIGBUS;

		goto out;
	}

	if (!follow_pfn(vma, addr, &pfn))
		goto out;

	ret = vmf_insert_pfn(vma, addr, PFN_DOWN(entry->epc_page->desc));
	if (ret != VM_FAULT_NOPAGE) {
		ret = VM_FAULT_SIGBUS;
		goto out;
	}

out:
	mutex_unlock(&encl->lock);
	return ret;
}

const struct vm_operations_struct sgx_vm_ops = {
	.close = sgx_vma_close,
	.open = sgx_vma_open,
	.fault = sgx_vma_fault,
};
EXPORT_SYMBOL_GPL(sgx_vm_ops);

/**
 * sgx_encl_find - find an enclave
 * @mm:		mm struct of the current process
 * @addr:	address in the ELRANGE
 * @vma:	the resulting VMA
 *
 * Find an enclave identified by the given address. Give back a VMA that is
 * part of the enclave and located in that address. The VMA is given back if it
 * is a proper enclave VMA even if an &sgx_encl instance does not exist yet
 * (enclave creation has not been performed).
 *
 * Return:
 *   0 on success,
 *   -EINVAL if an enclave was not found,
 *   -ENOENT if the enclave has not been created yet
 */
int sgx_encl_find(struct mm_struct *mm, unsigned long addr,
		  struct vm_area_struct **vma)
{
	struct vm_area_struct *result;
	struct sgx_encl *encl;

	result = find_vma(mm, addr);
	if (!result || result->vm_ops != &sgx_vm_ops || addr < result->vm_start)
		return -EINVAL;

	encl = result->vm_private_data;
	*vma = result;

	return encl ? 0 : -ENOENT;
}
EXPORT_SYMBOL_GPL(sgx_encl_find);

/**
 * sgx_encl_destroy() - destroy enclave resources
 * @encl:	an &sgx_encl instance
 */
void sgx_encl_destroy(struct sgx_encl *encl)
{
	struct sgx_encl_page *entry;
	struct radix_tree_iter iter;
	void **slot;

	encl->flags |= SGX_ENCL_DEAD;

	radix_tree_for_each_slot(slot, &encl->page_tree, &iter, 0) {
		entry = *slot;
		if (entry->epc_page) {
			if (!__sgx_free_page(entry->epc_page)) {
				encl->secs_child_cnt--;
				entry->epc_page = NULL;

			}

			radix_tree_delete(&entry->encl->page_tree,
					  PFN_DOWN(entry->desc));
		}
	}

	if (!encl->secs_child_cnt && encl->secs.epc_page) {
		sgx_free_page(encl->secs.epc_page);
		encl->secs.epc_page = NULL;
	}
}
EXPORT_SYMBOL_GPL(sgx_encl_destroy);

/**
 * sgx_encl_release - Destroy an enclave instance
 * @kref:	address of a kref inside &sgx_encl
 *
 * Used together with kref_put(). Frees all the resources associated with the
 * enclave and the instance itself.
 */
void sgx_encl_release(struct kref *ref)
{
	struct sgx_encl *encl = container_of(ref, struct sgx_encl, refcount);
	struct sgx_encl_mm *mm;

	if (encl->pm_notifier.notifier_call)
		unregister_pm_notifier(&encl->pm_notifier);

	sgx_encl_destroy(encl);

	if (encl->backing)
		fput(encl->backing);

	/* If sgx_encl_create() fails, can be non-empty */
	while (!list_empty(&encl->mm_list)) {
		mm = list_first_entry(&encl->mm_list, struct sgx_encl_mm, list);
		list_del(&mm->list);
		kfree(mm);
	}

	kfree(encl);
}
EXPORT_SYMBOL_GPL(sgx_encl_release);

/**
 * sgx_encl_get_index() - Convert a page descriptor to a page index
 * @encl:	an enclave
 * @page:	an enclave page
 *
 * Given an enclave page descriptor, convert it to a page index used to access
 * backing storage. The backing page for SECS is located after the enclave
 * pages.
 */
pgoff_t sgx_encl_get_index(struct sgx_encl *encl, struct sgx_encl_page *page)
{
	if (!PFN_DOWN(page->desc))
		return PFN_DOWN(encl->size);

	return PFN_DOWN(page->desc - encl->base);
}
EXPORT_SYMBOL_GPL(sgx_encl_get_index);

/**
 * sgx_encl_encl_get_backing_page() - Pin the backing page
 * @encl:	an enclave
 * @index:	page index
 *
 * Return: the pinned backing page
 */
struct page *sgx_encl_get_backing_page(struct sgx_encl *encl, pgoff_t index)
{
	struct inode *inode = encl->backing->f_path.dentry->d_inode;
	struct address_space *mapping = inode->i_mapping;
	gfp_t gfpmask = mapping_gfp_mask(mapping);

	return shmem_read_mapping_page_gfp(mapping, index, gfpmask);
}
EXPORT_SYMBOL_GPL(sgx_encl_get_backing_page);

/**
 * sgx_encl_next_mm() - Iterate to the next mm
 * @encl:	an enclave
 * @mm:		an mm list entry
 * @iter:	iterator status
 *
 * Return: the enclave mm or NULL
 */
struct sgx_encl_mm *sgx_encl_next_mm(struct sgx_encl *encl,
				     struct sgx_encl_mm *mm, int *iter)
{
	struct list_head *entry;

	WARN(!encl, "%s: encl is NULL", __func__);
	WARN(!iter, "%s: iter is NULL", __func__);

	spin_lock(&encl->mm_lock);

	entry = mm ? mm->list.next : encl->mm_list.next;
	WARN(!entry, "%s: entry is NULL", __func__);

	if (entry == &encl->mm_list) {
		mm = NULL;
		*iter = SGX_ENCL_MM_ITER_DONE;
		goto out;
	}

	mm = list_entry(entry, struct sgx_encl_mm, list);

	if (!kref_get_unless_zero(&mm->refcount)) {
		*iter = SGX_ENCL_MM_ITER_RESTART;
		mm = NULL;
		goto out;
	}

	if (!atomic_add_unless(&mm->mm->mm_count, 1, 0)) {
		kref_put(&mm->refcount, sgx_encl_release_mm);
		mm = NULL;
		*iter = SGX_ENCL_MM_ITER_RESTART;
		goto out;
	}

	*iter = SGX_ENCL_MM_ITER_NEXT;

out:
	spin_unlock(&encl->mm_lock);
	return mm;
}

void sgx_encl_release_mm(struct kref *ref)
{
	struct sgx_encl_mm *mm =
		container_of(ref, struct sgx_encl_mm, refcount);

	spin_lock(&mm->encl->mm_lock);
	list_del(&mm->list);
	spin_unlock(&mm->encl->mm_lock);

	kfree(mm);
}
