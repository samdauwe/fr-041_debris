/* ======================================================================== */
/*  scene traversal and IPP (genscene.cpp, genoverlay.cpp)                  */
/* ======================================================================== */

enum { WZ3__FJ_K_NONE = 0, WZ3__FJ_K_MESH, WZ3__FJ_K_MINMESH, WZ3__FJ_K_SCENE, WZ3__FJ_K_EFFECT };

static int wz3__fj_class_kind(uint8_t out_class)
{
  switch (out_class) {
  case WZ3_KC_MESH: return WZ3__FJ_K_MESH;
  case WZ3_KC_MINMESH: return WZ3__FJ_K_MINMESH;
  case WZ3_KC_SCENE: return WZ3__FJ_K_SCENE;
  case WZ3_KC_EFFECT: return WZ3__FJ_K_EFFECT;
  default: return WZ3__FJ_K_NONE;
  }
}

/* objectKind(value) */
static int wz3__fj_cache_kind(const wz3__rt_cache *c)
{
  switch (c->kind) {
  case WZ3__RC_MESH: return WZ3__FJ_K_MESH;
  case WZ3__RC_MINMESH: return WZ3__FJ_K_MINMESH;
  case WZ3__RC_SCENE: return WZ3__FJ_K_SCENE;
  case WZ3__RC_EFFECT: return WZ3__FJ_K_EFFECT;
  case WZ3__RC_MISSING: return wz3__fj_class_kind(c->class_id);   /* MissingCache.outputClass */
  default: return WZ3__FJ_K_NONE;
  }
}

/* outputKind(op): the cache's kind, else the class's output class */
static int wz3__fj_output_kind(const wz3_rt *rt, uint32_t index)
{
  const wz3__rt_cache *c = &rt->ops[index].cache;
  const int kind = c->kind != WZ3__RC_NONE ? wz3__fj_cache_kind(c) : WZ3__FJ_K_NONE;
  return kind ? kind : wz3__fj_class_kind(rt->ops[index].out_class);
}

static void wz3__fj_scene_free(wz3__fj_scene *s)
{
  if (!s) return;
  WZ3_FREE(s->child_mesh);
  WZ3_FREE(s);
}

static wz3__fj_scene *wz3__fj_scene_new(uint32_t n_children)
{
  wz3__fj_scene *s = (wz3__fj_scene *)WZ3_MALLOC(sizeof *s);
  if (!s) return NULL;
  memset(s, 0, sizeof *s);
  s->draw_mesh = s->effect = -1;
  if (n_children) {
    s->child_mesh = (int32_t *)WZ3_MALLOC((size_t)n_children * sizeof(int32_t));
    if (!s->child_mesh) { WZ3_FREE(s); return NULL; }
  }
  return s;
}

/* makeScene(value): NULL for anything that is no scene, mesh, minmesh or effect (*ok is the allocation result) */
static wz3__fj_scene *wz3__fj_make_scene(const wz3__rt_cache *cache, int32_t op_index, int *ok)
{
  *ok = 1;
  if (!cache) return NULL;
  const int kind = wz3__fj_cache_kind(cache);
  if (!kind) return NULL;
  const wz3__fj_scene *source = (kind == WZ3__FJ_K_SCENE && cache->kind == WZ3__RC_SCENE) ? (const wz3__fj_scene *)cache->obj : NULL;
  wz3__fj_scene *s = wz3__fj_scene_new(source ? source->n_children : 0);
  if (!s) { *ok = 0; return NULL; }
  if (source) {
    s->draw_mesh = source->draw_mesh;
    s->effect = source->effect;
    s->n_children = source->n_children;
    if (source->n_children) memcpy(s->child_mesh, source->child_mesh, (size_t)source->n_children * sizeof(int32_t));
  } else if (kind == WZ3__FJ_K_MESH || kind == WZ3__FJ_K_MINMESH) {
    s->draw_mesh = op_index;
  } else if (kind == WZ3__FJ_K_EFFECT) {
    s->effect = op_index;
  }
  return s;
}

/* scene op init: `children` are the input slots whose makeScene() is not null (in order); mode 0: a scene with
   those children, 1 (Scene): makeScene(input 0) or an empty scene, 2 (Add/Multiply/Marker/Particles/...) as 0 */
static wz3_result wz3__fj_init_scene(wz3_rt *rt, wz3__rt_op *op, wz3__rt_cache *fresh)
{
  const uint32_t n = op->src->n_inputs;
  wz3__fj_scene *scene = NULL;
  int ok = 1;
  switch (op->class_id) {
  case 0xc0: {                                                   /* Scene */
    scene = wz3__fj_make_scene(wz3__rt_input_cache(rt, op, 0), n ? (int32_t)op->src->inputs[0] : -1, &ok);
    if (!ok) return WZ3_ERR_OUT_OF_MEMORY;
    if (!scene) { scene = wz3__fj_scene_new(0); if (!scene) return WZ3_ERR_OUT_OF_MEMORY; }
    break;
  }
  case 0xc1: {                                                   /* Add: every input that makes a scene is a child */
    scene = wz3__fj_scene_new(n);
    if (!scene) return WZ3_ERR_OUT_OF_MEMORY;
    for (uint32_t i = 0; i < n; i++) {
      wz3__fj_scene *child = wz3__fj_make_scene(wz3__rt_input_cache(rt, op, i), (int32_t)op->src->inputs[i], &ok);
      if (!ok) { wz3__fj_scene_free(scene); return WZ3_ERR_OUT_OF_MEMORY; }
      if (!child) continue;
      scene->child_mesh[scene->n_children++] = child->draw_mesh;
      wz3__fj_scene_free(child);
    }
    break;
  }
  case 0xc2: case 0xc3: case 0xc5: case 0x182: {                 /* Multiply, Transform, Particles, Marker */
    wz3__fj_scene *child = wz3__fj_make_scene(wz3__rt_input_cache(rt, op, 0), n ? (int32_t)op->src->inputs[0] : -1, &ok);
    if (!ok) return WZ3_ERR_OUT_OF_MEMORY;
    if (!child && (op->class_id == 0xc2 || op->class_id == 0xc3)) return WZ3_OK;   /* no cache */
    scene = wz3__fj_scene_new(child ? 1 : 0);
    if (!scene) { wz3__fj_scene_free(child); return WZ3_ERR_OUT_OF_MEMORY; }
    if (child) { scene->child_mesh[0] = child->draw_mesh; scene->n_children = 1; wz3__fj_scene_free(child); }
    break;
  }
  default:                                                       /* Light, MatHack, Ambient: an empty scene */
    scene = wz3__fj_scene_new(0);
    if (!scene) return WZ3_ERR_OUT_OF_MEMORY;
    break;
  }
  fresh->kind = WZ3__RC_SCENE; fresh->owned = 1; fresh->class_id = WZ3_KC_SCENE; fresh->obj = scene;
  return WZ3_OK;
}

/* ---- frame ---- */

static void wz3__fj_camera_env_default(wz3_camera *c)
{
  memset(c, 0, sizeof *c);
  wz3_mat4_identity(c->camera_space);
  wz3_mat4_identity(c->model_space);
  c->far_clip = 4096; c->near_clip = 0.125;
  c->zoom_x = c->zoom_y = 1;
  c->fog_color = 0xff808080u;
  c->fog_start = 0; c->fog_end = 4096;
}

static void wz3__fj_camera_defaults(wz3_camera *c)
{
  wz3__fj_camera_env_default(c);
  c->fog_color = 0;
}

static void wz3__fj_refresh_view(wz3_rt *rt)
{
  wz3__fj *fj = &rt->fj;
  fj->view.n_outputs = fj->outputs.n;       fj->view.outputs = (const uint32_t *)fj->outputs.p;
  fj->view.n_nodes = fj->nodes.n;           fj->view.nodes = (const wz3_ipp_node *)fj->nodes.p;
  fj->view.mesh_jobs = (const wz3_mesh_job *)fj->mesh.p;
  fj->view.effect_jobs = (const wz3_effect_job *)fj->effect.p;
  fj->view.light_jobs = (const wz3_light_job *)fj->light.p;
  fj->view.instances = (const float *)fj->instance.p;
  fj->view.effect_variables = (const float *)fj->variables.p;
  fj->view.marker_tables = (const float *)fj->marker_tables.p;
}

/* Environment.initFrame, the part that concerns the frame jobs */
static void wz3__fj_init_frame(wz3_rt *rt)
{
  wz3__fj *fj = &rt->fj;
  fj->mesh.n = fj->effect.n = fj->light.n = fj->instance.n = fj->variables.n = fj->marker_tables.n = 0;
  fj->nodes.n = fj->outputs.n = 0;
  fj->b_mesh = fj->b_effect = fj->b_light = 0;
  fj->cur_table = -1;
  fj->ambient = 0;
  fj->last_output = -1;
  fj->render_pass_adjust = 0;
  if (fj->ipp_node) for (uint32_t i = 0; i < rt->n_ops; i++) fj->ipp_node[i] = -1;
  wz3__fj_camera_env_default(&fj->game_camera);
  wz3__fj_camera_env_default(&fj->current_camera);
  wz3__fj_refresh_view(rt);
}

/* ensureFrame(): the marker table frame.effectMarkers, created from the environment markers when absent */
static wz3_result wz3__fj_ensure_table(wz3_rt *rt)
{
  wz3__fj *fj = &rt->fj;
  if (fj->cur_table >= 0) return WZ3_OK;
  void *table;
  const wz3_result r = wz3__fj_push(&fj->marker_tables, 512 * sizeof(float), 1, &table);
  if (r != WZ3_OK) return r;
  memcpy(table, fj->markers, sizeof fj->markers);
  fj->cur_table = (int32_t)fj->marker_tables.n - 1;
  return WZ3_OK;
}

/* beginRenderFrame */
static wz3_result wz3__fj_begin_render_frame(wz3_rt *rt)
{
  wz3__fj *fj = &rt->fj;
  fj->b_mesh = fj->mesh.n; fj->b_effect = fj->effect.n; fj->b_light = fj->light.n;
  fj->cur_table = -1;
  fj->ambient = 0;
  return wz3__fj_ensure_table(rt);
}

static wz3_result wz3__fj_add_mesh_job(wz3_rt *rt, int32_t mesh_op, int32_t source_op, uint8_t has_instances,
                                       uint32_t first_instance, uint32_t n_instances, float time, int32_t pass_adjust)
{
  wz3_mesh_job *job;
  const wz3_result r = wz3__fj_push(&rt->fj.mesh, sizeof *job, 1, (void **)&job);
  if (r != WZ3_OK) return r;
  job->mesh_op = mesh_op;
  job->source_op = source_op;
  memcpy(job->matrix, wz3__fj_top(&rt->env), sizeof job->matrix);
  job->time = time;
  job->pass_adjust = pass_adjust;
  job->has_instances = has_instances;
  job->first_instance = first_instance;
  job->n_instances = n_instances;
  return WZ3_OK;
}

/* ---- execSceneInput(s) ---- */

static wz3_result wz3__fj_exec_scene_input(wz3_rt *rt, wz3__rt_op *op, uint32_t index)
{
  if (index >= op->src->n_inputs) return WZ3_OK;
  const uint32_t input = op->src->inputs[index];
  const int kind = wz3__fj_output_kind(rt, input);
  wz3__rt_env *env = &rt->env;
  if (kind == WZ3__FJ_K_MESH || kind == WZ3__FJ_K_MINMESH) {
    const wz3__fj_scene *scene = op->cache.kind == WZ3__RC_SCENE ? (const wz3__fj_scene *)op->cache.obj : NULL;
    int32_t mesh = -1;
    if (scene && scene->n_children) {
      if (index < scene->n_children) mesh = scene->child_mesh[index];
    } else if (scene) {
      mesh = scene->draw_mesh;
    }
    if (mesh < 0 && rt->ops[input].cache.kind != WZ3__RC_NONE) mesh = (int32_t)input;
    if (mesh < 0) return WZ3_OK;
    return wz3__fj_add_mesh_job(rt, mesh, (int32_t)input, 0, 0, 0, (float)wz3__fj_or0(env->vars[WZ3__FJ_KV_TIME][0]),
                                rt->fj.render_pass_adjust);
  }
  if (kind == WZ3__FJ_K_EFFECT) {
    wz3_result r = wz3__fj_ensure_table(rt);
    wz3_effect_job *job;
    float *vars;
    if (r != WZ3_OK) return r;
    if ((r = wz3__fj_push(&rt->fj.effect, sizeof *job, 1, (void **)&job)) != WZ3_OK) return r;
    if ((r = wz3__fj_push(&rt->fj.variables, WZ3__RT_MAX_VARS * 4 * sizeof(float), 1, (void **)&vars)) != WZ3_OK) {
      rt->fj.effect.n--;
      return r;
    }
    memcpy(vars, env->vars, sizeof env->vars);
    job->effect_op = input;
    memcpy(job->matrix, wz3__fj_top(env), sizeof job->matrix);
    job->variables = (rt->fj.variables.n - 1) * (WZ3__RT_MAX_VARS * 4);
    job->markers = (uint32_t)rt->fj.cur_table;
    job->pass_adjust = rt->fj.render_pass_adjust;
    return WZ3_OK;
  }
  return wz3__rt_exec_op(rt, input);
}

static wz3_result wz3__fj_exec_scene_inputs(wz3_rt *rt, wz3__rt_op *op)
{
  for (uint32_t i = 0; i < op->src->n_inputs; i++) {
    const wz3_result r = wz3__fj_exec_scene_input(rt, op, i);
    if (r != WZ3_OK) return r;
  }
  return WZ3_OK;
}

/* sceneTransformMatrix: srtFrom(parameters) with the defaults of an absent word */
static void wz3__fj_scene_matrix(const wz3__rt_op *op, float out[16])
{
  float srt[9];
  for (uint32_t i = 0; i < 9; i++) srt[i] = (float)wz3__fj_pd(op, i, i < 3 ? 1.0 : 0.0);
  wz3_mat4_srt(srt, out);
}

static wz3_result wz3__fj_exec_transformed(wz3_rt *rt, wz3__rt_op *op)
{
  float matrix[16];
  wz3__fj_scene_matrix(op, matrix);
  wz3_result r = wz3__rt_stack_push_mul(&rt->env, matrix);
  if (r != WZ3_OK) return r;
  r = wz3__fj_exec_scene_inputs(rt, op);
  wz3__rt_stack_pop(&rt->env);
  return r;
}

static wz3_result wz3__fj_exec_multiply(wz3_rt *rt, wz3__rt_op *op)
{
  wz3__rt_env *env = &rt->env;
  const int32_t count = wz3__fj_pi(op, 9);
  float matrix[16], next[16], saved[4];
  wz3__fj_scene_matrix(op, matrix);
  memcpy(saved, env->vars[WZ3__FJ_KV_SELECT], sizeof saved);
  wz3_result r = wz3__fj_stack_push(env, wz3__fj_top(env));            /* duplicate */
  for (int32_t i = 0; r == WZ3_OK && i < count; i++) {
    for (int k = 0; k < 4; k++) env->vars[WZ3__FJ_KV_SELECT][k] = (float)i;
    r = wz3__fj_exec_scene_inputs(rt, op);
    if (r != WZ3_OK) break;
    wz3_mat4_mul_a(matrix, wz3__fj_top(env), next);
    wz3__rt_stack_pop(env);
    r = wz3__fj_stack_push(env, next);
  }
  if (r == WZ3_OK) {
    wz3__rt_stack_pop(env);
    memcpy(env->vars[WZ3__FJ_KV_SELECT], saved, sizeof saved);
  }
  return r;
}

static wz3_result wz3__fj_exec_light(wz3_rt *rt, wz3__rt_op *op)
{
  float srt[9] = {1, 1, 1, 0, 0, 0, 0, 0, 0}, matrix[16];
  for (uint32_t i = 0; i < 6; i++) srt[3 + i] = (float)wz3__fj_or0(wz3__fj_p(op, i));
  wz3_mat4_srt(srt, matrix);
  wz3__rt_env *env = &rt->env;
  wz3_result r = wz3__rt_stack_push_mul(env, matrix);
  if (r != WZ3_OK) return r;
  const float *top = wz3__fj_top(env);
  const uint32_t flags = wz3__fj_pu(op, 6);
  const int directional = (flags & 4u) != 0;
  const double range = wz3__fj_or0(wz3__fj_p(op, 10));
  wz3_light_job *job;
  r = wz3__fj_push(&rt->fj.light, sizeof *job, 1, (void **)&job);
  if (r == WZ3_OK) {
    job->directional = (uint8_t)directional;
    job->op = op->id;
    for (int k = 0; k < 3; k++) {
      job->direction[k] = top[8 + k];
      job->position[k] = (directional && range == 0.0) ? (float)((double)top[8 + k] * 1e6) : top[12 + k];
    }
    job->flags = flags;
    job->color = wz3__fj_pu(op, 8);
    job->amplify = (float)wz3__fj_or0(wz3__fj_p(op, 9));
    job->range = range != 0.0 ? (double)(float)range : 1e15;
    job->event = env->current_event;
  }
  wz3__rt_stack_pop(env);
  return r;
}

static wz3_result wz3__fj_exec_marker(wz3_rt *rt, wz3__rt_op *op)
{
  const int32_t marker = wz3__fj_pi(op, 0);
  if (marker < 0 || marker >= 32) return WZ3_ERR_OPERATOR;            /* RangeError */
  wz3_result r = wz3__fj_ensure_table(rt);
  if (r != WZ3_OK) return r;
  memcpy(rt->fj.markers[marker], wz3__fj_top(&rt->env), 16 * sizeof(float));
  memcpy((float *)rt->fj.marker_tables.p + (size_t)rt->fj.cur_table * 512 + (size_t)marker * 16, rt->fj.markers[marker], 16 * sizeof(float));
  return wz3__fj_exec_scene_inputs(rt, op);
}

static void wz3__fj_add_ambient(wz3__fj *fj, uint32_t color)
{
  const uint32_t old = fj->ambient;
  uint32_t r = ((old >> 16) & 255u) + ((color >> 16) & 255u);
  uint32_t g = ((old >> 8) & 255u) + ((color >> 8) & 255u);
  uint32_t b = (old & 255u) + (color & 255u);
  if (r > 255) r = 255;
  if (g > 255) g = 255;
  if (b > 255) b = 255;
  fj->ambient = (r << 16) | (g << 8) | b;
}

/* ---- Particles ---- */

typedef struct wz3__fj_pmem {
  int32_t mode, count, seed;
  float rand[3], rand_rot[3], rand_forw, rand_rot_speed[3], come_out;
} wz3__fj_pmem;

static size_t wz3__fj_pmem_size(int32_t mode, int32_t count)
{
  return sizeof(wz3__fj_pmem) + ((size_t)count * 8 + ((mode & 0x10) ? (size_t)count * 6 : 0)) * sizeof(float);
}
static float *wz3__fj_pmem_pos(wz3__fj_pmem *m) { return (float *)(m + 1); }
static float *wz3__fj_pmem_speed(wz3__fj_pmem *m) { return wz3__fj_pmem_pos(m) + (size_t)m->count * 4; }
static float *wz3__fj_pmem_rot(wz3__fj_pmem *m) { return wz3__fj_pmem_speed(m) + (size_t)m->count * 4; }
static float *wz3__fj_pmem_rot_speed(wz3__fj_pmem *m) { return wz3__fj_pmem_rot(m) + (size_t)m->count * 3; }

static float wz3__fj_rndf(wz3_rng *r, double max)
{
  return (float)(((double)(wz3_rng_u32(r) & 0x3fffffffu) * max) / 1073741824.0);
}

/* particleMemoryChanged */
static int wz3__fj_particles_changed(const wz3__fj_pmem *m, const wz3__rt_op *op)
{
  const int32_t mode = wz3__fj_pi(op, 0), count = wz3__fj_pi(op, 1) < 0 ? 0 : wz3__fj_pi(op, 1);
  return m->mode != mode || m->count != count || m->seed != wz3__fj_pi(op, 2) ||
         m->rand[0] != (float)wz3__fj_p(op, 3) || m->rand[1] != (float)wz3__fj_p(op, 4) || m->rand[2] != (float)wz3__fj_p(op, 5) ||
         m->rand_rot[0] != (float)wz3__fj_p(op, 6) || m->rand_rot[1] != (float)wz3__fj_p(op, 7) || m->rand_rot[2] != (float)wz3__fj_p(op, 8) ||
         m->rand_forw != (float)wz3__fj_or0(wz3__fj_p(op, 22)) || m->come_out != (float)wz3__fj_or0(wz3__fj_p(op, 23)) ||
         m->rand_rot_speed[0] != (float)wz3__fj_p(op, 24) || m->rand_rot_speed[1] != (float)wz3__fj_p(op, 25) ||
         m->rand_rot_speed[2] != (float)wz3__fj_p(op, 26);
}

/* initParticleMemory: the memory block is replaced when the particle count or mode changed its size */
static wz3_result wz3__fj_particles_init(wz3__rt_instance *inst, const wz3__rt_op *op, wz3__fj_pmem **out)
{
  const int32_t mode = wz3__fj_pi(op, 0);
  const int32_t count = wz3__fj_pi(op, 1) < 0 ? 0 : wz3__fj_pi(op, 1);
  if (count > (1 << 24)) return WZ3_ERR_LIMIT;
  const size_t size = wz3__fj_pmem_size(mode, count);
  if (!inst->blob || inst->blob_size != size) {
    void *blob = WZ3_MALLOC(size);
    if (!blob) return WZ3_ERR_OUT_OF_MEMORY;
    WZ3_FREE(inst->blob);
    inst->blob = blob;
    inst->blob_size = (uint32_t)size;
  }
  wz3__fj_pmem *m = (wz3__fj_pmem *)inst->blob;
  memset(m, 0, size);
  m->mode = mode; m->count = count; m->seed = wz3__fj_pi(op, 2);
  for (int k = 0; k < 3; k++) {
    m->rand[k] = (float)wz3__fj_p(op, 3 + k);
    m->rand_rot[k] = (float)wz3__fj_p(op, 6 + k);
    m->rand_rot_speed[k] = (float)wz3__fj_p(op, 24 + k);
  }
  m->rand_forw = (float)wz3__fj_or0(wz3__fj_p(op, 22));
  m->come_out = (float)wz3__fj_or0(wz3__fj_p(op, 23));
  float *pos_all = wz3__fj_pmem_pos(m), *speed_all = wz3__fj_pmem_speed(m);
  float *rot_all = wz3__fj_pmem_rot(m), *rot_speed_all = wz3__fj_pmem_rot_speed(m);
  wz3_rng random;
  wz3_rng_init(&random);
  wz3_rng_seed(&random, m->seed);
  for (int32_t i = 0; i < count; i++) {
    float pos[4], speed[4];
    do {
      pos[0] = (float)((double)wz3__fj_rndf(&random, 2) - 1);
      pos[1] = (float)((double)wz3__fj_rndf(&random, 2) - 1);
      pos[2] = (float)((double)wz3__fj_rndf(&random, 2) - 1);
      if ((mode & 12) == 4) {
        const double v[3] = {pos[0], pos[1], pos[2]};
        wz3__fj_normalize(v, pos);
      }
    } while ((mode & 12) == 8 && (double)pos[0] * pos[0] + (double)pos[1] * pos[1] + (double)pos[2] * pos[2] > 1.0);
    pos[3] = (mode & 0x20) ? (float)((double)i / (double)count)
                           : (float)((double)wz3__fj_rndf(&random, 1.0 - (double)m->come_out) + (double)m->come_out);
    pos[0] = (float)((double)pos[0] * m->rand[0] * 0.5);
    pos[1] = (float)((double)pos[1] * m->rand[1] * 0.5);
    pos[2] = (float)((double)pos[2] * m->rand[2] * 0.5);
    memcpy(pos_all + (size_t)i * 4, pos, sizeof pos);
    do {
      speed[0] = (float)((double)wz3__fj_rndf(&random, 2) - 1);
      speed[1] = (float)((double)wz3__fj_rndf(&random, 2) - 1);
      speed[2] = (float)((double)wz3__fj_rndf(&random, 2) - 1);
    } while ((double)speed[0] * speed[0] + (double)speed[1] * speed[1] + (double)speed[2] * speed[2] > 1.0);
    speed[3] = wz3__fj_rndf(&random, 1.0);
    memcpy(speed_all + (size_t)i * 4, speed, sizeof speed);
    if (mode & 0x10) {
      float *rot = rot_all + (size_t)i * 3, *rs = rot_speed_all + (size_t)i * 3;
      rot[0] = wz3__fj_rndf(&random, m->rand_rot[0]);
      rot[1] = wz3__fj_rndf(&random, m->rand_rot[1]);
      rot[2] = wz3__fj_rndf(&random, m->rand_rot[2]);
      rs[0] = wz3__fj_rndf(&random, m->rand_rot_speed[0]);
      rs[1] = wz3__fj_rndf(&random, m->rand_rot_speed[1]);
      rs[2] = wz3__fj_rndf(&random, m->rand_rot_speed[2]);
    }
  }
  *out = m;
  return WZ3_OK;
}

/* evalGeneratedSpline: the matrix a generated (blob or shaker) spline gives at `time`, or 0 when it is none */
static int wz3__fj_eval_generated(const wz3_rt *rt, const wz3__rt_cache *spline, double time, float matrix[16])
{
  if (!wz3__rt_is_spline_cache(spline)) return 0;
  float zoom;
  wz3__rt_spline_eval(rt, spline, time, matrix, &zoom);
  return 1;
}

static void wz3__fj_eval_value_spline(const wz3_rt *rt, int32_t spline, double time, float out[4])
{
  wz3__rt_dspline_eval(&rt->splines[spline], time, out);
}

static wz3_result wz3__fj_exec_particles(wz3_rt *rt, wz3__rt_op *op)
{
  wz3__rt_env *env = &rt->env;
  wz3_result r;
  wz3__rt_instance *inst;
  if ((r = wz3__rt_chain_instance(env, op->id, &inst)) != WZ3_OK) return r;
  wz3__fj_pmem *mem = (wz3__fj_pmem *)inst->blob;
  if (inst->reset || !mem || wz3__fj_particles_changed(mem, op))
    if ((r = wz3__fj_particles_init(inst, op, &mem)) != WZ3_OK) return r;
  const int32_t mode = mem->mode, count = mem->count;
  double anim = wz3__fj_or0(wz3__fj_p(op, 12));
  if (!(mode & 0x80)) anim = fmod(anim, 1.0);
  if (anim < 0) anim += 1;

  const int32_t doc_spline = op->src->n_splines ? op->src->splines[0] : -1;
  const wz3__rt_cache *generated = op->src->n_inputs >= 2 || op->input_slots >= 2 ? wz3__rt_input_cache(rt, op, 1) : NULL;
  const int instanced = (mode & 0x100) != 0;
  const uint32_t first_instance = rt->fj.instance.n;
  uint32_t n_instances = 0;
  float saved_select[4], saved_fraction[4];
  memcpy(saved_select, env->vars[WZ3__FJ_KV_SELECT], sizeof saved_select);
  memcpy(saved_fraction, env->vars[WZ3__FJ_KV_FRACTION], sizeof saved_fraction);
  const double random_speed = wz3__fj_p(op, 16);
  const double gravity[3] = {wz3__fj_p(op, 19), wz3__fj_p(op, 20), wz3__fj_p(op, 21)};
  const double line[3] = {wz3__fj_p(op, 13), wz3__fj_p(op, 14), wz3__fj_p(op, 15)};
  const double pulse_rate = wz3__fj_p(op, 27), pulse_phase = wz3__fj_p(op, 28), pulse_amount = wz3__fj_p(op, 29);
  const double rotation_rate[3] = {wz3__fj_p(op, 9), wz3__fj_p(op, 10), wz3__fj_p(op, 11)};
  float *pos_all = wz3__fj_pmem_pos(mem), *speed_all = wz3__fj_pmem_speed(mem);
  float *rot_all = (mode & 0x10) ? wz3__fj_pmem_rot(mem) : NULL, *rot_speed_all = (mode & 0x10) ? wz3__fj_pmem_rot_speed(mem) : NULL;

  for (int32_t i = 0; i < count && r == WZ3_OK; i++) {
    const float *pos = pos_all + (size_t)i * 4, *speed = speed_all + (size_t)i * 4;
    double fraction = anim + (double)pos[3] + (double)mem->rand_forw * (double)speed[3] * anim;
    if (fraction < 1 && (mode & 64)) continue;
    while (fraction >= 1 && fraction < 9007199254740992.0) fraction -= 1;
    float matrix[16], position[3];
    wz3_mat4_identity(matrix);
    const double g = fraction * fraction;
    for (int k = 0; k < 3; k++)
      position[k] = (float)((double)pos[k] + (double)speed[k] * fraction * random_speed + g * gravity[k]);
    if (pulse_amount != 0.0 && pulse_amount == pulse_amount) {
      const double scale = wz3__sin(anim * 3.141592653589793 * 2 * pulse_rate + fraction * 3.141592653589793 * 2 * pulse_phase) * pulse_amount + 1;
      for (int k = 0; k < 3; k++) position[k] = (float)((double)position[k] * scale);
    }
    if ((mode & 1) && doc_spline >= 0) {
      float value[4], ahead[4];
      wz3__fj_eval_value_spline(rt, doc_spline, fraction, value);
      if (mode & 0x10) {
        wz3__fj_eval_value_spline(rt, doc_spline, fraction + 0.01, ahead);
        const double direction[3] = {(double)ahead[0] - value[0], (double)ahead[1] - value[1], (double)ahead[2] - value[2]};
        wz3__rt_init_direction(direction, matrix);
      }
      for (int k = 0; k < 3; k++) matrix[12 + k] = (float)((double)matrix[12 + k] + ((double)position[k] + (double)value[k]));
    }
    if (wz3__fj_eval_generated(rt, generated, fraction, matrix)) {
      const double x = (double)matrix[0] * position[0] + (double)matrix[4] * position[1] + (double)matrix[8] * position[2];
      const double y = (double)matrix[1] * position[0] + (double)matrix[5] * position[1] + (double)matrix[9] * position[2];
      const double z = (double)matrix[2] * position[0] + (double)matrix[6] * position[1] + (double)matrix[10] * position[2];
      matrix[12] = (float)((double)matrix[12] + x);
      matrix[13] = (float)((double)matrix[13] + y);
      matrix[14] = (float)((double)matrix[14] + z);
    } else {
      for (int k = 0; k < 3; k++) matrix[12 + k] = (float)((double)matrix[12 + k] + ((double)position[k] + line[k] * fraction));
    }
    if (rot_all) {
      double angle[3];
      for (int k = 0; k < 3; k++) {
        angle[k] = fraction * rotation_rate[k];
        angle[k] += (double)rot_all[(size_t)i * 3 + k];
        angle[k] += (double)rot_speed_all[(size_t)i * 3 + k] * fraction;
      }
      float rotation[16], composed[16];
      wz3_mat4_euler((float)angle[0], (float)angle[1], (float)angle[2], rotation);
      wz3_mat4_mul_a(rotation, matrix, composed);
      memcpy(matrix, composed, sizeof matrix);
    }
    if (instanced) {
      float *dest;
      r = wz3__fj_push(&rt->fj.instance, 16 * sizeof(float), 1, (void **)&dest);
      if (r == WZ3_OK) { memcpy(dest, matrix, sizeof matrix); n_instances++; }
    } else {
      for (int k = 0; k < 4; k++) { env->vars[WZ3__FJ_KV_SELECT][k] = (float)i; env->vars[WZ3__FJ_KV_FRACTION][k] = (float)fraction; }
      r = wz3__rt_stack_push_mul(env, matrix);
      if (r == WZ3_OK) {
        r = wz3__fj_exec_scene_input(rt, op, 0);
        wz3__rt_stack_pop(env);
      }
    }
  }
  if (r != WZ3_OK) return r;
  memcpy(env->vars[WZ3__FJ_KV_SELECT], saved_select, sizeof saved_select);
  memcpy(env->vars[WZ3__FJ_KV_FRACTION], saved_fraction, sizeof saved_fraction);
  if (instanced) {
    const wz3__fj_scene *scene = op->cache.kind == WZ3__RC_SCENE ? (const wz3__fj_scene *)op->cache.obj : NULL;
    int32_t mesh_op = -1;
    if (scene && scene->n_children && scene->child_mesh[0] >= 0) mesh_op = scene->child_mesh[0];
    else if (wz3__rt_input_cache(rt, op, 0)) mesh_op = (int32_t)op->src->inputs[0];
    if (mesh_op >= 0) {
      /* call.inputs[0] is whatever the input made; only a mesh or minmesh is geometry (a Scene input draws nothing) */
      const int kind = wz3__fj_cache_kind(&rt->ops[mesh_op].cache);
      r = wz3__fj_add_mesh_job(rt, (kind == WZ3__FJ_K_MESH || kind == WZ3__FJ_K_MINMESH) ? mesh_op : -1,
                               op->src->n_inputs ? (int32_t)op->src->inputs[0] : -1, 1, first_instance, n_instances, 0.0f, 0);
    }
  }
  return r;
}

/* ---- IPP: Viewport and Layer2D ---- */

static void wz3__fj_cross(const double a[3], const double b[3], float out[3])
{
  out[0] = (float)(a[1] * b[2] - a[2] * b[1]);
  out[1] = (float)(a[2] * b[0] - a[0] * b[2]);
  out[2] = (float)(a[0] * b[1] - a[1] * b[0]);
}

/* buildCamera */
static void wz3__fj_build_camera(wz3_rt *rt, const wz3__rt_op *op, wz3_camera *camera)
{
  const uint32_t flags = wz3__fj_pu(op, 1);
  wz3__fj_camera_defaults(camera);
  camera->far_clip = wz3__fj_p(op, 9);
  camera->near_clip = wz3__fj_p(op, 10);
  camera->center_x = wz3__fj_p(op, 11);
  camera->center_y = wz3__fj_p(op, 12);
  camera->zoom_x = wz3__fj_p(op, 13);
  camera->zoom_y = wz3__fj_p(op, 14);
  const double rotation[3] = {wz3__fj_p(op, 3), wz3__fj_p(op, 4), wz3__fj_p(op, 5)};
  const double position[3] = {wz3__fj_p(op, 6), wz3__fj_p(op, 7), wz3__fj_p(op, 8)};
  const wz3_camera *game = &rt->fj.game_camera;
  float *m = camera->camera_space;
  if (flags & 4u) {
    memcpy(m, game->camera_space, 16 * sizeof(float));
    if ((flags & 0x300u) == 0) {
      camera->center_x = game->center_x; camera->center_y = game->center_y;
      camera->zoom_x = game->zoom_x; camera->zoom_y = game->zoom_y;
      camera->far_clip = game->far_clip; camera->near_clip = game->near_clip;
    }
  } else {
    wz3__rt_euler_turns(rotation[0], rotation[1], rotation[2], m);
    m[12] = (float)position[0]; m[13] = (float)position[1]; m[14] = (float)position[2];
  }
  if ((flags & 0x300u) == 0x100u) {
    const double up[3] = {0, 1, 0};
    const double k[3] = {m[8], m[9], m[10]};
    float i[3], next_k[3];
    double crossed[3], id[3];
    float c[3];
    wz3__fj_cross(up, k, c);
    crossed[0] = c[0]; crossed[1] = c[1]; crossed[2] = c[2];
    wz3__fj_normalize(crossed, i);
    id[0] = i[0]; id[1] = i[1]; id[2] = i[2];
    wz3__fj_cross(id, up, next_k);
    m[0] = i[0]; m[1] = i[1]; m[2] = i[2];
    m[4] = 0; m[5] = 1; m[6] = 0;
    m[8] = next_k[0]; m[9] = next_k[1]; m[10] = next_k[2];
    m[12] = (float)((double)next_k[0] * position[2]);
    m[13] = (float)((double)next_k[1] * position[2]);
    m[14] = (float)((double)next_k[2] * position[2]);
  } else if ((flags & 0x300u) == 0x200u) {
    m[12] = (float)((double)m[8] * position[2]);
    m[13] = (float)((double)m[9] * position[2]);
    m[14] = (float)((double)m[10] * position[2]);
  }
  m[15] = 1;
  camera->fog_color = wz3__fj_pu(op, 15);
  camera->fog_end = wz3__fj_p(op, 16);
  camera->fog_start = wz3__fj_p(op, 17);
  if (!(flags & 0x1000u)) camera->zoom_y *= rt->fj.aspect;
  const wz3__rt_cache *spline = wz3__rt_input_cache(rt, op, 1);
  if (wz3__rt_is_spline_cache(spline)) {
    float zoom = 1, evaluated[16];
    memset(evaluated, 0, sizeof evaluated);                    /* spline.eval(time, 0) allocates a fresh matrix */
    wz3__rt_spline_eval(rt, spline, wz3__fj_or0(rt->env.vars[WZ3__FJ_KV_TIME][0]), evaluated, &zoom);
    memcpy(camera->camera_space, evaluated, sizeof evaluated);
    camera->zoom_x *= zoom;
    camera->zoom_y *= zoom;
  }
}

static wz3_result wz3__fj_add_node(wz3_rt *rt, wz3_ipp_node **node)
{
  const wz3_result r = wz3__fj_push(&rt->fj.nodes, sizeof **node, 1, (void **)node);
  if (r == WZ3_OK) memset(*node, 0, sizeof **node);
  return r;
}

static wz3_result wz3__fj_exec_viewport(wz3_rt *rt, wz3__rt_op *op)
{
  wz3__fj *fj = &rt->fj;
  if (!op->src->n_inputs) return WZ3_OK;
  if (fj->ipp_node[op->id] >= 0) { fj->last_output = fj->ipp_node[op->id]; return WZ3_OK; }
  wz3_result r = wz3__fj_begin_render_frame(rt);
  if (r != WZ3_OK) return r;
  wz3_camera camera;
  wz3__fj_build_camera(rt, op, &camera);
  fj->current_camera = camera;
  r = wz3__rt_exec_op(rt, op->src->inputs[0]);
  if (r != WZ3_OK) return r;
  wz3_light_job fallback;
  memset(&fallback, 0, sizeof fallback);
  memcpy(fallback.position, camera.camera_space + 12, 3 * sizeof(float));
  fallback.direction[2] = 1.0f;
  fallback.color = 0x78787878u;
  fallback.amplify = 1.0f;
  fallback.range = 32.0;
  fallback.event = -1;
  if (fj->light.n == fj->b_light) {
    wz3_light_job *job;
    if ((r = wz3__fj_push(&fj->light, sizeof *job, 1, (void **)&job)) != WZ3_OK) return r;
    *job = fallback;
  }
  wz3_ipp_node *node;
  if ((r = wz3__fj_add_node(rt, &node)) != WZ3_OK) return r;
  node->type = WZ3_IPP_VIEWPORT;
  node->size = wz3__fj_pi(op, 0);
  node->flags = wz3__fj_pu(op, 1);
  node->clear_color = wz3__fj_pu(op, 2);
  node->camera = camera;
  for (int k = 0; k < 4; k++) node->crop[k] = (float)wz3__fj_p(op, 20 + k);
  node->eye_distance = (float)wz3__fj_p(op, 18);
  node->focal_distance = (float)wz3__fj_p(op, 19);
  node->paint_mode = (int32_t)((wz3__fj_pu(op, 1) >> 4) & 3u);
  node->first_mesh = fj->b_mesh; node->n_mesh = fj->mesh.n - fj->b_mesh;
  node->first_effect = fj->b_effect; node->n_effect = fj->effect.n - fj->b_effect;
  node->first_light = fj->b_light; node->n_light = fj->light.n - fj->b_light;
  node->marker_table = (uint32_t)fj->cur_table;
  node->ambient = fj->ambient;
  node->default_light = fallback;
  node->input = -1;
  node->material_op = -1;
  fj->last_output = (int32_t)fj->nodes.n - 1;
  fj->ipp_node[op->id] = fj->last_output;
  return WZ3_OK;
}

static void wz3__fj_centered_rect(const wz3__rt_op *op, uint32_t first, int centered, double out[4])
{
  const double v[4] = {wz3__fj_p(op, first), wz3__fj_p(op, first + 1), wz3__fj_p(op, first + 2), wz3__fj_p(op, first + 3)};
  if (!centered) { memcpy(out, v, sizeof v); return; }
  out[0] = v[0] - v[2] * 0.5; out[1] = v[1] - v[3] * 0.5;
  out[2] = v[0] + v[2] * 0.5; out[3] = v[1] + v[3] * 0.5;
}

static wz3_result wz3__fj_exec_layer2d(wz3_rt *rt, wz3__rt_op *op)
{
  wz3__fj *fj = &rt->fj;
  if (fj->ipp_node[op->id] >= 0) { fj->last_output = fj->ipp_node[op->id]; return WZ3_OK; }
  fj->last_output = -1;
  wz3_result r = wz3__rt_exec_inputs(rt, op);
  if (r != WZ3_OK) return r;
  const uint32_t flags = wz3__fj_pu(op, 10);
  const int32_t input = fj->last_output;
  wz3_ipp_node *node;
  if ((r = wz3__fj_add_node(rt, &node)) != WZ3_OK) return r;
  node->type = WZ3_IPP_LAYER2D;
  node->input = input;
  node->material_op = op->src->n_links ? op->src->links[0] : -1;
  node->size = input >= 0 ? ((const wz3_ipp_node *)fj->nodes.p)[input].size : wz3__fj_pi(op, 0);
  wz3__fj_centered_rect(op, 1, (flags & 0x10u) != 0, node->screen);
  wz3__fj_centered_rect(op, 5, (flags & 0x20u) != 0, node->uv);
  node->z = (float)wz3__fj_p(op, 9);
  node->flags = flags;
  node->clear_flags = (int32_t)(flags & 3u);
  fj->last_output = (int32_t)fj->nodes.n - 1;
  fj->ipp_node[op->id] = fj->last_output;
  return WZ3_OK;
}

static void wz3__fj_ipp_branch_begin(wz3_rt *rt)
{
  rt->fj.last_output = -1;
  for (uint32_t i = 0; i < rt->n_ops; i++) rt->fj.ipp_node[i] = -1;
}

static wz3_result wz3__fj_ipp_branch_end(wz3_rt *rt)
{
  if (rt->fj.last_output < 0) return WZ3_OK;
  uint32_t *slot;
  const wz3_result r = wz3__fj_push(&rt->fj.outputs, sizeof *slot, 1, (void **)&slot);
  if (r == WZ3_OK) *slot = (uint32_t)rt->fj.last_output;
  return r;
}

WZ3_API const wz3_frame *wz3_rt_frame_jobs(const wz3_rt *rt) { return rt ? &rt->fj.view : NULL; }
WZ3_API void wz3_rt_set_aspect(wz3_rt *rt, double aspect) { if (rt) rt->fj.aspect = aspect; }
