/* Checks for player_load.h: the geometry sink of the player.
 *
 *   test_player_load <party.kx>
 *
 * On the party document: exactly the 574 geometries a scene draws are prepared and consumed once each, the other 10,366
 * generator intermediates are skipped, the statistics agree with what the consumer saw, a failing consumer or prepare stops
 * the precalculation with its error, and nothing leaks in any of these cases (every allocation of create + precalc with the
 * sink installed is failed at sampled points). ploader_fraction is hand-checked.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static long g_live, g_count, g_fail_at = -1;
static void *t_malloc(size_t n)
{
  if (g_count++ == g_fail_at) return NULL;
  size_t *p = (size_t *)malloc(n + 16);
  if (!p) return NULL;
  g_live++;
  return (char *)p + 16;
}
#if defined(__GNUC__)
__attribute__((noinline))
#endif
static void t_free(void *q)
{
  if (!q) return;
  free((void *)((uintptr_t)q - 16));
  g_live--;
}
#define WZ3_MALLOC(n)         t_malloc(n)
#define WZ3_FREE(p)           t_free(p)
#define PLAYER_LOAD_MALLOC(n) t_malloc(n)
#define PLAYER_LOAD_FREE(p)   t_free(p)

#define WZ3_IMPLEMENTATION
#include <debris/core/wz3.h>
#define PLAYER_LOAD_IMPLEMENTATION
#include <debris/player/player_load.h>
#define PLAYER_ARGS_IMPLEMENTATION
#include <debris/player/player_args.h>

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

static int read_file(const char *path, unsigned char **data, size_t *size)
{
  FILE *f = fopen(path, "rb");
  if (!f) return 0;
  fseek(f, 0, SEEK_END);
  const long n = ftell(f);
  fseek(f, 0, SEEK_SET);
  unsigned char *d = (unsigned char *)malloc((size_t)n + 1);
  if (!d || fread(d, 1, (size_t)n, f) != (size_t)n) {
    fclose(f);
    return 0;
  }
  fclose(f);
  *data = d;
  *size = (size_t)n;
  return 1;
}

typedef struct consumer {
  const uint8_t *wanted;
  uint8_t *seen;
  uint32_t n_ops, calls, not_wanted, twice, empty, bad_source, minmeshes;
  uint64_t bytes;
  uint32_t fail_at; /* 1-based call that returns an error; 0 = never */
} consumer;

static wz3_result consume(void *user, uint32_t op, wz3_geometry *g, const wz3_mesh *mesh, const wz3_minmesh *minmesh)
{
  consumer *c = (consumer *)user;
  c->calls++;
  if ((mesh != NULL) == (minmesh != NULL)) c->bad_source++; /* exactly one source */
  if (minmesh) c->minmeshes++;
  if (op >= c->n_ops || !c->wanted[op])
    c->not_wanted++;
  else if (c->seen[op]++)
    c->twice++;
  if (!g->n_vertices || !g->n_indices || !g->block_) c->empty++;
  c->bytes += wz3_geometry_bytes(g);
  wz3_geometry_free(g); /* the consumer owns it, error or not */
  return c->fail_at && c->calls == c->fail_at ? WZ3_ERR_LIMIT : WZ3_OK;
}

static wz3_rt *make_rt(const wz3_kx *kx, pgeometry *pg, wz3_result *res)
{
  wz3_rt_options o;
  memset(&o, 0, sizeof o);
  o.release_consumed = 1;
  o.frame_jobs = 1;
  o.host.user = pg;
  o.host.mesh = pgeometry_mesh;
  o.host.minmesh = pgeometry_minmesh;
  return wz3_rt_create(kx, &o, res);
}

static void test_fraction(void)
{
  CHECK(ploader_fraction(0, 0) == 0.0 && ploader_fraction(5, 0) == 0.0, "unknown total");
  CHECK(ploader_fraction(0, 10) == 0.0 && ploader_fraction(5, 10) == 0.5 && ploader_fraction(10, 10) == 1.0, "fractions");
  CHECK(ploader_fraction(11, 10) == 1.0 && ploader_fraction(0xffffffffu, 0xfffffffeu) == 1.0, "never above 1");
  /* the three stages: unknown totals count as not started, a stage fills its share, the sum reaches exactly 1 and never passes it */
  CHECK(ploader_stages(0, 0, 0, 0, 0, 0) == 0.0 && ploader_stages(5, 0, 5, 0, 5, 0) == 0.0, "stages without totals");
  CHECK(ploader_stages(10, 10, 0, 0, 0, 0) > 0.0 && ploader_stages(10, 10, 0, 0, 0, 0) < 0.1, "operators alone are a small share");
  CHECK(ploader_stages(10, 10, 10, 10, 10, 10) == 1.0 && ploader_stages(11, 10, 11, 10, 11, 10) == 1.0, "complete is exactly 1");
  double previous = 0.0;
  int monotonic = 1;
  for (uint32_t i = 0; i <= 30; i++) {
    const double f = ploader_stages(i > 10 ? 10 : i, 10, i > 10 ? (i > 20 ? 10 : i - 10) : 0, 10, i > 20 ? i - 20 : 0, 10);
    if (f < previous || f > 1.0) monotonic = 0;
    previous = f;
  }
  CHECK(monotonic && previous == 1.0, "stages grow monotonically to 1");
}

static void test_arguments(const wz3_kx *kx)
{
  pgeometry pg;
  CHECK(pgeometry_init(NULL, kx, NULL, NULL) == WZ3_ERR_ARGUMENT && pgeometry_init(&pg, NULL, NULL, NULL) == WZ3_ERR_ARGUMENT, "NULL arguments");
  pgeometry_destroy(NULL);
  const long live = g_live;
  for (long fail = 0; fail < 1; fail++) {
    g_count = 0;
    g_fail_at = fail;
    CHECK(pgeometry_init(&pg, kx, NULL, NULL) == WZ3_ERR_OUT_OF_MEMORY && !pg.wanted && g_live == live, "init failure");
    g_fail_at = -1;
  }
  CHECK(pgeometry_init(&pg, kx, NULL, NULL) == WZ3_OK, "init");
  /* operators outside the document and unmarked ones are skipped, a missing minmesh is counted as empty */
  CHECK(pgeometry_mesh(&pg, kx->n_ops + 5, NULL) == WZ3_OK && pg.skipped == 1 && pg.prepared == 0, "operator out of range");
  CHECK(pgeometry_minmesh(&pg, 0, NULL) == WZ3_OK && pg.empty == 1, "no minmesh");
  pgeometry_destroy(&pg);
  CHECK(g_live == live && !pg.wanted, "destroy leaks %ld", g_live - live);
}

/* every precalculation below runs the same document; `fail` fails that allocation (-1: none) */
static wz3_result run(const wz3_kx *kx, pgeometry_consume_fn fn, consumer *c, pgeometry *pg, long fail)
{
  wz3_result res = pgeometry_init(pg, kx, fn, c);
  if (res != WZ3_OK) return res;
  if (c) {
    c->wanted = pg->wanted;
    c->n_ops = kx->n_ops;
  }
  g_count = 0;
  g_fail_at = fail;
  wz3_rt *rt = make_rt(kx, pg, &res);
  if (rt) res = wz3_rt_precalc(rt);
  g_fail_at = -1;
  wz3_rt_destroy(rt);
  return res;
}

/* --texture-quality reaches the texture evaluation: the option means what the library's textureSizeOffset means, and the
   graph options it fills make bitmaps of that size (op 38 is a 512x512 texture at full quality), Render as the placeholder. */
static void test_texture_quality(const wz3_kx *kx)
{
  static const char *const accepted_texts[] = {
    "high", "ultra", "full", "normal", "reduced", "medium", "low", "HIGH", "Low", "0", "-1", "-2", "-3", "+0", "-0"
  };
  for (size_t i = 0; i < sizeof accepted_texts / sizeof accepted_texts[0]; i++) {
    int offset = 99, recognised = 0;
    CHECK(ptexture_quality(accepted_texts[i], &offset) == 1, "'%s' refused", accepted_texts[i]);
    CHECK(wz3_texture_quality_offset(accepted_texts[i], &recognised) == offset && recognised, "'%s': the player says %d, the library %d", accepted_texts[i],
          offset, wz3_texture_quality_offset(accepted_texts[i], NULL));
  }
  for (int offset = 0; offset >= -3; offset--) {
    wz3_graph_options go;
    ptexture_graph_options(&go, offset);
    CHECK(go.ctx.texture_offset == offset && go.ctx.default_format == 1 && go.release_consumed && go.render_placeholder && !go.render, "graph options at %d",
          offset);
    wz3_result res;
    wz3_graph *g = wz3_graph_create(kx, &go, &res);
    CHECK(g && res == WZ3_OK, "graph create");
    if (!g) continue;
    const wz3_bitmap *bm = NULL;
    CHECK(wz3_graph_eval_bitmap(g, 38, &bm) == WZ3_OK && bm && bm->width == (512 >> -offset) && bm->height == (512 >> -offset), "op 38 at offset %d is %dx%d",
          offset, bm ? bm->width : 0, bm ? bm->height : 0);
    const wz3_bitmap *render = NULL;
    CHECK(wz3_graph_eval_bitmap(g, 11943, &render) == WZ3_OK && render && render->width == (512 >> -offset), "Render is the placeholder at offset %d", offset);
    wz3_graph_destroy(g);
  }
}

int suite_player_load(int argc, char **argv)
{
  if (argc < 2) {
    printf("usage: test_player_load <party.kx>\n");
    return 2;
  }
  unsigned char *bytes;
  size_t size;
  if (!read_file(argv[1], &bytes, &size)) {
    printf("cannot read %s\n", argv[1]);
    return 2;
  }
  wz3_kx *kx = NULL;
  CHECK(wz3_kx_parse(&kx, bytes, size, NULL) == WZ3_OK, "parse");
  if (!kx) return 1;
  test_fraction();
  test_arguments(kx);
  test_texture_quality(kx);
  const long live = g_live;

  /* the full run with a consumer */
  consumer c;
  memset(&c, 0, sizeof c);
  c.seen = (uint8_t *)calloc(kx->n_ops, 1);
  pgeometry pg;
  wz3_result res = run(kx, consume, &c, &pg, -1);
  CHECK(res == WZ3_OK, "precalc: %s", wz3_result_string(res));
  CHECK(pg.prepared == 574 && c.calls == 574 && c.not_wanted == 0 && c.twice == 0 && c.empty == 0, "%u prepared, %u consumed, %u unwanted, %u twice, %u empty",
        pg.prepared, c.calls, c.not_wanted, c.twice, c.empty);
  CHECK(c.bad_source == 0 && c.minmeshes > 0 && c.minmeshes < 574, "%u consumer calls had no single source, %u from MinMeshes", c.bad_source, c.minmeshes);
  CHECK(pg.prepared + pg.skipped + pg.empty == 10940, "%u prepared + %u skipped + %u empty is not 10940", pg.prepared, pg.skipped, pg.empty);
  CHECK(pg.bytes == c.bytes && pg.bytes > 100u * 1048576u && pg.max_bytes <= pg.bytes && pg.vertices > 0 && pg.indices > 0,
        "statistics: %llu bytes vs %llu seen", (unsigned long long)pg.bytes, (unsigned long long)c.bytes);
  uint32_t marked_seen = 0;
  for (uint32_t i = 0; i < kx->n_ops; i++)
    if (pg.wanted[i] && c.seen[i] == 1) marked_seen++;
  CHECK(marked_seen == 574, "%u of the 574 marked operators were consumed once", marked_seen);
  printf("player_load: %u geometries (%.1f MiB) prepared, %u intermediates skipped\n", pg.prepared, (double)pg.bytes / 1048576.0, pg.skipped);
  pgeometry_destroy(&pg);
  CHECK(g_live == live, "full run leaks %ld blocks", g_live - live);

  /* no consumer: counted and freed */
  res = run(kx, NULL, NULL, &pg, -1);
  CHECK(res == WZ3_OK && pg.prepared == 574 && pg.bytes == c.bytes, "no consumer: %s, %u prepared", wz3_result_string(res), pg.prepared);
  pgeometry_destroy(&pg);
  CHECK(g_live == live, "no consumer leaks %ld blocks", g_live - live);

  /* a failing consumer stops the precalculation with its error */
  for (uint32_t fail_at = 1; fail_at <= 574; fail_at += 573) {
    memset(c.seen, 0, kx->n_ops);
    c.calls = c.twice = c.not_wanted = c.empty = 0;
    c.bytes = 0;
    c.fail_at = fail_at;
    res = run(kx, consume, &c, &pg, -1);
    CHECK(res == WZ3_ERR_LIMIT && c.calls == fail_at && pg.prepared == fail_at, "consumer failing at %u: %s after %u calls", fail_at, wz3_result_string(res),
          c.calls);
    pgeometry_destroy(&pg);
    CHECK(g_live == live, "consumer failing at %u leaks %ld blocks", fail_at, g_live - live);
  }
  c.fail_at = 0;

  /* allocation failures during the whole precalculation, sampled */
  memset(c.seen, 0, kx->n_ops);
  c.calls = 0;
  c.bytes = 0;
  res = run(kx, consume, &c, &pg, -1); /* counting run: g_count is the number of allocations */
  const long total = g_count;
  pgeometry_destroy(&pg);
  long points = 0, oom = 0;
  for (long i = 0; i < 28; i++) {
    const long fail = i < 8 ? i : 8 + (total - 16) * (i - 8) / 20;
    memset(c.seen, 0, kx->n_ops);
    c.calls = c.twice = c.not_wanted = c.empty = 0;
    c.bytes = 0;
    res = run(kx, consume, &c, &pg, fail);
    points++;
    if (res == WZ3_ERR_OUT_OF_MEMORY) oom++;
    CHECK(res == WZ3_ERR_OUT_OF_MEMORY, "allocation %ld reported as %s", fail, wz3_result_string(res));
    pgeometry_destroy(&pg);
    CHECK(g_live == live, "allocation %ld leaks %ld blocks", fail, g_live - live);
  }
  printf("player_load: %ld of %ld allocations failed in turn (%ld points), each reported as out of memory, nothing leaks\n", points, total, oom);
  free(c.seen);
  wz3_kx_destroy(kx);
  free(bytes);
  printf("%d checks, %d failures\n", g_checks, g_failures);
  return g_failures ? 1 : 0;
}
