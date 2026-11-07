/* Posed-geometry check for wz3.h against the JS renderer.
 *
 *   test_wz3_pose_geometry <assets dir> <fixtures dir>
 *
 * During the precalculation every animated MinMesh is prepared (wz3_minmesh_prepare) and posed and skinned at five times
 * (wz3_rt_minmesh_pose_geometry); the geometry hash plus the group boxes must equal tools/dump_pose_geometry.mjs'. Then
 * an allocation failure at every allocation of one posed geometry, and the contract of the call (arguments, ownership).
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

static const double k_times[5] = { 0, 0.37, 1.5, 4, 12.25 };

typedef struct expect {
  uint32_t hash[5];
  uint8_t has[5];
} expect;
typedef struct ctx {
  expect *table;
  uint32_t n_ops, checked, bad, meshes, oom_done;
  const wz3_rt *rt;
} ctx;

static uint32_t pose_hash(const wz3_geometry *g)
{
  /* geometry hash of the fixture, then per group whether it has a box and the box */
  hash h;
  hash_init(&h);
  uint32_t words = wz3_geometry_hash(g);
  h.h = words; /* continue the FNV-1a stream of wz3_geometry_hash */
  for (uint32_t i = 0; i < g->n_groups; i++) {
    hu(&h, g->groups[i].has_bounds);
    if (g->groups[i].has_bounds) {
      hfs(&h, g->groups[i].bounds_min, 3);
      hfs(&h, g->groups[i].bounds_max, 3);
    }
  }
  return h.h;
}

static void oom_sweep(ctx *c, const wz3_minmesh *mesh, const wz3_geometry *base)
{
  long total;
  wz3_geometry out;
  g_alloc_count = 0;
  g_fail_at = -1;
  CHECK(wz3_rt_minmesh_pose_geometry(c->rt, mesh, base, 1.5, &out) == WZ3_OK, "pose");
  total = g_alloc_count;
  wz3_geometry_free(&out);
  for (long fail = 0; fail < total; fail++) {
    const long before = g_live;
    g_alloc_count = 0;
    g_fail_at = fail;
    const wz3_result r = wz3_rt_minmesh_pose_geometry(c->rt, mesh, base, 1.5, &out);
    g_fail_at = -1;
    CHECK(r == WZ3_ERR_OUT_OF_MEMORY || r == WZ3_OK, "failure %ld: %s", fail, wz3_result_string(r));
    if (r == WZ3_OK) wz3_geometry_free(&out);
    CHECK(g_live == before, "failure %ld of %ld leaked %ld blocks", fail, total, g_live - before);
  }
  /* contract */
  CHECK(wz3_rt_minmesh_pose_geometry(NULL, mesh, base, 0, &out) == WZ3_ERR_ARGUMENT, "no runtime");
  CHECK(wz3_rt_minmesh_pose_geometry(c->rt, mesh, base, 0, (wz3_geometry *)base) == WZ3_ERR_ARGUMENT, "out == base");
  wz3_geometry other = *base;
  other.n_vertices++;
  CHECK(wz3_rt_minmesh_pose_geometry(c->rt, mesh, &other, 0, &out) == WZ3_ERR_ARGUMENT, "a base of another mesh");
  CHECK(wz3_rt_minmesh_pose_geometry(c->rt, mesh, base, 1.5, &out) == WZ3_OK, "pose");
  CHECK(out.positions != base->positions && out.normals != base->normals && out.tangents != base->tangents, "dynamic streams are owned");
  CHECK(out.uv0 == base->uv0 && out.indices == base->indices && out.bone_weights == base->bone_weights, "static streams alias the base");
  CHECK(out.animated == 1 && wz3_geometry_bytes(&out) > 0 && wz3_geometry_bytes(&out) < wz3_geometry_bytes(base), "bytes of the dynamic block only (%zu)",
        wz3_geometry_bytes(&out));
  wz3_geometry_free(&out);
  CHECK(out.block_ == NULL && wz3_geometry_bytes(&out) == 0, "freed");
  c->oom_done = 1;
}

static wz3_result on_minmesh(void *user, uint32_t op, const wz3_minmesh *mesh)
{
  ctx *c = (ctx *)user;
  if (!mesh || op >= c->n_ops) return WZ3_OK;
  const expect *x = &c->table[op];
  int any = 0;
  for (int k = 0; k < 5; k++) any |= x->has[k];
  if (!any) return WZ3_OK;
  c->meshes++;
  wz3_geometry base;
  wz3_result r = wz3_minmesh_prepare(mesh, &base);
  CHECK(r == WZ3_OK, "prepare op %u: %s", op, wz3_result_string(r));
  if (r != WZ3_OK) return WZ3_OK;
  for (int k = 0; k < 5; k++) {
    if (!x->has[k]) continue;
    wz3_geometry posed;
    r = wz3_rt_minmesh_pose_geometry(c->rt, mesh, &base, k_times[k], &posed);
    CHECK(r == WZ3_OK, "pose op %u t=%g: %s", op, k_times[k], wz3_result_string(r));
    if (r != WZ3_OK) continue;
    c->checked++;
    const uint32_t got = pose_hash(&posed);
    if (got != x->hash[k] && ++c->bad <= 8) printf("  op %u t=%g: C %x, JS %x\n", op, k_times[k], got, x->hash[k]);
    wz3_geometry_free(&posed);
  }
  if (!c->oom_done) oom_sweep(c, mesh, &base);
  wz3_geometry_free(&base);
  return WZ3_OK;
}

int suite_wz3_pose_geometry(int argc, char **argv)
{
  if (argc < 3) {
    printf("usage: test_wz3_pose_geometry <assets dir> <fixtures dir>\n");
    return 2;
  }
  size_t kx_size;
  uint8_t *kx_bytes = gate_read_all(argv[1], "debris_party.kx", &kx_size);
  char *fixture = (char *)gate_read_all(argv[2], "pose_geometry.txt", NULL);
  wz3_kx *kx = NULL;
  size_t at;
  CHECK(wz3_kx_parse(&kx, kx_bytes, kx_size, &at) == WZ3_OK, "parse");
  if (!kx) return 1;
  ctx c;
  memset(&c, 0, sizeof c);
  c.n_ops = kx->n_ops;
  c.table = (expect *)calloc(kx->n_ops, sizeof(expect));
  unsigned lines = 0;
  for (char *line = strtok(fixture, "\n"); line; line = strtok(NULL, "\n")) {
    unsigned op, index, h;
    if (sscanf(line, "A %u %u %x", &op, &index, &h) == 3 && op < kx->n_ops && index < 5) {
      c.table[op].hash[index] = h;
      c.table[op].has[index] = 1;
      lines++;
    }
  }
  wz3_rt_options options;
  memset(&options, 0, sizeof options);
  options.generic_exec = 1;
  options.release_consumed = 1;
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
  printf("pose geometry: %u of %u posed geometries of %u animated meshes checked, %u differ\n", c.checked, lines, c.meshes, c.bad);
  CHECK(lines >= 600 && c.checked == lines && c.bad == 0 && c.oom_done, "%u of %u geometries, %u differ", c.checked, lines, c.bad);
  CHECK(g_live == 0, "%ld blocks leaked", g_live);
  printf("pose geometry: %d checks, %d failures\n", g_checks, g_failures);
  return g_failures ? 1 : 0;
}
