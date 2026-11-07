/* Render-planning check for wz3.h against src/renderer.js.
 *
 *   test_wz3_render_plan <fixtures dir> [--print]
 *
 * Replays the random differential cases of tools/dump_render_plan.mjs: every line carries its inputs in hex and the
 * hash of the JavaScript result.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

static char *read_all(const char *dir, const char *name)
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
  char *data = (char *)malloc((size_t)n + 1);
  if (!data || fread(data, 1, (size_t)n, f) != (size_t)n) exit(2);
  fclose(f);
  data[n] = 0;
  return data;
}

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

/* token reader over one fixture line */
typedef struct reader {
  const char *p;
} reader;
static uint64_t rtok(reader *r)
{
  while (*r->p == ' ') r->p++;
  char *end;
  const uint64_t v = strtoull(r->p, &end, 16);
  r->p = end;
  return v;
}
static uint32_t ru32(reader *r) { return (uint32_t)rtok(r); }
static float rf32(reader *r)
{
  const uint32_t u = ru32(r);
  float f;
  memcpy(&f, &u, 4);
  return f;
}
static double rf64(reader *r)
{
  const uint64_t u = rtok(r);
  double d;
  memcpy(&d, &u, 8);
  return d;
}
static void rmat(reader *r, float *m)
{
  for (int i = 0; i < 16; i++) m[i] = rf32(r);
}
static void rbounds(reader *r, double min[3], double max[3])
{
  for (int i = 0; i < 3; i++) min[i] = rf64(r);
  for (int i = 0; i < 3; i++) max[i] = rf64(r);
}
static void rlight(reader *r, wz3_light_job *l)
{
  memset(l, 0, sizeof *l);
  l->directional = (uint8_t)ru32(r);
  for (int i = 0; i < 3; i++) l->position[i] = rf32(r);
  l->range = rf64(r);
  l->amplify = rf32(r);
}
static void rcamera(reader *r, wz3_camera *c)
{
  memset(c, 0, sizeof *c);
  rmat(r, c->camera_space);
  c->near_clip = rf64(r);
  c->zoom_x = rf64(r);
  c->zoom_y = rf64(r);
  c->center_x = rf64(r);
  c->center_y = rf64(r);
}

static void hash_planes(hash *h, const double (*planes)[4], uint32_t n, int ok)
{
  hu(h, ok ? n : 0xffffffffu);
  if (ok)
    for (uint32_t i = 0; i < n; i++)
      for (int k = 0; k < 4; k++) hd(h, planes[i][k]);
}
static void hash_bounds(hash *h, int ok, const wz3_bounds *b)
{
  hu(h, ok ? 1u : 0u);
  if (ok) {
    for (int i = 0; i < 3; i++) hd(h, b->min[i]);
    for (int i = 0; i < 3; i++) hd(h, b->max[i]);
  }
}

/* returns the hash of the C result for one line, or 0xffffffff when the kind is unknown */
static uint32_t run_case(const char *kind, reader *r)
{
  hash h;
  hash_init(&h);
  if (!strcmp(kind, "inverse")) {
    float m[16], out[16];
    rmat(r, m);
    const int ok = wz3_mat4_inverse(m, out);
    hu(&h, ok ? 1u : 0u);
    if (ok) hfs(&h, out, 16);
  } else if (!strcmp(kind, "camera")) {
    wz3_camera c;
    wz3_camera_mats cm;
    rcamera(r, &c);
    wz3_camera_matrices(&c, &cm);
    hfs(&h, cm.view, 16);
    hfs(&h, cm.projection, 16);
    hfs(&h, cm.view_projection, 16);
  } else if (!strcmp(kind, "compose")) {
    const int instanced = (int)ru32(r);
    const uint32_t n = ru32(r);
    float base[16];
    rmat(r, base);
    float *instances = (float *)malloc(((size_t)n + 1) * 16 * sizeof(float));
    float *out = (float *)malloc(((size_t)n + 1) * 16 * sizeof(float));
    for (uint32_t i = 0; i < n; i++) rmat(r, instances + (size_t)i * 16);
    const uint32_t count = wz3_compose_instances(base, instanced, instances, n, out);
    hu(&h, count);
    hfs(&h, out, (size_t)count * 16);
    free(instances);
    free(out);
  } else if (!strcmp(kind, "tbounds")) {
    double mn[3], mx[3];
    float m[16];
    wz3_bounds b;
    rbounds(r, mn, mx);
    rmat(r, m);
    const int ok = wz3_transform_bounds(mn, mx, m, &b);
    hash_bounds(&h, ok, &b);
  } else if (!strcmp(kind, "jbounds")) {
    double mn[3], mx[3];
    wz3_bounds b;
    rbounds(r, mn, mx);
    const uint32_t n = ru32(r);
    float *ms = (float *)malloc(((size_t)n + 1) * 16 * sizeof(float));
    for (uint32_t i = 0; i < n; i++) rmat(r, ms + (size_t)i * 16);
    const int ok = wz3_job_world_bounds(mn, mx, ms, n, &b);
    hash_bounds(&h, ok, &b);
    free(ms);
  } else if (!strcmp(kind, "sphere")) {
    const int has = (int)ru32(r);
    wz3_bounds b;
    rbounds(r, b.min, b.max);
    double center[3];
    for (int i = 0; i < 3; i++) center[i] = rf64(r);
    const double radius = rf64(r);
    wz3_light_job l;
    rlight(r, &l);
    hu(&h, wz3_bounds_intersect_sphere(has ? &b : NULL, center, radius) ? 1u : 0u);
    hu(&h, wz3_light_intersects_bounds(&l, has ? &b : NULL) ? 1u : 0u);
  } else if (!strcmp(kind, "frustum")) {
    float vp[16];
    double planes[5][4];
    rmat(r, vp);
    wz3_light_job l;
    rlight(r, &l);
    const uint32_t n = wz3_view_frustum_planes(vp, planes);
    hash_planes(&h, (const double (*)[4])planes, n, n != 0);
    hu(&h, wz3_light_intersects_frustum(&l, (const double (*)[4])planes, n) ? 1u : 0u);
    double sp[5][4];
    const uint32_t ns = wz3_shadow_view_frustum_planes(vp, l.position, sp);
    hash_planes(&h, (const double (*)[4])sp, ns, ns != 0);
  } else if (!strcmp(kind, "planetest")) {
    double plane[1][4];
    wz3_bounds b;
    for (int i = 0; i < 4; i++) plane[0][i] = rf64(r);
    rbounds(r, b.min, b.max);
    hu(&h, wz3_caster_uses_zfail(&b, (const double (*)[4])plane, 1) ? 1u : 0u);
    hu(&h, wz3_bounds_outside_planes(&b, (const double (*)[4])plane, 1) ? 1u : 0u);
  } else if (!strcmp(kind, "spherebounds")) {
    wz3_light_job l;
    float view[16];
    memset(&l, 0, sizeof l);
    for (int i = 0; i < 3; i++) l.position[i] = rf32(r);
    l.range = rf64(r);
    rmat(r, view);
    const double zx = rf64(r), zy = rf64(r);
    double rect[4];
    const int ok = wz3_light_sphere_bounds(l.position, l.range, view, zx, zy, rect);
    hu(&h, ok ? 1u : 0u);
    if (ok)
      for (int i = 0; i < 4; i++) hd(&h, rect[i]);
  } else if (!strcmp(kind, "casters")) {
    const int use_view = (int)ru32(r);
    wz3_camera c;
    wz3_light_job l;
    wz3_bounds b;
    rcamera(r, &c);
    rlight(r, &l);
    rbounds(r, b.min, b.max);
    wz3_camera_mats cm;
    wz3_camera_matrices(&c, &cm);
    float view[16];
    const int have_view = use_view && wz3_mat4_inverse(c.camera_space, view);
    double planes[6][4];
    const uint32_t n = wz3_shadow_zfail_planes(&c, &l, have_view ? view : NULL, NULL, planes);
    hash_planes(&h, (const double (*)[4])planes, n, n != 0);
    hu(&h, wz3_caster_uses_zfail(&b, (const double (*)[4])planes, n) ? 1u : 0u);
    hu(&h, wz3_caster_uses_zfail(NULL, (const double (*)[4])planes, n) ? 1u : 0u);
    double sp[5][4];
    const uint32_t ns = wz3_shadow_view_frustum_planes(cm.view_projection, l.position, sp);
    hu(&h, wz3_bounds_outside_planes(&b, (const double (*)[4])sp, ns) ? 1u : 0u);
    hu(&h, wz3_caster_may_affect_view(&l, &b, cm.view_projection) ? 1u : 0u);
  } else if (!strcmp(kind, "lights")) {
    const uint32_t limit = ru32(r), n = ru32(r);
    wz3_ipp_node node;
    memset(&node, 0, sizeof node);
    rcamera(r, &node.camera);
    wz3_light_job *lights = (wz3_light_job *)calloc((size_t)n + 1, sizeof *lights);
    for (uint32_t i = 0; i < n; i++) rlight(r, &lights[i]);
    wz3_frame frame;
    memset(&frame, 0, sizeof frame);
    frame.light_jobs = lights;
    node.first_light = 0;
    node.n_light = n;
    uint32_t chosen[64];
    const uint32_t count = wz3_select_active_lights(&frame, &node, limit, chosen);
    hu(&h, count);
    for (uint32_t i = 0; i < count; i++) hi(&h, (int32_t)chosen[i]);
    free(lights);
  } else if (!strcmp(kind, "fit")) {
    const int32_t w = (int32_t)ru32(r), hh = (int32_t)ru32(r);
    const double aspect = rf64(r);
    wz3_region g;
    wz3_fit_aspect_region(w, hh, aspect, &g);
    hi(&h, g.x);
    hi(&h, g.y);
    hi(&h, g.width);
    hi(&h, g.height);
  } else if (!strcmp(kind, "tbn")) {
    double n[3], t[3], m[3], out[3];
    for (int i = 0; i < 3; i++) n[i] = rf64(r);
    for (int i = 0; i < 3; i++) t[i] = rf64(r);
    for (int i = 0; i < 3; i++) m[i] = rf64(r);
    wz3_tangent_basis_normal(n, t, m, out);
    for (int i = 0; i < 3; i++) hd(&h, out[i]);
  } else if (!strcmp(kind, "atten")) {
    const double d = rf64(r), power = rf64(r);
    hd(&h, wz3_material_light_attenuation(d));
    hd(&h, wz3_material11_specular(d, power));
  } else if (!strcmp(kind, "transr")) {
    float m[16], out[3];
    double v[3];
    rmat(r, m);
    for (int i = 0; i < 3; i++) v[i] = rf64(r);
    const int point = (int)ru32(r);
    wz3_legacy_transr_vector(m, v, point, out);
    hfs(&h, out, 3);
  } else if (!strcmp(kind, "vlight")) {
    double p[3], n[3], t[3], light[4], att[4], cam[3];
    for (int i = 0; i < 3; i++) p[i] = rf64(r);
    for (int i = 0; i < 3; i++) n[i] = rf64(r);
    for (int i = 0; i < 3; i++) t[i] = rf64(r);
    for (int i = 0; i < 4; i++) light[i] = rf64(r);
    for (int i = 0; i < 4; i++) att[i] = rf64(r);
    for (int i = 0; i < 3; i++) cam[i] = rf64(r);
    const int cond = (int)ru32(r);
    wz3_vertex_lighting o;
    wz3_legacy_vertex_lighting(p, n, t, light, att, cam, cond, &o);
    hfs(&h, o.normal, 3);
    hfs(&h, o.tangent, 3);
    hfs(&h, o.bitangent, 3);
    hfs(&h, o.light, 3);
    hfs(&h, o.halfway, 3);
    hfs(&h, o.attenuation, 3);
  } else if (!strcmp(kind, "lconst")) {
    const uint32_t m = ru32(r), l = ru32(r);
    const double amplify = rf64(r);
    float out[4];
    wz3_material11_light_constant(m, l, amplify, out);
    hfs(&h, out, 4);
  } else if (!strcmp(kind, "cc")) {
    double p[12], src[4];
    for (int i = 0; i < 12; i++) p[i] = rf64(r);
    for (int i = 0; i < 4; i++) src[i] = rf64(r);
    float c[28], e[4];
    wz3_color_correction_constants(p, 12, c);
    wz3_evaluate_color_correction(p, 12, src, e);
    hfs(&h, c, 28);
    hfs(&h, e, 4);
  } else if (!strcmp(kind, "glare")) {
    double p[12], src[4], g[4], o[4];
    for (int i = 0; i < 12; i++) p[i] = rf64(r);
    for (int i = 0; i < 4; i++) src[i] = rf64(r);
    for (int i = 0; i < 4; i++) g[i] = rf64(r);
    for (int i = 0; i < 4; i++) o[i] = rf64(r);
    wz3_glare_plan plan;
    float tone[4], comp[4];
    wz3_glare_plan_make(p, 12, &plan);
    wz3_evaluate_glare_tone(p, 12, src, tone);
    wz3_evaluate_glare_composite(p, 12, g, o, comp);
    hi(&h, plan.flags);
    hu(&h, plan.downsample[0]);
    hu(&h, plan.downsample[1]);
    hu(&h, plan.copy_downsample);
    hfs(&h, plan.threshold, 4);
    hf(&h, plan.gray_scale);
    hd(&h, plan.gray_mix);
    for (int s = 0; s < 2; s++) {
      hd(&h, plan.blur[s]);
      hd(&h, plan.amplify[s]);
    }
    hfs(&h, plan.glare_color, 4);
    hfs(&h, plan.original_color, 4);
    hd(&h, plan.add_smooth);
    hfs(&h, tone, 4);
    hfs(&h, comp, 4);
  } else if (!strcmp(kind, "ribbon")) {
    const uint32_t count = ru32(r);
    float *points = (float *)malloc(((size_t)count + 1) * 3 * sizeof(float));
    for (uint32_t i = 0; i < count * 3; i++) points[i] = rf32(r);
    const double thickness = rf64(r);
    const int32_t ripped = (int32_t)ru32(r);
    const int has_view = (int)ru32(r);
    float view[16];
    if (has_view) rmat(r, view);
    float *positions = (float *)malloc(((size_t)count + 1) * 6 * sizeof(float));
    float *uvs = (float *)malloc(((size_t)count + 1) * 4 * sizeof(float));
    uint16_t *indices = (uint16_t *)malloc(((size_t)count + 1) * 6 * sizeof(uint16_t));
    const uint32_t n = wz3_build_chain_ribbon(points, count, thickness, ripped, has_view ? view : NULL, positions, uvs, indices);
    hfs(&h, positions, (size_t)count * 6);
    hfs(&h, uvs, (size_t)count * 4);
    hu(&h, n);
    for (uint32_t i = 0; i < n; i++) hu(&h, indices[i]);
    free(points);
    free(positions);
    free(uvs);
    free(indices);
  } else if (!strcmp(kind, "scissor")) {
    const int has = (int)ru32(r);
    double rect[4];
    for (int i = 0; i < 4; i++) rect[i] = rf64(r);
    wz3_region vp, out;
    memset(&vp, 0, sizeof vp);
    vp.x = (int32_t)ru32(r);
    vp.y = (int32_t)ru32(r);
    vp.width = (int32_t)ru32(r);
    vp.height = (int32_t)ru32(r);
    wz3_light_scissor_region(has ? rect : NULL, &vp, &out);
    hi(&h, out.x);
    hi(&h, out.y);
    hi(&h, out.width);
    hi(&h, out.height);
  } else if (!strcmp(kind, "quad")) {
    wz3_ipp_node layer;
    memset(&layer, 0, sizeof layer);
    for (int i = 0; i < 4; i++) layer.screen[i] = rf64(r);
    for (int i = 0; i < 4; i++) layer.uv[i] = rf64(r);
    layer.z = rf32(r);
    float positions[12], uvs[8];
    wz3_layer_quad_geometry(&layer, positions, uvs);
    hfs(&h, positions, 12);
    hfs(&h, uvs, 8);
  } else if (!strcmp(kind, "sort")) {
    const uint32_t n = ru32(r);
    wz3_render_item *items = (wz3_render_item *)calloc((size_t)n + 1, sizeof *items);
    for (uint32_t i = 0; i < n; i++) {
      items[i].render_pass = (int32_t)ru32(r);
      items[i].usage = (uint8_t)ru32(r);
      items[i].is_effect = (uint8_t)ru32(r);
      items[i].light_index = (int32_t)ru32(r);
      items[i].sort_material = (int32_t)ru32(r);
      items[i].sort_order = (int32_t)ru32(r);
      items[i].sequence = i; /* identifies the item in the output */
    }
    wz3_sort_render_items(items, n);
    hu(&h, n);
    for (uint32_t i = 0; i < n; i++) hu(&h, items[i].sequence); /* sequence is the original index */
    free(items);
  } else {
    return 0xffffffffu;
  }
  return h.h;
}

int suite_wz3_render_plan(int argc, char **argv)
{
  if (argc < 2) {
    printf("usage: test_wz3_render_plan <fixtures dir> [--print]\n");
    return 2;
  }
  const int print = argc > 2 && !strcmp(argv[2], "--print");
  char *text = read_all(argv[1], "render_plan.txt");
  size_t cases = 0, bad = 0;
  size_t per_kind[40] = { 0 };
  char kinds[40][16];
  size_t n_kinds = 0;
  char *p = text;
  while (*p) {
    char *nl = strchr(p, '\n');
    if (nl) *nl = 0;
    if (*p) {
      char kind[32];
      const char *rest = strchr(p, ' ');
      const size_t klen = rest ? (size_t)(rest - p) : strlen(p);
      snprintf(kind, sizeof kind, "%.*s", (int)klen, p);
      const char *eq = strstr(p, " = ");
      reader r = { rest ? rest : "" };
      uint32_t got;
      if (!strcmp(kind, "vregion")) {
        /* vregion: width height crop(4 floats) -> region hash; the scissor line that follows is separate */
        const int32_t w = (int32_t)ru32(&r), hh = (int32_t)ru32(&r);
        float crop[4];
        for (int i = 0; i < 4; i++) crop[i] = rf32(&r);
        wz3_region g;
        wz3_viewport_region(crop, w, hh, &g);
        hash h;
        hash_init(&h);
        hi(&h, g.x);
        hi(&h, g.y);
        hi(&h, g.width);
        hi(&h, g.height);
        hfs(&h, g.uv, 4);
        got = h.h;
      } else {
        got = run_case(kind, &r);
      }
      const unsigned long want = eq ? strtoul(eq + 3, NULL, 16) : 0;
      if (print)
        printf("%s = %x\n", kind, got);
      else {
        cases++;
        if (got != (uint32_t)want) {
          bad++;
          if (bad <= 5) printf("%s differs: JS %lx, C %x\n  %.200s\n", kind, want, got, p);
        }
        size_t k = 0;
        while (k < n_kinds && strcmp(kinds[k], kind)) k++;
        if (k == n_kinds && n_kinds < 40) {
          snprintf(kinds[n_kinds], 16, "%.15s", kind);
          n_kinds++;
        }
        per_kind[k]++;
      }
    }
    if (!nl) break;
    p = nl + 1;
  }
  if (!print) {
    CHECK(bad == 0, "%zu of %zu cases differ from the JS port", bad, cases);
    CHECK(n_kinds >= 24 && cases >= 9000, "only %zu kinds, %zu cases", n_kinds, cases);
    printf("render plan: %zu cases in %zu groups, %zu differ\n", cases, n_kinds, bad);
    for (size_t k = 0; k < n_kinds; k++) printf("  %-13s %zu\n", kinds[k], per_kind[k]);
  }
  free(text);
  printf("render plan: %d checks, %d failures\n", g_checks, g_failures);
  return g_failures ? 1 : 0;
}
