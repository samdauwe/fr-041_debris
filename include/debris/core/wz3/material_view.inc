/* ======================================================================== */
/*  Material views (genmaterial.cpp, engine.cpp: render mode,               */
/*  material state and the Material 1.1 / 2.0 helpers)                      */
/*                                                                          */
/*  Pure functions of a material record and a pass. Every statement follows */
/*  the JavaScript port: parameters that are missing read as NaN (JS        */
/*  `undefined`), `x >>> 0` is the ToUint32 of wz3__rt_to_int32, `x || y`   */
/*  is wz3__or_default and a Float32Array store is a (float) cast.          */
/* ======================================================================== */

#define WZ3__MV_ALPHA_REFERENCE (128.0 / 255.0)

static inline double wz3__mv_p(const double *p, uint32_t n, uint32_t i) { return (p && i < n) ? p[i] : (double)NAN; }
/* the JavaScript array has an entry at i (slice() of a short array leaves it out) */
static inline uint32_t wz3__mv_has(const double *p, uint32_t n, uint32_t i) { return (p && i < n) ? 1u : 0u; }
static inline uint32_t wz3__mv_u(const double *p, uint32_t n, uint32_t i) { return (uint32_t)wz3__rt_to_int32(wz3__mv_p(p, n, i)); }
/* Number(value) when finite, else `fallback` */
static inline double wz3__mv_fin(double v, double fallback) { return wz3__finite(v) ? v : fallback; }

/* the sMaterial11 DefaultMat of the player build (genoverlay.cpp): sMBF_ZON, sMLF_BUMPX, mip-mapped textures, LIGHT SET / COLOR0 MUL */
static const double wz3__mv_default_params[64] = {
  [0] = 0x0300, [2] = 0x0005, [4] = 0x0002, [5] = 0x0002, [6] = 0x0002, [7] = 0x0002, [8] = 0x0010, [9] = 0x0040,
  [28] = 0x00c0c0c0, [29] = 0x00404040, [30] = 4294967295.0, [31] = 4294967295.0, [32] = 32, [51] = 32, [52] = 0x00c0c0c0,
};

/* ---- B2: UV transforms and environment helpers ---- */

static void wz3__mv_rows(const float matrix[16], float rows[8])
{
  rows[0] = matrix[0]; rows[1] = matrix[4]; rows[2] = matrix[8]; rows[3] = matrix[12];
  rows[4] = matrix[1]; rows[5] = matrix[5]; rows[6] = matrix[9]; rows[7] = matrix[13];
}

static void wz3__mv_second_rows(double sx, double sy, double angle, double tx, double ty, float rows[8])
{
  const double sine = wz3__sin(angle), cosine = wz3__cos(angle);
  rows[0] = (float)(sx * cosine); rows[1] = (float)(sy * sine); rows[2] = 0.0f; rows[3] = (float)tx;
  rows[4] = (float)(-sx * sine); rows[5] = (float)(sy * cosine); rows[6] = 0.0f; rows[7] = (float)ty;
}

WZ3_API void wz3_material20_uv_transforms(const double *params, uint32_t n, float transform1[8], float transform2[8])
{
  float srt1[9], matrix[16];
  for (uint32_t i = 0; i < 9; i++) {
    const double v = wz3__mv_p(params, n, 24 + i);
    srt1[i] = (float)(wz3__finite(v) ? v : (i < 3 ? 1.0 : 0.0));
  }
  wz3_mat4_srt(srt1, matrix);
  wz3__mv_rows(matrix, transform1);
  const double sx = wz3__mv_fin(wz3__mv_p(params, n, 33), 1.0), sy = wz3__mv_fin(wz3__mv_p(params, n, 34), 1.0);
  const double angle = wz3__or_default(wz3__mv_p(params, n, 35), 0.0);
  const double tx = wz3__or_default(wz3__mv_p(params, n, 36), 0.0), ty = wz3__or_default(wz3__mv_p(params, n, 37), 0.0);
  wz3__mv_second_rows(sx, sy, angle, tx, ty, transform2);
}

WZ3_API void wz3_material20_uv(const double *params, uint32_t n, int32_t source_index, const double uv[2], float out[2])
{
  const int64_t index = source_index;
  const int lighting = index >= 4;
  const int64_t slot = lighting ? index - 4 : index;
  /* a negative slot still indexes the array (8 + slot): only a negative result is `undefined` */
  const int64_t flag_index = (lighting ? 16 : 8) + slot, scale_index = (lighting ? 20 : 12) + slot;
  const uint32_t flags = flag_index >= 0 ? wz3__mv_u(params, n, (uint32_t)flag_index) : 0u;
  const double scale = scale_index >= 0 ? wz3__mv_p(params, n, (uint32_t)scale_index) : (double)NAN;
  const uint32_t mode = (flags >> 12) & 15u;
  const double x = wz3__or_default(uv[0], 0.0), y = wz3__or_default(uv[1], 0.0);
  if (mode == 1) {
    const double value = wz3__finite(scale) ? scale : 1.0;
    out[0] = (float)(x * value); out[1] = (float)(y * value);
  } else if (mode == 2 || mode == 3) {
    float t1[8], t2[8];
    wz3_material20_uv_transforms(params, n, t1, t2);
    const float *rows = mode == 2 ? t1 : t2;
    out[0] = (float)(x * (double)rows[0] + y * (double)rows[1] + (double)rows[3]);
    out[1] = (float)(x * (double)rows[4] + y * (double)rows[5] + (double)rows[7]);
  } else {
    out[0] = (float)x; out[1] = (float)y;
  }
}

WZ3_API void wz3_transform_direction3(const float *matrix, const double vector[3], float out[3])
{
  const double x = wz3__or_default(vector[0], 0.0), y = wz3__or_default(vector[1], 0.0), z = wz3__or_default(vector[2], 0.0);
  if (!matrix) {
    out[0] = (float)x; out[1] = (float)y; out[2] = (float)z;
  } else {
    out[0] = (float)((double)matrix[0] * x + (double)matrix[4] * y + (double)matrix[8] * z);
    out[1] = (float)((double)matrix[1] * x + (double)matrix[5] * y + (double)matrix[9] * z);
    out[2] = (float)((double)matrix[2] * x + (double)matrix[6] * y + (double)matrix[10] * z);
  }
}

static void wz3__mv_vec3(const double *v, double out[3])
{
  for (int i = 0; i < 3; i++) out[i] = v ? wz3__or_default(v[i], 0.0) : 0.0;
}

WZ3_API void wz3_material20_environment_bump_direction(const double normal[3], const double tangent[3], const double sample[3],
                                                       const float *model, const float *view, int reflection, float out[3])
{
  double n[3], t[3], s[3];
  wz3__mv_vec3(normal, n); wz3__mv_vec3(tangent, t); wz3__mv_vec3(sample, s);
  const double b[3] = {n[1] * t[2] - n[2] * t[1], n[2] * t[0] - n[0] * t[2], n[0] * t[1] - n[1] * t[0]};
  float tn[3], tb[3], tt[3];
  wz3_transform_direction3(model, n, tn);
  wz3_transform_direction3(model, b, tb);
  wz3_transform_direction3(model, t, tt);
  float result[3];
  for (int i = 0; i < 3; i++) result[i] = (float)((double)tn[i] * s[0] + (double)tb[i] * s[1] + (double)tt[i] * s[2]);
  if (!reflection && view) {
    const double r[3] = {result[0], result[1], result[2]};
    wz3_transform_direction3(view, r, out);
  } else {
    out[0] = result[0]; out[1] = result[1]; out[2] = result[2];
  }
}

WZ3_API void wz3_material20_environment_eye(const float model[16], const double model_position[3], const double camera_world[3], float out[3])
{
  float camera_model[3];
  wz3_legacy_transr_vector(model, camera_world, 1, camera_model);
  const double mp[3] = {model_position ? wz3__or_default(model_position[0], 0.0) : 0.0,
                        model_position ? wz3__or_default(model_position[1], 0.0) : 0.0,
                        model_position ? wz3__or_default(model_position[2], 0.0) : 0.0};
  const double v[3] = {(double)camera_model[0] - mp[0], (double)camera_model[1] - mp[1], (double)camera_model[2] - mp[2]};
  wz3_transform_direction3(model, v, out);
}

WZ3_API void wz3_material11_environment_normals(const double normal[3], const float *model, int condition_basis, float model_out[3], float world_out[3])
{
  double n[3];
  wz3__mv_vec3(normal, n);
  if (condition_basis) {
    static const double fallback[3] = {0, 0, 1};
    double c[3];
    wz3__normalize3(n[0], n[1], n[2], fallback, c);
    n[0] = c[0]; n[1] = c[1]; n[2] = c[2];
  }
  for (int i = 0; i < 3; i++) model_out[i] = (float)n[i];
  wz3_transform_direction3(model, n, world_out);
}

static void wz3__mv_unit(const double *value, double out[3])
{
  double v[3];
  wz3__mv_vec3(value, v);
  const double length = wz3__js_hypot3(v[0], v[1], v[2]);
  if (length > 1e-20) { out[0] = v[0] / length; out[1] = v[1] / length; out[2] = v[2] / length; }
  else { out[0] = 0.0; out[1] = 0.0; out[2] = 1.0; }
}

WZ3_API void wz3_material20_environment_direction(const double normal[3], const double *eye_direction, int reflection, float out[3])
{
  double n[3], eye[3];
  wz3__mv_unit(normal, n);
  if (!reflection) { for (int i = 0; i < 3; i++) out[i] = (float)n[i]; return; }
  wz3__mv_unit(eye_direction, eye);
  const double projection = 2 * (eye[0] * n[0] + eye[1] * n[1] + eye[2] * n[2]);
  for (int i = 0; i < 3; i++) out[i] = (float)(eye[i] - projection * n[i]);
}

WZ3_API void wz3_material20_environment_uv(const double direction[3], float out[2])
{
  double v[3];
  wz3__mv_vec3(direction, v);
  const double length = wz3__js_hypot3(v[0], v[1], v[2]);
  if (!(length > 1e-20)) { out[0] = 0.5f; out[1] = 0.5f; return; }
  out[0] = (float)(v[0] / length * 0.5 + 0.5);
  out[1] = (float)(v[1] / length * -0.5 + 0.5);
}

WZ3_API void wz3_material11_uv_transforms(const double srt1[9], const double srt2[5], float transform1[8], float transform2[8])
{
  float s[9], matrix[16];
  for (int i = 0; i < 9; i++) s[i] = (float)wz3__mv_fin(srt1[i], 0.0);
  wz3_mat4_srt(s, matrix);
  wz3__mv_rows(matrix, transform1);
  const double angle = wz3__mv_fin(srt2[2], 0.0);
  wz3__mv_second_rows(wz3__mv_fin(srt2[0], 0.0), wz3__mv_fin(srt2[1], 0.0), angle, wz3__mv_fin(srt2[3], 0.0), wz3__mv_fin(srt2[4], 0.0), transform2);
}

/* vector4(value, fallback): Number(value?.[k] ?? fallback[k]) || 0 for an array of four numbers */
static void wz3__mv_vector4(const double *value, const double fallback[4], double out[4])
{
  for (int i = 0; i < 4; i++) out[i] = wz3__or_default(value ? value[i] : fallback[i], 0.0);
}

WZ3_API void wz3_material11_uv(const wz3_material_view *view, int32_t slot, const wz3_m11_uv_inputs *in, float out[2])
{
  static const double origin[4] = {0, 0, 0, 1}, forward[4] = {0, 0, 1, 0}, eye_forward[4] = {0, 0, 1, 1};
  slot = slot < 0 ? 0 : slot > 3 ? 3 : slot;
  const uint32_t flags = view->slot_texture_flags[slot];
  const double *uv = ((flags >> 4) & 3u) == 1 ? in->uv1 : in->uv0;
  double source[4], tmp[4];
  {
    /* [uv?.[0], uv?.[1], 0, 1] through vector4: an absent entry takes the fallback (0, 0, 0, 1) */
    const double raw[4] = {uv ? uv[0] : 0.0, uv ? uv[1] : 0.0, 0.0, 1.0};
    wz3__mv_vector4(raw, origin, source);
  }
  if (slot == 2) {
    const uint32_t mode = (view->special_flags >> 4) & 3u;
    double eye_normal[4];
    wz3__mv_vector4(in->eye_normal ? in->eye_normal : in->normal, forward, eye_normal);
    if (mode == 1) {
      source[0] = eye_normal[0] * 0.5 + 0.5;
      source[1] = eye_normal[1] * -0.5 + 0.5;
    } else if (mode == 2) {
      double ep[4];
      wz3__mv_vector4(in->eye_position ? in->eye_position : in->position, eye_forward, ep);
      const double eye_length = wz3__js_hypot3(ep[0], ep[1], ep[2]);
      const double ex = eye_length > 1e-20 ? ep[0] / eye_length : 0.0;
      const double ey = eye_length > 1e-20 ? ep[1] / eye_length : 0.0;
      const double ez = eye_length > 1e-20 ? ep[2] / eye_length : 0.0;
      const double normal_squared = eye_normal[0] * eye_normal[0] + eye_normal[1] * eye_normal[1] + eye_normal[2] * eye_normal[2];
      const double factor = normal_squared > 1e-20 ? 2 * (ex * eye_normal[0] + ey * eye_normal[1] + ez * eye_normal[2]) / normal_squared : 0.0;
      source[0] = (eye_normal[0] * factor - ex) * 0.5 + 0.5;
      source[1] = (eye_normal[1] * factor - ey) * -0.5 + 0.5;
      source[2] = eye_normal[2] * factor - ez;
    } else if (mode == 3) {
      wz3__mv_vector4(in->normal, forward, tmp);
      memcpy(source, tmp, sizeof source);
    }
  } else if (slot == 3) {
    const uint32_t mode = (view->special_flags >> 8) & 7u;
    if (mode == 1) wz3__mv_vector4(in->position, origin, source);
    else if (mode == 2) wz3__mv_vector4(in->world_position ? in->world_position : in->position, origin, source);
    else if (mode == 3) wz3__mv_vector4(in->eye_position ? in->eye_position : in->position, origin, source);
  }
  const uint32_t transform = (flags >> 12) & 3u;
  if (transform == 1) {
    const double value = wz3__mv_fin(view->slot_texture_scales[slot], 0.0);
    out[0] = (float)(source[0] * value); out[1] = (float)(source[1] * value);
  } else if (transform == 2 || transform == 3) {
    const float *rows = transform == 2 ? view->uv_transform1 : view->uv_transform2;
    out[0] = (float)(source[0] * (double)rows[0] + source[1] * (double)rows[1] + source[2] * (double)rows[2] + source[3] * (double)rows[3]);
    out[1] = (float)(source[0] * (double)rows[4] + source[1] * (double)rows[5] + source[2] * (double)rows[6] + source[3] * (double)rows[7]);
  } else {
    out[0] = (float)source[0]; out[1] = (float)source[1];
  }
}

/* ---- B1: materialView ---- */

static void wz3__mv_make20(const wz3_material *owner, const double *sp, uint32_t sn, const double *dp, uint32_t dn, uint8_t usage, wz3_material_view *out)
{
  const uint32_t static_flags = wz3__mv_u(sp, sn, 0), dynamic_flags = wz3__mv_u(dp, dn, 0);
  uint32_t base = 0x0200u | WZ3_MBF_ZEQUAL;
  int8_t map[4] = {0, 1, 2, -1};
  if (usage == WZ3_USAGE_BASE) {
    base = 0x0300u | WZ3_MBF_ZONLY;
    map[0] = 3; map[1] = map[2] = map[3] = -1;
  } else if (usage == WZ3_USAGE_AMBIENT) {
    base |= 0x3000u;
    map[0] = map[1] = map[2] = map[3] = -1;
  } else if (usage == WZ3_USAGE_SHADOW) {
    base = WZ3_MBF_ZREAD | WZ3_MBF_ZONLY | WZ3_MBF_SHADOWMASK;
    map[0] = map[1] = map[2] = map[3] = -1;
  } else if (usage == WZ3_USAGE_LIGHT) {
    base |= WZ3_MBF_STENCILTEST | ((static_flags & 4u) ? 0x5000u : 0x2000u);
    map[0] = -1; map[1] = 4; map[2] = 5; map[3] = -1;
  } else if (usage == WZ3_USAGE_POSTLIGHT) {
    base |= 0x2000u;
    map[0] = -1; map[1] = 4; map[2] = 6; map[3] = -1;
  }
  out->system = 20;
  out->base_flags = base;
  for (int i = 0; i < 4; i++) {
    out->texture_flags[i] = wz3__mv_u(sp, sn, 8 + (uint32_t)i);
    out->texture_scales[i] = wz3__or_default(wz3__mv_p(dp, dn, 12 + (uint32_t)i), 0.0);
    out->light_flags4[i] = wz3__mv_u(sp, sn, 16 + (uint32_t)i);
    out->light_scales[i] = wz3__or_default(wz3__mv_p(dp, dn, 20 + (uint32_t)i), 0.0);
    out->extra_texture_flags[i] = out->light_flags4[i];
    out->texture_map[i] = map[i];
  }
  for (uint32_t i = 0; i < 4; i++) {
    out->texture_scale_present = (uint8_t)(out->texture_scale_present | (wz3__mv_has(dp, dn, 12 + i) << i));
    out->light_scale_present = (uint8_t)(out->light_scale_present | (wz3__mv_has(dp, dn, 20 + i) << i));
  }
  wz3_material20_uv_transforms(dp, dn, out->uv_transform1, out->uv_transform2);
  out->color = usage == WZ3_USAGE_LIGHT ? wz3__mv_u(dp, dn, 1) : 0xffffffffu;
  out->specular = wz3__mv_u(dp, dn, 2);
  out->vertex_color_mode = 0;
  out->specular_power = wz3__or_default(wz3__mv_p(dp, dn, 3), 8.0);
  out->specular_color = wz3__mv_u(dp, dn, 2);
  out->specular_strength = (static_flags & 1u) ? 1.0 : 0.0;
  out->uv_scale = 1.0;
  out->alpha_cutoff = (usage == WZ3_USAGE_BASE && owner && owner->textures[3] >= 0) ? WZ3__MV_ALPHA_REFERENCE : 0.0;
  out->detail_ops[0] = (uint8_t)((wz3__mv_u(sp, sn, 9) >> 16) & 15u);
  out->detail_ops[1] = (uint8_t)((wz3__mv_u(sp, sn, 10) >> 16) & 15u);
  out->use_prelight = usage == WZ3_USAGE_LIGHT;
  out->light_scale = ((dynamic_flags & 0x40u) && usage == WZ3_USAGE_LIGHT) ? 2.0 : 1.0;
  out->flags = static_flags;
  out->runtime_flags = dynamic_flags;
  out->environment_flags = wz3__mv_u(sp, sn, 18);
  out->runtime_environment_flags = wz3__mv_u(dp, dn, 18);
}

static void wz3__mv_make11(const wz3_material *owner, const double *sp, uint32_t sn, const double *dp, uint32_t dn, uint8_t usage, wz3_material_view *out)
{
  const uint32_t multi = wz3__mv_u(sp, sn, 48), dynamic_multi = wz3__mv_u(dp, dn, 48);
  const int multipass = (multi & 0x1eu) != 0;
  uint32_t base = wz3__mv_u(sp, sn, 0);
  uint32_t color = wz3__mv_u(dp, dn, 28);
  int8_t map[4] = {0, 1, 2, 3};
  double uv_scale = wz3__or_default(wz3__mv_p(dp, dn, 24), 1.0);
  uint32_t special = wz3__mv_u(sp, sn, 1), alpha_combiner = wz3__mv_u(sp, sn, 23);
  double srt1[9], srt2[5];
  for (uint32_t i = 0; i < 9; i++) srt1[i] = wz3__mv_p(dp, dn, 34 + i);
  for (uint32_t i = 0; i < 5; i++) srt2[i] = wz3__mv_p(dp, dn, 43 + i);
  double alpha_cutoff = (base & WZ3_MBF_ALPHATEST) ? WZ3__MV_ALPHA_REFERENCE : 0.0;
  uint8_t vertex_color_mode = 0, generated_alpha = WZ3_GENERATED_ALPHA_NONE;
  if (multipass) {
    if (usage == WZ3_USAGE_BASE) {
      base = 0x0300u | 0x020000u | (wz3__mv_u(sp, sn, 0) & WZ3_MBF_DOUBLESIDED);
      color = (multi & 2u) ? wz3__mv_u(sp, sn, 53) : 0x00ffffffu;
      map[0] = (multi & 0x0400u) ? 0 : -1; map[1] = map[2] = map[3] = -1;
      alpha_cutoff = (multi & 0x0400u) ? WZ3__MV_ALPHA_REFERENCE : 0.0;
      generated_alpha = (multi & 0x0400u) ? WZ3_GENERATED_ALPHA_TEXTURE0 : WZ3_GENERATED_ALPHA_ZERO;
      vertex_color_mode = 4;
      special = 0;
      for (int i = 0; i < 9; i++) srt1[i] = 0.0;
      for (int i = 0; i < 5; i++) srt2[i] = 0.0;
    } else if (usage == WZ3_USAGE_LIGHT) {
      base = WZ3_MBF_ZREAD | WZ3_MBF_ZEQUAL | WZ3_MBF_STENCILTEST | ((multi & 0x4000u) ? 0x5000u : 0x2000u) | (wz3__mv_u(sp, sn, 0) & WZ3_MBF_DOUBLESIDED);
      color = wz3__mv_u(dp, dn, 52);
      if (!(dynamic_multi & 0x0200u)) color &= 0x00ffffffu;
      map[0] = -1; map[1] = 4; map[2] = -1; map[3] = -1;
      uv_scale = (dynamic_multi & 0x1400u) ? wz3__or_default(wz3__mv_p(dp, dn, 24), 1.0) : wz3__or_default(wz3__mv_p(dp, dn, 61), 1.0);
      alpha_cutoff = 0.0;
      special = !(multi & 0x0200u) ? 0x40u : ((multi & 0x2000u) ? 0x80u : 0u);
    } else if (usage == WZ3_USAGE_SHADOW) {
      base = WZ3_MBF_ZREAD | WZ3_MBF_ZONLY | WZ3_MBF_SHADOWMASK;
      color = 0;
      map[0] = map[1] = map[2] = map[3] = -1;
      alpha_cutoff = 0.0;
      special = 0;
      for (int i = 0; i < 9; i++) srt1[i] = 0.0;
      for (int i = 0; i < 5; i++) srt2[i] = 0.0;
    } else if (usage == WZ3_USAGE_POSTLIGHT) {
      base = WZ3_MBF_ZREAD | WZ3_MBF_ZEQUAL | 0x4000u;
      alpha_cutoff = 0.0;
      alpha_combiner = 0x0c;
      color = wz3__mv_u(dp, dn, 28);
    } else if (usage == WZ3_USAGE_POSTLIGHT2) {
      base = WZ3_MBF_ZREAD | WZ3_MBF_ZEQUAL | ((multi & 0x8000u) ? 0u : 0x2000u);
      color = wz3__mv_u(dp, dn, 53);
      map[0] = (multi & 0x4000u) ? 0 : -1; map[1] = -1; map[2] = 6; map[3] = -1;
      uv_scale = wz3__or_default(wz3__mv_p(dp, dn, 62), 1.0);
      alpha_cutoff = 0.0;
      special = (multi & 0x0100u) ? 0x10u : 0x20u;
      for (uint32_t i = 0; i < 9; i++) srt1[i] = wz3__mv_p(sp, sn, 34 + i);
      for (uint32_t i = 0; i < 5; i++) srt2[i] = wz3__mv_p(sp, sn, 43 + i);
    }
  } else if (usage == WZ3_USAGE_LIGHT) color = wz3__mv_u(dp, dn, 52);
  else if (usage == WZ3_USAGE_POSTLIGHT2) color = wz3__mv_u(dp, dn, 53);
  if (!multipass) {
    const uint32_t vertex_operation = wz3__mv_u(sp, sn, 19) & 0x00f0u;
    if (vertex_operation == 0x0010u) vertex_color_mode = 3;
    else if (vertex_operation == 0x0020u) vertex_color_mode = 2;
    else if (vertex_operation >= 0x0040u && vertex_operation <= 0x0070u) vertex_color_mode = 1;
  }
  const int dynamic_scales = !multipass || usage == WZ3_USAGE_POSTLIGHT;
  for (uint32_t i = 0; i < 4; i++) {
    out->texture_flags[i] = wz3__mv_u(sp, sn, 4 + i);
    out->extra_texture_flags[i] = wz3__mv_u(sp, sn, 56 + i);
    out->texture_scales[i] = dynamic_scales ? wz3__mv_fin(wz3__mv_p(dp, dn, 24 + i), 0.0) : wz3__mv_fin(wz3__mv_p(sp, sn, 24 + i), 0.0);
    out->extra_texture_scales[i] = wz3__mv_fin(wz3__mv_p(sp, sn, 60 + i), 0.0);
  }
  if (usage == WZ3_USAGE_LIGHT) out->extra_texture_scales[0] = wz3__or_default(wz3__mv_p(dp, dn, 60), 0.0);
  else if (usage == WZ3_USAGE_POSTLIGHT2) out->extra_texture_scales[2] = wz3__or_default(wz3__mv_p(dp, dn, 62), 0.0);
  uint32_t texture_present = 0, extra_present = 0;
  for (uint32_t i = 0; i < 4; i++) {
    texture_present |= (dynamic_scales ? wz3__mv_has(dp, dn, 24 + i) : wz3__mv_has(sp, sn, 24 + i)) << i;
    extra_present |= wz3__mv_has(sp, sn, 60 + i) << i;
  }
  if (usage == WZ3_USAGE_LIGHT) extra_present |= 1u;
  else if (usage == WZ3_USAGE_POSTLIGHT2) extra_present |= 4u;
  for (int i = 0; i < 4; i++) {
    const int source = map[i];
    out->texture_map[i] = map[i];
    if (source < 0 || (source < 4 ? texture_present >> source : extra_present >> (source - 4)) & 1u) out->slot_scale_present |= (uint8_t)(1u << i);
    if (source < 0) { out->slot_texture_flags[i] = 0; out->slot_texture_scales[i] = 0.0; }
    else if (source < 4) { out->slot_texture_flags[i] = out->texture_flags[source]; out->slot_texture_scales[i] = out->texture_scales[source]; }
    else { out->slot_texture_flags[i] = out->extra_texture_flags[source - 4]; out->slot_texture_scales[i] = out->extra_texture_scales[source - 4]; }
  }
  wz3_material11_uv_transforms(srt1, srt2, out->uv_transform1, out->uv_transform2);
  for (uint32_t i = 0; i < 4; i++) out->colors[i] = wz3__mv_u(dp, dn, 28 + i);
  if (multipass && usage == WZ3_USAGE_POSTLIGHT && (dynamic_multi & 0x0400u)) out->colors[3] = 0xff808080u;
  for (uint32_t i = 0; i < 13; i++) out->combiners[i] = wz3__mv_u(sp, sn, 8 + i);
  out->system = 11;
  out->base_flags = base;
  out->special_flags = special;
  out->light_flags = wz3__mv_u(sp, sn, 2);
  out->color = color;
  out->vertex_color_mode = vertex_color_mode;
  out->specular_power = usage == WZ3_USAGE_LIGHT ? wz3__or_default(wz3__mv_p(dp, dn, 51), 8.0) : wz3__or_default(wz3__mv_p(dp, dn, 32), 8.0);
  out->specular_color = color;
  out->specular_strength = multipass ? ((multi & 0x0200u) ? 1.0 : 0.0) : ((wz3__mv_u(sp, sn, 1) & 0x40u) ? 0.0 : 1.0);
  out->uv_scale = uv_scale;
  out->alpha_cutoff = alpha_cutoff;
  out->multi_flags = multi;
  out->material11_combiner = (uint8_t)((!multipass && (wz3__mv_u(sp, sn, 2) & 7u) == 0) || usage == WZ3_USAGE_POSTLIGHT);
  out->generated_base_alpha = generated_alpha;
  out->alpha_combiner = alpha_combiner;
  (void)owner;
}

WZ3_API void wz3_material_view_make(const wz3_material *material, const wz3_material *owner_in, const wz3_material_pass *pass, wz3_material_view *out)
{
  memset(out, 0, sizeof *out);
  const double *dp, *sp;
  uint32_t dn, sn;
  uint8_t usage = pass ? pass->usage : (uint8_t)WZ3_USAGE_OTHER;
  int32_t state = pass ? (int32_t)pass->state : WZ3_VIEW_STATE_NONE;
  const wz3_material *owner = NULL;
  int is20 = 0;
  if (!material) {
    /* Unlinked Mesh/MinMesh cluster 1 is DefaultMat in the player: its sole light pass, whatever pass the caller holds */
    dp = sp = wz3__mv_default_params;
    dn = sn = 64;
    usage = WZ3_USAGE_LIGHT;
    state = WZ3_VIEW_STATE_DEFAULT;
  } else {
    owner = owner_in ? owner_in : material;
    sp = owner->initial ? owner->initial : owner->params;
    sn = owner->initial ? owner->n_params : (owner->params ? owner->n_params : 0);
    dp = material->params ? material->params : owner->params;
    dn = material->params ? material->n_params : owner->n_params;
    is20 = owner->system == 20;
  }
  if (is20) wz3__mv_make20(owner, sp, sn, dp, dn, usage, out);
  else wz3__mv_make11(owner, sp, sn, dp, dn, usage, out);
  out->usage = usage;
  out->state = state;
  out->textures = owner ? owner->textures : NULL;
}

WZ3_API int wz3_material_render_mode(const wz3_material_view *v)
{
  const int is20 = v->system == 20;
  if (is20 && v->usage == WZ3_USAGE_BASE) return 1;
  if (v->usage == WZ3_USAGE_SHADOW) return 1;
  if (v->material11_combiner) return 7;
  if (v->usage == WZ3_USAGE_LIGHT) return 2;
  if (is20 && v->usage == WZ3_USAGE_POSTLIGHT) return 4;
  if (is20 && v->usage == WZ3_USAGE_PRELIGHT) return 6;
  if (v->usage == WZ3_USAGE_POSTLIGHT) return 3;
  if (v->usage == WZ3_USAGE_POSTLIGHT2) return 4;
  if (v->usage == WZ3_USAGE_AMBIENT) return 5;
  return 0;
}

WZ3_API void wz3_material_state_make(const wz3_material_view *view, wz3_material_state *out)
{
  /* materialState(view): blend modes 1..9 of the legacy table, indexed by bits 12..15 of the flags */
  static const uint8_t blend_table[10][3] = {
    {0, 0, 0},
    {WZ3_BLEND_SRC_ALPHA, WZ3_BLEND_ONE_MINUS_SRC_ALPHA, WZ3_BLEND_EQ_ADD},
    {WZ3_BLEND_ONE, WZ3_BLEND_ONE, WZ3_BLEND_EQ_ADD},
    {WZ3_BLEND_ZERO, WZ3_BLEND_SRC_COLOR, WZ3_BLEND_EQ_ADD},
    {WZ3_BLEND_DST_COLOR, WZ3_BLEND_SRC_COLOR, WZ3_BLEND_EQ_ADD},
    {WZ3_BLEND_ONE, WZ3_BLEND_ONE_MINUS_SRC_COLOR, WZ3_BLEND_EQ_ADD},
    {WZ3_BLEND_ONE, WZ3_BLEND_ONE, WZ3_BLEND_EQ_REVERSE_SUBTRACT},
    {WZ3_BLEND_ONE_MINUS_DST_COLOR, WZ3_BLEND_ONE_MINUS_SRC_COLOR, WZ3_BLEND_EQ_ADD},
    {WZ3_BLEND_DST_ALPHA, WZ3_BLEND_ONE, WZ3_BLEND_EQ_ADD},
    {WZ3_BLEND_SRC_ALPHA, WZ3_BLEND_ONE, WZ3_BLEND_EQ_ADD},
  };
  const uint32_t flags = view->base_flags;
  memset(out, 0, sizeof *out);
  out->depth_func = WZ3_DEPTH_ALWAYS;
  if (flags & WZ3_MBF_ZREAD) {
    if (flags & WZ3_MBF_ZEQUAL) out->depth_func = WZ3_DEPTH_EQUAL;
    else if (flags & WZ3_MBF_SHADOWMASK) out->depth_func = WZ3_DEPTH_LESS;
    else out->depth_func = WZ3_DEPTH_LEQUAL;
  }
  const uint32_t blend_mode = (flags & 0xf000u) >> 12;
  out->depth_test = (flags & 0x0300u) != 0;
  out->depth_write = (flags & WZ3_MBF_ZWRITE) != 0;
  out->color_write = !(flags & WZ3_MBF_ZONLY);
  out->cull = (flags & WZ3_MBF_DOUBLESIDED) ? WZ3_CULL_NONE : ((flags & WZ3_MBF_INVERTCULL) ? WZ3_CULL_FRONT : WZ3_CULL_BACK);
  if (blend_mode >= 1 && blend_mode <= 9) {
    out->blend = 1;
    out->blend_src = blend_table[blend_mode][0]; out->blend_dst = blend_table[blend_mode][1]; out->blend_equation = blend_table[blend_mode][2];
  }
  out->stencil_test = (flags & 0x700000u) == WZ3_MBF_STENCILTEST;
}

/* ---- B3: the two colour combiners ---- */

WZ3_API void wz3_evaluate_material20_prelight(const wz3_material_view *view, const float *s0, const float *s1, const float *s2, float out[4])
{
  const float *samples[3] = {s0, s1, s2};
  if (s0) memcpy(out, s0, 4 * sizeof(float));
  else { out[0] = out[1] = out[2] = 1.0f; out[3] = 0.0f; }
  for (int index = 1; index <= 2; index++) {
    if (!samples[index]) continue;
    const float *detail = samples[index];
    const uint8_t operation = view->detail_ops[index - 1];
    for (int channel = 0; channel < 3; channel++) {
      const double current = out[channel], value = detail[channel];
      if (operation == 0) out[channel] = (float)(current * value);
      else if (operation == 1) out[channel] = (float)(current * value * 2);
      else if (operation == 2) out[channel] = (float)(current * value * 4);
      else if (operation == 3) out[channel] = (float)(current + value);
      else if (operation == 4) out[channel] = (float)(current + value - current * value);
    }
  }
}

WZ3_API void wz3_evaluate_material11_combiner(const wz3_material_view *view, const float *const textures[4], const float *vertex, float out[4])
{
  static const float white[4] = {1, 1, 1, 1}, zero[4] = {0, 0, 0, 0};
  float colors[4][4];
  for (int i = 0; i < 4; i++) wz3__color_argb(view->colors[i], colors[i]);
  const float *tex[4];
  for (int i = 0; i < 4; i++) tex[i] = (textures && textures[i]) ? textures[i] : white;
  float current[4] = {0, 0, 0, 0};
  for (int index = 0; index < 13; index++) {
    const uint32_t word = view->combiners[index];
    const uint32_t operation = (word >> 4) & 15u;
    if (!operation) continue;
    const float *source;
    switch (index) {
    case 1: source = colors[0]; break;
    case 2: source = colors[1]; break;
    case 3: source = tex[0]; break;
    case 4: source = tex[1]; break;
    case 7: source = colors[2]; break;
    case 8: source = colors[3]; break;
    case 9: source = tex[2]; break;
    case 10: source = tex[3]; break;
    case 11: source = vertex ? vertex : white; break;
    default: source = zero; break;
    }
    float input[4];
    memcpy(input, source, sizeof input);
    const uint32_t alpha_code = word & 15u;
    double fade;
    if (alpha_code >= 4 && alpha_code <= 7) fade = tex[alpha_code - 4][3];
    else if (alpha_code >= 8 && alpha_code <= 11) fade = colors[alpha_code - 8][3];
    else if (alpha_code == 12) fade = 0.5;
    else if (alpha_code == 13) fade = 0.0;
    else fade = 1.0;
    /* the colour combiner's AlphaSrc never receives the AlphaCombiner inversion bits */
    if (alpha_code && operation != 9) for (int c = 0; c < 3; c++) input[c] = (float)((double)input[c] * fade);
    for (int c = 0; c < 3; c++) {
      const double cur = current[c], in = input[c];
      if (operation == 1) current[c] = (float)in;
      else if (operation == 2) current[c] = (float)(cur + in);
      else if (operation == 3) current[c] = (float)(cur - in);
      else if (operation == 4) current[c] = (float)(cur * in);
      else if (operation >= 5 && operation <= 7) current[c] = (float)(cur * in * (double)(1 << (operation - 4)));
      else if (operation == 8) current[c] = (float)(cur * (1 - in) + in);
      else if (operation == 9) current[c] = (float)(in * (1 - fade) + cur * fade);
      else if (operation == 10) current[c] = (float)(in - cur);
      else if (operation == 12) current[c] = (float)wz3__js_max(0, wz3__js_min(1, cur * in));
    }
    if (operation == 11) {
      const double dot = (double)current[0] * input[0] + (double)current[1] * input[1] + (double)current[2] * input[2];
      current[0] = current[1] = current[2] = (float)dot;
    }
  }
  const uint32_t alpha_word = view->alpha_combiner;
  const uint32_t second_code = (alpha_word >> 4) & 15u;
  double output_alpha;
#define WZ3__MV_ALPHA(code, result)                                                    \
  do {                                                                                 \
    const uint32_t c_ = (code);                                                        \
    if (c_ >= 4 && c_ <= 7) (result) = tex[c_ - 4][3];                                 \
    else if (c_ >= 8 && c_ <= 11) (result) = colors[c_ - 8][3];                        \
    else if (c_ == 12) (result) = 0.5;                                                 \
    else if (c_ == 13) (result) = 0.0;                                                 \
    else (result) = 1.0;                                                               \
  } while (0)
  if (second_code == 14) {
    output_alpha = ((double)tex[0][3] + (double)tex[1][3]) * (double)colors[0][3];
  } else {
    WZ3__MV_ALPHA(alpha_word & 15u, output_alpha);
    if (alpha_word & 0x100u) output_alpha = 1 - output_alpha;
    if (second_code) {
      double second;
      WZ3__MV_ALPHA(second_code, second);
      if (alpha_word & 0x200u) second = 1 - second;
      output_alpha *= second;
    }
  }
#undef WZ3__MV_ALPHA
  current[3] = (float)output_alpha;
  memcpy(out, current, sizeof current);
}
