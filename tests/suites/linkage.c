/* The single-header layout: the implementation is compiled into one translation unit (this one) and every other translation unit
 * includes the header without it, sees the whole public API and links against that implementation (linkage_b.c). The other
 * suites define *_STATIC to get a private copy of the libraries; this one needs the shared one. */

#undef V2_STATIC
#undef WZ3_STATIC

#define V2_IMPLEMENTATION
#include <debris/core/v2.h>
#define WZ3_IMPLEMENTATION
#include <debris/core/wz3.h>

#include <stdio.h>

#include "support/suite.h"

int linkage_v2_check(void);  /* linkage_b.c */
int linkage_wz3_check(void); /* linkage_b.c */

int suite_linkage(int argc, char **argv)
{
  (void)argc;
  (void)argv;
  int failures = 0;
  if (!linkage_v2_check()) {
    printf("v2.h: the second translation unit does not link or misbehaves\n");
    failures++;
  }
  const int wz3 = linkage_wz3_check();
  if (wz3) {
    printf("wz3.h: the second translation unit fails at step %d\n", wz3);
    failures++;
  }
  return failures ? 1 : 0;
}
