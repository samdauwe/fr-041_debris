/* Bitmap_Text rasterizer check for wz3.h.
 *
 *   test_wz3_text <assets dir> <fixtures dir> <texture offset> [browser reference.bin]
 *
 * Evaluates every Text operator of the party document with the built-in rasterizer
 * (wz3_text_default) at the given texture offset (-2 or 0) and
 *   - compares an FNV of every coverage map with the pinned C output in
 *     fixtures/text_hashes_offset_{m2,0}.txt (a regression guard: there is no bit-exact reference),
 *   - if given a reference written by tools/dump_text_reference.mjs (headless Chrome running the
 *     production JS canvas adapter), reports and bounds the visual difference.
 *
 * With WZ3_TEXT_WRITE=<file> the pinned hashes are (re)written instead of compared.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* counting allocator with fault injection (the library's WZ3_MALLOC/WZ3_FREE only) */
static long g_live;
static long g_alloc_count;
static long g_fail_at = -1;
static void *test_malloc(size_t n)
{
  if (g_alloc_count++ == g_fail_at) return NULL;
  size_t *p = (size_t *)malloc(n + 16);
  if (!p) return NULL;
  g_live++;
  return (char *)p + 16;
}
#if defined(__GNUC__)
__attribute__((noinline)) /* inlined into a NULL-checked caller, gcc misreports the header arithmetic */
#endif
static void test_free(void *q)
{
  if (!q) return;
  free((void *)((uintptr_t)q - 16));
  g_live--;
}
#define WZ3_MALLOC(n) test_malloc(n)
#define WZ3_FREE(p)   test_free(p)

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

static uint8_t *read_file(const char *dir, const char *name, size_t *size)
{
  char path[1024];
  if (dir)
    snprintf(path, sizeof path, "%s/%s", dir, name);
  else
    snprintf(path, sizeof path, "%s", name);
  FILE *f = fopen(path, "rb");
  if (!f) return NULL;
  fseek(f, 0, SEEK_END);
  const long n = ftell(f);
  fseek(f, 0, SEEK_SET);
  uint8_t *data = (uint8_t *)malloc((size_t)n + 1);
  if (!data || fread(data, 1, (size_t)n, f) != (size_t)n) exit(2);
  fclose(f);
  data[n] = 0;
  *size = (size_t)n;
  return data;
}

typedef struct record {
  int32_t width, height;
  int32_t line_height;
  uint32_t hash;
  uint8_t *coverage;
} record;

typedef struct recorder {
  record *list;
  size_t count, cap;
} recorder;

static uint32_t fnv(const uint8_t *p, size_t n)
{
  uint32_t h = 0x811c9dc5u;
  for (size_t i = 0; i < n; i++) h = (h ^ p[i]) * 0x01000193u;
  return h;
}

static int record_text(void *user, const wz3_text_request *req, wz3_text_result *out)
{
  recorder *r = (recorder *)user;
  if (!wz3_text_default(NULL, req, out)) return 0;
  if (r->count == r->cap) {
    r->cap = r->cap ? r->cap * 2 : 64;
    r->list = (record *)realloc(r->list, r->cap * sizeof *r->list);
  }
  record *rec = &r->list[r->count++];
  const size_t n = (size_t)req->width * (size_t)req->height;
  rec->width = req->width;
  rec->height = req->height;
  rec->line_height = out->line_height;
  rec->hash = fnv(out->coverage, n);
  rec->coverage = (uint8_t *)malloc(n);
  memcpy(rec->coverage, out->coverage, n);
  return 1;
}

static void test_api(void)
{
  wz3_text_request req;
  wz3_text_result out;
  memset(&req, 0, sizeof req);
  req.width = req.height = 64;
  req.x = 0.1f;
  req.y = 0.1f;
  req.text_width = 0.25f;
  req.line_skip = 1.0f;
  req.external_space = 2.0f;
  req.text = "Ag";
  req.font = "";

  CHECK(!wz3_text_default(NULL, NULL, &out) && !wz3_text_default(NULL, &req, NULL), "NULL arguments");
  req.width = 5000;
  CHECK(!wz3_text_default(NULL, &req, &out), "oversized request");
  req.width = 64;

  memset(&out, 0, sizeof out);
  CHECK(wz3_text_default(NULL, &req, &out) && out.coverage && out.line_height > 0, "draws");
  size_t ink = 0;
  for (size_t i = 0; out.coverage && i < 64 * 64; i++) ink += out.coverage[i];
  CHECK(ink > 1000, "two glyphs leave ink (%zu)", ink);
  WZ3_FREE(out.coverage);

  const char *const texts[] = { "", "\n\n", "   ", "\xc2\xb4", "\xff\xfe", "a\nb\nc" };
  for (size_t i = 0; i < sizeof texts / sizeof texts[0]; i++) {
    req.text = texts[i];
    memset(&out, 0, sizeof out);
    CHECK(wz3_text_default(NULL, &req, &out) && out.coverage, "text %zu", i);
    WZ3_FREE(out.coverage);
  }
  req.text = NULL;
  memset(&out, 0, sizeof out);
  CHECK(wz3_text_default(NULL, &req, &out) && out.coverage, "NULL text is empty");
  WZ3_FREE(out.coverage);

  /* a glyph far larger than the canvas, partly outside on every side, and tiny sizes */
  const float sizes[] = { 8.0f, 2.0f, 0.0f, 0.0001f };
  for (size_t i = 0; i < sizeof sizes / sizeof sizes[0]; i++) {
    req.text = "Mg";
    req.text_width = sizes[i];
    req.x = -0.5f;
    req.y = -0.5f;
    req.flags = 3;
    req.text_height = 0.1f;
    memset(&out, 0, sizeof out);
    CHECK(wz3_text_default(NULL, &req, &out) && out.coverage, "size %g", (double)sizes[i]);
    WZ3_FREE(out.coverage);
  }

  /* exact-area coverage: a fully covered pixel is 255, an empty one 0 (the stem of 'l') */
  req.text = "l";
  req.text_width = 0.5f;
  req.text_height = 0.0f;
  req.x = 0.25f;
  req.y = 0.0f;
  req.flags = 0;
  memset(&out, 0, sizeof out);
  CHECK(wz3_text_default(NULL, &req, &out) && out.coverage, "stem");
  int full = 0, empty = 0;
  for (size_t i = 0; out.coverage && i < 64 * 64; i++) {
    full += out.coverage[i] == 255;
    empty += out.coverage[i] == 0;
  }
  CHECK(full > 10 && empty > 2000, "solid interior (%d) and empty background (%d)", full, empty);
  WZ3_FREE(out.coverage);

  /* every allocation may fail: reported, nothing leaks */
  req.text = "Debris\nok";
  req.text_width = 0.2f;
  req.x = 0.1f;
  req.y = 0.1f;
  const long live = g_live;
  for (long fail = 0;; fail++) {
    g_alloc_count = 0;
    g_fail_at = fail;
    memset(&out, 0, sizeof out);
    const int ok = wz3_text_default(NULL, &req, &out);
    g_fail_at = -1;
    if (ok) {
      WZ3_FREE(out.coverage);
      break;
    }
    CHECK(out.coverage == NULL && g_live == live, "allocation %ld: leak or dangling result", fail);
    if (fail > 100) {
      CHECK(0, "never succeeds");
      break;
    }
  }
  CHECK(g_live == live, "no leak");
}

int suite_wz3_text(int argc, char **argv)
{
  test_api();
  if (argc < 4) {
    printf("usage: test_wz3_text <assets dir> <fixtures dir> <texture offset> [reference.bin]\n");
    return 2;
  }
  const int32_t offset = atoi(argv[3]);
  size_t kx_size;
  uint8_t *kx_bytes = read_file(argv[1], "debris_party.kx", &kx_size);
  if (!kx_bytes) {
    printf("cannot read the party document\n");
    return 2;
  }
  wz3_kx *kx = NULL;
  size_t at;
  CHECK(wz3_kx_parse(&kx, kx_bytes, kx_size, &at) == WZ3_OK, "parse");
  if (!kx) return 1;

  recorder rec = { 0 };
  wz3_graph_options options;
  memset(&options, 0, sizeof options);
  wz3_bitmap_ctx_init(&options.ctx);
  options.ctx.texture_offset = offset;
  options.ctx.text = record_text;
  options.ctx.text_user = &rec;
  options.release_consumed = 1;
  wz3_result res;
  wz3_graph *g = wz3_graph_create(kx, &options, &res);
  CHECK(g != NULL, "graph");
  if (!g) return 1;

  /* evaluate the whole bitmap graph in document order, as the player does */
  uint32_t *consumers = (uint32_t *)calloc(kx->n_ops, sizeof(uint32_t));
  for (uint32_t o = 0; o < kx->n_ops; o++)
    if (wz3__find_spec(kx->ops[o].class_id))
      for (uint32_t k = 0; k < kx->ops[o].n_inputs; k++) consumers[kx->ops[o].inputs[k]]++;
  uint32_t text_ops = 0, evaluated = 0, failed = 0;
  for (uint32_t o = 0; o < kx->n_ops; o++) {
    if (!wz3__find_spec(kx->ops[o].class_id)) continue;
    const wz3_bitmap *bm = NULL;
    const wz3_result r = wz3_graph_eval_bitmap(g, o, &bm);
    if (kx->ops[o].class_id == 0x031) text_ops++;
    if (r != WZ3_OK || !bm) {
      failed++;
      continue;
    }
    evaluated++;
    for (uint32_t k = 0; k < kx->ops[o].n_inputs; k++)
      if (--consumers[kx->ops[o].inputs[k]] == 0) wz3_graph_release(g, kx->ops[o].inputs[k]);
    if (consumers[o] == 0) wz3_graph_release(g, o);
  }
  printf("text: %u Text operators, %zu rasterized, %u bitmaps evaluated, %u failed\n", text_ops, rec.count, evaluated, failed);
  CHECK(text_ops == 34 && rec.count == 34, "34 Text operators expected, got %u (%zu rasterized)", text_ops, rec.count);
  CHECK(failed == 2, "only the two known unevaluable operators may fail, got %u", failed);

  /* pinned C output */
  const char *write_path = getenv("WZ3_TEXT_WRITE");
  if (write_path) {
    FILE *f = fopen(write_path, "wb");
    for (size_t i = 0; i < rec.count; i++) fprintf(f, "%zu %d %d %d %u\n", i, rec.list[i].width, rec.list[i].height, rec.list[i].line_height, rec.list[i].hash);
    fclose(f);
    printf("wrote %s\n", write_path);
  } else {
    size_t size;
    char name[64];
    snprintf(name, sizeof name, "text_hashes_offset_%s.txt", offset < 0 ? "m2" : "0");
    char *text = (char *)read_file(argv[2], name, &size);
    CHECK(text != NULL, "%s is missing", name);
    size_t seen = 0;
    for (char *line = text ? strtok(text, "\n") : NULL; line; line = strtok(NULL, "\n")) {
      unsigned i, w, h, lh, hash;
      if (sscanf(line, "%u %u %u %u %u", &i, &w, &h, &lh, &hash) != 5 || i >= rec.count) continue;
      seen++;
      CHECK((int32_t)w == rec.list[i].width && (int32_t)h == rec.list[i].height && (int32_t)lh == rec.list[i].line_height && hash == rec.list[i].hash,
            "text %u: C output changed (%u vs pinned %u)", i, rec.list[i].hash, hash);
    }
    CHECK(seen == rec.count, "%zu of %zu pinned hashes checked", seen, rec.count);
    free(text);
  }

  /* visual comparison with the browser's canvas */
  if (argc > 4) {
    size_t size;
    uint8_t *ref = read_file(NULL, argv[4], &size);
    CHECK(ref != NULL, "cannot read %s", argv[4]);
    size_t pos = 0;
    double worst_mean = 0.0;
    for (size_t i = 0; ref && i < rec.count; i++) {
      if (pos + 12 > size) {
        CHECK(0, "reference too short");
        break;
      }
      uint32_t w, h, lh;
      memcpy(&w, ref + pos, 4);
      memcpy(&h, ref + pos + 4, 4);
      memcpy(&lh, ref + pos + 8, 4);
      pos += 12;
      const size_t n = (size_t)w * h;
      CHECK((int32_t)w == rec.list[i].width && (int32_t)h == rec.list[i].height, "reference %zu has another size", i);
      if (pos + n > size) {
        CHECK(0, "reference too short");
        break;
      }
      double sum_abs = 0.0, sum_ref = 0.0, sum_c = 0.0;
      size_t big = 0;
      int32_t max_diff = 0;
      for (size_t k = 0; k < n; k++) {
        const int32_t a = rec.list[i].coverage[k], b = ref[pos + k];
        const int32_t d = a > b ? a - b : b - a;
        sum_abs += d;
        sum_ref += b;
        sum_c += a;
        if (d > 64) big++;
        if (d > max_diff) max_diff = d;
      }
      const double mean_abs = sum_abs / (double)n;
      /* ink: total coverage; a layout error shows up in both numbers */
      const double ink_ratio = sum_ref > 0.0 ? sum_c / sum_ref : 1.0;
      printf("  text %2zu %4ux%-4u lh C %d / canvas %u  mean|d| %.4f  ink C/canvas %.4f  px>64: %zu  max %d\n", i, w, h, rec.list[i].line_height, lh, mean_abs,
             ink_ratio, big, max_diff);
      if (mean_abs > worst_mean) worst_mean = mean_abs;
      /* bounds calibrated on Chrome 144 + Liberation Sans with --plain (no kerning, no hinting): the
         residual is Skia's text contrast boost on small sizes (up to ~7 % less ink here) and sub-pixel
         edge placement; a layout error (baseline, advance, scale) moves these numbers by 10x */
      CHECK(mean_abs <= 0.5 && max_diff <= 160 && ink_ratio >= 0.90 && ink_ratio <= 1.02 && (size_t)lh == (size_t)rec.list[i].line_height,
            "text %zu leaves the calibrated tolerance (mean %.4f max %d ink %.4f)", i, mean_abs, max_diff, ink_ratio);
      pos += n;
    }
    printf("text: worst mean abs difference %.4f of 255\n", worst_mean);
    free(ref);
  }

  for (size_t i = 0; i < rec.count; i++) free(rec.list[i].coverage);
  free(rec.list);
  free(consumers);
  wz3_graph_destroy(g);
  wz3_kx_destroy(kx);
  free(kx_bytes);
  printf("text: %d checks, %d failures\n", g_checks, g_failures);
  return g_failures ? 1 : 0;
}
