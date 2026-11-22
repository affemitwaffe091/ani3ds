#ifndef NET_H
#define NET_H
#include <stddef.h>

/* one-time setup; ca_bundle may be NULL on Linux (system store) */
int net_init(const char *ca_bundle);
void net_exit(void);

/* GET url completely; referer may be NULL. returns malloc'd NUL-terminated body
 * (also on HTTP errors, check *status), NULL on transport failure (err filled). */
char *net_get(const char *url, const char *referer, long *status, size_t *len, char *err, size_t errlen);

/* log sink, set by the app */
extern void (*net_log)(const char *fmt, ...);
#endif
