/* Bitmap_Render boundary check for wz3.h against src/bitmap.js and src/app.js.
 *
 *   test_wz3_render_boundary <assets dir> <fixtures dir>
 *
 * Replays tools/dump_render_boundary.mjs: the three Render operators of the Debris document and everything that consumes them as the
 * JavaScript port evaluates them without a renderer (a blank bitmap of the authored size) and with a renderer hook (the readback of
 * the rendered scene, flipped and converted like makeBitmapRenderer), random hook calls on random size exponents, and the size rule.
 * It also checks the host contract: what a graph without a hook does, a hook that fails or returns nothing, the dependency query and
 * allocation failures anywhere in these paths.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
#include "support/gate_common.h"

/* the bytes of the pretend glReadPixels of tools/dump_render_boundary.mjs */
static void pixel_fill(uint32_t seed, uint8_t *bytes, size_t n)
{
  uint32_t s = seed;
  for (size_t i = 0; i < n; i++) {
    s = s * 1664525u + 1013904223u;
    bytes[i] = (uint8_t)(s >> 24);
  }
}

static wz3_bitmap *rendered_scene(uint32_t seed, int32_t width, int32_t height)
{
  const size_t n = (size_t)width * (size_t)height * 4u;
  uint8_t *buffer = (uint8_t *)malloc(n);
  if (!buffer) return NULL;
  pixel_fill(seed, buffer, n);
  wz3_bitmap *b = wz3_bitmap_from_rgba8(width, height, buffer, width, 1);
  free(buffer);
  return b;
}

typedef enum hook_mode { HOOK_RENDER = 0, HOOK_UNSUPPORTED, HOOK_FAIL_WITH_BITMAP, HOOK_NOTHING } hook_mode;
typedef struct hook {
  const wz3_kx *kx;
  hook_mode mode;
  unsigned calls;
  uint32_t last_op;
} hook;

static wz3_result render_callback(void *user, const wz3_bitmap_render_request *rq, wz3_bitmap **out)
{
  hook *h = (hook *)user;
  h->calls++;
  h->last_op = rq->op;
  CHECK(rq->op < h->kx->n_ops && h->kx->ops[rq->op].class_id == 0x3f, "the request names a Render operator");
  CHECK(rq->has_input && rq->input < rq->op && rq->op < h->kx->n_ops && h->kx->ops[rq->op].n_inputs > 0 && rq->input == h->kx->ops[rq->op].inputs[0],
        "a Render operator has an input operator, its first");
  if (rq->has_input && rq->input < h->kx->n_ops) {
    const wz3_class_info *info = wz3_class_lookup(h->kx->ops[rq->input].class_id);
    CHECK(info && info->output_class != WZ3_KC_BITMAP, "the input of Render is a scene, not a bitmap (class %u)", h->kx->ops[rq->input].class_id);
  }
  switch (h->mode) {
  case HOOK_UNSUPPORTED: return WZ3_ERR_UNSUPPORTED;
  case HOOK_FAIL_WITH_BITMAP: *out = wz3_bitmap_create(rq->width, rq->height); return WZ3_ERR_OPERATOR;
  case HOOK_NOTHING: *out = NULL; return WZ3_OK;
  default: break;
  }
  *out = rendered_scene((uint32_t)(rq->op + 1u) * 2654435761u, rq->width, rq->height);
  return *out ? WZ3_OK : WZ3_ERR_OUT_OF_MEMORY;
}

static wz3_graph *make_graph(const wz3_kx *kx, int32_t offset, wz3_bitmap_render_fn render, void *user, int placeholder)
{
  wz3_graph_options o;
  memset(&o, 0, sizeof o);
  wz3_bitmap_ctx_init(&o.ctx);
  o.ctx.texture_offset = offset;
  o.render = render;
  o.render_user = user;
  o.render_placeholder = placeholder;
  wz3_result res;
  wz3_graph *g = wz3_graph_create(kx, &o, &res);
  CHECK(g && res == WZ3_OK, "graph create: %s", wz3_result_string(res));
  return g;
}

/* the Render operators and the first operator of each dependent chain, as the fixture lists them */
typedef struct want {
  uint32_t op, cls, w, h, fmt;
  unsigned long hash;
} want;

static unsigned read_want(char *text, const char *tag, want *out, unsigned max)
{
  unsigned n = 0;
  const size_t taglen = strlen(tag);
  for (char *line = text; *line && n < max;) {
    char *nl = strchr(line, '\n');
    if (nl) *nl = 0;
    if (!strncmp(line, tag, taglen) && line[taglen] == ' ') {
      want e;
      if (sscanf(line + taglen, "%u %u %u %u %u %lu", &e.op, &e.cls, &e.w, &e.h, &e.fmt, &e.hash) == 6) out[n++] = e;
    }
    if (!nl) break;
    *nl = '\n';
    line = nl + 1;
  }
  return n;
}

static void compare_chain(wz3_graph *g, const want *list, unsigned n, const char *what)
{
  for (unsigned i = 0; i < n; i++) {
    const wz3_bitmap *bm = NULL;
    const wz3_result r = wz3_graph_eval_bitmap(g, list[i].op, &bm);
    CHECK(r == WZ3_OK && bm, "%s: op %u: %s", what, list[i].op, wz3_result_string(r));
    if (r != WZ3_OK || !bm) continue;
    CHECK((uint32_t)bm->width == list[i].w && (uint32_t)bm->height == list[i].h && (uint32_t)bm->format == list[i].fmt &&
            wz3_bitmap_hash(bm) == (uint32_t)list[i].hash,
          "%s: op %u (class %u): %dx%d format %d hash %u, JavaScript %ux%u format %u hash %lu", what, list[i].op, list[i].cls, bm->width, bm->height,
          bm->format, wz3_bitmap_hash(bm), list[i].w, list[i].h, list[i].fmt, list[i].hash);
  }
}

static void check_dependents(const wz3_kx *kx, wz3_graph *g, const char *dline)
{
  unsigned count = 0, expected_count = 0;
  unsigned long expected_hash = 0;
  CHECK(sscanf(dline, "D %u %lx", &expected_count, &expected_hash) == 2, "D line");
  hash h;
  hash_init(&h);
  for (uint32_t i = 0; i < kx->n_ops; i++)
    if (wz3_graph_render_dependent(g, i)) {
      hu(&h, i);
      count++;
    }
  CHECK(count == expected_count && h.h == (uint32_t)expected_hash, "render dependents: %u operators hash %x, JavaScript %u operators hash %lx", count, h.h,
        expected_count, expected_hash);
  CHECK(!wz3_graph_render_dependent(g, kx->n_ops) && !wz3_graph_render_dependent(NULL, 0), "invalid arguments are not dependents");
}

static void production(const wz3_kx *kx, char *text, int32_t offset, const char *label)
{
  want placeholder[16], hooked[16];
  const unsigned n_placeholder = read_want(text, "R", placeholder, 16), n_hooked = read_want(text, "Rh", hooked, 16);
  CHECK(n_placeholder == 5 && n_hooked == 5, "%s: %u placeholder and %u hook lines", label, n_placeholder, n_hooked);
  const char *dline = strstr(text, "\nD ");
  CHECK(dline != NULL, "%s: no D line", label);

  /* without a hook: unsupported, as before the boundary existed */
  wz3_graph *g = make_graph(kx, offset, NULL, NULL, 0);
  for (unsigned i = 0; g && i < n_placeholder; i++) {
    const wz3_bitmap *bm = (const wz3_bitmap *)(uintptr_t)1;
    const wz3_result r = wz3_graph_eval_bitmap(g, placeholder[i].op, &bm);
    CHECK(r == WZ3_ERR_UNSUPPORTED && bm == NULL, "%s: op %u without a renderer: %s", label, placeholder[i].op, wz3_result_string(r));
  }
  if (g && dline) check_dependents(kx, g, dline + 1);
  wz3_graph_destroy(g);

  /* the placeholder of the JavaScript port without a hook */
  g = make_graph(kx, offset, NULL, NULL, 1);
  if (g) compare_chain(g, placeholder, n_placeholder, label);
  wz3_graph_destroy(g);

  /* a renderer hook */
  hook h;
  memset(&h, 0, sizeof h);
  h.kx = kx;
  g = make_graph(kx, offset, render_callback, &h, 0);
  if (g) compare_chain(g, hooked, n_hooked, label);
  CHECK(h.calls == 3, "%s: the hook ran %u times for three Render operators", label, h.calls);
  wz3_graph_destroy(g);

  /* a hook that declines falls back to the placeholder when asked to, and fails otherwise */
  h.mode = HOOK_UNSUPPORTED;
  h.calls = 0;
  g = make_graph(kx, offset, render_callback, &h, 1);
  if (g) compare_chain(g, placeholder, n_placeholder, label);
  CHECK(h.calls == 3, "%s: declining hook called %u times", label, h.calls);
  wz3_graph_destroy(g);
  g = make_graph(kx, offset, render_callback, &h, 0);
  if (g) {
    const wz3_bitmap *bm = NULL;
    CHECK(wz3_graph_eval_bitmap(g, placeholder[0].op, &bm) == WZ3_ERR_UNSUPPORTED && !bm, "%s: a declining hook without placeholder is unsupported", label);
    CHECK(wz3_graph_eval_bitmap(g, 11944, &bm) == WZ3_ERR_UNSUPPORTED, "%s: its dependents are too", label);
  }
  wz3_graph_destroy(g);

  /* a hook that fails leaves nothing behind (the bitmap it allocated is the library's to free) */
  h.mode = HOOK_FAIL_WITH_BITMAP;
  const long live = g_live;
  g = make_graph(kx, offset, render_callback, &h, 1);
  if (g) {
    const wz3_bitmap *bm = NULL;
    CHECK(wz3_graph_eval_bitmap(g, placeholder[0].op, &bm) == WZ3_ERR_OPERATOR && !bm, "%s: a failing hook fails the operator, not the placeholder", label);
    CHECK(wz3_graph_eval_bitmap(g, 11944, &bm) == WZ3_ERR_OPERATOR && !bm, "%s: and its consumers", label);
  }
  wz3_graph_destroy(g);
  CHECK(g_live == live, "%s: a failing hook leaked %ld allocations", label, g_live - live);
  h.mode = HOOK_NOTHING;
  g = make_graph(kx, offset, render_callback, &h, 1);
  if (g) {
    const wz3_bitmap *bm = NULL;
    CHECK(wz3_graph_eval_bitmap(g, placeholder[0].op, &bm) == WZ3_ERR_OPERATOR && !bm, "%s: a hook that renders nothing fails the operator", label);
  }
  wz3_graph_destroy(g);
  CHECK(g_live == live, "%s: leaked %ld allocations", label, g_live - live);

  /* release_consumed: the Render result is freed after its last consumer, like any bitmap */
  {
    wz3_graph_options o;
    memset(&o, 0, sizeof o);
    wz3_bitmap_ctx_init(&o.ctx);
    o.ctx.texture_offset = offset;
    o.release_consumed = 1;
    o.render_placeholder = 1;
    wz3_result res;
    wz3_graph *rg = wz3_graph_create(kx, &o, &res);
    const wz3_bitmap *a = NULL, *b = NULL;
    CHECK(rg && wz3_graph_eval_bitmap(rg, 11944, &a) == WZ3_OK && a, "release: 11944");
    if (rg && a) {
      const uint32_t before = wz3_bitmap_hash(a);
      CHECK(wz3_graph_eval_bitmap(rg, 11943, &b) == WZ3_OK && b && wz3_bitmap_hash(b) == (uint32_t)placeholder[0].hash,
            "release: the freed Render result is evaluated again");
      wz3_graph_release(rg, 11944);
      CHECK(before == (uint32_t)placeholder[1].hash, "release: 11944 hash");
    }
    wz3_graph_destroy(rg);
  }
}

/* allocation failures in the graph with a hook and with the placeholder */
static void alloc_sweep(const wz3_kx *kx)
{
  unsigned failures = 0, successes = 0;
  for (int variant = 0; variant < 2; variant++)
    for (long k = 0; k < 400; k++) {
      hook h;
      memset(&h, 0, sizeof h);
      h.kx = kx;
      const long live = g_live;
      g_alloc_count = 0;
      g_fail_at = k;
      wz3_graph_options o;
      memset(&o, 0, sizeof o);
      wz3_bitmap_ctx_init(&o.ctx);
      o.ctx.texture_offset = -2;
      if (variant == 0) {
        o.render = render_callback;
        o.render_user = &h;
      } else
        o.render_placeholder = 1;
      wz3_result res;
      wz3_graph *g = wz3_graph_create(kx, &o, &res);
      wz3_result r = res;
      const wz3_bitmap *bm = NULL;
      if (g) r = wz3_graph_eval_bitmap(g, 11944, &bm);
      const int hit = g_alloc_count > k; /* the failure was injected somewhere in this run */
      g_fail_at = -1;
      wz3_graph_destroy(g);
      CHECK(g_live == live, "variant %d fail at %ld: leaked %ld allocations", variant, k, g_live - live);
      if (!hit) {
        CHECK(r == WZ3_OK && bm, "variant %d: the run that never failed must succeed: %s", variant, wz3_result_string(r));
        successes++;
        break;
      }
      CHECK(r == WZ3_ERR_OUT_OF_MEMORY || r == WZ3_ERR_OPERATOR, "variant %d fail at %ld: %s", variant, k, wz3_result_string(r));
      failures++;
    }
  CHECK(successes == 2 && failures > 20, "allocation sweep: %u injected failures, %u clean runs", failures, successes);
  printf("render boundary: allocation sweep: %u injected failures survived\n", failures);
}

static unsigned random_cases(char *text, unsigned *dims)
{
  unsigned x_lines = 0;
  for (char *line = text; *line;) {
    char *nl = strchr(line, '\n');
    if (nl) *nl = 0;
    unsigned xs, ys, off, seed, w, h;
    unsigned long hash_value;
    if (!strncmp(line, "X ", 2) && sscanf(line + 2, "%x %x %x %x = %x %x %lu", &xs, &ys, &off, &seed, &w, &h, &hash_value) == 7) {
      wz3_bitmap_ctx ctx;
      wz3_bitmap_ctx_init(&ctx);
      ctx.texture_offset = (int32_t)off;
      int32_t cw, ch;
      wz3__bitmap_dims(&ctx, xs, ys, &cw, &ch);
      CHECK((unsigned)cw == w && (unsigned)ch == h, "line '%s': size %dx%d", line, cw, ch);
      wz3_bitmap *b = rendered_scene(seed, cw, ch);
      CHECK(b && wz3_bitmap_hash(b) == (uint32_t)hash_value, "line '%s': hash %u", line, b ? wz3_bitmap_hash(b) : 0u);
      wz3_bitmap_destroy(b);
      x_lines++;
    } else if (!strncmp(line, "Z ", 2) && sscanf(line + 2, "%x %x %x = %x %x", &xs, &ys, &off, &w, &h) == 5) {
      wz3_bitmap_ctx ctx;
      wz3_bitmap_ctx_init(&ctx);
      ctx.texture_offset = (int32_t)off;
      int32_t cw, ch;
      wz3__bitmap_dims(&ctx, xs, ys, &cw, &ch);
      CHECK((unsigned)cw == w && (unsigned)ch == h, "line '%s': size %dx%d", line, cw, ch);
      (*dims)++;
    }
    if (!nl) break;
    line = nl + 1;
  }
  return x_lines;
}

static void test_from_rgba8(void)
{
  /* orientation, stride and argument checks of the readback conversion */
  uint8_t px[2 * 2 * 4];
  for (int i = 0; i < 16; i++) px[i] = (uint8_t)(i * 16 + 1);
  wz3_bitmap *top = wz3_bitmap_from_rgba8(2, 2, px, 2, 0), *bottom = wz3_bitmap_from_rgba8(2, 2, px, 2, 1);
  CHECK(top && bottom, "from_rgba8 allocates");
  if (top && bottom) {
    uint16_t expect[4];
    wz3_packed_color(((uint32_t)px[3] << 24) | ((uint32_t)px[0] << 16) | ((uint32_t)px[1] << 8) | px[2], expect);
    CHECK(!memcmp(top->data, expect, sizeof expect), "top-down: the first pixel is the first source pixel");
    wz3_packed_color(((uint32_t)px[11] << 24) | ((uint32_t)px[8] << 16) | ((uint32_t)px[9] << 8) | px[10], expect);
    CHECK(!memcmp(bottom->data, expect, sizeof expect), "bottom-up: the first row is the last source row");
    CHECK(top->format == 1 && bottom->format == 1 && top->width == 2 && bottom->height == 2, "from_rgba8 makes a format 1 bitmap of the given size");
  }
  wz3_bitmap_destroy(top);
  wz3_bitmap_destroy(bottom);
  /* a stride wider than the bitmap skips the padding pixels */
  uint8_t wide[4 * 2 * 4];
  for (int i = 0; i < 32; i++) wide[i] = (uint8_t)(i + 7);
  wz3_bitmap *w = wz3_bitmap_from_rgba8(2, 2, wide, 4, 0);
  if (w) {
    uint16_t expect[4];
    wz3_packed_color(((uint32_t)wide[19] << 24) | ((uint32_t)wide[16] << 16) | ((uint32_t)wide[17] << 8) | wide[18], expect);
    CHECK(!memcmp(w->data + 8, expect, sizeof expect), "stride: the second row starts after the padding");
  }
  wz3_bitmap_destroy(w);
  CHECK(!wz3_bitmap_from_rgba8(2, 2, NULL, 2, 0) && !wz3_bitmap_from_rgba8(0, 2, px, 2, 0) && !wz3_bitmap_from_rgba8(2, 0, px, 2, 0) &&
          !wz3_bitmap_from_rgba8(2, 2, px, 1, 0),
        "invalid arguments");
}

int suite_wz3_render_boundary(int argc, char **argv)
{
  if (argc < 3) {
    printf("usage: test_wz3_render_boundary <assets dir> <fixtures dir>\n");
    return 2;
  }
  size_t kx_size;
  uint8_t *kx_bytes = gate_read_all(argv[1], "debris_party.kx", &kx_size);
  wz3_kx *kx = NULL;
  CHECK(wz3_kx_parse(&kx, kx_bytes, kx_size, NULL) == WZ3_OK, "parse");
  test_from_rgba8();
  if (kx) {
    char *m2 = (char *)gate_read_all(argv[2], "render_boundary_offset_m2.txt", NULL);
    production(kx, m2, -2, "offset -2");
    unsigned dims = 0;
    const unsigned x_lines = random_cases(m2, &dims);
    CHECK(x_lines >= 400 && dims >= 16, "%u random hook cases, %u size cases", x_lines, dims);
    free(m2);
#ifndef DEBRIS_TEST_SKIP_FULL_SIZE
    char *full = (char *)gate_read_all(argv[2], "render_boundary_offset_0.txt", NULL);
    production(kx, full, 0, "offset 0");
    free(full);
#endif
    alloc_sweep(kx);
    wz3_kx_destroy(kx);
    printf("render boundary: %d checks, %d failures\n", g_checks, g_failures);
  }
  free(kx_bytes);
  return g_failures ? 1 : 0;
}
