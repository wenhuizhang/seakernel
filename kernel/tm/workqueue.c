#include <sea/types.h>
#include <sea/mm/kmalloc.h>
#include <sea/kernel.h>
#include <sea/cpu/atomic.h>
#include <sea/tm/async_call.h>
#include <sea/tm/workqueue.h>
#include <sea/cpu/processor.h>
/* TODO: roll this out to all kernel objects */
#define KOBJ_CREATE(obj,flags,alloc_flag) do {\
	if(!obj) {\
		obj = kmalloc(sizeof(*obj)); \
		obj->flags = flags | alloc_flag; \
	} else {\
		memset(obj, 0, sizeof(*obj)); \
		obj->flags = flags; \
	} \
	} while(0)

#define KOBJ_DESTROY(obj,alloc_flag) do {\
	if(obj->flags & alloc_flag)\
		kfree(obj);\
	} while(0)

struct workqueue *workqueue_create(struct workqueue *wq, int flags)
{
	KOBJ_CREATE(wq, flags, WORKQUEUE_KMALLOC);
	heap_create(&wq->tasks, HEAP_LOCKLESS, HEAPMODE_MAX);
	mutex_create(&wq->lock, MT_NOSCHED);
	return wq;
}

void workqueue_destroy(struct workqueue *wq)
{
	heap_destroy(&wq->tasks);
	mutex_destroy(&wq->lock);
	KOBJ_DESTROY(wq, WORKQUEUE_KMALLOC);
}

void workqueue_insert(struct workqueue *wq, struct async_call *call)
{
	mutex_acquire(&wq->lock);
	heap_insert(&wq->tasks, call->priority, call);
	mutex_release(&wq->lock);
	add_atomic(&wq->count, 1);
}

int workqueue_dowork(struct workqueue *wq)
{
	struct async_call *call;
	/* TODO: this can cause a relock if an IRQ fires inside here ... */
	int old = cpu_interrupt_set(0);
	mutex_acquire(&wq->lock);
	if(heap_pop(&wq->tasks, 0, (void **)&call) == 0) {
		mutex_release(&wq->lock);
		sub_atomic(&wq->count, 1);
		cpu_interrupt_set(old);
		/* handle async_call */
		async_call_execute(call);
		async_call_destroy(call);
		return 0;
	}
	mutex_release(&wq->lock);
	cpu_interrupt_set(old);
	return -1;
}

