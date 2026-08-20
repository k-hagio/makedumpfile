/*
 * elfheader.c: Preserve resident ELF header pages from page cache.
 *
 * In order to accurately unwind userspace stacks, user debuginfo may be
 * necessary. At a minimum, it is needed to translate addresses to symbols or
 * function names, but it may also be needed for unwinding when frame pointers
 * are not in use. Normally, userspace core dumps will retain the first page of
 * mapped executable files, since they usually contain the build ID necessary
 * for identifying the executable and debuginfo. This extension takes
 * inspiration from this approach, with modifications suitable for the kdump
 * environment:
 * - Include non-anonymous page cache pages corresponding to executable files
 * - Only include the page at index 0
 * - Only include pages which start with the ELF magic header
 *
 * None of this guarantees that all ELF headers will be included. They may not
 * be resident in memory. And even if the first page is included, there's no
 * guarantee that the build ID resides in the first page. However, the best
 * effort is already good enough to make many userspace stacks intelligible with
 * minimal extra dump time or space.
 */
#include <limits.h>
#include <stdbool.h>
#include <string.h>

#include "../extension.h"
#include "../makedumpfile.h"
#include "../btf_info.h"

INIT_MOD_STRUCT_MEMBER(vmlinux, page, index);
INIT_MOD_STRUCT_MEMBER(vmlinux, address_space, host);
INIT_MOD_STRUCT_MEMBER(vmlinux, inode, i_mode);

#define MEMBER_OFF(S, M) \
	(GET_MOD_STRUCT_MEMBER_MOFF(vmlinux, S, M) / 8)

static int retained;
static int checked;

void extension_init(void)
{
}

int extension_callback(unsigned long pfn, const void *pcache, const struct pginfo *i)
{
	unsigned long index;
	unsigned long inode;
	unsigned short mode;
	char elfmag[SELFMAG];

	/* Only consider non-anonymous file cache pages */
	if (!(is_cache_page(i->flags) && !isAnon(i)))
		return PG_UNDECID;

	/* Only consider the first page in the file */
	index = ULONG(pcache + MEMBER_OFF(page, index));
	if (index != 0)
		return PG_UNDECID;

	/* Only retain pages which are actually mapped to userspace */
	if (OFFSET(page._mapcount) != NOT_FOUND_STRUCTURE &&
	    i->_mapcount == UINT_MAX)
		return PG_UNDECID;

	/* Only retain pages for executable inodes */
	checked++;
	if (!readmem(VADDR, i->mapping + MEMBER_OFF(address_space, host),
		     &inode, sizeof(inode)) || !inode)
		return PG_UNDECID;
	if (!readmem(VADDR, inode + MEMBER_OFF(inode, i_mode),
		     &mode, sizeof(mode)))
		return PG_UNDECID;
	if (!(mode & 0111))
		return PG_UNDECID;

	/* Only retain the page if its content looks like ELF */
	if (!readmem(PADDR, pfn_to_paddr(pfn), elfmag, sizeof(elfmag)) ||
	    memcmp(elfmag, ELFMAG, SELFMAG))
		return PG_UNDECID;

	retained++;
	return PG_INCLUDE_HEAD;
}

__attribute__((destructor))
static void elfheader_exit(void)
{
	if (checked || retained) {
		REPORT_MSG("Extension elfheader:\n");
		REPORT_MSG("  ELF headers retained: %d (candidates checked: %d)\n",
			   retained, checked);
	}
}
