#ifndef ECO_LIST_H
#define ECO_LIST_H

#include <stddef.h>

struct list_head {
	struct list_head *next;
	struct list_head *prev;
};

static inline void INIT_LIST_HEAD(struct list_head *head)
{
	head->next = head;
	head->prev = head;
}

static inline int list_empty(const struct list_head *head)
{
	return head->next == head;
}

static inline void list_add_tail(struct list_head *entry, struct list_head *head)
{
	entry->prev = head->prev;
	entry->next = head;
	head->prev->next = entry;
	head->prev = entry;
}

static inline void list_del(struct list_head *entry)
{
	entry->prev->next = entry->next;
	entry->next->prev = entry->prev;
	INIT_LIST_HEAD(entry);
}

#define list_entry(ptr, type, member) \
	((type *) ((char *) (ptr) - offsetof(type, member)))

#define list_for_each(pos, head) \
	for ((pos) = (head)->next; (pos) != (head); (pos) = (pos)->next)

/* Safe when the current entry is removed during traversal. */
#define list_for_each_safe(pos, next_pos, head) \
	for ((pos) = (head)->next, (next_pos) = (pos)->next; \
	     (pos) != (head); \
	     (pos) = (next_pos), (next_pos) = (pos)->next)

/* Single-pointer heads; pprev permits O(1) removal without a tail pointer.
 * A zero-initialized head is empty. Nodes must be linked before deletion. */
struct hlist_head {
	struct hlist_node *first;
};

struct hlist_node {
	struct hlist_node *next;
	struct hlist_node **pprev;
};

static inline int hlist_empty(const struct hlist_head *head)
{
	return head->first == NULL;
}

static inline void hlist_add_head(struct hlist_node *entry, struct hlist_head *head)
{
	entry->pprev = &head->first;
	entry->next = head->first;
	if (entry->next != NULL)
		entry->next->pprev = &entry->next;

	head->first = entry;
}

static inline void hlist_del(struct hlist_node *entry)
{
	*entry->pprev = entry->next;
	if (entry->next != NULL)
		entry->next->pprev = entry->pprev;

	entry->next = NULL;
	entry->pprev = NULL;
}

#define hlist_entry(ptr, type, member) \
	list_entry(ptr, type, member)

#define hlist_for_each(pos, head) \
	for ((pos) = (head)->first; (pos) != NULL; (pos) = (pos)->next)

/* Safe when the current entry is removed or freed during traversal. */
#define hlist_for_each_safe(pos, next_pos, head) \
	for ((pos) = (head)->first; \
	     (pos) != NULL && ((next_pos) = (pos)->next, 1); \
	     (pos) = (next_pos))

#endif
