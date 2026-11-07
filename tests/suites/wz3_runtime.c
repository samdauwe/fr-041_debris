/* Runtime check for wz3.h against the JavaScript runtime.
 *
 *   test_wz3_runtime <assets dir> <fixtures dir> [--print]
 *
 * Re-runs tools/dump_runtime_state.mjs in C: precalculates the party document with the generic
 * traversal, runs one frame per recorded audio sample and compares, line by line, the active event
 * set, the animation-VM execution chain, every operator's animated parameter words, the events and the
 * environment, then document-spline and blob-spline samples. --print writes the C lines to stdout
 * (the fixture format) instead of comparing.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* counting allocator with fault injection */
static long g_live;
static long g_alloc_count;
static long g_fail_at = -1;
static void *test_malloc(size_t n)
{
  if (g_alloc_count++ == g_fail_at) return NULL;
  size_t *p = (size_t *)malloc(n + 16);
  if (!p) return NULL;
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

static uint32_t fnv_word(uint32_t h, uint32_t w) { return (h ^ w) * 0x01000193u; }
static uint32_t f2u(float f)
{
  uint32_t u;
  memcpy(&u, &f, 4);
  return u;
}

typedef struct lines {
  char (*text)[320];
  size_t count, cap;
} lines;

static void emit(lines *l, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
#include <stdarg.h>
static void emit(lines *l, const char *fmt, ...)
{
  if (l->count == l->cap) {
    l->cap = l->cap ? l->cap * 2 : 256;
    l->text = (char (*)[320])realloc(l->text, l->cap * 320);
  }
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(l->text[l->count++], 320, fmt, ap);
  va_end(ap);
}

/* the sample list of the fixture: "F index sample ..." lines */
static size_t read_samples(const char *fixture, int32_t *samples, size_t max)
{
  size_t n = 0;
  const char *p = fixture;
  while (*p && n < max) {
    unsigned index;
    long sample;
    if (sscanf(p, "F %u %ld", &index, &sample) == 2) samples[n++] = (int32_t)sample;
    const char *nl = strchr(p, '\n');
    if (!nl) break;
    p = nl + 1;
  }
  return n;
}

/* ---- synthetic cases (tools/dump_runtime_synth.mjs) ---- */

static uint32_t canon_bits(uint32_t bits) { return ((bits & 0x7f800000u) == 0x7f800000u && (bits & 0x007fffffu)) ? 0x7fc00000u : bits; }

static size_t hex_bytes(const char *s, uint8_t *out, size_t max)
{
  size_t n = 0;
  while (s[0] && s[1] && s[0] != ' ' && n < max) {
    unsigned v;
    char pair[3] = { s[0], s[1], 0 };
    sscanf(pair, "%2x", &v);
    out[n++] = (uint8_t)v;
    s += 2;
  }
  return n;
}

/* "a,b,c" of hex numbers (stops at a space); returns the count */
static size_t hex_list(const char *s, uint64_t *out, size_t max)
{
  size_t n = 0;
  while (*s && *s != ' ' && n < max) {
    char *end;
    out[n++] = strtoull(s, &end, 16);
    s = *end == ',' ? end + 1 : end;
  }
  return n;
}

static float u2f(uint32_t u)
{
  float f;
  memcpy(&f, &u, 4);
  return f;
}
static double u2d(uint64_t u)
{
  double d;
  memcpy(&d, &u, 8);
  return d;
}

static uint32_t hash_pose(uint32_t h, const float matrix[16], float zoom)
{
  for (int i = 0; i < 16; i++) h = fnv_word(h, canon_bits(f2u(matrix[i])));
  return fnv_word(h, canon_bits(f2u(zoom)));
}

static const char *line_at(const char *p, char *buf, size_t cap)
{
  const char *nl = strchr(p, '\n');
  const size_t len = nl ? (size_t)(nl - p) : strlen(p);
  const size_t n = len < cap - 1 ? len : cap - 1;
  memcpy(buf, p, n);
  buf[n] = 0;
  return nl ? nl + 1 : p + len;
}

static void test_synth(wz3_rt *rt, const char *fixture)
{
  static char line[1 << 16], times_line[1 << 12];
  const char *p = fixture;
  size_t cases[256] = { 0 }, bad[256] = { 0 };
  uint32_t prog_bits[1];
  (void)prog_bits;
  while (*p) {
    p = line_at(p, line, sizeof line);
    if (!line[0] || line[0] == 'T') continue;
    const char kind = line[0];
    const char *arrow = strstr(line, "-> ");
    if (!arrow) continue;
    arrow += 3;
    char got[256];
    got[0] = 0;
    int ok = 1;
    if (kind == 'A') {
      uint8_t program[1024];
      uint64_t words[8];
      const char *q = line + 2;
      const size_t n = hex_bytes(q, program, sizeof program);
      q = strchr(q, ' ') + 1;
      hex_list(q, words, 8);
      q = strchr(q, ' ') + 1;
      const uint32_t seed = (uint32_t)strtoul(q, (char **)&q, 10);
      const long event_spline = strtol(q, NULL, 10);
      wz3__rt_op fop;
      wz3_kx_op fsrc;
      wz3_word edit[8], anim[8];
      memset(&fop, 0, sizeof fop);
      memset(&fsrc, 0, sizeof fsrc);
      fsrc.anim = program;
      fsrc.anim_size = (uint32_t)n;
      for (int i = 0; i < 8; i++) edit[i].u = (uint32_t)words[i];
      memcpy(anim, edit, sizeof anim);
      fop.src = &fsrc;
      fop.edit = edit;
      fop.anim = anim;
      fop.data_words = 8;
      fop.id = 0;
      uint32_t s = seed ? seed : 1u;
      for (int v = 0; v < WZ3__RT_MAX_VARS; v++)
        for (int k = 0; k < 4; k++) {
          s ^= s << 13;
          s ^= s >> 17;
          s ^= s << 5;
          rt->env.vars[v][k] = (float)(((double)(int32_t)(s % 2001u) - 1000.0) / 100.0);
        }
      rt->env.n_saves = 0;
      rt->env.event_spline = (int32_t)event_spline;
      uint32_t pops = 0;
      const wz3_result r = wz3__rt_exec_animation(rt, &fop, &pops);
      if (r != WZ3_OK) {
        snprintf(got, sizeof got, "error");
      } else {
        uint32_t vars = 0x811c9dc5u;
        for (int v = 0; v < WZ3__RT_MAX_VARS; v++)
          for (int k = 0; k < 4; k++) vars = fnv_word(vars, canon_bits(f2u(rt->env.vars[v][k])));
        int len = snprintf(got, sizeof got, "pops %u saves %u changed %d words ", pops, rt->env.n_saves, fop.changed ? 1 : 0);
        for (int i = 0; i < 8; i++) len += snprintf(got + len, sizeof got - (size_t)len, "%s%x", i ? "," : "", canon_bits(anim[i].u));
        snprintf(got + len, sizeof got - (size_t)len, " vars %x", vars);
      }
      wz3__rt_env_pop(&rt->env, rt->env.n_saves);
      rt->env.event_spline = -1;
    } else if (kind == 'B' || kind == 'C' || kind == 'D') {
      static uint8_t bytes[1 << 14];
      uint64_t tbits[64];
      p = line_at(p, times_line, sizeof times_line); /* the T line */
      const size_t n_times = hex_list(times_line + 2, tbits, 64);
      const size_t n = hex_bytes(line + 2, bytes, sizeof bytes);
      wz3_result r;
      wz3__rt_blob *blob = NULL;
      uint32_t h = 0x811c9dc5u;
      if (kind == 'C') {
        blob = wz3__rt_blob_from_pipe(bytes, n, &r);
      } else {
        blob = wz3__rt_blob_from_bytes(bytes, n, &r);
        if (blob && kind == 'D') {
          uint64_t pv[4];
          const char *q = strchr(line + 2, ' ') + 1;
          hex_list(q, pv, 4);
          wz3__rt_blob *scaled =
            wz3__rt_blob_scaled(blob, (double)u2f((uint32_t)pv[0]), (double)u2f((uint32_t)pv[1]), (double)u2f((uint32_t)pv[2]), (int32_t)pv[3]);
          wz3__rt_blob_free(blob);
          blob = scaled;
        }
      }
      if (!blob) {
        snprintf(got, sizeof got, "error");
      } else {
        for (size_t t = 0; t < n_times; t++) {
          float matrix[16], zoom;
          wz3__rt_blob_eval(blob, u2d(tbits[t]), matrix, &zoom);
          h = hash_pose(h, matrix, zoom);
        }
        if (kind == 'B')
          snprintf(got, sizeof got, "%x", h);
        else if (kind == 'D')
          snprintf(got, sizeof got, "%x %zu", h, n_times);
        else {
          uint32_t kh = 0x811c9dc5u;
          for (uint32_t i = 0; i < blob->n_keys; i++) {
            kh = fnv_word(kh, canon_bits(f2u(blob->keys[i].time)));
            for (int f = 0; f < 7; f++) kh = fnv_word(kh, canon_bits(f2u(blob->keys[i].f[f])));
          }
          snprintf(got, sizeof got, "keys %u keyhash %x eval %x", blob->n_keys, kh, h);
        }
        wz3__rt_blob_free(blob);
      }
    } else if (kind == 'E') {
      static uint8_t pbytes[1 << 14], abytes[1 << 14];
      static char tokens[10][4096];
      const char *q = line + 2;
      int nt = 0;
      while (nt < 10 && *q && strncmp(q, "-> ", 3)) {
        const char *sp = strchr(q, ' ');
        size_t len = (size_t)(sp - q);
        if (len > 4095) len = 4095;
        memcpy(tokens[nt], q, len);
        tokens[nt][len] = 0;
        nt++;
        q = sp + 1;
      }
      /* parent amp inner params var0 beat events times */
      wz3_rt fake;
      wz3__rt_op ops[8];
      wz3__rt_event events[4];
      memset(&fake, 0, sizeof fake);
      memset(ops, 0, sizeof ops);
      memset(events, 0, sizeof events);
      for (int i = 0; i < 8; i++) ops[i].first_event = -1;
      fake.ops = ops;
      fake.n_ops = 8;
      fake.events = events;
      fake.n_events = 4;
      wz3__rt_make_permutation(fake.perm);
      wz3__rt_cache parent, amp;
      memset(&parent, 0, sizeof parent);
      memset(&amp, 0, sizeof amp);
      wz3_result r;
      wz3__rt_blob *pb = NULL, *ab = NULL;
      wz3__rt_shaker *inner = NULL;
      if (strcmp(tokens[0], "-")) {
        pb = wz3__rt_blob_from_bytes(pbytes, hex_bytes(tokens[0], pbytes, sizeof pbytes), &r);
        parent.kind = WZ3__RC_BLOB;
        parent.obj = pb;
        parent.class_id = WZ3_KC_SPLINE;
      }
      if (strcmp(tokens[1], "-")) {
        ab = wz3__rt_blob_from_bytes(abytes, hex_bytes(tokens[1], abytes, sizeof abytes), &r);
        amp.kind = WZ3__RC_BLOB;
        amp.obj = ab;
        amp.class_id = WZ3_KC_SPLINE;
      }
      uint64_t pw[17], iw[17];
      double params[17], iparams[17];
      if (strcmp(tokens[2], "-")) {
        hex_list(tokens[2], iw, 17);
        for (int i = 0; i < 17; i++) iparams[i] = (double)u2f((uint32_t)iw[i]);
        inner = wz3__rt_shaker_make(3, &parent, NULL, iparams);
        memset(&parent, 0, sizeof parent);
        parent.kind = WZ3__RC_SHAKER;
        parent.obj = inner;
        parent.class_id = WZ3_KC_SPLINE;
      }
      hex_list(tokens[3], pw, 17);
      for (int i = 0; i < 17; i++) params[i] = (double)u2f((uint32_t)pw[i]);
      wz3__rt_shaker *outer = wz3__rt_shaker_make(7, &parent, &amp, params);
      const float var0 = u2f((uint32_t)strtoul(tokens[4], NULL, 16));
      fake.env.beat_time = (int32_t)strtol(tokens[5], NULL, 10);
      for (int c = 0; c < 4; c++) fake.env.vars[0][c] = var0;
      if (strcmp(tokens[6], "-")) {
        const char *e = tokens[6];
        int count = 0;
        while (*e && count < 4) {
          events[count].start = (int32_t)strtol(e, (char **)&e, 10);
          events[count].end = (int32_t)strtol(e + 1, (char **)&e, 10);
          events[count].next_op = -1;
          if (count) events[count - 1].next_op = count;
          count++;
          if (*e == ';') e++;
        }
        ops[7].first_event = 0;
      }
      uint64_t tw[16];
      const size_t n_times = hex_list(tokens[7], tw, 16);
      uint32_t h = 0x811c9dc5u;
      for (size_t t = 0; t < n_times; t++) {
        float matrix[16], zoom;
        memset(matrix, 0, sizeof matrix);
        wz3__rt_shaker_eval(&fake, outer, (double)u2f((uint32_t)tw[t]), matrix, &zoom);
        h = hash_pose(h, matrix, zoom);
      }
      snprintf(got, sizeof got, "%x", h);
      WZ3_FREE(outer);
      WZ3_FREE(inner);
      wz3__rt_blob_free(pb);
      wz3__rt_blob_free(ab);
    } else {
      ok = 0;
    }
    if (!ok) continue;
    cases[(unsigned char)kind]++;
    if (strcmp(got, arrow)) {
      if (++bad[(unsigned char)kind] <= 3) printf("synthetic %c differs\n  JS: %s\n  C : %s\n  in: %.160s\n", kind, arrow, got, line);
    }
  }
  for (int k = 0; k < 256; k++)
    if (cases[k]) {
      CHECK(bad[k] == 0, "synthetic %c: %zu of %zu cases differ from the JS runtime", k, bad[k], cases[k]);
      printf("  synthetic %c: %zu cases, %zu differ\n", k, cases[k], bad[k]);
    }
  CHECK(cases['A'] >= 300 && cases['B'] >= 100 && cases['C'] >= 20 && cases['D'] >= 20 && cases['E'] >= 80, "case counts");
}

static void run(const wz3_kx *kx, const int32_t *samples, size_t n_samples, lines *out, const char *synth)
{
  wz3_result res;
  wz3_rt *rt = wz3_rt_create(kx, NULL, &res);
  CHECK(rt != NULL && res == WZ3_OK, "runtime create: %s", wz3_result_string(res));
  if (!rt) return;
  if (synth) test_synth(rt, synth);
  res = wz3_rt_precalc(rt);
  CHECK(res == WZ3_OK, "precalc: %s", wz3_result_string(res));

  emit(out, "document ops %u events %u splines %u bpm %u length %u buzz %u", kx->n_ops, kx->n_events, kx->n_splines, kx->song_bpm_fixed, kx->song_length,
       (kx->flags & 4u) ? 1u : 0u);

  for (size_t index = 0; index < n_samples; index++) {
    const int32_t sample = samples[index];
    rt->trace = 1;
    rt->trace_chain = 0x811c9dc5u;
    rt->trace_count = 0;
    rt->trace_active = 0;
    const int32_t beat = wz3_sample_to_beat(sample, kx->song_bpm_fixed, (kx->flags & 4u) != 0, 44100);
    const int32_t ms = (int32_t)((int64_t)sample * 1000 / 44100);
    res = wz3_rt_frame_at_sample(rt, sample, 44100);
    rt->trace = 0;
    CHECK(res == WZ3_OK, "frame %zu: %s", index, wz3_result_string(res));
    uint32_t anim = 0x811c9dc5u, events = 0x811c9dc5u, vars = 0x811c9dc5u;
    for (uint32_t o = 0; o < rt->n_ops; o++)
      for (uint32_t w = 0; w < rt->ops[o].data_words; w++) anim = fnv_word(anim, rt->ops[o].anim[w].u);
    for (uint32_t e = 0; e < rt->n_events; e++) {
      events = fnv_word(events, (uint32_t)rt->events[e].start);
      events = fnv_word(events, (uint32_t)rt->events[e].end);
    }
    for (int v = 0; v < WZ3__RT_MAX_VARS; v++)
      for (int c = 0; c < 4; c++) vars = fnv_word(vars, f2u(rt->env.vars[v][c]));
    vars = fnv_word(vars, rt->env.depth);
    for (int i = 0; i < 16; i++) vars = fnv_word(vars, f2u(rt->env.stack[(size_t)(rt->env.depth - 1) * 16 + (size_t)i]));
    emit(out, "F %zu %d %d %d active %x execs %u chain %x anim %x events %x env %x dynamic %u", index, sample, beat, ms, rt->trace_active, rt->trace_count,
         rt->trace_chain, anim, events, vars, rt->env.n_dynamic);
  }

  /* document splines */
  static const double spline_times[] = { -0.25, 0, 0.1, 0.37, 0.5, 0.75, 0.999, 1, 1.5 };
  for (uint32_t s = 0; s < rt->n_splines; s++) {
    uint32_t h = 0x811c9dc5u;
    for (size_t t = 0; t < sizeof spline_times / sizeof spline_times[0]; t++) {
      float v[4];
      wz3__rt_dspline_eval(&rt->splines[s], spline_times[t], v);
      for (int i = 0; i < 4; i++) h = fnv_word(h, f2u(v[i]));
    }
    emit(out, "S %u %u %x", s, rt->splines[s].n_channels, h);
  }

  /* blob splines and shakers, evaluated in a fresh frame (shakers read vars[0] as their time) */
  static const double sample_times[] = { -0.1, 0, 0.001, 0.2, 0.4999, 0.5, 0.77, 1, 1.2 };
  static const float time_vars[] = { 0.13f, 0.5f, 0.87f };
  res = wz3__rt_init_frame(rt, 0, 0);
  CHECK(res == WZ3_OK, "init frame");
  for (uint32_t o = 0; o < rt->n_ops; o++) {
    const wz3__rt_cache *cache = &rt->ops[o].cache;
    if (cache->kind != WZ3__RC_BLOB && cache->kind != WZ3__RC_SHAKER) continue;
    const int shaker = cache->kind == WZ3__RC_SHAKER;
    uint32_t h = 0x811c9dc5u, evaluations = 0;
    for (size_t t = 0; t < sizeof sample_times / sizeof sample_times[0]; t++) {
      const size_t variants = shaker ? 3 : 1;
      for (size_t v = 0; v < variants; v++) {
        const float tv = shaker ? time_vars[v] : 0.0f;
        for (int c = 0; c < 4; c++) rt->env.vars[0][c] = tv;
        float matrix[16], zoom;
        memset(matrix, 0, sizeof matrix);
        wz3__rt_spline_eval(rt, cache, sample_times[t], matrix, &zoom);
        for (int i = 0; i < 16; i++) h = fnv_word(h, f2u(matrix[i]));
        h = fnv_word(h, f2u(zoom));
        evaluations++;
      }
    }
    emit(out, "B %u %u %s %u %u %x", o, rt->ops[o].class_id, shaker ? "ShakerSpline" : "Spline", shaker ? 0u : ((const wz3__rt_blob *)cache->obj)->n_keys,
         evaluations, h);
  }
  wz3__rt_exit_frame(rt);
  wz3_rt_destroy(rt);
}

/* ---- snapshot / restore, API errors, allocation failures ---- */

static uint32_t digest_instances(uint32_t h, const wz3__rt_chain *c)
{
  h = fnv_word(h, c->count);
  for (uint32_t i = 0; i < c->count; i++) {
    h = fnv_word(h, c->items[i].op_id);
    h = fnv_word(h, (uint32_t)c->items[i].reset);
    for (int k = 0; k < 4; k++) h = fnv_word(h, (uint32_t)c->items[i].words[k]);
  }
  return h;
}

/* everything one frame can observe or leave behind */
static uint32_t frame_digest(const wz3_rt *rt)
{
  uint32_t h = 0x811c9dc5u;
  h = fnv_word(h, rt->trace_chain);
  h = fnv_word(h, rt->trace_count);
  h = fnv_word(h, rt->trace_active);
  for (uint32_t o = 0; o < rt->n_ops; o++) {
    h = fnv_word(h, rt->ops[o].changed);
    for (uint32_t w = 0; w < rt->ops[o].data_words; w++) h = fnv_word(h, rt->ops[o].anim[w].u);
  }
  for (uint32_t e = 0; e < rt->n_events; e++) {
    h = fnv_word(h, (uint32_t)rt->events[e].start);
    h = fnv_word(h, (uint32_t)rt->events[e].end);
    h = digest_instances(h, &rt->events[e].instances);
  }
  h = digest_instances(h, &rt->env.default_instances);
  h = fnv_word(h, (uint32_t)rt->env.time_delta);
  h = fnv_word(h, (uint32_t)rt->env.time_slices);
  h = fnv_word(h, (uint32_t)rt->env.time_jitter);
  h = fnv_word(h, rt->env.n_dynamic);
  for (int v = 0; v < WZ3__RT_MAX_VARS; v++)
    for (int c = 0; c < 4; c++) h = fnv_word(h, f2u(rt->env.vars[v][c]));
  return h;
}

static uint32_t g_seek_state = 0x9e3779b9u;
static uint32_t seek_next(void)
{
  g_seek_state ^= g_seek_state << 13;
  g_seek_state ^= g_seek_state >> 17;
  g_seek_state ^= g_seek_state << 5;
  return g_seek_state;
}

static uint32_t run_one(wz3_rt *rt, int32_t sample)
{
  rt->trace = 1;
  rt->trace_chain = 0x811c9dc5u;
  rt->trace_count = 0;
  rt->trace_active = 0;
  const wz3_result r = wz3_rt_frame_at_sample(rt, sample, 44100);
  rt->trace = 0;
  CHECK(r == WZ3_OK, "frame at %d: %s", sample, wz3_result_string(r));
  return frame_digest(rt);
}

static void test_snapshots(const wz3_kx *kx)
{
  wz3_result res;
  wz3_rt *rt = wz3_rt_create(kx, NULL, &res);
  CHECK(rt && wz3_rt_precalc(rt) == WZ3_OK, "runtime for the seek test");
  if (!rt) return;
  const int32_t song_samples = (int32_t)((double)kx->song_length / 65536.0 * 60.0 * 44100.0 / ((double)kx->song_bpm_fixed / 65536.0));
  const long trials = 1000;
  long mismatches = 0, frames = 0;
  int32_t last_sample = 0;
  for (long trial = 0; trial < trials; trial++) {
    /* a random history, possibly seeking backwards */
    for (uint32_t i = seek_next() % 4u; i > 0; i--) {
      last_sample = (int32_t)(seek_next() % (uint32_t)(song_samples + song_samples / 50));
      run_one(rt, last_sample);
      frames++;
    }
    const size_t size = wz3_rt_snapshot_size(rt);
    uint8_t *snap = (uint8_t *)malloc(size);
    CHECK(wz3_rt_snapshot(rt, snap, size) == WZ3_OK, "snapshot");
    int32_t seq[3];
    uint32_t first[3], again[3];
    const uint32_t k = 1 + seek_next() % 3u;
    for (uint32_t j = 0; j < k; j++) {
      const uint32_t pick = seek_next() % 4u;
      seq[j] = pick == 0 ? last_sample : (int32_t)(seek_next() % (uint32_t)(song_samples + song_samples / 50));
      first[j] = run_one(rt, seq[j]);
      frames++;
    }
    for (uint32_t i = seek_next() % 3u; i > 0; i--) {
      run_one(rt, (int32_t)(seek_next() % (uint32_t)song_samples));
      frames++;
    }
    CHECK(wz3_rt_restore(rt, snap, size) == WZ3_OK, "restore");
    uint8_t *again_snap = (uint8_t *)malloc(size);
    CHECK(wz3_rt_snapshot_size(rt) == size && wz3_rt_snapshot(rt, again_snap, size) == WZ3_OK && !memcmp(snap, again_snap, size),
          "restore then snapshot reproduces the buffer");
    for (uint32_t j = 0; j < k; j++) {
      again[j] = run_one(rt, seq[j]);
      frames++;
    }
    for (uint32_t j = 0; j < k; j++)
      if (first[j] != again[j] && ++mismatches <= 3) printf("seek trial %ld frame %u differs after restore\n", trial, j);
    free(again_snap);
    free(snap);
  }
  CHECK(mismatches == 0, "%ld seek patterns diverge after restore", mismatches);
  printf("  snapshots: %ld random seek patterns (%ld frames), %ld divergences\n", trials, frames, mismatches);

  /* API behaviour */
  const size_t size = wz3_rt_snapshot_size(rt);
  uint8_t *snap = (uint8_t *)malloc(size + 8);
  CHECK(wz3_rt_snapshot(rt, snap, size - 1) == WZ3_ERR_LIMIT, "small buffer");
  CHECK(wz3_rt_snapshot(rt, snap, size) == WZ3_OK, "exact buffer");
  const uint32_t before = frame_digest(rt);
  const size_t cuts[] = { 0, 4, 12, 20, size / 2, size - 5, size - 1 };
  for (size_t i = 0; i < sizeof cuts / sizeof cuts[0]; i++)
    CHECK(wz3_rt_restore(rt, snap, cuts[i]) == WZ3_ERR_FORMAT && frame_digest(rt) == before, "truncated snapshot (%zu bytes)", cuts[i]);
  snap[0] ^= 1;
  CHECK(wz3_rt_restore(rt, snap, size) == WZ3_ERR_FORMAT, "foreign magic");
  snap[0] ^= 1;
  CHECK(wz3_rt_restore(rt, snap, size + 4) == WZ3_ERR_FORMAT, "trailing bytes");
  CHECK(wz3_rt_restore(NULL, snap, size) == WZ3_ERR_ARGUMENT && wz3_rt_snapshot(NULL, snap, size) == WZ3_ERR_ARGUMENT && wz3_rt_snapshot_size(NULL) == 0,
        "NULL arguments");
  CHECK(wz3_rt_frame(NULL, 0, 0) == WZ3_ERR_ARGUMENT && wz3_rt_frame_at_sample(rt, 0, 0) == WZ3_ERR_ARGUMENT && wz3_rt_precalc(NULL) == WZ3_ERR_ARGUMENT,
        "frame arguments");
  free(snap);
  wz3_rt_destroy(rt);
  wz3_rt_destroy(NULL);

  /* every allocation of wz3_rt_create may fail */
  const long live = g_live;
  long points = 0;
  for (long fail = 0;; fail++) {
    g_alloc_count = 0;
    g_fail_at = fail;
    wz3_rt *t = wz3_rt_create(kx, NULL, &res);
    g_fail_at = -1;
    points++;
    if (t) {
      wz3_rt_destroy(t);
      break;
    }
    CHECK(res == WZ3_ERR_OUT_OF_MEMORY && g_live == live, "create failure %ld: %s, %ld live blocks", fail, wz3_result_string(res), g_live - live);
    if (fail > 100) {
      CHECK(0, "create never succeeds");
      break;
    }
  }
  printf("  create: %ld allocation failure points\n", points);
  CHECK(g_live == live, "no blocks leaked");
}

int suite_wz3_runtime(int argc, char **argv)
{
  if (argc < 3) {
    printf("usage: test_wz3_runtime <assets dir> <fixtures dir> [--print]\n");
    return 2;
  }
  const int print = argc > 3 && !strcmp(argv[3], "--print");
  size_t kx_size, fixture_size, synth_size;
  uint8_t *kx_bytes = read_all(argv[1], "debris_party.kx", &kx_size);
  char *fixture = (char *)read_all(argv[2], "runtime_state.txt", &fixture_size);
  char *synth = (char *)read_all(argv[2], "runtime_synth.txt", &synth_size);
  wz3_kx *kx = NULL;
  size_t at;
  CHECK(wz3_kx_parse(&kx, kx_bytes, kx_size, &at) == WZ3_OK, "parse");
  if (!kx) return 1;

  int32_t samples[256];
  const size_t n_samples = read_samples(fixture, samples, 256);
  CHECK(n_samples >= 20, "fixture lists %zu frames", n_samples);

  lines got = { 0 };
  run(kx, samples, n_samples, &got, print ? NULL : synth);
  if (print) {
    for (size_t i = 0; i < got.count; i++) puts(got.text[i]);
    return 0;
  }

  size_t want_count = 0;
  size_t mismatches = 0;
  const char *p = fixture;
  while (*p) {
    const char *nl = strchr(p, '\n');
    const size_t len = nl ? (size_t)(nl - p) : strlen(p);
    if (len) {
      if (want_count < got.count) {
        if (strlen(got.text[want_count]) != len || strncmp(got.text[want_count], p, len)) {
          if (++mismatches <= 5) printf("line %zu differs\n  JS: %.*s\n  C : %s\n", want_count + 1, (int)len, p, got.text[want_count]);
        }
      }
      want_count++;
    }
    if (!nl) break;
    p = nl + 1;
  }
  CHECK(mismatches == 0, "%zu lines differ from the JS runtime", mismatches);
  CHECK(want_count == got.count, "fixture has %zu lines, C produced %zu", want_count, got.count);
  printf("runtime: %zu lines compared (%zu frames), %zu mismatches\n", got.count, n_samples, mismatches);
  test_snapshots(kx);

  free(got.text);
  free(fixture);
  free(synth);
  wz3_kx_destroy(kx);
  free(kx_bytes);
  CHECK(g_live == 0, "%ld blocks leaked", g_live);
  printf("runtime: %d checks, %d failures\n", g_checks, g_failures);
  return g_failures ? 1 : 0;
}
