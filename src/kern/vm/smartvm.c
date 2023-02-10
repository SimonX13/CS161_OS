
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
#include <machine/vm.h>
 

static struct spinlock pagetable_lock = SPINLOCK_INITIALIZER;         // pagetable lock 


/* 
 * return a virtual address of the allocated pages 
 */
static vaddr_t getppages(unsigned npages) {
	spinlock_acquire(&pagetable_lock);

	int chunk, head = 0; // track the head of the free block
	bool new_start = true; 
	
	// try to find consecutive free blocks
	for (int i = 0; i < total_pages; i++) {
		if (pagetable[i].status == free && new_start == true) {
			chunk++;
			head = i;
		} 
		else if (pagetable[i].status == free) {
			chunk++;
		} 
		else if(pagetable[i].status == allocated) {
			chunk = 0;
			head = 0;
			new_start = true;
		}
		else{
			kprintf("shouldn't be here");
		}
		// find consecutive free pages 
		if ((unsigned )chunk == npages) {
			// define these pages are allocated
			pagetable[head].number_pages = npages;
			for (unsigned j = 0; j <  npages; j++) {
				pagetable[j + head].status = allocated;
			}
			break;
		}
	}

	// check whether pagetable is full
	if ((unsigned )chunk < npages) {
		spinlock_release(&pagetable_lock);
		return 0;
	}
	 
	// addr is the physical address of the head of the free segment inside the page table
	paddr_t addr = pagetable_addr + head * PAGE_SIZE;
	as_zero_region(addr, npages);
	 
	pagetable[head].virtual_address = PADDR_TO_KVADDR(addr);
	spinlock_release(&pagetable_lock);
	return addr;
}



vaddr_t alloc_kpages(unsigned npages) {
	paddr_t paddr = getppages(npages);
	if(paddr == 0) {
		return 0;
	}
	return PADDR_TO_KVADDR(paddr);;
}


void
vm_bootstrap(void)
{
	paddr_t lastpaddr = ram_getsize();
	paddr_t firstpaddr = ram_getfirstfree();

	// identify the physical address of the page table
	if(firstpaddr % PAGE_SIZE != 0) {
		pagetable_addr = ((firstpaddr/PAGE_SIZE) + 1) * PAGE_SIZE; 
	}
	else{
		pagetable_addr = firstpaddr + PAGE_SIZE;
	}

	pagetable = (struct pagetable_entry*) PADDR_TO_KVADDR(pagetable_addr); // pointer to the virtual address space

	// identify the number of pages of the entire memory space
	if( lastpaddr % PAGE_SIZE !=0 ){
		total_pages =  ( lastpaddr / PAGE_SIZE ) + 1; // round up
	}
	else{
		total_pages =    lastpaddr / PAGE_SIZE ;
	}

	// number of pages that the data structure needs
	int pagetable_pages = ( total_pages * sizeof(struct pagetable_entry) / PAGE_SIZE ) + 1; // round up

	/*
		pagetable is allocated for pagetable_pages pages because this memory is reserverd for this data structure
		pagetable is not allcoated from its pages to the total_pages because it bootstraped just now
	*/
	pagetable[0].number_pages = pagetable_pages;
	for (int i = 0; i < pagetable_pages; i++) {
		pagetable[i].status = allocated;
	}
	for (int i = pagetable_pages; i < total_pages; i++) {
		pagetable[i].status = free;
		pagetable[i].number_pages = 0;
	}
}



/*
 * frees up pages
 */
void
free_kpages(vaddr_t addr)
{
	spinlock_acquire(&pagetable_lock);
	// find the page that contains the virtual address addr
	for ( int i = total_pages * sizeof(struct pagetable_entry) / PAGE_SIZE; i < total_pages; i++) {
		//found
		if (addr == pagetable[i].virtual_address) {
			// free up all the pages in this segment
			for (int j = 0; j < pagetable[i].number_pages; j++) {	 
				pagetable[i + j].status = free;
			}
			break;
		}
	}
	spinlock_release(&pagetable_lock);
}

/* TLB shootdown handling called from interprocessor_interrupt */
void vm_tlbshootdown_all(void) {}
void vm_tlbshootdown(const struct tlbshootdown *ts) {
	(void) ts;
}

/* 
 * page not found
 */
int vm_fault(int faulttype, vaddr_t faultaddress) {
	(void) faulttype;
	(void) faultaddress;

	return 0;
}


/*
 *  segment initialization
 */
void
as_zero_region(paddr_t paddr, unsigned npages)
{
	bzero((void *)PADDR_TO_KVADDR(paddr), npages * PAGE_SIZE);
}
