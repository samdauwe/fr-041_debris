/* Shadow topology / volume check for wz3.h against src/renderer.js.
 *
 *   test_wz3_shadow <fixtures dir> [--print]
 *
 * Replays tools/dump_shadow.mjs: prepareShadowTopology (native recipe and welded generic path, static and animated),
 * refreshShadowTopologyPositions, the reuse decision of the geometry cache, shadowGroupStructureMatches and
 * buildShadowVolume with and without caps. Then C-only checks: allocation failure at every allocation, and the shadow
 * volume of closed solids is a closed surface for every light position.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static long g_live, g_alloc_count, g_fail_at = -1;
static void *test_malloc(size_t n)
{
  if (g_alloc_count++ == g_fail_at) return NULL;
  void *p = malloc(n + 16);
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

static void hash_topology(hash *h, const wz3_shadow_topology *t)
{
  hu(h, t->native);
  hu(h, t->uses_shadow_vertex_map);
  hu(h, t->n_vertices);
  hfs(h, t->positions, (size_t)t->n_vertices * 3);
  for (uint32_t i = 0; i < t->n_vertices; i++) hu(h, t->source_indices[i]);
  hu(h, t->n_triangles);
  for (uint32_t i = 0; i < t->n_triangles * 3; i++) hu(h, t->faces[i]);
  for (uint32_t i = 0; i < t->n_triangles; i++) hu(h, t->triangle_polygon[i]);
  hu(h, t->n_polygons);
  hfs(h, t->polygon_planes, (size_t)t->n_polygons * 4);
  hu(h, t->n_edges);
  for (uint32_t e = 0; e < t->n_edges; e++) {
    const wz3_shadow_edge *edge = &t->edges[e];
    hu(h, edge->count);
    hu(h, edge->a0);
    hu(h, edge->b0);
    hu(h, edge->f0);
    if (edge->count >= 2) {
      hu(h, edge->a1);
      hu(h, edge->b1);
      hu(h, edge->f1);
    }
  }
  hu(h, t->boundary_edges);
  hu(h, t->non_manifold_edges);
  hu(h, t->winding_conflict_edges);
  hu(h, t->max_edge_incidence);
  hu(h, t->source_non_manifold_edges);
  hu(h, t->trailing_plane_slots);
  hfs(h, t->volume_positions, (size_t)t->n_vertices * 6);
  for (uint32_t i = 0; i < t->n_vertices * 2; i++) hu(h, t->extrusions[i]);
}

static void hash_volume(hash *h, const wz3_shadow_volume *v, uint32_t n_polygons)
{
  hu(h, v->wide);
  hu(h, v->n_indices);
  for (uint32_t i = 0; i < v->n_indices; i++) hu(h, v->indices[i]);
  hu(h, v->silhouette_index_count);
  hu(h, v->cap_index_count);
  hu(h, n_polygons);
  for (uint32_t i = 0; i < n_polygons; i++) hu(h, v->face_front[i]);
  hfs(h, v->light_position, 3);
}

static uint32_t *rwords(reader *r, uint32_t *n)
{
  *n = ru32(r);
  uint32_t *v = (uint32_t *)malloc(((size_t)*n + 1) * 4);
  for (uint32_t i = 0; i < *n; i++) v[i] = ru32(r);
  return v;
}
static float *rfloats(reader *r, uint32_t *n)
{
  *n = ru32(r);
  float *v = (float *)malloc(((size_t)*n + 1) * 4);
  for (uint32_t i = 0; i < *n; i++) v[i] = rf32(r);
  return v;
}

/* everything one `topo` line carries */
typedef struct topo_case {
  uint32_t nv, n_pos, n_indices, n_ranges, n_map, n_mask, n_face_map, n_normals, ni_src, n_faces, n_tp, n_planes, n_ev, n_ep, n_new;
  float *positions, *normals, *recipe_planes, *new_positions;
  uint32_t *indices, *ranges, *map, *face_map, *mask_words, *recipe_source, *recipe_faces, *recipe_tp, *recipe_ev, *recipe_ep;
  uint8_t *mask;
  int has_map, has_mask, has_face_map, has_normals, has_recipe, key_matches, dynamic, has_model, caps;
  uint32_t plane_count, trailing, source_nm, refresh_mode, dirty, flags, positions_same;
  float light[3], model[16];
  wz3_shadow_data data;
  wz3_shadow_recipe recipe;
  uint32_t *key_storage;
} topo_case;

static void parse_topo(reader *r, topo_case *c)
{
  memset(c, 0, sizeof *c);
  c->nv = ru32(r);
  c->positions = rfloats(r, &c->n_pos);
  c->indices = rwords(r, &c->n_indices);
  c->n_ranges = ru32(r);
  c->ranges = (uint32_t *)malloc(((size_t)c->n_ranges + 1) * 8);
  for (uint32_t i = 0; i < c->n_ranges; i++) {
    c->ranges[i * 2] = ru32(r);
    c->ranges[i * 2 + 1] = ru32(r);
  }
}

static void parse_rest(reader *r, topo_case *c)
{
  c->has_map = (int)ru32(r);
  if (c->has_map) c->map = rwords(r, &c->n_map);
  c->has_mask = (int)ru32(r);
  if (c->has_mask) {
    c->mask_words = rwords(r, &c->n_mask);
    c->mask = (uint8_t *)malloc((size_t)c->n_mask + 1);
    for (uint32_t i = 0; i < c->n_mask; i++) c->mask[i] = (uint8_t)c->mask_words[i];
  }
  c->has_face_map = (int)ru32(r);
  if (c->has_face_map) c->face_map = rwords(r, &c->n_face_map);
  c->has_normals = (int)ru32(r);
  if (c->has_normals) c->normals = rfloats(r, &c->n_normals);
  c->has_recipe = (int)ru32(r);
  if (c->has_recipe) {
    c->key_matches = (int)ru32(r);
    c->plane_count = ru32(r);
    c->recipe_source = rwords(r, &c->ni_src);
    c->recipe_faces = rwords(r, &c->n_faces);
    c->recipe_tp = rwords(r, &c->n_tp);
    c->recipe_planes = rfloats(r, &c->n_planes);
    c->recipe_ev = rwords(r, &c->n_ev);
    c->recipe_ep = rwords(r, &c->n_ep);
    c->trailing = ru32(r);
    c->source_nm = ru32(r);
  }
  c->dynamic = (int)ru32(r);
  uint32_t n_light;
  float *light = rfloats(r, &n_light);
  memcpy(c->light, light, 12);
  free(light);
  c->has_model = (int)ru32(r);
  if (c->has_model) rmat(r, c->model);
  c->caps = (int)ru32(r);
  c->refresh_mode = ru32(r);
  c->new_positions = rfloats(r, &c->n_new);
  c->dirty = ru32(r);
  c->flags = ru32(r);
  c->positions_same = ru32(r);
}

static void free_topo(topo_case *c)
{
  free(c->positions);
  free(c->indices);
  free(c->ranges);
  free(c->map);
  free(c->mask);
  free(c->mask_words);
  free(c->face_map);
  free(c->normals);
  free(c->recipe_source);
  free(c->recipe_faces);
  free(c->recipe_tp);
  free(c->recipe_planes);
  free(c->recipe_ev);
  free(c->recipe_ep);
  free(c->new_positions);
  free(c->key_storage);
}

static void build_inputs(topo_case *c, wz3_geometry *g)
{
  memset(g, 0, sizeof *g);
  g->n_vertices = c->nv;
  g->n_indices = c->n_indices;
  g->positions = c->positions;
  g->indices = c->indices;
  memset(&c->data, 0, sizeof c->data);
  c->data.n_vertices = c->nv;
  c->data.n_triangles = c->n_indices / 3;
  if (c->has_map) c->data.vertex_map = c->map;
  if (c->has_mask) c->data.triangle_mask = c->mask;
  if (c->has_face_map) c->data.face_map = c->face_map;
  if (c->has_normals) {
    c->data.face_normals = c->normals;
    c->data.n_face_normals = c->n_normals / 3;
  }
  if (c->has_recipe) {
    wz3_shadow_recipe *rc = &c->recipe;
    memset(rc, 0, sizeof *rc);
    rc->n_source = c->ni_src;
    rc->n_triangles = c->n_faces / 3;
    rc->n_planes = c->plane_count;
    rc->n_edges = c->n_ev / 2;
    rc->source_indices = c->recipe_source;
    rc->faces = c->recipe_faces;
    rc->triangle_planes = c->recipe_tp;
    rc->planes = c->recipe_planes;
    rc->edge_vertices = c->recipe_ev;
    rc->edge_planes = c->recipe_ep;
    rc->trailing_plane_slots = c->trailing;
    rc->source_non_manifold_edges = c->source_nm;
    rc->n_key = c->n_ranges;
    c->key_storage = (uint32_t *)malloc(((size_t)c->n_ranges + 1) * 8);
    for (uint32_t i = 0; i < c->n_ranges; i++) {
      c->key_storage[i * 2] = c->ranges[i * 2] + (c->key_matches ? 0u : 3u);
      c->key_storage[i * 2 + 1] = c->ranges[i * 2 + 1];
    }
    rc->key = c->key_storage;
    c->data.recipe = rc;
  }
}

static struct {
  unsigned native, generic, threw, caps, refreshed, kept, dropped, dynamic, winding, boundary, nonmanifold;
} g_seen;

static char g_desc[256];
static uint32_t run_topo(reader *r)
{
  topo_case c;
  parse_topo(r, &c);
  parse_rest(r, &c);
  wz3_geometry g;
  build_inputs(&c, &g);
  hash h;
  hash_init(&h);
  wz3_shadow_topology t;
  const wz3_result res = wz3_shadow_topology_prepare(&g, &c.data, c.ranges, c.n_ranges, c.dynamic, &t);
  hu(&h, res == WZ3_OK ? 0u : 1u);
  if (res == WZ3_OK) {
    g_seen.native += t.native;
    g_seen.generic += !t.native;
    g_seen.dynamic += c.dynamic;
    g_seen.winding += t.winding_conflict_edges > 0;
    g_seen.boundary += t.boundary_edges > 0;
    g_seen.nonmanifold += t.non_manifold_edges > 0;
    hash_topology(&h, &t);
    wz3_shadow_volume v;
    CHECK(wz3_shadow_volume_build(&t, c.light, c.has_model ? c.model : NULL, c.caps, &v) == WZ3_OK, "volume");
    hash_volume(&h, &v, t.n_polygons);
    g_seen.caps += c.caps && v.cap_index_count > 0;
    wz3_shadow_volume_free(&v);
    if (c.refresh_mode == 1) {
      hu(&h, (uint32_t)wz3_shadow_topology_refresh_positions(&t, c.new_positions, c.nv));
      hash_topology(&h, &t);
    } else if (c.refresh_mode == 2) {
      const int identical = c.dirty != 0 && c.dirty != 2 && c.flags == 0x3f;
      const int changed = c.dirty == 1 || !c.positions_same;
      const int code = wz3_shadow_topology_reuse(&t, identical, changed, c.positions_same ? c.positions : c.new_positions, c.nv);
      hu(&h, (uint32_t)(code == WZ3_SHADOW_DROP ? 2 : code == WZ3_SHADOW_REFRESHED ? 1 : 0));
      g_seen.refreshed += code == WZ3_SHADOW_REFRESHED;
      g_seen.kept += code == WZ3_SHADOW_KEEP;
      g_seen.dropped += code == WZ3_SHADOW_DROP;
      hash_topology(&h, &t);
    }
    wz3_shadow_topology_free(&t);
  } else
    g_seen.threw++;
  /* allocation failure at every allocation of the preparation and the volume of a sample of the lines */
  static unsigned sweeps;
  if (sweeps < 60 && c.refresh_mode == 0) {
    sweeps++;
    long total;
    g_alloc_count = 0;
    g_fail_at = -1;
    wz3_result r0 = wz3_shadow_topology_prepare(&g, &c.data, c.ranges, c.n_ranges, c.dynamic, &t);
    if (r0 == WZ3_OK) {
      wz3_shadow_volume v;
      wz3_shadow_volume_build(&t, c.light, NULL, 1, &v);
      wz3_shadow_volume_free(&v);
      wz3_shadow_topology_free(&t);
    }
    total = g_alloc_count;
    for (long fail = 0; fail < total; fail++) {
      const long before = g_live;
      g_alloc_count = 0;
      g_fail_at = fail;
      const wz3_result rf = wz3_shadow_topology_prepare(&g, &c.data, c.ranges, c.n_ranges, c.dynamic, &t);
      if (rf == WZ3_OK) {
        wz3_shadow_volume v;
        const wz3_result rv = wz3_shadow_volume_build(&t, c.light, NULL, 1, &v);
        g_fail_at = -1;
        if (rv == WZ3_OK) wz3_shadow_volume_free(&v);
        wz3_shadow_topology_free(&t);
      } else {
        g_fail_at = -1;
        CHECK(rf == WZ3_ERR_OUT_OF_MEMORY || r0 != WZ3_OK, "failure %ld: %s", fail, wz3_result_string(rf));
      }
      g_fail_at = -1;
      CHECK(g_live == before, "failure %ld of %ld leaked %ld blocks", fail, total, g_live - before);
    }
  }
  g_fail_at = -1;
  snprintf(g_desc, sizeof g_desc,
           "nsrc %u nfaces %u ntp %u nplanes %u nev %u  recipe %d key %d dynamic %d refresh %u dirty %u flags %x same %u map %d mask %d facemap %d normals %d "
           "nv %u ni %u ranges %u",
           c.ni_src, c.n_faces, c.n_tp, c.plane_count, c.n_ev, c.has_recipe, c.key_matches, c.dynamic, c.refresh_mode, c.dirty, c.flags, c.positions_same,
           c.has_map, c.has_mask, c.has_face_map, c.has_normals, c.nv, c.n_indices, c.n_ranges);
  const uint32_t result = h.h;
  free_topo(&c);
  return result;
}

static uint32_t run_gstruct(reader *r)
{
  wz3_geometry_group a[8], b[8];
  uint32_t na = ru32(r);
  for (uint32_t i = 0; i < na; i++) {
    a[i].material = ri32(r);
    a[i].slot = ri32(r);
    a[i].pass = ri32(r);
    a[i].start = ru32(r);
    a[i].count = ru32(r);
  }
  uint32_t nb = ru32(r);
  for (uint32_t i = 0; i < nb; i++) {
    b[i].material = ri32(r);
    b[i].slot = ri32(r);
    b[i].pass = ri32(r);
    b[i].start = ru32(r);
    b[i].count = ru32(r);
  }
  hash h;
  hash_init(&h);
  hu(&h, (uint32_t)wz3_shadow_group_structure_matches(a, na, b, nb));
  return h.h;
}

/* ---- the volume of a closed solid is closed ---- */

static unsigned edge_balance_open(const wz3_shadow_volume *v)
{
  /* every undirected edge of a closed triangle surface is used by exactly two triangles, once in each direction */
  typedef struct {
    uint32_t a, b;
    int dir;
  } rec;
  const uint32_t n = v->n_indices;
  rec *recs = (rec *)malloc(((size_t)n + 1) * sizeof(rec));
  uint32_t count = 0;
  for (uint32_t t = 0; t + 2 < n + 0u; t += 3) {
    for (int k = 0; k < 3; k++) {
      const uint32_t a = v->indices[t + k], b = v->indices[t + (k + 1) % 3];
      recs[count].a = a < b ? a : b;
      recs[count].b = a < b ? b : a;
      recs[count].dir = a < b ? 1 : -1;
      count++;
    }
  }
  /* sort by (a, b) and check each group sums to zero and has an even size */
  for (uint32_t i = 1; i < count; i++) {
    rec x = recs[i];
    uint32_t j = i;
    while (j > 0 && (recs[j - 1].a > x.a || (recs[j - 1].a == x.a && recs[j - 1].b > x.b))) {
      recs[j] = recs[j - 1];
      j--;
    }
    recs[j] = x;
  }
  unsigned open = 0;
  for (uint32_t i = 0; i < count;) {
    uint32_t j = i;
    int sum = 0;
    while (j < count && recs[j].a == recs[i].a && recs[j].b == recs[i].b) {
      sum += recs[j].dir;
      j++;
    }
    if (sum != 0 || ((j - i) & 1u)) open++;
    i = j;
  }
  free(recs);
  return open;
}

static void test_closed_volumes(void)
{
  /* a unit cube (six quads), an octahedron and a square pyramid with a quad base, as fans of polygons with the same outward winding */
  static const float cube_p[8][3] = { { 0, 0, 0 }, { 1, 0, 0 }, { 1, 1, 0 }, { 0, 1, 0 }, { 0, 0, 1 }, { 1, 0, 1 }, { 1, 1, 1 }, { 0, 1, 1 } };
  static const uint32_t cube_q[6][4] = { { 0, 3, 2, 1 }, { 4, 5, 6, 7 }, { 0, 1, 5, 4 }, { 1, 2, 6, 5 }, { 2, 3, 7, 6 }, { 3, 0, 4, 7 } };
  static const float oct_p[6][3] = { { 1, 0, 0 }, { -1, 0, 0 }, { 0, 1, 0 }, { 0, -1, 0 }, { 0, 0, 1 }, { 0, 0, -1 } };
  static const uint32_t oct_t[8][3] = { { 0, 2, 4 }, { 2, 1, 4 }, { 1, 3, 4 }, { 3, 0, 4 }, { 2, 0, 5 }, { 1, 2, 5 }, { 3, 1, 5 }, { 0, 3, 5 } };
  for (int solid = 0; solid < 2; solid++) {
    float positions[8 * 3];
    uint32_t indices[64];
    uint32_t n_indices = 0, nv = solid == 0 ? 8u : 6u;
    for (uint32_t i = 0; i < nv; i++) memcpy(positions + i * 3, solid == 0 ? cube_p[i] : oct_p[i], 12);
    if (solid == 0)
      for (int q = 0; q < 6; q++) {
        indices[n_indices++] = cube_q[q][0];
        indices[n_indices++] = cube_q[q][1];
        indices[n_indices++] = cube_q[q][2];
        indices[n_indices++] = cube_q[q][0];
        indices[n_indices++] = cube_q[q][2];
        indices[n_indices++] = cube_q[q][3];
      }
    else
      for (int q = 0; q < 8; q++) {
        indices[n_indices++] = oct_t[q][0];
        indices[n_indices++] = oct_t[q][1];
        indices[n_indices++] = oct_t[q][2];
      }
    wz3_geometry g;
    memset(&g, 0, sizeof g);
    g.n_vertices = nv;
    g.n_indices = n_indices;
    g.positions = positions;
    g.indices = indices;
    /* the cube's quads are polygons: two fan triangles share a face-map entry */
    uint32_t face_map[16];
    uint8_t mask[16];
    for (uint32_t t = 0; t < n_indices / 3; t++) {
      face_map[t] = solid == 0 ? t / 2 : t;
      mask[t] = 1;
    }
    wz3_shadow_data data;
    memset(&data, 0, sizeof data);
    data.n_triangles = n_indices / 3;
    data.face_map = face_map;
    data.triangle_mask = mask;
    const uint32_t range[2] = { 0, n_indices };
    wz3_shadow_topology t;
    CHECK(wz3_shadow_topology_prepare(&g, &data, range, 1, 0, &t) == WZ3_OK, "prepare solid %d", solid);
    CHECK(t.boundary_edges == 0 && t.non_manifold_edges == 0 && t.winding_conflict_edges == 0 && t.n_polygons == (solid == 0 ? 6u : 8u),
          "solid %d is a closed manifold: %u boundary, %u non-manifold, %u conflicts, %u polygons", solid, t.boundary_edges, t.non_manifold_edges,
          t.winding_conflict_edges, t.n_polygons);
    unsigned closed = 0, lights = 0;
    for (int ix = -3; ix <= 4; ix++)
      for (int iy = -3; iy <= 4; iy++)
        for (int iz = -3; iz <= 4; iz++) {
          const float light[3] = { (float)ix * 0.9f + 0.05f, (float)iy * 0.9f + 0.05f, (float)iz * 0.9f + 0.05f };
          wz3_shadow_volume v;
          CHECK(wz3_shadow_volume_build(&t, light, NULL, 1, &v) == WZ3_OK, "volume");
          lights++;
          closed += edge_balance_open(&v) == 0;
          wz3_shadow_volume_free(&v);
        }
    CHECK(closed == lights, "solid %d: %u of %u light positions give a closed volume", solid, closed, lights);
    wz3_shadow_topology_free(&t);
  }
}

/* the JavaScript indices are 16 bit up to 32767 volume pairs and 32 bit beyond */
static void test_wide(void)
{
  for (int pass = 0; pass < 2; pass++) {
    const uint32_t triangles = pass ? 10923u : 10922u; /* 32769 or 32766 vertices: 2 * n against 0xffff */
    const uint32_t nv = triangles * 3u;
    float *positions = (float *)calloc((size_t)nv * 3, 4);
    uint32_t *indices = (uint32_t *)malloc((size_t)nv * 4);
    for (uint32_t i = 0; i < nv; i++) {
      positions[i * 3] = (float)(i % 7);
      positions[i * 3 + 1] = (float)(i / 7);
      indices[i] = i;
    }
    wz3_geometry g;
    memset(&g, 0, sizeof g);
    g.n_vertices = nv;
    g.n_indices = nv;
    g.positions = positions;
    g.indices = indices;
    const uint32_t range[2] = { 0, nv };
    wz3_shadow_topology t;
    CHECK(wz3_shadow_topology_prepare(&g, NULL, range, 1, 0, &t) == WZ3_OK, "prepare");
    wz3_shadow_volume v;
    const float light[3] = { 0, 0, -5 };
    CHECK(wz3_shadow_volume_build(&t, light, NULL, 1, &v) == WZ3_OK, "volume");
    CHECK(t.n_vertices == nv && v.wide == (pass ? 1u : 0u), "%u vertices: wide %u", t.n_vertices, v.wide);
    wz3_shadow_volume_free(&v);
    wz3_shadow_topology_free(&t);
    free(positions);
    free(indices);
  }
}

int suite_wz3_shadow(int argc, char **argv)
{
  if (argc < 2) {
    printf("usage: test_wz3_shadow <fixtures dir> [--print]\n");
    return 2;
  }
  const int print = argc > 2 && !strcmp(argv[2], "--print");
  char *fixture = (char *)gate_read_all(argv[1], "shadow.txt", NULL);
  unsigned compared = 0, mismatches = 0;
  size_t line_number = 0;
  for (char *line = fixture; *line;) {
    char *nl = strchr(line, '\n');
    if (nl) *nl = 0;
    line_number++;
    char kind[16];
    if (sscanf(line, "%15s", kind) != 1) break;
    reader r;
    r.p = line + strlen(kind);
    const uint32_t got = !strcmp(kind, "topo") ? run_topo(&r) : !strcmp(kind, "gstruct") ? run_gstruct(&r) : 0xffffffffu;
    const char *eq = strstr(line, " = ");
    if (eq) {
      const uint32_t expected = (uint32_t)strtoul(eq + 3, NULL, 16);
      compared++;
      if (print)
        printf("%s = %x\n", kind, got);
      else if (got != expected) {
        mismatches++;
        CHECK(0, "line %zu (%s) hash %x, JavaScript %x [%s]", line_number, kind, got, expected, g_desc);
      }
    }
    if (!nl) break;
    line = nl + 1;
  }
  free(fixture);
  CHECK(compared == 1800, "%u lines", compared);
  CHECK(g_seen.native > 100 && g_seen.generic > 1000 && g_seen.threw > 5 && g_seen.caps > 500 && g_seen.refreshed > 20 && g_seen.kept > 5 &&
          g_seen.dropped > 50 && g_seen.dynamic > 300 && g_seen.winding > 50 && g_seen.boundary > 500 && g_seen.nonmanifold > 20,
        "coverage: %u native, %u generic, %u throwing, %u with caps, %u refreshed, %u kept, %u dropped, %u dynamic, %u winding, %u boundary, %u non-manifold",
        g_seen.native, g_seen.generic, g_seen.threw, g_seen.caps, g_seen.refreshed, g_seen.kept, g_seen.dropped, g_seen.dynamic, g_seen.winding,
        g_seen.boundary, g_seen.nonmanifold);
  test_closed_volumes();
  test_wide();
  CHECK(g_live == 0, "%ld blocks leaked", g_live);
  printf("shadow: %u lines compared, %u mismatches, %d checks, %d failures\n", compared, mismatches, g_checks, g_failures);
  return g_failures ? 1 : 0;
}
