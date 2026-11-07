/* Material-view check for wz3.h against src/renderer.js.
 *
 *   test_wz3_material_view <assets dir> <fixtures dir> [--print]
 *
 * Replays tools/dump_material_view.mjs: random differential cases of materialView / renderMode / materialState, the
 * Material 1.1 and 2.0 UV and environment helpers and the two colour combiners (every line carries its inputs and the
 * hash of the JavaScript result), and every pass of every material of the Debris document at nine times.
 */
#define WZ3_IMPLEMENTATION
#include <debris/core/wz3.h>
#include "support/gate_common.h"

/* JavaScript `value >>> 0` */
static uint32_t tou32(double v) { return (uint32_t)wz3__rt_to_int32(v); }

static void hash_view(hash *h, const wz3_material_view *v)
{
  hu(h, v->system);
  hu(h, v->usage);
  hi(h, v->state);
  hu(h, v->base_flags);
  hu(h, v->special_flags);
  hu(h, v->light_flags);
  hu(h, v->flags);
  hu(h, v->runtime_flags);
  hu(h, v->environment_flags);
  hu(h, v->runtime_environment_flags);
  hu(h, v->multi_flags);
  hu(h, v->alpha_combiner);
  hu(h, v->color);
  hu(h, v->specular);
  hu(h, v->specular_color);
  for (int i = 0; i < 4; i++) hu(h, v->texture_flags[i]);
  for (int i = 0; i < 4; i++) hu(h, v->extra_texture_flags[i]);
  for (int i = 0; i < 4; i++) hu(h, v->light_flags4[i]);
  for (int i = 0; i < 4; i++) hu(h, v->slot_texture_flags[i]);
  for (int i = 0; i < 13; i++) hu(h, v->combiners[i]);
  for (int i = 0; i < 4; i++) hu(h, v->colors[i]);
  for (int i = 0; i < 2; i++) hu(h, v->detail_ops[i]);
  for (int i = 0; i < 4; i++) hi(h, v->texture_map[i]);
  hu(h, v->vertex_color_mode);
  hu(h, v->material11_combiner);
  hu(h, v->generated_base_alpha);
  hu(h, v->use_prelight);
  for (int i = 0; i < 4; i++) hd(h, v->texture_scales[i]);
  for (int i = 0; i < 4; i++) hd(h, v->extra_texture_scales[i]);
  for (int i = 0; i < 4; i++) hd(h, v->light_scales[i]);
  for (int i = 0; i < 4; i++) hd(h, v->slot_texture_scales[i]);
  hd(h, v->specular_power);
  hd(h, v->specular_strength);
  hd(h, v->uv_scale);
  hd(h, v->alpha_cutoff);
  hd(h, v->light_scale);
  hfs(h, v->uv_transform1, 8);
  hfs(h, v->uv_transform2, 8);
}

static void hash_state(hash *h, const wz3_material_state *s)
{
  hu(h, s->depth_test);
  hu(h, s->depth_write);
  hu(h, s->depth_func);
  hu(h, s->color_write);
  hu(h, s->cull);
  hu(h, s->stencil_test);
  if (!s->blend)
    hu(h, 0);
  else {
    hu(h, 1);
    hu(h, s->blend_src);
    hu(h, s->blend_dst);
    hu(h, s->blend_equation);
  }
}

/* what the fixture lines say the generator covered */
static struct {
  uint32_t modes, depth_funcs, culls, blends, systems, generated, vcm;
  unsigned multipass, owners, defaults, stencil, combiner_views;
  uint32_t bind_modes;
  unsigned clamped, sampled;
} g_seen;

static void hash_setup(hash *h, const wz3_material_view *v)
{
  wz3_sampler_setup su;
  wz3_material_sampler_setup(v, &su);
  for (int i = 0; i < 4; i++) {
    wz3_sampler_request rq;
    wz3_sampler_setup_request(v, &su, i, &rq);
    hi(h, su.texture_op[i]);
    hi(h, su.m20_flags[i]);
    hi(h, su.m11_flags[i]);
    hf(h, su.m20_scales[i]);
    hf(h, su.m11_scales[i]);
    hu(h, su.bind_flags[i]);
    hu(h, rq.clamp ? 1u : 0u);
    hu(h, (uint32_t)rq.filter_mode);
    hu(h, rq.material11 ? 1u : 0u);
    CHECK(rq.has_clamp && rq.has_filter_mode && !rq.has_nearest, "slot request flags");
    g_seen.bind_modes |= 1u << (rq.filter_mode & 7);
    if (rq.clamp) g_seen.clamped++;
    if (su.texture_op[i] >= 0) g_seen.sampled++;
  }
  hi(h, su.texture_mask);
}

static void hash_view_all(hash *h, const wz3_material_view *v)
{
  wz3_material_state s;
  hash_view(h, v);
  const int mode = wz3_material_render_mode(v);
  hu(h, (uint32_t)mode);
  wz3_material_state_make(v, &s);
  hash_state(h, &s);
  hash_setup(h, v);
  g_seen.modes |= 1u << mode;
  g_seen.depth_funcs |= 1u << s.depth_func;
  g_seen.culls |= 1u << s.cull;
  g_seen.blends |= 1u << ((v->base_flags & 0xf000u) >> 12);
  g_seen.systems |= v->system == 20 ? 2u : 1u;
  g_seen.generated |= 1u << v->generated_base_alpha;
  g_seen.vcm |= 1u << v->vertex_color_mode;
  if (v->multi_flags & 0x1e) g_seen.multipass++;
  if (s.stencil_test) g_seen.stencil++;
  if (v->material11_combiner) g_seen.combiner_views++;
}

/* one material record of a `view` line: system texmask has_params has_init n [sparse params] [sparse init] */
typedef struct material_in {
  wz3_material m;
  double params[64], init[64];
} material_in;

static void rpairs(reader *r, double *out, uint32_t n)
{
  for (uint32_t i = 0; i < n; i++) out[i] = 0.0;
  const uint32_t count = ru32(r);
  for (uint32_t k = 0; k < count; k++) {
    const uint32_t index = ru32(r);
    const double value = rnum(r);
    if (index < n) out[index] = value;
  }
}

static int rmaterial(reader *r, material_in *mi)
{
  const uint32_t system = ru32(r);
  if (!system) return 0;
  memset(mi, 0, sizeof *mi);
  mi->m.system = (uint8_t)system;
  const uint32_t mask = ru32(r), has_params = ru32(r), has_init = ru32(r), n = ru32(r);
  for (int i = 0; i < 7; i++) mi->m.textures[i] = (mask >> i) & 1u ? i + 1 : -1;
  mi->m.n_params = n;
  if (has_params) {
    rpairs(r, mi->params, n);
    mi->m.params = mi->params;
  }
  if (has_init) {
    rpairs(r, mi->init, n);
    mi->m.initial = mi->init;
  }
  return 1;
}

static void rvec3(reader *r, double *v) { rnums(r, v, 3); }

/* a vector input that may be absent: `1 <n numbers>` or `0` */
static const double *roptional(reader *r, double *storage, int n)
{
  if (!ru32(r)) return NULL;
  rnums(r, storage, n);
  return storage;
}

static uint32_t run_case(const char *kind, reader *r)
{
  hash h;
  hash_init(&h);
  if (!strcmp(kind, "view")) {
    const uint32_t usage = ru32(r);
    const int32_t state = ri32(r);
    const int has_pass = (int)ru32(r), has_owner = (int)ru32(r);
    static material_in dynamic, owner;
    const int has_dynamic = rmaterial(r, &dynamic);
    if (has_owner) rmaterial(r, &owner);
    wz3_material_pass pass;
    memset(&pass, 0, sizeof pass);
    pass.usage = (uint8_t)usage;
    pass.state = (uint8_t)(state < 0 ? 0 : state);
    wz3_material_view v;
    wz3_material_view_make(has_dynamic ? &dynamic.m : NULL, (has_owner && has_pass) ? &owner.m : NULL, has_pass ? &pass : NULL, &v);
    hash_view_all(&h, &v);
    hash_view_all(&h, &v);
    if (!has_dynamic) g_seen.defaults++;
    if (has_owner && has_dynamic && has_pass) g_seen.owners++;
  } else if (!strcmp(kind, "rmode")) {
    wz3_material_view v;
    memset(&v, 0, sizeof v);
    v.system = (uint8_t)ru32(r);
    v.usage = (uint8_t)ru32(r);
    v.material11_combiner = (uint8_t)ru32(r);
    const int mode = wz3_material_render_mode(&v);
    hu(&h, (uint32_t)mode);
    g_seen.modes |= 1u << mode;
  } else if (!strcmp(kind, "state")) {
    wz3_material_view v;
    memset(&v, 0, sizeof v);
    v.base_flags = tou32(rnum(r));
    wz3_material_state s;
    wz3_material_state_make(&v, &s);
    hash_state(&h, &s);
    g_seen.blends |= 1u << ((v.base_flags & 0xf000u) >> 12);
  } else if (!strcmp(kind, "m20xf") || !strcmp(kind, "m20uv")) {
    double p[64];
    const uint32_t n = ru32(r);
    rpairs(r, p, n);
    float t1[8], t2[8], out[2];
    if (!strcmp(kind, "m20xf")) {
      wz3_material20_uv_transforms(p, n, t1, t2);
      hfs(&h, t1, 8);
      hfs(&h, t2, 8);
    } else {
      const int32_t source = ri32(r);
      double uv[2];
      rnums(r, uv, 2);
      wz3_material20_uv(p, n, source, uv, out);
      hfs(&h, out, 2);
    }
  } else if (!strcmp(kind, "xdir")) {
    float m[16], out[3];
    double v[3];
    const int has = (int)ru32(r);
    rmat(r, m);
    rvec3(r, v);
    wz3_transform_direction3(has ? m : NULL, v, out);
    hfs(&h, out, 3);
  } else if (!strcmp(kind, "m20bump")) {
    double n[3], t[3], s[3];
    float model[16], view[16], out[3];
    rvec3(r, n);
    rvec3(r, t);
    rvec3(r, s);
    const int has_model = (int)ru32(r);
    rmat(r, model);
    const int has_view = (int)ru32(r);
    rmat(r, view);
    const int reflection = (int)ru32(r);
    wz3_material20_environment_bump_direction(n, t, s, has_model ? model : NULL, has_view ? view : NULL, reflection, out);
    hfs(&h, out, 3);
  } else if (!strcmp(kind, "m20eye")) {
    float model[16], out[3];
    double position[3], camera[3];
    rmat(r, model);
    rvec3(r, position);
    rvec3(r, camera);
    wz3_material20_environment_eye(model, position, camera, out);
    hfs(&h, out, 3);
  } else if (!strcmp(kind, "m11env")) {
    double n[3];
    float model[16], model_out[3], world_out[3];
    rvec3(r, n);
    const int has_model = (int)ru32(r);
    rmat(r, model);
    const int condition = (int)ru32(r);
    wz3_material11_environment_normals(n, has_model ? model : NULL, condition, model_out, world_out);
    hfs(&h, model_out, 3);
    hfs(&h, world_out, 3);
  } else if (!strcmp(kind, "m20envdir")) {
    double n[3], eye[3];
    float out[3];
    rvec3(r, n);
    rvec3(r, eye);
    const int reflection = (int)ru32(r);
    wz3_material20_environment_direction(n, eye, reflection, out);
    hfs(&h, out, 3);
  } else if (!strcmp(kind, "m20envuv")) {
    double d[3];
    float out[2];
    rvec3(r, d);
    wz3_material20_environment_uv(d, out);
    hfs(&h, out, 2);
  } else if (!strcmp(kind, "m11xf")) {
    double srt1[9], srt2[5];
    float t1[8], t2[8];
    rnums(r, srt1, 9);
    rnums(r, srt2, 5);
    wz3_material11_uv_transforms(srt1, srt2, t1, t2);
    hfs(&h, t1, 8);
    hfs(&h, t2, 8);
  } else if (!strcmp(kind, "m11uv")) {
    wz3_material_view v;
    memset(&v, 0, sizeof v);
    for (int i = 0; i < 4; i++) v.slot_texture_flags[i] = ru32(r);
    v.special_flags = ru32(r);
    rnums(r, v.slot_texture_scales, 4);
    for (int i = 0; i < 8; i++) v.uv_transform1[i] = rf32(r);
    for (int i = 0; i < 8; i++) v.uv_transform2[i] = rf32(r);
    const int32_t slot = ri32(r);
    double storage[7][4];
    wz3_m11_uv_inputs in;
    in.uv0 = roptional(r, storage[0], 2);
    in.uv1 = roptional(r, storage[1], 2);
    in.eye_normal = roptional(r, storage[2], 4);
    in.normal = roptional(r, storage[3], 4);
    in.eye_position = roptional(r, storage[4], 4);
    in.position = roptional(r, storage[5], 4);
    in.world_position = roptional(r, storage[6], 4);
    float out[2];
    wz3_material11_uv(&v, slot, &in, out);
    hfs(&h, out, 2);
  } else if (!strcmp(kind, "m20pre")) {
    wz3_material_view v;
    memset(&v, 0, sizeof v);
    v.detail_ops[0] = (uint8_t)ru32(r);
    v.detail_ops[1] = (uint8_t)ru32(r);
    float storage[3][4], out[4];
    const float *samples[3];
    for (int k = 0; k < 3; k++) {
      samples[k] = NULL;
      if (ru32(r)) {
        for (int i = 0; i < 4; i++) storage[k][i] = rf32(r);
        samples[k] = storage[k];
      }
    }
    wz3_evaluate_material20_prelight(&v, samples[0], samples[1], samples[2], out);
    hfs(&h, out, 4);
  } else if (!strcmp(kind, "m11comb")) {
    wz3_material_view v;
    memset(&v, 0, sizeof v);
    for (int i = 0; i < 13; i++) v.combiners[i] = ru32(r);
    v.alpha_combiner = ru32(r);
    for (int i = 0; i < 4; i++) v.colors[i] = ru32(r);
    float storage[5][4], out[4];
    const float *textures[4], *vertex = NULL;
    for (int k = 0; k < 4; k++) {
      textures[k] = NULL;
      if (ru32(r)) {
        for (int i = 0; i < 4; i++) storage[k][i] = rf32(r);
        textures[k] = storage[k];
      }
    }
    if (ru32(r)) {
      for (int i = 0; i < 4; i++) storage[4][i] = rf32(r);
      vertex = storage[4];
    }
    wz3_evaluate_material11_combiner(&v, textures, vertex, out);
    hfs(&h, out, 4);
  } else {
    return 0xffffffffu;
  }
  return h.h;
}

static uint32_t production_hash(wz3_rt *rt, uint32_t n_ops, uint32_t *count)
{
  hash h;
  hash_init(&h);
  *count = 0;
  for (uint32_t op = 0; op < n_ops; op++) {
    const wz3_material *m = wz3_rt_material(rt, op);
    if (!m) continue;
    for (uint32_t i = 0; i <= m->n_passes; i++) {
      const wz3_material_pass *pass = i < m->n_passes ? &m->passes[i] : NULL;
      const wz3_material *owner = pass ? wz3_rt_material(rt, pass->owner) : NULL;
      wz3_material_view v;
      wz3_material_view_make(m, owner, pass, &v);
      hu(&h, op);
      hash_view_all(&h, &v);
      (*count)++;
    }
  }
  return h.h;
}

int suite_wz3_material_view(int argc, char **argv)
{
  if (argc < 3) {
    printf("usage: test_wz3_material_view <assets dir> <fixtures dir> [--print]\n");
    return 2;
  }
  const int print = argc > 3 && !strcmp(argv[3], "--print");
  size_t kx_size;
  uint8_t *kx_bytes = gate_read_all(argv[1], "debris_party.kx", &kx_size);
  char *fixture = (char *)gate_read_all(argv[2], "material_view.txt", NULL);
  wz3_kx *kx = NULL;
  size_t at;
  CHECK(wz3_kx_parse(&kx, kx_bytes, kx_size, &at) == WZ3_OK, "parse");
  if (!kx) return 1;
  wz3_rt *rt = NULL;
  unsigned compared = 0, production = 0, per_kind_failures = 0;
  size_t line_number = 0;
  for (char *line = fixture; *line;) {
    char *nl = strchr(line, '\n');
    if (nl) *nl = 0;
    line_number++;
    char kind[32];
    reader r;
    r.p = line;
    if (sscanf(line, "%31s", kind) != 1) break;
    r.p = line + strlen(kind);
    const char *eq = strstr(line, " = ");
    if (!eq) {
      CHECK(0, "line %zu has no result", line_number);
      break;
    }
    const uint32_t expected = (uint32_t)strtoul(eq + 3, NULL, 16);
    uint32_t got;
    if (!strcmp(kind, "P")) {
      char label[32];
      unsigned expected_count;
      if (sscanf(line, "P %31s %u", label, &expected_count) != 2) {
        CHECK(0, "line %zu: bad production line", line_number);
        break;
      }
      if (!rt) {
        wz3_rt_options opt;
        memset(&opt, 0, sizeof opt);
        opt.generic_exec = 1;
        opt.frame_jobs = 1;
        opt.geometry = WZ3_GEOMETRY_PLACEHOLDER;
        wz3_result res;
        rt = wz3_rt_create(kx, &opt, &res);
        CHECK(rt && res == WZ3_OK, "runtime create");
        if (!rt) break;
        wz3_rt_set_aspect(rt, 2.0);
        res = wz3_rt_precalc(rt);
        CHECK(res == WZ3_OK, "precalc: %s", wz3_result_string(res));
      }
      if (strcmp(label, "precalc")) {
        const wz3_result res = wz3_rt_frame_at_sample(rt, (int32_t)atol(label), 44100);
        CHECK(res == WZ3_OK, "frame %s: %s", label, wz3_result_string(res));
      }
      uint32_t count;
      got = production_hash(rt, kx->n_ops, &count);
      CHECK(count == expected_count, "production %s: %u views, expected %u", label, count, expected_count);
      production++;
    } else {
      got = run_case(kind, &r);
    }
    compared++;
    if (print)
      printf("%s = %x\n", kind, got);
    else if (got != expected) {
      per_kind_failures++;
      CHECK(0, "line %zu (%s) hash %x, JavaScript %x", line_number, kind, got, expected);
    }
    if (!nl) break;
    line = nl + 1;
  }
  wz3_rt_destroy(rt);
  wz3_kx_destroy(kx);
  free(kx_bytes);
  free(fixture);
  CHECK(compared >= 6500 && production >= 9, "%u lines, %u production", compared, production);
  /* the generator must reach every branch it is meant to compare */
  CHECK(g_seen.modes == 0xffu, "render modes seen: %x", g_seen.modes);
  CHECK(g_seen.depth_funcs == 0xfu && g_seen.culls == 7u, "depth %x cull %x", g_seen.depth_funcs, g_seen.culls);
  CHECK((g_seen.blends & 0x3feu) == 0x3feu, "blend factors seen: %x", g_seen.blends);
  CHECK(g_seen.systems == 3u && g_seen.generated == 7u && g_seen.vcm == 0x1fu, "systems %x generated %x vertex modes %x", g_seen.systems, g_seen.generated,
        g_seen.vcm);
  CHECK(g_seen.multipass > 500 && g_seen.owners > 200 && g_seen.defaults > 20 && g_seen.stencil > 100 && g_seen.combiner_views > 500,
        "coverage: multipass %u owners %u defaults %u stencil %u combiner %u", g_seen.multipass, g_seen.owners, g_seen.defaults, g_seen.stencil,
        g_seen.combiner_views);
  CHECK(g_seen.bind_modes == 0xffu && g_seen.clamped > 1000 && g_seen.sampled > 10000, "sampler slots: filter modes %x, clamped %u, textured %u",
        g_seen.bind_modes, g_seen.clamped, g_seen.sampled);
  printf("material view: %u lines compared (%u production), %u mismatches, %d checks, %d failures\n", compared, production, per_kind_failures, g_checks,
         g_failures);
  return g_failures ? 1 : 0;
}
