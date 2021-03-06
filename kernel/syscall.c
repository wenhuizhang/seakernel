
#include <sea/cpu/interrupt.h>
#include <sea/cpu/processor.h>
#include <sea/dm/dev.h>
#include <sea/errno.h>
#include <sea/fs/dir.h>
#include <sea/fs/inode.h>
#include <sea/fs/kerfs.h>
#include <sea/fs/mount.h>
#include <sea/fs/pipe.h>
#include <sea/fs/socket.h>
#include <sea/fs/stat.h>
#include <sea/kernel.h>
#include <sea/lib/timer.h>
#include <sea/loader/exec.h>
#include <sea/loader/module.h>
#include <sea/mm/map.h>
#include <sea/sys/stat.h>
#include <sea/sys/sysconf.h>
#include <sea/syscall.h>
#include <sea/tm/process.h>
#include <sea/tm/ptrace.h>
#include <sea/tm/thread.h>
#include <sea/uname.h>
#include <sea/vsprintf.h>
#include <sea/string.h>
#include <sea/dm/pty.h>
#include <sea/syslog.h>
/* #define SC_DEBUG 0 */
#define SC_TIMING 1
static unsigned int num_syscalls=0;
int sys_null(long a, long b, long c, long d, long e)
{
	#if CONFIG_DEBUG
	kprintf("[kernel]: Null system call (%d) called in task %d\n%x %x %x %x %x",
			current_thread->system, current_thread->tid, a, b, c, d, e);
	#endif
	return -ENOSYS;
}

int sys_gethost(int a, int b, int c, int d, int e)
{
	/* Remember to add systemcall buffer checks... */
	return -ENOSYS;
}

int sys_getserv(int a, int b, int c, int d, int e)
{
	return -ENOSYS;
}

int sys_setserv(int a, int b, int c, int d, int e)
{
	return -ENOSYS;
}


#include <sea/cpu/cpu-io.h>
int sys_setcurs(int x, int y)
{
	volatile unsigned short position = (y * 80 + x);
	outb(0x3D4, 0x0F);
	outb(0x3D5, (unsigned char)(position&0xFF));
	outb(0x3D4, 0x0E);
	outb(0x3D5, (unsigned char )((position>>8)&0xFF));
	return 0;
}

int sys_mapscreen(void)
{
	mm_virtual_map(0xb8000, 0xb8000, PAGE_PRESENT | PAGE_WRITE | PAGE_USER, 0x1000);
	return 0;
}

static void *syscall_table[129] = {
	[SYS_SETUP]           = SC sys_setup,
	[SYS_EXIT]            = SC sys_exit,
	[SYS_FORK]            = SC sys_clone,
	//[SYS_WAIT]            = SC tm_process_wait,
	[SYS_READ]            = SC sys_readpos,
	[SYS_WRITE]           = SC sys_writepos,
	[SYS_OPEN]            = SC sys_open,
	[SYS_CLOSE]           = SC sys_close,
	[SYS_FSTAT]           = SC sys_fstat,
	[SYS_STAT]            = SC sys_stat,
	[SYS_ISATTY]          = SC sys_isatty,
	[SYS_SEEK]            = SC sys_seek,
	[SYS_SIGNAL]          = SC sys_kill,
	[SYS_SBRK]            = SC sys_sbrk,
	[SYS_TIMES]           = SC sys_times,
	[SYS_DUP]             = SC sys_dup,
	[SYS_DUP2]            = SC sys_dup2,
	[SYS_IOCTL]           = SC sys_ioctl,
	[SYS_VFORK]           = SC sys_vfork,
	[SYS_RECV]            = SC sys_recv,
	[SYS_SEND]            = SC sys_send,
	[SYS_SOCKET]          = SC sys_socket,
	[SYS_ACCEPT]          = SC sys_accept,
	[SYS_CONNECT]         = SC sys_connect,
	[SYS_LISTEN]          = SC sys_listen,
	[SYS_BIND]            = SC sys_bind,
	[SYS_EXECVE]          = SC execve,
	#if CONFIG_MODULES
	[SYS_LMOD]            = SC sys_load_module,
	[SYS_ULMOD]           = SC sys_unload_module,

	[SYS_ULALLMODS]       = SC loader_unload_all_modules,
	#else
	[SYS_LMOD]            = SC sys_null,
	[SYS_ULMOD]           = SC sys_null,

	[SYS_ULALLMODS]       = SC sys_null,
	#endif
	[SYS_GETPID]          = SC sys_get_pid,
	[SYS_GETPPID]         = SC sys_getppid,
	[SYS_LINK]            = SC sys_link,
	[SYS_UNLINK]          = SC sys_unlink,
	[SYS_THREAD_SETPRI]   = SC sys_thread_setpriority,
	[SYS_THREAD_KILL]     = SC sys_kill_thread,
	[SYS_OPENPTY]         = SC sys_openpty,
	[SYS_GETSOCKNAME]     = SC sys_getsockname,
	[SYS_CHROOT]          = SC sys_chroot,
	[SYS_CHDIR]           = SC sys_chdir,
	[SYS_MOUNT]           = SC sys_mount,
	[SYS_UMOUNT]          = SC sys_umount,
	[SYS_ATTACHPTY]       = SC sys_attach_pty,
	[SYS_MKDIR]           = SC sys_mkdir,

	[SYS_SOCKSHUTDOWN]    = SC sys_sockshutdown,
	[SYS_GETSOCKOPT]      = SC sys_getsockopt,
	[SYS_SETSOCKOPT]      = SC sys_setsockopt,
	[SYS_RECVFROM]        = SC sys_recvfrom,
	[SYS_SENDTO]          = SC sys_sendto,
	[SYS_SYNC]            = SC sys_sync,
	[SYS_RMDIR]           = SC sys_rmdir,
	[SYS_FSYNC]           = SC sys_fsync,
	[SYS_ALARM]           = SC sys_alarm,
	[SYS_SELECT]          = SC sys_select,
	[SYS_GETDENTS]        = SC sys_getdents,
	[SYS_MAPSCREEN]       = SC sys_mapscreen,

	[SYS_SYSCONF]         = SC sys_sysconf,
	[SYS_SETSID]          = SC sys_setsid,
	[SYS_SETPGID]         = SC sys_setpgid,

	[SYS_SWAPON]          = SC sys_null,
	[SYS_SWAPOFF]         = SC sys_null,

	[SYS_NICE]            = SC sys_nice,
	[SYS_MMAP]            = SC sys_mmap,
	[SYS_MUNMAP]          = SC sys_munmap,
	[SYS_MSYNC]           = SC sys_msync,
	//[SYS_TSTAT]           = SC sys_task_stat,
	[SYS_PTRACE]          = SC sys_ptrace,

	[SYS_DELAY]           = SC sys_delay,
	[SYS_KRESET]          = SC kernel_reset,
	[SYS_KPOWOFF]         = SC kernel_poweroff,
	[SYS_GETUID]          = SC tm_get_uid,
	[SYS_GETGID]          = SC tm_get_gid,
	[SYS_SETUID]          = SC tm_set_uid,
	[SYS_SETGID]          = SC tm_set_gid,
	[SYS_MEMSTAT]         = SC sys_null,
	//[SYS_TPSTAT]          = SC sys_task_pstat,
	[SYS_MOUNT2]          = SC sys_mount2,
	[SYS_SETEUID]         = SC tm_set_euid,
	[SYS_SETEGID]         = SC tm_set_egid,
	[SYS_PIPE]            = SC sys_pipe,
	//[SYS_SETSIG]          = SC tm_set_signal,
	[SYS_GETEUID]         = SC tm_get_euid,
	[SYS_GETEGID]         = SC tm_get_egid,
	[SYS_GETTIMEEPOCH]    = SC time_get_epoch,

	[SYS_GETTIME]         = SC time_get,
	//[SYS_TIMERTH]         = SC sys_get_timer_th,
	//[SYS_ISSTATE]         = SC sys_isstate,
	[SYS_WAIT3]           = SC sys_wait3,

	[SYS_SETCURS]         = SC sys_setcurs,

	//[SYS_GETCWDLEN]       = SC sys_getcwdlen,
	#if CONFIG_SWAP
	[SYS_SWAPTASK]        = SC /**96*/sys_swaptask,
	#else
	[SYS_SWAPTASK]        = SC /**96*/sys_null,
	#endif
	//[SYS_DIRSTAT]         = SC sys_dirstat,
	[SYS_SIGACT]          = SC sys_sigact,
	[SYS_ACCESS]          = SC sys_access,
	[SYS_CHMOD]           = SC sys_chmod,
	[SYS_FCNTL]           = SC sys_fcntl,
	//[SYS_DIRSTATFD]       = SC sys_dirstat_fd,
	//[SYS_GETDEPTH]        = SC sys_getdepth,
	[SYS_WAITPID]         = SC sys_waitpid,
	[SYS_MKNOD]           = SC sys_mknod,
	[SYS_SYMLINK]         = SC sys_symlink,
	[SYS_READLINK]        = SC sys_readlink,
	[SYS_UMASK]           = SC sys_umask,
	[SYS_SIGPROCMASK]     = SC sys_sigprocmask,
	[SYS_FTRUNCATE]       = SC sys_ftruncate,
	//[SYS_GETNODESTR]      = SC sys_getnodestr,
	[SYS_CHOWN]           = SC sys_chown,
	[SYS_UTIME]           = SC sys_utime,
	[SYS_GETHOSTNAME]     = SC sys_gethostname,
	[SYS_GSPRI]           = SC sys_gsetpriority,
	[SYS_UNAME]           = SC sys_uname,
	[SYS_GETHOST]         = SC sys_gethost,
	[SYS_GETSERV]         = SC sys_getserv,
	[SYS_SETSERV]         = SC sys_setserv,
	[SYS_SYSLOG]          = SC sys_syslog,
	[SYS_POSFSSTAT]       = sys_posix_fsstat,
	[SYS_GETTIMEOFDAY]    = sys_gettimeofday,


	[SYS_SIGPENDING]      = sys_sigpending,
	[SYS_SIGSUSPEND]      = sys_sigsuspend,



	//[SYS_WAITAGAIN] = SC sys_waitagain,
	[SYS_RET_FROM_SIG] = SC sys_null,
};

struct timer systimers[129];
unsigned int syscounts[129];

void syscall_init(void)
{
	num_syscalls = sizeof(syscall_table)/sizeof(void *);
	for(int i=0;i<129;i++) {
		timer_create(&systimers[i], 0);
		syscounts[i]=0;
	}
}

int mm_is_valid_user_pointer(int num, void *p, char flags)
{
	addr_t addr = (addr_t)p;
	if(!addr && !flags) return 0;
	if(!(kernel_state_flags & KSF_HAVEEXECED))
		return 1;
	if(addr >= MEMMAP_USERSPACE_MAXIMUM) {
		#if CONFIG_DEBUG
		printk(0, "[kernel]: warning - task %d passed ptr %x to syscall %d (invalid)\n",
			   current_thread->tid, addr, num);
		#endif
		return 0;
	}
	return 1;
}

/* here we test to make sure that the task passed in valid pointers
 * to syscalls that have pointers are arguments, so that we make sure
 * we only ever modify user-space data when we think we're modifying
 * user-space data. */
int check_pointers(struct registers *regs)
{
	switch(SYSCALL_NUM_AND_RET) {
		case SYS_READ: case SYS_FSTAT: case SYS_STAT: /*case SYS_GETPATH:*/
		case SYS_READLINK: /*case SYS_GETNODESTR:*/
		case SYS_POSFSSTAT: case SYS_WRITE:
			return mm_is_valid_user_pointer(SYSCALL_NUM_AND_RET, (void *)_B_, 0);

		case SYS_TIMES: /*case SYS_GETPWD:*/ case SYS_PIPE:
		case SYS_MEMSTAT: case SYS_GETTIME: case SYS_GETHOSTNAME:
		case SYS_UNAME: case SYS_MSYNC: case SYS_MUNMAP:
			return mm_is_valid_user_pointer(SYSCALL_NUM_AND_RET, (void *)_A_, 0);

		case SYS_SETSIG: case SYS_WAITPID:
			return mm_is_valid_user_pointer(SYSCALL_NUM_AND_RET, (void *)_B_, 1);

		case SYS_SELECT:
			if(!mm_is_valid_user_pointer(SYSCALL_NUM_AND_RET, (void *)_B_, 1))
				return 0;
			if(!mm_is_valid_user_pointer(SYSCALL_NUM_AND_RET, (void *)_C_, 1))
				return 0;
			if(!mm_is_valid_user_pointer(SYSCALL_NUM_AND_RET, (void *)_D_, 1))
				return 0;
			if(!mm_is_valid_user_pointer(SYSCALL_NUM_AND_RET, (void *)_E_, 1))
				return 0;
			break;

		//case SYS_DIRSTAT:
		//	if(!mm_is_valid_user_pointer(SYSCALL_NUM_AND_RET, (void *)_A_, 0))
		//		return 0;
		/* fall through *
		case SYS_DIRSTATFD:
			if(!mm_is_valid_user_pointer(SYSCALL_NUM_AND_RET, (void *)_C_, 0))
				return 0;
			if(!mm_is_valid_user_pointer(SYSCALL_NUM_AND_RET, (void *)_D_, 0))
				return 0;
			break;
			*/

		case SYS_SIGACT: case SYS_SIGPROCMASK:
			return mm_is_valid_user_pointer(SYSCALL_NUM_AND_RET, (void *)_C_, 1);

		case SYS_CHOWN: case SYS_CHMOD: case SYS_TIMERTH: case SYS_CHDIR: case SYS_CHROOT:
			return mm_is_valid_user_pointer(SYSCALL_NUM_AND_RET, (void *)_A_, 1);

		case SYS_LMOD:
			if(!mm_is_valid_user_pointer(SYSCALL_NUM_AND_RET, (void *)_B_, 1))
				return 0;
		/* fall through */
		case SYS_ULMOD: case SYS_OPEN:
			if(!mm_is_valid_user_pointer(SYSCALL_NUM_AND_RET, (void *)_A_, 0))
				return 0;
			break;
		case SYS_MMAP:
			if(!mm_is_valid_user_pointer(SYSCALL_NUM_AND_RET, (void *)_A_, 1))
				return 0;
			if(!mm_is_valid_user_pointer(SYSCALL_NUM_AND_RET, (void *)_B_, 0))
				return 0;
			break;
		//default:
		//	printk(0, ":: UNTESTED SYSCALL: %d\n", SYSCALL_NUM_AND_RET);
	}
	return 1;
}

int syscall_handler(struct registers *regs)
{
	/* SYSCALL_NUM_AND_RET is defined to be the correct register in the syscall regs struct. */
	if(unlikely(SYSCALL_NUM_AND_RET >= num_syscalls))
		return -ENOSYS;
	if(unlikely(!syscall_table[SYSCALL_NUM_AND_RET])) {
		kprintf("[syscall]: (from %d(%d)) no such syscall %d\n", current_thread->tid, current_process->pid, SYSCALL_NUM_AND_RET);
		return -ENOSYS;
	}
	long ret = 0;
	if(!check_pointers(regs))
		return -EINVAL;
	if(kernel_state_flags & KSF_SHUTDOWN)
		for(;;);

	do {
		ret = 0;
		tm_thread_enter_system(SYSCALL_NUM_AND_RET);
		/* most syscalls are pre-emptible, so we enable interrupts and
	 	 * expect handlers to disable them if needed */
		cpu_interrupt_set(1);
		/* start accounting information! */
		atomic_fetch_add_explicit(&syscounts[SYSCALL_NUM_AND_RET], 1, memory_order_relaxed);

#ifdef SC_DEBUG
		if(SYSCALL_NUM_AND_RET != 0
				&& (current_process->pid != 2 || 1))
			printk(SC_DEBUG, "syscall %d(%d) (from: %x): enter %d %x\n",
					current_thread->tid, current_process->pid,
					current_thread->regs->eip, SYSCALL_NUM_AND_RET, _A_);
#endif
#ifdef SC_TIMING
		int timer_did_start = timer_start(&systimers[SYSCALL_NUM_AND_RET]);
#endif
		__do_syscall_jump(ret, syscall_table[SYSCALL_NUM_AND_RET], _E_, _D_,
				_C_, _B_, _A_);
#ifdef SC_TIMING
		if(!(SYSCALL_NUM_AND_RET == SYS_FORK && ret == 0)) {
			if(timer_did_start) timer_stop(&systimers[SYSCALL_NUM_AND_RET]);
		}
#endif
#ifdef SC_DEBUG
		if((ret < 0 || 1) && (ret == -EINTR || 1) && (current_process->pid != 2 || 1))
			printk(SC_DEBUG, "syscall pid %3d: #%3d ret %4d\n",
			   		current_thread->tid, current_thread->system, ret < 0 ? -ret : ret);
#endif
		cpu_interrupt_set(0);
		tm_thread_exit_system(SYSCALL_NUM_AND_RET, ret);
		/* if we need to reschedule, or we have overused our timeslice
	 	 * then we need to reschedule. this prevents tasks that do a continuous call
	 	 * to write() from starving the resources of other tasks. syscall_count resets
	 	 * on each call to tm_tm_schedule() */
		if(current_thread->flags & THREAD_SCHEDULE)
		{
			/* clear out the flag. Either way in the if statement, we've rescheduled. */
			tm_schedule();
		}
	} while(ret == -ERESTART);
	/* store the return value in the regs */
	SYSCALL_NUM_AND_RET = ret;
	/* if we're going to jump to a signal here, we need to back up our
	 * return value so that when we return to the system we have the
	 * original systemcall returning the proper value.
	 */
	return ret;
}

int kerfs_syscall_report(int direction, void *param, size_t size, size_t offset, size_t length, unsigned char *buf)
{
	size_t current = 0;
	KERFS_PRINTF(offset, length, buf, current,
			"Times are in microseconds, CALLS*MEAN in milliseconds.\n SC   # CALLS\t      MIN\t      MAX\t     MEAN\tCALLS*MEAN\n");
	for(int i=0;i<129;i++) {
		if(!syscounts[i])
			continue;
		KERFS_PRINTF(offset, length, buf, current,
				"%3d:\t%5d\t%9d\t%9d\t%9d\t%10d\n", i,
				syscounts[i], (uint32_t)systimers[i].min / 1000,
				(uint32_t)systimers[i].max / 1000, (uint32_t)systimers[i].mean / 1000,
				(uint32_t)((syscounts[i] * systimers[i].mean) / (1000 * 1000)));
	}
	return current;
}

