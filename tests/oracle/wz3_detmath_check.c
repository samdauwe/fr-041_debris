/* Checks the deterministic math in wz3.h against V8 results on stdin
 * (tools/dump_math_sweep.mjs):  node tools/dump_math_sweep.mjs sin 200000 | test_wz3_detmath
 * Lines: "fn x result" or "pow x y result", numbers as binary64 hex. NaN results compare as
 * "both NaN" because V8 and C disagree on NaN payloads.
 */
#define WZ3_IMPLEMENTATION
#include <debris/core/wz3.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static double from_bits(unsigned long long u)
{
  double d;
  memcpy(&d, &u, sizeof d);
  return d;
}

int main(void)
{
  char line[160];
  long cases = 0, bad = 0;
  while (fgets(line, sizeof line, stdin)) {
    char fn[8];
    unsigned long long a, b, c, d;
    double x, y = 0, want, got;
    if (!strncmp(line, "pow ", 4)) {
      if (sscanf(line, "%7s %llx %llx %llx", fn, &a, &b, &c) != 4) continue;
      x = from_bits(a);
      y = from_bits(b);
      want = from_bits(c);
      got = wz3__pow(x, y);
    } else {
      if (sscanf(line, "%7s %llx %llx", fn, &a, &c) != 3) continue;
      (void)d;
      x = from_bits(a);
      want = from_bits(c);
      if (!strcmp(fn, "sin"))
        got = wz3__sin(x);
      else if (!strcmp(fn, "cos"))
        got = wz3__cos(x);
      else if (!strcmp(fn, "atan"))
        got = wz3__atan(x);
      else if (!strcmp(fn, "log2"))
        got = wz3__log2(x);
      else if (!strcmp(fn, "log"))
        got = wz3__log(x);
      else if (!strcmp(fn, "acos"))
        got = wz3__acos(x);
      else if (!strcmp(fn, "exp2"))
        got = wz3__pow(2.0, x);
      else
        continue;
    }
    cases++;
    const int same = (want != want && got != got) || memcmp(&want, &got, sizeof want) == 0;
    if (!same) {
      if (bad < 5) printf("%s(%.17g, %.17g): C %.17g, V8 %.17g\n", fn, x, y, got, want);
      bad++;
    }
  }
  printf("detmath: %ld cases, %ld differ\n", cases, bad);
  return cases > 0 && bad == 0 ? 0 : 1;
}
