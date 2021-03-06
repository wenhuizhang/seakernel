/* kernel.c: Copyright (c) 2010 Daniel Bittman
 * Provides some fairly standard functions for the kernel */
#include <sea/kernel.h>
#include <sea/asm/system.h>
#include <sea/loader/module.h>
#include <sea/tm/process.h>
#include <sea/cpu/processor.h>
#include <sea/fs/mount.h>
#include <sea/cpu/interrupt.h>
#include <sea/asm/system.h>
#include <sea/vsprintf.h>
#if CONFIG_ARCH == TYPE_ARCH_X86
#include <sea/cpu/cpu-x86.h>
#else
#include <sea/cpu/cpu-x86_64.h>
#endif

int PRINT_LEVEL = DEF_PRINT_LEVEL;
_Atomic unsigned kernel_state_flags=0;

void kernel_shutdown(void)
{
	current_process->effective_uid=current_process->real_uid=0;
	set_ksf(KSF_SHUTDOWN);
	sys_sync(PRINT_LEVEL);
	fs_unmount_all();
#if CONFIG_MODULES
	//loader_unload_all_modules(1);
#endif
#if CONFIG_SMP
	printk(0, "[smp]: shutting down application processors\n");
	cpu_send_ipi(CPU_IPI_DEST_OTHERS, IPI_SHUTDOWN, 0);
	while(cpu_get_num_halted_processors() 
			< cpu_get_num_secondary_processors())
		cpu_pause();
#endif
	kprintf("Everything under the sun is in tune, but the sun is eclipsed by the moon.\n");
}

void kernel_reset(void)
{
	if(current_process->effective_uid)
		return;
	kernel_shutdown();
	kprintf("Rebooting system...\n");
	cpu_reset();
}

void kernel_poweroff(void)
{
	if(current_process->effective_uid)
		return;
	kernel_shutdown();
	cpu_interrupt_set(0);
	kprintf("\nYou can now turn off your computer.\n");
	for(;;) 
		arch_cpu_halt();
}
