/* Frame-job check for wz3.h against the JavaScript runtime.
 *
 *   test_wz3_frame <assets dir> <fixtures dir> [--print]
 *
 * Replays the script of tools/dump_frame_jobs.mjs in C - materials, scene traversal, IPP and deferred effects of
 * the party document - and compares, line by line, the IPP tree, lights, mesh jobs, effect jobs, material records
 * and the results of every deferred effect (chain-line points, the water grid, glare / colour-correction
 * parameters). Then: a snapshot taken mid-script restores bit-identically, and every allocation of the whole
 * pipeline may fail.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

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
static int g_hash_versions = 1; /* material update counters are not part of a snapshot */
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

/* ---- hashing (tools/dump_frame_jobs.mjs) ---- */

typedef struct hash {
  uint32_t h;
} hash;
static void hash_init(hash *h) { h->h = 0x811c9dc5u; }
static void hu(hash *h, uint32_t v) { h->h = (h->h ^ v) * 0x01000193u; }
static void hi(hash *h, int32_t v) { hu(h, (uint32_t)v); }
static void hf(hash *h, float f)
{
  uint32_t u;
  memcpy(&u, &f, 4);
  if (f != f) u = 0x7fc00000u;
  hu(h, u);
}
static void hd(hash *h, double d)
{
  uint64_t u;
  memcpy(&u, &d, 8);
  if (d != d) u = 0x7ff8000000000000ull;
  hu(h, (uint32_t)u);
  hu(h, (uint32_t)(u >> 32));
}
static void hfs(hash *h, const float *v, size_t n)
{
  for (size_t i = 0; i < n; i++) hf(h, v[i]);
}

typedef struct stats {
  uint32_t nodes, meshes, effects, lights, instances, chain, water, glare, cc, waves;
} stats;

static void hash_light(hash *h, const wz3_light_job *l)
{
  hu(h, l->directional ? 1u : 0u);
  hu(h, l->op);
  hfs(h, l->position, 3);
  hfs(h, l->direction, 3);
  hu(h, l->flags);
  hu(h, l->color);
  hf(h, l->amplify);
  hd(h, l->range);
  hi(h, l->event);
}

typedef struct frame_hash {
  uint32_t outputs, nodes, meshes, effects, instances;
  hash ipp, lights, mesh, effect;
} frame_hash;

static void visit_node(const wz3_frame *f, uint32_t index, frame_hash *fh, stats *st)
{
  const wz3_ipp_node *node = &f->nodes[index];
  fh->nodes++;
  if (node->type == WZ3_IPP_LAYER2D) {
    hu(&fh->ipp, 2);
    hi(&fh->ipp, node->size);
    hu(&fh->ipp, node->flags);
    hi(&fh->ipp, node->material_op);
    for (int i = 0; i < 4; i++) hd(&fh->ipp, node->screen[i]);
    for (int i = 0; i < 4; i++) hd(&fh->ipp, node->uv[i]);
    hf(&fh->ipp, node->z);
    hi(&fh->ipp, node->clear_flags);
    hi(&fh->ipp, node->input >= 0 ? 1 : 0);
    if (node->input >= 0) visit_node(f, (uint32_t)node->input, fh, st);
    return;
  }
  hash *ipp = &fh->ipp;
  hu(ipp, 1);
  hi(ipp, node->size);
  hu(ipp, node->flags);
  hu(ipp, node->clear_color);
  hi(ipp, node->paint_mode);
  hfs(ipp, node->crop, 4);
  hf(ipp, node->eye_distance);
  hf(ipp, node->focal_distance);
  const wz3_camera *c = &node->camera;
  hfs(ipp, c->camera_space, 16);
  hfs(ipp, c->model_space, 16);
  hd(ipp, c->far_clip);
  hd(ipp, c->near_clip);
  hd(ipp, c->center_x);
  hd(ipp, c->center_y);
  hd(ipp, c->zoom_x);
  hd(ipp, c->zoom_y);
  hu(ipp, c->fog_color);
  hd(ipp, c->fog_start);
  hd(ipp, c->fog_end);
  hu(ipp, node->ambient);
  hu(&fh->lights, node->n_light);
  for (uint32_t i = 0; i < node->n_light; i++) hash_light(&fh->lights, &f->light_jobs[node->first_light + i]);
  hash_light(&fh->lights, &node->default_light);
  st->lights += node->n_light;
  hu(&fh->mesh, node->n_mesh);
  for (uint32_t i = 0; i < node->n_mesh; i++) {
    const wz3_mesh_job *job = &f->mesh_jobs[node->first_mesh + i];
    hi(&fh->mesh, job->source_op);
    hi(&fh->mesh, job->mesh_op);
    hfs(&fh->mesh, job->matrix, 16);
    hf(&fh->mesh, job->time);
    hi(&fh->mesh, job->pass_adjust);
    if (job->has_instances) {
      hu(&fh->mesh, 1);
      hu(&fh->mesh, job->n_instances);
      hfs(&fh->mesh, f->instances + (size_t)job->first_instance * 16, (size_t)job->n_instances * 16);
      fh->instances += job->n_instances;
    } else {
      hu(&fh->mesh, 0);
    }
    fh->meshes++;
  }
  hu(&fh->effect, node->n_effect);
  for (uint32_t i = 0; i < node->n_effect; i++) {
    const wz3_effect_job *job = &f->effect_jobs[node->first_effect + i];
    hu(&fh->effect, job->effect_op);
    hfs(&fh->effect, job->matrix, 16);
    hfs(&fh->effect, f->effect_variables + job->variables, 128);
    hfs(&fh->effect, f->marker_tables + (size_t)job->markers * 512, 512);
    hi(&fh->effect, job->pass_adjust);
    fh->effects++;
  }
}

static void hash_frame(const wz3_frame *f, frame_hash *fh, stats *st)
{
  memset(fh, 0, sizeof *fh);
  hash_init(&fh->ipp);
  hash_init(&fh->lights);
  hash_init(&fh->mesh);
  hash_init(&fh->effect);
  fh->outputs = f->n_outputs;
  for (uint32_t i = 0; i < f->n_outputs; i++) visit_node(f, f->outputs[i], fh, st);
  st->nodes += fh->nodes;
  st->meshes += fh->meshes;
  st->effects += fh->effects;
  st->instances += fh->instances;
}

static void hash_materials(const wz3_rt *rt, uint32_t n_ops, uint32_t *count, uint32_t *out)
{
  hash h;
  hash_init(&h);
  *count = 0;
  for (uint32_t op = 0; op < n_ops; op++) {
    const wz3_material *m = wz3_rt_material(rt, op);
    if (!m) continue;
    (*count)++;
    hu(&h, op);
    hu(&h, m->system);
    hu(&h, g_hash_versions ? m->version : 0);
    hu(&h, m->n_params);
    for (uint32_t i = 0; i < m->n_params; i++) hd(&h, m->params[i]);
    for (uint32_t i = 0; i < m->n_params; i++) hd(&h, m->initial[i]);
    for (int i = 0; i < 7; i++) hi(&h, m->textures[i]);
    hu(&h, m->n_passes);
    for (uint32_t i = 0; i < m->n_passes; i++) {
      const wz3_material_pass *p = &m->passes[i];
      hu(&h, p->usage);
      hu(&h, p->program);
      hi(&h, p->render_pass);
      hd(&h, p->size);
      hd(&h, p->aspect);
      hu(&h, p->state);
      hu(&h, p->owner);
    }
    hu(&h, m->multi_flags);
    hu(&h, m->shader_mask);
  }
  *out = h.h;
}

/* geometry whose static indices were hashed already */
static const wz3_water_geometry *g_seen_water[8];
static void hash_output(hash *h, const wz3_effect_output *o)
{
  if (o->kind == WZ3_EFFECT_OUT_CHAIN_LINE) {
    hu(h, 1);
    hu(h, o->op);
    hi(h, o->material_op);
    hfs(h, o->matrix, 16);
    hfs(h, o->points, 96);
    hd(h, o->thickness);
    hi(h, o->ripped);
    hu(h, o->color);
  } else if (o->kind == WZ3_EFFECT_OUT_WATER) {
    const wz3_water_geometry *g = o->water;
    hu(h, 2);
    hu(h, o->op);
    hi(h, o->material_op);
    hi(h, o->spline_op);
    hfs(h, o->matrix, 16);
    hd(h, o->time);
    hu(h, o->n_params);
    for (uint32_t i = 0; i < o->n_params; i++) hd(h, o->params[i]);
    hi(h, g->grid);
    hi(h, g->width);
    hi(h, g->height);
    hu(h, g_hash_versions ? g->version : 0);
    hu(h, g->wave_count);
    hu(h, g->uv_changed ? 1u : 0u);
    hfs(h, g->bounds_min, 3);
    hfs(h, g->bounds_max, 3);
    hfs(h, g->positions, (size_t)g->n_vertices * 3);
    hfs(h, g->normals, (size_t)g->n_vertices * 3);
    hfs(h, g->uvs, (size_t)g->n_vertices * 2);
    int seen = 0;
    for (int i = 0; i < 8 && g_seen_water[i]; i++) seen |= g_seen_water[i] == g;
    if (!seen) {
      for (int i = 0; i < 8; i++)
        if (!g_seen_water[i]) {
          g_seen_water[i] = g;
          break;
        }
      hu(h, g->n_indices);
      for (uint32_t i = 0; i < g->n_indices; i++) hu(h, g->indices[i]);
    }
  } else {
    hu(h, o->kind == WZ3_EFFECT_OUT_GLARE ? 3u : 4u);
    hu(h, o->op);
    hu(h, o->n_params);
    for (uint32_t i = 0; i < o->n_params; i++) hd(h, o->params[i]);
  }
}

static wz3_result capture_node(wz3_rt *rt, const wz3_frame *f, uint32_t index, hash *h, uint32_t *count, stats *st)
{
  const wz3_ipp_node *node = &f->nodes[index];
  if (node->type == WZ3_IPP_LAYER2D) return node->input >= 0 ? capture_node(rt, f, (uint32_t)node->input, h, count, st) : WZ3_OK;
  if (node->type != WZ3_IPP_VIEWPORT || !node->n_effect) return WZ3_OK;
  uint32_t *order = (uint32_t *)malloc((size_t)node->n_effect * sizeof(uint32_t));
  wz3_frame_effect_order(rt, node, order);
  wz3_result r = WZ3_OK;
  for (uint32_t i = 0; i < node->n_effect && r == WZ3_OK; i++) {
    wz3_effect_job job = f->effect_jobs[node->first_effect + order[i]];
    const wz3_effect_output *out;
    r = wz3_rt_effect_run(rt, &job, &out);
    if (r != WZ3_OK) break;
    (*count)++;
    if (out->kind) {
      hash_output(h, out);
      switch (out->kind) {
      case WZ3_EFFECT_OUT_CHAIN_LINE: st->chain++; break;
      case WZ3_EFFECT_OUT_WATER:
        st->water++;
        st->waves += out->water->wave_count;
        break;
      case WZ3_EFFECT_OUT_GLARE: st->glare++; break;
      default: st->cc++; break;
      }
    }
    /* the frame arrays are stable while effects run; the job was copied anyway */
  }
  free(order);
  return r;
}

typedef struct script_entry {
  int32_t sample;
  char mode;
} script_entry;

static int g_detail_frame = -1; /* WZ3_DETAIL=<case>:<frame> prints the mesh jobs of that frame of a synthetic case */


typedef struct lines {
  char (*text)[512];
  size_t count, cap;
} lines;
static void emit(lines *l, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void emit(lines *l, const char *fmt, ...)
{
  if (l->count == l->cap) {
    l->cap = l->cap ? l->cap * 2 : 256;
    l->text = (char (*)[512])realloc(l->text, l->cap * 512);
  }
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(l->text[l->count++], 512, fmt, ap);
  va_end(ap);
}

static size_t read_script(const char *fixture, script_entry *script, size_t max)
{
  size_t n = 0;
  const char *p = fixture;
  while (*p && n < max) {
    unsigned index;
    long sample;
    char mode;
    if (sscanf(p, "F %u %ld %c", &index, &sample, &mode) == 3) {
      script[n].sample = (int32_t)sample;
      script[n].mode = mode;
      n++;
    }
    const char *nl = strchr(p, '\n');
    if (!nl) break;
    p = nl + 1;
  }
  return n;
}

static wz3_rt *make_runtime(const wz3_kx *kx, wz3_result *res)
{
  wz3_rt_options opt;
  memset(&opt, 0, sizeof opt);
  opt.generic_exec = 1;
  opt.frame_jobs = 1;
  opt.geometry = WZ3_GEOMETRY_PLACEHOLDER;
  wz3_rt *rt = wz3_rt_create(kx, &opt, res);
  if (rt) wz3_rt_set_aspect(rt, 2.0);
  return rt;
}

/* one frame of the script; fills the line fields and returns the result */
static wz3_result run_frame(wz3_rt *rt, const script_entry *e, uint32_t n_ops, lines *out, unsigned index, stats *total)
{
  wz3_result r = wz3_rt_frame_at_sample(rt, e->sample, 44100);
  if (r != WZ3_OK) return r;
  const wz3_frame *f = wz3_rt_frame_jobs(rt);
  frame_hash fh;
  hash_frame(f, &fh, total);
  if ((int)index == g_detail_frame) {
    for (uint32_t i = 0; i < f->n_outputs; i++) {
      const wz3_ipp_node *node = &f->nodes[f->outputs[i]];
      if (node->type != WZ3_IPP_VIEWPORT) continue;
      for (uint32_t k = 0; k < node->n_mesh; k++) {
        const wz3_mesh_job *job = &f->mesh_jobs[node->first_mesh + k];
        hash h;
        hash_init(&h);
        hfs(&h, job->matrix, 16);
        if (job->has_instances) hfs(&h, f->instances + (size_t)job->first_instance * 16, (size_t)job->n_instances * 16);
        printf("J %u src %d mesh %d inst %d %x\n", k, job->source_op, job->mesh_op, job->has_instances ? (int)job->n_instances : -1, h.h);
        if (job->has_instances && job->n_instances) {
          printf("  first");
          for (int m = 0; m < 16; m++) {
            char b[64];
            snprintf(b, sizeof b, "%.9g", (double)f->instances[(size_t)job->first_instance * 16 + m]);
            printf(" %s", b);
          }
          printf("\n");
        }
      }
    }
  }
  hash eh;
  hash_init(&eh);
  uint32_t run = 0;
  stats st;
  memset(&st, 0, sizeof st);
  if (e->mode == 'c') {
    for (uint32_t i = 0; i < f->n_outputs && r == WZ3_OK; i++) r = capture_node(rt, f, f->outputs[i], &eh, &run, &st);
  } else {
    r = wz3_rt_effects_advance(rt, &run);
  }
  if (r != WZ3_OK) return r;
  total->chain += st.chain;
  total->water += st.water;
  total->glare += st.glare;
  total->cc += st.cc;
  total->waves += st.waves;
  uint32_t mat_count, mat_hash;
  hash_materials(rt, n_ops, &mat_count, &mat_hash);
  if (out)
    emit(out, "F %u %d %c outputs %u nodes %u ipp %x lights %x meshes %u %x effects %u %x instances %u run %u out %x mat %x ts %d", index, e->sample, e->mode,
         fh.outputs, fh.nodes, fh.ipp.h, fh.lights.h, fh.meshes, fh.mesh.h, fh.effects, fh.effect.h, fh.instances, run, eh.h, mat_hash, rt->env.time_slices);
  return WZ3_OK;
}

typedef struct override {
  uint32_t op, index, bits;
} override;

static wz3_rt *make_runtime_with(const wz3_kx *kx, const override *overrides, size_t n_overrides, wz3_result *res)
{
  wz3_rt *rt = make_runtime(kx, res);
  if (!rt) return NULL;
  for (size_t i = 0; i < n_overrides; i++) {
    wz3__rt_op *op = &rt->ops[overrides[i].op];
    op->edit[overrides[i].index].u = overrides[i].bits;
    op->anim[overrides[i].index].u = overrides[i].bits;
  }
  return rt;
}

static void run_script(const wz3_kx *kx, const override *overrides, size_t n_overrides, const script_entry *script, size_t n, lines *out)
{
  wz3_result res;
  wz3_rt *rt = make_runtime_with(kx, overrides, n_overrides, &res);
  CHECK(rt != NULL && res == WZ3_OK, "runtime create: %s", wz3_result_string(res));
  if (!rt) return;
  res = wz3_rt_precalc(rt);
  CHECK(res == WZ3_OK, "precalc: %s", wz3_result_string(res));
  {
    uint32_t count, h;
    hash_materials(rt, kx->n_ops, &count, &h);
    emit(out, "M0 %u %x", count, h);
    hash eh;
    hash_init(&eh);
    uint32_t effects = 0;
    for (uint32_t op = 0; op < kx->n_ops; op++) {
      const wz3_effect *e = wz3_rt_effect(rt, op);
      if (!e) continue;
      effects++;
      hu(&eh, op);
      hu(&eh, e->type);
      hi(&eh, e->material_op);
      hi(&eh, e->pass);
      hu(&eh, e->usage);
      hu(&eh, e->need_current_render);
    }
    emit(out, "E0 %u %x", effects, eh.h);
  }
  stats total;
  memset(&total, 0, sizeof total);
  for (size_t i = 0; i < n; i++) {
    res = run_frame(rt, &script[i], kx->n_ops, out, (unsigned)i, &total);
    CHECK(res == WZ3_OK, "frame %zu: %s", i, wz3_result_string(res));
    if (res != WZ3_OK) break;
  }
  emit(out, "T nodes %u meshes %u effects %u lights %u instances %u chain %u water %u glare %u cc %u waves %u", total.nodes, total.meshes, total.effects,
       total.lights, total.instances, total.chain, total.water, total.glare, total.cc, total.waves);
  wz3_rt_destroy(rt);
}

/* compares the C lines with the expected text (up to `length` bytes); returns the mismatch count */
static size_t compare_lines(const char *expected, size_t length, const lines *got, const char *label)
{
  size_t want = 0, mismatches = 0;
  const char *p = expected, *end = expected + length;
  while (p < end) {
    const char *nl = memchr(p, '\n', (size_t)(end - p));
    const size_t len = nl ? (size_t)(nl - p) : (size_t)(end - p);
    if (len) {
      if (want < got->count && (strlen(got->text[want]) != len || strncmp(got->text[want], p, len))) {
        if (++mismatches <= 3) printf("%s line %zu differs\n  JS: %.*s\n  C : %s\n", label, want + 1, (int)len, p, got->text[want]);
      }
      want++;
    }
    if (!nl) break;
    p = nl + 1;
  }
  if (want != got->count) {
    printf("%s: expected %zu lines, C produced %zu\n", label, want, got->count);
    mismatches++;
  }
  return mismatches;
}

/* generated cases: data words replaced before the init pass (tools/dump_frame_jobs_synth.mjs) */
static void test_synthetic(const wz3_kx *kx, const char *text)
{
  size_t cases = 0, bad = 0, frames = 0;
  const char *p = strstr(text, "S ");
  while (p) {
    const char *next = strstr(p + 2, "\nS ");
    const char *end = next ? next + 1 : text + strlen(text);
    unsigned seed, count, n_rewires;
    if (sscanf(p, "S %u %u %u", &seed, &count, &n_rewires) != 3) break;
    override *overrides = (override *)malloc(((size_t)count + 1) * sizeof *overrides);
    const char *line = strchr(p, '\n') + 1;
    size_t n_over = 0, n_re = 0;
    while (line < end && line[0] == 'X' && n_over < count) {
      unsigned op, index, bits;
      sscanf(line, "X %u %u %x", &op, &index, &bits);
      overrides[n_over].op = op;
      overrides[n_over].index = index;
      overrides[n_over].bits = bits;
      n_over++;
      line = strchr(line, '\n') + 1;
    }
    CHECK(n_over == count, "case %u lists %zu of %u overrides", seed, n_over, count);
    /* rewires patch the (shared) document and are undone after the case */
    wz3_kx *mutable_kx = (wz3_kx *)kx;
    uint32_t *patched[256];
    const uint32_t *saved_inputs[256];
    uint32_t saved_n[256], saved_op[256];
    while (line < end && line[0] == 'R' && n_re < n_rewires && n_re < 256) {
      unsigned op, target;
      sscanf(line, "R %u %u", &op, &target);
      patched[n_re] = (uint32_t *)malloc(sizeof(uint32_t));
      patched[n_re][0] = target;
      saved_op[n_re] = op;
      saved_inputs[n_re] = mutable_kx->ops[op].inputs;
      saved_n[n_re] = mutable_kx->ops[op].n_inputs;
      mutable_kx->ops[op].inputs = patched[n_re];
      mutable_kx->ops[op].n_inputs = 1;
      n_re++;
      line = strchr(line, '\n') + 1;
    }
    CHECK(n_re == n_rewires, "case %u lists %zu of %u rewires", seed, n_re, n_rewires);
    static script_entry script[512];
    char *body = (char *)malloc((size_t)(end - line) + 1);
    memcpy(body, line, (size_t)(end - line));
    body[end - line] = 0;
    const size_t n = read_script(body, script, 512);
    lines got = { 0 };
    {
      const char *detail = getenv("WZ3_DETAIL");
      unsigned dc, df;
      if (detail && sscanf(detail, "%u:%u", &dc, &df) == 2 && dc == seed) g_detail_frame = (int)df;
    }
    run_script(kx, overrides, n_over, script, n, &got);
    g_detail_frame = -1;
    char label[32];
    snprintf(label, sizeof label, "synthetic %u", seed);
    const size_t mismatches = compare_lines(body, strlen(body), &got, label);
    CHECK(mismatches == 0, "synthetic case %u: %zu lines differ from the JS runtime", seed, mismatches);
    for (size_t i = n_re; i-- > 0;) {
      mutable_kx->ops[saved_op[i]].inputs = saved_inputs[i];
      mutable_kx->ops[saved_op[i]].n_inputs = saved_n[i];
      free(patched[i]);
    }
    cases++;
    bad += mismatches != 0;
    frames += n;
    free(got.text);
    free(body);
    free(overrides);
    p = next ? next + 1 : NULL;
  }
  CHECK(cases >= 6, "%zu synthetic cases", cases);
  printf("frame jobs: %zu synthetic cases, %zu frames, %zu differ\n", cases, frames, bad);
}

/* a snapshot taken mid-script brings every later frame back bit for bit */
static void test_snapshot(const wz3_kx *kx, const script_entry *script, size_t n)
{
  wz3_result res;
  wz3_rt *rt = make_runtime(kx, &res);
  if (!rt) {
    CHECK(0, "create");
    return;
  }
  res = wz3_rt_precalc(rt);
  CHECK(res == WZ3_OK, "precalc");
  stats total;
  memset(&total, 0, sizeof total);
  g_hash_versions = 0;
  const size_t split = n > 150 ? 150 : n / 2;
  for (size_t i = 0; i < split && res == WZ3_OK; i++) res = run_frame(rt, &script[i], kx->n_ops, NULL, (unsigned)i, &total);
  CHECK(res == WZ3_OK, "prefix: %s", wz3_result_string(res));
  const size_t size = wz3_rt_snapshot_size(rt);
  void *snap = malloc(size);
  CHECK(wz3_rt_snapshot(rt, snap, size) == WZ3_OK, "snapshot");
  CHECK(wz3_rt_snapshot(rt, snap, size - 1) == WZ3_ERR_LIMIT, "short buffer");
  lines a = { 0 }, b = { 0 };
  for (size_t i = split; i < n && res == WZ3_OK; i++) res = run_frame(rt, &script[i], kx->n_ops, &a, (unsigned)i, &total);
  CHECK(res == WZ3_OK, "suffix: %s", wz3_result_string(res));
  CHECK(wz3_rt_restore(rt, snap, size) == WZ3_OK, "restore");
  for (size_t i = split; i < n && res == WZ3_OK; i++) res = run_frame(rt, &script[i], kx->n_ops, &b, (unsigned)i, &total);
  size_t diff = 0;
  for (size_t i = 0; i < a.count && i < b.count; i++)
    if (strcmp(a.text[i], b.text[i])) {
      if (++diff <= 3) printf("replay differs:\n  %s\n  %s\n", a.text[i], b.text[i]);
    }
  CHECK(a.count == b.count && diff == 0, "restored replay: %zu of %zu frames differ", diff, a.count);
  printf("  snapshot: %zu bytes, %zu frames replayed, %zu differ\n", size, a.count, diff);
  /* damaged snapshots are rejected without touching the runtime */
  CHECK(wz3_rt_restore(rt, snap, size - 3) == WZ3_ERR_FORMAT, "truncated snapshot");
  ((uint8_t *)snap)[0] ^= 1;
  CHECK(wz3_rt_restore(rt, snap, size) == WZ3_ERR_FORMAT, "foreign snapshot");
  free(snap);
  free(a.text);
  free(b.text);
  g_hash_versions = 1;
  wz3_rt_destroy(rt);
}

/* once every instance memory exists, a frame (and the deferred effects) allocates nothing */
static void test_steady_state(const wz3_kx *kx, const script_entry *script, size_t n)
{
  wz3_result res;
  wz3_rt *rt = make_runtime(kx, &res);
  if (!rt) {
    CHECK(0, "create");
    return;
  }
  res = wz3_rt_precalc(rt);
  stats total;
  memset(&total, 0, sizeof total);
  for (size_t i = 0; i < n && res == WZ3_OK; i++) res = run_frame(rt, &script[i], kx->n_ops, NULL, (unsigned)i, &total);
  CHECK(res == WZ3_OK, "warm-up: %s", wz3_result_string(res));
  g_alloc_count = 0;
  for (size_t i = 0; i < n && res == WZ3_OK; i++) {
    res = wz3_rt_frame_at_sample(rt, script[i].sample, 44100);
    uint32_t run = 0;
    if (res == WZ3_OK) res = wz3_rt_effects_advance(rt, &run);
  }
  CHECK(res == WZ3_OK, "steady state: %s", wz3_result_string(res));
  CHECK(g_alloc_count == 0, "%ld allocations in %zu steady-state frames", g_alloc_count, n);
  printf("  steady state: %ld allocations in %zu frames\n", g_alloc_count, n);
  wz3_rt_destroy(rt);
}

/* every allocation of create, precalc, frames, effect execution, snapshot and restore may fail */
static void test_oom(const wz3_kx *kx, const script_entry *script, size_t n)
{
  const long live = g_live;
  long points = 0, failed = 0;
  /* count the allocations of one clean run first */
  g_alloc_count = 0;
  g_fail_at = -1;
  {
    wz3_result res;
    wz3_rt *rt = make_runtime(kx, &res);
    stats total;
    memset(&total, 0, sizeof total);
    res = rt ? wz3_rt_precalc(rt) : res;
    for (size_t i = 0; rt && res == WZ3_OK && i < n && i < 40; i++) res = run_frame(rt, &script[i], kx->n_ops, NULL, (unsigned)i, &total);
    wz3_rt_destroy(rt);
  }
  const long total_allocs = g_alloc_count;
  const long stride = total_allocs / 400 + 1;
  for (long fail = 0; fail < total_allocs; fail += (fail < 80 ? 1 : stride)) {
    wz3_result res;
    g_alloc_count = 0;
    g_fail_at = fail;
    wz3_rt *rt = make_runtime(kx, &res);
    stats total;
    memset(&total, 0, sizeof total);
    if (rt) res = wz3_rt_precalc(rt);
    for (size_t i = 0; rt && res == WZ3_OK && i < n && i < 40; i++) res = run_frame(rt, &script[i], kx->n_ops, NULL, (unsigned)i, &total);
    g_fail_at = -1;
    points++;
    if (res != WZ3_OK) {
      failed++;
      CHECK(res == WZ3_ERR_OUT_OF_MEMORY, "failure at allocation %ld reported as %s", fail, wz3_result_string(res));
    }
    wz3_rt_destroy(rt);
    CHECK(g_live == live, "allocation %ld: %ld live blocks after destroy", fail, g_live - live);
    if (g_live != live) break;
  }
  printf("  oom: %ld failure points of %ld allocations, %ld reported\n", points, total_allocs, failed);
}

int suite_wz3_frame(int argc, char **argv)
{
  if (argc < 3) {
    printf("usage: test_wz3_frame <assets dir> <fixtures dir> [--print]\n");
    return 2;
  }
  const int print = argc > 3 && !strcmp(argv[3], "--print");
  size_t kx_size, fixture_size;
  uint8_t *kx_bytes = read_all(argv[1], "debris_party.kx", &kx_size);
  char *fixture = (char *)read_all(argv[2], "frame_jobs.txt", &fixture_size);
  size_t synth_size;
  char *synth = (char *)read_all(argv[2], "frame_jobs_synth.txt", &synth_size);
  wz3_kx *kx = NULL;
  size_t at;
  CHECK(wz3_kx_parse(&kx, kx_bytes, kx_size, &at) == WZ3_OK, "parse");
  if (!kx) return 1;
  static script_entry script[1024];
  const size_t n = read_script(fixture, script, 1024);
  CHECK(n >= 100, "fixture lists %zu frames", n);

  lines got = { 0 };
  emit(&got, "document ops %u events %u aspect 2", kx->n_ops, kx->n_events);
  run_script(kx, NULL, 0, script, n, &got);
  if (print) {
    for (size_t i = 0; i < got.count; i++) puts(got.text[i]);
    return 0;
  }
  const size_t mismatches = compare_lines(fixture, strlen(fixture), &got, "production");
  CHECK(mismatches == 0, "%zu lines differ from the JS runtime", mismatches);
  printf("frame jobs: %zu lines compared (%zu frames), %zu mismatches\n", got.count, n, mismatches);
  /* the script must exercise every job kind, or the comparison proves nothing */
  {
    unsigned long v[10] = { 0 };
    if (got.count)
      sscanf(got.text[got.count - 1], "T nodes %lu meshes %lu effects %lu lights %lu instances %lu chain %lu water %lu glare %lu cc %lu waves %lu", &v[0],
             &v[1], &v[2], &v[3], &v[4], &v[5], &v[6], &v[7], &v[8], &v[9]);
    CHECK(v[0] > 100 && v[1] > 1000 && v[2] > 1000 && v[3] > 100 && v[4] > 1000 && v[5] > 100 && v[6] >= 10 && v[7] >= 10 && v[8] >= 10 && v[9] >= 10,
          "script coverage: %lu nodes, %lu meshes, %lu effect jobs, %lu lights, %lu instances, %lu chain, %lu water, %lu glare, %lu cc, %lu waves", v[0], v[1],
          v[2], v[3], v[4], v[5], v[6], v[7], v[8], v[9]);
  }
  test_synthetic(kx, synth);
  test_snapshot(kx, script, n);
  test_steady_state(kx, script, n);
  test_oom(kx, script, n);

  free(got.text);
  free(fixture);
  free(synth);
  wz3_kx_destroy(kx);
  free(kx_bytes);
  CHECK(g_live == 0, "%ld blocks leaked", g_live);
  printf("frame jobs: %d checks, %d failures\n", g_checks, g_failures);
  return g_failures ? 1 : 0;
}
