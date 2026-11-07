/* The host of the glr frame tests (what the player keeps resident): the geometry sinks (static geometry, shadow data, retained animated
 * MinMeshes), the texture graph at offset -2 with the pretend rasterizer of tools/lib/glr_frames.mjs, and the Bitmap_Render hook.
 * Include after wz3.h, glr.h and gate_common.h. */
#ifndef GLR_TEST_FRAME_WORLD_H
#define GLR_TEST_FRAME_WORLD_H

/* ---- the host: what the player keeps resident ---- */

typedef struct world {
  const wz3_kx *kx;
  wz3_rt *rt;
  wz3_graph *graph;
  uint8_t *wanted, *have, *have_shadow, *failed_bitmap;
  wz3_geometry *geometry;
  wz3_shadow_data *shadow;
  wz3_minmesh **retained;
  wz3_geometry ***slots;
  uint32_t *n_slots;
  glr *renderer; /* the Bitmap_Render hook draws with it */
  const glr_host *host;
  uint32_t prepared, animated, with_shadow_recipe;
} world;

static int casts_fn(void *user, int32_t op)
{
  const wz3_material *m = wz3_rt_material((const wz3_rt *)user, (uint32_t)op);
  if (!m) return 0;
  for (uint32_t i = 0; i < m->n_passes; i++)
    if (m->passes[i].usage == WZ3_USAGE_SHADOW) return 1;
  return 0;
}

static wz3_result take(world *w, uint32_t op, wz3_result r, wz3_geometry *g, const wz3_mesh *mesh, const wz3_minmesh *minmesh)
{
  if (r != WZ3_OK) return r;
  wz3_shadow_data d;
  r = mesh ? wz3_mesh_shadow_data(mesh, g, casts_fn, w->rt, &d) : wz3_minmesh_shadow_data(minmesh, g, casts_fn, w->rt, &d);
  if (r != WZ3_OK) {
    wz3_geometry_free(g);
    return r;
  }
  if (minmesh && g->animated) {
    w->retained[op] = wz3_minmesh_retain(minmesh);
    if (!w->retained[op]) {
      wz3_shadow_data_free(&d);
      wz3_geometry_free(g);
      return WZ3_ERR_OUT_OF_MEMORY;
    }
    w->animated++;
  }
  w->geometry[op] = *g;
  w->shadow[op] = d;
  w->have[op] = 1;
  w->have_shadow[op] = 1;
  w->prepared++;
  w->with_shadow_recipe += d.recipe != NULL;
  return WZ3_OK;
}

static wz3_result sink_mesh(void *user, uint32_t op, const wz3_mesh *mesh)
{
  world *w = (world *)user;
  if (!mesh || !w->wanted[op]) return WZ3_OK;
  wz3_geometry g;
  const wz3_result r = wz3_mesh_prepare(mesh, &g);
  return take(w, op, r, &g, mesh, NULL);
}

static wz3_result sink_minmesh(void *user, uint32_t op, const wz3_minmesh *mesh)
{
  world *w = (world *)user;
  if (!mesh || !w->wanted[op]) return WZ3_OK;
  wz3_geometry g;
  const wz3_result r = wz3_minmesh_prepare(mesh, &g);
  return take(w, op, r, &g, NULL, mesh);
}

static const wz3_geometry *host_geometry(void *user, uint32_t op, double time, int32_t slot)
{
  world *w = (world *)user;
  if (!w->have[op]) return NULL;
  const wz3_geometry *base = &w->geometry[op];
  if (time < 0 || !base->animated || !w->retained[op]) return base;
  if ((uint32_t)slot >= w->n_slots[op]) {
    const uint32_t n = (uint32_t)slot + 1;
    w->slots[op] = (wz3_geometry **)realloc(w->slots[op], (size_t)n * sizeof *w->slots[op]);
    if (!w->slots[op]) return NULL;
    memset(w->slots[op] + w->n_slots[op], 0, (size_t)(n - w->n_slots[op]) * sizeof *w->slots[op]);
    w->n_slots[op] = n;
  }
  wz3_geometry **s = &w->slots[op][slot];
  if (!*s)
    *s = (wz3_geometry *)calloc(1, sizeof **s);
  else
    wz3_geometry_free(*s);
  if (!*s) return NULL;
  if (wz3_rt_minmesh_pose_geometry(w->rt, w->retained[op], base, time, *s) != WZ3_OK) return NULL;
  return *s;
}

static const wz3_shadow_data *host_shadow(void *user, uint32_t op)
{
  world *w = (world *)user;
  return w->have_shadow[op] ? &w->shadow[op] : NULL;
}

static const wz3_bitmap *host_bitmap(void *user, uint32_t op)
{
  world *w = (world *)user;
  if (w->failed_bitmap[op]) return NULL;
  const wz3_bitmap *bitmap = NULL;
  if (wz3_graph_eval_bitmap(w->graph, op, &bitmap) != WZ3_OK || !bitmap) {
    w->failed_bitmap[op] = 1;
    return NULL;
  }
  return bitmap;
}

/* wz3_bitmap_render_fn: the scene of a Render operator drawn by glr */
static wz3_result render_hook(void *user, const wz3_bitmap_render_request *request, wz3_bitmap **out)
{
  world *w = (world *)user;
  if (!w->renderer || !request->has_input) return WZ3_ERR_UNSUPPORTED;
  const glr_result r = glr_render_bitmap(w->renderer, w->host, request->input, request->width, request->height, out);
  return r == GLR_OK ? WZ3_OK : r == GLR_ERR_OUT_OF_MEMORY ? WZ3_ERR_OUT_OF_MEMORY : WZ3_ERR_OPERATOR;
}

/* the pretend rasterizer of tools/lib/glr_frames.mjs: no coverage, line height max(1, round(text height * height)) */
static int text_empty(void *user, const wz3_text_request *req, wz3_text_result *out)
{
  (void)user;
  const size_t n = (size_t)req->width * (size_t)req->height;
  uint8_t *coverage = (uint8_t *)WZ3_MALLOC(n ? n : 1);
  if (!coverage) return 0;
  memset(coverage, 0, n ? n : 1);
  const double lines = floor((double)req->text_height * (double)req->height + 0.5);
  out->coverage = coverage;
  out->line_height = lines < 1.0 ? 1 : (int32_t)lines;
  return 1;
}

static void world_free(world *w, uint32_t n)
{
  for (uint32_t op = 0; op < n; op++) {
    for (uint32_t s = 0; s < w->n_slots[op]; s++)
      if (w->slots[op][s]) {
        wz3_geometry_free(w->slots[op][s]);
        free(w->slots[op][s]);
      }
    free(w->slots[op]);
    if (w->have[op]) wz3_geometry_free(&w->geometry[op]);
    if (w->have_shadow[op]) wz3_shadow_data_free(&w->shadow[op]);
    wz3_minmesh_release(w->retained[op]);
  }
  wz3_rt_destroy(w->rt);
  wz3_graph_destroy(w->graph);
  free(w->wanted);
  free(w->have);
  free(w->have_shadow);
  free(w->failed_bitmap);
  free(w->geometry);
  free(w->shadow);
  free(w->retained);
  free(w->slots);
  free(w->n_slots);
}

/* Builds the host of the (already patched) document: geometry sinks, graph, runtime, precalculation. 0 on failure (CHECKed). */
static int world_open(world *w, const wz3_kx *kx, int bitmap_render)
{
  memset(w, 0, sizeof *w);
  w->kx = kx;
  const uint32_t n = kx->n_ops;
  w->wanted = (uint8_t *)calloc(n, 1);
  w->have = (uint8_t *)calloc(n, 1);
  w->have_shadow = (uint8_t *)calloc(n, 1);
  w->failed_bitmap = (uint8_t *)calloc(n, 1);
  w->geometry = (wz3_geometry *)calloc(n, sizeof *w->geometry);
  w->shadow = (wz3_shadow_data *)calloc(n, sizeof *w->shadow);
  w->retained = (wz3_minmesh **)calloc(n, sizeof *w->retained);
  w->slots = (wz3_geometry ***)calloc(n, sizeof *w->slots);
  w->n_slots = (uint32_t *)calloc(n, sizeof *w->n_slots);
  CHECK(w->wanted && w->have && w->have_shadow && w->failed_bitmap && w->geometry && w->shadow && w->retained && w->slots && w->n_slots, "allocation");
  wz3_kx_scene_geometry(kx, w->wanted);
  wz3_graph_options go;
  memset(&go, 0, sizeof go);
  wz3_bitmap_ctx_init(&go.ctx);
  go.ctx.texture_offset = -2;
  go.ctx.text = text_empty;
  go.render_placeholder = 1;
  if (bitmap_render) {
    go.render = render_hook;
    go.render_user = w;
  }
  wz3_result res;
  w->graph = wz3_graph_create(kx, &go, &res);
  CHECK(w->graph != NULL, "graph");
  wz3_rt_options opt;
  memset(&opt, 0, sizeof opt);
  opt.generic_exec = 1;
  opt.release_consumed = 1;
  opt.frame_jobs = 1;
  opt.geometry = WZ3_GEOMETRY_GENERATE;
  opt.host.user = w;
  opt.host.mesh = sink_mesh;
  opt.host.minmesh = sink_minmesh;
  opt.host.bitmap = host_bitmap;
  w->rt = wz3_rt_create(kx, &opt, &res);
  CHECK(w->rt && res == WZ3_OK, "runtime create");
  if (!w->rt || !w->graph) return 0;
  wz3_rt_set_aspect(w->rt, 2.0);
  res = wz3_rt_precalc(w->rt);
  CHECK(res == WZ3_OK, "precalc: %s", wz3_result_string(res));
  return res == WZ3_OK;
}

#endif
