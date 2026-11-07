/* GenMesh check for wz3.h against the JavaScript port.
 *
 *   test_wz3_mesh <assets dir> <fixtures dir>
 *
 * Precalculates the party document with the C runtime (mesh handlers on, meshes released as soon as their
 * last consumer ran) and compares, for every mesh operator the C port produces, the counts and the
 * summary hash with fixtures/mesh_hashes.txt (tools/dump_mesh_hashes.mjs: the JS summary taken when the
 * handler returned).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* tracking allocator: live and peak bytes of everything the library allocates */
static size_t g_live_bytes, g_peak_bytes;
static long g_allocations;
static void *test_malloc(size_t n)
{
  size_t *p = (size_t *)malloc(n + 16);
  if (!p) return NULL;
  p[0] = n;
  g_live_bytes += n;
  if (g_live_bytes > g_peak_bytes) g_peak_bytes = g_live_bytes;
  g_allocations++;
  return (char *)p + 16;
}
static void test_free(void *q)
{
  if (!q) return;
  size_t *p = (size_t *)((char *)q - 16);
  g_live_bytes -= p[0];
  free(p);
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
  uint8_t *data = (uint8_t *)malloc((size_t)n + 1);
  if (!data || fread(data, 1, (size_t)n, f) != (size_t)n) exit(2);
  fclose(f);
  data[n] = 0;
  *size = (size_t)n;
  return data;
}

typedef struct expect {
  uint32_t cls, v, e, f, hash, normals_hash;
  int present, seen, has_normals;
} expect;

typedef struct ctx {
  expect *table;
  uint32_t n;
  uint32_t matched, mismatched, unexpected, normals_checked, normals_bad;
  uint32_t per_class_matched[256], per_class_total[256];
  const wz3_kx *kx;
  int verbose;
} ctx;

static wz3_result on_mesh(void *user, uint32_t op, const wz3_mesh *mesh)
{
  ctx *c = (ctx *)user;
  uint32_t v, e, f, h;
  if (wz3_mesh_summary(mesh, &v, &e, &f, &h)) {
    c->mismatched++;
    return WZ3_OK;
  }
  if (op >= c->n || !c->table[op].present) {
    c->unexpected++;
    return WZ3_OK;
  }
  expect *x = &c->table[op];
  x->seen = 1;
  if (x->v == v && x->e == e && x->f == f && x->hash == h) {
    c->matched++;
    c->per_class_matched[x->cls & 255]++;
  } else if (++c->mismatched <= 8) {
    printf("  op %u (class 0x%x): C %u/%u/%u hash %x, JS %u/%u/%u hash %x\n", op, x->cls, v, e, f, h, x->v, x->e, x->f, x->hash);
  }
  if (x->has_normals) {
    /* GenMesh::CalcNormals on a private copy (normals and UV-derived tangents) */
    wz3_mesh *copy = wz3__mesh_clone(mesh);
    uint32_t nh = 0;
    if (copy && !wz3__mesh_calc_normals(copy)) {
      uint32_t topology, vertex_hash, active;
      wz3__mesh_hashes(copy, &topology, &vertex_hash, &active);
      nh = (((0x811c9dc5u ^ topology) * 0x01000193u) ^ vertex_hash) * 0x01000193u;
    }
    wz3__mesh_free(copy);
    c->normals_checked++;
    if (nh != x->normals_hash && ++c->normals_bad <= 5) printf("  op %u (class 0x%x): CalcNormals C %x, JS %x\n", op, x->cls, nh, x->normals_hash);
  }
  return WZ3_OK;
}

int suite_wz3_mesh(int argc, char **argv)
{
  if (argc < 3) {
    printf("usage: test_wz3_mesh <assets dir> <fixtures dir>\n");
    return 2;
  }
  size_t kx_size, fx_size;
  uint8_t *kx_bytes = read_all(argv[1], "debris_party.kx", &kx_size);
  char *fixture = (char *)read_all(argv[2], "mesh_hashes.txt", &fx_size);
  wz3_kx *kx = NULL;
  size_t at;
  CHECK(wz3_kx_parse(&kx, kx_bytes, kx_size, &at) == WZ3_OK, "parse");
  if (!kx) return 1;

  ctx c;
  memset(&c, 0, sizeof c);
  c.n = kx->n_ops;
  c.kx = kx;
  c.table = (expect *)calloc(kx->n_ops, sizeof(expect));
  uint32_t total = 0;
  for (char *line = strtok(fixture, "\n"); line; line = strtok(NULL, "\n")) {
    unsigned op, cls, v, e, f, h;
    if (line[0] == 'Q' && sscanf(line, "Q %u %x", &op, &h) == 2 && op < kx->n_ops) {
      c.table[op].has_normals = 1;
      c.table[op].normals_hash = h;
      continue;
    }
    if (sscanf(line, "M %u %u %u %u %u %x", &op, &cls, &v, &e, &f, &h) != 6 || op >= kx->n_ops) continue;
    c.table[op].present = 1;
    c.table[op].cls = cls;
    c.table[op].v = v;
    c.table[op].e = e;
    c.table[op].f = f;
    c.table[op].hash = h;
    c.per_class_total[cls & 255]++;
    total++;
  }
  CHECK(total == 10391, "fixture lists %u mesh operators", total);

  wz3_rt_options options;
  memset(&options, 0, sizeof options);
  options.generic_exec = 1;
  options.release_consumed = 1;
  options.host.mesh = on_mesh;
  options.host.user = &c;
  wz3_result res;
  wz3_rt *rt = wz3_rt_create(kx, &options, &res);
  CHECK(rt != NULL && res == WZ3_OK, "create");
  if (rt) {
    res = wz3_rt_precalc(rt);
    CHECK(res == WZ3_OK, "precalc: %s", wz3_result_string(res));
    wz3_rt_destroy(rt);
  }

  printf("mesh: %u of %u operators match the JS port, %u mismatches, %u unexpected\n", c.matched, total, c.mismatched, c.unexpected);
  for (int k = 0; k < 256; k++)
    if (c.per_class_total[k]) printf("  class 0x%02x: %u / %u\n", k, c.per_class_matched[k], c.per_class_total[k]);
  printf("mesh: CalcNormals on %u copies, %u differ; library allocations %ld, peak %.1f MiB live\n", c.normals_checked, c.normals_bad, g_allocations,
         (double)g_peak_bytes / 1048576.0);
  CHECK(c.normals_checked >= 800 && c.normals_bad == 0, "%u of %u CalcNormals results differ", c.normals_bad, c.normals_checked);
  CHECK(c.mismatched == 0, "%u operators differ from the JS port", c.mismatched);
  CHECK(c.unexpected == 0, "%u meshes where JS produced none", c.unexpected);

  free(c.table);
  free(fixture);
  wz3_kx_destroy(kx);
  free(kx_bytes);
  printf("mesh: %d checks, %d failures\n", g_checks, g_failures);
  return g_failures ? 1 : 0;
}
