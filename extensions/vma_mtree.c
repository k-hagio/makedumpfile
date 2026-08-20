#include <stdbool.h>
#include "../btf_info.h"
#include "../kallsyms.h"
#include "../makedumpfile.h"

INIT_OPT_MOD_STRUCT(vmlinux, maple_tree);
INIT_OPT_MOD_STRUCT(vmlinux, maple_node);
INIT_OPT_MOD_STRUCT_MEMBER(vmlinux, maple_tree, ma_root);
INIT_OPT_MOD_STRUCT_MEMBER(vmlinux, maple_arange_64, pivot);
INIT_OPT_MOD_STRUCT_MEMBER(vmlinux, maple_arange_64, slot);
INIT_OPT_MOD_STRUCT_MEMBER(vmlinux, maple_arange_64, meta);
INIT_OPT_MOD_STRUCT_MEMBER(vmlinux, maple_range_64, pivot);
INIT_OPT_MOD_STRUCT_MEMBER(vmlinux, maple_range_64, slot);
INIT_OPT_MOD_STRUCT_MEMBER(vmlinux, maple_range_64, meta);
INIT_OPT_MOD_STRUCT_MEMBER(vmlinux, maple_metadata, end);

#define MEMBER_OFF(S, M) \
	GET_MOD_STRUCT_MEMBER_MOFF(vmlinux, S, M) / 8
#define KERN_STRUCT_MEMBER_EXIST(S, M) \
	MOD_STRUCT_MEMBER_EXIST(vmlinux, S, M)
#define GET_KERN_SYM(SYM) GET_MOD_SYM(vmlinux, SYM)
#define KERN_SYM_EXIST(SYM) MOD_SYM_EXIST(vmlinux, SYM)
#define GET_KERN_STRUCT_SSIZE(S) \
	GET_MOD_STRUCT_SSIZE(vmlinux, S)
#define KERN_STRUCT_EXIST(SYM) MOD_STRUCT_EXIST(vmlinux, SYM)

#define MAPLE_NODE_MASK			255UL
#define MAPLE_NODE_TYPE_MASK		0x0F
#define MAPLE_NODE_TYPE_SHIFT		0x03
#define XA_ZERO_ENTRY			xa_mk_internal(257)

static unsigned long xa_mk_internal(unsigned long v)
{
	return (v << 2) | 2;
}

static bool xa_is_internal(unsigned long entry)
{
	return (entry & 3) == 2;
}

static bool xa_is_node(unsigned long entry)
{
	return xa_is_internal(entry) && entry > 4096;
}

bool vma_mtree_init(void)
{
	if (!KERN_STRUCT_EXIST(maple_tree) ||
	    !KERN_STRUCT_EXIST(maple_node) ||
	    !KERN_STRUCT_MEMBER_EXIST(maple_tree, ma_root) ||
	    !KERN_STRUCT_MEMBER_EXIST(maple_arange_64, pivot) ||
	    !KERN_STRUCT_MEMBER_EXIST(maple_arange_64, slot) ||
	    !KERN_STRUCT_MEMBER_EXIST(maple_arange_64, meta) ||
	    !KERN_STRUCT_MEMBER_EXIST(maple_range_64, pivot) ||
	    !KERN_STRUCT_MEMBER_EXIST(maple_range_64, slot) ||
	    !KERN_STRUCT_MEMBER_EXIST(maple_range_64, meta) ||
	    !KERN_STRUCT_MEMBER_EXIST(maple_metadata, end)) {
		ERRMSG("Missing required maple tree syms/types\n");
		return false;
	}

	return true;
}

int find_vma_mtree(unsigned long mt, unsigned long index, unsigned long *ret)
{
	unsigned long long entry;

	if (!readmem(VADDR, mt + MEMBER_OFF(maple_tree, ma_root), &entry, sizeof(entry)))
		return FALSE;

	if (!xa_is_node(entry)) {
		if (index == 0)
			*ret = entry;
		else
			*ret = 0;
		return TRUE;
	}
	unsigned long long max = ULONGLONG_MAX;
	void *node = malloc(GET_KERN_STRUCT_SSIZE(maple_node));
	if (!node) {
		ERRMSG("failed to allocate memory\n");
		return FALSE;
	}

	for (;;) {
		if (!readmem(VADDR, entry & ~MAPLE_NODE_MASK, node, GET_KERN_STRUCT_SSIZE(maple_node))) {
			ERRMSG("failed to read maple node: %llx\n", entry & ~MAPLE_NODE_MASK);
			free(node);
			return FALSE;
		}

		int node_type = (entry >> MAPLE_NODE_TYPE_SHIFT) & MAPLE_NODE_TYPE_MASK;
		unsigned long long *pivot, *slot;
		uint8_t end;
		if (node_type == 3) {
			pivot = node + MEMBER_OFF(maple_arange_64, pivot);
			slot = node + MEMBER_OFF(maple_arange_64, slot);
			end = ((uint8_t *)node)[MEMBER_OFF(maple_arange_64, meta) + MEMBER_OFF(maple_metadata, end)];
		} else if (node_type == 1 || node_type == 2) {
			pivot = node + MEMBER_OFF(maple_range_64, pivot);
			slot = node + MEMBER_OFF(maple_range_64, slot);
			unsigned long long p = *(slot - 1);
			if (!p)
				end = ((uint8_t *)node)[MEMBER_OFF(maple_range_64, meta) + MEMBER_OFF(maple_metadata, end)];
			else {
				end = slot - pivot;
				if (p == max)
					end--;
			}
		} else {
			ERRMSG("unrecognized maple node type: %d\n", node_type);
			free(node);
			return FALSE;
		}
		int offset = 0;
		for (offset = 0; offset < end; offset++) {
			if (pivot[offset] >= index) {
				max = pivot[offset];
				break;
			}
		}
		if (&pivot[offset] >= slot)
			offset = end;

		entry = slot[offset];
		if (node_type == 1) {
			// leaf:
			free(node);
			if (entry == XA_ZERO_ENTRY)
				*ret = 0;
			else
				*ret = entry;
			return TRUE;
		}
	}
	*ret = 0;
	return TRUE;
}
