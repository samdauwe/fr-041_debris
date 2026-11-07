/* GenMinMesh, Mesh_ToMin and Font3D check for wz3.h against the JavaScript port.
 *
 *   test_wz3_minmesh <assets dir> <fixtures dir>
 *
 * Precalculates the party document with the C runtime (mesh and MinMesh handlers on, meshes released as soon
 * as their last consumer ran) and compares, for each of the 549 MinMesh operators, the counts and the hash
 * over every vertex attribute, polygon, cluster and bone record with fixtures/minmesh_hashes.txt
 * (tools/dump_minmesh_hashes.mjs: the JS state when the handler returned). Every seventh operator also
 * pins calcNormals on a copy.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* tracking allocator: live and peak bytes of everything the library allocates */
static size_t g_live_bytes, g_peak_bytes;
static long g_allocations;
static long g_fail_countdown = -1; /* >= 0: the allocation that finds it at zero fails */
static void *test_malloc(size_t n)
{
  if (g_fail_countdown == 0) {
    g_fail_countdown = -1;
    return NULL;
  }
  if (g_fail_countdown > 0) g_fail_countdown--;
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
  uint32_t cls, v, f, c, b, hash, normals_hash;
  uint32_t frame_hash[5];
  uint8_t has_frame[5];
  int present, seen, null_result, has_normals;
} expect;

static const double k_frame_times[5] = { 0, 0.37, 1.5, 4, 12.25 };

typedef struct ctx {
  expect *table;
  uint32_t n;
  uint32_t matched, mismatched, unexpected, normals_checked, normals_bad, nulls, frames_checked, frames_bad;
  const wz3_rt *rt;
  uint32_t per_class_matched[512], per_class_total[512];
  int verbose;
} ctx;

static wz3_result on_minmesh(void *user, uint32_t op, const wz3_minmesh *mesh)
{
  ctx *c = (ctx *)user;
  if (op >= c->n || !c->table[op].present) {
    c->unexpected++;
    return WZ3_OK;
  }
  expect *x = &c->table[op];
  x->seen = 1;
  if (!mesh) {
    if (x->null_result) {
      c->matched++;
      c->per_class_matched[x->cls & 511]++;
    } else if (++c->mismatched <= 8)
      printf("  op %u (class 0x%x): C produced no mesh, JS did\n", op, x->cls);
    return WZ3_OK;
  }
  uint32_t v, f, cl, b, h;
  wz3_minmesh_summary(mesh, &v, &f, &cl, &b, &h);
  if (!x->null_result && x->v == v && x->f == f && x->c == cl && x->b == b && x->hash == h) {
    c->matched++;
    c->per_class_matched[x->cls & 511]++;
  } else if (++c->mismatched <= 8) {
    printf("  op %u (class 0x%x): C %u v %u f %u c %u b hash %x, JS %u v %u f %u c %u b hash %x\n", op, x->cls, v, f, cl, b, h, x->v, x->f, x->c, x->b,
           x->hash);
  }
  for (int k = 0; k < 5; k++) {
    if (!x->has_frame[k]) continue;
    const uint32_t fh = wz3_rt_minmesh_frame_hash(c->rt, mesh, k_frame_times[k]);
    c->frames_checked++;
    if (fh != x->frame_hash[k] && ++c->frames_bad <= 5)
      printf("  op %u (class 0x%x): frame %d (t=%g) C %x, JS %x\n", op, x->cls, k, k_frame_times[k], fh, x->frame_hash[k]);
  }
  if (x->has_normals) {
    const uint32_t nh = wz3_minmesh_normals_hash(mesh);
    c->normals_checked++;
    if (nh != x->normals_hash && ++c->normals_bad <= 5) printf("  op %u (class 0x%x): calcNormals C %x, JS %x\n", op, x->cls, nh, x->normals_hash);
  }
  return WZ3_OK;
}


/* ---- synthetic cases (tools/dump_minmesh_synth.mjs) ---- */

typedef struct tok {
  const char *s;
} tok;

static uint64_t next_hex(tok *t)
{
  while (*t->s == ' ') t->s++;
  char *end;
  const uint64_t v = strtoull(t->s, &end, 16);
  t->s = end;
  return v;
}

static void skip_bar(tok *t)
{
  while (*t->s == ' ') t->s++;
  if (*t->s == '|') t->s++;
}

static float next_float(tok *t)
{
  const uint32_t u = (uint32_t)next_hex(t);
  float f;
  memcpy(&f, &u, 4);
  return f;
}

static double next_double(tok *t)
{
  const uint64_t u = next_hex(t);
  double d;
  memcpy(&d, &u, 8);
  return d;
}

static wz3_minmesh *parse_mesh(tok *t)
{
  wz3_minmesh *m = wz3__mm_new();
  if (!m) return NULL;
  const uint32_t nv = (uint32_t)next_hex(t), nf = (uint32_t)next_hex(t), nc = (uint32_t)next_hex(t), nb = (uint32_t)next_hex(t);
  const uint32_t flags = (uint32_t)next_hex(t);
  m->completely_rigid = (flags & 1) != 0;
  m->stripped = (flags & 2) != 0;
  m->normals_valid = (flags & 4) != 0;
  for (uint32_t i = 0; i < nv; i++) {
    wz3__mm_vertex *v = NULL;
    wz3__mm_push_vertex(m, NULL, &v);
    v->select = (int32_t)next_hex(t);
    v->bone_count = (int32_t)next_hex(t);
    v->temp_byte = (int32_t)next_hex(t);
    v->merge_tag = (int32_t)next_hex(t);
    v->color = (uint32_t)next_hex(t);
    for (int k = 0; k < 3; k++) v->position[k] = next_float(t);
    for (int k = 0; k < 3; k++) v->normal[k] = next_float(t);
    for (int k = 0; k < 3; k++) v->tangent[k] = next_float(t);
    for (int k = 0; k < 2; k++) v->uv[0][k] = next_float(t);
    for (int k = 0; k < 2; k++) v->uv[1][k] = next_float(t);
    for (int k = 0; k < 4; k++) v->weights[k] = next_float(t);
    for (int k = 0; k < 4; k++) v->matrices[k] = (uint16_t)next_hex(t);
  }
  for (uint32_t i = 0; i < nf; i++) {
    const int32_t select = (int32_t)next_hex(t), count = (int32_t)next_hex(t), cluster = (int32_t)next_hex(t);
    wz3__mm_face tmp;
    memset(&tmp, 0, sizeof tmp);
    tmp.select = select;
    tmp.cluster = cluster;
    tmp.temp = (int32_t)next_hex(t);
    tmp.flags = (uint32_t)next_hex(t);
    for (int k = 0; k < 3; k++) tmp.normal[k] = next_float(t);
    int32_t idx[64];
    for (int32_t k = 0; k < count; k++) idx[k] = (int32_t)next_hex(t);
    wz3__mm_push_face(m, idx, count, cluster, select, &tmp);
  }
  m->n_clusters = 0;
  for (uint32_t i = 0; i < nc; i++) {
    wz3__mm_cluster c;
    c.material = (int32_t)next_hex(t);
    c.render_pass = (int32_t)next_hex(t);
    c.id = (int32_t)next_hex(t);
    c.anim_type = (int32_t)next_hex(t);
    c.anim_matrix = (int32_t)next_hex(t);
    wz3__mm_push_cluster(m, c);
  }
  if (nb) {
    m->bones = (wz3__mm_bone *)WZ3_MALLOC((size_t)nb * sizeof(wz3__mm_bone));
    memset(m->bones, 0, (size_t)nb * sizeof(wz3__mm_bone));
    m->n_bones = nb;
    for (uint32_t i = 0; i < nb; i++) {
      wz3__mm_bone *b = &m->bones[i];
      for (int k = 0; k < 16; k++) b->base_pose[k] = next_float(t);
      for (int k = 0; k < 16; k++) b->no_animation[k] = next_float(t);
      b->parent = (int32_t)next_hex(t);
      b->factor = next_float(t);
      b->spread = next_float(t);
      b->offset = next_float(t);
      b->spline_kind = (int32_t)next_hex(t);
      if (b->spline_kind == WZ3__MM_SPLINE_EXTERNAL)
        b->spline_op = (int32_t)next_hex(t);
      else if (b->spline_kind == WZ3__MM_SPLINE_KINETIC) {
        for (int k = 0; k < 3; k++) b->kin_position[k] = next_float(t);
        for (int k = 0; k < 3; k++) b->kin_speed[k] = next_float(t);
        for (int k = 0; k < 3; k++) b->kin_gravity[k] = next_float(t);
        for (int k = 0; k < 3; k++) b->kin_rotation[k] = next_float(t);
        b->kin_has_axis = (int32_t)next_hex(t);
        if (b->kin_has_axis)
          for (int k = 0; k < 3; k++) b->kin_axis[k] = next_float(t);
        b->kin_angle = next_float(t);
      }
    }
  }
  return m;
}

static void format_result(char *out, size_t n, const wz3_minmesh *m)
{
  uint32_t v, f, c, b, h;
  if (!m) {
    snprintf(out, n, "Z");
    return;
  }
  wz3_minmesh_summary(m, &v, &f, &c, &b, &h);
  snprintf(out, n, "M %u %u %u %u %x", v, f, c, b, h);
}

/* ---- the GLU tessellator ---- */

typedef struct lt_sink {
  uint32_t n_vertices, n_tris, n_combined;
  uint32_t tris[3 * 8192];
  double combined[3 * 1024];
  int failed;
} lt_sink;

static int32_t lt_combine(void *user, const double c[3])
{
  lt_sink *s = (lt_sink *)user;
  if (s->n_combined >= 1024) {
    s->failed = 1;
    return -1;
  }
  memcpy(&s->combined[3 * s->n_combined++], c, 3 * sizeof(double));
  return (int32_t)s->n_vertices++;
}

static int lt_emit(void *user, int32_t a, int32_t b, int32_t c)
{
  lt_sink *s = (lt_sink *)user;
  if (a < 0 || b < 0 || c < 0 || (uint32_t)a >= s->n_vertices || (uint32_t)b >= s->n_vertices || (uint32_t)c >= s->n_vertices) {
    s->failed = 1;
    return -1;
  }
  if (s->n_tris >= 8192) {
    s->failed = 1;
    return -1;
  }
  s->tris[3 * s->n_tris] = (uint32_t)a;
  s->tris[3 * s->n_tris + 1] = (uint32_t)b;
  s->tris[3 * s->n_tris + 2] = (uint32_t)c;
  s->n_tris++;
  return 0;
}

static uint32_t fold(uint32_t h, uint32_t w) { return (h ^ w) * 0x01000193u; }

static void test_tessellation(const char *line, uint32_t *cases, uint32_t *bad)
{
  const char *arrow = strstr(line, " -> ");
  tok t = { line + 1 };
  const uint32_t n = (uint32_t)strtoul(t.s, (char **)&t.s, 10);
  uint32_t lengths[32];
  for (uint32_t i = 0; i < n && i < 32; i++) lengths[i] = (uint32_t)strtoul(t.s, (char **)&t.s, 10);
  skip_bar(&t);
  static lt_sink sink; /* too big for the stack; the test is single threaded */
  memset(&sink, 0, sizeof sink);
  wz3__lt_tess tess;
  memset(&tess, 0, sizeof tess);
  tess.user = &sink;
  tess.combine = lt_combine;
  tess.emit = lt_emit;
  int status = wz3__lt_begin_polygon(&tess);
  for (uint32_t c = 0; c < n; c++) {
    wz3__lt_begin_contour(&tess);
    for (uint32_t i = 0; i < lengths[c]; i++) {
      const double coords[3] = { next_float(&t), next_float(&t), 0.0 };
      wz3__lt_add_vertex(&tess, coords, (int32_t)sink.n_vertices++);
    }
  }
  status = status || wz3__lt_end_polygon(&tess) || sink.failed;
  char got[64];
  if (status)
    snprintf(got, sizeof got, "F");
  else {
    uint32_t h = 0x811c9dc5u;
    h = fold(h, sink.n_tris);
    for (uint32_t i = 0; i < sink.n_tris * 3; i++) h = fold(h, sink.tris[i]);
    h = fold(h, sink.n_combined);
    for (uint32_t i = 0; i < sink.n_combined * 3; i++) {
      uint64_t u;
      memcpy(&u, &sink.combined[i], 8);
      h = fold(h, (uint32_t)(u >> 32));
      h = fold(h, (uint32_t)u);
    }
    snprintf(got, sizeof got, "OK %u %x", sink.n_tris, h);
  }
  (*cases)++;
  if (strcmp(got, arrow + 4) != 0 && ++*bad <= 3) printf("tessellation differs\n  JS: %s\n  C : %s\n", arrow + 4, got);
}

static void hex_bytes(const char *s, char *out, size_t cap)
{
  size_t n = 0;
  if (*s != '-')
    while (s[0] && s[0] != ' ' && s[1] && n + 1 < cap) {
      char pair[3] = { s[0], s[1], 0 };
      out[n++] = (char)strtoul(pair, NULL, 16);
      s += 2;
    }
  out[n] = 0;
}

static void test_font(const char *line, uint32_t *cases, uint32_t *bad)
{
  const char *arrow = strstr(line, " -> ");
  tok t = { line + 1 };
  const double height = next_double(&t), extrude = next_double(&t), max_error = next_double(&t);
  while (*t.s == ' ') t.s++;
  char text[256], font[256];
  hex_bytes(t.s, text, sizeof text);
  while (*t.s && *t.s != ' ') t.s++;
  while (*t.s == ' ') t.s++;
  hex_bytes(t.s, font, sizeof font);
  wz3_result res;
  wz3_minmesh *m = wz3__mm_op_font3d(height, extrude, max_error, text, font, &res);
  char got[96];
  if (!m)
    snprintf(got, sizeof got, "E");
  else
    format_result(got, sizeof got, m);
  (*cases)++;
  if (strcmp(got, arrow + 4) != 0 && ++*bad <= 3) printf("font differs (text %s, font '%s')\n  JS: %s\n  C : %s\n", text, font, arrow + 4, got);
  wz3__mm_free(m);
}

static char g_pipe_case[16384]; /* the first synthetic pipe case that produced a mesh (for the allocation sweep) */

static void test_operator(const char *line, uint32_t *cases, uint32_t *bad, const char *name_out, char *failed_name)
{
  (void)name_out;
  const char *arrow = strstr(line, " -> ");
  char name[32];
  tok t = { line + 2 };
  size_t n = 0;
  while (*t.s == ' ') t.s++;
  while (*t.s && *t.s != ' ' && n + 1 < sizeof name) name[n++] = *t.s++;
  name[n] = 0;
  const uint32_t np = (uint32_t)strtoul(t.s, (char **)&t.s, 10);
  double p[32];
  memset(p, 0, sizeof p);
  for (uint32_t i = 0; i < np && i < 32; i++) p[i] = next_double(&t);
  const uint32_t nm = (uint32_t)strtoul(t.s, (char **)&t.s, 10);
  const uint32_t has_blob = (uint32_t)strtoul(t.s, (char **)&t.s, 10);
  wz3_minmesh *meshes[4] = { NULL, NULL, NULL, NULL };
  for (uint32_t i = 0; i < nm && i < 4; i++) {
    skip_bar(&t);
    meshes[i] = parse_mesh(&t);
  }
  wz3__rt_blob *blob = NULL;
  uint8_t blob_bytes[1024];
  size_t blob_len = 0;
  if (has_blob) {
    skip_bar(&t);
    while (*t.s == ' ') t.s++;
    while (t.s[0] && t.s[0] != ' ' && t.s[1] && blob_len < sizeof blob_bytes) {
      char pair[3] = { t.s[0], t.s[1], 0 };
      blob_bytes[blob_len++] = (uint8_t)strtoul(pair, NULL, 16);
      t.s += 2;
    }
    if (!strcmp(name, "pipe")) {
      wz3_result res;
      blob = wz3__rt_blob_from_pipe(blob_bytes, blob_len, &res);
    }
  }
  wz3_minmesh *out = NULL;
  int unsupported = 0;
  wz3_minmesh *m0 = meshes[0];
  /* Explode may carry a bitmap: width, height (16 bit little endian words) and the RGBA words */
  wz3_bitmap bitmap_storage, *texture = NULL;
  uint16_t texels[8 * 8 * 4];
  if (blob_len >= 4 && !strcmp(name, "explode")) {
    bitmap_storage.width = blob_bytes[0] | (blob_bytes[1] << 8);
    bitmap_storage.height = blob_bytes[2] | (blob_bytes[3] << 8);
    for (size_t i = 0; i < (size_t)bitmap_storage.width * bitmap_storage.height * 4 && i < 8 * 8 * 4; i++)
      texels[i] = (uint16_t)(blob_bytes[4 + 2 * i] | (blob_bytes[5 + 2 * i] << 8));
    bitmap_storage.data = texels;
    texture = &bitmap_storage;
  }
  const wz3_minmesh *valid[4];
  if (!strcmp(name, "grid"))
    out = wz3__mm_op_grid(wz3__rt_to_int32(p[0]), wz3__rt_to_int32(p[1]), wz3__rt_to_int32(p[2]));
  else if (!strcmp(name, "sphere"))
    out = wz3__mm_op_sphere(wz3__rt_to_int32(p[0]), wz3__rt_to_int32(p[1]));
  else if (!strcmp(name, "matlink"))
    out = wz3__mm_op_mat_link(m0, wz3__rt_to_int32(p[0]), wz3__rt_to_int32(p[1]), wz3__rt_to_int32(p[2]));
  else if (!strcmp(name, "add")) {
    for (uint32_t i = 0; i < nm; i++) valid[i] = meshes[i];
    out = wz3__mm_op_add(valid, nm);
  } else if (!strcmp(name, "selectcube"))
    out = wz3__mm_op_select_cube(m0, wz3__rt_to_int32(p[0]), p + 1);
  else if (!strcmp(name, "deletefaces"))
    out = wz3__mm_op_delete_faces(m0);
  else if (!strcmp(name, "transformex"))
    out = wz3__mm_op_transform_ex(m0, wz3__rt_to_int32(p[0]), p + 1, &unsupported);
  else if (!strcmp(name, "perlin"))
    out = wz3__mm_op_perlin(m0, wz3__rt_to_int32(p[0]), p + 1);
  else if (!strcmp(name, "bend2"))
    out = wz3__mm_op_bend2(m0, p);
  else if (!strcmp(name, "bonechain"))
    out = wz3__mm_op_bone_chain(m0, p);
  else if (!strcmp(name, "bonetrain"))
    out = wz3__mm_op_bone_train(m0, p[0] != 0.0 ? 77 : -1, p[1], wz3__rt_to_int32(p[2]), p[3]);
  else if (!strcmp(name, "triangulate"))
    out = wz3__mm_op_triangulate(m0);
  else if (!strcmp(name, "multiply"))
    out = wz3__mm_op_multiply(m0, p);
  else if (!strcmp(name, "center"))
    out = wz3__mm_op_center(m0, wz3__rt_to_int32(p[0]));
  else if (!strcmp(name, "explode"))
    out = wz3__mm_op_explode(m0, p, texture);
  else if (!strcmp(name, "pipe")) {
    int pipe_oom;
    out = wz3__mm_op_pipe(blob, m0, meshes[1], meshes[2], wz3__rt_to_int32(p[0]), p[1], p[2], p[3], &pipe_oom);
    CHECK(!pipe_oom, "pipe synthetic case reports out of memory");
    if (out && !g_pipe_case[0] && strlen(line) < sizeof g_pipe_case) strcpy(g_pipe_case, line);
  } else
    printf("unknown synthetic operator %s\n", name);
  char got[160], after[48] = "";
  format_result(got, sizeof got, out);
  if (m0) snprintf(after, sizeof after, " in %x", wz3__mm_hash(m0));
  strncat(got, after, sizeof got - strlen(got) - 1);
  (*cases)++;
  if (strcmp(got, arrow + 4) != 0) {
    if (++*bad <= 3) printf("operator %s differs\n  JS: %s\n  C : %s\n", name, arrow + 4, got);
    snprintf(failed_name, 32, "%.31s", name);
  }
  wz3__mm_free(out);
  for (uint32_t i = 0; i < 4; i++) wz3__mm_free(meshes[i]);
  wz3__rt_blob_free(blob);
}

static void test_to_min(const char *line, uint32_t *cases, uint32_t *bad)
{
  const char *arrow = strstr(line, " -> ");
  char kind[16];
  tok t = { line + 2 };
  size_t n = 0;
  while (*t.s == ' ') t.s++;
  while (*t.s && *t.s != ' ' && n + 1 < sizeof kind) kind[n++] = *t.s++;
  kind[n] = 0;
  const uint32_t np = (uint32_t)strtoul(t.s, (char **)&t.s, 10);
  double p[16];
  memset(p, 0, sizeof p);
  for (uint32_t i = 0; i < np && i < 16; i++) p[i] = next_double(&t);
  const int select_all = (int)strtoul(t.s, (char **)&t.s, 10);
  wz3_mesh *mesh = NULL;
  if (!strcmp(kind, "cube"))
    mesh = wz3__mesh_cube(wz3__rt_to_int32(p[0]), wz3__rt_to_int32(p[1]), wz3__rt_to_int32(p[2]), wz3__rt_to_int32(p[3]), p + 4);
  else if (!strcmp(kind, "cylinder"))
    mesh = wz3__mesh_cylinder(wz3__rt_to_int32(p[0]), wz3__rt_to_int32(p[1]), wz3__rt_to_int32(p[2]), wz3__rt_to_int32(p[3]), wz3__rt_to_int32(p[4]));
  else if (!strcmp(kind, "sphere"))
    mesh = wz3__mesh_sphere(wz3__rt_to_int32(p[0]), wz3__rt_to_int32(p[1]));
  else if (!strcmp(kind, "torus"))
    mesh = wz3__mesh_torus(wz3__rt_to_int32(p[0]), wz3__rt_to_int32(p[1]), p[2], p[3], p[4], p[5], wz3__rt_to_int32(p[6]));
  else if (!strcmp(kind, "grid"))
    mesh = wz3__mesh_grid(wz3__rt_to_int32(p[0]), wz3__rt_to_int32(p[1]), wz3__rt_to_int32(p[2]));
  char got[96];
  if (!mesh)
    snprintf(got, sizeof got, "E");
  else {
    if (select_all) wz3__mesh_all_to_selection(mesh, 1, WZ3__ALL_FACE);
    wz3_minmesh *m = wz3__mm_from_mesh(mesh);
    format_result(got, sizeof got, m);
    wz3__mm_free(m);
  }
  (*cases)++;
  if (strcmp(got, arrow + 4) != 0 && ++*bad <= 3) printf("ToMin %s differs\n  JS: %s\n  C : %s\n", kind, arrow + 4, got);
  wz3__mesh_free(mesh);
}

static void test_synthetic(const char *dir)
{
  size_t size;
  char *fixture = (char *)read_all(dir, "minmesh_synth.txt", &size);
  uint32_t cases[128], bad[128];
  memset(cases, 0, sizeof cases);
  memset(bad, 0, sizeof bad);
  char failed_operators[512] = "";
  for (char *line = fixture; *line;) {
    char *eol = strchr(line, '\n');
    if (eol) *eol = 0;
    const unsigned char kind = (unsigned char)line[0];
    if (kind == 'L')
      test_tessellation(line, &cases['L'], &bad['L']);
    else if (kind == 'F')
      test_font(line, &cases['F'], &bad['F']);
    else if (kind == 'A') {
      char failed[32] = "";
      test_operator(line, &cases['A'], &bad['A'], NULL, failed);
      if (failed[0] && !strstr(failed_operators, failed)) {
        strncat(failed_operators, failed, sizeof failed_operators - strlen(failed_operators) - 2);
        strcat(failed_operators, " ");
      }
    } else if (kind == 'T')
      test_to_min(line, &cases['T'], &bad['T']);
    if (!eol) break;
    line = eol + 1;
  }
  const char *labels = "LFAT";
  const char *names[] = { "GLU tessellator polygons", "Font3D strings", "operator cases", "Mesh_ToMin conversions" };
  for (int k = 0; k < 4; k++) {
    const unsigned char kind = (unsigned char)labels[k];
    printf("  synthetic %-26s %u cases, %u differ\n", names[k], cases[kind], bad[kind]);
    CHECK(cases[kind] > 0, "no synthetic %s cases", names[k]);
    CHECK(bad[kind] == 0, "%u of %u synthetic %s differ from the JS port", bad[kind], cases[kind], names[k]);
  }
  if (failed_operators[0]) printf("  failing operators: %s\n", failed_operators);
  free(fixture);
}


/* ---- Explode with the evaluated bitmap (tools/dump_explode_bitmap.mjs, texture offset -2) ---- */

typedef struct explode_ctx {
  wz3_graph *graph;
  expect table[64];
  uint32_t ops[64], n;
  uint32_t matched, mismatched;
} explode_ctx;

static const wz3_bitmap *explode_bitmap(void *user, uint32_t op)
{
  explode_ctx *e = (explode_ctx *)user;
  const wz3_bitmap *bitmap = NULL;
  if (wz3_graph_eval_bitmap(e->graph, op, &bitmap) != WZ3_OK) return NULL;
  return bitmap;
}

static wz3_result explode_check(void *user, uint32_t op, const wz3_minmesh *mesh)
{
  explode_ctx *e = (explode_ctx *)user;
  for (uint32_t i = 0; i < e->n; i++) {
    if (e->ops[i] != op) continue;
    uint32_t v = 0, f = 0, cl = 0, b = 0, h = 0;
    if (mesh) wz3_minmesh_summary(mesh, &v, &f, &cl, &b, &h);
    if (mesh && e->table[i].v == v && e->table[i].f == f && e->table[i].c == cl && e->table[i].b == b && e->table[i].hash == h)
      e->matched++;
    else {
      e->mismatched++;
      printf("  Explode op %u: C %u/%u/%u/%u hash %x, JS %u/%u/%u/%u hash %x\n", op, v, f, cl, b, h, e->table[i].v, e->table[i].f, e->table[i].c, e->table[i].b,
             e->table[i].hash);
    }
  }
  return WZ3_OK;
}

static void test_explode_bitmap(const wz3_kx *kx, const char *dir)
{
  size_t size;
  char *fixture = (char *)read_all(dir, "explode_bitmap_offset_m2.txt", &size);
  static explode_ctx e;
  memset(&e, 0, sizeof e);
  for (char *line = strtok(fixture, "\n"); line && e.n < 64; line = strtok(NULL, "\n")) {
    unsigned op, cls, v, f, cl, b, h;
    if (sscanf(line, "X %u %u %u %u %u %u %x", &op, &cls, &v, &f, &cl, &b, &h) != 7) continue;
    e.ops[e.n] = op;
    e.table[e.n].v = v;
    e.table[e.n].f = f;
    e.table[e.n].c = cl;
    e.table[e.n].b = b;
    e.table[e.n].hash = h;
    e.n++;
  }
  CHECK(e.n == 2, "fixture lists %u Explode operators with a bitmap", e.n);
  wz3_graph_options go;
  memset(&go, 0, sizeof go);
  wz3_bitmap_ctx_init(&go.ctx);
  go.ctx.texture_offset = -2;
  wz3_result res;
  e.graph = wz3_graph_create(kx, &go, &res);
  CHECK(e.graph != NULL, "graph");
  wz3_rt_options options;
  memset(&options, 0, sizeof options);
  options.generic_exec = 1;
  options.release_consumed = 1;
  options.host.minmesh = explode_check;
  options.host.bitmap = explode_bitmap;
  options.host.user = &e;
  wz3_rt *rt = wz3_rt_create(kx, &options, &res);
  CHECK(rt != NULL && res == WZ3_OK, "create");
  if (rt && e.graph) {
    res = wz3_rt_precalc(rt);
    CHECK(res == WZ3_OK, "precalc: %s", wz3_result_string(res));
  }
  wz3_rt_destroy(rt);
  wz3_graph_destroy(e.graph);
  printf("minmesh: Explode with the evaluated bitmap: %u of %u match the JS port\n", e.matched, e.n);
  CHECK(e.matched == e.n && e.mismatched == 0, "%u Explode operators differ with their bitmap", e.mismatched);
  free(fixture);
}


/* ---- allocation failure: every allocation of a few representative operators fails in turn; nothing may leak or crash ---- */

/* The pipe operator with each of its allocations failing in turn: an empty result must say whether it is out of memory (the
   runtime reports that as an error) or the JS handler's null (no result, not an error), and nothing may leak. */
static void test_pipe_allocation_failure(void)
{
  CHECK(g_pipe_case[0] != 0, "no synthetic pipe case produced a mesh");
  if (!g_pipe_case[0]) return;
  const size_t baseline = g_live_bytes;
  uint32_t points = 0, oom = 0;
  for (long fail_at = 0; fail_at < 4000; fail_at++) {
    tok t = { g_pipe_case + 2 };
    char name[32];
    size_t n = 0;
    while (*t.s == ' ') t.s++;
    while (*t.s && *t.s != ' ' && n + 1 < sizeof name) name[n++] = *t.s++;
    name[n] = 0;
    const uint32_t np = (uint32_t)strtoul(t.s, (char **)&t.s, 10);
    double p[32];
    memset(p, 0, sizeof p);
    for (uint32_t i = 0; i < np && i < 32; i++) p[i] = next_double(&t);
    const uint32_t nm = (uint32_t)strtoul(t.s, (char **)&t.s, 10);
    (void)strtoul(t.s, (char **)&t.s, 10); /* has_blob: a pipe case always has one */
    wz3_minmesh *meshes[4] = { NULL, NULL, NULL, NULL };
    for (uint32_t i = 0; i < nm && i < 4; i++) {
      skip_bar(&t);
      meshes[i] = parse_mesh(&t);
    }
    skip_bar(&t);
    while (*t.s == ' ') t.s++;
    uint8_t bytes[1024];
    size_t length = 0;
    while (t.s[0] && t.s[0] != ' ' && t.s[1] && length < sizeof bytes) {
      const char pair[3] = { t.s[0], t.s[1], 0 };
      bytes[length++] = (uint8_t)strtoul(pair, NULL, 16);
      t.s += 2;
    }
    wz3_result res;
    wz3__rt_blob *blob = wz3__rt_blob_from_pipe(bytes, length, &res);
    int pipe_oom = 0;
    g_fail_countdown = fail_at;
    wz3_minmesh *out = wz3__mm_op_pipe(blob, meshes[0], meshes[1], meshes[2], wz3__rt_to_int32(p[0]), p[1], p[2], p[3], &pipe_oom);
    const int exhausted = g_fail_countdown >= 0; /* the failing allocation was never reached */
    g_fail_countdown = -1;
    points++;
    if (out && !exhausted) CHECK(0, "pipe: failing allocation %ld still produced a mesh", fail_at);
    if (!out && !exhausted) {
      oom++;
      CHECK(pipe_oom, "pipe: failing allocation %ld is not reported as out of memory", fail_at);
    }
    if (exhausted) CHECK(out && !pipe_oom, "pipe: the unfailed run produced no mesh");
    wz3__mm_free(out);
    for (uint32_t i = 0; i < 4; i++) wz3__mm_free(meshes[i]);
    wz3__rt_blob_free(blob);
    if (g_live_bytes != baseline) {
      CHECK(0, "pipe: failing allocation %ld leaked %zu bytes", fail_at, g_live_bytes - baseline);
      break;
    }
    if (exhausted) break;
  }
  CHECK(oom > 10, "only %u pipe failure points", oom);
  printf("minmesh: pipe allocation failure: %u points, %u reported as out of memory, nothing leaked\n", points, oom);
}

static void test_allocation_failure(void)
{
  uint32_t runs = 0, produced = 0;
  const size_t baseline = g_live_bytes;
  for (int kind = 0; kind < 5; kind++) {
    for (long fail_at = 0; fail_at < 4000; fail_at++) {
      wz3_minmesh *m = NULL;
      wz3_mesh *mesh = NULL;
      wz3_result res;
      int ready = 1; /* false when the setup of the operator input already hit the failure */
      g_fail_countdown = fail_at;
      switch (kind) {
      case 0: m = wz3__mm_op_font3d(1.0, 0.1, 0.05, "oe", "georgia", &res); break;
      case 1: {
        wz3_minmesh *grid = wz3__mm_op_grid(1, 4, 3);
        ready = grid != NULL;
        g_fail_countdown = fail_at;
        if (grid) {
          const double srt[18] = { 1, 1, 1, 0, 0, 0, 0.5, 0, 0, 3, 3, 0.5, 0.5, 0, 0, 0, 0, 0 };
          m = wz3__mm_op_multiply(grid, srt);
          wz3__mm_free(grid);
        }
        break;
      }
      case 2: {
        mesh = wz3__mesh_cylinder(12, 1, 3, 1, 0);
        if (mesh) wz3__mesh_all_to_selection(mesh, 1, WZ3__ALL_FACE);
        ready = mesh != NULL;
        g_fail_countdown = fail_at;
        if (mesh) m = wz3__mm_from_mesh(mesh);
        break;
      }
      case 3: {
        wz3_minmesh *grid = wz3__mm_op_grid(0, 3, 3);
        ready = grid != NULL;
        g_fail_countdown = fail_at;
        if (grid) {
          const double p[21] = { 0, 0, 0, 0.1, 1, 1, -1, 0.5, 1, 1, 1, 0.5, 1, 0.5, 0.1, 6, 1, 0, 0, 0.5, 0.5 };
          m = wz3__mm_op_explode(grid, p, NULL);
          wz3__mm_free(grid);
        }
        break;
      }
      default: {
        wz3_minmesh *grid = wz3__mm_op_grid(1, 3, 3);
        ready = grid != NULL;
        g_fail_countdown = fail_at;
        if (grid) {
          const double p[8] = { 0, 0, 0, 0, 0, 1, 4, 0 };
          wz3_minmesh *chain = wz3__mm_op_bone_chain(grid, p);
          m = chain ? wz3__mm_op_triangulate(chain) : NULL;
          wz3__mm_free(chain);
          wz3__mm_free(grid);
        }
        break;
      }
      }
      const int exhausted = ready && g_fail_countdown >= 0; /* the allocation to fail was never reached: all calls ran */
      g_fail_countdown = -1;
      runs++;
      if (m && !exhausted) { CHECK(0, "kind %d: failing allocation %ld still produced a mesh", kind, fail_at); }
      if (kind == 0 && !exhausted) CHECK(!m && res == WZ3_ERR_OUT_OF_MEMORY, "Font3D: failing allocation %ld reported as %s", fail_at, wz3_result_string(res));
      if (exhausted && m) produced++;
      wz3__mm_free(m);
      wz3__mesh_free(mesh);
      if (g_live_bytes != baseline) {
        CHECK(0, "kind %d, failing allocation %ld leaked %zu bytes", kind, fail_at, g_live_bytes - baseline);
        break;
      }
      if (exhausted) {
        CHECK(m != NULL, "kind %d: the unfailed run produced no mesh", kind);
        break;
      }
    }
  }
  printf("minmesh: allocation failure: %u runs over 5 operators, every injected failure reported and nothing leaked (%u reference runs ok)\n", runs, produced);
}

int suite_wz3_minmesh(int argc, char **argv)
{
  if (argc < 3) {
    printf("usage: test_wz3_minmesh <assets dir> <fixtures dir>\n");
    return 2;
  }
  size_t kx_size, fx_size;
  uint8_t *kx_bytes = read_all(argv[1], "debris_party.kx", &kx_size);
  char *fixture = (char *)read_all(argv[2], "minmesh_hashes.txt", &fx_size);
  wz3_kx *kx = NULL;
  size_t at;
  CHECK(wz3_kx_parse(&kx, kx_bytes, kx_size, &at) == WZ3_OK, "parse");
  if (!kx) return 1;

  ctx c;
  memset(&c, 0, sizeof c);
  c.n = kx->n_ops;
  c.table = (expect *)calloc(kx->n_ops, sizeof(expect));
  uint32_t total = 0;
  for (char *line = strtok(fixture, "\n"); line; line = strtok(NULL, "\n")) {
    unsigned op, cls, v, f, cl, b, h;
    if (line[0] == 'Q' && sscanf(line, "Q %u %x", &op, &h) == 2 && op < kx->n_ops) {
      c.table[op].has_normals = 1;
      c.table[op].normals_hash = h;
      continue;
    }
    if (line[0] == 'P') {
      unsigned index;
      if (sscanf(line, "P %u %u %x", &op, &index, &h) == 3 && op < kx->n_ops && index < 5) {
        c.table[op].frame_hash[index] = h;
        c.table[op].has_frame[index] = 1;
      }
      continue;
    }
    if (line[0] == 'Z' && sscanf(line, "Z %u %u", &op, &cls) == 2 && op < kx->n_ops) {
      c.table[op].present = 1;
      c.table[op].null_result = 1;
      c.table[op].cls = cls;
      c.per_class_total[cls & 511]++;
      c.nulls++;
      total++;
      continue;
    }
    if (sscanf(line, "M %u %u %u %u %u %u %x", &op, &cls, &v, &f, &cl, &b, &h) != 7 || op >= kx->n_ops) continue;
    c.table[op].present = 1;
    c.table[op].cls = cls;
    c.table[op].v = v;
    c.table[op].f = f;
    c.table[op].c = cl;
    c.table[op].b = b;
    c.table[op].hash = h;
    c.per_class_total[cls & 511]++;
    total++;
  }
  CHECK(total == 549, "fixture lists %u MinMesh operators", total);

  wz3_rt_options options;
  memset(&options, 0, sizeof options);
  options.generic_exec = 1;
  options.release_consumed = 1;
  options.host.minmesh = on_minmesh;
  options.host.user = &c;
  wz3_result res;
  wz3_rt *rt = wz3_rt_create(kx, &options, &res);
  CHECK(rt != NULL && res == WZ3_OK, "create");
  c.rt = rt;
  if (rt) {
    res = wz3_rt_precalc(rt);
    CHECK(res == WZ3_OK, "precalc: %s", wz3_result_string(res));
    wz3_rt_destroy(rt);
  }

  printf("minmesh: %u of %u operators match the JS port, %u mismatches, %u unexpected\n", c.matched, total, c.mismatched, c.unexpected);
  for (int k = 0; k < 512; k++)
    if (c.per_class_total[k]) printf("  class 0x%03x: %u / %u\n", k, c.per_class_matched[k], c.per_class_total[k]);
  printf("minmesh: calcNormals on %u copies, %u differ; library allocations %ld, peak %.1f MiB live\n", c.normals_checked, c.normals_bad, g_allocations,
         (double)g_peak_bytes / 1048576.0);
  printf("minmesh: %u posed and skinned frames of animated meshes checked, %u differ\n", c.frames_checked, c.frames_bad);
  CHECK(c.frames_checked == 655 && c.frames_bad == 0, "%u of %u animated frames differ", c.frames_bad, c.frames_checked);
  CHECK(c.normals_checked >= 70 && c.normals_bad == 0, "%u of %u calcNormals results differ", c.normals_bad, c.normals_checked);
  CHECK(c.mismatched == 0, "%u operators differ from the JS port", c.mismatched);
  CHECK(c.unexpected == 0, "%u meshes where JS produced none", c.unexpected);
  CHECK(c.matched == total, "%u of %u operators matched", c.matched, total);

  test_synthetic(argv[2]);
  test_explode_bitmap(kx, argv[2]);
  test_allocation_failure();
  test_pipe_allocation_failure();

  free(c.table);
  free(fixture);
  wz3_kx_destroy(kx);
  free(kx_bytes);
  printf("minmesh: %d checks, %d failures\n", g_checks, g_failures);
  return g_failures ? 1 : 0;
}
