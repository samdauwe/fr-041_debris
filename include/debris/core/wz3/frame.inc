/* ======================================================================== */
/*  materials, scene traversal and IPP as frame jobs                        */
/*                                                                          */
/*  Port of genmaterial.cpp, genscene.cpp and genoverlay.cpp. The handlers  */
/*  only record deterministic jobs (wz3_frame); rendering is the host's.    */
/*  Parameters follow the runtime's call parameters: float words are float  */
/*  values, integer words integers, read as doubles (wz3__fj_p).            */
/* ======================================================================== */

#define WZ3__FJ_KV_TIME 0
#define WZ3__FJ_KV_SELECT 3
#define WZ3__FJ_KV_FRACTION 0x0b

/* ---- helpers ---- */

static const wz3__rt_cache *wz3__rt_input_cache(const wz3_rt *rt, const wz3__rt_op *op, uint32_t i)
{
  if (i >= op->src->n_inputs) return NULL;
  const wz3__rt_cache *cache = &rt->ops[op->src->inputs[i]].cache;
  return cache->kind == WZ3__RC_NONE ? NULL : cache;
}

static int wz3__rt_is_spline_cache(const wz3__rt_cache *c)
{
  return c && (c->kind == WZ3__RC_BLOB || c->kind == WZ3__RC_SHAKER);
}

/* JS `x || 0`: zero, negative zero and NaN become +0 */
static inline double wz3__fj_or0(double v) { return (v == v && v != 0.0) ? v : 0.0; }

/* call.parameters[i]; operators are only ever read within their data words, a missing word reads 0 */
static double wz3__fj_p(const wz3__rt_op *op, uint32_t i) { return i < op->data_words ? wz3__rt_anim_param(op, i) : 0.0; }
/* call.parameters[i] ?? fallback */
static double wz3__fj_pd(const wz3__rt_op *op, uint32_t i, double fallback) { return i < op->data_words ? wz3__rt_anim_param(op, i) : fallback; }
static int32_t wz3__fj_pi(const wz3__rt_op *op, uint32_t i) { return wz3__rt_to_int32(wz3__fj_p(op, i)); }
static uint32_t wz3__fj_pu(const wz3__rt_op *op, uint32_t i) { return (uint32_t)wz3__rt_to_int32(wz3__fj_p(op, i)); }

static wz3_result wz3__fj_push(wz3__fj_vec *v, size_t elem, uint32_t add, void **out)
{
  if (v->n + add > v->cap) {
    uint32_t cap = v->cap ? v->cap * 2 : 16;
    while (cap < v->n + add) cap *= 2;
    void *items = WZ3_MALLOC((size_t)cap * elem);
    if (!items) return WZ3_ERR_OUT_OF_MEMORY;
    if (v->n) memcpy(items, v->p, (size_t)v->n * elem);
    WZ3_FREE(v->p);
    v->p = items;
    v->cap = cap;
  }
  *out = (char *)v->p + (size_t)v->n * elem;
  v->n += add;
  return WZ3_OK;
}

static float *wz3__fj_top(wz3__rt_env *env) { return env->stack + (size_t)(env->depth - 1) * 16; }

static wz3_result wz3__fj_stack_push(wz3__rt_env *env, const float matrix[16])
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
  memcpy(env->stack + (size_t)env->depth * 16, matrix, 16 * sizeof(float));
  env->depth++;
  return WZ3_OK;
}

/* vector normalize of _types.cpp: the length (not its square) is tested against 1e-20 */
static void wz3__fj_normalize(const double v[3], float out[3])
{
  const double length = sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
  if (length > 1e-20) {
    out[0] = (float)(v[0] / length);
    out[1] = (float)(v[1] / length);
    out[2] = (float)(v[2] / length);
  } else {
    out[0] = 1.0f; out[1] = out[2] = 0.0f;
  }
}

/* ---- materials ---- */

static void wz3__fj_material_free(wz3__fj_material *m)
{
  if (!m) return;
  WZ3_FREE(m->store);
  WZ3_FREE(m->passes);
  WZ3_FREE(m);
}

static wz3_result wz3__fj_add_pass(wz3__fj_material *m, uint8_t usage, uint8_t program, int32_t render_pass,
                                   double size, double aspect, uint8_t state, uint32_t owner)
{
  if (m->pub.n_passes == m->cap_passes) {
    const uint32_t cap = m->cap_passes ? m->cap_passes * 2 : 8;
    wz3_material_pass *items = (wz3_material_pass *)WZ3_MALLOC((size_t)cap * sizeof *items);
    if (!items) return WZ3_ERR_OUT_OF_MEMORY;
    if (m->pub.n_passes) memcpy(items, m->passes, (size_t)m->pub.n_passes * sizeof *items);
    WZ3_FREE(m->passes);
    m->passes = items;
    m->cap_passes = cap;
    m->pub.passes = items;
  }
  wz3_material_pass *p = &m->passes[m->pub.n_passes++];
  p->usage = usage; p->program = program; p->state = state; p->render_pass = render_pass;
  p->size = size; p->aspect = aspect; p->owner = owner;
  return WZ3_OK;
}

static int wz3__fj_bitmap_op(const wz3_rt *rt, int32_t index)
{
  return index >= 0 && (uint32_t)index < rt->n_ops && rt->ops[index].out_class == WZ3_KC_BITMAP &&
         rt->ops[index].cache.kind != WZ3__RC_NONE;
}

/* configureMaterial11 (reads the parameters the material was initialised with) */
static wz3_result wz3__fj_configure_material11(wz3__fj_material *m, const wz3__rt_op *op)
{
  const double *p = m->pub.initial;
  const uint32_t n = m->pub.n_params;
  const uint32_t flags = (uint32_t)wz3__rt_to_int32(wz3__fj_or0(48 < n ? p[48] : 0.0));
  const int32_t render_pass = wz3__rt_to_int32(wz3__fj_or0(49 < n ? p[49] : 0.0));
  const uint32_t mask = (uint32_t)wz3__rt_to_int32(wz3__fj_or0(50 < n ? p[50] : 0.0));
  wz3_result r;
  m->pub.multi_flags = flags;
  m->pub.shader_mask = mask;
  if ((flags & 0x1eu) == 0) {
    uint8_t program = (flags & 0x800u) ? WZ3_PROGRAM_INSTANCES : WZ3_PROGRAM_STATIC;
    double size = 1, aspect = 1;
    if (flags & 0x30000u) {
      size = 60 < n ? p[60] : 1.0;
      aspect = 62 < n ? p[62] : 1.0;
      const uint32_t finalizer = (flags >> 16) & 3u;
      if (finalizer == 1) program = WZ3_PROGRAM_SPRITES;
      else if (finalizer == 2) program = WZ3_PROGRAM_THICK_LINES;
    }
    static const uint8_t usages[8] = {WZ3_USAGE_BASE, WZ3_USAGE_PRELIGHT, WZ3_USAGE_AMBIENT, WZ3_USAGE_SHADOW,
                                      WZ3_USAGE_LIGHT, WZ3_USAGE_POSTLIGHT, WZ3_USAGE_POSTLIGHT2, WZ3_USAGE_OTHER};
    const uint32_t phase = (flags >> 20) & 15u;
    const uint8_t usage = phase == 0 ? WZ3_USAGE_OTHER : (phase <= 8 ? usages[phase - 1] : WZ3_USAGE_OTHER);
    return wz3__fj_add_pass(m, usage, program, render_pass, size, aspect, WZ3_PSTATE_M11_SINGLE, op->id);
  }
  const uint8_t program = (flags & 0x800u) ? WZ3_PROGRAM_INSTANCES : WZ3_PROGRAM_STATIC;
  /* the original always creates its depth/base phase once a multipass bit is set, then appends the others */
  if ((r = wz3__fj_add_pass(m, WZ3_USAGE_BASE, program, render_pass, 1, 1, WZ3_PSTATE_M11_BASE, op->id)) != WZ3_OK) return r;
  if ((flags & 0x02u) && (r = wz3__fj_add_pass(m, WZ3_USAGE_LIGHT, program, render_pass, 1, 1, WZ3_PSTATE_M11_LIGHT, op->id)) != WZ3_OK) return r;
  if ((flags & 0x04u) && (r = wz3__fj_add_pass(m, WZ3_USAGE_SHADOW, WZ3_PROGRAM_SHADOW, render_pass, 1, 1, WZ3_PSTATE_SHADOW_VOLUME, op->id)) != WZ3_OK) return r;
  if ((flags & 0x08u) && (r = wz3__fj_add_pass(m, WZ3_USAGE_POSTLIGHT, program, render_pass, 1, 1, WZ3_PSTATE_M11_POSTLIGHT, op->id)) != WZ3_OK) return r;
  if ((flags & 0x10u) && (r = wz3__fj_add_pass(m, WZ3_USAGE_POSTLIGHT2, program, render_pass, 1, 1, WZ3_PSTATE_M11_POSTLIGHT2, op->id)) != WZ3_OK) return r;
  return WZ3_OK;
}

/* initMaterial11 / initMaterial20 */
static wz3_result wz3__fj_init_material(wz3_rt *rt, wz3__rt_op *op, wz3__rt_cache *fresh)
{
  const int is20 = op->class_id == 0xd3;
  const uint32_t n = op->data_words;
  wz3__fj_material *m = (wz3__fj_material *)WZ3_MALLOC(sizeof *m);
  if (!m) return WZ3_ERR_OUT_OF_MEMORY;
  memset(m, 0, sizeof *m);
  m->store = (double *)WZ3_MALLOC(((size_t)n * 2 + 1) * sizeof(double));
  if (!m->store) { wz3__fj_material_free(m); return WZ3_ERR_OUT_OF_MEMORY; }
  m->pub.system = is20 ? 20 : 11;
  m->pub.n_params = n;
  m->pub.params = m->store;
  m->pub.initial = m->store + n;
  for (uint32_t i = 0; i < n; i++) m->store[i] = m->store[n + i] = wz3__rt_anim_param(op, i);
  for (int i = 0; i < 7; i++) m->pub.textures[i] = -1;
  if (!(op->convention & WZ3_OPC_DONTCALLLINK))
    for (uint32_t i = 0; i < 7 && i < op->src->n_links; i++)
      if (wz3__fj_bitmap_op(rt, op->src->links[i])) m->pub.textures[i] = op->src->links[i];
  wz3_result r = WZ3_OK;
  if (is20) {
    static const uint8_t input_map[7] = {8, 9, 10, 11, 16, 17, 18};
    for (int i = 0; i < 7; i++) {
      const int32_t input = (int32_t)((wz3__fj_pu(op, input_map[i]) & 0x03000000u) >> 24) - 1;
      if (input >= 0 && input < 3 && (uint32_t)input < op->src->n_inputs && wz3__fj_bitmap_op(rt, (int32_t)op->src->inputs[input]))
        m->pub.textures[i] = (int32_t)op->src->inputs[input];
    }
    const uint32_t flags = (uint32_t)wz3__rt_to_int32(wz3__fj_or0(n > 0 ? m->pub.initial[0] : 0.0));
    r = wz3__fj_add_pass(m, WZ3_USAGE_BASE, WZ3_PROGRAM_STATIC, 0, 1, 1, WZ3_PSTATE_M20_ZFILL, op->id);
    if (r == WZ3_OK) r = wz3__fj_add_pass(m, WZ3_USAGE_PRELIGHT, WZ3_PROGRAM_STATIC, 0, 1, 1, WZ3_PSTATE_M20_TEXTURE, op->id);
    if (r == WZ3_OK) r = wz3__fj_add_pass(m, WZ3_USAGE_AMBIENT, WZ3_PROGRAM_STATIC, 0, 1, 1, WZ3_PSTATE_M20_AMBIENT, op->id);
    if (r == WZ3_OK && (flags & 0x10u)) r = wz3__fj_add_pass(m, WZ3_USAGE_SHADOW, WZ3_PROGRAM_SHADOW, 0, 1, 1, WZ3_PSTATE_SHADOW_VOLUME, op->id);
    if (r == WZ3_OK) r = wz3__fj_add_pass(m, WZ3_USAGE_LIGHT, WZ3_PROGRAM_STATIC, 0, 1, 1, WZ3_PSTATE_M20_LIGHT, op->id);
    if (r == WZ3_OK && m->pub.textures[6] >= 0) r = wz3__fj_add_pass(m, WZ3_USAGE_POSTLIGHT, WZ3_PROGRAM_STATIC, 0, 1, 1, WZ3_PSTATE_M20_ENVIRONMENT, op->id);
  } else {
    r = wz3__fj_configure_material11(m, op);
    /* Material_Add: passes of an upstream material come first and keep their owner */
    const wz3__rt_cache *up = wz3__rt_input_cache(rt, op, 0);
    if (r == WZ3_OK && up && up->kind == WZ3__RC_MATERIAL && up->obj) {
      const wz3__fj_material *u = (const wz3__fj_material *)up->obj;
      const uint32_t own = m->pub.n_passes;
      wz3_material_pass own_passes[8];
      for (uint32_t i = 0; i < own && i < 8; i++) own_passes[i] = m->passes[i];
      m->pub.n_passes = 0;
      for (uint32_t i = 0; r == WZ3_OK && i < u->pub.n_passes; i++) {
        const wz3_material_pass *s = &u->passes[i];
        r = wz3__fj_add_pass(m, s->usage, s->program, s->render_pass, s->size, s->aspect, s->state, s->owner);
      }
      for (uint32_t i = 0; r == WZ3_OK && i < own; i++) {
        const wz3_material_pass *s = &own_passes[i];
        r = wz3__fj_add_pass(m, s->usage, s->program, s->render_pass, s->size, s->aspect, s->state, s->owner);
      }
    }
  }
  if (r != WZ3_OK) { wz3__fj_material_free(m); return r; }
  fresh->kind = WZ3__RC_MATERIAL; fresh->owned = 1; fresh->class_id = WZ3_KC_MATERIAL; fresh->obj = m;
  return WZ3_OK;
}

/* Material.update: true when a parameter changed */
static int wz3__fj_material_update(wz3__fj_material *m, const wz3__rt_op *op)
{
  int changed = 0;
  for (uint32_t i = 0; i < m->pub.n_params; i++) {
    const double v = wz3__rt_anim_param(op, i), o = m->store[i];
    const int same = (v != v && o != o) || (v == o && (v != 0.0 || (1.0 / v == 1.0 / o)));
    if (!same) { changed = 1; m->store[i] = v; }
  }
  if (changed) m->pub.version++;
  return changed;
}

static wz3_result wz3__fj_exec_material(wz3_rt *rt, wz3__rt_op *op)
{
  wz3_result r = wz3__rt_exec_inputs(rt, op);
  if (r != WZ3_OK) return r;
  if (op->cache.kind != WZ3__RC_MATERIAL || !op->cache.obj) return WZ3_ERR_OPERATOR;
  wz3__fj_material *m = (wz3__fj_material *)op->cache.obj;
  wz3__fj_material_update(m, op);
  if (op->class_id == 0xd0) {
    const uint32_t flags = (uint32_t)wz3__rt_to_int32(wz3__fj_or0(wz3__fj_p(op, 48)));
    if (flags & 0x30000u)
      for (uint32_t i = 0; i < m->pub.n_passes; i++) {
        if (m->passes[i].usage != WZ3_USAGE_OTHER) continue;
        m->passes[i].size = wz3__fj_pd(op, 60, 1.0);
        m->passes[i].aspect = wz3__fj_pd(op, 62, 1.0);
      }
  }
  return WZ3_OK;
}

WZ3_API const wz3_material *wz3_rt_material(const wz3_rt *rt, uint32_t op)
{
  if (!rt || op >= rt->n_ops || rt->ops[op].cache.kind != WZ3__RC_MATERIAL) return NULL;
  return (const wz3_material *)rt->ops[op].cache.obj;
}
