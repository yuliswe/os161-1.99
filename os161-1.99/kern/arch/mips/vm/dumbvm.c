/*
 * Copyright (c) 2000, 2001, 2002, 2003, 2004, 2005, 2008, 2009
 *	The President and Fellows of Harvard College.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the University nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE UNIVERSITY AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE UNIVERSITY OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include <types.h>
#include <kern/errno.h>
#include <lib.h>
#include <spl.h>
#include <spinlock.h>
#include <proc.h>
#include <current.h>
#include <mips/tlb.h>
#include <addrspace.h>
#include <vm.h>
#include "opt-A3.h"
#include <array.h>


/*
 * Dumb MIPS-only "VM system" that is intended to only be just barely
 * enough to struggle off the ground.
 */

/* under dumbvm, always have 48k of user stack */
#define DUMBVM_STACKPAGES    12

/*
 * Wrap rma_stealmem in a spinlock.
 */
static struct spinlock stealmem_lock = SPINLOCK_INITIALIZER;

static paddr_t MEM_AVAL_START;
static paddr_t MEM_START;
static paddr_t MEM_END;
static struct frame* CORE_MAP;
static unsigned long MEM_BYTES; 
static unsigned long NUM_PAGES; 
static unsigned long CORE_MAP_BYTES;
static unsigned long CORE_MAP_NPAGES;
static bool BOOTSTRAP_DONE = false;

struct frame {
   bool in_use;
   unsigned long block_len;
};

void vm_bootstrap(void) {
	/* Init core map */
	ram_getsize(&MEM_START, &MEM_END);
	MEM_BYTES = MEM_END - MEM_START;
	CORE_MAP = (struct frame*)PADDR_TO_KVADDR(MEM_START);
	NUM_PAGES = MEM_BYTES / PAGE_SIZE;
	CORE_MAP_BYTES = NUM_PAGES * sizeof(struct frame);
	CORE_MAP_NPAGES = DIVROUNDUP(CORE_MAP_BYTES, PAGE_SIZE);
	MEM_AVAL_START = MEM_START + CORE_MAP_NPAGES * PAGE_SIZE;
	// kprintf("MEM_BYTES=%lu, NUM_PAGES=%lu, CORE_MAP_BYTES=%lu, CORE_MAP_NPAGES=%lu, CORE_MAP=%p, MEM_AVAL_START=%p, MEM_START=%p, MEM_END=%p\n",
	//         	MEM_BYTES,
	//         	NUM_PAGES,
	//         	CORE_MAP_BYTES,
	//         	CORE_MAP_NPAGES,
	//         	(void*)CORE_MAP,
	//         	(void*)MEM_AVAL_START,
	//         	(void*)MEM_START,
	//         	(void*)MEM_END);
	for (unsigned long i = 0; i < NUM_PAGES; i++) {
		struct frame f;
		f.in_use = (i < CORE_MAP_NPAGES);
		f.block_len = 0;
		// kprintf("CORE_MAP[%lu]=%p\n", i, CORE_MAP+i);
		CORE_MAP[i] = f;
	}
	BOOTSTRAP_DONE = true;
	// kprintf("CORE_MAP inited\n");
}


static
paddr_t
getppages(unsigned long npages)
{
	paddr_t addr = 0;
	
	spinlock_acquire(&stealmem_lock);
	
	if (BOOTSTRAP_DONE) {
	
		// kprintf("getppages(%lu)\n", npages);
		// find a space 
		bool found_space = false;
		unsigned long i = 0; // frame number
		for (i = 0; i < NUM_PAGES; i++) {
			if (CORE_MAP[i].in_use) {
				// kprintf("CORE_MAP[%lu] is in use\n", i);
				continue;
			}
			// found an empty frame, check if the consecutive space is long enough
			bool long_enough = true;
			unsigned long c;
			for (c = 0; c < npages; c++) {
				if (CORE_MAP[i+c].in_use) {
					long_enough = false;
					break;
				}
			}
			if (! long_enough) {
				i += c;
				continue;
			}
			// if long enough
			found_space = true;
			CORE_MAP[i].block_len = npages;
			addr = MEM_START + i * PAGE_SIZE;
			for (c = 0; c < npages; c++) {
				KASSERT(! CORE_MAP[i+c].in_use);
				CORE_MAP[i+c].in_use = true;
			}
			break;
		}
		
		// kprintf("getppages(%lu), alloc addr=%p, frame number=%lu\n", npages, (void*)addr, i);
		
		if (i >= NUM_PAGES) {
			panic("getppages: Out of memory!\n");
		}

	} else {
		addr = ram_stealmem(npages);
	}
	
	spinlock_release(&stealmem_lock);
	return addr;
}

/* Allocate/free some kernel-space virtual pages */
vaddr_t 
alloc_kpages(int npages)
{
	paddr_t pa;
	pa = getppages(npages);
	
	if (pa==0) {
		return 0;
	}
	
	return PADDR_TO_KVADDR(pa);
}

void 
free_kpages(vaddr_t addr)
{
	
	if (KVADDR_TO_PADDR(addr) < MEM_START) {
		return;
	}
	
	spinlock_acquire(&stealmem_lock);
	
	long unsigned fn = (KVADDR_TO_PADDR(addr) - MEM_START) / PAGE_SIZE;
	// kprintf("kfree_kpages(%p), paddr=%p, frame number freed=%lu\n", 
	//         (void*)addr, (void*)KVADDR_TO_PADDR(addr), fn);
	struct frame f = CORE_MAP[fn];
	// must be a memory allocated by kmalloc
	KASSERT(f.block_len > 0);
	// free the block
	for (long unsigned i = 0; i < f.block_len; i++) {
		KASSERT(CORE_MAP[fn + i].in_use);
		KASSERT(i == 0 ? CORE_MAP[fn + i].block_len > 0 : CORE_MAP[fn + i].block_len == 0);
		CORE_MAP[fn + i].in_use = false;
	}
	CORE_MAP[fn].block_len = 0;
	
	spinlock_release(&stealmem_lock);
}

void
vm_tlbshootdown_all(void)
{
	panic("dumbvm tried to do tlb shootdown?!\n");
}

void
vm_tlbshootdown(const struct tlbshootdown *ts)
{
	(void)ts;
	panic("dumbvm tried to do tlb shootdown?!\n");
}

int
vm_fault(int faulttype, vaddr_t faultaddress)
{
	vaddr_t vbase1, vtop1, vbase2, vtop2, stackbase, stacktop;
	paddr_t paddr;
	int i;
	uint32_t ehi, elo;
	struct addrspace *as;
	int spl;

	faultaddress &= PAGE_FRAME;

	DEBUG(DB_VM, "dumbvm: fault: 0x%x\n", faultaddress);

	switch (faulttype) {
	    case VM_FAULT_READONLY:
		/* We always create pages read-write, so we can't get this */
			// panic("dumbvm: got VM_FAULT_READONLY\n");
		    return 0;
	    case VM_FAULT_READ:
	    case VM_FAULT_WRITE:
			break;
	    default:
			return EINVAL;
	}

	if (curproc == NULL) {
		/*
		 * No process. This is probably a kernel fault early
		 * in boot. Return EFAULT so as to panic instead of
		 * getting into an infinite faulting loop.
		 */
		return EFAULT;
	}

	as = curproc_getas();
	if (as == NULL) {
		/*
		 * No address space set up. This is probably also a
		 * kernel fault early in boot.
		 */
		return EFAULT;
	}

	/* Assert that the address space has been set up properly. */
	KASSERT(as->as_vbase1 != 0);
	KASSERT(as->as_page_table1 != 0);
	KASSERT(as->as_npages1 != 0);
	KASSERT(as->as_vbase2 != 0);
	KASSERT(as->as_page_table2 != 0);
	KASSERT(as->as_npages2 != 0);
	KASSERT(as->as_stack_page_table != 0);
	KASSERT((as->as_vbase1 & PAGE_FRAME) == as->as_vbase1);
	KASSERT((as->as_page_table1 & PAGE_FRAME) == as->as_page_table1);
	KASSERT((as->as_vbase2 & PAGE_FRAME) == as->as_vbase2);
	KASSERT((as->as_page_table2 & PAGE_FRAME) == as->as_page_table2);
	KASSERT((as->as_stack_page_table & PAGE_FRAME) == as->as_stack_page_table);

	vbase1 = as->as_vbase1;
	vtop1 = vbase1 + as->as_npages1 * PAGE_SIZE;
	vbase2 = as->as_vbase2;
	vtop2 = vbase2 + as->as_npages2 * PAGE_SIZE;
	stackbase = USERSTACK - DUMBVM_STACKPAGES * PAGE_SIZE;
	stacktop = USERSTACK;

	if (faultaddress >= vbase1 && faultaddress < vtop1) {
		paddr = (faultaddress - vbase1) + as->as_page_table1;
	}
	else if (faultaddress >= vbase2 && faultaddress < vtop2) {
		paddr = (faultaddress - vbase2) + as->as_page_table2;
	}
	else if (faultaddress >= stackbase && faultaddress < stacktop) {
		paddr = (faultaddress - stackbase) + as->as_stack_page_table;
	}
	else {
		return EFAULT;
	}

	/* make sure it's page-aligned */
	KASSERT((paddr & PAGE_FRAME) == paddr);

	/* Disable interrupts on this CPU while frobbing the TLB. */
	spl = splhigh();

#if OPT_A3
	ehi = faultaddress;
	// if it's the code segment, set read only by removing the dirty bit
	if (faultaddress >= vbase1 && faultaddress < vtop1 && as->load_done) {
		elo = paddr | TLBLO_VALID;
	} else {
		elo = paddr | TLBLO_DIRTY | TLBLO_VALID;
	}
	tlb_random(ehi, elo);
	(void)i;
	splx(spl);
	return 0;
#else
	for (i=0; i<NUM_TLB; i++) {
		tlb_read(&ehi, &elo, i);
		if (elo & TLBLO_VALID) {
			continue;
		}
		ehi = faultaddress;
		elo = paddr | TLBLO_DIRTY | TLBLO_VALID;
		DEBUG(DB_VM, "dumbvm: 0x%x -> 0x%x\n", faultaddress, paddr);
		tlb_write(ehi, elo, i);
		splx(spl);
		return 0;
	}
#endif
	
	kprintf("dumbvm: Ran out of TLB entries - cannot handle page fault\n");
	splx(spl);
	return EFAULT;
}

struct addrspace *
as_create(void)
{
	struct addrspace *as = kmalloc(sizeof(struct addrspace));
	if (as==NULL) {
		return NULL;
	}

	as->as_vbase1 = 0;
	as->as_page_table1 = 0;
	as->as_npages1 = 0;
	as->as_vbase2 = 0;
	as->as_page_table2 = 0;
	as->as_npages2 = 0;
	as->as_stack_page_table = 0;
	as->load_done = false;
	
	return as;
}

void
as_destroy(struct addrspace *as)
{
	// if (as->as_page_table1) {
		// kprintf("as_destroy: kfree_kpages %p\n", (void*)PADDR_TO_KVADDR(as->as_page_table1));
		free_kpages(PADDR_TO_KVADDR(as->as_page_table1));
	// }
	// if (as->as_page_table2) {
		// kprintf("as_destroy: kfree_kpages %p\n", (void*)PADDR_TO_KVADDR(as->as_page_table2));
		free_kpages(PADDR_TO_KVADDR(as->as_page_table2));
	// }
	// if (as->as_stack_page_table) {
		// kprintf("as_destroy: kfree_kpages %p\n", (void*)PADDR_TO_KVADDR(as->as_stack_page_table));
		free_kpages(PADDR_TO_KVADDR(as->as_stack_page_table));
	// }
	
	kfree(as);
}

void
as_activate(void)
{
	int i, spl;
	struct addrspace *as;

	as = curproc_getas();
#ifdef UW
        /* Kernel threads don't have an address spaces to activate */
#endif
	if (as == NULL) {
		return;
	}

	/* Disable interrupts on this CPU while frobbing the TLB. */
	spl = splhigh();

	for (i=0; i<NUM_TLB; i++) {
		tlb_write(TLBHI_INVALID(i), TLBLO_INVALID(), i);
	}

	splx(spl);
}

void
as_deactivate(void)
{
	/* nothing */
}

int
as_define_region(struct addrspace *as, vaddr_t vaddr, size_t sz,
		 int readable, int writeable, int executable)
{
	size_t npages; 

	/* Align the region. First, the base... */
	sz += vaddr & ~(vaddr_t)PAGE_FRAME;
	vaddr &= PAGE_FRAME;

	/* ...and now the length. */
	sz = (sz + PAGE_SIZE - 1) & PAGE_FRAME;

	npages = sz / PAGE_SIZE;

	/* We don't use these - all pages are read-write */
	(void)readable;
	(void)writeable;
	(void)executable;

	if (as->as_vbase1 == 0) {
		as->as_vbase1 = vaddr;
		as->as_npages1 = npages;
		return 0;
	}

	if (as->as_vbase2 == 0) {
		as->as_vbase2 = vaddr;
		as->as_npages2 = npages;
		return 0;
	}

	/*
	 * Support for more than two regions is not available.
	 */
	kprintf("dumbvm: Warning: too many regions\n");
	return EUNIMP;
}

static
void
as_zero_region(paddr_t paddr, unsigned npages)
{
	bzero((void *)PADDR_TO_KVADDR(paddr), npages * PAGE_SIZE);
}

int
as_prepare_load(struct addrspace *as)
{
	KASSERT(as->as_page_table1 == 0);
	KASSERT(as->as_page_table2 == 0);
	KASSERT(as->as_stack_page_table == 0);

	as->as_page_table1 = getppages(as->as_npages1);
	if (as->as_page_table1 == 0) {
		return ENOMEM;
	}

	as->as_page_table2 = getppages(as->as_npages2);
	if (as->as_page_table2 == 0) {
		return ENOMEM;
	}

	as->as_stack_page_table = getppages(DUMBVM_STACKPAGES);
	if (as->as_stack_page_table == 0) {
		return ENOMEM;
	}
	
	as_zero_region(as->as_page_table1, as->as_npages1);
	as_zero_region(as->as_page_table2, as->as_npages2);
	as_zero_region(as->as_stack_page_table, DUMBVM_STACKPAGES);

	return 0;
}

int
as_complete_load(struct addrspace *as)
{
#if OPT_A3
	as->load_done = true;
#endif
	return 0;
}

int
as_define_stack(struct addrspace *as, vaddr_t *stackptr)
{
	KASSERT(as->as_stack_page_table != 0);

	*stackptr = USERSTACK;
	return 0;
}

int
as_copy(struct addrspace *old, struct addrspace **ret)
{
	struct addrspace *new;

	new = as_create();
	if (new==NULL) {
		return ENOMEM;
	}

	new->as_vbase1 = old->as_vbase1;
	new->as_npages1 = old->as_npages1;
	new->as_vbase2 = old->as_vbase2;
	new->as_npages2 = old->as_npages2;

	/* (Mis)use as_prepare_load to allocate some physical memory. */
	if (as_prepare_load(new)) {
		as_destroy(new);
		return ENOMEM;
	}

	KASSERT(new->as_page_table1 != 0);
	KASSERT(new->as_page_table2 != 0);
	KASSERT(new->as_stack_page_table != 0);

	memmove((void *)PADDR_TO_KVADDR(new->as_page_table1),
		(const void *)PADDR_TO_KVADDR(old->as_page_table1),
		old->as_npages1*PAGE_SIZE);

	memmove((void *)PADDR_TO_KVADDR(new->as_page_table2),
		(const void *)PADDR_TO_KVADDR(old->as_page_table2),
		old->as_npages2*PAGE_SIZE);

	memmove((void *)PADDR_TO_KVADDR(new->as_stack_page_table),
		(const void *)PADDR_TO_KVADDR(old->as_stack_page_table),
		DUMBVM_STACKPAGES*PAGE_SIZE);
	
	*ret = new;
	return 0;
}
