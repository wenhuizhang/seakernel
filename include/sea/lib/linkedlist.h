#ifndef __SEA_LIB_LINKEDLIST_H
#define __SEA_LIB_LINKEDLIST_H

#define LINKEDLIST_ALLOC 1
#define LINKEDLIST_LOCKLESS 1

struct linkedentry {
	void *obj;
	struct linkedentry *next, *prev;
};

#include <sea/spinlock.h>
#include <stdbool.h>
struct linkedlist {
	struct linkedentry *head;
	struct linkedentry sentry;
	struct spinlock lock;
	size_t count;
	int flags;
};

struct linkedlist *linkedlist_create(struct linkedlist *list, int flags);
void linkedlist_destroy(struct linkedlist *list);
void linkedlist_insert(struct linkedlist *list, struct linkedentry *entry, void *obj);
void linkedlist_remove(struct linkedlist *list, struct linkedentry *entry);
void linkedlist_apply(struct linkedlist *list, void (*fn)(struct linkedentry *));
void linkedlist_apply_data(struct linkedlist *list, void (*fn)(struct linkedentry *, void *data), void *);
struct linkedentry *linkedlist_find(struct linkedlist *list, bool (*fn)(struct linkedentry *, void *data), void *data);
unsigned long linkedlist_reduce(struct linkedlist *list, unsigned long (*fn)(struct linkedentry *, unsigned long), unsigned long init);
void linkedlist_apply_head(struct linkedlist *list, void (*fn)(struct linkedentry *));

#endif

