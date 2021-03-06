/* Routines for generating and handling Interprocessor Interrupts
 * 
 * A CPU may send an IPI for the following reasons (in order of priority):
 *   1) kernel panic
 *   2) kernel debug interrupt
 *   3) update TLB
 *   4) ack update TLB
 *   5) kernel shutdown
 *   6) reshedule a new thread
 */
#include <sea/config.h>
#if CONFIG_SMP
#include <sea/kernel.h>
#include <sea/tm/process.h>
#include <sea/mutex.h>
#include <sea/cpu/processor.h>
#include <sea/mm/vmm.h>

#include <sea/cpu/interrupt.h>
#include <sea/asm/system.h>
#if CONFIG_ARCH == TYPE_ARCH_X86
#include <sea/cpu/cpu-x86.h>
#else
#include <sea/cpu/cpu-x86_64.h>
#endif

#include <sea/spinlock.h>

struct spinlock ipi_lock;

int x86_cpu_send_ipi(unsigned char dest_shorthand, unsigned int dst, unsigned int v)
{
	assert((v & LAPIC_ICR_DM_INIT) || (v & LAPIC_ICR_LEVELASSERT));
	/* if we've initialized SMP, but we've disabled it, don't send any IPIs */
	if(!(kernel_state_flags & KSF_SMP_ENABLE))
		return 1;
	int to, send_status;
	int old = cpu_interrupt_set(0);
	spinlock_acquire(&ipi_lock);
	/* Writing to the lower ICR register causes the interrupt
	 * to get sent off (Intel 3A 10.6.1), so do the higher reg first */
	LAPIC_WRITE(LAPIC_ICR+0x10, (dst << 24));
	unsigned lower = v | (dest_shorthand << 18);
	/* gotta have assert for all except init */
	LAPIC_WRITE(LAPIC_ICR, lower);
	/* Wait for send to finish */
	to = 0;
	do {
		asm("pause");
		send_status = LAPIC_READ(LAPIC_ICR) & LAPIC_ICR_STATUS_PEND;
	} while (send_status && (to++ < 1000));
	spinlock_release(&ipi_lock);
	cpu_interrupt_set(old);
	return (to < 1000);
}

void arch_cpu_send_ipi(int dest, unsigned signal, unsigned flags)
{
	x86_cpu_send_ipi(dest, 0, LAPIC_ICR_LEVELASSERT | LAPIC_ICR_TM_LEVEL | signal);
}

#endif

