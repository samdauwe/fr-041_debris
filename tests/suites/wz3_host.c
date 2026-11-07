/* Host interface check for wz3.h: sinks, progress and cancellation of the runtime precalculation.
 *
 *   test_wz3_host <assets dir>
 *
 * On the party document: progress is reported once per operator and reaches the total; cancelling at any point is
 * clean (nothing leaks) and resumable, ending in exactly the state of an uninterrupted run; a sink that fails stops
 * the precalculation with its error and leaves nothing behind; the sinks see every mesh once and are skipped for
 * placeholder geometry; every allocation of create + precalc (with progress) may fail.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static long g_live, g_count, g_fail_at = -1;
static void *test_malloc(size_t n)
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

static uint8_t *read_all(const char *dir, const char *name, size_t *size)
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
  uint8_t *d = (uint8_t *)malloc((size_t)n + 1);
  if (!d || fread(d, 1, (size_t)n, f) != (size_t)n) exit(2);
  fclose(f);
  d[n] = 0;
  *size = (size_t)n;
  return d;
}

#define NEVER 0xffffffffu
typedef struct host {
  uint32_t calls, first_done, last_done, last_op, total;
  int monotonic;
  uint32_t cancel_after;                  /* cancelled() answers yes once this many operators completed in the run */
  uint32_t meshes, minmeshes, null_minmeshes;
  uint32_t fail_mesh_at, fail_minmesh_at; /* 1-based call number that fails; 0 = never */
} host;

static void host_reset(host *h)
{
  memset(h, 0, sizeof *h);
  h->monotonic = 1;
  h->cancel_after = NEVER;
}

static void on_progress(void *user, uint32_t op, uint32_t done, uint32_t total)
{
  host *h = (host *)user;
  if (h->calls == 0)
    h->first_done = done;
  else if (done != h->last_done + 1)
    h->monotonic = 0;
  h->calls++;
  h->last_done = done;
  h->last_op = op;
  h->total = total;
}

static int on_cancelled(void *user)
{
  const host *h = (const host *)user;
  return h->calls >= h->cancel_after;
}

static wz3_result on_mesh(void *user, uint32_t op, const wz3_mesh *mesh)
{
  host *h = (host *)user;
  (void)op;
  (void)mesh;
  h->meshes++;
  return h->fail_mesh_at && h->meshes == h->fail_mesh_at ? WZ3_ERR_LIMIT : WZ3_OK;
}

static wz3_result on_minmesh(void *user, uint32_t op, const wz3_minmesh *mesh)
{
  host *h = (host *)user;
  (void)op;
  h->minmeshes++;
  if (!mesh) h->null_minmeshes++;
  return h->fail_minmesh_at && h->minmeshes == h->fail_minmesh_at ? WZ3_ERR_LIMIT : WZ3_OK;
}

static wz3_rt *make_rt(const wz3_kx *kx, wz3_geometry_policy geometry, host *h, int progress, int sinks, wz3_result *res)
{
  wz3_rt_options o;
  memset(&o, 0, sizeof o);
  o.release_consumed = 1;
  o.frame_jobs = 1;
  o.geometry = geometry;
  o.host.user = h;
  if (progress) {
    o.host.progress = on_progress;
    o.host.cancelled = on_cancelled;
  }
  if (sinks) {
    o.host.mesh = on_mesh;
    o.host.minmesh = on_minmesh;
  }
  return wz3_rt_create(kx, &o, res);
}

/* Frames at fixed samples and the snapshot after them: the state a run ends in. */
static wz3_result run_frames(wz3_rt *rt, uint8_t **snap, size_t *size)
{
  static const int32_t samples[] = { 0, 88200, 441000 };
  for (size_t i = 0; i < sizeof samples / sizeof samples[0]; i++) {
    wz3_result r = wz3_rt_frame_at_sample(rt, samples[i], 44100);
    uint32_t n;
    if (r == WZ3_OK) r = wz3_rt_effects_advance(rt, &n);
    if (r != WZ3_OK) return r;
  }
  *size = wz3_rt_snapshot_size(rt);
  *snap = (uint8_t *)malloc(*size ? *size : 1);
  if (!*snap) return WZ3_ERR_OUT_OF_MEMORY;
  return wz3_rt_snapshot(rt, *snap, *size);
}

static void test_progress(const wz3_kx *kx, uint8_t **reference, size_t *reference_size, uint32_t *reference_total)
{
  host h;
  host_reset(&h);
  const long live = g_live;
  wz3_result res;
  wz3_rt *rt = make_rt(kx, WZ3_GEOMETRY_PLACEHOLDER, &h, 1, 1, &res);
  CHECK(rt != NULL, "create: %s", wz3_result_string(res));
  if (!rt) return;
  res = wz3_rt_precalc(rt);
  CHECK(res == WZ3_OK, "precalc: %s", wz3_result_string(res));
  CHECK(h.calls == h.total && h.total > 1000 && h.total <= kx->n_ops, "%u progress calls for %u reachable of %u operators", h.calls, h.total, kx->n_ops);
  CHECK(h.first_done == 1 && h.last_done == h.total && h.monotonic, "progress is 1..total in steps of one (first %u, last %u)", h.first_done, h.last_done);
  CHECK(h.last_op == kx->roots[0], "the root is calculated last (op %u, root %u)", h.last_op, kx->roots[0]);
  CHECK(h.meshes == 0 && h.minmeshes == 0, "placeholder geometry calls no sink (%u, %u)", h.meshes, h.minmeshes);
  *reference_total = h.total;
  /* a second call has nothing to do: no progress, same total semantics */
  const uint32_t calls = h.calls;
  res = wz3_rt_precalc(rt);
  CHECK(res == WZ3_OK && h.calls == calls, "second precalc is a no-op (%u more calls)", h.calls - calls);
  res = run_frames(rt, reference, reference_size);
  CHECK(res == WZ3_OK, "frames: %s", wz3_result_string(res));
  wz3_rt_destroy(rt);
  CHECK(g_live == live, "%ld blocks leaked", g_live - live);
  printf("host: progress reports %u operators of %u, root last\n", *reference_total, kx->n_ops);
}

/* Cancelling after N operators, then resuming, ends in the state of the uninterrupted run. */
static void test_cancel_resume(const wz3_kx *kx, const uint8_t *reference, size_t reference_size, uint32_t total)
{
  const uint32_t points[] = { 0, 1, 2, 7, total / 3, total / 2, total - 2, total - 1 };
  for (size_t i = 0; i < sizeof points / sizeof points[0]; i++) {
    const uint32_t n = points[i];
    host h;
    host_reset(&h);
    h.cancel_after = n;
    const long live = g_live;
    wz3_result res;
    wz3_rt *rt = make_rt(kx, WZ3_GEOMETRY_PLACEHOLDER, &h, 1, 0, &res);
    CHECK(rt != NULL, "create");
    if (!rt) continue;
    res = wz3_rt_precalc(rt);
    CHECK(res == WZ3_ERR_CANCELLED && h.calls == n, "cancel after %u: %s, %u operators completed", n, wz3_result_string(res), h.calls);
    /* a frame request precalculates on demand and is cancelled the same way */
    CHECK(wz3_rt_frame_at_sample(rt, 0, 44100) == WZ3_ERR_CANCELLED && h.calls == n, "frame while cancelled (%u)", h.calls);
    h.cancel_after = NEVER;
    h.calls = 0;
    res = wz3_rt_precalc(rt);
    CHECK(res == WZ3_OK, "resume after %u: %s", n, wz3_result_string(res));
    CHECK(h.total == total && h.last_done == total && h.first_done == n + 1 && h.monotonic, "resume after %u: done %u..%u of %u (monotonic %d)", n,
          h.first_done, h.last_done, h.total, h.monotonic);
    uint8_t *snap = NULL;
    size_t size = 0;
    res = run_frames(rt, &snap, &size);
    CHECK(res == WZ3_OK, "frames after resume: %s", wz3_result_string(res));
    CHECK(res != WZ3_OK || (size == reference_size && !memcmp(snap, reference, size)), "resume after %u operators ends in a different state", n);
    free(snap);
    wz3_rt_destroy(rt);
    CHECK(g_live == live, "cancel after %u: %ld blocks leaked", n, g_live - live);
  }
  printf("host: cancel + resume at %zu points reproduces the uninterrupted state\n", sizeof points / sizeof points[0]);

  /* destroying a cancelled runtime must not need the resume */
  host h;
  host_reset(&h);
  h.cancel_after = total / 2;
  const long live = g_live;
  wz3_result res;
  wz3_rt *rt = make_rt(kx, WZ3_GEOMETRY_PLACEHOLDER, &h, 1, 0, &res);
  if (rt) {
    CHECK(wz3_rt_precalc(rt) == WZ3_ERR_CANCELLED, "cancel");
    wz3_rt_destroy(rt);
  }
  CHECK(g_live == live, "destroy after cancel: %ld blocks leaked", g_live - live);
}

/* Real geometry: the sinks see each mesh, a failing sink stops with its error, leaks nothing and can be retried. */
static void test_sinks(const wz3_kx *kx)
{
  const long live = g_live;
  host h;
  uint32_t failures[2] = { 0, 0 };
  for (int which = 0; which < 2; which++) {
    host_reset(&h);
    if (which == 0)
      h.fail_mesh_at = 3;
    else
      h.fail_minmesh_at = 3;
    wz3_result res;
    wz3_rt *rt = make_rt(kx, WZ3_GEOMETRY_GENERATE, &h, 1, 1, &res);
    CHECK(rt != NULL, "create");
    if (!rt) continue;
    res = wz3_rt_precalc(rt);
    CHECK(res == WZ3_ERR_LIMIT, "%s sink failure is reported as the sink's error, got %s", which ? "minmesh" : "mesh", wz3_result_string(res));
    CHECK(which ? h.minmeshes == 3 : h.meshes == 3, "the failing sink is the last call (%u meshes, %u minmeshes)", h.meshes, h.minmeshes);
    failures[which] = h.calls;
    wz3_rt_destroy(rt);
    CHECK(g_live == live, "%s sink failure leaks %ld blocks", which ? "minmesh" : "mesh", g_live - live);
  }
  /* retry after a sink failure: the failed operator is calculated again and the run completes */
  host_reset(&h);
  h.fail_mesh_at = 3;
  wz3_result res;
  wz3_rt *rt = make_rt(kx, WZ3_GEOMETRY_GENERATE, &h, 1, 1, &res);
  CHECK(rt != NULL, "create");
  if (rt) {
    CHECK(wz3_rt_precalc(rt) == WZ3_ERR_LIMIT, "first run fails");
    h.fail_mesh_at = 0;
    h.calls = 0;
    res = wz3_rt_precalc(rt);
    CHECK(res == WZ3_OK && h.last_done == h.total, "retry: %s (%u of %u)", wz3_result_string(res), h.last_done, h.total);
    CHECK(h.meshes == 10391 + 1, "the sink saw %u meshes (10,391 operators, the failed one twice)", h.meshes);
    printf("host: sinks saw %u meshes and %u minmeshes (%u without geometry); failures at progress %u and %u are clean\n", h.meshes, h.minmeshes,
           h.null_minmeshes, failures[0], failures[1]);
    wz3_rt_destroy(rt);
  }
  CHECK(g_live == live, "retry leaks %ld blocks", g_live - live);
}

/* Every allocation of create + precalc (with the reachability count) may fail: the first ones, those around the end of
   create (where the count runs), evenly spread ones and the last ones (a full sweep is 100k+ runs). */
static void sweep_point(const wz3_kx *kx, long fail, long *oom)
{
  host h;
  host_reset(&h);
  const long live = g_live;
  g_count = 0;
  g_fail_at = fail;
  wz3_result res;
  wz3_rt *rt = make_rt(kx, WZ3_GEOMETRY_PLACEHOLDER, &h, 1, 1, &res);
  if (rt) res = wz3_rt_precalc(rt);
  g_fail_at = -1;
  wz3_rt_destroy(rt);
  CHECK(g_live == live, "allocation %ld: %ld blocks leaked", fail, g_live - live);
  CHECK(res == WZ3_ERR_OUT_OF_MEMORY, "allocation %ld reported as %s", fail, wz3_result_string(res));
  if (res == WZ3_ERR_OUT_OF_MEMORY) (*oom)++;
}

static void test_alloc_sweep(const wz3_kx *kx)
{
  host h;
  host_reset(&h);
  wz3_result res;
  g_count = 0;
  wz3_rt *rt = make_rt(kx, WZ3_GEOMETRY_PLACEHOLDER, &h, 1, 1, &res);
  const long created = g_count;
  if (rt) res = wz3_rt_precalc(rt);
  const long total = g_count;
  wz3_rt_destroy(rt);
  CHECK(rt && res == WZ3_OK && total > created + 1000, "counting run: %ld allocations, create %ld", total, created);
  long oom = 0;
  for (long fail = 0; fail < 64 && fail < total; fail++) sweep_point(kx, fail, &oom);
  for (long fail = created - 4; fail < created + 8 && fail < total; fail++)
    if (fail >= 64) sweep_point(kx, fail, &oom);
  for (long i = 0; i < 100; i++) {
    const long fail = 64 + (total - 72) * i / 100;
    if (fail >= created + 8) sweep_point(kx, fail, &oom);
  }
  for (long fail = total - 8; fail < total; fail++) sweep_point(kx, fail, &oom);
  printf(
    "host: allocation sweep over create + precalc (%ld allocations, create takes %ld): %ld failure points, each reported as out of memory, nothing leaks\n",
    total, created, oom);
}

int suite_wz3_host(int argc, char **argv)
{
  if (argc < 2) {
    printf("usage: test_wz3_host <assets dir>\n");
    return 2;
  }
  size_t size;
  uint8_t *bytes = read_all(argv[1], "debris_party.kx", &size);
  wz3_kx *kx = NULL;
  CHECK(wz3_kx_parse(&kx, bytes, size, NULL) == WZ3_OK, "parse");
  if (!kx) return 1;
  uint8_t *reference = NULL;
  size_t reference_size = 0;
  uint32_t total = 0;
  test_progress(kx, &reference, &reference_size, &total);
  if (reference && total > 8) test_cancel_resume(kx, reference, reference_size, total);
  test_alloc_sweep(kx);
  test_sinks(kx);
  free(reference);
  wz3_kx_destroy(kx);
  free(bytes);
  printf("%d checks, %d failures\n", g_checks, g_failures);
  return g_failures ? 1 : 0;
}
