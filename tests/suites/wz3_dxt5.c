/* BC3 (DXT5) codec check for wz3.h.
 *
 *   test_wz3_dxt5 <fixtures dir> [oracle output]
 *
 * Runs the synthetic block corpus shared with tools/oracles/dxt5_oracle.cpp and
 * tools/dump_dxt5_vectors.mjs (8 classes x 3000 blocks) and compares
 *   - the FNV-1a of the encoded blocks with the JS port (fixtures/dxt5_vectors.txt)
 *     and with the released rygdxt source (the oracle's "class N hash" lines),
 *   - the FNV-1a of the decoded blocks with the JS port,
 * and re-derives the embedded endpoint match tables.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define WZ3_IMPLEMENTATION
#include <debris/core/wz3.h>

static int g_checks, g_failures;
#define CHECK(cond, ...)                          \
  do {                                            \
    g_checks++;                                   \
    if (!(cond)) {                                \
      g_failures++;                               \
      printf("FAIL %s:%d: ", __FILE__, __LINE__); \
      printf(__VA_ARGS__);                        \
      printf("\n");                               \
    }                                             \
  } while (0)

static uint32_t g_rng;
static uint32_t next(void)
{
  g_rng ^= g_rng << 13;
  g_rng ^= g_rng >> 17;
  g_rng ^= g_rng << 5;
  return g_rng;
}

static void make_block(int cls, uint8_t px[64])
{
  uint8_t c0[4], c1[4];
  switch (cls) {
  case 0:
    for (int i = 0; i < 64; i++) px[i] = (uint8_t)(next() & 255);
    break;
  case 1:
    for (int k = 0; k < 4; k++) c0[k] = (uint8_t)(next() & 255);
    for (int k = 0; k < 4; k++) c1[k] = (uint8_t)(next() & 255);
    for (int i = 0; i < 16; i++) {
      const int t = i * 17;
      for (int k = 0; k < 4; k++) px[i * 4 + k] = (uint8_t)((c0[k] * (255 - t) + c1[k] * t) / 255);
    }
    break;
  case 2:
    for (int k = 0; k < 4; k++) c0[k] = (uint8_t)(next() & 255);
    for (int k = 0; k < 4; k++) c1[k] = (uint8_t)(next() & 255);
    for (int i = 0; i < 16; i++) {
      const uint8_t *c = (next() & 1) ? c1 : c0;
      for (int k = 0; k < 4; k++) px[i * 4 + k] = c[k];
    }
    break;
  case 3:
    for (int k = 0; k < 4; k++) c0[k] = (uint8_t)(next() & 255);
    for (int i = 0; i < 16; i++)
      for (int k = 0; k < 4; k++) px[i * 4 + k] = c0[k];
    break;
  case 4:
    for (int k = 0; k < 4; k++) c0[k] = (uint8_t)(next() & 255);
    for (int i = 0; i < 16; i++) {
      for (int k = 0; k < 3; k++) px[i * 4 + k] = c0[k];
      px[i * 4 + 3] = (uint8_t)(next() & 255);
    }
    break;
  case 5:
    for (int k = 0; k < 4; k++) c0[k] = (uint8_t)(next() & 255);
    for (int i = 0; i < 16; i++) {
      for (int k = 0; k < 3; k++) {
        const int v = c0[k] + (int)(next() & 3) - 1;
        px[i * 4 + k] = (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v);
      }
      px[i * 4 + 3] = 255;
    }
    break;
  case 6:
    for (int i = 0; i < 16; i++) {
      for (int k = 0; k < 3; k++) px[i * 4 + k] = (uint8_t)(next() & 255);
      px[i * 4 + 3] = (next() & 1) ? 255 : 0;
    }
    break;
  default:
    for (int i = 0; i < 16; i++) {
      const uint8_t g = (uint8_t)(next() & 255);
      for (int k = 0; k < 4; k++) px[i * 4 + k] = g;
    }
    break;
  }
}

static uint32_t fnv(uint32_t h, const uint8_t *p, size_t n)
{
  for (size_t i = 0; i < n; i++) h = (h ^ p[i]) * 0x01000193u;
  return h;
}

static char *read_text(const char *dir, const char *name)
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
  char *s = (char *)malloc((size_t)n + 1);
  if (!s || fread(s, 1, (size_t)n, f) != (size_t)n) exit(2);
  fclose(f);
  s[n] = 0;
  return s;
}

int suite_wz3_dxt5(int argc, char **argv)
{
  if (argc < 2) {
    printf("usage: test_wz3_dxt5 <fixtures dir> [oracle output]\n");
    return 2;
  }
  uint32_t enc_hash[8], dec_hash[8];
  for (int cls = 0; cls < 8; cls++) {
    g_rng = 0x1234567u + (uint32_t)cls * 7919u;
    uint32_t he = 0x811c9dc5u, hd = 0x811c9dc5u;
    for (int n = 0; n < 3000; n++) {
      uint8_t px[64], enc[16], dec[64];
      make_block(cls, px);
      wz3_bc3_compress_block(px, enc);
      wz3_bc3_decompress_block(enc, dec);
      he = fnv(he, enc, 16);
      hd = fnv(hd, dec, 64);
    }
    enc_hash[cls] = he;
    dec_hash[cls] = hd;
  }

  char *vectors = read_text(argv[1], "dxt5_vectors.txt");
  int seen = 0;
  for (char *line = strtok(vectors, "\n"); line; line = strtok(NULL, "\n")) {
    int cls;
    unsigned long e, d;
    if (sscanf(line, "class%d %lu %lu", &cls, &e, &d) != 3 || cls < 0 || cls > 7) continue;
    seen++;
    CHECK(enc_hash[cls] == (uint32_t)e, "class %d encoded: C %u, JS %lu", cls, enc_hash[cls], e);
    CHECK(dec_hash[cls] == (uint32_t)d, "class %d decoded: C %u, JS %lu", cls, dec_hash[cls], d);
  }
  CHECK(seen == 8, "fixture has %d classes", seen);
  free(vectors);

  if (argc > 2) {
    FILE *f = fopen(argv[2], "rb");
    int oracle_seen = 0;
    CHECK(f != NULL, "cannot open oracle output %s", argv[2]);
    char line[128];
    while (f && fgets(line, sizeof line, f)) {
      int cls;
      unsigned long h;
      if (sscanf(line, "class%d %lu", &cls, &h) != 2 || cls < 0 || cls > 7) continue;
      oracle_seen++;
      CHECK(enc_hash[cls] == (uint32_t)h, "class %d: C %u, rygdxt %lu", cls, enc_hash[cls], h);
    }
    if (f) fclose(f);
    CHECK(oracle_seen == 8, "oracle printed %d classes", oracle_seen);
  }

  /* the embedded OMatch tables are the ones the encoder would derive at start-up */
  uint8_t table[512];
  wz3__bc3_match_table(table, 32, 3, 2);
  CHECK(!memcmp(table, wz3__bc3_match5, 512), "match5 table");
  wz3__bc3_match_table(table, 64, 2, 4);
  CHECK(!memcmp(table, wz3__bc3_match6, 512), "match6 table");

  /* properties independent of the references */
  {
    uint8_t px[64], enc[16], dec[64];
    for (int i = 0; i < 16; i++) {
      px[i * 4] = 0;
      px[i * 4 + 1] = 0;
      px[i * 4 + 2] = 0;
      px[i * 4 + 3] = 255;
    }
    wz3_bc3_compress_block(px, enc);
    wz3_bc3_decompress_block(enc, dec);
    CHECK(!memcmp(px, dec, 64), "opaque black round trips");
    for (int i = 0; i < 16; i++) {
      px[i * 4] = 255;
      px[i * 4 + 1] = 255;
      px[i * 4 + 2] = 255;
      px[i * 4 + 3] = 0;
    }
    wz3_bc3_compress_block(px, enc);
    wz3_bc3_decompress_block(enc, dec);
    CHECK(!memcmp(px, dec, 64), "transparent white round trips");
  }

  printf("dxt5: %d checks, %d failures\n", g_checks, g_failures);
  return g_failures ? 1 : 0;
}
