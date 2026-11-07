/* ======================================================================== */
/*  effects (geneffectdebris.cpp) and the class dispatch of the frame jobs  */
/* ======================================================================== */

#define WZ3__FJ_CHAIN_POINTS 32

typedef struct wz3__fj_wave { double x, z; float values[1024]; } wz3__fj_wave;

typedef struct wz3__fj_water {
  wz3_water_geometry pub;
  wz3__fj_wave *waves;
  uint32_t cap_waves;
  float *last_row;
  double layout_scale, layout_uv_scale;
  int laid_out;
} wz3__fj_water;

typedef struct wz3__fj_effect {
  wz3_effect pub;
  int has_passes;
  wz3__rt_chain scene_instances;       /* op.sceneInstances: the private SceneMemLink tape of a deferred effect */
  wz3__fj_water *water;
} wz3__fj_effect;

typedef struct wz3__fj_line_point { float old[3], position[3], next[3]; } wz3__fj_line_point;
typedef struct wz3__fj_line_mem {
  wz3__fj_line_point lines[WZ3__FJ_CHAIN_POINTS];
  int32_t ripped, collision_axis, first_cycles, pad;
  double time_counter;
  float old_a[3], old_b[3];
} wz3__fj_line_mem;

/* ---- JavaScript Math.min / Math.max (NaN propagates, -0 < +0) ---- */

static double wz3__js_min(double a, double b)
{
  if (a != a || b != b) return a + b;
  if (a == 0.0 && b == 0.0) return (1.0 / a < 0.0) ? a : b;
  return a < b ? a : b;
}
static double wz3__js_max(double a, double b)
{
  if (a != a || b != b) return a + b;
  if (a == 0.0 && b == 0.0) return (1.0 / a > 0.0) ? a : b;
  return a > b ? a : b;
}

/* x**3 rounded once: V8's Math.pow is correctly rounded for this exponent in all but rare cases, and the result is
   rounded to a float right after, so Dekker products stand in for it */
static double wz3__fj_cube(double x)
{
  const double c = 134217729.0;
  double t = c * x, xh = t - (t - x), xl = x - xh;
  const double p = x * x;
  const double e = ((xh * xh - p) + 2.0 * xh * xl) + xl * xl;           /* x*x = p + e exactly */
  t = c * p;
  const double ph = t - (t - p), pl = p - ph;
  const double q = p * x;
  const double f = ((ph * xh - q) + ph * xl + pl * xh) + pl * xl;       /* p*x = q + f exactly */
  return q + (f + e * x);
}

/* The perlin3D of the JavaScript port: the player's sPerlin3D without the float rounding of every step */
static void wz3__fj_perlin3d(const uint8_t perm[512], const float position[3], float out[3])
{
  int32_t integer[3];
  double fade[3];
  for (int axis = 0; axis < 3; axis++) {
    const double v = (double)position[axis];
    const int32_t base = wz3__rt_round_even(v - 0.5);
    double fraction = wz3__js_max(0.0, wz3__js_min(1.0, v - (double)base));
    integer[axis] = (int32_t)((uint32_t)base & 255u);
    fraction = wz3__fj_cube(fraction) * (10 + fraction * (6 * fraction - 15));
    fade[axis] = (double)(float)fraction;
  }
#define WZ3__FJ_GRAD(x, y, z) (wz3__rt_gradients + (perm[perm[perm[(x)] + (y)] + (z)] & 15) * 4)
#define WZ3__FJ_LERP3(o, a, b, t)                                                                \
  do {                                                                                           \
    (o)[0] = (float)((double)(a)[0] + ((double)(b)[0] - (double)(a)[0]) * (t));                  \
    (o)[1] = (float)((double)(a)[1] + ((double)(b)[1] - (double)(a)[1]) * (t));                  \
    (o)[2] = (float)((double)(a)[2] + ((double)(b)[2] - (double)(a)[2]) * (t));                  \
  } while (0)
  const int32_t x = integer[0], y = integer[1], z = integer[2];
  float t0[3], ty[3], t1[3], t2[3];
  WZ3__FJ_LERP3(t0, WZ3__FJ_GRAD(x, y, z), WZ3__FJ_GRAD(x + 1, y, z), fade[0]);
  WZ3__FJ_LERP3(ty, WZ3__FJ_GRAD(x, y + 1, z), WZ3__FJ_GRAD(x + 1, y + 1, z), fade[0]);
  WZ3__FJ_LERP3(t0, t0, ty, fade[1]);
  WZ3__FJ_LERP3(t1, WZ3__FJ_GRAD(x, y, z + 1), WZ3__FJ_GRAD(x + 1, y, z + 1), fade[0]);
  WZ3__FJ_LERP3(t2, WZ3__FJ_GRAD(x, y + 1, z + 1), WZ3__FJ_GRAD(x + 1, y + 1, z + 1), fade[0]);
  WZ3__FJ_LERP3(t1, t1, t2, fade[1]);
  WZ3__FJ_LERP3(out, t0, t1, fade[2]);
#undef WZ3__FJ_GRAD
}

/* ---- ChainLine ---- */

static void wz3__fj_line_initialize(wz3__fj_line_mem *m, const float a[3], const float b[3], double segment, uint32_t flags)
{
  const double difference[3] = {(double)a[0] - b[0], (double)a[1] - b[1], (double)a[2] - b[2]};
  if (flags & 4u) {
    for (int i = 0; i < WZ3__FJ_CHAIN_POINTS; i++) {
      float *position = m->lines[i].position;
      position[0] = a[0];
      position[1] = (float)((double)a[1] - (double)i * segment);
      position[2] = a[2];
      memcpy(m->lines[i].old, position, 3 * sizeof(float));
    }
    m->ripped = WZ3__FJ_CHAIN_POINTS - 1;
  }
  const double ax = difference[0] < 0 ? -difference[0] : difference[0];
  const double ay = difference[1] < 0 ? -difference[1] : difference[1];
  const double az = difference[2] < 0 ? -difference[2] : difference[2];
  m->collision_axis = ax > ay ? (ax > az ? 0 : 2) : (ay > az ? 1 : 2);
  m->time_counter = 0;
  m->first_cycles = 250;
  memcpy(m->old_a, a, 3 * sizeof(float));
  memcpy(m->old_b, b, 3 * sizeof(float));
}

static void wz3__fj_line_catenary(wz3__fj_line_mem *m, const float a[3], const float b[3], double segment)
{
  const double dx = (double)a[0] - b[0], dy = (double)a[1] - b[1], dz = (double)a[2] - b[2];
  const double delta = dx * dx + dy * dy + dz * dz;
  const double reach = segment * (WZ3__FJ_CHAIN_POINTS - 1);
  double height = reach * reach - delta;
  height = height <= 0 ? 0 : sqrt(height) * 0.4;
  for (int i = 0; i < WZ3__FJ_CHAIN_POINTS; i++) {
    const double n = (double)i / (WZ3__FJ_CHAIN_POINTS - 1);
    const double mm = ((double)i - (WZ3__FJ_CHAIN_POINTS - 1) / 2.0) / ((WZ3__FJ_CHAIN_POINTS - 1) / 2.0);
    float *position = m->lines[i].position;
    position[0] = (float)((double)a[0] + ((double)b[0] - (double)a[0]) * n);
    position[1] = (float)wz3__js_max(0.0, (double)a[1] + ((double)b[1] - (double)a[1]) * n - (1 - mm * mm) * height);
    position[2] = (float)((double)a[2] + ((double)b[2] - (double)a[2]) * n);
    memcpy(m->lines[i].old, position, 3 * sizeof(float));
  }
  m->ripped = -1;
}

static wz3_result wz3__fj_exec_chain_line(wz3_rt *rt, wz3__rt_op *op)
{
  wz3__rt_env *env = &rt->env;
  wz3__fj *fj = &rt->fj;
  const int32_t marker_a = wz3__fj_pi(op, 6), marker_b = wz3__fj_pi(op, 7);
  if (marker_a < 0 || marker_a >= 32 || marker_b < 0 || marker_b >= 32) return WZ3_ERR_OPERATOR;
  float va[4], vb[4], a4[4], b4[4];
  va[0] = (float)wz3__fj_p(op, 0); va[1] = (float)wz3__fj_p(op, 1); va[2] = (float)wz3__fj_p(op, 2); va[3] = 1.0f;
  vb[0] = (float)wz3__fj_p(op, 3); vb[1] = (float)wz3__fj_p(op, 4); vb[2] = (float)wz3__fj_p(op, 5); vb[3] = 1.0f;
  wz3_mat4_transform_point(fj->markers[marker_a], va, a4);
  wz3_mat4_transform_point(fj->markers[marker_b], vb, b4);
  const float *a = a4, *b = b4;
  const double segment = wz3__fj_p(op, 8) / (WZ3__FJ_CHAIN_POINTS - 1);
  const double gravity = wz3__fj_p(op, 10) * wz3__fj_p(op, 10);
  const uint32_t flags = wz3__fj_pu(op, 16);
  const double p11 = wz3__fj_p(op, 11), p12 = wz3__fj_p(op, 12), p13 = wz3__fj_p(op, 13), p14 = wz3__fj_p(op, 14);
  const double p15 = wz3__fj_p(op, 15), p17 = wz3__fj_p(op, 17), p18 = wz3__fj_p(op, 18), p19 = wz3__fj_p(op, 19);
  wz3__rt_instance *inst;
  wz3_result r = wz3__rt_chain_instance(env, op->id, &inst);
  if (r != WZ3_OK) return r;
  int reset = inst->reset;
  if (!inst->blob) {
    inst->blob = WZ3_MALLOC(sizeof(wz3__fj_line_mem));
    if (!inst->blob) return WZ3_ERR_OUT_OF_MEMORY;
    memset(inst->blob, 0, sizeof(wz3__fj_line_mem));
    inst->blob_size = sizeof(wz3__fj_line_mem);
    ((wz3__fj_line_mem *)inst->blob)->ripped = -1;
    reset = 1;
  }
  if (inst->blob_size != sizeof(wz3__fj_line_mem)) return WZ3_ERR_FORMAT;
  wz3__fj_line_mem *mem = (wz3__fj_line_mem *)inst->blob;
  if (reset) wz3__fj_line_initialize(mem, a, b, segment, flags);
  if ((reset && !(flags & 4u)) || (flags & 8u)) wz3__fj_line_catenary(mem, a, b, segment);

  const int64_t timeslices = env->time_slices < 0 ? 0 : env->time_slices;
  const int64_t ticks = timeslices * 10;
  if (!(flags & 8u)) {
    for (int64_t tick = 0; tick < ticks; tick++) {
      const double interpolation = (double)(tick + 1) / (double)ticks;
      float end_a[3], end_b[3], wind_position[3], wind[3];
      for (int k = 0; k < 3; k++) {
        end_a[k] = (float)((double)mem->old_a[k] + ((double)a[k] - (double)mem->old_a[k]) * interpolation);
        end_b[k] = (float)((double)mem->old_b[k] + ((double)b[k] - (double)mem->old_b[k]) * interpolation);
      }
      const int axis = mem->collision_axis;
      const double collision_min = wz3__js_min(end_a[axis], end_b[axis]);
      const double collision_max = wz3__js_max(end_a[axis], end_b[axis]);
      const double wind_time = (mem->time_counter + (double)tick) * p14;
      wind_position[0] = (float)wind_time;
      wind_position[1] = (float)(wind_time * 1.1);
      wind_position[2] = (float)(wind_time * 1.2);
      wz3__fj_perlin3d(rt->perm, wind_position, wind);
      wind[0] = (float)((double)wind[0] * p15 + p17);
      wind[1] = (float)((double)wind[1] * p15 * 0.5 + p18);
      wind[2] = (float)((double)wind[2] * p15 + p19);
      const double wind_d[3] = {wind[0], wind[1], wind[2]};
      float wind_direction[3];
      wz3__rt_normalize3(wind_d, wind_direction);

      for (int i = 0; i < WZ3__FJ_CHAIN_POINTS; i++) {
        wz3__fj_line_point *line = &mem->lines[i];
        const double damping = mem->first_cycles == 0 ? p11 : 0.975;
        for (int k = 0; k < 3; k++)
          line->next[k] = (float)((double)line->position[k] + (double)line->position[k] * damping - (double)line->old[k] * damping);
        line->next[1] = (float)((double)line->next[1] - gravity);
      }
      for (int i = 0; i < WZ3__FJ_CHAIN_POINTS - 1; i++) {
        wz3__fj_line_point *first = &mem->lines[i], *second = &mem->lines[i + 1];
        const double dx = (float)((double)second->position[0] - first->position[0]);
        const double dy = (float)((double)second->position[1] - first->position[1]);
        const double dz = (float)((double)second->position[2] - first->position[2]);
        const double cross_x = (float)(dy * wind_direction[2] - dz * wind_direction[1]);
        const double cross_y = (float)(dz * wind_direction[0] - dx * wind_direction[2]);
        const double cross_z = (float)(dx * wind_direction[1] - dy * wind_direction[0]);
        const double wind_force = sqrt(cross_x * cross_x + cross_y * cross_y + cross_z * cross_z) * 0.0001;
        for (int k = 0; k < 3; k++) first->next[k] = (float)((double)first->next[k] + (double)wind[k] * wind_force);
        if (i != mem->ripped) {
          const double length = sqrt(dx * dx + dy * dy + dz * dz);
          const double inverse = length > 1e-10 ? 1.0 / length : 0.0;
          const double direction[3] = {length > 1e-10 ? (double)(float)(dx * inverse) : 1.0,
                                       length > 1e-10 ? (double)(float)(dy * inverse) : 0.0,
                                       length > 1e-10 ? (double)(float)(dz * inverse) : 0.0};
          if (length > segment)
            for (int k = 0; k < 3; k++) {
              const double force = direction[k] * (length - segment) * p12;
              first->next[k] = (float)((double)first->next[k] + force);
              second->next[k] = (float)((double)second->next[k] - force);
            }
        }
      }
      for (int i = 0; i < WZ3__FJ_CHAIN_POINTS; i++) {
        wz3__fj_line_point *line = &mem->lines[i];
        if (line->next[1] < 0) line->next[1] = 0;
        if ((flags & 3u) == 1) {
          if ((double)line->next[axis] < collision_min) {
            line->position[axis] = (float)((double)line->position[axis] - ((double)line->next[axis] - collision_min));
            line->next[axis] = (float)collision_min;
          }
          if ((double)line->next[axis] > collision_max) {
            line->position[axis] = (float)((double)line->position[axis] - ((double)line->next[axis] - collision_max));
            line->next[axis] = (float)collision_max;
          }
        } else if ((flags & 3u) == 2) {
          line->next[axis] = (float)wz3__js_max(collision_min, wz3__js_min(collision_max, (double)line->next[axis]));
        }
      }
      memcpy(mem->lines[0].next, end_a, sizeof end_a);
      if (!(flags & 4u)) memcpy(mem->lines[WZ3__FJ_CHAIN_POINTS - 1].next, end_b, sizeof end_b);
      if (mem->ripped == -1) {
        const double dx = (double)end_a[0] - end_b[0], dy = (double)end_a[1] - end_b[1], dz = (double)end_a[2] - end_b[2];
        if (dx * dx + dy * dy + dz * dz > p13 * p13) mem->ripped = wz3__fj_pi(op, 20);
      }
      for (int i = 0; i < WZ3__FJ_CHAIN_POINTS; i++) {
        memcpy(mem->lines[i].old, mem->lines[i].position, 3 * sizeof(float));
        memcpy(mem->lines[i].position, mem->lines[i].next, 3 * sizeof(float));
      }
      if (mem->first_cycles > 0) mem->first_cycles--;
    }
  }
  mem->time_counter += (double)ticks;
  memcpy(mem->old_a, a, 3 * sizeof(float));
  memcpy(mem->old_b, b, 3 * sizeof(float));

  if (op->cache.kind == WZ3__RC_EFFECT && ((const wz3__fj_effect *)op->cache.obj)->has_passes) {
    const wz3__fj_effect *effect = (const wz3__fj_effect *)op->cache.obj;
    wz3_effect_output *out = &fj->out;
    memset(out, 0, sizeof *out);
    out->kind = WZ3_EFFECT_OUT_CHAIN_LINE;
    out->op = op->id;
    out->material_op = effect->pub.material_op;
    wz3_mat4_identity(out->matrix);
    for (int i = 0; i < WZ3__FJ_CHAIN_POINTS; i++) memcpy(fj->line_points + i * 3, mem->lines[i].position, 3 * sizeof(float));
    out->points = fj->line_points;
    out->thickness = wz3__fj_p(op, 9);
    out->ripped = mem->ripped;
    out->color = 0xff404040u;
    out->spline_op = -1;
  }
  return WZ3_OK;
}

/* ---- Water ---- */

static void wz3__fj_water_free(wz3__fj_water *w)
{
  if (!w) return;
  WZ3_FREE(w->pub.positions);
  WZ3_FREE(w->pub.normals);
  WZ3_FREE(w->pub.uvs);
  WZ3_FREE(w->pub.indices);
  WZ3_FREE(w->waves);
  WZ3_FREE(w->last_row);
  WZ3_FREE(w);
}

static wz3__fj_water *wz3__fj_water_create(int32_t grid)
{
  wz3__fj_water *w = (wz3__fj_water *)WZ3_MALLOC(sizeof *w);
  if (!w) return NULL;
  memset(w, 0, sizeof *w);
  const int32_t width = 1 << grid;
  const uint32_t vertices = (uint32_t)width * (uint32_t)width;
  const uint32_t n_indices = (uint32_t)(width - 1) * (uint32_t)(width - 1) * 6u;
  w->pub.grid = grid; w->pub.width = w->pub.height = width;
  w->pub.n_vertices = vertices; w->pub.n_indices = n_indices;
  w->pub.positions = (float *)WZ3_MALLOC((size_t)vertices * 3 * sizeof(float));
  w->pub.normals = (float *)WZ3_MALLOC((size_t)vertices * 3 * sizeof(float));
  w->pub.uvs = (float *)WZ3_MALLOC((size_t)vertices * 2 * sizeof(float));
  w->pub.indices = (uint16_t *)WZ3_MALLOC(((size_t)n_indices + 1) * sizeof(uint16_t));
  w->last_row = (float *)WZ3_MALLOC((size_t)width * sizeof(float));
  if (!w->pub.positions || !w->pub.normals || !w->pub.uvs || !w->pub.indices || !w->last_row) { wz3__fj_water_free(w); return NULL; }
  memset(w->pub.positions, 0, (size_t)vertices * 3 * sizeof(float));
  memset(w->pub.normals, 0, (size_t)vertices * 3 * sizeof(float));
  memset(w->pub.uvs, 0, (size_t)vertices * 2 * sizeof(float));
  uint32_t at = 0;
  for (int32_t z = 0; z < width - 1; z++)
    for (int32_t x = 0; x < width - 1; x++) {
      const int32_t i = z * width + x;
      w->pub.indices[at++] = (uint16_t)(i + width); w->pub.indices[at++] = (uint16_t)(i + 1 + width); w->pub.indices[at++] = (uint16_t)(i + 1);
      w->pub.indices[at++] = (uint16_t)(i + width); w->pub.indices[at++] = (uint16_t)(i + 1); w->pub.indices[at++] = (uint16_t)i;
    }
  return w;
}

/* buildWaterGeometry */
static wz3_result wz3__fj_water_build(const wz3__rt_op *op, const wz3__rt_cache *spline, double time, wz3__fj_effect *effect)
{
  int32_t grid = wz3__fj_pi(op, 5);
  grid = grid < 0 ? 0 : grid > 8 ? 8 : grid;
  if (!effect->water || effect->water->pub.grid != grid) {
    wz3__fj_water *fresh = wz3__fj_water_create(grid);
    if (!fresh) return WZ3_ERR_OUT_OF_MEMORY;
    wz3__fj_water_free(effect->water);
    effect->water = fresh;
  }
  wz3__fj_water *w = effect->water;
  const int32_t width = w->pub.width, height = w->pub.height;
  float *positions = w->pub.positions, *normals = w->pub.normals, *uvs = w->pub.uvs;
  const double scale = wz3__fj_or0(wz3__fj_p(op, 0));
  const double scale_position = wz3__fj_or0(wz3__fj_p(op, 1));
  const double scale_uv = wz3__fj_or0(wz3__fj_p(op, 2));
  const double speed = (float)(wz3__fj_or0(wz3__fj_p(op, 3)) * 32);
  const double scale_amplitude = (float)wz3__fj_or0(wz3__fj_p(op, 4));
  const wz3__rt_blob *blob = (spline && spline->kind == WZ3__RC_BLOB) ? (const wz3__rt_blob *)spline->obj : NULL;
  uint32_t wave_count = 0;
  for (uint32_t k = 0; blob && k < blob->n_keys; k++) {
    const wz3__rt_blob_key *key = &blob->keys[k];
    if ((double)key->time > time) continue;
    const double delta_time = (float)((double)(float)time - (double)key->time);
    const double decay = (float)(1.0 / wz3_pow((double)key->f[5], delta_time));
    if (!(decay > 0.0001)) continue;
    if (wave_count == w->cap_waves) {
      const uint32_t cap = w->cap_waves ? w->cap_waves * 2 : 4;
      wz3__fj_wave *waves = (wz3__fj_wave *)WZ3_MALLOC((size_t)cap * sizeof *waves);
      if (!waves) return WZ3_ERR_OUT_OF_MEMORY;
      if (w->cap_waves) memcpy(waves, w->waves, (size_t)w->cap_waves * sizeof *waves);
      WZ3_FREE(w->waves);
      w->waves = waves;
      w->cap_waves = cap;
    }
    wz3__fj_wave *wave = &w->waves[wave_count];
    for (int index = 0; index < 1024; index++) {
      const double distance = (float)(((double)index / 1024 * 16) * (double)key->f[4]);
      const double phase = (float)((double)(float)(delta_time * speed) - (double)(float)(distance * (double)key->f[3]));
      double damping = (float)scale_amplitude;
      damping = (float)(damping * (double)(float)(1.0 / wz3_pow((double)key->f[5], phase)));
      damping = (float)(damping * (double)(float)(1.0 / wz3_pow(20.0, distance)));
      damping = (float)(damping * (double)key->f[6]);
      wave->values[index] = phase < 0 ? 0.0f : (float)(wz3__sin((double)(float)(phase * 3.14)) * damping);
    }
    wave->x = (double)key->f[0] * scale_position * (width - 1);
    wave->z = (double)key->f[2] * scale_position * (height - 1);
    wave_count++;
  }
  const double distance_scale = ((double)width * width + (double)height * height) / 1024;
  const double width_denominator = wz3__js_max(1, width - 1), height_denominator = wz3__js_max(1, height - 1);
  const int layout_changed = !w->laid_out || w->layout_scale != scale || w->layout_uv_scale != scale_uv;
  if (layout_changed) {
    for (int32_t z = 0; z < height; z++)
      for (int32_t x = 0; x < width; x++) {
        const size_t vertex = (size_t)z * width + x;
        positions[vertex * 3] = (float)(scale * (x - (width - 1) / 2.0) / width_denominator);
        positions[vertex * 3 + 2] = (float)(scale * (z - (height - 1) / 2.0) / height_denominator);
        uvs[vertex * 2] = (float)(x * scale_uv / width_denominator);
        uvs[vertex * 2 + 1] = (float)(z * scale_uv / height_denominator);
      }
    w->layout_scale = scale;
    w->layout_uv_scale = scale_uv;
    w->laid_out = 1;
  }
  memset(w->last_row, 0, (size_t)width * sizeof(float));
  double last_column = 0, minimum_y = (double)INFINITY, maximum_y = -(double)INFINITY;
  for (int32_t z = 0; z < height; z++)
    for (int32_t x = 0; x < width; x++) {
      double py = 0;
      for (uint32_t k = 0; k < wave_count; k++) {
        const wz3__fj_wave *wave = &w->waves[k];
        const double dx = wave->x - (x - (width - 1) / 2.0);
        const double dz = wave->z - (z - (height - 1) / 2.0);
        double distance = (dx * dx + dz * dz) / distance_scale;
        if (distance != distance) { py -= distance; continue; }
        int64_t index = distance < 4.0e18 ? (int64_t)distance : 4000000000000000000ll;
        distance -= (double)index;
        if (index > 1022) { index = 1022; distance = 0; }
        py -= (double)wave->values[index + 1] * distance + (double)wave->values[index] * (1 - distance);
      }
      const size_t vertex = (size_t)z * width + x;
      const float position_y = (float)(scale * py);
      positions[vertex * 3 + 1] = position_y;
      minimum_y = wz3__js_min(minimum_y, position_y);
      maximum_y = wz3__js_max(maximum_y, position_y);
      const double nx = py - last_column, ny = scale / width, nz = py - (double)w->last_row[x];
      const double length = sqrt(nx * nx + ny * ny + nz * nz);
      const double inverse = 1.0 / ((length != 0.0 && length == length) ? length : 1.0);
      normals[vertex * 3] = (float)(nx * inverse);
      normals[vertex * 3 + 1] = (float)(ny * inverse);
      normals[vertex * 3 + 2] = (float)(nz * inverse);
      last_column = py;
      w->last_row[x] = (float)py;
    }
  const double half_scale = (scale < 0 ? -scale : scale) * 0.5;
  const int finite = minimum_y == minimum_y && minimum_y - minimum_y == 0.0;
  w->pub.bounds_min[0] = (float)-half_scale; w->pub.bounds_min[1] = (float)(finite ? minimum_y : 0.0); w->pub.bounds_min[2] = (float)-half_scale;
  w->pub.bounds_max[0] = (float)half_scale; w->pub.bounds_max[1] = (float)(finite ? maximum_y : 0.0); w->pub.bounds_max[2] = (float)half_scale;
  w->pub.version++;
  w->pub.wave_count = wave_count;
  w->pub.uv_changed = (uint8_t)layout_changed;
  return WZ3_OK;
}

static void wz3__fj_post_output(wz3__fj *fj, const wz3__rt_op *op, uint8_t kind)
{
  wz3_effect_output *out = &fj->out;
  memset(out, 0, sizeof *out);
  out->kind = kind;
  out->op = op->id;
  out->material_op = -1;
  out->spline_op = -1;
  wz3_mat4_identity(out->matrix);
  out->n_params = op->data_words;
  for (uint32_t i = 0; i < op->data_words; i++) fj->post_params[i] = wz3__rt_anim_param(op, i);
  out->params = fj->post_params;
}

static wz3_result wz3__fj_exec_water(wz3_rt *rt, wz3__rt_op *op)
{
  wz3__fj *fj = &rt->fj;
  if (op->cache.kind != WZ3__RC_EFFECT) return WZ3_ERR_OPERATOR;
  wz3__fj_effect *effect = (wz3__fj_effect *)op->cache.obj;
  const wz3__rt_cache *spline = wz3__rt_input_cache(rt, op, 0);
  if (!effect->has_passes || !spline) return WZ3_OK;
  const double time = wz3__fj_or0(rt->env.vars[0][0]);
  wz3_result r = wz3__fj_water_build(op, spline, time, effect);
  if (r != WZ3_OK) return r;
  wz3__fj_post_output(fj, op, WZ3_EFFECT_OUT_WATER);
  fj->out.material_op = effect->pub.material_op;
  fj->out.spline_op = (int32_t)op->src->inputs[0];
  memcpy(fj->out.matrix, wz3__fj_top(&rt->env), sizeof fj->out.matrix);
  fj->out.time = (float)time;
  fj->out.water = &effect->water->pub;
  return WZ3_OK;
}

/* ---- effect objects ---- */

static void wz3__fj_effect_free(wz3__fj_effect *e)
{
  if (!e) return;
  wz3__rt_chain_free(&e->scene_instances);
  wz3__fj_water_free(e->water);
  WZ3_FREE(e);
}

static wz3_result wz3__fj_init_effect(wz3_rt *rt, wz3__rt_op *op, wz3__rt_cache *fresh)
{
  wz3__fj_effect *e = (wz3__fj_effect *)WZ3_MALLOC(sizeof *e);
  if (!e) return WZ3_ERR_OUT_OF_MEMORY;
  memset(e, 0, sizeof *e);
  e->pub.material_op = -1;
  e->pub.usage = WZ3_USAGE_OTHER;
  switch (op->class_id) {
  case 0x6b: e->pub.type = WZ3_EFFECT_CHAIN_LINE; break;
  case 0x73: e->pub.type = WZ3_EFFECT_WATER; break;
  case 0x74: e->pub.type = WZ3_EFFECT_GLARE; break;
  default: e->pub.type = WZ3_EFFECT_COLOR_CORRECTION; break;
  }
  if (op->class_id == 0x6b || op->class_id == 0x73) {
    const int32_t link = (!(op->convention & WZ3_OPC_DONTCALLLINK) && op->src->n_links) ? op->src->links[0] : -1;
    if (link >= 0 && (uint32_t)link < rt->n_ops && rt->ops[link].cache.kind == WZ3__RC_MATERIAL && rt->ops[link].cache.obj) {
      const wz3__fj_material *m = (const wz3__fj_material *)rt->ops[link].cache.obj;
      e->pub.material_op = link;
      e->has_passes = m->pub.n_passes > 0;
      if (m->pub.n_passes) { e->pub.pass = m->passes[0].render_pass; e->pub.usage = m->passes[0].usage; }
    }
  } else {
    e->pub.pass = wz3__fj_pi(op, 0);
    e->pub.usage = WZ3_USAGE_BASE;
    e->pub.need_current_render = op->class_id == 0x75;
  }
  fresh->kind = WZ3__RC_EFFECT; fresh->owned = 1; fresh->class_id = WZ3_KC_EFFECT; fresh->obj = e;
  return WZ3_OK;
}

WZ3_API const wz3_effect *wz3_rt_effect(const wz3_rt *rt, uint32_t op)
{
  if (!rt || op >= rt->n_ops || rt->ops[op].cache.kind != WZ3__RC_EFFECT) return NULL;
  return &((const wz3__fj_effect *)rt->ops[op].cache.obj)->pub;
}

/* ---- deferred execution (Engine::Paint) ---- */

WZ3_API wz3_result wz3_rt_effect_run(wz3_rt *rt, const wz3_effect_job *job, const wz3_effect_output **out)
{
  if (!rt || !job || !out || !rt->options.frame_jobs) return WZ3_ERR_ARGUMENT;
  wz3__fj *fj = &rt->fj;
  wz3__rt_env *env = &rt->env;
  *out = NULL;
  if (job->effect_op >= rt->n_ops || rt->ops[job->effect_op].cache.kind != WZ3__RC_EFFECT) return WZ3_ERR_ARGUMENT;
  if (job->markers >= fj->marker_tables.n || (size_t)job->variables + WZ3__RT_MAX_VARS * 4 > (size_t)fj->variables.n * (WZ3__RT_MAX_VARS * 4))
    return WZ3_ERR_ARGUMENT;
  wz3__fj_effect *effect = (wz3__fj_effect *)rt->ops[job->effect_op].cache.obj;
  float saved_vars[WZ3__RT_MAX_VARS][4], saved_markers[32][16];
  memcpy(saved_vars, env->vars, sizeof saved_vars);
  memcpy(env->vars, (const float *)fj->variables.p + job->variables, sizeof env->vars);
  memcpy(saved_markers, fj->markers, sizeof saved_markers);
  memcpy(fj->markers, (const float *)fj->marker_tables.p + (size_t)job->markers * 512, sizeof fj->markers);
  wz3_result r = wz3__fj_stack_push(env, job->matrix);
  if (r == WZ3_OK) {
    wz3__rt_chain *previous_chain = env->chain;
    const uint32_t previous_cursor = env->cursor;
    env->chain = &effect->scene_instances;
    env->cursor = 0;
    fj->out.kind = WZ3_EFFECT_OUT_NONE;
    r = wz3__rt_exec_op(rt, job->effect_op);
    env->chain = previous_chain;
    env->cursor = previous_cursor;
    wz3__rt_stack_pop(env);
  }
  memcpy(fj->markers, saved_markers, sizeof saved_markers);
  memcpy(env->vars, saved_vars, sizeof saved_vars);
  if (r == WZ3_OK) *out = &fj->out;
  return r;
}

static int wz3__fj_effect_usage_order(uint8_t usage)
{
  switch (usage) {
  case WZ3_USAGE_BASE: return 0;
  case WZ3_USAGE_PRELIGHT: return 1;
  case WZ3_USAGE_AMBIENT: return 2;
  case WZ3_USAGE_SHADOW: return 3;
  case WZ3_USAGE_LIGHT: return 4;
  case WZ3_USAGE_POSTLIGHT: return 5;
  case WZ3_USAGE_POSTLIGHT2: return 6;
  default: return 7;
  }
}

static int64_t wz3__fj_effect_key(const wz3_rt *rt, const wz3_effect_job *job)
{
  const wz3__fj_effect *e = (const wz3__fj_effect *)rt->ops[job->effect_op].cache.obj;
  int64_t pass = (int64_t)e->pub.pass + (int64_t)job->pass_adjust;
  pass = pass < 0 ? 0 : pass > 255 ? 255 : pass;
  return pass * 16 + wz3__fj_effect_usage_order(e->pub.usage);
}

/* Engine::AddPaintJob inserts at the head of its list and the radix sort is stable: reverse scene order, then a stable
   sort by (pass, usage) */
WZ3_API uint32_t wz3_frame_effect_order(const wz3_rt *rt, const wz3_ipp_node *viewport, uint32_t *order)
{
  if (!rt || !viewport || !order) return 0;
  const wz3_effect_job *jobs = (const wz3_effect_job *)rt->fj.effect.p + viewport->first_effect;
  const uint32_t n = viewport->n_effect;
  for (uint32_t i = 0; i < n; i++) {
    const uint32_t candidate = n - 1 - i;
    const int64_t key = wz3__fj_effect_key(rt, &jobs[candidate]);
    uint32_t at = i;
    while (at > 0 && wz3__fj_effect_key(rt, &jobs[order[at - 1]]) > key) { order[at] = order[at - 1]; at--; }
    order[at] = candidate;
  }
  return n;
}

static wz3_result wz3__fj_advance_node(wz3_rt *rt, uint32_t node_index, uint32_t *count)
{
  wz3__fj *fj = &rt->fj;
  wz3_ipp_node node = ((const wz3_ipp_node *)fj->nodes.p)[node_index];
  if (node.type == WZ3_IPP_LAYER2D) return node.input >= 0 ? wz3__fj_advance_node(rt, (uint32_t)node.input, count) : WZ3_OK;
  if (node.type != WZ3_IPP_VIEWPORT || !node.n_effect) return WZ3_OK;
  /* the order is kept in a runtime-owned buffer (nested layers reuse the tail), so a frame allocates nothing */
  if (fj->order.n + node.n_effect > fj->order.cap) {
    wz3__fj_vec grown = fj->order;
    void *unused;
    grown.p = NULL;                  /* a fresh block: wz3__fj_push releases the block it grows from, and that one is still in use below */
    grown.cap = 0;
    grown.n = 0;
    const wz3_result g = wz3__fj_push(&grown, sizeof(uint32_t), fj->order.n + node.n_effect, &unused);
    if (g != WZ3_OK) return g;
    if (fj->order.n) memcpy(grown.p, fj->order.p, (size_t)fj->order.n * sizeof(uint32_t));
    WZ3_FREE(fj->order.p);
    grown.n = fj->order.n;
    fj->order = grown;
  }
  uint32_t *order = (uint32_t *)fj->order.p + fj->order.n;
  fj->order.n += node.n_effect;
  wz3_frame_effect_order(rt, &node, order);
  wz3_result r = WZ3_OK;
  for (uint32_t i = 0; i < node.n_effect && r == WZ3_OK; i++) {
    const wz3_effect_job job = ((const wz3_effect_job *)fj->effect.p)[node.first_effect + order[i]];
    const wz3_effect_output *unused;
    r = wz3_rt_effect_run(rt, &job, &unused);
    (*count)++;
  }
  fj->order.n -= node.n_effect;
  return r;
}

WZ3_API wz3_result wz3_rt_effects_advance(wz3_rt *rt, uint32_t *n)
{
  if (!rt || !rt->options.frame_jobs) return WZ3_ERR_ARGUMENT;
  uint32_t count = 0;
  wz3_result r = WZ3_OK;
  wz3__fj *fj = &rt->fj;
  if (fj->outputs.n) {
    for (uint32_t i = 0; i < fj->outputs.n && r == WZ3_OK; i++) r = wz3__fj_advance_node(rt, ((const uint32_t *)fj->outputs.p)[i], &count);
  } else if (fj->last_output >= 0) {
    r = wz3__fj_advance_node(rt, (uint32_t)fj->last_output, &count);
  }
  if (n) *n = count;
  return r;
}

/* ---- class dispatch ---- */

static void wz3__fj_cache_free(uint8_t kind, void *obj)
{
  if (kind == WZ3__RC_MATERIAL) wz3__fj_material_free((wz3__fj_material *)obj);
  else if (kind == WZ3__RC_SCENE) wz3__fj_scene_free((wz3__fj_scene *)obj);
  else if (kind == WZ3__RC_EFFECT) wz3__fj_effect_free((wz3__fj_effect *)obj);
}

/* init pass: *handled is set for the classes of the frame jobs */
static wz3_result wz3__fj_init_op(wz3_rt *rt, wz3__rt_op *op, wz3__rt_cache *fresh, int *handled)
{
  *handled = 1;
  switch (op->class_id) {
  case 0xd0: case 0xd3: return wz3__fj_init_material(rt, op, fresh);
  case 0xc0: case 0xc1: case 0xc2: case 0xc3: case 0xc4: case 0xc5: case 0xd2: case 0x182: case 0x184:
    return wz3__fj_init_scene(rt, op, fresh);
  case 0x6b: case 0x73: case 0x74: case 0x75: return wz3__fj_init_effect(rt, op, fresh);
  case 0xf0: fresh->kind = WZ3__RC_IPP; fresh->class_id = WZ3_KC_IPP; return WZ3_OK;
  case 0xe9: {
    const int32_t link = (!(op->convention & WZ3_OPC_DONTCALLLINK) && op->src->n_links) ? op->src->links[0] : -1;
    if (link >= 0 && (uint32_t)link < rt->n_ops && rt->ops[link].cache.kind == WZ3__RC_MATERIAL) {
      fresh->kind = WZ3__RC_IPP; fresh->class_id = WZ3_KC_IPP;
    }
    return WZ3_OK;
  }
  default: *handled = 0; return WZ3_OK;
  }
}

/* exec pass: *handled is set for the classes of the frame jobs */
static wz3_result wz3__fj_exec(wz3_rt *rt, wz3__rt_op *op, int *handled)
{
  *handled = 1;
  switch (op->class_id) {
  case 0xd0: case 0xd3: return wz3__fj_exec_material(rt, op);
  case 0xc0: case 0xc3: return wz3__fj_exec_transformed(rt, op);
  case 0xc1: return wz3__fj_exec_scene_inputs(rt, op);
  case 0xc2: return wz3__fj_exec_multiply(rt, op);
  case 0xc4: return wz3__fj_exec_light(rt, op);
  case 0xc5: return wz3__fj_exec_particles(rt, op);
  case 0xd2: return wz3__rt_exec_inputs(rt, op);
  case 0x182: return wz3__fj_exec_marker(rt, op);
  case 0x184: wz3__fj_add_ambient(&rt->fj, wz3__fj_pu(op, 0)); return WZ3_OK;
  case 0x6b: return wz3__fj_exec_chain_line(rt, op);
  case 0x73: return wz3__fj_exec_water(rt, op);
  case 0x74: wz3__fj_post_output(&rt->fj, op, WZ3_EFFECT_OUT_GLARE); return WZ3_OK;
  case 0x75: wz3__fj_post_output(&rt->fj, op, WZ3_EFFECT_OUT_COLOR_CORRECTION); return WZ3_OK;
  case 0xf0: return wz3__fj_exec_viewport(rt, op);
  case 0xe9: return wz3__fj_exec_layer2d(rt, op);
  default: *handled = 0; return WZ3_OK;
  }
}
