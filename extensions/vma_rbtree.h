#ifndef RBTREE_H_
#define RBTREE_H_
#include <stdbool.h>

#include "../btf_info.h"

DECLARE_MOD_STRUCT_MEMBER(vmlinux, vm_area_struct, vm_start);
DECLARE_MOD_STRUCT_MEMBER(vmlinux, vm_area_struct, vm_end);

int find_vma_rbtree(unsigned long rb_root, unsigned long address, unsigned long *ret);
bool vma_rbtree_init(void);
#endif // RBTREE_H_
