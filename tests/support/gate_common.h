/* Shared pieces of the differential suites of tests/suites (the fixture lines of tools/dump_*.mjs): the check
 * macro, file loading, the FNV-1a word hash of tools/lib/frame_jobs.mjs and a token reader over one fixture line.
 * Include after the library header. */
#ifndef WZ3_GATE_COMMON_H
#define WZ3_GATE_COMMON_H

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_checks, g_failures;
#define CHECK(cond, ...)                            \
  do {                                              \
    g_checks++;                                     \
    if (!(cond)) {                                  \
      g_failures++;                                 \
      if (g_failures <= 30) {                       \
        printf("FAIL %s:%d: ", __FILE__, __LINE__); \
        printf(__VA_ARGS__);                        \
        printf("\n");                               \
      }                                             \
    }                                               \
  } while (0)

static inline uint8_t *gate_read_all(const char *dir, const char *name, size_t *size)
{
  char path[1024];
  snprintf(path, sizeof path, "%s/%s", dir, name);
  FILE *f = fopen(path, "rb");
  if (!f) {
    printf("cannot open %s\n", path);
    exit(2);
  }
  fseek(f, 0, SEEK_END);
  const long n = ftell(f);
  fseek(f, 0, SEEK_SET);
  uint8_t *data = (uint8_t *)malloc((size_t)n + 1);
  if (!data || fread(data, 1, (size_t)n, f) != (size_t)n) exit(2);
  fclose(f);
  data[n] = 0;
  if (size) *size = (size_t)n;
  return data;
}

typedef struct hash {
  uint32_t h;
} hash;
static inline void hash_init(hash *h) { h->h = 0x811c9dc5u; }
static inline void hu(hash *h, uint32_t v) { h->h = (h->h ^ v) * 0x01000193u; }
static inline void hi(hash *h, int32_t v) { hu(h, (uint32_t)v); }
static inline void hf(hash *h, float f)
{
  uint32_t u;
  memcpy(&u, &f, 4);
  if (f != f) u = 0x7fc00000u;
  hu(h, u);
}
static inline void hd(hash *h, double d)
{
  uint64_t u;
  memcpy(&u, &d, 8);
  if (d != d) u = 0x7ff8000000000000ull;
  hu(h, (uint32_t)u);
  hu(h, (uint32_t)(u >> 32));
}
static inline void hfs(hash *h, const float *v, size_t n)
{
  for (size_t i = 0; i < n; i++) hf(h, v[i]);
}

/* tokens of one fixture line: hex words; a number is `i<hex>` (non-negative integer) or 16 hex digits of a binary64 */
typedef struct reader {
  const char *p;
} reader;
static inline uint64_t rtok(reader *r)
{
  while (*r->p == ' ') r->p++;
  char *end;
  const uint64_t v = strtoull(r->p, &end, 16);
  r->p = end;
  return v;
}
static inline uint32_t ru32(reader *r) { return (uint32_t)rtok(r); }
static inline int32_t ri32(reader *r) { return (int32_t)ru32(r); }
static inline float rf32(reader *r)
{
  const uint32_t u = ru32(r);
  float f;
  memcpy(&f, &u, 4);
  return f;
}
static inline double rf64(reader *r)
{
  const uint64_t u = rtok(r);
  double d;
  memcpy(&d, &u, 8);
  return d;
}
static inline double rnum(reader *r)
{
  while (*r->p == ' ') r->p++;
  if (*r->p == 'i') {
    r->p++;
    return (double)rtok(r);
  }
  return rf64(r);
}
static inline void rmat(reader *r, float *m)
{
  for (int i = 0; i < 16; i++) m[i] = rf32(r);
}
static inline void rnums(reader *r, double *v, int n)
{
  for (int i = 0; i < n; i++) v[i] = rnum(r);
}

#endif /* WZ3_GATE_COMMON_H */
