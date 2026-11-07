/* Checks for player_scene.h: what the player keeps resident for the renderer.
 *
 *   test_player_scene <party.kx>
 *
 * On the party document, at texture offset -3, with the recording GL of gl_mock.h:
 *   - the arguments of every entry point, and every allocation of pscene_create failed in turn;
 *   - the precalculation through the pscene sinks keeps the 574 scene geometries with shadow data and a retained copy of the
 *     animated MinMeshes; the plan lists the 256 textures of the materials, the three Render operators, the static geometries;
 *   - the worker part (pscene_eval_textures) reports monotonic progress, stops at a cancel and resumes to the same result;
 *   - the GL-thread part (pscene_warm_step) uploads everything in order, every texture is given back to the graph after its
 *     upload (256 released), an allocation failure leaves the item pending and the retry succeeds, nothing leaks;
 *   - the first frame after the warm-up creates no texture and uploads no static buffer, and never asks the host for a bitmap.
 */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static long g_live, g_alloc_count, g_fail_at = -1;
static void *test_malloc(size_t n)
{
  if (g_alloc_count++ == g_fail_at) return NULL;
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
#define WZ3_MALLOC(n)         test_malloc(n)
#define WZ3_FREE(p)           test_free(p)
#define GLR_MALLOC(n)         test_malloc(n)
#define GLR_FREE(p)           test_free(p)
#define PLAYER_LOAD_MALLOC(n) test_malloc(n)
#define PLAYER_LOAD_FREE(p)   test_free(p)

#define WZ3_IMPLEMENTATION
#include <debris/core/wz3.h>
#define GLR_IMPLEMENTATION
#include <debris/render/glr.h>
#include "support/gate_common.h"
#include "support/gl_mock.h"
#define PLAYER_LOAD_IMPLEMENTATION
#include <debris/player/player_load.h>
#define PLAYER_SCENE_IMPLEMENTATION
#include <debris/player/player_scene.h>

enum { TEXTURE_OFFSET = -3 };

static uint64_t g_texture_uploads, g_static_uploads, g_dynamic_uploads;
static void frame_hook(void *user, const char *line, uint64_t index)
{
  (void)user;
  (void)index;
  if (!strncmp(line, "compressedTexImage2D", 20) || (!strncmp(line, "texImage2D", 10) && strstr(line, " d:")))
    g_texture_uploads++; /* pixels, not a render target */
  else if (!strncmp(line, "bufferData", 10)) {
    if (strstr(line, " 35044"))
      g_static_uploads++;
    else
      g_dynamic_uploads++;
  }
}

typedef struct progress {
  uint32_t calls, last_done, total;
  int monotonic;
  uint32_t cancel_after;
} progress;
static void on_progress(void *user, uint32_t done, uint32_t total)
{
  progress *p = (progress *)user;
  if (p->calls && done < p->last_done) p->monotonic = 0;
  if (p->calls && total != p->total) p->monotonic = 0;
  p->calls++;
  p->last_done = done;
  p->total = total;
}
static int on_cancel(void *user)
{
  progress *p = (progress *)user;
  return p->cancel_after && p->last_done >= p->cancel_after;
}

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

static void test_arguments(const wz3_kx *kx)
{
  pscene ps;
  pscene_options o;
  memset(&o, 0, sizeof o);
  o.texture_offset = TEXTURE_OFFSET;
  CHECK(pscene_create(NULL, kx, &o) == WZ3_ERR_ARGUMENT && pscene_create(&ps, NULL, &o) == WZ3_ERR_ARGUMENT && pscene_create(&ps, kx, NULL) == WZ3_ERR_ARGUMENT,
        "pscene_create arguments");
  pscene_destroy(NULL);
  pscene_attach(NULL, NULL);
  pscene_set_renderer(NULL, NULL);
  CHECK(pscene_glr_host(NULL) == NULL, "host of nothing");
  CHECK(pscene_plan(NULL) == WZ3_ERR_ARGUMENT && pscene_eval_textures(NULL, NULL, NULL, NULL) == WZ3_ERR_ARGUMENT &&
          pscene_warm_step(NULL, NULL, NULL) == GLR_ERR_ARGUMENT,
        "calls on nothing");
  const long live = g_live;
  CHECK(pscene_create(&ps, kx, &o) == WZ3_OK, "create");
  CHECK(pscene_plan(&ps) == WZ3_ERR_ARGUMENT, "plan before a runtime is attached");
  CHECK(pscene_eval_textures(&ps, NULL, NULL, NULL) == WZ3_ERR_ARGUMENT, "evaluation before the plan");
  uint32_t done = 0, total = 0;
  CHECK(pscene_warm_step(&ps, &done, &total) == GLR_ERR_ARGUMENT, "warm-up before a renderer");
  CHECK(pscene_bitmap(&ps, kx->n_ops) == NULL && pscene_glr_host(&ps) && pscene_glr_host(&ps)->bitmap_uploaded == NULL, "no release unless asked for");
  CHECK(pscene_glr_host(&ps)->geometry(&ps, kx->n_ops, -1.0, 0) == NULL && pscene_glr_host(&ps)->shadow_data(&ps, kx->n_ops) == NULL, "geometry out of range");
  pscene_destroy(&ps);
  CHECK(g_live == live, "destroy leaks %ld blocks", g_live - live);
  o.release_uploaded = 1;
  CHECK(pscene_create(&ps, kx, &o) == WZ3_OK && pscene_glr_host(&ps)->bitmap_uploaded != NULL, "release option");
  pscene_destroy(&ps);
  /* every allocation of the creation fails in turn */
  long points = 0;
  for (long k = 0; k < 200; k++) {
    g_alloc_count = 0;
    g_fail_at = k;
    const wz3_result r = pscene_create(&ps, kx, &o);
    g_fail_at = -1;
    if (r == WZ3_OK) {
      pscene_destroy(&ps);
      break;
    }
    points++;
    CHECK(r == WZ3_ERR_OUT_OF_MEMORY && g_live == live, "creation failing at allocation %ld: %s, %ld blocks left", k, wz3_result_string(r), g_live - live);
  }
  CHECK(points >= 8 && points < 200, "%ld allocation points in pscene_create", points);
  CHECK(g_live == live, "creation sweep leaks %ld blocks", g_live - live);
}

typedef struct flow {
  pscene ps;
  wz3_rt *rt;
} flow;

static wz3_rt *make_rt(const wz3_kx *kx, pscene *ps)
{
  wz3_rt_options o;
  memset(&o, 0, sizeof o);
  o.generic_exec = 1;
  o.release_consumed = 1;
  o.frame_jobs = 1;
  o.host.user = ps;
  o.host.mesh = pscene_mesh;
  o.host.minmesh = pscene_minmesh;
  o.host.bitmap = pscene_bitmap;
  wz3_result res;
  wz3_rt *rt = wz3_rt_create(kx, &o, &res);
  if (rt) {
    pscene_attach(ps, rt);
    wz3_rt_set_aspect(rt, 2.0);
  }
  return rt;
}

int suite_player_scene(int argc, char **argv)
{
  if (argc < 2) {
    printf("usage: test_player_scene <party.kx>\n");
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
  test_arguments(kx);
  const long live = g_live;

  pscene ps;
  pscene_options o;
  memset(&o, 0, sizeof o);
  o.texture_offset = TEXTURE_OFFSET;
  o.bitmap_render = 1;
  o.release_uploaded = 1;
  CHECK(pscene_create(&ps, kx, &o) == WZ3_OK, "create");
  wz3_rt *rt = make_rt(kx, &ps);
  CHECK(rt != NULL, "runtime");
  if (!rt) return 1;
  wz3_result res = wz3_rt_precalc(rt);
  CHECK(res == WZ3_OK, "precalc: %s", wz3_result_string(res));
  uint32_t have = 0, with_shadow = 0;
  for (uint32_t op = 0; op < kx->n_ops; op++) {
    if (!ps.have[op]) continue;
    have++;
    CHECK(pscene_glr_host(&ps)->geometry(&ps, op, -1.0, 0) == &ps.geo[op], "operator %u has its geometry", op);
    with_shadow += pscene_glr_host(&ps)->shadow_data(&ps, op) != NULL;
    CHECK(ps.geo[op].animated == (ps.retained[op] != NULL), "operator %u: animated geometry and retained MinMesh agree", op);
  }
  CHECK(have == 574 && ps.geometry.prepared == 574 && with_shadow == 574, "%u geometries kept, %u prepared, %u with shadow data", have, ps.geometry.prepared,
        with_shadow);
  CHECK(ps.animated > 0 && ps.animated < 574 && ps.with_shadow_recipe > 0, "%u animated, %u with a shadow recipe", ps.animated, ps.with_shadow_recipe);
  /* an animated geometry poses into its own scratch slot per request, a static one is returned as is */
  for (uint32_t op = 0; op < kx->n_ops; op++) {
    if (!ps.retained[op]) continue;
    const glr_host *h = pscene_glr_host(&ps);
    const wz3_geometry *a = h->geometry(&ps, op, 0.5, 0), *b = h->geometry(&ps, op, 0.75, 1), *again = h->geometry(&ps, op, 0.5, 0);
    CHECK(a && b && a != b && again == a && a->n_vertices == ps.geo[op].n_vertices, "pose slots of operator %u", op);
    CHECK(h->geometry(&ps, op, 0.5, -1) == NULL, "negative slot");
    break;
  }

  /* the plan */
  CHECK(pscene_plan(&ps) == WZ3_OK, "plan");
  uint32_t n_render = 0, n_texture_items = 0, n_geometry_items = 0;
  for (uint32_t i = 0; i < ps.n_items; i++) {
    n_render += ps.items[i].kind == PSCENE_ITEM_RENDER;
    n_texture_items += ps.items[i].kind == PSCENE_ITEM_TEXTURE;
    n_geometry_items += ps.items[i].kind == PSCENE_ITEM_GEOMETRY;
    if (i) CHECK(ps.items[i].kind >= ps.items[i - 1].kind, "items are ordered Render, texture, geometry");
  }
  CHECK(n_render == 3 && n_texture_items == ps.n_textures && n_geometry_items == 574 - ps.animated, "%u Render, %u texture, %u geometry items", n_render,
        n_texture_items, n_geometry_items);
  CHECK(ps.n_textures == 256 && ps.n_independent > 200 && ps.n_independent < ps.n_textures, "%u textures, %u independent of the renderer", ps.n_textures,
        ps.n_independent);
  for (uint32_t i = 1; i < ps.n_textures; i++) CHECK(ps.textures[i] > ps.textures[i - 1], "textures ascend");

  /* the worker part: progress, cancel, resume */
  progress p;
  memset(&p, 0, sizeof p);
  p.monotonic = 1;
  p.cancel_after = 7;
  CHECK(pscene_eval_textures(&ps, on_progress, on_cancel, &p) == WZ3_ERR_CANCELLED && p.last_done == 7, "cancelled after %u textures", p.last_done);
  memset(&p, 0, sizeof p);
  p.monotonic = 1;
  CHECK(pscene_eval_textures(&ps, on_progress, on_cancel, &p) == WZ3_OK, "evaluation");
  CHECK(p.monotonic && p.calls == ps.n_independent + 1 && p.last_done == ps.n_independent && p.total == ps.n_independent, "progress: %u calls, %u of %u",
        p.calls, p.last_done, p.total);
  CHECK(ps.bitmap_failures == 0, "%u bitmaps failed", ps.bitmap_failures);

  /* the GL thread part */
  glm_reset();
  glr_options go;
  memset(&go, 0, sizeof go);
  glr_result gres;
  glr *r = glr_create(glm_loader, &go, &gres);
  CHECK(r && gres == GLR_OK, "glr_create");
  if (!r) return 1;
  CHECK(glr_resize(r, 320, 180, 2.0) == GLR_OK, "resize");
  uint32_t done = 0, total = 0;
  CHECK(pscene_warm_step(&ps, &done, &total) == GLR_ERR_ARGUMENT, "warm-up needs the renderer set");
  pscene_set_renderer(&ps, r);
  uint32_t previous = 0, steps = 0, oom_retries = 0, injected_at = 0xffffffffu;
  int ordered = 1;
  for (;;) {
    long inject = -1;
    /* An allocation failure inside the graph (Render operators, textures that depend on them) is a failed operator for good - the graph
       keeps errors - and draws the fallback texture; the retry contract is for the uploads, which is what is injected here. */
    const pscene_item *next = ps.next_item < ps.n_items ? &ps.items[ps.next_item] : NULL;
    const int uploads_only =
      next && (next->kind == PSCENE_ITEM_GEOMETRY || (next->kind == PSCENE_ITEM_TEXTURE && !wz3_graph_render_dependent(ps.graph, next->op)));
    if (uploads_only && steps % 11 == 5 && injected_at != steps) { /* once per step: the retry runs clean */
      injected_at = steps;
      g_alloc_count = 0;
      inject = (long)(steps / 11) % 5;
      g_fail_at = inject;
    }
    gres = pscene_warm_step(&ps, &done, &total);
    g_fail_at = -1;
    if (gres == GLR_ERR_OUT_OF_MEMORY) {
      oom_retries++;
      CHECK(done == previous && inject >= 0, "an out-of-memory step %u advanced to %u (was %u)", steps, done, previous);
      continue; /* the item is still pending: the same step again */
    }
    CHECK(gres == GLR_OK, "warm step %u: result %d", steps, (int)gres);
    if (gres != GLR_OK) break;
    if (done < previous || done > previous + 1) ordered = 0;
    previous = done;
    steps++;
    if (done >= total) break;
  }
  CHECK(ordered && total == ps.n_items && done == total, "warm-up: %u of %u items in %u steps", done, total, steps);
  CHECK(oom_retries > 0, "no injected failure reached a warm-up step");
  CHECK(pscene_warm_step(&ps, &done, &total) == GLR_OK && done == total, "a finished warm-up stays finished");
  CHECK(ps.released == ps.n_textures && ps.bitmap_failures == 0, "%u of %u textures given back, %u failures", ps.released, ps.n_textures, ps.bitmap_failures);
  CHECK(glr_resize(r, 320, 180, 2.0) == GLR_OK, "resize after the Render operators");
  const long objects_warm = glm.live_objects;
  CHECK(objects_warm > 700, "%ld GL objects after the warm-up", objects_warm);
  printf("player_scene: %u textures, %u geometries warm (%ld GL objects), %u out-of-memory retries\n", ps.n_textures, n_geometry_items, objects_warm,
         oom_retries);

  /* the first frame: no texture, no static buffer, no bitmap request */
  CHECK(wz3_rt_frame_at_sample(rt, 3037500, 44100) == WZ3_OK, "frame");
  glm.hook = frame_hook;
  const uint64_t requests = ps.bitmap_requests;
  glr_result fr = glr_render_frame(r, pscene_glr_host(&ps));
  glm.hook = NULL;
  CHECK(fr == GLR_OK, "frame result %d", (int)fr);
  glr_stats st;
  glr_get_stats(r, &st);
  CHECK(st.draw_calls > 100 && st.triangles > 10000, "the frame draws %u calls, %u triangles", st.draw_calls, st.triangles);
  CHECK(g_texture_uploads == 0, "%llu textures uploaded during the first frame", (unsigned long long)g_texture_uploads);
  CHECK(g_static_uploads == 0, "%llu static buffers uploaded during the first frame", (unsigned long long)g_static_uploads);
  CHECK(ps.bitmap_requests == requests, "the frame asked for %llu bitmaps", (unsigned long long)(ps.bitmap_requests - requests));
  printf("player_scene: first frame: %u draw calls, %llu dynamic uploads, no texture, no static buffer\n", st.draw_calls,
         (unsigned long long)g_dynamic_uploads);

  pscene_set_renderer(&ps, NULL);
  glr_destroy(r);
  CHECK(glm.live_objects == 0, "%ld GL objects left after glr_destroy", glm.live_objects);
  pscene_destroy(&ps);
  wz3_rt_destroy(rt);
  CHECK(g_live == live, "the full flow leaks %ld blocks", g_live - live);
  glm_reset();
  wz3_kx_destroy(kx);
  free(bytes);
  printf("%d checks, %d failures\n", g_checks, g_failures);
  return g_failures ? 1 : 0;
}
