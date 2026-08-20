#ifndef _MAPLE_TREE_H
#define _MAPLE_TREE_H
#include <stdbool.h>
bool vma_mtree_init(void);
int find_vma_mtree(unsigned long mt, unsigned long address, unsigned long *ret);
#endif /* _MAPLE_TREE_H */

