/* Production frame check for glr.h against src/renderer.js.
 *
 *   test_glr_frames <assets dir> <fixtures dir> [--frames N] [--dump file] [--trace js-trace-file]
 *
 * Runs the Debris party document through the library runtime (geometry sinks, shadow data, retained animated MinMeshes,
 * bitmap graph at texture offset -2 with the empty-coverage text callback), renders the frames of
 * tools/dump_glr_frames.mjs with glr_render_frame on the recording GL of gl_mock.h and compares, per frame, the number of GL
 * calls and the FNV-1a hash of every trace line with what the real JavaScript Renderer asked of the recording WebGL, plus the
 * draw-call and triangle counters. The renderer persists across frames on both sides, so the caches (textures, geometry,
 * topologies, instance buffer, render targets) are part of the comparison.
 *
 * Development aids: --dump writes the C trace (same layout as `dump_glr_frames.mjs --trace`); --trace compares line by line with
 * such a file and reports the first divergence of every frame with the calls before it.
 */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* counting allocator with fault injection, shared by the library and the renderer: the sweep makes every allocation of a frame fail in turn */
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
#define WZ3_MALLOC(n) test_malloc(n)
#define WZ3_FREE(p)   test_free(p)
#define GLR_MALLOC(n) test_malloc(n)
#define GLR_FREE(p)   test_free(p)

#define WZ3_IMPLEMENTATION
#include <debris/core/wz3.h>
#define GLR_IMPLEMENTATION
#include <debris/render/glr.h>
#include "support/gate_common.h"
#include "support/gl_mock.h"

#include "support/frame_world.h"

/* ---- the trace comparison against a JS trace file (development aid) ---- */

typedef struct cmp {
  FILE *f;
  int active, diverged;
  char ring[8][400];
  uint64_t first_bad;
  unsigned divergences;
} cmp;

static cmp g_cmp;

static int cmp_seek(cmp *c, const char *title)
{
  c->active = 0;
  c->diverged = 0;
  if (!c->f) return 0;
  static char line[4096];
  char want[128];
  snprintf(want, sizeof want, "# %s\n", title);
  rewind(c->f);
  while (fgets(line, sizeof line, c->f)) {
    if (!strcmp(line, want)) {
      c->active = 1;
      return 1;
    }
  }
  return 0;
}

static void cmp_hook(void *user, const char *line, uint64_t index)
{
  cmp *c = (cmp *)user;
  strncpy(c->ring[index % 8], line, sizeof c->ring[0] - 1);
  c->ring[index % 8][sizeof c->ring[0] - 1] = 0;
  if (!c->active || c->diverged) return;
  static char expect[4096];
  const long where = ftell(c->f);
  if (!fgets(expect, sizeof expect, c->f) || expect[0] == '#') {
    c->diverged = 1;
    c->divergences++;
    printf("  call %llu: C has more calls than JS: %s\n", (unsigned long long)index, line);
    (void)where;
    return;
  }
  expect[strcspn(expect, "\n")] = 0;
  if (strcmp(expect, line)) {
    c->diverged = 1;
    c->divergences++;
    c->first_bad = index;
    printf("  first divergence at call %llu\n    JS: %.300s\n    C : %.300s\n  before it:\n", (unsigned long long)index, expect, line);
    for (uint64_t k = index >= 6 ? index - 6 : 0; k < index; k++) printf("    %.200s\n", c->ring[k % 8]);
  }
}

static void cmp_end(cmp *c, uint64_t calls)
{
  if (!c->active || c->diverged) return;
  static char expect[4096];
  if (fgets(expect, sizeof expect, c->f) && expect[0] != '#') {
    c->divergences++;
    printf("  C ends after %llu calls, JS continues: %.300s", (unsigned long long)calls, expect);
  }
}

/* ---- the fixture ---- */


/* coverage of the production frames, counted on the trace lines */
static struct {
  uint64_t stencil_ops, instanced, indexed, arrays, blit, buffer_data, buffer_sub, tex_image, tex_param, generate_mipmap, compressed, uniforms, programs,
    targets;
} g_cover;

static void cover_hook(void *user, const char *line, uint64_t index)
{
  cmp_hook(user, line, index);
  switch (line[0]) {
  case 's':
    if (!strncmp(line, "stencilOp", 9)) g_cover.stencil_ops++;
    break;
  case 'd':
    if (!strncmp(line, "drawElementsInstanced", 21))
      g_cover.instanced++;
    else if (!strncmp(line, "drawElements ", 13))
      g_cover.indexed++;
    else if (!strncmp(line, "drawArrays", 10))
      g_cover.arrays++;
    break;
  case 'b':
    if (!strncmp(line, "blitFramebuffer", 15))
      g_cover.blit++;
    else if (!strncmp(line, "bufferData", 10))
      g_cover.buffer_data++;
    else if (!strncmp(line, "bufferSubData", 13))
      g_cover.buffer_sub++;
    break;
  case 't':
    if (!strncmp(line, "texImage2D", 10))
      g_cover.tex_image++;
    else if (!strncmp(line, "texParameter", 12))
      g_cover.tex_param++;
    break;
  case 'g':
    if (!strncmp(line, "generateMipmap", 14)) g_cover.generate_mipmap++;
    break;
  case 'c':
    if (!strncmp(line, "compressedTexImage2D", 20)) g_cover.compressed++;
    break;
  case 'u':
    if (!strncmp(line, "uniform", 7)) g_cover.uniforms++;
    break;
  case 'f':
    if (!strncmp(line, "framebufferTexture2D", 20)) g_cover.targets++;
    break;
  default: break;
  }
}

typedef struct frame_expect {
  long sample;
  unsigned calls, hash, draws, triangles;
} frame_expect;
/* a document patch: set one word of one operator (class_id < 0), or clear/set bits of a word of every operator of a class */
typedef struct patch {
  int32_t class_id;
  uint32_t op, word, bits, and_mask;
} patch;
typedef struct scenario {
  char name[32];
  int shadows, s3tc;
  double anisotropy;
  patch patches[32];
  unsigned n_patches;
  unsigned constructor_calls, constructor_hash, render_calls, render_hash;
  int bitmap_render;
  frame_expect frames[64];
  unsigned n_frames;
} scenario;

static unsigned g_compared, g_mismatches;
static uint64_t g_total_calls;
static FILE *g_dump;
static long g_max_frames = -1;
static int g_sweep = 1;
static long g_sweep_point = -1;

static uint64_t g_null_binds;
static void null_bind_hook(void *user, const char *line, uint64_t index)
{
  (void)user;
  (void)index;
  if (!strncmp(line, "bindTexture 3553 null", 21)) g_null_binds++;
}

/* Every allocation of one frame (and of one Bitmap_Render) fails in turn: the result is the out-of-memory error or success, nothing crashes, and
   once the renderer is destroyed no block and no GL object is left. The first points are all tried, then a stride keeps the run short. */
static void sweep_allocations(world *w, const glr_host *host, const glr_options *options, long sample, int bitmap_render, const char *name)
{
  glr_result res;
  long points = 0, tried = 0, failed_oom = 0;
  uint64_t clean_null_binds = 0;
  for (int pass = 0; pass < 2; pass++) { /* pass 0 counts, pass 1 sweeps */
    long stride = pass ? (points > 400 ? points / 300 : 1) : 1;
    for (long k = (pass && g_sweep_point >= 0) ? g_sweep_point : 0; k < (pass ? ((g_sweep_point >= 0) ? g_sweep_point + 1 : points) : 1);
         k += (k < 60 ? 1 : stride)) {
      glm_reset();
      glm.hook = null_bind_hook;
      g_null_binds = 0;
      glm.keep = g_sweep_point >= 0;
      const long live_before = g_live;
      glr *r = glr_create(glm_loader, options, &res);
      CHECK(r != NULL, "[%s] sweep: create", name);
      if (!r) return;
      w->renderer = r;
      CHECK(glr_resize(r, 640, 360, 2.0) == GLR_OK, "[%s] sweep: resize", name);
      if (!bitmap_render) CHECK(wz3_rt_frame_at_sample(w->rt, (int32_t)sample, 44100) == WZ3_OK, "[%s] sweep: frame", name);
      wz3_bitmap *bitmap = NULL;
      g_alloc_count = 0;
      g_fail_at = pass ? k : -1;
      if (bitmap_render) {
        uint32_t render_op = 0;
        for (uint32_t op = 0; op < w->kx->n_ops; op++)
          if (w->kx->ops[op].class_id == 0x3f) {
            render_op = op;
            break;
          }
        res = glr_render_bitmap(r, host, w->kx->ops[render_op].inputs[0], 128, 128, &bitmap);
      } else
        res = glr_render_frame(r, host);
      g_fail_at = -1;
      if (!pass) {
        points = g_alloc_count;
        clean_null_binds = g_null_binds;
      }
      tried += pass;
      failed_oom += res == GLR_ERR_OUT_OF_MEMORY;
      CHECK(res == GLR_OK || res == GLR_ERR_OUT_OF_MEMORY, "[%s] sweep point %ld: result %d", name, k, (int)res);
      if (bitmap_render) CHECK((res == GLR_OK) == (bitmap != NULL), "[%s] sweep point %ld: bitmap and result disagree", name, k);
      if (pass && !bitmap_render && res == GLR_ERR_OUT_OF_MEMORY) {
        /* the caches must be usable after the failure: the same frame again binds exactly the textures of a clean frame */
        g_null_binds = 0;
        CHECK(glr_resize(r, 640, 360, 2.0) == GLR_OK && glr_render_frame(r, host) == GLR_OK, "[%s] sweep point %ld: the frame after the failure", name, k);
        CHECK(g_null_binds == clean_null_binds, "[%s] sweep point %ld: %llu unbound texture slots after the failure, %llu in a clean frame", name, k,
              (unsigned long long)g_null_binds, (unsigned long long)clean_null_binds);
      }
      glm.hook = NULL;
      wz3_bitmap_destroy(bitmap);
      glr_destroy(r);
      w->renderer = NULL;
      if (glm.live_objects != 0 && glm.keep) { /* development aid (--sweep-point): which GL object stayed */
        unsigned id = 0;
        char deleted[4096] = { 0 };
        for (uint64_t i = 0; glm_line(i); i++) {
          const char *l = glm_line(i);
          if (!strncmp(l, "create", 6) && strncmp(l, "createShader", 12) == 0)
            id++;
          else if (!strncmp(l, "create", 6))
            id++;
          unsigned d;
          if (!strncmp(l, "delete", 6) && sscanf(strchr(l, '#') + 1, "%u", &d) == 1 && d < sizeof deleted) deleted[d] = 1;
        }
        id = 0;
        for (uint64_t i = 0; glm_line(i); i++) {
          const char *l = glm_line(i);
          if (!strncmp(l, "create", 6)) {
            id++;
            if (!deleted[id]) {
              printf("  leaked #%u: %s (call %llu)\n", id, l, (unsigned long long)i);
              for (uint64_t j = i + 1; j < i + 4 && glm_line(j); j++) printf("      %s\n", glm_line(j));
            }
          }
        }
      }
      CHECK(g_live == live_before && glm.live_objects == 0, "[%s] sweep point %ld: %ld blocks and %ld GL objects left", name, k, g_live - live_before,
            glm.live_objects);
      if (!pass) break;
    }
  }
  printf("glr frames [%s]: allocation sweep: %ld allocation points, %ld tried, %ld ended out of memory\n", name, points, tried, failed_oom);
  CHECK(points > (bitmap_render ? 100 : 1000) && failed_oom >= tried / 2, "[%s] sweep covers %ld points, %ld ran out of memory", name, points, failed_oom);
}

/* argument contracts of the frame entry points */
static uint32_t g_asked, g_given_back;
static const glr_host *g_inner;
static const wz3_bitmap *counting_bitmap(void *user, uint32_t op)
{
  (void)user;
  g_asked++;
  return g_inner->bitmap(g_inner->user, op);
}
static void counting_uploaded(void *user, uint32_t op)
{
  (void)user;
  (void)op;
  g_given_back++;
}

static void check_contracts(world *w, glr *r, const glr_host *host)
{
  int32_t root = 7;
  uint32_t render_input = 0;
  for (uint32_t op = 0; op < w->kx->n_ops; op++)
    if (w->kx->ops[op].class_id == 0x3f) {
      render_input = w->kx->ops[op].inputs[0];
      break;
    }
  CHECK(wz3_rt_render_begin(NULL, 0, 1.0, &root) == WZ3_ERR_ARGUMENT && wz3_rt_render_begin(w->rt, 0, 1.0, NULL) == WZ3_ERR_ARGUMENT, "render_begin arguments");
  CHECK(wz3_rt_render_begin(w->rt, w->kx->n_ops, 1.0, &root) == WZ3_ERR_ARGUMENT && root == -1, "render_begin: operator out of range");
  {
    wz3_result res;
    wz3_rt_options options;
    memset(&options, 0, sizeof options);
    options.generic_exec = 1;
    options.release_consumed = 1;
    options.frame_jobs = 1;
    options.geometry = WZ3_GEOMETRY_PLACEHOLDER;
    wz3_rt *fresh = wz3_rt_create(w->kx, &options, &res);
    CHECK(fresh && res == WZ3_OK && wz3_rt_render_begin(fresh, render_input, 1.0, &root) == WZ3_ERR_ARGUMENT, "render_begin before the precalculation");
    wz3_rt_destroy(fresh);
  }
  CHECK(wz3_rt_render_end(NULL) == WZ3_ERR_ARGUMENT && wz3_rt_render_end(w->rt) == WZ3_OK, "render_end without begin");
  CHECK(wz3_rt_render_begin(w->rt, render_input, 1.0, &root) == WZ3_OK && root >= 0, "render_begin");
  CHECK(wz3_rt_render_begin(w->rt, render_input, 1.0, &root) == WZ3_ERR_ARGUMENT, "render_begin does not nest");
  CHECK(wz3_rt_render_end(w->rt) == WZ3_OK && wz3_rt_render_end(w->rt) == WZ3_OK, "render_end, twice");
  wz3_bitmap *bitmap = NULL;
  CHECK(glr_render_bitmap(NULL, host, render_input, 8, 8, &bitmap) == GLR_ERR_ARGUMENT &&
          glr_render_bitmap(r, NULL, render_input, 8, 8, &bitmap) == GLR_ERR_ARGUMENT &&
          glr_render_bitmap(r, host, render_input, 0, 8, &bitmap) == GLR_ERR_ARGUMENT &&
          glr_render_bitmap(r, host, render_input, 8, 8, NULL) == GLR_ERR_ARGUMENT &&
          glr_render_bitmap(r, host, w->kx->n_ops, 8, 8, &bitmap) == GLR_ERR_ARGUMENT && bitmap == NULL,
        "glr_render_bitmap arguments");
  {
    /* the bitmap of a Render is the read-back flipped: GL rows run bottom-up, bitmaps top-down */
    const int32_t size = 16;
    glm.fill_pixels = 1;
    wz3_bitmap *got = NULL;
    CHECK(glr_render_bitmap(r, host, render_input, size, size, &got) == GLR_OK && got && got->width == size && got->height == size,
          "glr_render_bitmap of %dx%d", size, size);
    glm.fill_pixels = 0;
    for (int32_t y = 0; got && y < size; y += size - 1) {
      uint8_t texel[4];
      for (int k = 0; k < 4; k++) texel[k] = (uint8_t)(((size_t)(size - 1 - y) * (size_t)size * 4u + (size_t)k) * 37u + 11u); /* first texel of that GL row */
      wz3_bitmap *expect = wz3_bitmap_from_rgba8(1, 1, texel, 1, 0);
      CHECK(expect && !memcmp(got->data + (size_t)y * (size_t)size * 4u, expect->data, 8), "row %d of the bitmap is GL row %d", y, size - 1 - y);
      wz3_bitmap_destroy(expect);
    }
    wz3_bitmap_destroy(got);
  }
  /* the overlay (credits, statistics): arguments, one texture kept for every call, an allocation failure is reported and leaks nothing */
  {
    uint8_t px[16] = { 0 };
    const long objects = glm.live_objects;
    CHECK(glr_present_overlay(r, px, 2, 2, 0, 0) == GLR_OK && glm.live_objects == objects + 1, "overlay creates its texture once");
    CHECK(glr_present_overlay(r, px, 2, 2, 5, 5) == GLR_OK && glm.live_objects == objects + 1, "overlay reuses its texture");
    CHECK(glr_present_overlay(r, NULL, 2, 2, 0, 0) == GLR_ERR_ARGUMENT && glr_present_overlay(r, px, 0, 2, 0, 0) == GLR_ERR_ARGUMENT &&
            glr_present_overlay(r, px, 2, 8193, 0, 0) == GLR_ERR_ARGUMENT,
          "overlay arguments");
    const long live = g_live;
    g_alloc_count = 0;
    g_fail_at = 0;
    CHECK(glr_present_overlay(r, px, 2, 2, 0, 0) == GLR_ERR_OUT_OF_MEMORY, "overlay out of memory");
    g_fail_at = -1;
    CHECK(g_live == live, "overlay out of memory leaks %ld blocks", g_live - live);
  }
  CHECK(glr_render_frame(NULL, host) == GLR_ERR_ARGUMENT && glr_render_frame(r, NULL) == GLR_ERR_ARGUMENT, "glr_render_frame arguments");
  /* the warm-up entry points of the player: arguments, and a warmed operator is a hit afterwards */
  CHECK(glr_warm_texture(NULL, host, 0) == GLR_ERR_ARGUMENT && glr_warm_texture(r, NULL, 0) == GLR_ERR_ARGUMENT &&
          glr_warm_texture(r, host, w->kx->n_ops) == GLR_ERR_ARGUMENT,
        "glr_warm_texture arguments");
  CHECK(glr_warm_geometry(NULL, host, 0) == GLR_ERR_ARGUMENT && glr_warm_geometry(r, NULL, 0) == GLR_ERR_ARGUMENT &&
          glr_warm_geometry(r, host, w->kx->n_ops) == GLR_ERR_ARGUMENT,
        "glr_warm_geometry arguments");
  {
    glr_host no_bitmap = *host, no_geometry = *host;
    no_bitmap.bitmap = NULL;
    no_geometry.geometry = NULL;
    CHECK(glr_warm_texture(r, &no_bitmap, 0) == GLR_ERR_ARGUMENT && glr_warm_geometry(r, &no_geometry, 0) == GLR_ERR_ARGUMENT,
          "warm-up without the host callback");
  }
  for (uint32_t op = 0; op < w->kx->n_ops; op++) {
    if (!w->have[op] || w->geometry[op].animated) continue;
    const uint64_t calls = glm.calls;
    CHECK(glr_warm_geometry(r, host, op) == GLR_OK && glm.calls > calls, "warming geometry %u uploads it", op);
    const uint64_t after = glm.calls;
    CHECK(glr_warm_geometry(r, host, op) == GLR_OK && glm.calls == after, "warming geometry %u again is a hit", op);
    break;
  }
  for (uint32_t op = 0; op < w->kx->n_ops; op++) {
    if (!w->have[op] || !w->geometry[op].animated) continue;
    const uint64_t calls = glm.calls;
    CHECK(glr_warm_geometry(r, host, op) == GLR_OK && glm.calls == calls, "an animated geometry (%u) needs no warm-up", op);
    break;
  }
  {
    const wz3_bitmap *bm = NULL;
    uint32_t texture_ops[2] = { w->kx->n_ops, w->kx->n_ops };
    for (uint32_t op = 0; op < w->kx->n_ops && texture_ops[1] == w->kx->n_ops; op++) {
      const wz3_material *m = wz3_rt_material(w->rt, op);
      if (!m || m->textures[0] < 0 || wz3_graph_render_dependent(w->graph, (uint32_t)m->textures[0])) continue;
      const uint32_t t = (uint32_t)m->textures[0];
      if (t == texture_ops[0]) continue;
      texture_ops[texture_ops[0] == w->kx->n_ops ? 0 : 1] = t;
    }
    CHECK(texture_ops[1] < w->kx->n_ops && wz3_graph_eval_bitmap(w->graph, texture_ops[0], &bm) == WZ3_OK && bm &&
            wz3_graph_eval_bitmap(w->graph, texture_ops[1], &bm) == WZ3_OK && bm,
          "two textures to warm");
    if (texture_ops[1] < w->kx->n_ops) {
      const uint64_t calls = glm.calls;
      CHECK(glr_warm_texture(r, host, texture_ops[0]) == GLR_OK && glm.calls > calls, "warming texture %u uploads it", texture_ops[0]);
      const uint64_t after = glm.calls;
      CHECK(glr_warm_texture(r, host, texture_ops[0]) == GLR_OK && glm.calls == after, "warming texture %u again is a hit", texture_ops[0]);
      /* a host that takes its pixels back after the upload is told once and never asked again: the texture is final */
      glr_host releasing = *host;
      g_inner = host;
      g_asked = g_given_back = 0;
      releasing.bitmap = counting_bitmap;
      releasing.bitmap_uploaded = counting_uploaded;
      CHECK(glr_warm_texture(r, &releasing, texture_ops[1]) == GLR_OK && g_asked == 1 && g_given_back == 1, "warm with release: asked %u, given back %u",
            g_asked, g_given_back);
      const uint64_t hit = glm.calls;
      CHECK(glr_warm_texture(r, &releasing, texture_ops[1]) == GLR_OK && g_asked == 1 && g_given_back == 1 && glm.calls == hit,
            "a released texture is a hit without asking");
    }
  }
  CHECK(wz3_minmesh_retain(NULL) == NULL, "retain of nothing");
  wz3_minmesh_release(NULL);
  /* the retained copy of an animated MinMesh poses like the geometry the sink made */
  for (uint32_t op = 0; op < w->kx->n_ops; op++) {
    if (!w->retained[op]) continue;
    wz3_geometry posed;
    CHECK(wz3_rt_minmesh_pose_geometry(w->rt, w->retained[op], &w->geometry[op], 0.5, &posed) == WZ3_OK, "pose of the retained mesh %u", op);
    wz3_geometry_free(&posed);
    break;
  }
}

/* One run: the document (patched), a runtime with its geometry sinks, a Renderer, the listed frames. scan_step > 0 renders without comparing. */
static int run_scenario(const scenario *sc, const uint8_t *kx_bytes, size_t kx_size, long scan_step, long scan_from, long scan_to)
{
  wz3_kx *kx = NULL;
  size_t at;
  CHECK(wz3_kx_parse(&kx, kx_bytes, kx_size, &at) == WZ3_OK, "parse");
  if (!kx) return 0;
  for (unsigned i = 0; i < sc->n_patches; i++) {
    const patch *p = &sc->patches[i];
    if (p->class_id >= 0) {
      unsigned touched = 0;
      for (uint32_t op = 0; op < kx->n_ops; op++) {
        if (kx->ops[op].class_id != (uint16_t)p->class_id) continue;
        CHECK(p->word < kx->classes[kx->ops[op].class_index].packing_length, "class patch %u", i);
        wz3_word *word = &((wz3_word *)kx->ops[op].params)[p->word];
        word->u = (word->u & p->and_mask) | p->bits;
        touched++;
      }
      CHECK(touched > 0, "class patch %u touches no operator", i);
      continue;
    }
    CHECK(p->op < kx->n_ops && p->word < kx->classes[kx->ops[p->op].class_index].packing_length, "patch %u", i);
    if (p->op >= kx->n_ops) continue;
    ((wz3_word *)kx->ops[p->op].params)[p->word].u = p->bits;
  }
  world w;
  if (!world_open(&w, kx, sc->bitmap_render)) return 0;
  const uint32_t n = kx->n_ops;
  wz3_result res = WZ3_OK;
  printf("glr frames [%s]: %u scene geometries prepared (%u animated, %u with a shadow recipe)\n", sc->name, w.prepared, w.animated, w.with_shadow_recipe);

  glr_host host;
  memset(&host, 0, sizeof host);
  host.user = &w;
  host.rt = w.rt;
  host.n_ops = n;
  host.geometry = host_geometry;
  host.shadow_data = host_shadow;
  host.bitmap = host_bitmap;
  w.host = &host;

  glm_reset();
  glm.hook = cover_hook;
  glm.hook_user = &g_cmp;
  glm.sink = g_dump;
  glr_options options;
  memset(&options, 0, sizeof options);
  options.no_shadows = !sc->shadows;
  options.s3tc = sc->s3tc;
  options.max_anisotropy = sc->anisotropy;
  char title[96];
  snprintf(title, sizeof title, "%s C", sc->name);
  if (glm.sink) fprintf(glm.sink, "# %s\n", title);
  cmp_seek(&g_cmp, title);
  glr_result gcreate;
  glr *r = glr_create(glm_loader, &options, &gcreate);
  CHECK(r && gcreate == GLR_OK, "glr_create");
  if (!r) return 0;
  w.renderer = r;
  cmp_end(&g_cmp, glm.calls);
  if (!scan_step) {
    CHECK(glm.calls == sc->constructor_calls && glm.hash == sc->constructor_hash, "[%s] constructor: %llu calls %08x, JavaScript %u calls %08x", sc->name,
          (unsigned long long)glm.calls, glm.hash, sc->constructor_calls, sc->constructor_hash);
  }

  if (sc->bitmap_render && !scan_step) {
    /* the precalculation of the JavaScript runtime draws the Render operators in operator order through the same renderer */
    snprintf(title, sizeof title, "%s R", sc->name);
    if (glm.sink) fprintf(glm.sink, "# %s\n", title);
    cmp_seek(&g_cmp, title);
    glm_restart_hash();
    unsigned rendered = 0;
    for (uint32_t op = 0; op < n; op++) {
      if (kx->ops[op].class_id != 0x3f) continue;
      const wz3_bitmap *bitmap = NULL;
      CHECK(wz3_graph_eval_bitmap(w.graph, op, &bitmap) == WZ3_OK && bitmap && bitmap->width == 128, "[%s] Render operator %u", sc->name, op);
      rendered++;
    }
    cmp_end(&g_cmp, glm.calls);
    CHECK(rendered == 3 && glm.calls == sc->render_calls && glm.hash == sc->render_hash, "[%s] %u Render operators: %llu calls %08x, JavaScript %u calls %08x",
          sc->name, rendered, (unsigned long long)glm.calls, glm.hash, sc->render_calls, sc->render_hash);
  }

  if (scan_step > 0) { /* development aid: render, do not compare (coverage scans) */
    glm.hook = NULL;
    for (long sample = scan_from; sample <= scan_to; sample += scan_step) {
      res = wz3_rt_frame_at_sample(w.rt, (int32_t)sample, 44100);
      if (res != WZ3_OK) {
        printf("frame %ld: %s\n", sample, wz3_result_string(res));
        break;
      }
      glr_resize(r, 640, 360, 2.0);
      glr_render_frame(r, &host);
    }
  } else {
    for (unsigned i = 0; i < sc->n_frames && (g_max_frames < 0 || (long)i < g_max_frames); i++) {
      const frame_expect *e = &sc->frames[i];
      res = wz3_rt_frame_at_sample(w.rt, (int32_t)e->sample, 44100);
      CHECK(res == WZ3_OK, "[%s] frame %ld: %s", sc->name, e->sample, wz3_result_string(res));
      if (res != WZ3_OK) break;
      snprintf(title, sizeof title, "%s F %ld", sc->name, e->sample);
      if (glm.sink) fprintf(glm.sink, "# %s\n", title);
      cmp_seek(&g_cmp, title);
      glm_restart_hash();
      glr_result gr = glr_resize(r, 640, 360, 2.0);
      if (gr == GLR_OK) gr = glr_render_frame(r, &host);
      CHECK(gr == GLR_OK, "[%s] frame %ld: glr result %d (%s)", sc->name, e->sample, (int)gr, glr_last_error(r));
      cmp_end(&g_cmp, glm.calls);
      glr_stats st;
      glr_get_stats(r, &st);
      g_compared++;
      g_total_calls += glm.calls;
      if (glm.calls != e->calls || glm.hash != e->hash || st.draw_calls != e->draws || st.triangles != e->triangles) {
        g_mismatches++;
        CHECK(0, "[%s] frame %ld: %llu calls %08x %u draws %u triangles, JavaScript %u calls %08x %u draws %u triangles", sc->name, e->sample,
              (unsigned long long)glm.calls, glm.hash, st.draw_calls, st.triangles, e->calls, e->hash, e->draws, e->triangles);
      }
    }
  }
  glm.sink = NULL;
  glm.hook = NULL;
  w.renderer = NULL;
  glr_destroy(r);
  CHECK(glm.live_objects == 0, "[%s] %ld GL objects leaked after glr_destroy", sc->name, glm.live_objects);
  if (!scan_step && !strcmp(sc->name, "main")) {
    glr *again = glr_create(glm_loader, &options, &gcreate);
    if (again) {
      check_contracts(&w, again, &host);
      glr_destroy(again);
    }
    glm_reset();
  }
  if (!scan_step && g_sweep && (!strcmp(sc->name, "main") || sc->bitmap_render)) {
    sweep_allocations(&w, &host, &options, sc->frames[0].sample, sc->bitmap_render, sc->name);
  }
  glm_reset();
  world_free(&w, n);
  wz3_kx_destroy(kx);
  return 1;
}

static scenario *g_scenarios;
static unsigned g_n_scenarios;

static void parse_fixture(char *fixture)
{
  g_scenarios = (scenario *)calloc(16, sizeof *g_scenarios);
  for (char *line = strtok(fixture, "\n"); line; line = strtok(NULL, "\n")) {
    char name[32];
    unsigned shadows, s3tc, calls, hash_value, draws, tris, op, word, bits, and_mask;
    double anisotropy;
    long sample;
    scenario *sc = g_n_scenarios ? &g_scenarios[g_n_scenarios - 1] : NULL;
    if (sscanf(line, "S %31s %u %u %lf", name, &shadows, &s3tc, &anisotropy) == 4 && line[0] == 'S' && g_n_scenarios < 16) {
      sc = &g_scenarios[g_n_scenarios++];
      snprintf(sc->name, sizeof sc->name, "%s", name);
      sc->shadows = (int)shadows;
      sc->s3tc = (int)s3tc;
      sc->anisotropy = anisotropy;
    } else if (sc && line[0] == 'P' && sscanf(line, "P %u %u %x", &op, &word, &bits) == 3 && sc->n_patches < 32) {
      patch *p = &sc->patches[sc->n_patches++];
      p->class_id = -1;
      p->op = op;
      p->word = word;
      p->bits = bits;
    } else if (sc && line[0] == 'Q' && sscanf(line, "Q %u %u %x %x", &op, &word, &and_mask, &bits) == 4 && sc->n_patches < 32) {
      patch *p = &sc->patches[sc->n_patches++];
      p->class_id = (int32_t)op;
      p->word = word;
      p->and_mask = and_mask;
      p->bits = bits;
    } else if (sc && line[0] == 'C' && sscanf(line, "C %u %x", &calls, &hash_value) == 2) {
      sc->constructor_calls = calls;
      sc->constructor_hash = hash_value;
    } else if (sc && line[0] == 'R' && sscanf(line, "R %u %x", &calls, &hash_value) == 2) {
      sc->bitmap_render = 1;
      sc->render_calls = calls;
      sc->render_hash = hash_value;
    } else if (sc && line[0] == 'F' && sscanf(line, "F %ld %u %x %u %u", &sample, &calls, &hash_value, &draws, &tris) == 5 && sc->n_frames < 64) {
      frame_expect *e = &sc->frames[sc->n_frames++];
      e->sample = sample;
      e->calls = calls;
      e->hash = hash_value;
      e->draws = draws;
      e->triangles = tris;
    }
  }
}

int suite_glr_frames(int argc, char **argv)
{
  if (argc < 3) {
    printf("usage: test_glr_frames <assets dir> <fixtures dir> [--only scenario] [--no-sweep] [--frames N] [--dump file] [--trace js-trace-file]\n"
           "       [--scan step from to [--patch op word hex]... [--s3tc]]\n");
    return 2;
  }
  long scan_step = 0, scan_from = 0, scan_to = 18400000;
  const char *dump_file = NULL, *trace_file = NULL, *only = NULL;
  scenario scan_scenario;
  memset(&scan_scenario, 0, sizeof scan_scenario);
  snprintf(scan_scenario.name, sizeof scan_scenario.name, "scan");
  scan_scenario.shadows = 1;
  for (int i = 3; i < argc; i++) {
    if (!strcmp(argv[i], "--frames") && i + 1 < argc)
      g_max_frames = atol(argv[++i]);
    else if (!strcmp(argv[i], "--dump") && i + 1 < argc)
      dump_file = argv[++i];
    else if (!strcmp(argv[i], "--trace") && i + 1 < argc)
      trace_file = argv[++i];
    else if (!strcmp(argv[i], "--only") && i + 1 < argc)
      only = argv[++i];
    else if (!strcmp(argv[i], "--no-sweep"))
      g_sweep = 0;
    else if (!strcmp(argv[i], "--sweep-point") && i + 1 < argc)
      g_sweep_point = atol(argv[++i]);
    else if (!strcmp(argv[i], "--scan") && i + 3 < argc) {
      scan_step = atol(argv[++i]);
      scan_from = atol(argv[++i]);
      scan_to = atol(argv[++i]);
    } else if (!strcmp(argv[i], "--s3tc")) {
      scan_scenario.s3tc = 1;
      scan_scenario.shadows = 0;
      scan_scenario.anisotropy = 16.0;
    } else if (!strcmp(argv[i], "--patch") && i + 3 < argc && scan_scenario.n_patches < 32) {
      patch *p = &scan_scenario.patches[scan_scenario.n_patches++];
      p->class_id = -1;
      p->op = (uint32_t)atol(argv[++i]);
      p->word = (uint32_t)atol(argv[++i]);
      p->bits = (uint32_t)strtoul(argv[++i], NULL, 16);
    } else if (!strcmp(argv[i], "--qpatch") && i + 4 < argc && scan_scenario.n_patches < 32) {
      patch *p = &scan_scenario.patches[scan_scenario.n_patches++];
      p->class_id = atoi(argv[++i]);
      p->word = (uint32_t)atol(argv[++i]);
      p->and_mask = (uint32_t)strtoul(argv[++i], NULL, 16);
      p->bits = (uint32_t)strtoul(argv[++i], NULL, 16);
    } else {
      printf("unknown argument %s\n", argv[i]);
      return 2;
    }
  }
  size_t kx_size;
  uint8_t *kx_bytes = gate_read_all(argv[1], "debris_party.kx", &kx_size);
  if (trace_file) {
    g_cmp.f = fopen(trace_file, "rb");
    if (!g_cmp.f) {
      printf("cannot open %s\n", trace_file);
      return 2;
    }
  }
  if (dump_file) {
    g_dump = fopen(dump_file, "wb");
    if (!g_dump) {
      printf("cannot write %s\n", dump_file);
      return 2;
    }
  }

  if (scan_step > 0) {
    run_scenario(&scan_scenario, kx_bytes, kx_size, scan_step, scan_from, scan_to);
    free(kx_bytes);
    return g_failures ? 1 : 0;
  }
  char *fixture = (char *)gate_read_all(argv[2], "glr_frames.txt", NULL);
  parse_fixture(fixture);
  CHECK(g_n_scenarios >= 10, "fixture lists %u scenarios", g_n_scenarios);
  unsigned run = 0;
  for (unsigned i = 0; i < g_n_scenarios; i++) {
    if (only && strcmp(only, g_scenarios[i].name)) continue;
    CHECK(g_scenarios[i].n_frames > 0 && g_scenarios[i].constructor_calls > 0, "scenario %s is empty", g_scenarios[i].name);
    run += run_scenario(&g_scenarios[i], kx_bytes, kx_size, 0, 0, 0);
  }
  if (g_dump) fclose(g_dump);
  if (g_cmp.f) fclose(g_cmp.f);
  printf("glr frames: %u scenarios, %u frames compared (%llu GL calls), %u differ\n", run, g_compared, (unsigned long long)g_total_calls, g_mismatches);
  printf("glr frames: coverage: %llu stencil ops, %llu instanced draws, %llu indexed draws, %llu array draws, %llu blits, %llu bufferData, %llu bufferSubData, "
         "%llu texImage2D, %llu compressedTexImage2D, %llu texParameter, %llu mipmaps, %llu uniforms, %llu attachments\n",
         (unsigned long long)g_cover.stencil_ops, (unsigned long long)g_cover.instanced, (unsigned long long)g_cover.indexed,
         (unsigned long long)g_cover.arrays, (unsigned long long)g_cover.blit, (unsigned long long)g_cover.buffer_data, (unsigned long long)g_cover.buffer_sub,
         (unsigned long long)g_cover.tex_image, (unsigned long long)g_cover.compressed, (unsigned long long)g_cover.tex_param,
         (unsigned long long)g_cover.generate_mipmap, (unsigned long long)g_cover.uniforms, (unsigned long long)g_cover.targets);
  if (g_max_frames < 0 && !only) {
    unsigned expected = 0;
    for (unsigned i = 0; i < g_n_scenarios; i++) expected += g_scenarios[i].n_frames;
    CHECK(g_compared == expected, "%u of %u frames compared", g_compared, expected);
    CHECK(g_cover.stencil_ops > 1000 && g_cover.instanced > 10000 && g_cover.indexed > 100 && g_cover.blit > 10 && g_cover.buffer_sub > 1000 &&
            g_cover.tex_image > 1000 && g_cover.compressed > 0 && g_cover.generate_mipmap > 0 && g_cover.uniforms > 500000 && g_cover.arrays > 100,
          "coverage of the frames is too thin");
  }
  free(g_scenarios);
  free(kx_bytes);
  free(fixture);
  printf("glr frames: %d checks, %d failures\n", g_checks, g_failures);
  return g_failures ? 1 : 0;
}
