#include <sea/kernel.h>
#include <sea/mm/vmm.h>
#include <sea/tm/process.h>
#include <sea/mm/swap.h>
#include <sea/loader/elf.h>
#include <sea/mm/map.h>
static void print_pfe(int x, registers_t *regs, addr_t cr2)
{
	assert(regs);
	printk (x, "Woah! Page Fault at 0x%x, faulting address %x\n", regs->eip, cr2);
	if(!(regs->err_code & 1))
		printk (x, "Present, ");
	else
		printk(x, "Non-present, ");
	if(regs->err_code & 2)
		printk (x, "read-only, ");
	printk(x, "while in ");
	if(regs->err_code & 4)
		printk (x, "User");
	else
		printk(x, "Supervisor");
	printk(x, " mode");
	printk(x, "\nIn function");
	const char *g = elf32_lookup_symbol (regs->eip, &kernel_elf);
	printk(x, " [0x%x] %s\n", regs->eip, g ? g : "(unknown)");
	printk(x, "Occured in task %d.\n\tstate=%d, flags=%d, F=%d, magic=%x.\n\tlast syscall=%d", current_task->pid, current_task->state, current_task->flags, current_task->flag, current_task->magic, current_task->last);
	if(current_task->system) 
		printk(x, ", in syscall %d", current_task->system);
	printk(x, "\n");
}
#define USER_TASK (err_code & 0x4)

static int do_map_page(addr_t addr, unsigned attr)
{
	addr &= PAGE_MASK;
	if(!mm_vm_get_map(addr, 0, 1))
		mm_vm_map(addr, mm_alloc_physical_page(), attr, MAP_CRIT | MAP_PDLOCKED);
	return 1;
}

static int map_in_page(unsigned long cr2, unsigned err_code)
{
	if(cr2 >= current_task->heap_start && cr2 <= current_task->heap_end) {
		do_map_page(cr2, PAGE_PRESENT | PAGE_WRITE | PAGE_USER);
		memset((void *)(cr2&PAGE_MASK), 0, PAGE_SIZE);
		return 1;
	}
	if(cr2 >= TOP_TASK_MEM_EXEC && cr2 < (TOP_TASK_MEM_EXEC+STACK_SIZE*2))
		return do_map_page(cr2, PAGE_PRESENT | PAGE_WRITE | PAGE_USER);
	return 0;
}

void arch_mm_page_fault(registers_t *regs, int int_no)
{
	assert(regs);
	addr_t cr2, err_code = regs->err_code;
	__asm__ volatile ("mov %%cr2, %0" : "=r" (cr2));
	int pf_error=0;
	if(!(err_code & 1))
		pf_error |= PF_CAUSE_NONPRESENT;
	if(err_code & (1 << 1))
		pf_error |= PF_CAUSE_WRITE;
	else
		pf_error |= PF_CAUSE_READ;
	if(err_code & (1 << 2))
		pf_error |= PF_CAUSE_USER;
	else
		pf_error |= PF_CAUSE_SUPER;
	if(err_code & (1 << 3))
		pf_error |= PF_CAUSE_RSVD;
	if(err_code & (1 << 4))
		pf_error |= PF_CAUSE_IFETCH;
	if(USER_TASK) {
		/* if we were in a user-space task, we can actually just
		 * pretend to be a second-stage interrupt handler. */
		assert(!current_task->sysregs);
		current_task->sysregs = regs;
		#if CONFIG_SWAP
		/* Has the page been swapped out? NOTE: We must always check this first */
		if(current_task && num_swapdev && current_task->num_swapped && 
			swap_in_page((task_t *)current_task, cr2 & PAGE_MASK) == 0) {
			printk(1, "[swap]: Swapped back in page %x for task %d\n", 
				   cr2 & PAGE_MASK, current_task->pid);
			current_task->sysregs = 0;
			return;
		}
		#endif
		
		if(mm_page_fault_test_mappings(cr2, pf_error) == 0) {
			current_task->sysregs = 0;
			return;
		}
		
		print_pfe(0, regs, cr2);
		mutex_acquire(&pd_cur_data->lock);
		if(map_in_page(cr2, err_code)) {
			mutex_release(&pd_cur_data->lock);
			current_task->sysregs = 0;
			return;
		}
		mutex_release(&pd_cur_data->lock);
		
		printk(0, "[pf]: Invalid Memory Access in task %d: eip=%x addr=%x flags=%x\n", 
			   current_task->pid, regs->eip, cr2, err_code);
		printk(0, "[pf]: task heap: %x -> %x\n", current_task->heap_start, current_task->heap_end);
		kprintf("[pf]: Segmentation Fault\n");
		tm_kill_process(current_task->pid);
		current_task->sysregs = 0;
		return;
	} else {
		/* this may not be safe... */
		if(mm_page_fault_test_mappings(cr2, pf_error) == 0)
			return;
	}
	print_pfe(5, regs, cr2);
	if(!current_task) {
		if(kernel_task)
		{
			/* Page fault while panicing */
			
			for(;;) asm("cli;hlt");
		}
		panic(PANIC_MEM | PANIC_NOSYNC, "Early Page Fault (pf_cause=%x)", pf_error);
	}
	panic(PANIC_MEM | PANIC_NOSYNC, "Page Fault (pf_cause=%x)", pf_error);
}
