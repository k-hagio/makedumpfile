#ifndef MAKEDUMPFILE_EXT_LIST_H
#define MAKEDUMPFILE_EXT_LIST_H

#include "../makedumpfile.h"
#include "../btf_info.h"
#include "../detect_cycle.h"

#define LIST_READERR  0UL
#define LIST_CYCLE    1UL
#define LIST_INVAL    2UL
#define LIST_ALLOCERR 3UL
#define LIST_END      4UL

#define LIST_ERR(pos) ((pos) <= LIST_ALLOCERR)
#define LIST_STOP(pos) ((pos) <= LIST_END)

DECLARE_MOD_STRUCT_MEMBER(vmlinux, list_head, next);
DECLARE_MOD_STRUCT_MEMBER(vmlinux, list_head, prev);

static void *list_next(void *prev, void *data)
{
	unsigned long head = (unsigned long)data;
	unsigned long next;
	unsigned long nextprev;

	if (!readmem(VADDR, (unsigned long)prev, &next, sizeof(next)))
		return (void *)LIST_READERR;
	if (!readmem(VADDR, next + (GET_MOD_STRUCT_MEMBER_MOFF(vmlinux, list_head, prev) / 8),
		     &nextprev, sizeof(nextprev)))
		return (void *)LIST_READERR;
	if ((unsigned long)prev != nextprev)
		return (void *)LIST_INVAL;
	if (next == head)
		return (void *)LIST_END;
	return (void *)next;
}

static inline struct detect_cycle *list_iterator_start(unsigned long head)
{
	return dc_init((void *)head, (void *)head, list_next);
}

static inline unsigned long list_iterator_next(struct detect_cycle *dc, unsigned long offset)
{
	void *nextp;
	int is_cycle;
	if (!dc)
		return LIST_ALLOCERR;
	is_cycle = dc_next(dc, &nextp);
	if (is_cycle) {
		free(dc);
		return LIST_CYCLE;
	}
	unsigned long next = (unsigned long) nextp;
	if (!LIST_STOP(next))
		next -= offset;
	else
		free(dc);
	return next;
}

static inline int list_iterator_errmsg(unsigned long pos, const char *prefix) {
	switch (pos) {
	case LIST_READERR:
		ERRMSG("%s: error reading next pointer\n", prefix);
		break;
	case LIST_CYCLE:
		ERRMSG("%s: detected cycle\n", prefix);
		break;
	case LIST_INVAL:
		ERRMSG("%s: corrupt list (invalid prev pointer)\n", prefix);
		break;
	case LIST_ALLOCERR:
		ERRMSG("%s: allocation error\n", prefix);
		break;
	default:
		ERRMSG("%s: BUG: list_iterator_errmsg() with no error\n", prefix);
		break;
	}
	return FALSE;
}

/*
 * Iterate over each object in a linked list.
 *
 * pos: name of an unsigned long variable which is set to the address of
 *    each object
 * head: address of the list_head anchoring the list
 * offset: offset of the list_head within each object
 *
 * Unlike the standard kernel list_for_each_entry() implementation, we perform
 * list validation. That is, we ensure that (a) each prev pointer points back to
 * the correct head, and (b) there are no cycles. The loop terminates for errors
 * or success, so the "pos" variable does double-duty as a status variable. At
 * the end of the loop, users must use LIST_ERR() to check whether an iteration
 * error occurred, and if so, they are encouraged to use list_iterator_errmsg to
 * report an error and return FALSE:
 *
 *    if (LIST_ERR(pos))
 *        return list_iterator_errmsg(pos, "descriptive prefix");
 */
#define list_for_each_entry(pos, head, offset) \
	for (struct detect_cycle *__dc = list_iterator_start(head); \
	     !LIST_STOP(pos = list_iterator_next(__dc, offset));)

#endif // MAKEDUMPFILE_EXT_LIST_H
