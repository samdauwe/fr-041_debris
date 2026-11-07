/* Tests for wz3.h: core math vs JS-derived vectors, the KX parser, the bitmap graph evaluator vs the JS port's per-operator
 * hashes for the complete released Debris graph, robustness (fuzzing) and allocation failure injection. The comparisons with
 * the C++ sources live in tests/oracle/. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* counting allocator with fault injection */
static size_t g_live;
static long g_alloc_count;
static long g_fail_at = -1; /* fail the allocation with this index (0-based), -1: never */
static void *test_malloc(size_t n)
{
  const long index = g_alloc_count++;
  if (index == g_fail_at) return NULL;
  size_t *p = (size_t *)malloc(n + 16);
  if (!p) return NULL;
  p[0] = n;
  g_live++;
  return (char *)p + 16;
}
#if defined(__GNUC__)
__attribute__((noinline)) /* inlined into a NULL-checked caller, gcc misreports the header arithmetic */
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
      if (g_failures <= 40) {                       \
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
  long n = ftell(f);
  fseek(f, 0, SEEK_SET);
  uint8_t *data = (uint8_t *)malloc((size_t)n + 1);
  if (!data || fread(data, 1, (size_t)n, f) != (size_t)n) {
    printf("cannot read %s\n", path);
    exit(2);
  }
  fclose(f);
  data[n] = 0;
  *size = (size_t)n;
  return data;
}

static uint32_t f2u(float f)
{
  uint32_t u;
  memcpy(&u, &f, 4);
  return u;
}
static float u2f(uint32_t u)
{
  float f;
  memcpy(&f, &u, 4);
  return f;
}
static uint64_t d2u(double d)
{
  uint64_t u;
  memcpy(&u, &d, 8);
  return u;
}
static double u2d(uint64_t u)
{
  double d;
  memcpy(&d, &u, 8);
  return d;
}

/* ---- fixture line helpers ---------------------------------------------------------------------- */

typedef struct lines {
  char *text;
  char *cursor;
} lines;
static char *next_line(lines *l)
{
  if (!*l->cursor) return NULL;
  char *start = l->cursor;
  char *nl = strchr(start, '\n');
  if (nl) {
    *nl = 0;
    l->cursor = nl + 1;
  } else
    l->cursor = start + strlen(start);
  return start;
}

/* tokenizes a line into hex numbers after the first word; returns the count */
static int hex_tokens(char *line, uint64_t *out, int max)
{
  int n = 0;
  char *p = strchr(line, ' ');
  while (p && n < max) {
    p++;
    char *end;
    out[n++] = strtoull(p, &end, 16);
    p = *end == ' ' ? end : NULL;
  }
  return n;
}

/* ---- core vectors ------------------------------------------------------------------------------ */

static void test_core(const char *dir)
{
  size_t size;
  lines l;
  l.text = l.cursor = (char *)read_all(dir, "core_vectors.txt", &size);
  float m[4][16];
  int n_m = 0;
  char *line;
  int n_lines = 0;
  while ((line = next_line(&l)) != NULL) {
    uint64_t t[80] = { 0 };
    char kind[32];
    sscanf(line, "%31s", kind);
    const int n = hex_tokens(line, t, 80);
    n_lines++;
    if (!strcmp(kind, "rng_default")) {
      wz3_rng r;
      wz3_rng_init(&r);
      for (int i = 0; i < n; i++) CHECK(wz3_rng_u32(&r) == (uint32_t)t[i], "rng_default %d", i);
    } else if (!strcmp(kind, "rng_seed")) {
      wz3_rng r;
      wz3_rng_init(&r);
      wz3_rng_seed(&r, (int32_t)(uint32_t)t[0]);
      for (int i = 1; i < n; i++) CHECK(wz3_rng_u32(&r) == (uint32_t)t[i], "rng_seed %llx #%d", (unsigned long long)t[0], i);
    } else if (!strcmp(kind, "rng_int")) {
      wz3_rng r;
      wz3_rng_init(&r);
      wz3_rng_seed(&r, (int32_t)(uint32_t)t[0]);
      for (int i = 2; i < n; i++)
        CHECK(wz3_rng_int(&r, (uint32_t)t[1]) == (uint32_t)t[i], "rng_int %llx %llx #%d", (unsigned long long)t[0], (unsigned long long)t[1], i);
    } else if (!strcmp(kind, "rng_float")) {
      wz3_rng r;
      wz3_rng_init(&r);
      wz3_rng_seed(&r, (int32_t)(uint32_t)t[0]);
      for (int i = 2; i < n; i++) CHECK(f2u(wz3_rng_float(&r, u2f((uint32_t)t[1]))) == (uint32_t)t[i], "rng_float %llx #%d", (unsigned long long)t[0], i);
    } else if (!strcmp(kind, "pow")) {
      const double got = wz3_pow(u2d(t[0]), u2d(t[1]));
      CHECK(d2u(got) == t[2], "pow(%a, %a) = %a, JS %a", u2d(t[0]), u2d(t[1]), got, u2d(t[2]));
    } else if (!strcmp(kind, "sincos")) {
      float s, c;
      wz3_sincos(u2f((uint32_t)t[0]), &s, &c);
      CHECK(f2u(s) == (uint32_t)t[1] && f2u(c) == (uint32_t)t[2], "sincos(%g)", (double)u2f((uint32_t)t[0]));
    } else {
      /* matrix and vector lines: floats only */
      float v[80];
      for (int i = 0; i < n; i++) v[i] = u2f((uint32_t)t[i]);
      float o[16];
      int ok = 1;
      if (!strcmp(kind, "mat_identity")) {
        wz3_mat4_identity(o);
        ok = !memcmp(o, v, sizeof o);
      } else if (!strcmp(kind, "mat_srt")) {
        wz3_mat4_srt(v, o);
        ok = !memcmp(o, v + 9, sizeof o);
        if (n_m < 4) {
          memcpy(m[n_m], o, sizeof o);
          n_m++;
        }
      } else if (!strcmp(kind, "mat_euler")) {
        wz3_mat4_euler(v[0], v[1], v[2], o);
        ok = !memcmp(o, v + 3, sizeof o);
      } else if (!strcmp(kind, "mat_euler_turns")) {
        wz3_mat4_euler_turns(v, o);
        ok = !memcmp(o, v + 3, sizeof o);
      } else if (!strcmp(kind, "mat_mul") || !strcmp(kind, "mat_mul_a") || !strcmp(kind, "mat_mul3")) {
        /* the two leading indices were printed in hex too, but they are < 10 */
        const int i = (int)t[0], j = (int)t[1];
        float *expect = v + 2;
        if (!strcmp(kind, "mat_mul"))
          wz3_mat4_mul(m[i], m[j], o);
        else if (!strcmp(kind, "mat_mul_a"))
          wz3_mat4_mul_a(m[i], m[j], o);
        else
          wz3_mat4_mul3(m[i], m[j], o);
        ok = !memcmp(o, expect, sizeof o);
        /* aliasing: out == a, and out == b */
        float a[16], b[16];
        memcpy(a, m[i], sizeof a);
        memcpy(b, m[j], sizeof b);
        if (!strcmp(kind, "mat_mul"))
          wz3_mat4_mul(a, b, a);
        else if (!strcmp(kind, "mat_mul_a"))
          wz3_mat4_mul_a(a, b, a);
        else
          wz3_mat4_mul3(a, b, a);
        ok = ok && !memcmp(a, expect, sizeof a);
        memcpy(a, m[i], sizeof a);
        memcpy(b, m[j], sizeof b);
        if (!strcmp(kind, "mat_mul"))
          wz3_mat4_mul(a, b, b);
        else if (!strcmp(kind, "mat_mul_a"))
          wz3_mat4_mul_a(a, b, b);
        else
          wz3_mat4_mul3(a, b, b);
        ok = ok && !memcmp(b, expect, sizeof b);
      } else if (!strcmp(kind, "mat_direction")) {
        wz3_mat4_direction(v, o);
        ok = !memcmp(o, v + 3, sizeof o);
      } else if (!strcmp(kind, "transform")) {
        float r[4];
        wz3_mat4_transform_point(m[(int)t[0]], v + 1, r);
        ok = !memcmp(r, v + 5, sizeof r);
      } else if (!strcmp(kind, "cross")) {
        float r[3];
        wz3_vec3_cross(v, v + 3, r);
        ok = !memcmp(r, v + 6, sizeof r);
      } else if (!strcmp(kind, "normalize")) {
        float r[3];
        wz3_vec3_normalize(v, r);
        ok = !memcmp(r, v + 3, sizeof r);
      } else if (!strcmp(kind, "normalize_safe")) {
        float r[3];
        wz3_vec3_normalize_safe(v, r);
        ok = !memcmp(r, v + 3, sizeof r);
      } else {
        CHECK(0, "unknown core vector kind %s", kind);
      }
      CHECK(ok, "core vector mismatch: %.60s", line);
    }
  }
  CHECK(n_lines > 300, "core fixture has %d lines", n_lines);
  free(l.text);

  /* sFtol is round-to-nearest-even with INT32_MIN as the x87 indefinite value */
  CHECK(wz3_ftol(0.5) == 0 && wz3_ftol(1.5) == 2 && wz3_ftol(2.5) == 2 && wz3_ftol(-0.5) == 0 && wz3_ftol(-1.5) == -2 && wz3_ftol(-2.5) == -2 &&
          wz3_ftol(0.4999) == 0 && wz3_ftol(0.5001) == 1,
        "ftol rounding");
  CHECK(wz3_ftol(3e9) == INT32_MIN && wz3_ftol(-3e9) == INT32_MIN && wz3_ftol(NAN) == INT32_MIN && wz3_ftol(2147483647.0) == 2147483647 &&
          wz3_ftol(-2147483648.0) == INT32_MIN,
        "ftol range");

  wz3_rng r;
  wz3_rng_init(&r);
  CHECK(wz3_rng_int(&r, 0) == 0, "rng_int(0) is 0");
}

/* ---- KX parser --------------------------------------------------------------------------------- */

typedef struct fnv {
  uint32_t h;
} fnv;
static void fnv_byte(fnv *f, uint32_t b) { f->h = (f->h ^ (b & 255u)) * 0x01000193u; }
static void fnv_u32(fnv *f, uint32_t v)
{
  fnv_byte(f, v);
  fnv_byte(f, v >> 8);
  fnv_byte(f, v >> 16);
  fnv_byte(f, v >> 24);
}
static void fnv_bytes(fnv *f, const uint8_t *p, size_t n)
{
  for (size_t i = 0; i < n; i++) fnv_byte(f, p[i]);
}

/* the same stream as tools/dump_kx_fingerprint.mjs */
static uint32_t kx_fingerprint(const wz3_kx *kx)
{
  fnv h = { 0x811c9dc5u };
  fnv_u32(&h, kx->flags);
  fnv_u32(&h, kx->song_size);
  fnv_u32(&h, kx->sample_size);
  fnv_u32(&h, kx->song_bpm_fixed);
  fnv_u32(&h, kx->song_length);
  fnv_u32(&h, kx->n_ops);
  fnv_u32(&h, kx->n_events);
  fnv_u32(&h, kx->n_splines);
  fnv_u32(&h, kx->n_classes);
  for (int i = 0; i < WZ3_MAX_OP_ROOT; i++) fnv_u32(&h, kx->roots[i]);
  const wz3_kx_sections *s = &kx->sections;
  fnv_u32(&h, s->header);
  fnv_u32(&h, s->song);
  fnv_u32(&h, s->samples);
  fnv_u32(&h, s->document);
  fnv_u32(&h, s->graph);
  fnv_u32(&h, s->parameters);
  fnv_u32(&h, s->animations);
  fnv_u32(&h, s->events);
  fnv_u32(&h, s->splines);
  fnv_u32(&h, s->blobs);
  fnv_u32(&h, s->end);
  fnv song = { 0x811c9dc5u };
  fnv_bytes(&song, kx->song, kx->song_size);
  fnv_u32(&h, song.h);
  fnv samples = { 0x811c9dc5u };
  fnv_bytes(&samples, kx->samples, kx->sample_size);
  fnv_u32(&h, samples.h);

  for (uint32_t c = 0; c < kx->n_classes; c++) {
    const wz3_kx_class *cls = &kx->classes[c];
    fnv_u32(&h, cls->convention);
    fnv_u32(&h, cls->id);
    fnv_u32(&h, cls->packing_length);
    for (uint32_t i = 0; i < cls->packing_length; i++) fnv_u32(&h, (uint8_t)cls->packing[i]);
  }
  for (uint32_t o = 0; o < kx->n_ops; o++) {
    const wz3_kx_op *op = &kx->ops[o];
    const wz3_kx_class *cls = &kx->classes[op->class_index];
    fnv_u32(&h, op->class_index);
    fnv_u32(&h, op->n_inputs);
    for (uint32_t i = 0; i < op->n_inputs; i++) fnv_u32(&h, op->inputs[i]);
    fnv_u32(&h, op->n_links);
    for (uint32_t i = 0; i < op->n_links; i++) fnv_u32(&h, (uint32_t)op->links[i]);
    for (uint32_t i = 0; i < cls->packing_length; i++) fnv_u32(&h, op->params[i].u);
    fnv_u32(&h, op->n_strings);
    for (uint32_t i = 0; i < op->n_strings; i++) {
      const size_t len = strlen(op->strings[i]);
      fnv_u32(&h, (uint32_t)len);
      for (size_t k = 0; k < len; k++) fnv_u32(&h, (uint8_t)op->strings[i][k]);
    }
    fnv_u32(&h, op->n_splines);
    for (uint32_t i = 0; i < op->n_splines; i++) fnv_u32(&h, (uint32_t)op->splines[i]);
    fnv_u32(&h, op->blob_size);
    if (op->blob) fnv_bytes(&h, op->blob, op->blob_size);
    fnv_u32(&h, op->anim_size);
    fnv_bytes(&h, op->anim, op->anim_size);
  }
  for (uint32_t i = 0; i < kx->n_events; i++) {
    const wz3_kx_event *e = &kx->events[i];
    fnv_u32(&h, e->operation);
    fnv_u32(&h, (uint32_t)e->start);
    fnv_u32(&h, (uint32_t)e->end);
    fnv_u32(&h, f2u(e->velocity));
    fnv_u32(&h, f2u(e->modulation));
    fnv_u32(&h, e->select);
    for (int k = 0; k < 3; k++) fnv_u32(&h, f2u(e->scale[k]));
    for (int k = 0; k < 3; k++) fnv_u32(&h, f2u(e->rotate[k]));
    for (int k = 0; k < 3; k++) fnv_u32(&h, f2u(e->translate[k]));
    fnv_u32(&h, e->color);
    fnv_u32(&h, (uint32_t)e->spline);
    fnv_u32(&h, f2u(e->start_interval));
    fnv_u32(&h, f2u(e->end_interval));
    fnv_u32(&h, e->flags);
  }
  for (uint32_t i = 0; i < kx->n_splines; i++) {
    const wz3_kx_spline *sp = &kx->splines[i];
    fnv_u32(&h, sp->interpolation);
    fnv_u32(&h, sp->n_channels);
    for (uint32_t c = 0; c < sp->n_channels; c++) {
      fnv_u32(&h, sp->channels[c].n_keys);
      for (uint32_t k = 0; k < sp->channels[c].n_keys; k++) {
        fnv_u32(&h, sp->channels[c].key_time[k]);
        fnv_u32(&h, f2u(sp->channels[c].key_value[k]));
      }
    }
  }
  return h.h;
}

static void test_kx(const wz3_kx *kx, const char *fixtures, size_t kx_size)
{
  size_t size;
  lines l;
  l.text = l.cursor = (char *)read_all(fixtures, "kx_fingerprint.txt", &size);
  char expect[64][160];
  int n = 0;
  char *line;
  while ((line = next_line(&l)) && n < 64) snprintf(expect[n++], sizeof expect[0], "%s", line);
  free(l.text);

  char got[64][160];
  int g = 0;
#define LINE(...) snprintf(got[g++], sizeof got[0], __VA_ARGS__)
  LINE("flags %u", kx->flags);
  LINE("name %s", kx->name);
  LINE("song_size %u", kx->song_size);
  LINE("sample_size %u", kx->sample_size);
  LINE("song_bpm_fixed %u", kx->song_bpm_fixed);
  LINE("song_length %u", kx->song_length);
  LINE("n_classes %u", kx->n_classes);
  LINE("n_ops %u", kx->n_ops);
  LINE("n_events %u", kx->n_events);
  LINE("n_splines %u", kx->n_splines);
  LINE("size %u", (unsigned)kx_size);
  LINE("section_header %u", kx->sections.header);
  LINE("section_song %u", kx->sections.song);
  LINE("section_samples %u", kx->sections.samples);
  LINE("section_document %u", kx->sections.document);
  LINE("section_graph %u", kx->sections.graph);
  LINE("section_parameters %u", kx->sections.parameters);
  LINE("section_animations %u", kx->sections.animations);
  LINE("section_events %u", kx->sections.events);
  LINE("section_splines %u", kx->sections.splines);
  LINE("section_blobs %u", kx->sections.blobs);
  LINE("section_end %u", kx->sections.end);
  LINE("fingerprint %u", kx_fingerprint(kx));
#undef LINE
  CHECK(n == g, "fingerprint fixture has %d lines, expected %d", n, g);
  for (int i = 0; i < g && i < n; i++) CHECK(!strcmp(expect[i], got[i]), "kx: JS '%s' vs C '%s'", expect[i], got[i]);
  CHECK(kx->sections.end == kx_size, "the document spans the whole file");

  /* class metadata covers every class of the document */
  for (uint32_t c = 0; c < kx->n_classes; c++) CHECK(wz3_class_lookup(kx->classes[c].id) != NULL, "class 0x%x is not in the class table", kx->classes[c].id);
  CHECK(wz3_class_lookup(0x021) && !strcmp(wz3_class_lookup(0x021)->name, "Flat") && wz3_class_lookup(0x021)->output_class == WZ3_KC_BITMAP,
        "class lookup Flat");
  CHECK(wz3_class_lookup(0x0b5) && wz3_class_lookup(0x0b5)->output_class == WZ3_KC_MINMESH, "class lookup <- Mesh");
  CHECK(wz3_class_lookup(0x184) && !wz3_class_lookup(0x185) && !wz3_class_lookup(0), "class lookup bounds");
  for (size_t i = 1; i < sizeof wz3__class_table / sizeof wz3__class_table[0]; i++)
    CHECK(wz3__class_table[i - 1].id < wz3__class_table[i].id, "class table sorted at %zu", i);
}

/* ---- graph vs JS ------------------------------------------------------------------------------- */

/* the stream of tools/dump_texture_hashes.mjs: every level folds in its index and size, then its bytes */
static uint32_t texture_hash(const wz3_texture_image *t)
{
  fnv h = { 0x811c9dc5u };
  for (uint32_t i = 0; i < t->n_levels; i++) {
    fnv_u32(&h, i);
    fnv_u32(&h, (uint32_t)t->levels[i].width);
    fnv_u32(&h, (uint32_t)t->levels[i].height);
    fnv_bytes(&h, t->data + t->levels[i].offset, t->levels[i].size);
  }
  return h.h;
}

typedef struct texture_expect {
  uint32_t op;
  int format;
  int width, height, levels;
  char kind[16];
  uint32_t hash, fallback_hash;
  int has_fallback;
} texture_expect;

static void check_texture(const wz3_bitmap *bm, const texture_expect *e, int *checked)
{
  static const char *const kinds[] = { "rgba8", "snorm8", "bc3" };
  wz3_texture_image t;
  const wz3_result r = wz3_texture_prepare(&t, bm, 1);
  CHECK(r == WZ3_OK, "op %u texture prepare: %s", e->op, wz3_result_string(r));
  if (r != WZ3_OK) return;
  const int ok = bm->format == e->format && t.width == e->width && t.height == e->height && (int)t.n_levels == e->levels && !strcmp(kinds[t.kind], e->kind) &&
                 texture_hash(&t) == e->hash;
  if (!ok)
    printf("  op %u texture: C fmt %d %dx%d levels %u %s hash %u, JS fmt %d %dx%d levels %d %s hash %u\n", e->op, bm->format, t.width, t.height, t.n_levels,
           kinds[t.kind], texture_hash(&t), e->format, e->width, e->height, e->levels, e->kind, e->hash);
  CHECK(ok, "op %u texture differs from the JS port", e->op);
  wz3_texture_release(&t);
  if (e->has_fallback) {
    const wz3_result r2 = wz3_texture_prepare(&t, bm, 0);
    CHECK(r2 == WZ3_OK && t.kind == WZ3_TEXTURE_RGBA8 && texture_hash(&t) == e->fallback_hash, "op %u BC3 fallback texture differs from the JS port", e->op);
    if (r2 == WZ3_OK) wz3_texture_release(&t);
  }
  (*checked)++;
}

static texture_expect *load_texture_expect(const char *fixtures, const char *file, size_t *count)
{
  size_t size;
  lines l;
  l.text = l.cursor = (char *)read_all(fixtures, file, &size);
  texture_expect *list = NULL;
  size_t n = 0, cap = 0;
  char *line;
  while ((line = next_line(&l)) != NULL) {
    texture_expect e;
    memset(&e, 0, sizeof e);
    unsigned long hash = 0, fallback = 0;
    const int fields = sscanf(line, "%u %d %d %d %d %15s %lu %lu", &e.op, &e.format, &e.width, &e.height, &e.levels, e.kind, &hash, &fallback);
    if (fields < 7) continue;
    e.hash = (uint32_t)hash;
    e.has_fallback = fields == 8;
    e.fallback_hash = (uint32_t)fallback;
    if (n == cap) {
      cap = cap ? cap * 2 : 64;
      list = (texture_expect *)realloc(list, cap * sizeof *list);
    }
    list[n++] = e;
  }
  free(l.text);
  *count = n;
  return list;
}

static void test_graph_fixture(const wz3_kx *kx, const char *fixtures, const char *file, int32_t texture_offset, int release_consumed, uint32_t max_op,
                               const char *texture_file)
{
  size_t size;
  lines l;
  size_t n_textures = 0, next_texture = 0;
  int textures_checked = 0;
  texture_expect *textures = texture_file ? load_texture_expect(fixtures, texture_file, &n_textures) : NULL;
  l.text = l.cursor = (char *)read_all(fixtures, file, &size);
  wz3_graph_options options;
  memset(&options, 0, sizeof options);
  wz3_bitmap_ctx_init(&options.ctx);
  options.ctx.texture_offset = texture_offset;
  options.release_consumed = release_consumed;
  wz3_result res;
  wz3_graph *g = wz3_graph_create(kx, &options, &res);
  CHECK(g != NULL && res == WZ3_OK, "graph create");
  if (!g) {
    free(l.text);
    return;
  }

  uint32_t *consumers = (uint32_t *)calloc(kx->n_ops, sizeof(uint32_t));
  for (uint32_t o = 0; o < kx->n_ops; o++)
    if (wz3__find_spec(kx->ops[o].class_id))
      for (uint32_t k = 0; k < kx->ops[o].n_inputs; k++) consumers[kx->ops[o].inputs[k]]++;

  int evaluated = 0, skipped = 0, mismatches = 0;
  char *line;
  while ((line = next_line(&l)) != NULL) {
    unsigned op, cls, w, h, fmt;
    unsigned long hash;
    char word[16];
    if (sscanf(line, "%u %u %15s", &op, &cls, word) != 3) continue;
    if (op > max_op) break;
    const wz3_bitmap *bm = NULL;
    const wz3_result r = wz3_graph_eval_bitmap(g, op, &bm);
    if (!strcmp(word, "skip")) {
      /* The JS port had no font here (101 operators: Text and its dependents). The built-in rasterizer
         evaluates them; only 11944 and 12943 (inputs that cannot be produced by any bitmap handler)
         still fail, in JS and in C. Their pixels are pinned by test_wz3_text. */
      skipped++;
      if (op == 11944 || op == 12943) {
        CHECK(r != WZ3_OK && bm == NULL, "op %u cannot evaluate, got %s", op, wz3_result_string(r));
        continue;
      }
      CHECK(r == WZ3_OK && bm != NULL, "op %u (text or a dependent) should evaluate: %s", op, wz3_result_string(r));
      if (r != WZ3_OK || !bm) continue;
      for (uint32_t k = 0; k < kx->ops[op].n_inputs; k++)
        if (--consumers[kx->ops[op].inputs[k]] == 0) wz3_graph_release(g, kx->ops[op].inputs[k]);
      if (consumers[op] == 0) wz3_graph_release(g, op);
      continue;
    }
    sscanf(line, "%*u %*u %u %u %u %lu", &w, &h, &fmt, &hash);
    if (r != WZ3_OK || !bm) {
      if (++mismatches < 10) printf("  op %u (class 0x%x): %s\n", op, cls, wz3_result_string(r));
      CHECK(0, "op %u failed: %s", op, wz3_result_string(r));
      continue;
    }
    evaluated++;
    const int bad = (unsigned)bm->width != w || (unsigned)bm->height != h || (unsigned)bm->format != fmt || wz3_bitmap_hash(bm) != (uint32_t)hash;
    if (bad && ++mismatches < 10)
      printf("  op %u (class 0x%x): C %dx%d fmt %d hash %u, JS %ux%u fmt %u hash %lu\n", op, cls, bm->width, bm->height, bm->format, wz3_bitmap_hash(bm), w, h,
             fmt, hash);
    CHECK(!bad, "op %u (class 0x%x) differs from the JS port", op, cls);
    if (next_texture < n_textures && textures[next_texture].op == op) check_texture(bm, &textures[next_texture++], &textures_checked);

    /* the consumer bookkeeping of the JS driver */
    for (uint32_t k = 0; k < kx->ops[op].n_inputs; k++)
      if (--consumers[kx->ops[op].inputs[k]] == 0) wz3_graph_release(g, kx->ops[op].inputs[k]);
    if (consumers[op] == 0) wz3_graph_release(g, op);
  }
  printf("  %s (texture offset %d, release_consumed=%d): %d bitmaps match, %d JS skips\n", file, texture_offset, release_consumed, evaluated, skipped);
  CHECK(evaluated > 0, "no operators evaluated");
  if (texture_file) {
    printf("  %s: %d textures match\n", texture_file, textures_checked);
    CHECK((size_t)textures_checked == n_textures, "%d of %zu texture fixtures were checked", textures_checked, n_textures);
  }
  free(textures);
  free(consumers);
  free(l.text);
  wz3_graph_destroy(g);
}

static void test_graph_api(const wz3_kx *kx)
{
  wz3_graph_options options;
  memset(&options, 0, sizeof options);
  wz3_bitmap_ctx_init(&options.ctx);
  options.ctx.texture_offset = -2;
  wz3_result res;
  wz3_graph *g = wz3_graph_create(kx, &options, &res);
  CHECK(g && res == WZ3_OK, "graph create");
  const wz3_bitmap *bm = NULL;
  CHECK(wz3_graph_eval_bitmap(g, kx->n_ops, &bm) == WZ3_ERR_ARGUMENT && bm == NULL, "out of range op");
  CHECK(wz3_graph_eval_bitmap(g, 0, NULL) == WZ3_ERR_ARGUMENT, "NULL out");
  CHECK(wz3_graph_eval_bitmap(NULL, 0, &bm) == WZ3_ERR_ARGUMENT, "NULL graph");
  wz3_graph_release(NULL, 0);
  wz3_graph_release(g, kx->n_ops + 5);
  CHECK(wz3_graph_create(NULL, NULL, &res) == NULL && res == WZ3_ERR_ARGUMENT, "NULL document");

  /* non-bitmap operators and Render are reported as unsupported */
  int saw_render = 0, saw_other = 0;
  for (uint32_t o = 0; o < kx->n_ops && (!saw_render || !saw_other); o++) {
    const uint16_t id = kx->ops[o].class_id;
    if (id == 0x3f && !saw_render) {
      CHECK(wz3_graph_eval_bitmap(g, o, &bm) == WZ3_ERR_UNSUPPORTED, "Render is unsupported");
      saw_render = 1;
    } else if (id >= 0x81 && !saw_other) {
      CHECK(wz3_graph_eval_bitmap(g, o, &bm) == WZ3_ERR_UNSUPPORTED && bm == NULL, "mesh op 0x%x is unsupported", id);
      /* the failure is sticky and consistent */
      CHECK(wz3_graph_eval_bitmap(g, o, &bm) == WZ3_ERR_UNSUPPORTED, "failure is cached");
      saw_other = 1;
    }
  }
  CHECK(saw_render && saw_other, "document contains Render and non-bitmap operators");

  /* eval twice returns the same cached bitmap; release + eval recomputes the same pixels */
  uint32_t target = 0;
  for (uint32_t o = 0; o < kx->n_ops; o++)
    if (kx->ops[o].class_id == 0x29) {
      target = o;
      break;
    } /* the first Blur */
  const wz3_bitmap *a = NULL, *b = NULL;
  CHECK(wz3_graph_eval_bitmap(g, target, &a) == WZ3_OK && a, "eval blur chain");
  CHECK(wz3_graph_eval_bitmap(g, target, &b) == WZ3_OK && a == b, "cached result is reused");
  const uint32_t hash = wz3_bitmap_hash(a);
  wz3_graph_release(g, target);
  CHECK(wz3_graph_eval_bitmap(g, target, &b) == WZ3_OK && wz3_bitmap_hash(b) == hash, "recompute after release");
  wz3_graph_destroy(g);
  wz3_graph_destroy(NULL);
}

/* ---- direct bitmap API ------------------------------------------------------------------------- */

static int fake_text(void *user, const wz3_text_request *req, wz3_text_result *out)
{
  int *calls = (int *)user;
  (*calls)++;
  CHECK(req->width == 16 && req->height == 8 && req->text && !strcmp(req->text, "hi") && !strcmp(req->font, "Arial"), "text request");
  out->coverage = (uint8_t *)WZ3_MALLOC((size_t)req->width * (size_t)req->height);
  for (int i = 0; i < req->width * req->height; i++) out->coverage[i] = (uint8_t)((i % req->width) * 16);
  out->line_height = 8;
  return 1;
}

static int fake_import(void *user, const uint8_t *blob, size_t blob_size, const char *filename, int32_t *w, int32_t *h, uint8_t **rgba)
{
  (void)user;
  (void)blob;
  (void)blob_size;
  CHECK(!strcmp(filename, "x.png"), "import file name");
  *w = 5;
  *h = 7;
  *rgba = (uint8_t *)WZ3_MALLOC(5 * 7 * 4);
  for (int i = 0; i < 5 * 7; i++) {
    (*rgba)[i * 4] = (uint8_t)(i * 7);
    (*rgba)[i * 4 + 1] = (uint8_t)(i * 3);
    (*rgba)[i * 4 + 2] = (uint8_t)i;
    (*rgba)[i * 4 + 3] = 255;
  }
  return 1;
}

static void test_bitmap_api(void)
{
  wz3_bitmap_ctx ctx;
  wz3_bitmap_ctx_init(&ctx);
  CHECK(ctx.texture_offset == 0 && ctx.default_format == 1 && ctx.text == wz3_text_default && !ctx.import_image, "ctx defaults");

  uint16_t c[4];
  wz3_packed_color(0xff804020u, c);
  CHECK(c[0] == (0x80 * 257) >> 1 && c[1] == (0x40 * 257) >> 1 && c[2] == (0x20 * 257) >> 1 && c[3] == (255 * 257) >> 1, "packed color");

  wz3_bitmap *b = wz3_bitmap_flat(&ctx, 4, 3, 0xff804020u);
  CHECK(b && b->width == 16 && b->height == 8 && b->format == 1 && b->data[0] == c[0] && b->data[3] == c[3], "flat");
  ctx.texture_offset = -1;
  wz3_bitmap *half = wz3_bitmap_flat(&ctx, 4, 3, 0xff804020u);
  CHECK(half && half->width == 8 && half->height == 4, "texture offset");
  wz3_bitmap *fixed = wz3_bitmap_flat(&ctx, 4 | 0x80, 3, 0xff804020u);
  CHECK(fixed && fixed->width == 16 && fixed->height == 8, "bit 7 of xs bypasses the offset (got %d x %d)", fixed ? fixed->width : 0,
        fixed ? fixed->height : 0);
  ctx.texture_offset = 0;

  wz3_bitmap *copy = wz3_bitmap_copy(b);
  CHECK(copy && copy->data != b->data && wz3_bitmap_hash(copy) == wz3_bitmap_hash(b), "copy");
  copy->data[5] ^= 1;
  CHECK(wz3_bitmap_hash(copy) != wz3_bitmap_hash(b), "copy is independent");

  /* invalid combinations produce NULL, never a crash */
  CHECK(wz3_bitmap_color(NULL, 0, 0) == NULL, "color(NULL)");
  CHECK(wz3_bitmap_blur(NULL, 2, 1, 1, 1) == NULL && wz3_bitmap_sharpen(NULL, 1, 1, 1, 1) == NULL, "blur(NULL)");
  CHECK(wz3_bitmap_merge(0, NULL, 0) == NULL, "merge(none)");
  CHECK(wz3_bitmap_mask(b, b, NULL, 0) == NULL, "mask with a missing input");
  CHECK(wz3_bitmap_distort(b, half, 1, 0) == NULL, "distort with different sizes");
  CHECK(wz3_bitmap_bump(b, NULL, 1, 0, 0, 0, 0, 0, 0, 0, 0.0f, 1, 1, 0, 0, 0) == NULL, "bump with outer == 0");
  CHECK(wz3_bitmap_perlin(&ctx, 4, 4, 31, 1, 0.5f, 0, 0, 1, 1, 0, 0) == NULL, "perlin frequency limit");
  CHECK(wz3_bitmap_cell(&ctx, 4, 4, 0, 0, 0, 0, 0, 1, 1, 0, 0, 0, 0) == NULL, "cell needs at least one point");
  CHECK(wz3_bitmap_rotate(&ctx, b, 0, 0, 1, 0, 0, 0, 0, 0) == NULL, "rotate with zero scale");
  const wz3_bitmap *pair[2] = { b, half };
  CHECK(wz3_bitmap_merge(0, pair, 2) == NULL, "merge with different sizes");
  wz3_bitmap_destroy(NULL);

  /* format only changes the settings */
  wz3_bitmap *f = wz3_bitmap_format(b, 7, 3, 2);
  CHECK(f && f->format == 7 && f->mip_count == 3 && f->mip_threshold == 2 && !memcmp(f->data, b->data, 16 * 8 * 8) && b->format == 1, "format");
  /* inputs are never modified */
  const uint32_t before = wz3_bitmap_hash(b);
  wz3_bitmap *blurred = wz3_bitmap_blur(b, 0x12, 0.1f, 0.1f, 1.0f);
  wz3_bitmap *glow = wz3_bitmap_glowrect(b, 0.5f, 0.5f, 0.2f, 0.2f, 0, 0, 0xffffffffu, 1, 1, 1, 0);
  CHECK(blurred && glow && wz3_bitmap_hash(b) == before, "inputs are untouched");
  wz3_bitmap_destroy(blurred);
  wz3_bitmap_destroy(glow);

  /* a degenerate blur (order 0) is a copy */
  wz3_bitmap *same = wz3_bitmap_blur(b, 0, 1, 1, 1);
  CHECK(same && wz3_bitmap_hash(same) == before, "blur order 0");
  wz3_bitmap_destroy(same);

  /* Text through a host rasterizer */
  int calls = 0;
  ctx.text = fake_text;
  ctx.text_user = &calls;
  wz3_bitmap *text = wz3_bitmap_text(&ctx, NULL, 0, b, 0, 0, 1, 1, 0xffffffffu, 0, 0, 0, 0, "hi", "Arial", NULL);
  CHECK(text && calls == 1, "text via host rasterizer");
  /* zero coverage keeps the color up to the fade's truncation (weight 0x7fff); full coverage moves it */
  CHECK(text && text->data[0] + 2 >= b->data[0] && text->data[0] <= b->data[0] && text->data[15 * 4] > b->data[15 * 4] + 100, "text coverage blends the color");
  wz3_bitmap_destroy(text);
  ctx.text = NULL;
  CHECK(wz3_bitmap_text(&ctx, NULL, 0, b, 0, 0, 1, 1, 0, 0, 0, 0, 0, "hi", "Arial", NULL) == NULL, "text without a host");

  /* Import through a host decoder crops to powers of two */
  ctx.import_image = fake_import;
  wz3_bitmap *imp = wz3_bitmap_import(&ctx, NULL, 0, "x.png");
  CHECK(imp && imp->width == 4 && imp->height == 4, "import crop (got %d x %d)", imp ? imp->width : 0, imp ? imp->height : 0);
  if (imp) {
    uint16_t expect[4];
    /* pixel (1,2) of the 5-wide source is index 11 */
    wz3_packed_color(0xff000000u | ((11u * 7u & 255u) << 16) | ((11u * 3u & 255u) << 8) | 11u, expect);
    CHECK(!memcmp(imp->data + (2 * 4 + 1) * 4, expect, sizeof expect), "import pixel");
  }
  wz3_bitmap_destroy(imp);
  ctx.import_image = NULL;
  CHECK(wz3_bitmap_import(&ctx, NULL, 0, "x.png") == NULL, "import without a host");

  wz3_bitmap_destroy(fixed);
  wz3_bitmap_destroy(half);
  wz3_bitmap_destroy(copy);
  wz3_bitmap_destroy(f);
  wz3_bitmap_destroy(b);
}

/* ---- KX robustness ------------------------------------------------------------------------------ */

static uint32_t g_rng = 0x2545f491u;
static uint32_t xorshift(void)
{
  g_rng ^= g_rng << 13;
  g_rng ^= g_rng >> 17;
  g_rng ^= g_rng << 5;
  return g_rng;
}

static void test_kx_errors(const uint8_t *kx_bytes, size_t kx_size)
{
  wz3_kx *kx = NULL;
  size_t at = 123;
  CHECK(wz3_kx_parse(NULL, kx_bytes, kx_size, &at) == WZ3_ERR_ARGUMENT, "NULL out");
  CHECK(wz3_kx_parse(&kx, NULL, 5, &at) == WZ3_ERR_ARGUMENT && kx == NULL && at == 0, "NULL data");
  CHECK(wz3_kx_parse(&kx, NULL, 0, &at) == WZ3_ERR_TRUNCATED && kx == NULL, "empty document");

  uint8_t bad[8] = { 8, 0, 0, 0, 0, 0, 0, 0 }; /* unknown flag bit */
  CHECK(wz3_kx_parse(&kx, bad, sizeof bad, &at) == WZ3_ERR_FORMAT && kx == NULL, "unknown flags");

  /* every prefix of the document is rejected cleanly, with the offset inside it */
  const size_t step = kx_size / 400 + 1;
  for (size_t n = 0; n < kx_size; n += step) {
    const wz3_result r = wz3_kx_parse(&kx, kx_bytes, n, &at);
    CHECK(r != WZ3_OK && kx == NULL && at <= n, "prefix %zu: %s at %zu", n, wz3_result_string(r), at);
  }
  CHECK(wz3_kx_parse(&kx, kx_bytes, kx_size - 1, &at) == WZ3_ERR_TRUNCATED, "one byte short");
  wz3_kx_destroy(kx);

  /* trailing bytes after the document are ignored */
  uint8_t *padded = (uint8_t *)malloc(kx_size + 100);
  memcpy(padded, kx_bytes, kx_size);
  memset(padded + kx_size, 0xaa, 100);
  CHECK(wz3_kx_parse(&kx, padded, kx_size + 100, &at) == WZ3_OK && kx && kx->sections.end == kx_size, "trailing data");
  wz3_kx_destroy(kx);
  /* the parsed document owns its bytes */
  CHECK(wz3_kx_parse(&kx, padded, kx_size, &at) == WZ3_OK && kx, "reparse");
  memset(padded, 0, kx_size);
  CHECK(kx && kx->n_ops == 16478 && kx->ops[100].anim_size > 0 && kx->classes[0].packing != NULL, "copy survives caller");
  wz3_kx_destroy(kx);
  free(padded);

  /* mutation fuzzing: random byte flips anywhere, concentrated on the header and tables */
  uint8_t *m = (uint8_t *)malloc(kx_size);
  int ok_count = 0, error_count = 0;
  for (int iteration = 0; iteration < 1500; iteration++) {
    memcpy(m, kx_bytes, kx_size);
    const int flips = 1 + (int)(xorshift() % 6);
    for (int k = 0; k < flips; k++) {
      size_t pos;
      switch (xorshift() % 3) {
      case 0: pos = 182420 - 40 + (xorshift() % 400); break; /* document header + classes */
      case 1: pos = 183798 + (xorshift() % 6000); break;     /* graph */
      default: pos = xorshift() % kx_size; break;
      }
      if (pos >= kx_size) pos = kx_size - 1;
      m[pos] = (uint8_t)xorshift();
    }
    const size_t len = (xorshift() % 8 == 0) ? (size_t)(xorshift() % kx_size) : kx_size;
    const wz3_result r = wz3_kx_parse(&kx, m, len, &at);
    if (r == WZ3_OK) {
      ok_count++;
      CHECK(kx && at == 0, "ok result");
      /* a well-formed mutation must also be usable by the graph without touching bad memory */
      wz3_graph *g = wz3_graph_create(kx, NULL, NULL);
      CHECK(g != NULL, "graph over mutated document");
      wz3_graph_destroy(g);
      wz3_kx_destroy(kx);
    } else {
      error_count++;
      CHECK(kx == NULL && at <= len, "error result offset %zu <= %zu", at, len);
    }
  }
  printf("  mutation fuzz: %d accepted, %d rejected\n", ok_count, error_count);
  free(m);

  /* random garbage */
  for (int iteration = 0; iteration < 500; iteration++) {
    uint8_t junk[96];
    const size_t len = xorshift() % sizeof junk;
    for (size_t i = 0; i < len; i++) junk[i] = (uint8_t)xorshift();
    if (len >= 4 && (iteration & 1)) junk[0] &= 7, junk[1] = junk[2] = junk[3] = 0;
    const wz3_result r = wz3_kx_parse(&kx, junk, len, &at);
    if (r == WZ3_OK)
      wz3_kx_destroy(kx);
    else
      CHECK(kx == NULL && at <= len, "garbage offset");
  }

  /* strings of the error codes */
  for (int r = 0; r <= WZ3_ERR_CANCELLED; r++) {
    const char *s = wz3_result_string((wz3_result)r);
    CHECK(s != NULL && strcmp(s, "unknown error") != 0, "result string %d", r);
    for (int q = 0; q < r; q++) CHECK(strcmp(s, wz3_result_string((wz3_result)q)) != 0, "result strings %d and %d are equal", q, r);
  }
  CHECK(!strcmp(wz3_result_string((wz3_result)(WZ3_ERR_CANCELLED + 1)), "unknown error"), "out-of-range result string");

  /* hand-made limits: more than 128 classes */
  {
    uint8_t doc[4 + 4 + 4 + 4 + 2 + 16 + 129 * 8 + 4];
    size_t pos = 0;
    memset(doc, 0, sizeof doc);
    pos += 4;       /* flags */
    pos += 4;       /* song size */
    pos += 8;       /* bpm, length */
    doc[pos++] = 0; /* operations */
    doc[pos++] = 0; /* splines */
    pos += 16;      /* roots */
    for (int i = 0; i < 129; i++) {
      doc[pos] = 1; /* convention != 0 */
      pos += 4;
      pos += 2;     /* id */
      doc[pos++] = 'b';
      doc[pos++] = 0;
    }
    CHECK(wz3_kx_parse(&kx, doc, pos + 4, &at) == WZ3_ERR_LIMIT && kx == NULL, "129 classes");
  }
}

/* parse + evaluate under allocation failure: every failure is reported and nothing leaks */
static void test_alloc_failures(const uint8_t *kx_bytes, size_t kx_size)
{
  size_t at;
  long attempts = 0;
  const size_t live_before = g_live;
  for (long fail = 0;; fail++) {
    g_alloc_count = 0;
    g_fail_at = fail;
    wz3_kx *kx = NULL;
    const wz3_result r = wz3_kx_parse(&kx, kx_bytes, kx_size, &at);
    g_fail_at = -1;
    attempts++;
    if (r == WZ3_OK) {
      wz3_kx_destroy(kx);
      CHECK(g_alloc_count <= fail, "parse completed although allocation %ld was due to fail", fail);
      break;
    }
    CHECK(r == WZ3_ERR_OUT_OF_MEMORY && kx == NULL, "allocation %ld: %s", fail, wz3_result_string(r));
    CHECK(g_live == live_before, "leak after parse failure %ld: %zu live blocks", fail, g_live - live_before);
    if (fail > 5000) {
      CHECK(0, "parse never succeeds");
      break;
    }
  }
  printf("  parse: %ld allocation failure points\n", attempts);

  wz3_kx *kx = NULL;
  CHECK(wz3_kx_parse(&kx, kx_bytes, kx_size, &at) == WZ3_OK, "parse for graph alloc test");
  /* the whole of op 400's dependency chain, at the small texture size */
  uint32_t target = 0;
  for (uint32_t o = 0; o < kx->n_ops && !target; o++)
    if (kx->ops[o].class_id == 0x2c && o > 400) target = o; /* a Rotate */
  attempts = 0;
  const size_t live_graph_before = g_live;
  for (long fail = 0;; fail++) {
    wz3_graph_options options;
    memset(&options, 0, sizeof options);
    wz3_bitmap_ctx_init(&options.ctx);
    options.ctx.texture_offset = -2;
    options.release_consumed = fail & 1;
    g_alloc_count = 0;
    g_fail_at = fail;
    wz3_result res;
    wz3_graph *g = wz3_graph_create(kx, &options, &res);
    wz3_result r = res;
    const wz3_bitmap *bm = NULL;
    int finished = 0;
    if (g) {
      r = wz3_graph_eval_bitmap(g, target, &bm);
      finished = r == WZ3_OK && g_alloc_count <= fail;
      wz3_graph_destroy(g);
    }
    g_fail_at = -1;
    attempts++;
    if (finished) break;
    CHECK(g == NULL ? r == WZ3_ERR_OUT_OF_MEMORY : (r == WZ3_ERR_OPERATOR || r == WZ3_ERR_OUT_OF_MEMORY || r == WZ3_OK), "graph allocation %ld: %s", fail,
          wz3_result_string(r));
    CHECK(g_live == live_graph_before, "leak after graph failure %ld: %zu live blocks", fail, g_live - live_graph_before);
    if (fail > 20000) {
      CHECK(0, "graph never succeeds");
      break;
    }
  }
  printf("  graph eval of op %u: %ld allocation failure points\n", target, attempts);
  wz3_kx_destroy(kx);
  CHECK(g_live == live_before, "no blocks leaked overall");
}

/* texture preparation: argument handling, level counts, formats the production graph never uses */
static void test_texture_api(void)
{
  wz3_texture_image t;
  wz3_bitmap *b = wz3_bitmap_create(8, 4);
  CHECK(b != NULL, "bitmap for texture tests");
  if (!b) return;
  for (size_t i = 0; i < 8 * 4 * 4; i++) b->data[i] = (uint16_t)((i * 977u) & 0x7fff);

  CHECK(wz3_texture_prepare(NULL, b, 1) == WZ3_ERR_ARGUMENT, "NULL output");
  CHECK(wz3_texture_prepare(&t, NULL, 1) == WZ3_ERR_ARGUMENT && t.data == NULL, "NULL bitmap");
  wz3_texture_release(NULL);

  b->format = 1;
  b->mip_count = 0;
  CHECK(wz3_texture_prepare(&t, b, 1) == WZ3_OK && t.n_levels == 3 && t.levels[2].width == 2 && t.levels[2].height == 1 && t.size == (32 + 8 + 2) * 4,
        "full chain of an 8x4 bitmap");
  CHECK(t.data[0] == (uint8_t)((b->data[0] >> 7) & 0xff), "level 0 takes bits 7..14");
  wz3_texture_release(&t);
  CHECK(t.data == NULL && t.size == 0, "release clears the image");

  b->mip_count = 2;
  CHECK(wz3_texture_prepare(&t, b, 1) == WZ3_OK && t.n_levels == 2, "explicit level count");
  wz3_texture_release(&t);

  /* TexMipTresh: point-pick levels below the threshold, reverse flips the choice */
  b->mip_count = 0;
  b->mip_threshold = 15;
  CHECK(wz3_texture_prepare(&t, b, 1) == WZ3_OK && t.data[t.levels[1].offset] == (uint8_t)((b->data[0] >> 7) & 0xff),
        "level 1 is a point sample below the threshold");
  wz3_texture_release(&t);
  b->mip_threshold = 0;

  b->format = 8;
  CHECK(wz3_texture_prepare(&t, b, 1) == WZ3_OK && t.n_levels == 3 && t.kind == WZ3_TEXTURE_RGBA8, "format 8 chain");
  {
    const uint8_t *p = t.data;
    const uint8_t *q = t.data + t.levels[1].offset;
    CHECK(q[0] == (uint8_t)(((unsigned)p[0] + p[4] + p[32] + p[36] + 2u) >> 2), "format 8 box filter");
  }
  wz3_texture_release(&t);

  b->format = 5;
  CHECK(wz3_texture_prepare(&t, b, 1) == WZ3_OK && t.kind == WZ3_TEXTURE_SNORM8, "format 5 kind");
  wz3_texture_release(&t);

  b->format = 9;
  b->mip_threshold = 64;
  CHECK(wz3_texture_prepare(&t, b, 1) == WZ3_ERR_UNSUPPORTED && t.data == NULL, "non-production DXT quality");
  b->mip_threshold = 0;
  CHECK(wz3_texture_prepare(&t, b, 1) == WZ3_OK && t.kind == WZ3_TEXTURE_BC3 && t.levels[0].size == 32 && t.levels[1].size == 16 && t.levels[2].size == 16,
        "BC3 level sizes");
  wz3_texture_release(&t);
  CHECK(wz3_texture_prepare(&t, b, 0) == WZ3_OK && t.kind == WZ3_TEXTURE_RGBA8 && t.size == (32 + 8 + 2) * 4, "BC3 fallback is RGBA8");
  wz3_texture_release(&t);
  wz3_bitmap_destroy(b);

  /* non power of two: a single level */
  b = wz3_bitmap_create(3, 5);
  if (b) {
    b->format = 1;
    CHECK(wz3_texture_prepare(&t, b, 1) == WZ3_OK && t.n_levels == 1 && t.size == 60, "non power of two");
    wz3_texture_release(&t);
    wz3_bitmap_destroy(b);
  }

  /* allocation failures: reported, nothing leaks */
  b = wz3_bitmap_create(16, 16);
  if (!b) return;
  for (int format = 1; format <= 9; format += 4) {
    b->format = format;
    b->mip_count = 0;
    const size_t live = g_live;
    for (long fail = 0;; fail++) {
      g_alloc_count = 0;
      g_fail_at = fail;
      const wz3_result r = wz3_texture_prepare(&t, b, 1);
      g_fail_at = -1;
      if (r == WZ3_OK) {
        wz3_texture_release(&t);
        break;
      }
      CHECK(r == WZ3_ERR_OUT_OF_MEMORY && t.data == NULL, "texture allocation %ld: %s", fail, wz3_result_string(r));
      CHECK(g_live == live, "texture leak at %ld", fail);
      if (fail > 20) {
        CHECK(0, "texture never succeeds");
        break;
      }
    }
    CHECK(g_live == live, "texture release is complete");
  }
  wz3_bitmap_destroy(b);
}

int suite_wz3(int argc, char **argv)
{
  (void)argc;
  (void)argv;
  printf("wz3.h tests\n");
  size_t kx_size;
  uint8_t *kx_bytes = read_all(DEBRIS_ASSETS_DIR, "debris_party.kx", &kx_size);

  test_core(DEBRIS_TEST_DATA_DIR);
  test_texture_api();

  wz3_kx *kx = NULL;
  size_t at = 0;
  const wz3_result r = wz3_kx_parse(&kx, kx_bytes, kx_size, &at);
  CHECK(r == WZ3_OK && kx, "debris_party.kx parses: %s at %zu", wz3_result_string(r), at);
  if (!kx) return 1;
  test_kx(kx, DEBRIS_TEST_DATA_DIR, kx_size);
  test_bitmap_api();
  test_graph_api(kx);
  printf("graph vs JS port (every bitmap operator of the released Debris document):\n");
  test_graph_fixture(kx, DEBRIS_TEST_DATA_DIR, "bitmap_hashes_offset_m2.txt", -2, 1, 0xffffffffu, "texture_hashes_offset_m2.txt");
  test_graph_fixture(kx, DEBRIS_TEST_DATA_DIR, "bitmap_hashes_offset_m2.txt", -2, 0, 700, NULL); /* keep-everything mode */
#ifndef DEBRIS_TEST_SKIP_FULL_SIZE
  test_graph_fixture(kx, DEBRIS_TEST_DATA_DIR, "bitmap_hashes_offset_0.txt", 0, 1, 0xffffffffu, "texture_hashes_offset_0.txt");
#endif
  wz3_kx_destroy(kx);

  printf("robustness:\n");
  test_kx_errors(kx_bytes, kx_size);
  test_alloc_failures(kx_bytes, kx_size);
  free(kx_bytes);
  CHECK(g_live == 0, "all library allocations were released (%zu live)", g_live);

  printf("%d checks, %d failures\n", g_checks, g_failures);
  return g_failures ? 1 : 0;
}
