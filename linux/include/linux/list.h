/**
 * @file linux/include/linux/list.h
 * @brief Doubly linked list implementation.
 *
 * This file provides a compact, efficient doubly linked list
 * implementation used throughout the kernel. The core type is
 * `struct list_head` and the header exposes a small set of
 * macros and inline helpers to initialize, add, delete and iterate
 * list entries.
 *
 * @section api-index API Index
 * - `struct list_head` : core list node containing `next` and `prev`.
 * - `LIST_HEAD_INIT(name)` : static initializer for a `struct list_head`.
 * - `LIST_HEAD(name)` : declare and initialize a list head variable.
 * - `INIT_LIST_HEAD(ptr)` : initialize a `struct list_head` at runtime.
 * - `__list_add(new, prev, next)` : internal helper to insert `new` between `prev` and `next`.
 * - `list_add(new, head)` : add `new` after `head` (stack-like push).
 * - `list_add_tail(new, head)` : add `new` before `head` (queue append).
 * - `__list_del(prev, next)` : internal helper to unlink entries by linking `prev` and `next`.
 * - `list_del(entry)` : remove `entry` from its list (leaves entry in undefined state).
 * - `list_del_init(entry)` : remove `entry` and reinitialize it as an empty list head.
 * - `list_empty(head)` : test whether the list is empty.
 * - `list_splice(list, head)` : splice list `list` into `head`.
 * - `list_entry(ptr, type, member)` : obtain containing structure from list head pointer.
 * - `list_for_each(pos, head)` : iterate forwards over list heads.
 * - `list_for_each_safe(pos, n, head)` : iterate forwards allowing removal of `pos` during iteration.
 * - `list_for_each_prev(pos, head)` : iterate backwards over list heads.
 *
 * @section usage Usage notes
 * - List heads are typically embedded in container structures; use `list_entry` to recover the container.
 * - Use `list_for_each_safe` when deleting entries while iterating.
 * - `INIT_LIST_HEAD` sets `next` and `prev` to point to the head itself, representing an empty list.
 */
#ifndef _LINUX_LIST_H
#define _LINUX_LIST_H

#if defined(__KERNEL__) || defined(_LVM_H_INCLUDE)

#include <linux/prefetch.h>

/**
 * @brief Simple doubly linked list implementation.
 *
 * Some of the internal functions ("__xxx") are useful when
 * manipulating whole lists rather than single entries, as
 * sometimes we already know the next/prev entries and we can
 * generate better code by using them directly rather than
 * using the generic single-entry routines.
 */

struct list_head {
	struct list_head *next, *prev;
};

#define LIST_HEAD_INIT(name) { &(name), &(name) }

#define LIST_HEAD(name) \
	struct list_head name = LIST_HEAD_INIT(name)

#define INIT_LIST_HEAD(ptr) do { \
	(ptr)->next = (ptr); (ptr)->prev = (ptr); \
} while (0)

/*
 * Insert a new entry between two known consecutive entries. 
 *
 * This is only for internal list manipulation where we know
 * the prev/next entries already!
 */
static __inline__ void __list_add(struct list_head * new,
	struct list_head * prev,
	struct list_head * next)
{
	next->prev = new;
	new->next = next;
	new->prev = prev;
	prev->next = new;
}

/**
 * list_add - add a new entry
 * @new: new entry to be added
 * @head: list head to add it after
 *
 * Insert a new entry after the specified head.
 * This is good for implementing stacks.
 */
static __inline__ void list_add(struct list_head *new, struct list_head *head)
{
	__list_add(new, head, head->next);
}

/**
 * list_add_tail - add a new entry
 * @new: new entry to be added
 * @head: list head to add it before
 *
 * Insert a new entry before the specified head.
 * This is useful for implementing queues.
 */
static __inline__ void list_add_tail(struct list_head *new, struct list_head *head)
{
	__list_add(new, head->prev, head);
}

/*
 * Delete a list entry by making the prev/next entries
 * point to each other.
 *
 * This is only for internal list manipulation where we know
 * the prev/next entries already!
 */
static __inline__ void __list_del(struct list_head * prev,
				  struct list_head * next)
{
	next->prev = prev;
	prev->next = next;
}

/**
 * list_del - deletes entry from list.
 * @entry: the element to delete from the list.
 * Note: list_empty on entry does not return true after this, the entry is in an undefined state.
 */
static __inline__ void list_del(struct list_head *entry)
{
	__list_del(entry->prev, entry->next);
}

/**
 * list_del_init - deletes entry from list and reinitialize it.
 * @entry: the element to delete from the list.
 */
static __inline__ void list_del_init(struct list_head *entry)
{
	__list_del(entry->prev, entry->next);
	INIT_LIST_HEAD(entry); 
}

/**
 * list_empty - tests whether a list is empty
 * @head: the list to test.
 */
static __inline__ int list_empty(struct list_head *head)
{
	return head->next == head;
}

/**
 * list_splice - join two lists
 * @list: the new list to add.
 * @head: the place to add it in the first list.
 */
static __inline__ void list_splice(struct list_head *list, struct list_head *head)
{
	struct list_head *first = list->next;

	if (first != list) {
		struct list_head *last = list->prev;
		struct list_head *at = head->next;

		first->prev = head;
		head->next = first;

		last->next = at;
		at->prev = last;
	}
}

/**
 * list_entry - get the struct for this entry
 * @ptr:	the &struct list_head pointer.
 * @type:	the type of the struct this is embedded in.
 * @member:	the name of the list_struct within the struct.
 */
#define list_entry(ptr, type, member) \
	((type *)((char *)(ptr)-(unsigned long)(&((type *)0)->member)))

/**
 * list_for_each	-	iterate over a list
 * @pos:	the &struct list_head to use as a loop counter.
 * @head:	the head for your list.
 */
#define list_for_each(pos, head) \
	for (pos = (head)->next, prefetch(pos->next); pos != (head); \
        	pos = pos->next, prefetch(pos->next))
        	
/**
 * list_for_each_safe	-	iterate over a list safe against removal of list entry
 * @pos:	the &struct list_head to use as a loop counter.
 * @n:		another &struct list_head to use as temporary storage
 * @head:	the head for your list.
 */
#define list_for_each_safe(pos, n, head) \
	for (pos = (head)->next, n = pos->next; pos != (head); \
		pos = n, n = pos->next)

/**
 * list_for_each_prev	-	iterate over a list in reverse order
 * @pos:	the &struct list_head to use as a loop counter.
 * @head:	the head for your list.
 */
#define list_for_each_prev(pos, head) \
	for (pos = (head)->prev, prefetch(pos->prev); pos != (head); \
        	pos = pos->prev, prefetch(pos->prev))
        	

#endif /* __KERNEL__ || _LVM_H_INCLUDE */

#endif
