#include <sea/tm/process.h>
#include <sea/tm/thread.h>
#include <sea/ll.h>
#include <sea/cpu/atomic.h>
#include <sea/mm/kmalloc.h>
#include <sea/mm/map.h>
#include <sea/fs/inode.h>
#include <sea/fs/file.h>
#include <sea/cpu/interrupt.h>

static pid_t __next_pid = 0;
static pid_t __next_tid = 0;

pid_t tm_thread_next_tid(void)
{
	return add_atomic(&__next_tid, 1);
}

pid_t tm_process_next_pid(void)
{
	return add_atomic(&__next_pid, 1);
}

struct thread *tm_thread_fork(int flags)
{
	struct thread *thr = kmalloc(sizeof(struct thread));
	thr->magic = THREAD_MAGIC;
	thr->tid = tm_thread_next_tid();
	thr->flags = THREAD_FORK;
	thr->priority = current_thread->priority;
	thr->kernel_stack = tm_thread_reserve_kernelmode_stack();
	thr->sig_mask = current_thread->sig_mask;
	thr->refs = 1;
	mutex_create(&thr->block_mutex, MT_NOSCHED);
	return thr;
}

static void __copy_mappings(struct process *ch, struct process *pa)
{
	/* copy the memory mappings. All mapping types are inherited, but
	 * they only barely modified. Like, just enough for the new process
	 * to not screw up the universe. This is accomplished by the fact
	 * that everything is just pointers to sections of memory that are
	 * copied during mm_vm_clone anyway... */
	mutex_acquire(&pa->map_lock);
	struct llistnode *node;
	struct memmap *map;
	ll_for_each_entry(&pa->mappings, node, struct memmap *, map) {
		/* new mapping object, copy the data */
		struct memmap *n = kmalloc(sizeof(*map));
		memcpy(n, map, sizeof(*n));
		/* of course, we have another reference to the backing inode */
		vfs_inode_get(n->node);
		if(map->flags & MAP_SHARED) {
			/* and if it's shared, tell the inode that another processor
			 * cares about some section of memory */
			fs_inode_map_region(n->node, n->offset, n->length);
		}
		n->entry = ll_insert(&ch->mappings, n);
	}
	/* for this simple copying of data to work, we rely on the address space being cloned BEFORE
	 * this is called, so the pointers are actually valid */
	memcpy(&(ch->mmf_valloc), &(pa->mmf_valloc), sizeof(pa->mmf_valloc));
	ch->mmf_valloc.lock.lock = 0;
	mutex_release(&pa->map_lock);
}

static struct process *tm_process_copy(int flags)
{
	/* copies the current_process structure into
	 * a new one (cloning the things that need
	 * to be cloned) */
	struct process *newp = kmalloc(sizeof(struct process));
	newp->magic = PROCESS_MAGIC;
	mm_vm_clone(&current_process->vmm_context, &newp->vmm_context);
	newp->pid = tm_process_next_pid();
	newp->cmask = current_process->cmask;
	newp->refs = 1;
	newp->tty = current_process->tty;
	newp->heap_start = current_process->heap_start;
	newp->heap_end = current_process->heap_end;
	newp->global_sig_mask = current_process->global_sig_mask;
	memcpy((void *)newp->signal_act, current_process->signal_act, sizeof(struct sigaction) * NUM_SIGNALS);
	tm_process_inc_reference(current_process);
	newp->parent = current_process;
	ll_create(&newp->threadlist);
	ll_create_lockless(&newp->mappings);
	mutex_create(&newp->map_lock, MT_NOSCHED); /* we need to lock this during page faults */
	mutex_create(&newp->stacks_lock, 0);
	/* TODO: what the fuck is this? */
	valloc_create(&newp->mmf_valloc, MMF_BEGIN, MMF_END, PAGE_SIZE, VALLOC_USERMAP);
	for(addr_t a = MMF_BEGIN;a < (MMF_BEGIN + (size_t)newp->mmf_valloc.nindex);a+=PAGE_SIZE)
		mm_vm_set_attrib(a, PAGE_PRESENT | PAGE_WRITE);
	__copy_mappings(newp, current_process);
	if(current_process->root) {
		newp->root = current_process->root;
		vfs_inode_get(newp->root);
	}
	if(current_process->cwd) {
		newp->cwd = current_process->cwd;
		vfs_inode_get(newp->cwd);
	}
	fs_copy_file_handles(current_process, newp);
	mutex_create(&newp->files_lock, 0);
	return newp;
}

void tm_thread_add_to_process(struct thread *thr, struct process *proc)
{
	ll_do_insert(&proc->threadlist, &thr->pnode, thr);
	thr->process = proc;
	tm_process_inc_reference(proc);
	add_atomic(&proc->thread_count, 1);
}

void tm_thread_add_to_cpu(struct thread *thr, struct cpu *cpu)
{
	thr->cpu = cpu;
	add_atomic(&cpu->numtasks, 1);
	tqueue_insert(cpu->active_queue, thr, &thr->activenode);
}

struct cpu *tm_fork_pick_cpu(void)
{
#if CONFIG_SMP
	static int last_cpu = 0;
	struct cpu *cpu = cpu_get(add_atomic(&last_cpu, 1));
	if(!(cpu->flags & CPU_RUNNING))
		cpu = 0;
	if(!cpu) {
		last_cpu = 0;
		return primary_cpu;
	}
	return cpu;
#else
	return primary_cpu;
#endif
}

int tm_clone(int flags)
{
	struct process *proc = current_process;
	if(!(flags & CLONE_SHARE_PROCESS)) {
		proc = tm_process_copy(flags);
		add_atomic(&running_processes, 1);
		tm_process_inc_reference(proc);
		assert(!hash_table_set_entry(process_table, &proc->pid, sizeof(proc->pid), 1, proc));
		ll_do_insert(process_list, &proc->listnode, proc);
	} else if(flags & CLONE_KTHREAD) {
		proc = kernel_process;
	}
	struct thread *thr = tm_thread_fork(flags);
	assert(!hash_table_set_entry(thread_table, &thr->tid, sizeof(thr->tid), 1, thr));
	add_atomic(&running_threads, 1);
	tm_thread_add_to_process(thr, proc);
	thr->state = THREADSTATE_UNINTERRUPTIBLE;
	thr->usermode_stack_num = tm_thread_reserve_usermode_stack(thr);
	thr->usermode_stack_end = tm_thread_usermode_stack_end(thr->usermode_stack_num);

	struct cpu *target_cpu = tm_fork_pick_cpu();

	cpu_disable_preemption();
	arch_tm_fork_setup_stack(thr);


	if(current_thread == thr) {
		current_thread->jump_point = 0;
		cpu_enable_preemption();
		return 0;
	} else {
		cpu_enable_preemption();
		thr->state = THREADSTATE_RUNNING;
		tm_thread_add_to_cpu(thr, target_cpu);
		return thr->tid;
	}
}

int sys_clone(int f)
{
	/* userspace calls aren't allowed to fork a kernel thread. */
	f &= ~CLONE_KTHREAD;
	return tm_clone(f);
}

/* vfork is a performance hack designed to make the normal fork-exec proces
 * faster by not copying page tables during fork. There isn't really a reason
 * for this now, since we can just do copy-on-write. So....just have this
 * call normal fork. */
int sys_vfork(void)
{
	return sys_clone(0);
}

