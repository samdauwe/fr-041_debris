/* Render-geometry check for wz3.h.
 *
 *   test_wz3_geometry <assets dir> <fixtures dir>
 *
 * Precalculates the party document and, for every Mesh and MinMesh a handler produces, prepares the geometry the renderer
 * uploads (wz3_mesh_prepare / wz3_minmesh_prepare) and compares counts and the stream hash with
 * fixtures/geometry_hashes.txt (tools/dump_geometry.mjs: renderer.js normalizePreparedGeometry of the JS prepare()).
 * Every allocation of one prepare is then failed in turn.
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
  d[n] = 0;
  *size = (size_t)n;
  fclose(f);
  return d;
}

typedef struct expect {
  uint8_t present, seen, kind;
  uint32_t vertices, indices, groups, hash;
} expect;
typedef struct ctx {
  expect *table;
  uint32_t n, matched, mismatched, unexpected, alloc_points_checked;
  long fail_sweeps;
} ctx;

static void check_one(ctx *c, uint32_t op, int kind, const wz3_mesh *mesh, const wz3_minmesh *minmesh)
{
  wz3_geometry g;
  const wz3_result res = kind == 'm' ? wz3_mesh_prepare(mesh, &g) : wz3_minmesh_prepare(minmesh, &g);
  if (op >= c->n || !c->table[op].present) {
    c->unexpected++;
    if (res == WZ3_OK) wz3_geometry_free(&g);
    return;
  }
  expect *x = &c->table[op];
  x->seen = 1;
  if (res != WZ3_OK) {
    if (++c->mismatched <= 8) printf("  op %u: prepare failed: %s\n", op, wz3_result_string(res));
    return;
  }
  const uint32_t hash = wz3_geometry_hash(&g);
  { /* the byte size is the end of the last stream plus at most the 16-byte alignment pad (pointer arithmetic is independent of the layout code) */
    const size_t bytes = wz3_geometry_bytes(&g);
    const size_t end = (size_t)((const uint8_t *)(g.groups + g.n_groups) - (const uint8_t *)g.positions);
    CHECK(bytes >= end && bytes - end < 16 && (const void *)g.positions == g.block_, "op %u: %zu bytes, streams end at %zu", op, bytes, end);
  }
  if (x->kind == kind && x->vertices == g.n_vertices && x->indices == g.n_indices && x->groups == g.n_groups && x->hash == hash)
    c->matched++;
  else if (++c->mismatched <= 8)
    printf("  op %u (%c): C %u/%u/%u hash %x, JS %c %u/%u/%u hash %x\n", op, kind, g.n_vertices, g.n_indices, g.n_groups, hash, x->kind, x->vertices,
           x->indices, x->groups, x->hash);
  /* every ~97th geometry: each allocation of the prepare fails in turn, nothing may leak */
  if (op % 97 == 0) {
    wz3_geometry_free(&g);
    const long live = g_live;
    long points = 0;
    for (long fail = 0;; fail++) {
      g_count = 0;
      g_fail_at = fail;
      wz3_geometry t;
      const wz3_result r = kind == 'm' ? wz3_mesh_prepare(mesh, &t) : wz3_minmesh_prepare(minmesh, &t);
      g_fail_at = -1;
      points++;
      if (r == WZ3_OK) {
        wz3_geometry_free(&t);
        break;
      }
      CHECK(r == WZ3_ERR_OUT_OF_MEMORY && g_live == live, "op %u: allocation %ld: %d, %ld blocks leaked", op, fail, (int)r, g_live - live);
      if (fail > 64) {
        CHECK(0, "op %u: prepare never succeeds", op);
        break;
      }
    }
    c->fail_sweeps += points;
    c->alloc_points_checked++;
    return;
  }
  wz3_geometry_free(&g);
}

static wz3_result on_mesh(void *user, uint32_t op, const wz3_mesh *mesh)
{
  check_one((ctx *)user, op, 'm', mesh, NULL);
  return WZ3_OK;
}
static wz3_result on_minmesh(void *user, uint32_t op, const wz3_minmesh *mesh)
{
  if (mesh) check_one((ctx *)user, op, 'n', NULL, mesh);
  return WZ3_OK;
}

/* Hand-checked MinMesh: production colours are all white, so byte order and the second UV channel are pinned here. */
static void test_synthetic(void)
{
  wz3_minmesh *m = wz3__mm_new();
  CHECK(m != NULL, "minmesh");
  if (!m) return;
  for (int i = 0; i < 3; i++) {
    wz3__mm_vertex *v = NULL;
    CHECK(wz3__mm_push_vertex(m, NULL, &v) == 0 && v, "vertex");
    if (!v) break;
    v->position[0] = (float)i;
    v->position[1] = (float)(i * i);
    v->position[2] = 0.5f;
    v->color = 0x11223344u;
    v->uv[0][0] = 0.25f * (float)i;
    v->uv[1][1] = 0.75f;
  }
  const int32_t tri[3] = { 0, 1, 2 };
  wz3__mm_push_face(m, tri, 3, 1, 0, NULL); /* drawn */
  wz3__mm_push_face(m, tri, 3, 0, 0, NULL); /* cluster 0: suppressed */
  wz3_geometry g;
  CHECK(wz3_minmesh_prepare(m, &g) == WZ3_OK, "prepare");
  CHECK(g.n_vertices == 3 && g.n_indices == 3 && g.n_groups == 1 && g.groups[0].slot == 1 && g.groups[0].count == 3, "counts");
  CHECK(g.colors_u8 && g.colors_u8[0] == 0x22 && g.colors_u8[1] == 0x33 && g.colors_u8[2] == 0x44 && g.colors_u8[3] == 0x11, "ARGB -> RGBA bytes");
  CHECK(g.uv1[1] == 0.75f && g.uv1[3] == 0.75f && g.uv0[2] == 0.25f && !g.uv1_is_uv0, "uv channels");
  CHECK(g.tangents[3] == 1.0f && g.indices[0] == 0 && g.indices[1] == 1 && g.indices[2] == 2, "tangent w, indices");
  CHECK(g.bounds_min[0] == 0.0f && g.bounds_max[0] == 2.0f && g.bounds_max[1] == 4.0f, "bounds");
  wz3_geometry_free(&g);
  wz3__mm_free(m);
}

/* The marking of scene-side geometry on a hand-built document: only a geometry operator consumed by a non-geometry one counts. */
static void test_scene_geometry_synthetic(void)
{
  uint16_t mesh_id = 0, minmesh_id = 0, scene_id = 0;
  for (uint16_t id = 1; id < 512; id++) {
    const wz3_class_info *info = wz3_class_lookup(id);
    if (!info) continue;
    if (info->output_class == WZ3_KC_MESH && !mesh_id) mesh_id = id;
    if (info->output_class == WZ3_KC_MINMESH && !minmesh_id) minmesh_id = id;
    if (info->output_class == WZ3_KC_SCENE && !scene_id) scene_id = id;
  }
  CHECK(mesh_id && minmesh_id && scene_id, "class ids found");
  /* 0 mesh, 1 mesh <- 0, 2 minmesh, 3 scene <- 1, 4 scene <- 3 and 2, 5 scene <- 99 (out of range) and 4 */
  static const uint32_t in1[] = { 0 }, in3[] = { 1 }, in4[] = { 3, 2 }, in5[] = { 99, 4 };
  wz3_kx_op ops[6];
  memset(ops, 0, sizeof ops);
  ops[0].class_id = mesh_id;
  ops[1].class_id = mesh_id;
  ops[1].n_inputs = 1;
  ops[1].inputs = in1;
  ops[2].class_id = minmesh_id;
  ops[3].class_id = scene_id;
  ops[3].n_inputs = 1;
  ops[3].inputs = in3;
  ops[4].class_id = scene_id;
  ops[4].n_inputs = 2;
  ops[4].inputs = in4;
  ops[5].class_id = scene_id;
  ops[5].n_inputs = 2;
  ops[5].inputs = in5;
  wz3_kx kx;
  memset(&kx, 0, sizeof kx);
  kx.n_ops = 6;
  kx.ops = ops;
  uint8_t marks[6];
  memset(marks, 0xaa, sizeof marks);
  const uint32_t n = wz3_kx_scene_geometry(&kx, marks);
  CHECK(n == 2 && !marks[0] && marks[1] && marks[2] && !marks[3] && !marks[4] && !marks[5], "marks %d%d%d%d%d%d (%u)", marks[0], marks[1], marks[2], marks[3],
        marks[4], marks[5], n);
  CHECK(wz3_kx_scene_geometry(NULL, marks) == 0 && wz3_kx_scene_geometry(&kx, NULL) == 0, "NULL arguments");
  wz3_geometry empty;
  memset(&empty, 0, sizeof empty);
  CHECK(wz3_geometry_bytes(&empty) == 0 && wz3_geometry_bytes(NULL) == 0, "no bytes for no geometry");
}

int suite_wz3_geometry(int argc, char **argv)
{
  test_synthetic();
  test_scene_geometry_synthetic();
  if (argc < 3) {
    printf("usage: test_wz3_geometry <assets dir> <fixtures dir>\n");
    return 2;
  }
  size_t kx_size, fx_size;
  uint8_t *kx_bytes = read_all(argv[1], "debris_party.kx", &kx_size);
  char *fixture = (char *)read_all(argv[2], "geometry_hashes.txt", &fx_size);
  wz3_kx *kx = NULL;
  CHECK(wz3_kx_parse(&kx, kx_bytes, kx_size, NULL) == WZ3_OK, "parse");
  if (!kx) return 1;
  ctx c;
  memset(&c, 0, sizeof c);
  c.n = kx->n_ops;
  c.table = (expect *)calloc(kx->n_ops, sizeof(expect));
  uint32_t total = 0;
  for (char *line = strtok(fixture, "\n"); line; line = strtok(NULL, "\n")) {
    unsigned op, v, i, g, h;
    char kind;
    if (sscanf(line, "G %u %c %u %u %u %x", &op, &kind, &v, &i, &g, &h) != 6 || op >= kx->n_ops) continue;
    c.table[op].present = 1;
    c.table[op].kind = (uint8_t)kind;
    c.table[op].vertices = v;
    c.table[op].indices = i;
    c.table[op].groups = g;
    c.table[op].hash = h;
    total++;
  }
  CHECK(total == 10940, "fixture lists %u geometries", total);
  wz3_rt_options o;
  memset(&o, 0, sizeof o);
  o.generic_exec = 1;
  o.release_consumed = 1;
  o.host.mesh = on_mesh;
  o.host.minmesh = on_minmesh;
  o.host.user = &c;
  wz3_result res;
  wz3_rt *rt = wz3_rt_create(kx, &o, &res);
  CHECK(rt != NULL, "create");
  if (rt) {
    res = wz3_rt_precalc(rt);
    CHECK(res == WZ3_OK, "precalc: %s", wz3_result_string(res));
    wz3_rt_destroy(rt);
  }
  uint32_t unseen = 0;
  uint8_t *marks = (uint8_t *)malloc(kx->n_ops);
  CHECK(marks && wz3_kx_scene_geometry(kx, marks) == 574, "574 geometry operators feed a scene");
  for (uint32_t op = 0; marks && op < c.n; op++)
    if (marks[op]) CHECK(c.table[op].present && c.table[op].seen, "marked op %u produced no geometry", op);
  free(marks);
  for (uint32_t op = 0; op < c.n; op++)
    if (c.table[op].present && !c.table[op].seen) unseen++;
  printf("geometry: %u of %u match the JS renderer's buffers, %u mismatches, %u unexpected, %u never produced; %u allocation sweeps (%ld failure points)\n",
         c.matched, total, c.mismatched, c.unexpected, unseen, c.alloc_points_checked, c.fail_sweeps);
  CHECK(c.mismatched == 0 && c.unexpected == 0 && unseen == 0 && c.matched == total, "geometry differs from the JS port");
  free(c.table);
  free(fixture);
  wz3_kx_destroy(kx);
  free(kx_bytes);
  printf("geometry: %d checks, %d failures\n", g_checks, g_failures);
  return g_failures ? 1 : 0;
}
