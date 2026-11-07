/* ======================================================================== */
/*  runtime                                                                 */
/*                                                                          */
/*  Port of kdoc.cpp and genblobspline.cpp. The JavaScript arithmetic is binary64 with an   */
/*  explicit f32() at every point the x87 player stores a float; each       */
/*  expression below keeps that structure (wz3__rt_*f take and return       */
/*  float-valued doubles, plain double expressions stay unrounded).         */
/* ======================================================================== */

static inline float wz3__rt_addf(double a, double b) { return (float)(a + b); }
static inline float wz3__rt_subf(double a, double b) { return (float)(a - b); }
static inline float wz3__rt_mulf(double a, double b) { return (float)(a * b); }
static inline float wz3__rt_divf(double a, double b) { return (float)(a / b); }

static inline int32_t wz3__rt_round_even(double v)
{
  if (!(v == v) || v < -2147483648.0 || v >= 2147483648.0) return INT32_MIN;
  int64_t fi = (int64_t)v;
  if ((double)fi > v) fi -= 1;
  const double fraction = v - (double)fi;
  if (fraction < 0.5) return (int32_t)(uint32_t)(uint64_t)fi;
  if (fraction > 0.5) return (int32_t)(uint32_t)(uint64_t)(fi + 1);
  return (int32_t)(uint32_t)(uint64_t)((fi & 1) ? fi + 1 : fi);
}
static inline int32_t wz3__rt_trunc_int(double v)
{
  if (!(v == v) || v < -2147483648.0 || v >= 2147483648.0) return INT32_MIN;
  return (int32_t)v;
}
/* JavaScript ToInt32 for the values that reach `|`/`&` on animated parameters */
static inline int32_t wz3__rt_to_int32(double v)
{
  if (!(v == v) || v > 1.0e18 || v < -1.0e18) return 0;
  return (int32_t)(uint32_t)(uint64_t)(int64_t)v;
}
static inline double wz3__rt_clamp(double value, double maximum, double minimum)
{
  return value >= maximum ? maximum : value <= minimum ? minimum : value;
}

#define WZ3__RT_MAX_VARS 32
#define WZ3__RT_MAX_SAVES 256
#define WZ3__RT_KV_MATRIX_I 0x1c
#define WZ3__RT_KEF_NOTIME 1
#define WZ3__RT_TWO_PI ((double)(float)6.283185307179586)
#define WZ3__RT_PI_F ((double)(float)3.141592653589793)

enum { WZ3__RC_NONE = 0, WZ3__RC_MISSING, WZ3__RC_DEMO, WZ3__RC_BLOB, WZ3__RC_SHAKER, WZ3__RC_MESH, WZ3__RC_MINMESH,
       WZ3__RC_MATERIAL, WZ3__RC_SCENE, WZ3__RC_IPP, WZ3__RC_EFFECT };

typedef struct wz3__rt_blob_key {
  int32_t select;
  float time;
  float f[7];                      /* px py pz rx ry rz zoom */
} wz3__rt_blob_key;

typedef struct wz3__rt_pipe_key { float x, y, z, radius; } wz3__rt_pipe_key;   /* a PipeSpline corner */

typedef struct wz3__rt_blob {
  int32_t version, select, mode;
  float target[4];
  float continuity, tension;
  int32_t uniform;
  uint32_t n_keys;
  wz3__rt_blob_key *keys;
  wz3__rt_pipe_key *pipe_keys;     /* PipeSpline corners (spline.pipe.keys); NULL for other splines */
  uint32_t n_pipe_keys;
} wz3__rt_blob;

/* what an operator's init pass produced (op.cache in JS) */
typedef struct wz3__rt_cache {
  uint8_t kind;
  uint8_t owned;                   /* the creating operator frees obj */
  uint8_t class_id;                /* KC_* of the object */
  void *obj;
} wz3__rt_cache;

typedef struct wz3__rt_shaker {
  uint32_t op;
  wz3__rt_cache parent, amplitude;
  uint32_t mode;
  float translate_amp[3], rotate_amp[3], translate_freq[3], rotate_freq[3], center[3];
} wz3__rt_shaker;

typedef struct wz3__rt_instance {
  uint32_t op_id;
  int32_t reset;
  int32_t words[4];
  void *blob;                      /* plain-old-data memory of an operator (particles, chain lines); snapshotted as bytes */
  uint32_t blob_size;
} wz3__rt_instance;

typedef struct wz3__rt_chain {
  wz3__rt_instance *items;
  uint32_t count, cap;
} wz3__rt_chain;

typedef struct wz3__rt_op {
  uint32_t id, convention;
  uint16_t class_id;
  uint8_t input_slots, link_slots, data_words, out_class;
  const wz3_kx_op *src;
  const wz3_kx_class *cls;
  wz3_word *edit, *anim;           /* data_words each */
  uint8_t changed, calc_error, calc_state, executing;
  int32_t first_event;
  wz3__rt_cache cache;
} wz3__rt_op;

typedef struct wz3__rt_event {
  uint32_t id, op;
  int32_t next_op;
  int32_t start, end;
  float velocity, modulation;
  int32_t select;
  float scale[4], rotate[4], translate[4];
  uint32_t color;
  int32_t spline;
  float start_interval, end_interval;
  int32_t flags;
  float matrix[16];
  wz3__rt_chain instances;
  int dynamic;
} wz3__rt_event;

typedef struct wz3__rt_channel { uint32_t n; float *time; const float *value; } wz3__rt_channel;
typedef struct wz3__rt_dspline {
  int32_t interpolation;
  uint32_t n_channels;
  wz3__rt_channel channels[4];
} wz3__rt_dspline;

typedef struct wz3__rt_save { uint8_t index; float value[4]; } wz3__rt_save;

typedef struct wz3__rt_env {
  float vars[WZ3__RT_MAX_VARS][4];
  wz3__rt_save saves[WZ3__RT_MAX_SAVES];
  uint32_t n_saves;
  int32_t event_spline, current_event;
  uint32_t *dynamic; uint32_t n_dynamic, cap_dynamic;
  uint32_t *cleanup; uint32_t n_cleanup, cap_cleanup;
  wz3__rt_chain default_instances;
  wz3__rt_chain *chain;
  uint32_t cursor;
  int clear_instance_memory;
  float *stack; uint32_t depth, cap_stack;      /* matrix stack, 16 floats per level */
  int32_t beat_time, current_time, last_time, time_delta, time_slices, time_jitter;
  int time_reset;
} wz3__rt_env;

struct wz3_rt {
  const wz3_kx *kx;
  wz3_rt_options options;
  uint32_t n_ops;       wz3__rt_op *ops;       wz3_word *words;
  uint32_t n_events;    wz3__rt_event *events;
  uint32_t n_splines;   wz3__rt_dspline *splines;   float *spline_times;
  uint32_t current_root;
  int precalculated;
  uint32_t progress_done, progress_total;       /* wz3_rt_host.progress */
  uint8_t perm[512];
  uint32_t *remaining;                          /* consumers left per operator (release_consumed) */
  wz3__rt_env env;
  int trace;                                    /* tests: FNV chain over every animation execution */
  uint32_t trace_chain, trace_count, trace_active;
  wz3__fj fj;                                   /* frame jobs */
  uint8_t *render_saved;                        /* wz3_rt_render_begin: the state of the frame in progress, until wz3_rt_render_end */
  size_t render_saved_size;
  double render_saved_aspect;
};

static void wz3__rt_chain_clear(wz3__rt_chain *c)
{
  for (uint32_t i = 0; i < c->count; i++) WZ3_FREE(c->items[i].blob);
  c->count = 0;
}
static void wz3__rt_chain_free(wz3__rt_chain *c)
{
  for (uint32_t i = 0; i < c->count; i++) WZ3_FREE(c->items[i].blob);
  WZ3_FREE(c->items);
  c->items = NULL;
  c->count = c->cap = 0;
}

static const float wz3__rt_gradients[16 * 4] = {
  1, 1, 0, 0, -1, 1, 0, 0, 1, -1, 0, 0, -1, -1, 0, 0,
  1, 0, 1, 0, -1, 0, 1, 0, 1, 0, -1, 0, -1, 0, -1, 0,
  0, 1, 1, 0, 0, -1, 1, 0, 0, 1, -1, 0, 0, -1, -1, 0,
  1, 1, 0, 0, -1, 1, 0, 0, 0, -1, 1, 0, 0, -1, -1, 0,
};

/* makePerlinPermutation: ranks from sGetRnd after seed 1, ordered by the exchange sort of the source */
static void wz3__rt_make_permutation(uint8_t perm[512])
{
  wz3_rng random;
  uint32_t ranks[256];
  wz3_rng_init(&random);
  wz3_rng_seed(&random, 1);
  for (uint32_t i = 0; i < 256; i++) {
    ranks[i] = wz3_rng_u32(&random) % 0x10000u;
    perm[i] = (uint8_t)i;
  }
  for (uint32_t i = 0; i < 255; i++)
    for (uint32_t j = i + 1; j < 256; j++)
      if (ranks[i] > ranks[j]) {
        const uint32_t rank = ranks[i]; ranks[i] = ranks[j]; ranks[j] = rank;
        const uint8_t value = perm[i]; perm[i] = perm[j]; perm[j] = value;
      }
  for (uint32_t i = 0; i < 256; i++) perm[256 + i] = perm[i];
}

/* sPerlin3D under the player's 24-bit precision: every Scale3/polynomial step is a float */
static void wz3__rt_perlin3d(const uint8_t perm[512], const double position[3], float out[4])
{
  int32_t integer[3];
  float fade[3];
  for (int j = 0; j < 3; j++) {
    const float coordinate = (float)position[j];
    const int32_t lattice = wz3__rt_round_even(wz3__rt_subf(coordinate, 0.5));
    integer[j] = (int32_t)((uint32_t)lattice & 255u);
    const double fraction = wz3__rt_clamp(wz3__rt_subf(coordinate, (double)lattice), 1.0, 0.0);
    const float fraction2 = wz3__rt_mulf(fraction, fraction);
    const float fraction3 = wz3__rt_mulf(fraction2, fraction);
    const float polynomial = wz3__rt_addf(10.0, wz3__rt_mulf(fraction, wz3__rt_subf(wz3__rt_mulf(6.0, fraction), 15.0)));
    fade[j] = wz3__rt_mulf(fraction3, polynomial);
  }
#define WZ3__RT_GRAD(x, y, z) (wz3__rt_gradients + (perm[perm[perm[(x)] + (y)] + (z)] & 15) * 4)
#define WZ3__RT_LERP3(o, a, b, t)                                                       \
  do {                                                                                  \
    (o)[0] = wz3__rt_addf((a)[0], wz3__rt_mulf(wz3__rt_subf((b)[0], (a)[0]), (t)));     \
    (o)[1] = wz3__rt_addf((a)[1], wz3__rt_mulf(wz3__rt_subf((b)[1], (a)[1]), (t)));     \
    (o)[2] = wz3__rt_addf((a)[2], wz3__rt_mulf(wz3__rt_subf((b)[2], (a)[2]), (t)));     \
    (o)[3] = 0.0f;                                                                      \
  } while (0)
  const int32_t x = integer[0], y = integer[1], z = integer[2];
  float t0[4], t1[4], t2[4];
  WZ3__RT_LERP3(t0, WZ3__RT_GRAD(x, y, z), WZ3__RT_GRAD(x + 1, y, z), fade[0]);
  WZ3__RT_LERP3(t1, WZ3__RT_GRAD(x, y + 1, z), WZ3__RT_GRAD(x + 1, y + 1, z), fade[0]);
  { float r[4]; WZ3__RT_LERP3(r, t0, t1, fade[1]); memcpy(t0, r, sizeof r); }
  WZ3__RT_LERP3(t1, WZ3__RT_GRAD(x, y, z + 1), WZ3__RT_GRAD(x + 1, y, z + 1), fade[0]);
  WZ3__RT_LERP3(t2, WZ3__RT_GRAD(x, y + 1, z + 1), WZ3__RT_GRAD(x + 1, y + 1, z + 1), fade[0]);
  { float r[4]; WZ3__RT_LERP3(r, t1, t2, fade[1]); memcpy(t1, r, sizeof r); }
  WZ3__RT_LERP3(out, t0, t1, fade[2]);
#undef WZ3__RT_GRAD
#undef WZ3__RT_LERP3
}

/* ---- small matrix helpers (kdoc.cpp) ---- */

static void wz3__rt_normalize3(const double v[3], float out[3])
{
  const double length2 = v[0] * v[0] + v[1] * v[1] + v[2] * v[2];
  if (length2 > 1e-20) {
    const double scale = 1.0 / sqrt(length2);
    out[0] = (float)(v[0] * scale);
    out[1] = (float)(v[1] * scale);
    out[2] = (float)(v[2] * scale);
  } else {
    out[0] = 1.0f; out[1] = out[2] = 0.0f;
  }
}

static void wz3__rt_cross3(const double a[3], const double b[3], float out[3])
{
  const double x = a[1] * b[2] - a[2] * b[1];
  const double y = a[2] * b[0] - a[0] * b[2];
  const double z = a[0] * b[1] - a[1] * b[0];
  out[0] = (float)x; out[1] = (float)y; out[2] = (float)z;
}

static void wz3__rt_euler(double a, double b, double c, float out[16])
{
  wz3_mat4_euler((float)a, (float)b, (float)c, out);
}

static void wz3__rt_euler_turns(double x, double y, double z, float out[16])
{
  if ((x < 0 ? -x : x) < 1e-6 && (y < 0 ? -y : y) < 1e-6 && (z < 0 ? -z : z) < 1e-6) {
    wz3_mat4_identity(out);
    return;
  }
  wz3__rt_euler((float)((double)(float)x * WZ3__RT_TWO_PI), (float)((double)(float)y * WZ3__RT_TWO_PI),
                (float)((double)(float)z * WZ3__RT_TWO_PI), out);
}

static void wz3__rt_init_direction(const double direction[3], float out[16])
{
  float k[3], i[3], j[3];
  const double up[3] = {0.0, 1.0, 0.0};
  double kd[3], id[3];
  wz3__rt_normalize3(direction, k);
  kd[0] = k[0]; kd[1] = k[1]; kd[2] = k[2];
  wz3__rt_cross3(up, kd, i);
  id[0] = i[0]; id[1] = i[1]; id[2] = i[2];
  wz3__rt_normalize3(id, i);
  id[0] = i[0]; id[1] = i[1]; id[2] = i[2];
  wz3__rt_cross3(kd, id, j);
  out[0] = i[0]; out[1] = i[1]; out[2] = i[2]; out[3] = 0.0f;
  out[4] = j[0]; out[5] = j[1]; out[6] = j[2]; out[7] = 0.0f;
  out[8] = k[0]; out[9] = k[1]; out[10] = k[2]; out[11] = 0.0f;
  out[12] = out[13] = out[14] = 0.0f; out[15] = 1.0f;
}

static void wz3__rt_init_axis_angle(const float axis[3], double angle, float out[16])
{
  const double ad[3] = {axis[0], axis[1], axis[2]};
  float u[3];
  wz3__rt_normalize3(ad, u);
  const double c = wz3__cos(angle), s = wz3__sin(angle);
  const double x = u[0], y = u[1], z = u[2];
  out[0] = (float)(x * x + c * (1 - x * x));
  out[1] = (float)(x * y + c * (-x * y) + s * z);
  out[2] = (float)(x * z + c * (-x * z) - s * y); out[3] = 0.0f;
  out[4] = (float)(y * x + c * (-y * x) - s * z);
  out[5] = (float)(y * y + c * (1 - y * y));
  out[6] = (float)(y * z + c * (-y * z) + s * x); out[7] = 0.0f;
  out[8] = (float)(z * x + c * (-z * x) + s * y);
  out[9] = (float)(z * y + c * (-z * y) - s * x);
  out[10] = (float)(z * z + c * (1 - z * z)); out[11] = 0.0f;
  out[12] = out[13] = out[14] = 0.0f; out[15] = 1.0f;
}

static void wz3__rt_quat_from_matrix(const float m[16], float q[4])
{
  const double trace = (double)m[0] + (double)m[5] + (double)m[10];
  double s;
  if (trace >= 0) {
    s = sqrt(trace + 1);
    q[0] = (float)(s * 0.5);
    s = 0.5 / s;
    q[1] = (float)(((double)m[9] - m[6]) * s);
    q[2] = (float)(((double)m[2] - m[8]) * s);
    q[3] = (float)(((double)m[4] - m[1]) * s);
  } else {
    int index;
    if (m[5] > m[0]) index = m[10] > m[5] ? 2 : 1;
    else index = m[10] > m[0] ? 2 : 0;
    if (index == 0) {
      s = sqrt(((double)m[0] - ((double)m[5] + m[10])) + 1);
      q[1] = (float)(s * 0.5); s = 0.5 / s;
      q[2] = (float)(((double)m[1] + m[4]) * s);
      q[3] = (float)(((double)m[8] + m[2]) * s);
      q[0] = (float)(((double)m[9] - m[6]) * s);
    } else if (index == 1) {
      s = sqrt(((double)m[5] - ((double)m[10] + m[0])) + 1);
      q[2] = (float)(s * 0.5); s = 0.5 / s;
      q[3] = (float)(((double)m[6] + m[9]) * s);
      q[1] = (float)(((double)m[1] + m[4]) * s);
      q[0] = (float)(((double)m[2] - m[8]) * s);
    } else {
      s = sqrt(((double)m[10] - ((double)m[0] + m[5])) + 1);
      q[3] = (float)(s * 0.5); s = 0.5 / s;
      q[1] = (float)(((double)m[8] + m[2]) * s);
      q[2] = (float)(((double)m[6] + m[9]) * s);
      q[0] = (float)(((double)m[4] - m[1]) * s);
    }
  }
}

static void wz3__rt_quat_to_matrix(const float q[4], float out[16])
{
  const double w = q[0], x = q[1], y = q[2], z = q[3];
  const double xx = 2 * x * x, xy = 2 * x * y, xz = 2 * x * z;
  const double yy = 2 * y * y, yz = 2 * y * z, zz = 2 * z * z;
  const double xw = 2 * x * w, yw = 2 * y * w, zw = 2 * z * w;
  out[0] = (float)(1 - yy - zz); out[1] = (float)(xy - zw); out[2] = (float)(xz + yw); out[3] = 0.0f;
  out[4] = (float)(xy + zw); out[5] = (float)(1 - xx - zz); out[6] = (float)(yz - xw); out[7] = 0.0f;
  out[8] = (float)(xz - yw); out[9] = (float)(yz + xw); out[10] = (float)(1 - xx - yy); out[11] = 0.0f;
  out[12] = out[13] = out[14] = 0.0f; out[15] = 1.0f;
}

static void wz3__rt_quat_lerp(const float a[4], const float b[4], double time, float out[4])
{
  const double dot = (double)a[0] * b[0] + (double)a[1] * b[1] + (double)a[2] * b[2] + (double)a[3] * b[3];
  const double sign = dot < 0 ? -1.0 : 1.0;
  for (int i = 0; i < 4; i++) out[i] = (float)(a[i] + (sign * b[i] - a[i]) * time);
  const double scale = 1.0 / sqrt((double)out[0] * out[0] + (double)out[1] * out[1] + (double)out[2] * out[2] +
                                  (double)out[3] * out[3]);
  for (int i = 0; i < 4; i++) out[i] = (float)(out[i] * scale);
}

/* ---- document splines (genblobspline.cpp, with x87 24-bit precision) ---- */

static double wz3__rt_dspline_channel(const wz3__rt_channel *ch, int32_t interpolation, double time)
{
  const uint32_t count = ch->n;
  const float *kt = ch->time, *kv = ch->value;
  if (!count) return 0.0;
  if (time <= kt[0]) return kv[0];
  if (time >= kt[count - 1]) return kv[count - 1];
  for (uint32_t k = 0; k + 1 < count; k++) {
    if (kt[k + 1] > time) {
      const float difference = wz3__rt_subf(kv[k + 1], kv[k]);
      const float time_difference = wz3__rt_subf(kt[k + 1], kt[k]);
      const float t = wz3__rt_divf(wz3__rt_subf((double)(float)time, kt[k]), time_difference);
      if (interpolation == 1) return wz3__rt_addf(wz3__rt_mulf(t, difference), kv[k]);
      if (interpolation == 2) return kv[k];
      const float d0 = k == 0 ? difference
                              : wz3__rt_divf(wz3__rt_mulf(time_difference, wz3__rt_subf(kv[k + 1], kv[k - 1])),
                                             wz3__rt_subf(kt[k + 1], kt[k - 1]));
      const float d1 = k + 2 == count ? difference
                                      : wz3__rt_divf(wz3__rt_mulf(time_difference, wz3__rt_subf(kv[k + 2], kv[k])),
                                                     wz3__rt_subf(kt[k + 2], kt[k]));
      float value = wz3__rt_addf(d0, d1);
      value = wz3__rt_subf(value, wz3__rt_mulf(2.0, difference));
      value = wz3__rt_mulf(value, t);
      value = wz3__rt_addf(value, wz3__rt_mulf(3.0, difference));
      value = wz3__rt_subf(value, wz3__rt_mulf(2.0, d0));
      value = wz3__rt_subf(value, d1);
      value = wz3__rt_mulf(value, t);
      value = wz3__rt_addf(value, d0);
      value = wz3__rt_mulf(value, t);
      return wz3__rt_addf(value, kv[k]);
    }
  }
  return 0.0;
}

static void wz3__rt_dspline_eval(const wz3__rt_dspline *s, double time, float out[4])
{
  out[0] = out[1] = out[2] = out[3] = 0.0f;
  for (uint32_t i = 0; i < s->n_channels && i < 4; i++)
    out[i] = (float)wz3__rt_dspline_channel(&s->channels[i], s->interpolation, time);
}

/* ---- blob splines (BlobSplinePath) ---- */

typedef struct wz3__rt_segment {
  const wz3__rt_blob_key *p0, *p1, *p2, *p3;
  float fade;
} wz3__rt_segment;

static void wz3__rt_blob_segment(const wz3__rt_blob *s, double time, wz3__rt_segment *out)
{
  const uint32_t n = s->n_keys;
  uint32_t index;
  if (time <= s->keys[0].time) { index = 1; time = 0.0; }
  else if (time >= s->keys[n - 1].time) { index = n - 1; time = s->keys[index].time; }
  else {
    index = 1;
    while (index < n && s->keys[index].time <= time) index++;
  }
  const wz3__rt_blob_key *p1 = &s->keys[index - 1], *p2 = &s->keys[index];
  out->fade = p2->time == p1->time ? p2->time
                                   : wz3__rt_divf(wz3__rt_subf((double)(float)time, p1->time),
                                                  wz3__rt_subf(p2->time, p1->time));
  out->p0 = index >= 2 ? &s->keys[index - 2] : NULL;
  out->p1 = p1;
  out->p2 = p2;
  out->p3 = index + 1 < n ? &s->keys[index + 1] : NULL;
}

static void wz3__rt_hermite(const wz3__rt_blob_key *p0, const wz3__rt_blob_key *p1, const wz3__rt_blob_key *p2,
                            const wz3__rt_blob_key *p3, double fade_in, double tension_in, double continuity_in,
                            double bias_in, int ignore_time, int derivative, float value[7], float delta[7])
{
  const float fade = (float)fade_in, tension = (float)tension_in, continuity = (float)continuity_in,
              bias = (float)bias_in;
  const float one_minus_tension = wz3__rt_subf(1.0, tension);
  float a1 = wz3__rt_mulf(wz3__rt_mulf(one_minus_tension, wz3__rt_subf(1.0, continuity)), wz3__rt_addf(1.0, bias));
  const float b1 = wz3__rt_mulf(wz3__rt_mulf(one_minus_tension, wz3__rt_addf(1.0, continuity)), wz3__rt_subf(1.0, bias));
  const float a2 = wz3__rt_mulf(wz3__rt_mulf(one_minus_tension, wz3__rt_addf(1.0, continuity)), wz3__rt_addf(1.0, bias));
  float b2 = wz3__rt_mulf(wz3__rt_mulf(one_minus_tension, wz3__rt_subf(1.0, continuity)), wz3__rt_subf(1.0, bias));
  float t0, t1 = p1->time, t2 = p2->time, t3;
  if (!p0) { p0 = p2; a1 = -a1; t0 = wz3__rt_addf(t1, wz3__rt_subf(t1, t2)); } else t0 = p0->time;
  if (!p3) { p3 = p1; b2 = -b2; t3 = wz3__rt_addf(t2, wz3__rt_subf(t2, t1)); } else t3 = p3->time;

  const float ff = wz3__rt_mulf(fade, fade), fff = wz3__rt_mulf(ff, fade);
  const float two_fade2 = wz3__rt_mulf(wz3__rt_mulf(2.0, fade), fade);
  const float two_fade3 = wz3__rt_mulf(two_fade2, fade);
  const float three_fade2 = wz3__rt_mulf(wz3__rt_mulf(3.0, fade), fade);
  const float six_fade2 = wz3__rt_mulf(wz3__rt_mulf(6.0, fade), fade);
  float factors[4], derivatives[4];
  factors[0] = wz3__rt_addf(wz3__rt_subf(two_fade3, three_fade2), 1.0);
  factors[1] = wz3__rt_addf(-(double)two_fade3, three_fade2);
  factors[2] = wz3__rt_addf(wz3__rt_subf(fff, two_fade2), fade);
  factors[3] = wz3__rt_subf(fff, ff);
  derivatives[0] = wz3__rt_subf(six_fade2, wz3__rt_mulf(6.0, fade));
  derivatives[1] = wz3__rt_addf(-(double)six_fade2, wz3__rt_mulf(6.0, fade));
  derivatives[2] = wz3__rt_addf(wz3__rt_subf(three_fade2, wz3__rt_mulf(4.0, fade)), 1.0);
  derivatives[3] = wz3__rt_subf(three_fade2, wz3__rt_mulf(2.0, fade));
  if (!ignore_time) {
    t0 = wz3__rt_subf(t1, t0); t1 = wz3__rt_subf(t2, t1); t2 = wz3__rt_subf(t3, t2);
    factors[2] = wz3__rt_divf(wz3__rt_mulf(factors[2], t1), wz3__rt_addf(t1, t0));
    factors[3] = wz3__rt_divf(wz3__rt_mulf(factors[3], t1), wz3__rt_addf(t1, t2));
    derivatives[2] = wz3__rt_divf(wz3__rt_mulf(derivatives[2], t1), wz3__rt_addf(t1, t0));
    derivatives[3] = wz3__rt_divf(wz3__rt_mulf(derivatives[3], t1), wz3__rt_addf(t1, t2));
  } else {
    factors[2] = wz3__rt_mulf(factors[2], 0.5); factors[3] = wz3__rt_mulf(factors[3], 0.5);
    derivatives[2] = wz3__rt_mulf(derivatives[2], 0.5); derivatives[3] = wz3__rt_mulf(derivatives[3], 0.5);
  }
  for (int i = 0; i < 4; i++) derivatives[i] = wz3__rt_mulf(derivatives[i], 0.25);

  for (int f = 0; f < 7; f++) {
    const float tangent1 = wz3__rt_addf(wz3__rt_mulf(a1, wz3__rt_subf(p1->f[f], p0->f[f])),
                                        wz3__rt_mulf(b1, wz3__rt_subf(p2->f[f], p1->f[f])));
    const float tangent2 = wz3__rt_addf(wz3__rt_mulf(a2, wz3__rt_subf(p2->f[f], p1->f[f])),
                                        wz3__rt_mulf(b2, wz3__rt_subf(p3->f[f], p2->f[f])));
    value[f] = wz3__rt_addf(wz3__rt_addf(wz3__rt_addf(wz3__rt_mulf(factors[0], p1->f[f]), wz3__rt_mulf(factors[1], p2->f[f])),
                                         wz3__rt_mulf(factors[2], tangent1)),
                            wz3__rt_mulf(factors[3], tangent2));
    if (derivative)
      delta[f] = wz3__rt_addf(wz3__rt_addf(wz3__rt_addf(wz3__rt_mulf(derivatives[0], p1->f[f]), wz3__rt_mulf(derivatives[1], p2->f[f])),
                                           wz3__rt_mulf(derivatives[2], tangent1)),
                              wz3__rt_mulf(derivatives[3], tangent2));
  }
}

/* BlobSplinePath.eval: matrix gets the pose, *zoom the lens value */
static void wz3__rt_blob_eval(const wz3__rt_blob *s, double time, float matrix[16], float *zoom)
{
  wz3__rt_segment seg;
  wz3__rt_blob_segment(s, time, &seg);
  if (s->mode == 4) {
    const float t = seg.fade;
    const float tt = wz3__rt_mulf(t, t), ttt = wz3__rt_mulf(tt, t);
    const float f1 = wz3__rt_addf(wz3__rt_subf(wz3__rt_mulf(2.0, ttt), wz3__rt_mulf(3.0, tt)), 1.0);
    const float f2 = wz3__rt_addf(wz3__rt_mulf(-2.0, ttt), wz3__rt_mulf(3.0, tt));
    const float f3 = wz3__rt_mulf(wz3__rt_addf(wz3__rt_subf(ttt, wz3__rt_mulf(2.0, tt)), t), wz3__rt_subf(1.0, s->tension));
    const float f4 = wz3__rt_mulf(wz3__rt_subf(ttt, tt), wz3__rt_subf(1.0, s->tension));
    float q1[4], q2[4], quaternion[4], m1[16], m2[16];
    q1[0] = seg.p1->f[6]; q1[1] = seg.p1->f[3]; q1[2] = seg.p1->f[4]; q1[3] = seg.p1->f[5];
    q2[0] = seg.p2->f[6]; q2[1] = seg.p2->f[3]; q2[2] = seg.p2->f[4]; q2[3] = seg.p2->f[5];
    wz3__rt_quat_lerp(q1, q2, t, quaternion);
    wz3__rt_quat_to_matrix(quaternion, matrix);
    wz3__rt_quat_to_matrix(q1, m1);
    wz3__rt_quat_to_matrix(q2, m2);
    matrix[12] = wz3__rt_addf(wz3__rt_addf(wz3__rt_addf(wz3__rt_mulf(seg.p1->f[0], f1), wz3__rt_mulf(seg.p2->f[0], f2)),
                                           wz3__rt_mulf(m1[8], f3)), wz3__rt_mulf(m2[8], f4));
    matrix[13] = wz3__rt_addf(wz3__rt_addf(wz3__rt_addf(wz3__rt_mulf(seg.p1->f[1], f1), wz3__rt_mulf(seg.p2->f[1], f2)),
                                           wz3__rt_mulf(m1[9], f3)), wz3__rt_mulf(m2[9], f4));
    matrix[14] = wz3__rt_addf(wz3__rt_addf(wz3__rt_addf(wz3__rt_mulf(seg.p1->f[2], f1), wz3__rt_mulf(seg.p2->f[2], f2)),
                                           wz3__rt_mulf(m1[10], f3)), wz3__rt_mulf(m2[10], f4));
    matrix[15] = 1.0f;
    *zoom = 1.0f;
    return;
  }
  float key[7], key_delta[7];
  memset(key_delta, 0, sizeof key_delta);
  wz3__rt_hermite(seg.p0, seg.p1, seg.p2, seg.p3, seg.fade, s->tension, s->continuity, 0.0, s->uniform != 0,
                  s->mode == 3, key, key_delta);
  const float px = key[0], py = key[1], pz = key[2], rx = key[3], ry = key[4], rz = key[5], zm = key[6];
  double direction[3];
  switch (s->mode) {
  case 1:
    direction[0] = (double)s->target[0] - px; direction[1] = (double)s->target[1] - py;
    direction[2] = (double)s->target[2] - pz;
    wz3__rt_init_direction(direction, matrix);
    break;
  case 2:
    direction[0] = (double)rx - px; direction[1] = (double)ry - py; direction[2] = (double)rz - pz;
    wz3__rt_init_direction(direction, matrix);
    break;
  case 3:
    direction[0] = key_delta[0]; direction[1] = key_delta[1]; direction[2] = key_delta[2];
    wz3__rt_init_direction(direction, matrix);
    break;
  case 5: {
    float mx[16], my[16], mz[16], rotation[16];
    wz3__rt_euler((double)rx * WZ3__RT_TWO_PI, 0, 0, mx);
    wz3__rt_euler(0, (double)ry * WZ3__RT_TWO_PI, 0, my);
    wz3__rt_euler(0, 0, (double)rz * WZ3__RT_TWO_PI, mz);
    wz3_mat4_mul3(mz, mx, rotation);
    wz3_mat4_mul3(rotation, my, matrix);
    break;
  }
  default:
    wz3__rt_euler_turns(rx, ry, rz, matrix);
    break;
  }
  matrix[12] = px; matrix[13] = py; matrix[14] = pz; matrix[15] = 1.0f;
  *zoom = zm;
}

/* BlobSplinePath.normalize: re-times the keys by arc length (used by pipe splines) */
static double wz3__rt_blob_normalize(wz3__rt_blob *s)
{
  s->uniform = 1;
  for (uint32_t i = 1; i < s->n_keys; i++) {            /* stable insertion sort by time */
    const wz3__rt_blob_key key = s->keys[i];
    uint32_t j = i;
    while (j > 0 && s->keys[j - 1].time > key.time) { s->keys[j] = s->keys[j - 1]; j--; }
    s->keys[j] = key;
  }
  const uint32_t segments = s->n_keys - 1;
  float distances[1024];
  float *dist = segments <= 1024 ? distances : (float *)WZ3_MALLOC((size_t)segments * sizeof(float));
  if (!dist) return 0.0;
  float matrix[16], zoom;
  double distance = 0.0;
  wz3__rt_blob_eval(s, s->keys[0].time, matrix, &zoom);
  float previous[3] = {matrix[12], matrix[13], matrix[14]};
  for (uint32_t i = 0; i < segments; i++) {
    for (int j = 1; j <= 16; j++) {
      const double time = (double)s->keys[i].time + ((double)s->keys[i + 1].time - (double)s->keys[i].time) * ((double)j / 16.0);
      wz3__rt_blob_eval(s, time, matrix, &zoom);
      const double dx = (double)matrix[12] - previous[0], dy = (double)matrix[13] - previous[1],
                   dz = (double)matrix[14] - previous[2];
      distance = (float)(distance + sqrt(dx * dx + dy * dy + dz * dz));
      previous[0] = matrix[12]; previous[1] = matrix[13]; previous[2] = matrix[14];
    }
    dist[i] = (float)distance;
  }
  if (distance == 0.0) distance = 1.0;
  s->keys[0].time = 0.0f;
  for (uint32_t i = 1; i < s->n_keys; i++) s->keys[i].time = (float)((double)dist[i - 1] / distance);
  if (dist != distances) WZ3_FREE(dist);
  return distance;
}

static inline int32_t wz3__rt_rd_i32(const uint8_t *p)
{
  return (int32_t)((uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24));
}
static inline float wz3__rt_rd_f32(const uint8_t *p)
{
  const uint32_t u = (uint32_t)wz3__rt_rd_i32(p);
  float f;
  memcpy(&f, &u, 4);
  return f;
}

static void wz3__rt_blob_free(wz3__rt_blob *s)
{
  if (!s) return;
  WZ3_FREE(s->keys);
  WZ3_FREE(s->pipe_keys);
  WZ3_FREE(s);
}

/* BlobSplinePath.fromBlob */
static wz3__rt_blob *wz3__rt_blob_from_bytes(const uint8_t *b, size_t size, wz3_result *res)
{
  if (size < 64) { *res = WZ3_ERR_TRUNCATED; return NULL; }
  const int32_t count = wz3__rt_rd_i32(b);
  if (count < 2 || 64 + (size_t)count * 36 > size) { *res = WZ3_ERR_FORMAT; return NULL; }
  wz3__rt_blob *s = (wz3__rt_blob *)WZ3_MALLOC(sizeof *s);
  if (!s) { *res = WZ3_ERR_OUT_OF_MEMORY; return NULL; }
  memset(s, 0, sizeof *s);
  s->keys = (wz3__rt_blob_key *)WZ3_MALLOC((size_t)count * sizeof(wz3__rt_blob_key));
  if (!s->keys) { WZ3_FREE(s); *res = WZ3_ERR_OUT_OF_MEMORY; return NULL; }
  s->n_keys = (uint32_t)count;
  for (int32_t i = 0; i < count; i++) {
    const uint8_t *k = b + 64 + (size_t)i * 36;
    s->keys[i].select = wz3__rt_rd_i32(k);
    s->keys[i].time = wz3__rt_rd_f32(k + 4);
    for (int f = 0; f < 7; f++) s->keys[i].f[f] = wz3__rt_rd_f32(k + 8 + 4 * f);
  }
  s->version = wz3__rt_rd_i32(b + 4);
  s->select = wz3__rt_rd_i32(b + 8);
  s->mode = wz3__rt_rd_i32(b + 12);
  for (int i = 0; i < 4; i++) s->target[i] = wz3__rt_rd_f32(b + 16 + 4 * i);
  s->continuity = wz3__rt_rd_f32(b + 32);
  s->tension = wz3__rt_rd_f32(b + 36);
  s->uniform = wz3__rt_rd_i32(b + 40);
  if (s->version < 3) { s->uniform = 0; s->tension = 0.0f; s->continuity = 0.0f; }
  s->version = 3;
  *res = WZ3_OK;
  return s;
}

/* parsePipeBlob + pipeToSpline */
static wz3__rt_blob *wz3__rt_blob_from_pipe(const uint8_t *b, size_t size, wz3_result *res)
{
  if (size < 64) { *res = WZ3_ERR_TRUNCATED; return NULL; }
  const int32_t count = wz3__rt_rd_i32(b);
  if (count < 1 || 64 + (size_t)count * 32 > size) { *res = WZ3_ERR_FORMAT; return NULL; }
  const float tension = wz3__rt_rd_f32(b + 12);
  const float start[3] = {wz3__rt_rd_f32(b + 16), wz3__rt_rd_f32(b + 20), wz3__rt_rd_f32(b + 24)};
  const float rotation = wz3__rt_rd_f32(b + 28);

  wz3__rt_blob *s = (wz3__rt_blob *)WZ3_MALLOC(sizeof *s);
  if (!s) { *res = WZ3_ERR_OUT_OF_MEMORY; return NULL; }
  memset(s, 0, sizeof *s);
  s->keys = (wz3__rt_blob_key *)WZ3_MALLOC((size_t)count * 2 * sizeof(wz3__rt_blob_key));
  s->pipe_keys = (wz3__rt_pipe_key *)WZ3_MALLOC((size_t)count * sizeof(wz3__rt_pipe_key));
  if (!s->keys || !s->pipe_keys) { WZ3_FREE(s->keys); WZ3_FREE(s->pipe_keys); WZ3_FREE(s); *res = WZ3_ERR_OUT_OF_MEMORY; return NULL; }
  s->n_keys = (uint32_t)count * 2;
  s->n_pipe_keys = (uint32_t)count;

  float matrix[16];
  wz3__rt_euler(0, 0, (double)rotation * WZ3__RT_TWO_PI, matrix);
  double old_radius = 0.0;
  float current[3] = {start[0], start[1], start[2]};
  for (int32_t i = 0; i < count; i++) {
    const uint8_t *pk = b + 64 + (size_t)i * 32;
    float previous[3] = {current[0], current[1], current[2]};
    current[0] = wz3__rt_rd_f32(pk); current[1] = wz3__rt_rd_f32(pk + 4); current[2] = wz3__rt_rd_f32(pk + 8);
    const double radius = wz3__rt_rd_f32(pk + 12);
    s->pipe_keys[i].x = current[0]; s->pipe_keys[i].y = current[1]; s->pipe_keys[i].z = current[2];
    s->pipe_keys[i].radius = (float)radius;
    const double diff[3] = {(double)current[0] - previous[0], (double)current[1] - previous[1],
                            (double)current[2] - previous[2]};
    float direction[3];
    wz3__rt_normalize3(diff, direction);
    double ac = (double)matrix[8] * direction[0] + (double)matrix[9] * direction[1] + (double)matrix[10] * direction[2];
    ac = wz3__rt_clamp(ac, 1.0, -1.0);
    const double angle = wz3__acos(ac);
    const double md[3] = {matrix[8], matrix[9], matrix[10]}, dd[3] = {direction[0], direction[1], direction[2]};
    float axis[3], axis_angle[16], product[16], quaternion[4];
    wz3__rt_cross3(md, dd, axis);
    wz3__rt_init_axis_angle(axis, angle, axis_angle);
    wz3_mat4_mul3(matrix, axis_angle, product);
    memcpy(matrix, product, sizeof matrix);
    wz3__rt_quat_from_matrix(matrix, quaternion);
    wz3__rt_blob_key *k0 = &s->keys[i * 2], *k1 = &s->keys[i * 2 + 1];
    k0->select = 0;
    k0->time = (float)i;
    k0->f[0] = (float)((double)previous[0] + direction[0] * old_radius);
    k0->f[1] = (float)((double)previous[1] + direction[1] * old_radius);
    k0->f[2] = (float)((double)previous[2] + direction[2] * old_radius);
    k0->f[3] = quaternion[1]; k0->f[4] = quaternion[2]; k0->f[5] = quaternion[3]; k0->f[6] = quaternion[0];
    old_radius = radius;
    k1->select = 0;
    k1->time = (float)((double)i + 0.5);
    k1->f[0] = (float)((double)current[0] - direction[0] * old_radius);
    k1->f[1] = (float)((double)current[1] - direction[1] * old_radius);
    k1->f[2] = (float)((double)current[2] - direction[2] * old_radius);
    k1->f[3] = quaternion[1]; k1->f[4] = quaternion[2]; k1->f[5] = quaternion[3]; k1->f[6] = quaternion[0];
  }
  s->version = 3;
  s->select = -1;
  s->mode = 4;
  s->target[3] = 1.0f;
  s->tension = tension;
  wz3__rt_blob_normalize(s);
  *res = WZ3_OK;
  return s;
}

/* ---- evaluation of any spline-class cache ---- */

static void wz3__rt_spline_eval(const wz3_rt *rt, const wz3__rt_cache *cache, double time, float matrix[16], float *zoom);

/* ShakerSpline.eval (GenSplineShaker::Eval): vars[0] is the stand-in event time without an event */
static void wz3__rt_shaker_eval(const wz3_rt *rt, const wz3__rt_shaker *sh, double time2, float matrix[16], float *zoom_out)
{
  const wz3__rt_env *env = &rt->env;
  double zoom = 1.0, amplitude = 1.0;
  if (sh->amplitude.kind == WZ3__RC_BLOB || sh->amplitude.kind == WZ3__RC_SHAKER) {
    float scratch[16], z;
    wz3__rt_spline_eval(rt, &sh->amplitude, time2, scratch, &z);
    amplitude = z;
  }
  if (sh->parent.kind == WZ3__RC_BLOB || sh->parent.kind == WZ3__RC_SHAKER) {
    float z;
    wz3__rt_spline_eval(rt, &sh->parent, time2, matrix, &z);
    zoom = z;
  } else {
    wz3_mat4_identity(matrix);
  }

  const wz3__rt_op *op = &rt->ops[sh->op];
  int32_t event = op->first_event;
  int fake_mode = event < 0;
  while (event >= 0 || fake_mode) {
    double time;
    if (fake_mode) {
      time = env->vars[0][0];
    } else {
      const wz3__rt_event *e = &rt->events[event];
      time = (float)(((double)e->end - (double)env->beat_time) / ((double)e->end - (double)e->start));
    }
    if (time > 0 && time < 1) {
      double ramp;
      switch (sh->mode & 15u) {
      case 1: ramp = time; time = wz3__rt_subf(1.0, time); break;
      case 2: ramp = (float)wz3__sin(wz3__rt_mulf(time, WZ3__RT_PI_F)); break;
      case 3: ramp = 1.0; break;
      default: ramp = wz3__rt_subf(1.0, time); break;
      }
      ramp = wz3__rt_mulf(ramp, amplitude);
      double tx, ty, tz, rx, ry, rz;
      if (sh->mode & 32u) {
        const double tp[3] = {wz3__rt_mulf(sh->translate_freq[0], time), wz3__rt_mulf(sh->translate_freq[1], time),
                              wz3__rt_mulf(sh->translate_freq[2], time)};
        const double rp[3] = {wz3__rt_mulf(sh->rotate_freq[0], time), wz3__rt_mulf(sh->rotate_freq[1], time),
                              wz3__rt_mulf(sh->rotate_freq[2], time)};
        float translation[4], rotation[4];
        wz3__rt_perlin3d(rt->perm, tp, translation);
        wz3__rt_perlin3d(rt->perm, rp, rotation);
        tx = translation[0]; ty = translation[1]; tz = translation[2];
        rx = rotation[0]; ry = rotation[1]; rz = rotation[2];
      } else {
#define WZ3__RT_WAVE(freq) wz3__sin(wz3__rt_mulf(wz3__rt_mulf(time, (freq)), WZ3__RT_TWO_PI))
        tx = wz3__rt_mulf(wz3__rt_mulf(WZ3__RT_WAVE(sh->translate_freq[0]), sh->translate_amp[0]), ramp);
        ty = wz3__rt_mulf(wz3__rt_mulf(WZ3__RT_WAVE(sh->translate_freq[1]), sh->translate_amp[1]), ramp);
        tz = wz3__rt_mulf(wz3__rt_mulf(WZ3__RT_WAVE(sh->translate_freq[2]), sh->translate_amp[2]), ramp);
        rx = wz3__rt_mulf(wz3__rt_mulf(WZ3__RT_WAVE(sh->rotate_freq[0]), sh->rotate_amp[0]), ramp);
        ry = wz3__rt_mulf(wz3__rt_mulf(WZ3__RT_WAVE(sh->rotate_freq[1]), sh->rotate_amp[1]), ramp);
        rz = wz3__rt_mulf(wz3__rt_mulf(WZ3__RT_WAVE(sh->rotate_freq[2]), sh->rotate_amp[2]), ramp);
#undef WZ3__RT_WAVE
      }
      /* the repeated amplitude/ramp multiplies (and tz's y amplitude) are source behaviour */
      tx = wz3__rt_mulf(wz3__rt_mulf(tx, sh->translate_amp[0]), ramp);
      ty = wz3__rt_mulf(wz3__rt_mulf(ty, sh->translate_amp[1]), ramp);
      tz = wz3__rt_mulf(wz3__rt_mulf(tz, sh->translate_amp[1]), ramp);
      rx = wz3__rt_mulf(wz3__rt_mulf(rx, sh->rotate_amp[0]), ramp);
      ry = wz3__rt_mulf(wz3__rt_mulf(ry, sh->rotate_amp[1]), ramp);
      rz = wz3__rt_mulf(wz3__rt_mulf(rz, sh->rotate_amp[2]), ramp);
      float shake[16];
      wz3__rt_euler(rx, ry, rz, shake);
      shake[12] = (float)tx; shake[13] = (float)ty; shake[14] = (float)tz;
      if (sh->mode & 16u) {
        matrix[12] = wz3__rt_subf(matrix[12], sh->center[0]);
        matrix[13] = wz3__rt_subf(matrix[13], sh->center[1]);
        matrix[14] = wz3__rt_subf(matrix[14], sh->center[2]);
        wz3_mat4_mul_a(matrix, shake, matrix);
        matrix[12] = wz3__rt_addf(matrix[12], sh->center[0]);
        matrix[13] = wz3__rt_addf(matrix[13], sh->center[1]);
        matrix[14] = wz3__rt_addf(matrix[14], sh->center[2]);
      } else {
        wz3_mat4_mul_a(shake, matrix, matrix);
      }
    }
    if (fake_mode) fake_mode = 0;
    else event = rt->events[event].next_op;
  }
  *zoom_out = (float)zoom;
}

static void wz3__rt_spline_eval(const wz3_rt *rt, const wz3__rt_cache *cache, double time, float matrix[16], float *zoom)
{
  if (cache->kind == WZ3__RC_BLOB) wz3__rt_blob_eval((const wz3__rt_blob *)cache->obj, time, matrix, zoom);
  else wz3__rt_shaker_eval(rt, (const wz3__rt_shaker *)cache->obj, time, matrix, zoom);
}

/* ---- environment ---- */

static wz3_result wz3__rt_chain_instance(wz3__rt_env *env, uint32_t op_id, wz3__rt_instance **out)
{
  wz3__rt_chain *chain = env->chain;
  const uint32_t index = env->cursor++;
  if (index < chain->count) {
    wz3__rt_instance *instance = &chain->items[index];
    if (instance->op_id != op_id) return WZ3_ERR_OPERATOR;   /* instance chain mismatch */
    instance->reset = 0;
    *out = instance;
    return WZ3_OK;
  }
  if (chain->count == chain->cap) {
    const uint32_t cap = chain->cap ? chain->cap * 2 : 8;
    wz3__rt_instance *items = (wz3__rt_instance *)WZ3_MALLOC((size_t)cap * sizeof *items);
    if (!items) return WZ3_ERR_OUT_OF_MEMORY;
    if (chain->count) memcpy(items, chain->items, (size_t)chain->count * sizeof *items);
    WZ3_FREE(chain->items);
    chain->items = items;
    chain->cap = cap;
  }
  wz3__rt_instance *instance = &chain->items[chain->count++];
  memset(instance, 0, sizeof *instance);
  instance->op_id = op_id;
  instance->reset = 1;
  *out = instance;
  return WZ3_OK;
}

static wz3_result wz3__rt_env_pop(wz3__rt_env *env, uint32_t count)
{
  while (count-- > 0) {
    if (!env->n_saves) return WZ3_ERR_OPERATOR;                /* variable-save stack underflow */
    const wz3__rt_save *saved = &env->saves[--env->n_saves];
    memcpy(env->vars[saved->index], saved->value, sizeof saved->value);
  }
  return WZ3_OK;
}

static wz3_result wz3__rt_stack_push_mul(wz3__rt_env *env, const float matrix[16])
{
  if (env->depth == env->cap_stack) {
    const uint32_t cap = env->cap_stack * 2;
    float *stack = (float *)WZ3_MALLOC((size_t)cap * 16 * sizeof(float));
    if (!stack) return WZ3_ERR_OUT_OF_MEMORY;
    memcpy(stack, env->stack, (size_t)env->depth * 16 * sizeof(float));
    WZ3_FREE(env->stack);
    env->stack = stack;
    env->cap_stack = cap;
  }
  float *top = env->stack + (size_t)(env->depth - 1) * 16;
  wz3_mat4_mul_a(matrix, top, env->stack + (size_t)env->depth * 16);   /* PushMul: MulA(matrix, top) */
  env->depth++;
  return WZ3_OK;
}
static void wz3__rt_stack_pop(wz3__rt_env *env) { if (env->depth > 1) env->depth--; }
static void wz3__rt_stack_pop_all(wz3__rt_env *env)
{
  env->depth = 1;
  wz3_mat4_identity(env->stack);
}

static wz3_result wz3__rt_push_index(uint32_t **list, uint32_t *count, uint32_t *cap, uint32_t value)
{
  if (*count == *cap) {
    const uint32_t ncap = *cap ? *cap * 2 : 16;
    uint32_t *items = (uint32_t *)WZ3_MALLOC((size_t)ncap * sizeof(uint32_t));
    if (!items) return WZ3_ERR_OUT_OF_MEMORY;
    if (*count) memcpy(items, *list, (size_t)*count * sizeof(uint32_t));
    WZ3_FREE(*list);
    *list = items;
    *cap = ncap;
  }
  (*list)[(*count)++] = value;
  return WZ3_OK;
}

/* Environment.addStaticEvent: *added is 1 when the event is active this frame */
static wz3_result wz3__rt_add_static_event(wz3_rt *rt, uint32_t ev, int *added)
{
  wz3__rt_env *env = &rt->env;
  wz3__rt_event *event = &rt->events[ev];
  *added = 0;
  if (event->start == event->end || (env->beat_time >= event->start && env->beat_time < event->end)) {
    if (env->clear_instance_memory) wz3__rt_chain_clear(&event->instances);
    wz3__rt_op *op = &rt->ops[event->op];
    if (op->first_event < 0) {
      const wz3_result r = wz3__rt_push_index(&env->cleanup, &env->n_cleanup, &env->cap_cleanup, event->op);
      if (r != WZ3_OK) return r;
    }
    event->next_op = op->first_event;
    op->first_event = (int32_t)ev;
    *added = 1;
  }
  return WZ3_OK;
}

static void wz3__fj_init_frame(wz3_rt *rt);
static wz3_result wz3__fj_exec(wz3_rt *rt, wz3__rt_op *op, int *handled);
static void wz3__fj_ipp_branch_begin(wz3_rt *rt);
static wz3_result wz3__fj_ipp_branch_end(wz3_rt *rt);

static wz3_result wz3__rt_init_frame(wz3_rt *rt, int32_t beat, int32_t milliseconds)
{
  wz3__rt_env *env = &rt->env;
  memset(env->vars, 0, sizeof env->vars);
  env->vars[0][0] = env->vars[0][1] = env->vars[0][2] = env->vars[0][3] = (float)((double)beat / 65536.0);
  env->vars[4][0] = env->vars[4][1] = env->vars[4][2] = 1.0f;
  env->n_saves = 0;
  wz3__rt_stack_pop_all(env);
  wz3__fj_init_frame(rt);
  env->event_spline = -1;
  env->current_event = -1;
  env->beat_time = beat;
  env->last_time = env->current_time;
  env->current_time = milliseconds;
  env->time_delta = (int32_t)((uint32_t)env->current_time - (uint32_t)env->last_time);
  if (env->time_delta < 0) {
    env->time_reset = 1;
  } else {
    env->time_slices = env->current_time / 10 - env->last_time / 10;
    env->time_jitter = env->current_time % 10;
    env->time_reset = 0;
  }
  if (env->time_reset) env->time_delta = env->time_slices = env->time_jitter = 0;
  if (env->n_cleanup) return WZ3_ERR_OPERATOR;               /* initFrame called before exitFrame */
  for (uint32_t i = 0; i < env->n_dynamic; i++) {
    int added;
    const wz3_result r = wz3__rt_add_static_event(rt, env->dynamic[i], &added);
    if (r != WZ3_OK) return r;
  }
  env->chain = &env->default_instances;
  env->cursor = 0;
  return WZ3_OK;
}

static void wz3__rt_exit_frame(wz3_rt *rt)
{
  wz3__rt_env *env = &rt->env;
  for (uint32_t i = 0; i < env->n_cleanup; i++) rt->ops[env->cleanup[i]].first_event = -1;
  env->n_cleanup = 0;
  for (uint32_t i = 0; i < env->n_dynamic;) {
    wz3__rt_event *event = &rt->events[env->dynamic[i]];
    if (event->end != event->start && env->beat_time >= event->end) {
      wz3__rt_chain_clear(&event->instances);
      env->dynamic[i] = env->dynamic[env->n_dynamic - 1];
      env->n_dynamic--;
    } else {
      i++;
    }
  }
  env->clear_instance_memory = 0;
}

/* ---- parameters ---- */

static int wz3__rt_kind_is_float(char kind) { return kind == 'g' || kind == 'f' || kind == 'e' || kind == 'G' || kind == 'F' || kind == 'E'; }
static int wz3__rt_kind_is_signed(char kind) { return kind == 'i' || kind == 's' || kind == 'I' || kind == 'S'; }

/* Op.syncAnimParameters()[i] */
static double wz3__rt_anim_param(const wz3__rt_op *op, uint32_t i)
{
  const char kind = i < op->cls->packing_length ? op->cls->packing[i] : '-';
  if (wz3__rt_kind_is_float(kind)) return (double)op->anim[i].f;
  if (wz3__rt_kind_is_signed(kind)) return (double)op->anim[i].i;
  return (double)op->anim[i].u;
}

/* ---- animation VM (Environment.executeAnimation) ---- */

static void wz3__rt_trace_op(wz3_rt *rt, const wz3__rt_op *op)
{
  if (!rt->trace) return;
  uint32_t h = rt->trace_chain;
  h = (h ^ op->id) * 0x01000193u;
  for (uint32_t i = 0; i < op->data_words; i++) h = (h ^ op->anim[i].u) * 0x01000193u;
  rt->trace_chain = h;
  rt->trace_count++;
}

static wz3_result wz3__rt_exec_animation(wz3_rt *rt, wz3__rt_op *op, uint32_t *pop_count_out)
{
  wz3__rt_env *env = &rt->env;
  const uint8_t *bytes = op->src->anim;
  const size_t size = op->src->anim_size;
  *pop_count_out = 0;
  if (size == 0 || (size == 1 && bytes[0] == 0x01)) {          /* the canonical static program */
    wz3__rt_trace_op(rt, op);
    return WZ3_OK;
  }
  float stack[260][4];
  uint32_t sp = 0, pop_count = 0;
  size_t cursor = 0;
  int changed = 0;
#define WZ3__RT_NEED(n) do { if (cursor + (size_t)(n) > size) return WZ3_ERR_TRUNCATED; } while (0)
#define WZ3__RT_TOP() (stack[sp - 1])
#define WZ3__RT_REQUIRE_STACK() do { if (!sp) return WZ3_ERR_OPERATOR; } while (0)
#define WZ3__RT_SCALAR(v, x) do { const float s_ = (float)(x); (v)[0] = (v)[1] = (v)[2] = (v)[3] = s_; } while (0)
  for (;;) {
    WZ3__RT_NEED(1);
    const uint32_t command = bytes[cursor++];
    const uint32_t base = command >= 0x80 ? command & 0xf0u : command;
    switch (base) {
    case 0x00: break;
    case 0x01:
      if (sp) return WZ3_ERR_OPERATOR;
      if (changed) op->changed = 1;
      wz3__rt_trace_op(rt, op);
      *pop_count_out = pop_count;
      return WZ3_OK;
    case 0x02: {
      WZ3__RT_NEED(1);
      const uint32_t index = bytes[cursor++];
      if (index >= WZ3__RT_MAX_VARS) return WZ3_ERR_OPERATOR;
      memcpy(stack[sp++], env->vars[index], sizeof stack[0]);
      break;
    }
    case 0x03: case 0x04: case 0x05: case 0x06: {
      WZ3__RT_NEED(1);
      const uint32_t index = bytes[cursor++], count = base - 0x03u + 1u;
      if (index + count > op->data_words) return WZ3_ERR_OPERATOR;
      float *value = stack[sp++];
      value[0] = value[1] = value[2] = value[3] = 0.0f;
      if (count == 1) WZ3__RT_SCALAR(value, op->edit[index].f);
      else for (uint32_t i = 0; i < count; i++) value[i] = op->edit[index + i].f;
      break;
    }
    case 0x07: case 0x08: case 0x09: case 0x0a: {
      WZ3__RT_REQUIRE_STACK();
      const float v = WZ3__RT_TOP()[base - 0x07u];
      WZ3__RT_SCALAR(WZ3__RT_TOP(), v);
      break;
    }
    case 0x0b: case 0x0c: case 0x0d: case 0x0e: case 0x0f: case 0x1f: {
      WZ3__RT_REQUIRE_STACK();
      float right[4];
      memcpy(right, stack[--sp], sizeof right);
      WZ3__RT_REQUIRE_STACK();
      float *left = WZ3__RT_TOP();
      for (int i = 0; i < 4; i++) {
        if (base == 0x0b) left[i] = (float)((double)left[i] + (double)right[i]);
        else if (base == 0x0c) left[i] = (float)((double)left[i] - (double)right[i]);
        else if (base == 0x0d) left[i] = (float)((double)left[i] * (double)right[i]);
        else if (base == 0x0e) left[i] = (float)((double)left[i] / (double)right[i]);
        else if (base == 0x0f) left[i] = (float)fmod((double)left[i], (double)right[i]);
        else left[i] = (float)wz3_pow((double)left[i], (double)right[i]);
      }
      break;
    }
    case 0x11: WZ3__RT_REQUIRE_STACK(); for (int i = 0; i < 4; i++) WZ3__RT_TOP()[i] = -WZ3__RT_TOP()[i]; break;
    case 0x10:
      WZ3__RT_REQUIRE_STACK();
      for (int i = 0; i < 4; i++) WZ3__RT_TOP()[i] = (float)(1.0 - (double)WZ3__RT_TOP()[i]);
      break;
    case 0x12: WZ3__RT_REQUIRE_STACK(); WZ3__RT_SCALAR(WZ3__RT_TOP(), wz3__sin((double)WZ3__RT_TOP()[0] * WZ3__RT_TWO_PI)); break;
    case 0x13: WZ3__RT_REQUIRE_STACK(); WZ3__RT_SCALAR(WZ3__RT_TOP(), wz3__cos((double)WZ3__RT_TOP()[0] * WZ3__RT_TWO_PI)); break;
    case 0x14: {
      WZ3__RT_REQUIRE_STACK();
      double value = fmod((double)WZ3__RT_TOP()[0], 1.0);
      if (value < 0) value += 1;
      WZ3__RT_SCALAR(WZ3__RT_TOP(), value < 0.5 ? 1 : 0);
      break;
    }
    case 0x15: {
      WZ3__RT_REQUIRE_STACK();
      double value = fmod((double)WZ3__RT_TOP()[0], 1.0);
      if (value < 0) value += 1;
      WZ3__RT_SCALAR(WZ3__RT_TOP(), value);
      break;
    }
    case 0x16: {
      WZ3__RT_NEED(16);
      float *value = stack[sp++];
      for (int i = 0; i < 4; i++) value[i] = wz3__rt_rd_f32(bytes + cursor + (size_t)i * 4);
      cursor += 16;
      break;
    }
    case 0x17: {
      WZ3__RT_NEED(4);
      float *value = stack[sp++];
      for (int i = 0; i < 4; i++) value[i] = (float)((double)bytes[cursor + (size_t)i] / 255.0);
      cursor += 4;
      break;
    }
    case 0x18: {
      WZ3__RT_NEED(4);
      WZ3__RT_SCALAR(stack[sp], wz3__rt_rd_f32(bytes + cursor));
      sp++;
      cursor += 4;
      break;
    }
    case 0x19: {
      WZ3__RT_NEED(2);
      const uint32_t index = (uint32_t)bytes[cursor] | ((uint32_t)bytes[cursor + 1] << 8);
      cursor += 2;
      WZ3__RT_REQUIRE_STACK();
      const double value = WZ3__RT_TOP()[0];
      if (index >= rt->n_splines) return WZ3_ERR_OPERATOR;
      wz3__rt_dspline_eval(&rt->splines[index], value, WZ3__RT_TOP());
      break;
    }
    case 0x1a: {
      WZ3__RT_REQUIRE_STACK();
      const double value = WZ3__RT_TOP()[0];
      if (env->event_spline >= 0) wz3__rt_dspline_eval(&rt->splines[env->event_spline], value, WZ3__RT_TOP());
      else WZ3__RT_TOP()[0] = WZ3__RT_TOP()[1] = WZ3__RT_TOP()[2] = WZ3__RT_TOP()[3] = 0.0f;
      break;
    }
    case 0x1b: {
      WZ3__RT_REQUIRE_STACK();
      float matrix[16];
      for (int i = 0; i < 4; i++) memcpy(matrix + i * 4, env->vars[WZ3__RT_KV_MATRIX_I + i], 4 * sizeof(float));
      const double x = WZ3__RT_TOP()[0], y = WZ3__RT_TOP()[1], z = WZ3__RT_TOP()[2], w = WZ3__RT_TOP()[3];
      WZ3__RT_TOP()[0] = (float)((double)matrix[0] * x + (double)matrix[4] * y + (double)matrix[8] * z + (double)matrix[12] * w);
      WZ3__RT_TOP()[1] = (float)((double)matrix[1] * x + (double)matrix[5] * y + (double)matrix[9] * z + (double)matrix[13] * w);
      WZ3__RT_TOP()[2] = (float)((double)matrix[2] * x + (double)matrix[6] * y + (double)matrix[10] * z + (double)matrix[14] * w);
      WZ3__RT_TOP()[3] = (float)((double)matrix[3] * x + (double)matrix[7] * y + (double)matrix[11] * z + (double)matrix[15] * w);
      break;
    }
    case 0x1c: {
      WZ3__RT_REQUIRE_STACK();
      double value = fmod((double)WZ3__RT_TOP()[0], 1.0);
      if (value < 0) value += 1;
      const double scaled = value * 65536.0;                 /* Math.trunc(x) | 0: NaN and huge values become 0 */
      const uint32_t integer = (scaled > -2147483648.0 && scaled < 2147483648.0) ? (uint32_t)(int32_t)scaled : 0u;
      const uint32_t result = (uint32_t)rt->perm[(integer >> 8) & 255u] | ((uint32_t)rt->perm[integer & 255u] << 8);
      WZ3__RT_SCALAR(WZ3__RT_TOP(), (double)result / 65535.0);
      break;
    }
    case 0x1d: {
      WZ3__RT_NEED(8);
      double ease_in = wz3__rt_rd_f32(bytes + cursor), ease_out = wz3__rt_rd_f32(bytes + cursor + 4);
      cursor += 8;
      WZ3__RT_REQUIRE_STACK();
      if (ease_in > 0.5) ease_in = 0.5;
      if (ease_out > 0.5) ease_out = 0.5;
      ease_out = 1 - ease_out;
      double distance = ease_out - ease_in;
      if (distance < 0.1) {
        ease_out += (0.1 - distance) / 2;
        ease_in -= (0.1 - distance) / 2;
        distance = ease_out - ease_in;
      }
      const double scale = 1 / distance;
      float *value = WZ3__RT_TOP();
      for (int i = 0; i < 4; i++) {
        double x = value[i], a, c;
        if (x < 0) x = -ease_in * scale / 2;
        else if (x < ease_in) { a = -ease_in * scale / 2; c = scale / (2 * ease_in); x = a + x * x * c; }
        else if (x < ease_out) x = (x - ease_in) * scale;
        else if (x < 1) { a = -(1 - ease_out) * scale / 2; c = scale / (2 * (1 - ease_out)); x = 1 - a - (1 - x) * (1 - x) * c; }
        else x = 1 + (1 - ease_out) * scale / 2;
        value[i] = (float)((x + ease_in * scale / 2) / (1 + ease_in * scale / 2 + (1 - ease_out) * scale / 2));
      }
      break;
    }
    case 0x1e: {
      WZ3__RT_NEED(2);
      const double t0 = bytes[cursor] / 256.0, t1 = bytes[cursor + 1] / 256.0;
      cursor += 2;
      WZ3__RT_REQUIRE_STACK();
      float *value = WZ3__RT_TOP();
      for (int i = 0; i < 4; i++)
        value[i] = value[i] <= t0 ? 0.0f : value[i] >= t1 ? 1.0f : (float)(((double)value[i] - t0) / (t1 - t0));
      break;
    }
    case 0x20:
      WZ3__RT_REQUIRE_STACK();
      for (int i = 0; i < 4; i++) WZ3__RT_TOP()[i] = (float)(wz3__log((double)WZ3__RT_TOP()[i]) * 0.6931471805599453);
      break;
    case 0x21:
      WZ3__RT_REQUIRE_STACK();
      for (int i = 0; i < 4; i++) WZ3__RT_TOP()[i] = (float)wz3_pow(2.0, (double)WZ3__RT_TOP()[i]);
      break;
    case 0x80: {
      WZ3__RT_NEED(1);
      const uint32_t index = bytes[cursor++];
      WZ3__RT_REQUIRE_STACK();
      float value[4];
      memcpy(value, stack[--sp], sizeof value);
      if (index >= WZ3__RT_MAX_VARS || env->n_saves >= WZ3__RT_MAX_SAVES) return WZ3_ERR_OPERATOR;
      env->saves[env->n_saves].index = (uint8_t)index;
      memcpy(env->saves[env->n_saves].value, env->vars[index], sizeof value);
      env->n_saves++;
      pop_count++;
      for (int i = 0; i < 4; i++)
        if (command & (1u << i)) env->vars[index][i] = value[i];
      break;
    }
    case 0x90: case 0xd0: case 0xa0: case 0xe0: case 0xb0: case 0xf0: {
      WZ3__RT_NEED(1);
      const uint32_t index = bytes[cursor++];
      WZ3__RT_REQUIRE_STACK();
      float value[4];
      memcpy(value, stack[--sp], sizeof value);
      int did_change = 0;
      for (int i = 0; i < 4; i++) {
        if (!(command & (1u << i))) continue;
        if (base == 0xb0 || base == 0xf0) {
          const uint32_t at = index * 4u + (uint32_t)i;
          if (at >= (uint32_t)op->data_words * 4u) return WZ3_ERR_OPERATOR;
          int32_t next = wz3__rt_trunc_int((double)value[i] * 255.0);
          next = next >= 255 ? 255 : next <= 0 ? 0 : next;
          uint8_t *byte = (uint8_t *)op->anim + at;
          if (*byte != next) did_change = 1;
          *byte = (uint8_t)next;
        } else {
          const uint32_t at = index + (uint32_t)i;
          if (at >= op->data_words) return WZ3_ERR_OPERATOR;
          if (base == 0xa0 || base == 0xe0) {
            const int32_t next = wz3__rt_trunc_int(value[i]);
            if (op->anim[at].i != next) did_change = 1;
            op->anim[at].i = next;
          } else {
            const float next = value[i];
            if (op->anim[at].f != next) did_change = 1;
            op->anim[at].f = next;
          }
        }
      }
      if (base == 0xd0 || base == 0xe0 || base == 0xf0) changed = changed || did_change;
      break;
    }
    default: return WZ3_ERR_OPERATOR;
    }
    if (sp > 256) return WZ3_ERR_OPERATOR;
  }
#undef WZ3__RT_NEED
#undef WZ3__RT_TOP
#undef WZ3__RT_REQUIRE_STACK
#undef WZ3__RT_SCALAR
}

/* ---- execution ---- */

static wz3_result wz3__rt_exec_op(wz3_rt *rt, uint32_t index);

static wz3_result wz3__rt_exec_inputs(wz3_rt *rt, wz3__rt_op *op)
{
  for (uint32_t i = 0; i < op->src->n_inputs; i++) {
    const wz3_result r = wz3__rt_exec_op(rt, op->src->inputs[i]);
    if (r != WZ3_OK) return r;
  }
  if (!(op->convention & WZ3_OPC_DONTCALLLINK)) {
    for (uint32_t i = 0; i < op->src->n_links; i++) {
      if (op->src->links[i] < 0) continue;
      const wz3_result r = wz3__rt_exec_op(rt, (uint32_t)op->src->links[i]);
      if (r != WZ3_OK) return r;
    }
  }
  return WZ3_OK;
}

/* Op.execEvent: runs `op_index` with the event's variables, spline, matrix and instance chain */
static wz3_result wz3__rt_exec_event(wz3_rt *rt, uint32_t op_index, uint32_t ev)
{
  wz3__rt_env *env = &rt->env;
  wz3__rt_event *event = &rt->events[ev];
  float saved[8][4];
  memcpy(saved, env->vars, sizeof saved);
  const int32_t old_spline = env->event_spline, old_event = env->current_event;
  double time = 0.0;
  if (event->start < event->end)
    time = (float)(((double)env->beat_time - (double)event->start) / ((double)event->end - (double)event->start));
  time = wz3__rt_clamp(time, 1.0, 0.0);
  time = (float)((double)event->start_interval + time * ((double)event->end_interval - (double)event->start_interval));
  if (!(event->flags & WZ3__RT_KEF_NOTIME))
    env->vars[0][0] = env->vars[0][1] = env->vars[0][2] = env->vars[0][3] = (float)time;
  env->vars[1][0] = env->vars[1][1] = env->vars[1][2] = env->vars[1][3] = event->velocity;
  env->vars[2][0] = env->vars[2][1] = env->vars[2][2] = env->vars[2][3] = event->modulation;
  env->vars[3][0] = env->vars[3][1] = env->vars[3][2] = env->vars[3][3] = (float)event->select;
  memcpy(env->vars[4], event->scale, sizeof event->scale);
  memcpy(env->vars[5], event->rotate, sizeof event->rotate);
  memcpy(env->vars[6], event->translate, sizeof event->translate);
  const uint32_t color = event->color;
  env->vars[7][0] = (float)((double)((color >> 16) & 255u) / 255.0);
  env->vars[7][1] = (float)((double)((color >> 8) & 255u) / 255.0);
  env->vars[7][2] = (float)((double)(color & 255u) / 255.0);
  env->vars[7][3] = (float)((double)((color >> 24) & 255u) / 255.0);
  if (event->spline >= 0) env->event_spline = event->spline;
  env->current_event = (int32_t)ev;
  wz3_result r = wz3__rt_stack_push_mul(env, event->matrix);
  if (r == WZ3_OK) {
    wz3__rt_chain *previous_chain = env->chain;
    const uint32_t previous_cursor = env->cursor;
    env->chain = &event->instances;
    env->cursor = 0;
    r = wz3__rt_exec_op(rt, op_index);
    env->chain = previous_chain;
    env->cursor = previous_cursor;
    wz3__rt_stack_pop(env);
  }
  env->event_spline = old_spline;
  env->current_event = old_event;
  memcpy(env->vars, saved, sizeof saved);
  return r;
}

/* Exec_Misc_Event */
static wz3_result wz3__rt_exec_misc_event(wz3_rt *rt, wz3__rt_op *op)
{
  wz3__rt_env *env = &rt->env;
  const double duration = wz3__rt_anim_param(op, 0);
  for (int32_t ev = op->first_event; ev >= 0; ev = rt->events[ev].next_op) {
    wz3__rt_event *event = &rt->events[ev];
    if (event->start == event->end) {
      event->start = env->beat_time;
      event->end = (int32_t)((uint32_t)event->start + (uint32_t)wz3__rt_round_even((float)(duration * 65536.0)));
    }
    if (!op->src->n_inputs) return WZ3_ERR_OPERATOR;
    const wz3_result r = wz3__rt_exec_event(rt, op->src->inputs[0], (uint32_t)ev);
    if (r != WZ3_OK) return r;
  }
  return WZ3_OK;
}

/* Exec_Misc_EventTime */
static wz3_result wz3__rt_exec_misc_event_time(wz3_rt *rt, wz3__rt_op *op)
{
  wz3__rt_env *env = &rt->env;
  wz3__rt_instance *memory;
  wz3_result r = wz3__rt_chain_instance(env, op->id, &memory);
  if (r != WZ3_OK) return r;
  if (memory->reset) memory->words[0] = 0x7fffffff;
  if (op->first_event >= 0) {
    const wz3__rt_event *event = &rt->events[op->first_event];
    if (!op->src->n_inputs) return WZ3_ERR_OPERATOR;
    r = wz3__rt_exec_event(rt, op->src->inputs[0], (uint32_t)op->first_event);
    memory->words[0] = event->end;
    return r;
  }
  float saved[4];
  memcpy(saved, env->vars[0], sizeof saved);
  env->vars[0][0] = env->vars[0][1] = env->vars[0][2] = env->vars[0][3] = env->beat_time > memory->words[0] ? 1.0f : 0.0f;
  r = wz3__rt_exec_inputs(rt, op);
  memcpy(env->vars[0], saved, sizeof saved);
  return r;
}

/* Exec_Misc_Demo: demo inputs run, IPP branches run (their output is the renderer's business) */
static wz3_result wz3__rt_exec_misc_demo(wz3_rt *rt, wz3__rt_op *op)
{
  for (uint32_t i = 0; i < op->src->n_inputs; i++) {
    const wz3__rt_op *input = &rt->ops[op->src->inputs[i]];
    if (input->cache.kind == WZ3__RC_NONE) continue;
    if (input->cache.class_id == WZ3_KC_DEMO) {
      const wz3_result r = wz3__rt_exec_op(rt, op->src->inputs[i]);
      if (r != WZ3_OK) return r;
    } else if (input->cache.class_id == WZ3_KC_IPP) {
      if (rt->options.frame_jobs) wz3__fj_ipp_branch_begin(rt);
      wz3_result r = wz3__rt_exec_op(rt, op->src->inputs[i]);
      if (r == WZ3_OK && rt->options.frame_jobs) r = wz3__fj_ipp_branch_end(rt);
      if (r != WZ3_OK) return r;
    }
  }
  return WZ3_OK;
}

static wz3_result wz3__rt_exec_op(wz3_rt *rt, uint32_t index)
{
  wz3__rt_op *op = &rt->ops[index];
  if (op->out_class == WZ3_KC_BITMAP) return WZ3_OK;
  if (op->executing) return WZ3_ERR_OPERATOR;                /* operator execution cycle */
  op->executing = 1;
  uint32_t pops = 0;
  wz3_result r = wz3__rt_exec_animation(rt, op, &pops);
  if (r == WZ3_OK) {
    int handled = 0;
    if (rt->options.frame_jobs) r = wz3__fj_exec(rt, op, &handled);
    if (!handled && r == WZ3_OK) {
      switch (op->class_id) {
      case 0x06: r = wz3__rt_exec_misc_event(rt, op); break;
      case 0x0d: r = wz3__rt_exec_misc_demo(rt, op); break;
      case 0x16: r = wz3__rt_exec_misc_event_time(rt, op); break;
      case 0x18: case 0x19: case 0x1a: case 0x1c: r = wz3__rt_exec_inputs(rt, op); break;
      default: r = rt->options.generic_exec ? wz3__rt_exec_inputs(rt, op) : WZ3_OK; break;
      }
    }
  }
  const wz3_result popped = wz3__rt_env_pop(&rt->env, pops);
  op->executing = 0;
  return r != WZ3_OK ? r : popped;
}

/* ---- init pass (precalc) ---- */

