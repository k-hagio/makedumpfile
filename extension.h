#ifndef _EXTENSION_H
#define _EXTENSION_H
#include <stdbool.h>

struct pginfo;
enum {
	PG_INCLUDE,		/* Extension will keep the full page */
	PG_INCLUDE_HEAD,	/* Extension will keep just the head page */
	PG_EXCLUDE,		/* Extension will discard the full page */
	PG_UNDECID,		/* Extension makes no decision */
};
int run_extension_callback(unsigned long pfn, const void *pcache, const struct pginfo *i);
void init_extensions(void);
void cleanup_extensions(void);
bool add_extension_opts(char *opt);
bool extension_has_callback(void);
#endif /* _EXTENSION_H */

