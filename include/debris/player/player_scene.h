/* player_scene.h: what the player keeps resident for the renderer, SDL-free so that it can be tested headless.
 * Include wz3.h, glr.h and player_load.h first.
 *
 *   #define PLAYER_SCENE_IMPLEMENTATION in exactly one source file.
 *
 * A pscene owns, for one document:
 *   - the geometry sinks (pgeometry feeds them during wz3_rt_precalc): the prepared geometry of the 574 operators a scene draws,
 *     their shadow data, and a retained copy of every animated MinMesh (posed per frame into per-slot scratch geometry);
 *   - the texture graph at --texture-quality with the real text rasterizer, and the Bitmap_Render hook that draws through glr;
 *   - the loading plan: the texture operators of the materials, evaluated on the precalculation worker (the CPU part, pure
 *     functions of the document), then the warm-up the GL thread runs in slices while the loader bar is shown (Render operators in
 *     operator order, the textures they feed, every GL texture and static geometry buffer), so that the first frame finds hits.
 * After an upload the pixels are given back (glr_host.bitmap_uploaded): the GPU copy is final.
 *
 * Threads: pscene_mesh / pscene_minmesh / pscene_bitmap and pscene_eval_textures run on the precalculation worker, everything else
 * on the GL thread after the worker was joined. The runtime is owned by the caller (attach it before the precalculation).
 */
#ifndef PLAYER_SCENE_H
#define PLAYER_SCENE_H

#include <stdint.h>

#ifndef PLAYER_SCENE_API
#ifdef PLAYER_SCENE_STATIC
#define PLAYER_SCENE_API static
#else
#define PLAYER_SCENE_API extern
#endif
#endif

typedef struct pscene_options {
  int texture_offset;   /* --texture-quality: 0 full size, -1 normal, -2 low */
  wz3_text_fn text;     /* NULL: wz3_text_default */
  void *text_user;
  int bitmap_render;    /* the Bitmap_Render operators are drawn by glr (pscene_set_renderer before the warm-up) */
  int release_uploaded; /* free a bitmap once glr uploaded it (the player does; a test that compares with the JS port keeps them) */
} pscene_options;

typedef enum pscene_item_kind { PSCENE_ITEM_RENDER = 1, PSCENE_ITEM_TEXTURE, PSCENE_ITEM_GEOMETRY } pscene_item_kind;
typedef struct pscene_item {
  uint8_t kind;
  uint32_t op;
} pscene_item;

typedef struct pscene {
  const wz3_kx *kx;
  wz3_rt *rt;
  wz3_graph *graph;
  glr *renderer;
  glr_host host;
  pgeometry geometry; /* the sink: statistics of what the precalculation prepared */
  int release_uploaded;
  uint32_t n_ops;
  uint8_t *have, *failed_bitmap;
  wz3_geometry *geo;
  wz3_shadow_data *shadow;
  wz3_minmesh **retained;
  wz3_geometry ***slots;
  uint32_t *n_slots;
  uint32_t animated, with_shadow_recipe;
  /* loading plan */
  uint32_t *textures;                 /* the bitmap operators the materials sample, ascending */
  uint32_t n_textures, n_independent; /* ... and how many of those do not depend on a Render operator */
  uint32_t evaluated;                 /* independent textures evaluated by pscene_eval_textures */
  pscene_item *items;
  uint32_t n_items, next_item;
  uint32_t released, bitmap_failures;
  uint64_t bitmap_requests; /* how often the host was asked for a bitmap (a released texture is never asked again) */
  int bitmap_oom;           /* the last bitmap evaluation ran out of memory (glr then draws the fallback; the warm-up retries) */
} pscene;

PLAYER_SCENE_API wz3_result pscene_create(pscene *ps, const wz3_kx *kx, const pscene_options *options);
PLAYER_SCENE_API void pscene_destroy(pscene *ps);
/* The runtime the shadow recipes and animated poses ask (wz3_rt_create(...) with the host of pscene_rt_host). Before the precalculation. */
PLAYER_SCENE_API void pscene_attach(pscene *ps, wz3_rt *rt);
/* wz3_rt_host.mesh / .minmesh / .bitmap with `user` the pscene; a caller with its own user forwards to pscene_mesh & co. */
PLAYER_SCENE_API wz3_result pscene_mesh(void *user, uint32_t op, const wz3_mesh *mesh);
PLAYER_SCENE_API wz3_result pscene_minmesh(void *user, uint32_t op, const wz3_minmesh *mesh);
PLAYER_SCENE_API const wz3_bitmap *pscene_bitmap(void *user, uint32_t op);
/* The host of glr_render_frame / glr_warm_*: valid as long as the pscene is. */
PLAYER_SCENE_API const glr_host *pscene_glr_host(pscene *ps);
PLAYER_SCENE_API void pscene_set_renderer(pscene *ps, glr *renderer);

/* After the precalculation: collects the texture operators, and builds the warm-up list. */
PLAYER_SCENE_API wz3_result pscene_plan(pscene *ps);
/* Evaluates the textures that do not need the renderer. progress(user, done, total) after each; cancelled(user) != 0 stops with
   WZ3_ERR_CANCELLED. An operator that cannot be evaluated is counted in bitmap_failures and draws as the fallback texture. */
PLAYER_SCENE_API wz3_result pscene_eval_textures(pscene *ps, void (*progress)(void *user, uint32_t done, uint32_t total), int (*cancelled)(void *user),
                                                 void *user);
/* One warm-up item on the GL thread (pscene_set_renderer first). *done / *total count the items; returns GLR_OK with *done == *total when
   everything is warm. An out-of-memory result leaves the item pending: the next call retries it. */
PLAYER_SCENE_API glr_result pscene_warm_step(pscene *ps, uint32_t *done, uint32_t *total);

#endif /* PLAYER_SCENE_H */

#ifdef PLAYER_SCENE_IMPLEMENTATION
#ifndef PLAYER_SCENE_IMPLEMENTATION_DONE
#define PLAYER_SCENE_IMPLEMENTATION_DONE

#include <stdlib.h>
#include <string.h>

#ifndef PLAYER_LOAD_MALLOC
#define PLAYER_LOAD_MALLOC(n) malloc(n)
#define PLAYER_LOAD_FREE(p)   free(p)
#endif

static void *pscene__zalloc(size_t n, size_t size)
{
  if (size && n > (size_t)-1 / size) return NULL;
  const size_t bytes = n * size;
  void *p = PLAYER_LOAD_MALLOC(bytes ? bytes : 1);
  if (p) memset(p, 0, bytes ? bytes : 1);
  return p;
}

static int pscene__casts_shadow(void *user, int32_t op)
{
  const wz3_material *m = wz3_rt_material((const wz3_rt *)user, (uint32_t)op);
  if (!m) return 0;
  for (uint32_t i = 0; i < m->n_passes; i++)
    if (m->passes[i].usage == WZ3_USAGE_SHADOW) return 1;
  return 0;
}

/* wz3_bitmap_render_fn: the scene of a Render operator drawn by glr (the renderer side of Bitmap_Render) */
static wz3_result pscene__render_hook(void *user, const wz3_bitmap_render_request *request, wz3_bitmap **out)
{
  pscene *ps = (pscene *)user;
  if (!ps->renderer || !request->has_input) return WZ3_ERR_UNSUPPORTED;
  const glr_result r = glr_render_bitmap(ps->renderer, &ps->host, request->input, request->width, request->height, out);
  return r == GLR_OK ? WZ3_OK : r == GLR_ERR_OUT_OF_MEMORY ? WZ3_ERR_OUT_OF_MEMORY : WZ3_ERR_OPERATOR;
}

static const wz3_geometry *pscene__geometry(void *user, uint32_t op, double time, int32_t slot)
{
  pscene *ps = (pscene *)user;
  if (op >= ps->n_ops || !ps->have[op]) return NULL;
  const wz3_geometry *base = &ps->geo[op];
  if (time < 0 || !base->animated || !ps->retained[op]) return base;
  if (slot < 0) return NULL;
  if ((uint32_t)slot >= ps->n_slots[op]) {
    const uint32_t n = (uint32_t)slot + 1;
    wz3_geometry **grown = (wz3_geometry **)pscene__zalloc(n, sizeof *grown);
    if (!grown) return NULL;
    if (ps->n_slots[op]) memcpy(grown, ps->slots[op], (size_t)ps->n_slots[op] * sizeof *grown);
    PLAYER_LOAD_FREE(ps->slots[op]);
    ps->slots[op] = grown;
    ps->n_slots[op] = n;
  }
  wz3_geometry **s = &ps->slots[op][slot];
  if (!*s)
    *s = (wz3_geometry *)pscene__zalloc(1, sizeof **s);
  else
    wz3_geometry_free(*s);
  if (!*s) return NULL;
  if (wz3_rt_minmesh_pose_geometry(ps->rt, ps->retained[op], base, time, *s) != WZ3_OK) return NULL;
  return *s;
}

static const wz3_shadow_data *pscene__shadow(void *user, uint32_t op)
{
  pscene *ps = (pscene *)user;
  return op < ps->n_ops && ps->have[op] ? &ps->shadow[op] : NULL;
}

static void pscene__uploaded(void *user, uint32_t op)
{
  pscene *ps = (pscene *)user;
  wz3_graph_release(ps->graph, op);
  ps->released++;
}

static void pscene__fill_host(pscene *ps)
{
  memset(&ps->host, 0, sizeof ps->host);
  ps->host.user = ps;
  ps->host.rt = ps->rt;
  ps->host.n_ops = ps->n_ops;
  ps->host.geometry = pscene__geometry;
  ps->host.shadow_data = pscene__shadow;
  ps->host.bitmap = pscene_bitmap;
  if (ps->release_uploaded) ps->host.bitmap_uploaded = pscene__uploaded;
}

/* the consumer of pgeometry: shadow data from the source, a retained copy of an animated MinMesh; owns `g` either way */
static wz3_result pscene__consume(void *user, uint32_t op, wz3_geometry *g, const wz3_mesh *mesh, const wz3_minmesh *minmesh)
{
  pscene *ps = (pscene *)user;
  if (!ps->rt || op >= ps->n_ops) {
    wz3_geometry_free(g);
    return WZ3_ERR_ARGUMENT;
  }
  wz3_shadow_data d;
  wz3_result r = mesh ? wz3_mesh_shadow_data(mesh, g, pscene__casts_shadow, ps->rt, &d) : wz3_minmesh_shadow_data(minmesh, g, pscene__casts_shadow, ps->rt, &d);
  if (r != WZ3_OK) {
    wz3_geometry_free(g);
    return r;
  }
  if (minmesh && g->animated) {
    ps->retained[op] = wz3_minmesh_retain(minmesh);
    if (!ps->retained[op]) {
      wz3_shadow_data_free(&d);
      wz3_geometry_free(g);
      return WZ3_ERR_OUT_OF_MEMORY;
    }
    ps->animated++;
  }
  ps->geo[op] = *g;
  ps->shadow[op] = d;
  ps->have[op] = 1;
  ps->with_shadow_recipe += d.recipe != NULL;
  return WZ3_OK;
}

PLAYER_SCENE_API void pscene_destroy(pscene *ps)
{
  if (!ps) return;
  for (uint32_t op = 0; op < ps->n_ops; op++) {
    if (ps->slots && ps->n_slots) {
      for (uint32_t s = 0; s < ps->n_slots[op]; s++) {
        if (ps->slots[op][s]) {
          wz3_geometry_free(ps->slots[op][s]);
          PLAYER_LOAD_FREE(ps->slots[op][s]);
        }
      }
      PLAYER_LOAD_FREE(ps->slots[op]);
    }
    if (ps->have && ps->have[op]) {
      wz3_geometry_free(&ps->geo[op]);
      wz3_shadow_data_free(&ps->shadow[op]);
    }
    if (ps->retained) wz3_minmesh_release(ps->retained[op]);
  }
  wz3_graph_destroy(ps->graph);
  pgeometry_destroy(&ps->geometry);
  PLAYER_LOAD_FREE(ps->have);
  PLAYER_LOAD_FREE(ps->failed_bitmap);
  PLAYER_LOAD_FREE(ps->geo);
  PLAYER_LOAD_FREE(ps->shadow);
  PLAYER_LOAD_FREE(ps->retained);
  PLAYER_LOAD_FREE(ps->slots);
  PLAYER_LOAD_FREE(ps->n_slots);
  PLAYER_LOAD_FREE(ps->textures);
  PLAYER_LOAD_FREE(ps->items);
  memset(ps, 0, sizeof *ps);
}

PLAYER_SCENE_API wz3_result pscene_create(pscene *ps, const wz3_kx *kx, const pscene_options *options)
{
  if (!ps || !kx || !options) return WZ3_ERR_ARGUMENT;
  memset(ps, 0, sizeof *ps);
  const uint32_t n = kx->n_ops;
  ps->kx = kx;
  ps->n_ops = n;
  ps->release_uploaded = options->release_uploaded;
  ps->have = (uint8_t *)pscene__zalloc(n, 1);
  ps->failed_bitmap = (uint8_t *)pscene__zalloc(n, 1);
  ps->geo = (wz3_geometry *)pscene__zalloc(n, sizeof *ps->geo);
  ps->shadow = (wz3_shadow_data *)pscene__zalloc(n, sizeof *ps->shadow);
  ps->retained = (wz3_minmesh **)pscene__zalloc(n, sizeof *ps->retained);
  ps->slots = (wz3_geometry ***)pscene__zalloc(n, sizeof *ps->slots);
  ps->n_slots = (uint32_t *)pscene__zalloc(n, sizeof *ps->n_slots);
  wz3_result res = (ps->have && ps->failed_bitmap && ps->geo && ps->shadow && ps->retained && ps->slots && ps->n_slots) ? WZ3_OK : WZ3_ERR_OUT_OF_MEMORY;
  if (res == WZ3_OK) res = pgeometry_init(&ps->geometry, kx, pscene__consume, ps);
  if (res == WZ3_OK) {
    wz3_graph_options go;
    ptexture_graph_options(&go, options->texture_offset);
    if (options->text) {
      go.ctx.text = options->text;
      go.ctx.text_user = options->text_user;
    }
    if (options->bitmap_render) {
      go.render = pscene__render_hook;
      go.render_user = ps;
    }
    ps->graph = wz3_graph_create(kx, &go, &res);
    if (!ps->graph && res == WZ3_OK) res = WZ3_ERR_OUT_OF_MEMORY;
  }
  if (res != WZ3_OK) {
    pscene_destroy(ps);
    return res;
  }
  pscene__fill_host(ps);
  return WZ3_OK;
}

PLAYER_SCENE_API void pscene_attach(pscene *ps, wz3_rt *rt)
{
  if (!ps) return;
  ps->rt = rt;
  ps->host.rt = rt;
}

PLAYER_SCENE_API wz3_result pscene_mesh(void *user, uint32_t op, const wz3_mesh *mesh) { return pgeometry_mesh(&((pscene *)user)->geometry, op, mesh); }
PLAYER_SCENE_API wz3_result pscene_minmesh(void *user, uint32_t op, const wz3_minmesh *mesh)
{
  return pgeometry_minmesh(&((pscene *)user)->geometry, op, mesh);
}

PLAYER_SCENE_API const wz3_bitmap *pscene_bitmap(void *user, uint32_t op)
{
  pscene *ps = (pscene *)user;
  ps->bitmap_requests++;
  if (op >= ps->n_ops || ps->failed_bitmap[op]) return NULL;
  const wz3_bitmap *bitmap = NULL;
  const wz3_result r = wz3_graph_eval_bitmap(ps->graph, op, &bitmap);
  if (r != WZ3_OK || !bitmap) {
    if (r == WZ3_ERR_OUT_OF_MEMORY)
      ps->bitmap_oom = 1;
    else {
      ps->failed_bitmap[op] = 1;
      ps->bitmap_failures++;
    } /* an allocation failure may succeed later */
    return NULL;
  }
  return bitmap;
}

PLAYER_SCENE_API const glr_host *pscene_glr_host(pscene *ps) { return ps ? &ps->host : NULL; }
PLAYER_SCENE_API void pscene_set_renderer(pscene *ps, glr *renderer)
{
  if (ps) ps->renderer = renderer;
}

PLAYER_SCENE_API wz3_result pscene_plan(pscene *ps)
{
  if (!ps || !ps->rt) return WZ3_ERR_ARGUMENT;
  const wz3_kx *kx = ps->kx;
  const uint32_t n = ps->n_ops;
  PLAYER_LOAD_FREE(ps->textures);
  PLAYER_LOAD_FREE(ps->items);
  ps->textures = NULL;
  ps->items = NULL;
  ps->n_textures = ps->n_independent = ps->evaluated = ps->n_items = ps->next_item = 0;
  uint8_t *mark = (uint8_t *)pscene__zalloc(n, 1);
  if (!mark) return WZ3_ERR_OUT_OF_MEMORY;
  uint32_t n_render = 0, n_geometry = 0;
  for (uint32_t op = 0; op < n; op++) {
    const wz3_material *m = wz3_rt_material(ps->rt, op);
    if (m) {
      for (int i = 0; i < 7; i++) {
        const int32_t t = m->textures[i];
        if (t >= 0 && (uint32_t)t < n && !mark[t]) {
          mark[t] = 1;
          ps->n_textures++;
        }
      }
    }
    if (kx->ops[op].class_id == 0x3f) n_render++;
    if (ps->have[op] && !ps->geo[op].animated) n_geometry++;
  }
  ps->textures = (uint32_t *)pscene__zalloc(ps->n_textures, sizeof *ps->textures);
  ps->items = (pscene_item *)pscene__zalloc((size_t)n_render + ps->n_textures + n_geometry, sizeof *ps->items);
  if (!ps->textures || !ps->items) {
    PLAYER_LOAD_FREE(mark);
    PLAYER_LOAD_FREE(ps->textures);
    PLAYER_LOAD_FREE(ps->items);
    ps->textures = NULL;
    ps->items = NULL;
    ps->n_textures = 0;
    return WZ3_ERR_OUT_OF_MEMORY;
  }
  uint32_t t = 0;
  for (uint32_t op = 0; op < n; op++) {
    if (kx->ops[op].class_id == 0x3f) {
      ps->items[ps->n_items].kind = PSCENE_ITEM_RENDER;
      ps->items[ps->n_items++].op = op;
    }
  }
  for (uint32_t op = 0; op < n; op++) {
    if (!mark[op]) continue;
    ps->textures[t++] = op;
    if (!wz3_graph_render_dependent(ps->graph, op)) ps->n_independent++;
    ps->items[ps->n_items].kind = PSCENE_ITEM_TEXTURE;
    ps->items[ps->n_items++].op = op;
  }
  for (uint32_t op = 0; op < n; op++) {
    if (ps->have[op] && !ps->geo[op].animated) {
      ps->items[ps->n_items].kind = PSCENE_ITEM_GEOMETRY;
      ps->items[ps->n_items++].op = op;
    }
  }
  PLAYER_LOAD_FREE(mark);
  return WZ3_OK;
}

PLAYER_SCENE_API wz3_result pscene_eval_textures(pscene *ps, void (*progress)(void *user, uint32_t done, uint32_t total), int (*cancelled)(void *user),
                                                 void *user)
{
  if (!ps || !ps->textures) return WZ3_ERR_ARGUMENT;
  uint32_t done = 0;
  if (progress) progress(user, 0, ps->n_independent);
  for (uint32_t i = 0; i < ps->n_textures; i++) {
    const uint32_t op = ps->textures[i];
    if (wz3_graph_render_dependent(ps->graph, op) || ps->failed_bitmap[op]) continue;
    if (cancelled && cancelled(user)) return WZ3_ERR_CANCELLED;
    const wz3_bitmap *bitmap = NULL;
    const wz3_result r = wz3_graph_eval_bitmap(ps->graph, op, &bitmap);
    if (r == WZ3_ERR_OUT_OF_MEMORY) return r;
    if (r != WZ3_OK || !bitmap) {
      ps->failed_bitmap[op] = 1;
      ps->bitmap_failures++;
    }
    done++;
    if (progress) progress(user, done, ps->n_independent);
  }
  ps->evaluated = done;
  return WZ3_OK;
}

PLAYER_SCENE_API glr_result pscene_warm_step(pscene *ps, uint32_t *done, uint32_t *total)
{
  if (!ps || !ps->renderer || !ps->items) return GLR_ERR_ARGUMENT;
  glr_result res = GLR_OK;
  ps->bitmap_oom = 0;
  if (ps->next_item < ps->n_items) {
    const pscene_item *item = &ps->items[ps->next_item];
    if (item->kind == PSCENE_ITEM_RENDER) {
      const wz3_bitmap *bitmap = NULL;
      const wz3_result r = wz3_graph_eval_bitmap(ps->graph, item->op, &bitmap);
      if (r == WZ3_ERR_OUT_OF_MEMORY)
        res = GLR_ERR_OUT_OF_MEMORY;
      else if (r != WZ3_OK || !bitmap) {
        ps->failed_bitmap[item->op] = 1;
        ps->bitmap_failures++;
      }
    } else if (item->kind == PSCENE_ITEM_TEXTURE) {
      res = glr_warm_texture(ps->renderer, &ps->host, item->op);
    } else {
      res = glr_warm_geometry(ps->renderer, &ps->host, item->op);
    }
    if (res == GLR_OK && ps->bitmap_oom) res = GLR_ERR_OUT_OF_MEMORY;
    if (res == GLR_OK) ps->next_item++;
  }
  if (done) *done = ps->next_item;
  if (total) *total = ps->n_items;
  return res;
}

#endif /* PLAYER_SCENE_IMPLEMENTATION_DONE */
#endif /* PLAYER_SCENE_IMPLEMENTATION */
