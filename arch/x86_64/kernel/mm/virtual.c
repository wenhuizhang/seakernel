/* Defines functions for virtual memory management */
#include <kernel.h>
#include <memory.h>
#include <isr.h>
#include <task.h>
#include <cpu.h>
#include <atomic.h>
volatile addr_t *kernel_dir=0;
int id_tables=0;
struct pd_data *pd_cur_data = (struct pd_data *)PDIR_DATA;
extern void id_map_apic(page_dir_t *);
/* This function will setup a paging environment with a basic page dir, 
 * enough to process the memory map passed by grub */

void early_vm_map(pml4_t *pml4, addr_t addr, addr_t map)
{
	pdpt_t *pdpt;
	page_dir_t *pd;
	page_table_t *pt;
	
	if(!pml4[PML4_IDX(addr/0x1000)])
		pml4[PML4_IDX(addr/0x1000)] = pm_alloc_page() | PAGE_PRESENT | PAGE_WRITE;
	pdpt = (addr_t *)(pml4[PML4_IDX(addr/0x1000)] & PAGE_MASK);
	if(!pdpt[PDPT_IDX(addr/0x1000)])
		pdpt[PDPT_IDX(addr/0x1000)] = pm_alloc_page() | PAGE_PRESENT | PAGE_WRITE;
	pd = (addr_t *)(pdpt[PDPT_IDX(addr/0x1000)] & PAGE_MASK);
	if(!pd[PAGE_DIR_IDX(addr/0x1000)])
		pd[PAGE_DIR_IDX(addr/0x1000)] = pm_alloc_page() | PAGE_PRESENT | PAGE_WRITE;
	/* passing map as zero allows us to map in all the tables, but leave the
	 * true mapping null. This is handy for the page stack and heap */
	pt = (addr_t *)(pd[PAGE_DIR_IDX(addr/0x1000)] & PAGE_MASK);
	pt[PAGE_TABLE_IDX(addr/0x1000)] = map;
}

void early_vm_map_2m(pml4_t *pml4, addr_t addr, addr_t map)
{
	pdpt_t *pdpt;
	page_dir_t *pd;
	page_table_t *pt;
	
	if(!pml4[PML4_IDX(addr/0x1000)])
		printk(0, "PML4\n"), pml4[PML4_IDX(addr/0x1000)] = pm_alloc_page() | PAGE_PRESENT | PAGE_WRITE;
	pdpt = (addr_t *)(pml4[PML4_IDX(addr/0x1000)] & PAGE_MASK);
	if(!pdpt[PDPT_IDX(addr/0x1000)])
		printk(0, "PDPT\n"), pdpt[PDPT_IDX(addr/0x1000)] = pm_alloc_page() | PAGE_PRESENT | PAGE_WRITE;
	pd = (addr_t *)(pdpt[PDPT_IDX(addr/0x1000)] & PAGE_MASK);
	pd[PAGE_DIR_IDX(addr/0x1000)] = map | (1 << 7);
}

void vm_init(addr_t id_map_to)
{
	/* Register some stuff... */
	register_interrupt_handler (14, (isr_t)&page_fault, 0);

	/* Create kernel directory */
	pml4_t *pml4 = (addr_t *)pm_alloc_page();
	memset(pml4, 0, 0x1000);
	/* Identity map the kernel */
	pml4[0] = pm_alloc_page() | PAGE_PRESENT | PAGE_WRITE;
	pdpt_t *pdpt = (addr_t *)(pml4[0] & PAGE_MASK);
	pdpt[0] = pm_alloc_page() | PAGE_PRESENT | PAGE_WRITE;
	page_dir_t *pd = (addr_t *)(pdpt[0] & PAGE_MASK);
	pd[0] = (addr_t)(pm_alloc_page() | PAGE_PRESENT | PAGE_WRITE);
	pd[1] = (addr_t)(pm_alloc_page() | PAGE_PRESENT | PAGE_WRITE);
	pd[2] = (addr_t)(pm_alloc_page() | PAGE_PRESENT | PAGE_WRITE);
	pd[3] = (addr_t)(pm_alloc_page() | PAGE_PRESENT | PAGE_WRITE);
	pd[4] = (addr_t)(pm_alloc_page() | PAGE_PRESENT | PAGE_WRITE);
	pd[5] = (addr_t)(pm_alloc_page() | PAGE_PRESENT | PAGE_WRITE);
	pd[6] = (addr_t)(pm_alloc_page() | PAGE_PRESENT | PAGE_WRITE);
	
	page_table_t *pt;
	addr_t address = 0;
	for(int pdi = 0; pdi < 7; pdi++)
	{
		pt = (addr_t *)(pd[pdi] & PAGE_MASK);
		for(int t = 0; t < 512; t++)
		{
			pt[t] = address | PAGE_PRESENT | PAGE_WRITE;
			address += 0x1000;
		}
	}

	/* map in all possible physical memory, up to 512 GB. This way we can
	 * access any physical page by simple accessing virtual memory (phys + PHYS_PAGE_MAP).
	 * This should make mapping memory a LOT easier */
	pml4[PML4_IDX(PHYS_PAGE_MAP/0x1000)] = pm_alloc_page() | PAGE_PRESENT | PAGE_WRITE;
	pdpt = (addr_t *)(pml4[PML4_IDX(PHYS_PAGE_MAP/0x1000)] & PAGE_MASK);
	if(primary_cpu->cpuid.ext_features_edx & CPU_EXT_FEATURES_GBPAGE) {
		printk(0, "!!! CPU supports 1GB pages. This is untested code !!!\n");
		for(int i = 0; i < 512; i++)
			pdpt[i] = ((addr_t)0x40000000)*i | PAGE_PRESENT | PAGE_WRITE | (1 << 7);
	} else {
		address = 0;
		for(int i = 0; i < 512; i++)
		{
			pdpt[i] = pm_alloc_page() | PAGE_PRESENT | PAGE_WRITE;
			pd = (addr_t *)(pdpt[i] & PAGE_MASK);
			for(int j = 0; j < 512; j++) {
				pd[j] = address | PAGE_PRESENT | PAGE_WRITE | (1 << 7);
				address += 0x200000;
			}
		}
	}
#if CONFIG_SMP
	id_map_apic(pd);
#endif
	/* map in the signal return inject code. we need to do this, because
	 * user code may not run the the kernel area of the page directory */
	early_vm_map(pml4, SIGNAL_INJECT, pm_alloc_page() | PAGE_PRESENT | PAGE_WRITE);
	/* CR3 requires the physical address, so we directly 
	 * set it because we have the physical address */
	printk(0, "Setting new CR3...\n");
	asm("mov %0, %%cr3"::"r"(pml4));
 	kernel_dir = pml4;
	/* Enable paging */
	printk(0, "Paging enabled!\n");
	set_ksf(KSF_PAGING);
	memset(0, 0, 0x1000);
}

/* This relocates the stack to a safe place which is copied 
 * upon clone, and creates a new directory that is...well, complete */
void vm_init_2()
{
	setup_kernelstack(id_tables);
#if CONFIG_SMP
	unsigned int i=0;
	while(i < cpu_array_num)
	{
		printk(0, "[mm]: cloning directory for processor %d (%x)\n", cpu_array[i].apicid, &cpu_array[i]);
		page_dir_t *pd = vm_clone(page_directory, 0);
		cpu_array[i].kd_phys = pd[1023] & PAGE_MASK;
		cpu_array[i].kd = pd;
		i++;
	}
#else
	primary_cpu->kd = vm_clone(page_directory, 0);
	primary_cpu->kd_phys = primary_cpu->kd[1023] & PAGE_MASK;
#endif
	kernel_dir = primary_cpu->kd;
	vm_switch((page_dir_t *)primary_cpu->kd);
}

void vm_switch(page_dir_t *n/*VIRTUAL ADDRESS*/)
{
	/* n[1023] is the mapped bit that loops to itself */
	//__asm__ volatile ("mov %0, %%cr3" : : "r" (n[1023]&PAGE_MASK));
}

addr_t vm_do_getmap(addr_t v, addr_t *p, unsigned locked)
{
	addr_t vpage = (v&PAGE_MASK)/0x1000;
	unsigned vp4 = PML4_IDX(vpage);
	unsigned vpdpt = PDPT_IDX(vpage);
	unsigned vdir = PAGE_DIR_IDX(vpage);
	unsigned vtbl = PAGE_TABLE_IDX(vpage);
	if(kernel_task && !locked)
		mutex_acquire(&pd_cur_data->lock);
	page_dir_t *pd;
	page_table_t *pt;
	pdpt_t *pdpt;
	pml4_t *pml4;
	
	pml4 = (pml4_t *)((kernel_task && current_task) ? current_task->pd : kernel_dir);
	if(!pml4[vp4])
		pml4[vp4] = pm_alloc_page() | PAGE_PRESENT | PAGE_WRITE;
	pdpt = (addr_t *)((pml4[vp4]&PAGE_MASK) + PHYS_PAGE_MAP);
	if(!pdpt[vpdpt])
		pdpt[vpdpt] = pm_alloc_page() | PAGE_PRESENT | PAGE_WRITE;
	pd = (addr_t *)((pdpt[vpdpt]&PAGE_MASK) + PHYS_PAGE_MAP);
	if(!pd[vdir])
		pd[vdir] = pm_alloc_page() | PAGE_PRESENT | PAGE_WRITE;
	pt = (addr_t *)((pd[vdir]&PAGE_MASK) + PHYS_PAGE_MAP);
	
	addr_t ret = pt[vtbl] & PAGE_MASK;
	if(kernel_task && !locked)
		mutex_release(&pd_cur_data->lock);
	if(p)
		*p = ret;
	return ret;
}

unsigned int vm_setattrib(addr_t v, short attr)
{
	addr_t vpage = (v&PAGE_MASK)/0x1000;
	unsigned vp4 = PML4_IDX(vpage);
	unsigned vpdpt = PDPT_IDX(vpage);
	unsigned vdir = PAGE_DIR_IDX(vpage);
	unsigned vtbl = PAGE_TABLE_IDX(vpage);
	if(kernel_task)
		mutex_acquire(&pd_cur_data->lock);
	page_dir_t *pd;
	page_table_t *pt;
	pdpt_t *pdpt;
	pml4_t *pml4;
	
	pml4 = (pml4_t *)((kernel_task && current_task) ? current_task->pd : kernel_dir);
	if(!pml4[vp4])
		pml4[vp4] = pm_alloc_page() | PAGE_PRESENT | PAGE_WRITE;
	pdpt = (addr_t *)((pml4[vp4]&PAGE_MASK) + PHYS_PAGE_MAP);
	if(!pdpt[vpdpt])
		pdpt[vpdpt] = pm_alloc_page() | PAGE_PRESENT | PAGE_WRITE;
	pd = (addr_t *)((pdpt[vpdpt]&PAGE_MASK) + PHYS_PAGE_MAP);
	if(!pd[vdir])
		pd[vdir] = pm_alloc_page() | PAGE_PRESENT | PAGE_WRITE;
	pt = (addr_t *)((pd[vdir]&PAGE_MASK) + PHYS_PAGE_MAP);
	
	pt[vtbl] &= PAGE_MASK;
	pt[vtbl] |= attr;
	if(kernel_task)
		mutex_release(&pd_cur_data->lock);
	return 0;
}

unsigned int vm_do_getattrib(addr_t v, unsigned *p, unsigned locked)
{
	addr_t vpage = (v&PAGE_MASK)/0x1000;
	unsigned vp4 = PML4_IDX(vpage);
	unsigned vpdpt = PDPT_IDX(vpage);
	unsigned vdir = PAGE_DIR_IDX(vpage);
	unsigned vtbl = PAGE_TABLE_IDX(vpage);
	if(kernel_task && !locked)
		mutex_acquire(&pd_cur_data->lock);
	page_dir_t *pd;
	page_table_t *pt;
	pdpt_t *pdpt;
	pml4_t *pml4;
	
	pml4 = (pml4_t *)((kernel_task && current_task) ? current_task->pd : kernel_dir);
	if(!pml4[vp4])
		pml4[vp4] = pm_alloc_page() | PAGE_PRESENT | PAGE_WRITE;
	pdpt = (addr_t *)((pml4[vp4]&PAGE_MASK) + PHYS_PAGE_MAP);
	if(!pdpt[vpdpt])
		pdpt[vpdpt] = pm_alloc_page() | PAGE_PRESENT | PAGE_WRITE;
	pd = (addr_t *)((pdpt[vpdpt]&PAGE_MASK) + PHYS_PAGE_MAP);
	if(!pd[vdir])
		pd[vdir] = pm_alloc_page() | PAGE_PRESENT | PAGE_WRITE;
	pt = (addr_t *)((pd[vdir]&PAGE_MASK) + PHYS_PAGE_MAP);
	
	unsigned ret = pt[vtbl] & ATTRIB_MASK;
	if(kernel_task && !locked)
		mutex_release(&pd_cur_data->lock);
	if(p)
		*p = ret;
	return ret;
}
