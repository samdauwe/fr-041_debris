/* Native shadow-job check for wz3.h against the JS port.
 *
 *   test_wz3_shadow_recipes <assets dir> <fixtures dir>
 *
 * During the precalculation every Mesh / MinMesh the Debris frames draw is prepared and its shadow data (vertex map, triangle
 * mask, source polygons, stored face normals and the native recipe) must fold to the hash of tools/dump_shadow_recipes.mjs. Then
 * the recipe of every geometry with casters is turned into a topology (native path), and the same geometry without it (welded
 * generic path) must describe the same closed shadow volumes wherever the source is a closed manifold; allocation failure at every
 * allocation of one mesh and one MinMesh recipe.
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

typedef struct expect {
  uint8_t present, recipe, has_t;
  uint32_t hash, t_groups, t_native, t_generic;
} expect;
typedef struct ctx {
  expect *table;
  uint32_t current_op, n_ops, checked, bad, recipes, no_recipe, topologies, closed_checked, closed_open, closed_solids, t_bad, oom_mesh, oom_minmesh;
  const wz3_rt *rt;
} ctx;

static int casts_fn(void *user, int32_t op)
{
  const wz3_material *m = wz3_rt_material((const wz3_rt *)user, (uint32_t)op);
  if (!m) return 0;
  for (uint32_t i = 0; i < m->n_passes; i++)
    if (m->passes[i].usage == WZ3_USAGE_SHADOW) return 1;
  return 0;
}

static uint32_t hash_shadow(const wz3_geometry *g, const wz3_shadow_data *d)
{
  hash h;
  hash_init(&h);
  hu(&h, g->n_vertices);
#define LIST(ptr, n)                                                           \
  do {                                                                         \
    if (!(ptr))                                                                \
      hu(&h, 0xffffffffu);                                                     \
    else {                                                                     \
      hu(&h, (uint32_t)(n));                                                   \
      for (size_t i_ = 0; i_ < (size_t)(n); i_++) hu(&h, (uint32_t)(ptr)[i_]); \
    }                                                                          \
  } while (0)
#define FLOATS(ptr, n)             \
  do {                             \
    if (!(ptr))                    \
      hu(&h, 0xffffffffu);         \
    else {                         \
      hu(&h, (uint32_t)(n));       \
      hfs(&h, (ptr), (size_t)(n)); \
    }                              \
  } while (0)
  LIST(d->vertex_map, d->n_vertices);
  LIST(d->triangle_mask, d->n_triangles);
  LIST(d->face_map, d->n_triangles);
  FLOATS(d->face_normals, (size_t)d->n_face_normals * 3);
  const wz3_shadow_recipe *r = d->recipe;
  hu(&h, r ? 1u : 0u);
  if (r) {
    hu(&h, r->n_key);
    for (uint32_t i = 0; i < r->n_key * 2; i++) hu(&h, r->key[i]);
    LIST(r->source_indices, r->n_source);
    LIST(r->faces, (size_t)r->n_triangles * 3);
    LIST(r->triangle_planes, r->n_triangles);
    FLOATS(r->planes, (size_t)r->n_planes * 4);
    LIST(r->edge_vertices, (size_t)r->n_edges * 2);
    LIST(r->edge_planes, (size_t)r->n_edges * 2);
    hu(&h, r->n_planes);
    hu(&h, r->trailing_plane_slots);
    hu(&h, r->source_non_manifold_edges);
  }
#undef LIST
#undef FLOATS
  return h.h;
}

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

/* every undirected edge of a closed triangle surface is used by two triangles, once in each direction */
static unsigned open_edges(const wz3_shadow_volume *v)
{
  typedef struct {
    uint32_t a, b;
    int dir;
  } rec;
  const uint32_t triangles = v->n_indices / 3;
  rec *recs = (rec *)malloc(((size_t)triangles * 3 + 1) * sizeof(rec));
  uint32_t count = 0;
  for (uint32_t t = 0; t < triangles; t++)
    for (int k = 0; k < 3; k++) {
      const uint32_t a = v->indices[t * 3 + (uint32_t)k], b = v->indices[t * 3 + (uint32_t)((k + 1) % 3)];
      if (a == b) continue; /* a collapsed edge draws nothing */
      recs[count].a = a < b ? a : b;
      recs[count].b = a < b ? b : a;
      recs[count].dir = a < b ? 1 : -1;
      count++;
    }
  /* shell sort by (a, b) */
  for (uint32_t gap = count / 2; gap > 0; gap /= 2)
    for (uint32_t i = gap; i < count; i++) {
      rec x = recs[i];
      uint32_t j = i;
      while (j >= gap && (recs[j - gap].a > x.a || (recs[j - gap].a == x.a && recs[j - gap].b > x.b))) {
        recs[j] = recs[j - gap];
        j -= gap;
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
    if (sum != 0) open++;
    i = j;
  }
  free(recs);
  return open;
}

static const float k_lights[3][3] = { { 40, 55, -70 }, { 0.3f, 2.1f, 0.7f }, { -8, 3, 12 } };

/* topology + volumes of one geometry: the hash of tools/dump_shadow_recipes.mjs; `closed` collects the closed-volume property */
static uint32_t topology_hash(ctx *c, const wz3_geometry *g, const wz3_shadow_data *d, const uint32_t *ranges, uint32_t n, int check_closed)
{
  hash h;
  hash_init(&h);
  wz3_shadow_topology t;
  const wz3_result r = wz3_shadow_topology_prepare(g, d, ranges, n, 0, &t);
  CHECK(r == WZ3_OK, "topology: %s", wz3_result_string(r));
  if (r != WZ3_OK) return 0;
  hash_topology(&h, &t);
  const int manifold = t.boundary_edges == 0 && t.non_manifold_edges == 0 && t.winding_conflict_edges == 0 && !t.native;
  for (int li = 0; li < 3; li++)
    for (int caps = 1; caps >= 0; caps--) {
      wz3_shadow_volume v;
      CHECK(wz3_shadow_volume_build(&t, k_lights[li], NULL, caps, &v) == WZ3_OK, "volume");
      hu(&h, v.wide);
      hu(&h, v.n_indices);
      for (uint32_t i = 0; i < v.n_indices; i++) hu(&h, v.indices[i]);
      hu(&h, v.silhouette_index_count);
      hu(&h, v.cap_index_count);
      hu(&h, t.n_polygons);
      for (uint32_t i = 0; i < t.n_polygons; i++) hu(&h, v.face_front[i]);
      hfs(&h, v.light_position, 3);
      if (check_closed && manifold && caps) {
        c->closed_checked++;
        c->closed_open += open_edges(&v) != 0;
      }
      wz3_shadow_volume_free(&v);
    }
  c->closed_solids += check_closed && manifold;
  wz3_shadow_topology_free(&t);
  return h.h;
}

/* shadowGroupsForGeometry: a group once per shadow pass of its material */
static uint32_t shadow_ranges(const ctx *c, const wz3_geometry *g, uint32_t *ranges, uint32_t capacity)
{
  uint32_t n = 0;
  for (uint32_t i = 0; i < g->n_groups; i++) {
    const wz3_material *m = g->groups[i].material >= 0 ? wz3_rt_material(c->rt, (uint32_t)g->groups[i].material) : NULL;
    for (uint32_t p = 0; m && p < m->n_passes; p++)
      if (m->passes[p].usage == WZ3_USAGE_SHADOW && n < capacity) {
        ranges[n * 2] = g->groups[i].start;
        ranges[n * 2 + 1] = g->groups[i].count;
        n++;
      }
  }
  return n;
}

static void check_topologies(ctx *c, const wz3_geometry *g, wz3_shadow_data *d)
{
  /* the native topology of the recipe, then the generic one from the same geometry without a recipe */
  uint32_t ranges[2 * 64];
  const wz3_shadow_recipe *r = d->recipe;
  if (!r || r->n_key > 64) return;
  memcpy(ranges, r->key, (size_t)r->n_key * 8);
  {
    const expect *x = &c->table[c->current_op];
    uint32_t sr[2 * 64];
    const uint32_t n = shadow_ranges(c, g, sr, 64);
    wz3_shadow_data generic = *d;
    generic.recipe = NULL;
    const uint32_t hn = topology_hash(c, g, d, sr, n, 0), hg = topology_hash(c, g, &generic, sr, n, 1);
    CHECK(x->has_t && x->t_groups == n, "op %u: %u shadow groups, JavaScript %u", c->current_op, n, x->t_groups);
    if ((hn != x->t_native || hg != x->t_generic) && ++c->t_bad <= 8)
      printf("  op %u: topology native C %x JS %x, generic C %x JS %x\n", c->current_op, hn, x->t_native, hg, x->t_generic);
  }
  wz3_shadow_topology native;
  wz3_result res = wz3_shadow_topology_prepare(g, d, ranges, r->n_key, g->animated, &native);
  CHECK(res == WZ3_OK, "native topology: %s", wz3_result_string(res));
  if (res != WZ3_OK) return;
  CHECK(native.native == 1, "the recipe's key selects the native path");
  c->topologies++;
  /* a light well outside the caster: the volume must reference only existing vertices and have whole triangles */
  wz3_shadow_volume v;
  const float light[3] = { 40.0f, 55.0f, -70.0f };
  CHECK(wz3_shadow_volume_build(&native, light, NULL, 1, &v) == WZ3_OK, "volume");
  int sane = v.n_indices % 3 == 0;
  for (uint32_t i = 0; i < v.n_indices; i++) sane &= v.indices[i] < native.n_vertices * 2u;
  CHECK(sane, "volume indices stay in range");
  wz3_shadow_volume_free(&v);
  wz3_shadow_topology_free(&native);
}

static void oom_sweep(ctx *c, const wz3_mesh *mesh, const wz3_minmesh *minmesh, const wz3_geometry *g)
{
  wz3_shadow_data d;
  long total;
  g_alloc_count = 0;
  g_fail_at = -1;
  wz3_result r = mesh ? wz3_mesh_shadow_data(mesh, g, casts_fn, (void *)c->rt, &d) : wz3_minmesh_shadow_data(minmesh, g, casts_fn, (void *)c->rt, &d);
  CHECK(r == WZ3_OK, "shadow data");
  if (r == WZ3_OK) wz3_shadow_data_free(&d);
  total = g_alloc_count;
  for (long fail = 0; fail < total; fail++) {
    const long before = g_live;
    g_alloc_count = 0;
    g_fail_at = fail;
    r = mesh ? wz3_mesh_shadow_data(mesh, g, casts_fn, (void *)c->rt, &d) : wz3_minmesh_shadow_data(minmesh, g, casts_fn, (void *)c->rt, &d);
    g_fail_at = -1;
    CHECK(r == WZ3_ERR_OUT_OF_MEMORY || r == WZ3_OK, "failure %ld: %s", fail, wz3_result_string(r));
    if (r == WZ3_OK) wz3_shadow_data_free(&d);
    CHECK(g_live == before, "failure %ld of %ld leaked %ld blocks", fail, total, g_live - before);
  }
  g_fail_at = -1;
}

static wz3_result on_geometry(ctx *c, uint32_t op, const wz3_mesh *mesh, const wz3_minmesh *minmesh)
{
  if (op >= c->n_ops || !c->table[op].present) return WZ3_OK;
  const expect *x = &c->table[op];
  c->current_op = op;
  wz3_geometry g;
  wz3_result r = mesh ? wz3_mesh_prepare(mesh, &g) : wz3_minmesh_prepare(minmesh, &g);
  CHECK(r == WZ3_OK, "prepare op %u: %s", op, wz3_result_string(r));
  if (r != WZ3_OK) return WZ3_OK;
  wz3_shadow_data d;
  r = mesh ? wz3_mesh_shadow_data(mesh, &g, casts_fn, (void *)c->rt, &d) : wz3_minmesh_shadow_data(minmesh, &g, casts_fn, (void *)c->rt, &d);
  CHECK(r == WZ3_OK, "shadow data op %u: %s", op, wz3_result_string(r));
  if (r == WZ3_OK) {
    c->checked++;
    const uint32_t got = hash_shadow(&g, &d);
    if ((got != x->hash || (d.recipe != NULL) != (x->recipe != 0)) && ++c->bad <= 8)
      printf("  op %u (%s): C %x recipe %d, JS %x recipe %d\n", op, mesh ? "mesh" : "minmesh", got, d.recipe != NULL, x->hash, x->recipe);
    if (d.recipe) {
      c->recipes++;
      check_topologies(c, &g, &d);
    } else
      c->no_recipe++;
    if (d.recipe && mesh && !c->oom_mesh) {
      c->oom_mesh = 1;
      oom_sweep(c, mesh, NULL, &g);
    }
    if (d.recipe && minmesh && !c->oom_minmesh) {
      c->oom_minmesh = 1;
      oom_sweep(c, NULL, minmesh, &g);
    }
    wz3_shadow_data_free(&d);
  }
  wz3_geometry_free(&g);
  return WZ3_OK;
}

static wz3_result on_mesh(void *user, uint32_t op, const wz3_mesh *mesh) { return mesh ? on_geometry((ctx *)user, op, mesh, NULL) : WZ3_OK; }
static wz3_result on_minmesh(void *user, uint32_t op, const wz3_minmesh *mesh) { return mesh ? on_geometry((ctx *)user, op, NULL, mesh) : WZ3_OK; }

int suite_wz3_shadow_recipes(int argc, char **argv)
{
  if (argc < 3) {
    printf("usage: test_wz3_shadow_recipes <assets dir> <fixtures dir>\n");
    return 2;
  }
  size_t kx_size;
  uint8_t *kx_bytes = gate_read_all(argv[1], "debris_party.kx", &kx_size);
  char *fixture = (char *)gate_read_all(argv[2], "shadow_recipes.txt", NULL);
  wz3_kx *kx = NULL;
  size_t at;
  CHECK(wz3_kx_parse(&kx, kx_bytes, kx_size, &at) == WZ3_OK, "parse");
  if (!kx) return 1;
  ctx c;
  memset(&c, 0, sizeof c);
  c.n_ops = kx->n_ops;
  c.table = (expect *)calloc(kx->n_ops, sizeof(expect));
  unsigned lines = 0, js_recipes = 0, t_lines = 0;
  for (char *line = strtok(fixture, "\n"); line; line = strtok(NULL, "\n")) {
    unsigned op, recipe, h, tg, tn, tgen;
    if (line[0] == 'T' && sscanf(line, "T %u %u %x %x", &op, &tg, &tn, &tgen) == 4 && op < kx->n_ops) {
      c.table[op].has_t = 1;
      c.table[op].t_groups = tg;
      c.table[op].t_native = tn;
      c.table[op].t_generic = tgen;
      t_lines++;
      continue;
    }
    if (sscanf(line, "S %u %u %x", &op, &recipe, &h) == 3 && op < kx->n_ops) {
      c.table[op].present = 1;
      c.table[op].recipe = (uint8_t)recipe;
      c.table[op].hash = h;
      lines++;
      js_recipes += recipe;
    }
  }
  wz3_rt_options options;
  memset(&options, 0, sizeof options);
  options.generic_exec = 1;
  options.release_consumed = 1;
  options.frame_jobs = 1;
  options.host.mesh = on_mesh;
  options.host.minmesh = on_minmesh;
  options.host.user = &c;
  wz3_result res;
  wz3_rt *rt = wz3_rt_create(kx, &options, &res);
  CHECK(rt && res == WZ3_OK, "create");
  c.rt = rt;
  if (rt) {
    res = wz3_rt_precalc(rt);
    CHECK(res == WZ3_OK, "precalc: %s", wz3_result_string(res));
    wz3_rt_destroy(rt);
  }
  free(c.table);
  wz3_kx_destroy(kx);
  free(kx_bytes);
  free(fixture);
  printf("shadow recipes: %u of %u geometries checked (%u with a recipe, %u without), %u differ, %u native topologies\n", c.checked, lines, c.recipes,
         c.no_recipe, c.bad, c.topologies);
  CHECK(lines == 400 && js_recipes == 231 && c.checked == lines && c.bad == 0 && c.recipes == js_recipes && c.oom_mesh && c.oom_minmesh,
        "%u of %u geometries, %u differ, %u recipes of %u", c.checked, lines, c.bad, c.recipes, js_recipes);
  printf("shadow recipes: %u topologies and volumes (native and generic) compared, %u differ; %u closed source solids, %u of their %u volumes open\n", t_lines,
         c.t_bad, c.closed_solids, c.closed_open, c.closed_checked);
  CHECK(t_lines == 231 && c.t_bad == 0, "%u topology lines, %u differ", t_lines, c.t_bad);
  CHECK(c.closed_solids >= 20 && c.closed_open == 0, "%u of %u volumes of %u closed solids are open", c.closed_open, c.closed_checked, c.closed_solids);
  CHECK(g_live == 0, "%ld blocks leaked", g_live);
  printf("shadow recipes: %d checks, %d failures\n", g_checks, g_failures);
  return g_failures ? 1 : 0;
}
