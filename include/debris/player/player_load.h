/* player_load.h: the host side of the runtime precalculation, SDL-free so that it can be tested headless.
 * Include wz3.h first.
 *
 *   #define PLAYER_LOAD_IMPLEMENTATION in exactly one source file.
 *
 * pgeometry is the geometry sink of the player: while wz3_rt_precalc() generates the 10,940 Mesh / MinMesh operators of the
 * party document it prepares the render-ready buffers (wz3_mesh_prepare) of the 574 that a scene draws - the others are
 * intermediates of the generators and are skipped - and hands each to a consumer together with the mesh or MinMesh it was
 * prepared from (exactly one of the two is non-NULL; valid during the call only: a consumer that needs a MinMesh later
 * retains it with wz3_minmesh_retain, and builds shadow data from the source here). The consumer owns the geometry (frees it,
 * or keeps it, e.g. in a GPU cache) whether it returns an error or not. Without a consumer the geometry is counted and freed.
 * A failing prepare or consumer stops the precalculation with its error.
 */
#ifndef PLAYER_LOAD_H
#define PLAYER_LOAD_H

#include <stdint.h>

#ifndef PLAYER_LOAD_API
#ifdef PLAYER_LOAD_STATIC
#define PLAYER_LOAD_API static
#else
#define PLAYER_LOAD_API extern
#endif
#endif

typedef wz3_result (*pgeometry_consume_fn)(void *user, uint32_t op, wz3_geometry *geometry, const wz3_mesh *mesh, const wz3_minmesh *minmesh);

typedef struct pgeometry {
  uint8_t *wanted; /* wz3_kx_scene_geometry marks, one per operator */
  uint32_t n_ops;
  pgeometry_consume_fn consume;
  void *consume_user;
  uint32_t prepared, skipped, empty; /* prepared and consumed, intermediates not prepared, operators without geometry */
  uint64_t vertices, indices, bytes, max_bytes;
} pgeometry;

PLAYER_LOAD_API wz3_result pgeometry_init(pgeometry *g, const wz3_kx *kx, pgeometry_consume_fn consume, void *consume_user);
PLAYER_LOAD_API void pgeometry_destroy(pgeometry *g);
/* The wz3_rt_host.mesh / .minmesh callbacks; user is the pgeometry. */
PLAYER_LOAD_API wz3_result pgeometry_mesh(void *user, uint32_t op, const wz3_mesh *mesh);
PLAYER_LOAD_API wz3_result pgeometry_minmesh(void *user, uint32_t op, const wz3_minmesh *mesh);

/* The graph options of the texture evaluation: `texture_offset` is --texture-quality (poptions.texture_offset), intermediates are
   freed after their last consumer, and the Bitmap_Render operators are the JavaScript port's blank placeholder until the renderer
   supplies them through options->render (wz3_graph_options.render). */
PLAYER_LOAD_API void ptexture_graph_options(wz3_graph_options *out, int texture_offset);

/* Loader bar position of a precalculation that has completed `done` of `total` operators (0 until the total is known). */
PLAYER_LOAD_API double ploader_fraction(uint32_t done, uint32_t total);

/* The whole loading screen: the operator precalculation, the texture evaluation and the first uploads are three stages that fill
   the bar in proportion to what they cost on the party document (about 0.4 s of operators, 7 s of textures at full quality, 1 s of
   uploads). A stage whose total is unknown (0) counts as not started; the result never decreases while the counts grow and never
   exceeds 1. */
PLAYER_LOAD_API double ploader_stages(uint32_t ops_done, uint32_t ops_total, uint32_t textures_done, uint32_t textures_total, uint32_t uploads_done,
                                      uint32_t uploads_total);

#endif /* PLAYER_LOAD_H */

#ifdef PLAYER_LOAD_IMPLEMENTATION
#ifndef PLAYER_LOAD_IMPLEMENTATION_DONE
#define PLAYER_LOAD_IMPLEMENTATION_DONE

#include <stdlib.h>
#include <string.h>

#ifndef PLAYER_LOAD_MALLOC
#define PLAYER_LOAD_MALLOC(n) malloc(n)
#define PLAYER_LOAD_FREE(p)   free(p)
#endif

PLAYER_LOAD_API wz3_result pgeometry_init(pgeometry *g, const wz3_kx *kx, pgeometry_consume_fn consume, void *consume_user)
{
  if (!g || !kx) return WZ3_ERR_ARGUMENT;
  memset(g, 0, sizeof *g);
  g->wanted = (uint8_t *)PLAYER_LOAD_MALLOC(kx->n_ops ? kx->n_ops : 1);
  if (!g->wanted) return WZ3_ERR_OUT_OF_MEMORY;
  g->n_ops = kx->n_ops;
  wz3_kx_scene_geometry(kx, g->wanted);
  g->consume = consume;
  g->consume_user = consume_user;
  return WZ3_OK;
}

PLAYER_LOAD_API void pgeometry_destroy(pgeometry *g)
{
  if (!g) return;
  PLAYER_LOAD_FREE(g->wanted);
  memset(g, 0, sizeof *g);
}

static wz3_result pgeometry_take(pgeometry *g, uint32_t op, wz3_geometry *geometry, const wz3_mesh *mesh, const wz3_minmesh *minmesh)
{
  const uint64_t bytes = wz3_geometry_bytes(geometry);
  g->prepared++;
  g->vertices += geometry->n_vertices;
  g->indices += geometry->n_indices;
  g->bytes += bytes;
  if (bytes > g->max_bytes) g->max_bytes = bytes;
  if (g->consume) return g->consume(g->consume_user, op, geometry, mesh, minmesh);
  wz3_geometry_free(geometry);
  return WZ3_OK;
}

PLAYER_LOAD_API wz3_result pgeometry_mesh(void *user, uint32_t op, const wz3_mesh *mesh)
{
  pgeometry *g = (pgeometry *)user;
  if (op >= g->n_ops || !g->wanted[op]) {
    g->skipped++;
    return WZ3_OK;
  }
  wz3_geometry geometry;
  const wz3_result r = wz3_mesh_prepare(mesh, &geometry);
  return r != WZ3_OK ? r : pgeometry_take(g, op, &geometry, mesh, NULL);
}

PLAYER_LOAD_API wz3_result pgeometry_minmesh(void *user, uint32_t op, const wz3_minmesh *mesh)
{
  pgeometry *g = (pgeometry *)user;
  if (!mesh) {
    g->empty++;
    return WZ3_OK;
  }
  if (op >= g->n_ops || !g->wanted[op]) {
    g->skipped++;
    return WZ3_OK;
  }
  wz3_geometry geometry;
  const wz3_result r = wz3_minmesh_prepare(mesh, &geometry);
  return r != WZ3_OK ? r : pgeometry_take(g, op, &geometry, NULL, mesh);
}

PLAYER_LOAD_API void ptexture_graph_options(wz3_graph_options *out, int texture_offset)
{
  memset(out, 0, sizeof *out);
  wz3_bitmap_ctx_init(&out->ctx);
  out->ctx.texture_offset = texture_offset;
  out->release_consumed = 1;
  out->render_placeholder = 1;
}

PLAYER_LOAD_API double ploader_fraction(uint32_t done, uint32_t total)
{
  if (!total) return 0.0;
  return done >= total ? 1.0 : (double)done / (double)total;
}

PLAYER_LOAD_API double ploader_stages(uint32_t ops_done, uint32_t ops_total, uint32_t textures_done, uint32_t textures_total, uint32_t uploads_done,
                                      uint32_t uploads_total)
{
  const double f = (6.0 * ploader_fraction(ops_done, ops_total) + 80.0 * ploader_fraction(textures_done, textures_total) +
                    14.0 * ploader_fraction(uploads_done, uploads_total)) /
                   100.0;
  return f > 1.0 ? 1.0 : f;
}

#endif /* PLAYER_LOAD_IMPLEMENTATION_DONE */
#endif /* PLAYER_LOAD_IMPLEMENTATION */
