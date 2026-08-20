/*
 * userstack.c: An extension for preserving userspace stack memory pages
 *
 * It can be useful to know what userspace tasks were doing at the time of a
 * crash, but including all userspace memory is usually too much: usually a
 * simple stack trace would do the trick. This extension preserves the topmost
 * userspace stack pages for each thread in each process, making it possible
 * to create a stack trace with a tool such as contrib/pstack.py in drgn.
 */
#include <assert.h>
#include <stdbool.h>

#include "../extension.h"
#include "../makedumpfile.h"
#include "../btf_info.h"
#include "../kallsyms.h"
#include "vma_mtree.h"
#include "vma_rbtree.h"
#include "list.h"

/* Required struct fields */
INIT_MOD_STRUCT_MEMBER(vmlinux, task_struct, tasks);
INIT_MOD_STRUCT_MEMBER(vmlinux, task_struct, signal);
INIT_MOD_STRUCT_MEMBER(vmlinux, task_struct, thread_node);
INIT_MOD_STRUCT_MEMBER(vmlinux, task_struct, stack);
INIT_MOD_STRUCT_MEMBER(vmlinux, task_struct, mm);
INIT_MOD_STRUCT_MEMBER(vmlinux, vm_area_struct, anon_vma);
INIT_MOD_STRUCT_MEMBER(vmlinux, vm_area_struct, vm_pgoff);
INIT_MOD_STRUCT_MEMBER(vmlinux, page, index);
INIT_MOD_STRUCT_MEMBER(vmlinux, signal_struct, thread_head);
INIT_MOD_STRUCT_MEMBER(vmlinux, list_head, next);
INIT_MOD_STRUCT_MEMBER(vmlinux, list_head, prev);
INIT_MOD_STRUCT_MEMBER(vmlinux, pt_regs, sp);
INIT_MOD_STRUCT(vmlinux, pt_regs);

/* Optional struct fields */
INIT_OPT_MOD_STRUCT_MEMBER(vmlinux, thread_union, stack);
INIT_OPT_MOD_STRUCT_MEMBER(vmlinux, mm_struct, mm_mt);
INIT_OPT_MOD_STRUCT_MEMBER(vmlinux, mm_struct, mm_rb);

/* Required symbols */
INIT_MOD_SYM(vmlinux, init_task);

/* Optional symbols */
INIT_OPT_MOD_SYM(vmlinux, fred_rsp0);
INIT_OPT_MOD_SYM(vmlinux, __start_init_stack);
INIT_OPT_MOD_SYM(vmlinux, __end_init_stack);
INIT_OPT_MOD_SYM(vmlinux, __start_init_task);
INIT_OPT_MOD_SYM(vmlinux, __end_init_task);

unsigned long THREAD_SIZE;
bool ready;

#define MEMBER_OFF(S, M) \
	(GET_MOD_STRUCT_MEMBER_MOFF(vmlinux, S, M) / 8)


static int for_each_task(bool (*task_fn)(unsigned long))
{
	unsigned long curr_proc;

	// NOTE: this explicitly skips "init_task" because it treats it as the
	// head of the list. This is fine: init_task is a kernel thread, so
	// never has a stack to retain.
	list_for_each_entry(curr_proc,
			    GET_MOD_SYM(vmlinux, init_task) + MEMBER_OFF(task_struct, tasks),
			    MEMBER_OFF(task_struct, tasks)) {
		unsigned long mm;
		if (!readmem(VADDR, curr_proc + MEMBER_OFF(task_struct, mm),
			     &mm, sizeof(mm))) {
			ERRMSG("error: failed to read task.mm\n");
		}
		if (!mm)
			continue;

		unsigned long signal;
		if (!readmem(VADDR, curr_proc + MEMBER_OFF(task_struct, signal),
			     &signal, sizeof(signal))) {
			ERRMSG("error: failed to read task.signal\n");
			break;
		}

		unsigned long curr_thread;
		list_for_each_entry(curr_thread,
				    signal + MEMBER_OFF(signal_struct, thread_head),
				    MEMBER_OFF(task_struct, thread_node)) {
			if (!task_fn(curr_thread))
				return FALSE;
		}
		if (LIST_ERR(curr_thread))
			return list_iterator_errmsg(curr_thread, "iterating thread list");
	}
	if (LIST_ERR(curr_proc))
		return list_iterator_errmsg(curr_proc, "iterating task list");

	return TRUE;
}

static unsigned long task_sp(unsigned long taskp)
{
	// The stack pointer is stored on entry to the kernel at the top of the
	// kernel stack. If the task in on-cpu, the stack pointer will be in the
	// PRSTATUS, but the stale value is very likely to be useful enough.
	unsigned long user_sp_loc;
	if (!readmem(VADDR, taskp + MEMBER_OFF(task_struct, stack),
		     &user_sp_loc, sizeof(user_sp_loc)))
		return 0;

	user_sp_loc += THREAD_SIZE;
	user_sp_loc -= GET_MOD_STRUCT_SSIZE(vmlinux, pt_regs);
	if (MOD_SYM_EXIST(vmlinux, fred_rsp0))
		user_sp_loc -= 16;
	user_sp_loc += MEMBER_OFF(pt_regs, sp);

	unsigned long sp;
	if (!readmem(VADDR, user_sp_loc, &sp, sizeof(sp)))
		return 0;
	return sp;
}

struct task_stack {
	unsigned long anon_vma;
	unsigned long index_start;
	unsigned long index_end;
};

static struct task_stack *stacks;
static size_t stacks_count;
static size_t stacks_alloc;

// Avoid using too much memory when processing an especially large vmcore, or in
// the case of a bug that causes us to create too many entries. 1M threads
// requires 24 MiB of memory to track. While it's not the maximum amount we
// could see, by a long shot, it's enough where most common workloads won't hit
// it, and any more than this will make it far more likely that the dump will
// hit an OOM issue.
#define MAX_TASK_STACKS (1LU << 20) /* 1M * 24 bytes = 24 MiB */

static bool append_task_stack(struct task_stack *newstack)
{
	if (stacks_count >= MAX_TASK_STACKS) {
		ERRMSG("userstack error: hit maximum stack count %lu, aborting collection\n", MAX_TASK_STACKS);
		goto fail;
	}
	if (stacks_count == stacks_alloc) {
		if (stacks_alloc)
			stacks_alloc *= 2;
		else
			stacks_alloc = 512;
		struct task_stack *newarr = realloc(stacks, stacks_alloc * sizeof(stacks[0]));
		if (!newarr) {
			ERRMSG("userstack: allocation error for stack tracking (size %lu)\n", stacks_alloc);
			goto fail;
		}
		stacks = newarr;
	}
	stacks[stacks_count++] = *newstack;
	return TRUE;

fail:
	free(stacks);
	stacks = NULL;
	stacks_count = stacks_alloc = 0;
	return FALSE;
}

static bool record_task_stack(unsigned long taskp)
{
	unsigned long task_mm;
	if (!readmem(VADDR, taskp + MEMBER_OFF(task_struct, mm), &task_mm, sizeof(task_mm)))
		/* Propagate failure to read a value from the task_struct */
		return FALSE;
	if (!task_mm)
		/* NULL task_mm is expected, continue */
		return TRUE;

	unsigned long sp = task_sp(taskp);
	if (!sp)
		/* Propagate error reading SP */
		return FALSE;

	int ret;
	unsigned long vma = 0;
	if (MOD_STRUCT_MEMBER_EXIST(vmlinux, mm_struct, mm_mt))
		ret = find_vma_mtree(task_mm + MEMBER_OFF(mm_struct, mm_mt), sp, &vma);
	else if (MOD_STRUCT_MEMBER_EXIST(vmlinux, mm_struct, mm_rb))
		ret = find_vma_rbtree(task_mm + MEMBER_OFF(mm_struct, mm_rb), sp, &vma);
	else
		assert(FALSE); /* should be impossible */
	if (!ret)
		/* propagate error from find_vma_xxx() */
		return FALSE;
	else if (!vma)
		/* No VMA found for the stack. This is unexpected, but gracefully
		 * handle the condition and continue. */
		return TRUE;

	unsigned long vm_start, vm_end, anon_vma, vm_pgoff;
	if (!readmem(VADDR, vma + MEMBER_OFF(vm_area_struct, anon_vma), &anon_vma, sizeof(anon_vma)))
		return FALSE;

	if (!anon_vma)
		/* Not an anonymous VMA. This is unexpected but valid. Move on to
		 * the next task. */
		return TRUE;

	if (!readmem(VADDR, vma + MEMBER_OFF(vm_area_struct, vm_start), &vm_start, sizeof(vm_start)) ||
	    !readmem(VADDR, vma + MEMBER_OFF(vm_area_struct, vm_end), &vm_end, sizeof(vm_end)) ||
	    !readmem(VADDR, vma + MEMBER_OFF(vm_area_struct, vm_pgoff), &vm_pgoff, sizeof(vm_pgoff)))
		return FALSE;

	/* Construct a range of indices we would like to retain. This is the
	 * range of stack pages starting with the stack pointer, and continuing
	 * to the top of the stack vma, or until a limit of 128 pages per task
	 * is reached. */
	unsigned long pgoff_start = (sp - vm_start) >> PAGESHIFT();
	pgoff_start += vm_pgoff;
	unsigned long pgoff_end = (vm_end - vm_start) >> PAGESHIFT();
	pgoff_end += vm_pgoff;
	if (pgoff_start + 128 < pgoff_end)
		pgoff_end = pgoff_start + 128;

	struct task_stack stack = {anon_vma | 1, pgoff_start, pgoff_end};
	if (!append_task_stack(&stack))
		return FALSE;

	return TRUE;
}

static int stack_compar(const void *lhs, const void *rhs)
{
	const struct task_stack *lhss = lhs, *rhss = rhs;
	if (lhss->anon_vma < rhss->anon_vma)
		return -1;
	else if (lhss->anon_vma > rhss->anon_vma)
		return 1;
	else
		return 0;
}

void extension_init(void)
{
	if (MOD_STRUCT_MEMBER_EXIST(vmlinux, mm_struct, mm_mt)) {
		if (!vma_mtree_init())
			return;
	} else if (MOD_STRUCT_MEMBER_EXIST(vmlinux, mm_struct, mm_rb)) {
		if (!vma_rbtree_init())
			return;
	} else {
		ERRMSG("error: Neither mtree nor rbtree available for VMA walking\n");
		return;
	}
	if (!MOD_STRUCT_EXIST(vmlinux, pt_regs)) {
		ERRMSG("error: missing pt_regs incfo\n");
		return;
	}
	if (!MOD_SYM_EXIST(vmlinux, init_task)) {
		ERRMSG("error: missing init_task symbol\n");
		return;
	}

	// Determine THREAD_SIZE, which is necessary to find the offset of the
	// userspace stack pointer register from the kernel thread stack.
	//
	// - Prior to v4.16, 0500871f21b23 ("Construct init thread stack in the
	//   linker script rather than by union"), it was found in thread_union.
	// - Between v4.16 and v6.10, 8f69cba096b5c ("x86: Rename
	//   __{start,end}_init_task to __{start,end}_init_stack"), the stack
	//   size can be inferred by the __{start,end}_init_task symbols.
	// - Since v6.10, the size is inferred by __{start,end}_init_stack.
	if (MOD_STRUCT_MEMBER_EXIST(vmlinux, thread_union, stack)) {
		THREAD_SIZE = GET_MOD_STRUCT_MEMBER_MSIZE(vmlinux, thread_union, stack);
	} else if (MOD_SYM_EXIST(vmlinux, __start_init_stack) &&
		   MOD_SYM_EXIST(vmlinux, __end_init_stack) &&
		   GET_MOD_SYM(vmlinux, __end_init_stack) > GET_MOD_SYM(vmlinux, __start_init_stack)) {
		THREAD_SIZE = GET_MOD_SYM(vmlinux, __end_init_stack) - GET_MOD_SYM(vmlinux, __start_init_stack);
	} else if (MOD_SYM_EXIST(vmlinux, __start_init_task) &&
		   MOD_SYM_EXIST(vmlinux, __end_init_task) &&
		   GET_MOD_SYM(vmlinux, __end_init_task) > GET_MOD_SYM(vmlinux, __start_init_task)) {
		THREAD_SIZE = GET_MOD_SYM(vmlinux, __end_init_task) - GET_MOD_SYM(vmlinux, __start_init_task);
	} else {
		ERRMSG("Could not determine THREAD_SIZE: neither __start_init_stack "
		       "nor __start_init_task found in kallsyms, nor is thread_union "
		       "found in BTF.\n");
		return;
	}

	if (!for_each_task(&record_task_stack)) {
		free(stacks);
		stacks = NULL;
		stacks_alloc = stacks_count = 0;
		ERRMSG("Could not gather all task stack VMAs, userstack disabled\n");
		return;
	}
	struct task_stack *tmp = realloc(stacks, stacks_count * sizeof(*tmp));
	if (tmp) {
		stacks = tmp;
		stacks_alloc = stacks_count;
	}
	qsort(stacks, stacks_count, sizeof(*stacks), &stack_compar);
	ready = TRUE;
}

static int count_retained;
static int count_checked;
static int count_cached;
int extension_callback(unsigned long pfn, const void *pcache, const struct pginfo *i)
{
	unsigned long index;
	static struct {
		unsigned long mapping;
		struct task_stack *result;
	} cache;

	if (!ready || !isAnon(i))
		return PG_UNDECID;

	index = ULONG(pcache + MEMBER_OFF(page, index));
	if (!(i->mapping & 1))
		return PG_UNDECID;

	if (i->mapping != cache.mapping) {
		count_checked++;
		struct task_stack search = {i->mapping, 0, 0};
		struct task_stack *result = bsearch(&search, stacks, stacks_count,
						sizeof(search), &stack_compar);
		if (!result)
			return PG_UNDECID;

		cache.mapping = i->mapping;
		cache.result = result;
	} else {
		count_cached++;
	}

	if (cache.result->index_start <= index && index < cache.result->index_end) {
		count_retained++;
		return PG_INCLUDE;
	} else {
		return PG_UNDECID;
	}
}

__attribute__((destructor))
static void userstack_exit(void) {
	if (count_retained || count_checked || count_cached || stacks_count) {
		REPORT_MSG("Extension userstack:\n");
		REPORT_MSG("  PFNs retained: %d searched: %d, cached: %d\n", count_retained, count_checked, count_cached);
		REPORT_MSG("  Recorded %zu stack anon_vmas\n", stacks_count);
	}
}
