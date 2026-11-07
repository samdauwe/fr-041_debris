/* ======================================================================== */
/*  Draw list (engine.cpp, up to the sorted item list)                      */
/*                                                                          */
/*  Insert tracker, native head-inserted job order, pass sort identity,     */
/*  viewport plan, effect items and appendMeshRenderItems with its          */
/*  per-group culling.                                                      */
/* ======================================================================== */

/* ---- insert tracker ---- */

WZ3_API int wz3_material_insert_kind(const wz3_material *material)
{
  if (!material) return WZ3_INSERT_NONE;
  if (material->system == 20) return WZ3_INSERT_MATERIAL20;
  if (material->system != 11) return WZ3_INSERT_NONE;
  /* the insert topology is fixed at init, like the pass list: prefer the captured value */
  return (material->multi_flags & 0x1eu) ? WZ3_INSERT_MATERIAL11 : WZ3_INSERT_NONE;
}

WZ3_API int wz3_insert_action(int insert_kind, uint8_t usage)
{
  if (insert_kind != WZ3_INSERT_MATERIAL11) return WZ3_INSERT_ACTION_NONE;
  if (usage == WZ3_USAGE_BASE) return WZ3_INSERT_ACTION_CLEAR_DESTINATION_ALPHA;
  if (usage == WZ3_USAGE_POSTLIGHT) return WZ3_INSERT_ACTION_ADD_DESTINATION_ALPHA;
  return WZ3_INSERT_ACTION_NONE;
}

WZ3_API int wz3_render_usage_changed(const wz3_render_item *previous, const wz3_render_item *next)
{
  if (!previous || !next) return 1;
  return previous->render_pass != next->render_pass || previous->usage != next->usage || previous->light_index != next->light_index;
}

WZ3_API void wz3_insert_tracker_init(wz3_insert_tracker *t, const wz3_draw_item *items, uint32_t n)
{
  /* materialInsertPlan: per render pass the strongest insert (2.0 over 1.1) of its non-effect items */
  memset(t, 0, sizeof *t);
  for (uint32_t i = 0; i < n; i++) {
    const wz3_render_item *item = &items[i].key;
    if (item->is_effect || item->insert_kind == WZ3_INSERT_NONE) continue;
    uint8_t *slot = &t->kind[(uint32_t)item->render_pass & 255u];
    if (item->insert_kind > *slot) *slot = item->insert_kind;
  }
}

static int wz3__tracker_finish_item(const wz3_insert_tracker *t, const wz3_render_item *item)
{
  return wz3_insert_action(t->kind[(uint32_t)item->render_pass & 255u], item->usage);
}

WZ3_API int wz3_insert_tracker_transition(wz3_insert_tracker *t, const wz3_draw_item *item)
{
  int action = WZ3_INSERT_ACTION_NONE;
  if (t->has_previous && wz3_render_usage_changed(&t->previous, &item->key)) action = wz3__tracker_finish_item(t, &t->previous);
  t->previous = item->key;
  t->has_previous = 1;
  return action;
}

WZ3_API int wz3_insert_tracker_finish(wz3_insert_tracker *t)
{
  const int action = t->has_previous ? wz3__tracker_finish_item(t, &t->previous) : WZ3_INSERT_ACTION_NONE;
  t->has_previous = 0;
  return action;
}

WZ3_API uint32_t wz3_head_inserted_job(uint32_t count, uint32_t position) { return count - 1u - position; }

/* ---- pass sort identity ---- */

WZ3_API void wz3_pass_sort_ids_init(wz3_pass_sort_ids *s)
{
  memset(s, 0, sizeof *s);
  s->next = 1;
}

WZ3_API void wz3_pass_sort_ids_free(wz3_pass_sort_ids *s)
{
  WZ3_FREE(s->keys);
  WZ3_FREE(s->ids);
  wz3_pass_sort_ids_init(s);
}

static uint32_t wz3__sort_hash(uint64_t key)
{
  key ^= key >> 33; key *= 0xff51afd7ed558ccdull; key ^= key >> 33;
  return (uint32_t)key;
}

static int wz3__sort_grow(wz3_pass_sort_ids *s)
{
  const uint32_t capacity = s->capacity ? s->capacity * 2u : 256u;
  uint64_t *keys = (uint64_t *)WZ3_MALLOC((size_t)capacity * sizeof *keys);
  int32_t *ids = (int32_t *)WZ3_MALLOC((size_t)capacity * sizeof *ids);
  if (!keys || !ids) { WZ3_FREE(keys); WZ3_FREE(ids); return 0; }
  memset(keys, 0, (size_t)capacity * sizeof *keys);
  for (uint32_t i = 0; i < s->capacity; i++) {
    if (!s->keys[i]) continue;
    uint32_t at = wz3__sort_hash(s->keys[i]) & (capacity - 1u);
    while (keys[at]) at = (at + 1u) & (capacity - 1u);
    keys[at] = s->keys[i]; ids[at] = s->ids[i];
  }
  WZ3_FREE(s->keys); WZ3_FREE(s->ids);
  s->keys = keys; s->ids = ids; s->capacity = capacity;
  return 1;
}

WZ3_API int32_t wz3_pass_sort_id(wz3_pass_sort_ids *s, int32_t material_op, int32_t pass_index)
{
  /* never zero: material_op >= -1 and pass_index >= -1 give non-zero halves */
  const uint64_t key = ((uint64_t)(uint32_t)(material_op + 2) << 32) | (uint32_t)(pass_index + 2);
  if (s->capacity) {
    uint32_t at = wz3__sort_hash(key) & (s->capacity - 1u);
    while (s->keys[at]) {
      if (s->keys[at] == key) return s->ids[at];
      at = (at + 1u) & (s->capacity - 1u);
    }
  }
  if ((s->count + 1u) * 2u > s->capacity && !wz3__sort_grow(s)) return -1;
  uint32_t at = wz3__sort_hash(key) & (s->capacity - 1u);
  while (s->keys[at]) at = (at + 1u) & (s->capacity - 1u);
  s->keys[at] = key;
  s->ids[at] = s->next++;
  s->count++;
  return s->ids[at];
}

/* ---- draw list container ---- */

WZ3_API void wz3_draw_list_init(wz3_draw_list *list) { memset(list, 0, sizeof *list); }
WZ3_API void wz3_draw_list_clear(wz3_draw_list *list) { list->n_items = 0; list->n_shadow_groups = 0; }
WZ3_API void wz3_draw_list_free(wz3_draw_list *list)
{
  WZ3_FREE(list->items);
  WZ3_FREE(list->shadow_groups);
  wz3_draw_list_init(list);
}

static wz3_draw_item *wz3__draw_push(wz3_draw_list *list)
{
  if (list->n_items == list->cap_items) {
    const uint32_t capacity = list->cap_items ? list->cap_items * 2u : 256u;
    wz3_draw_item *items = (wz3_draw_item *)WZ3_MALLOC((size_t)capacity * sizeof *items);
    if (!items) return NULL;
    if (list->n_items) memcpy(items, list->items, (size_t)list->n_items * sizeof *items);
    WZ3_FREE(list->items);
    list->items = items; list->cap_items = capacity;
  }
  wz3_draw_item *item = &list->items[list->n_items++];
  memset(item, 0, sizeof *item);
  item->group = -1;
  item->key.light_index = -1;
  return item;
}

static int wz3__draw_push_group(wz3_draw_list *list, uint32_t group)
{
  if (list->n_shadow_groups == list->cap_shadow_groups) {
    const uint32_t capacity = list->cap_shadow_groups ? list->cap_shadow_groups * 2u : 256u;
    uint32_t *groups = (uint32_t *)WZ3_MALLOC((size_t)capacity * sizeof *groups);
    if (!groups) return 0;
    if (list->n_shadow_groups) memcpy(groups, list->shadow_groups, (size_t)list->n_shadow_groups * sizeof *groups);
    WZ3_FREE(list->shadow_groups);
    list->shadow_groups = groups; list->cap_shadow_groups = capacity;
  }
  list->shadow_groups[list->n_shadow_groups++] = group;
  return 1;
}

static int wz3__draw_item_compare(const void *pa, const void *pb)
{
  return wz3__item_compare(&((const wz3_draw_item *)pa)->key, &((const wz3_draw_item *)pb)->key);
}

WZ3_API void wz3_draw_list_sort(wz3_draw_list *list)
{
  for (uint32_t i = 0; i < list->n_items; i++) { list->items[i].key.sort_order = (int32_t)i; list->items[i].key.sequence = i; }
  if (list->n_items > 1) qsort(list->items, list->n_items, sizeof *list->items, wz3__draw_item_compare);
}

/* ---- viewport plan ---- */

WZ3_API void wz3_viewport_plan_make(const wz3_camera *camera, const wz3_light_job *const *lights, uint32_t n_lights, wz3_viewport_plan *out)
{
  memset(out, 0, sizeof *out);
  wz3_camera_matrices(camera, &out->matrices);
  out->n_view_planes = wz3_view_frustum_planes(out->matrices.view_projection, out->view_planes);
  if (n_lights > WZ3_MAX_LIGHTS) n_lights = WZ3_MAX_LIGHTS;
  out->n_lights = n_lights;
  for (uint32_t i = 0; i < n_lights; i++) {
    const wz3_light_job *light = lights[i];
    out->lights[i] = light;
    /* InsertLightJob computes this rectangle once and shares it between the scissor and ZFailVolume */
    out->has_rect[i] = (uint8_t)wz3_light_sphere_bounds(light->position, light->range, out->matrices.view, camera->zoom_x, camera->zoom_y, out->rect[i]);
    out->n_shadow_planes[i] = light->directional ? 0u : wz3_shadow_view_frustum_planes(out->matrices.view_projection, light->position, out->shadow_planes[i]);
    out->n_zfail_planes[i] = wz3_shadow_zfail_planes(camera, light, out->matrices.view, out->has_rect[i] ? out->rect[i] : NULL, out->zfail_planes[i]);
  }
}

/* ---- mesh items ---- */

static int wz3__world_bounds(const wz3_mesh_job *job, const float *minimum_f, const float *maximum_f, wz3_bounds *out)
{
  /* meshJobWorldBounds with composeInstanceMatrices of a bounded job: one matrix, or none for an empty instance list */
  float matrices[16];
  double minimum[3], maximum[3];
  for (int i = 0; i < 3; i++) { minimum[i] = minimum_f[i]; maximum[i] = maximum_f[i]; }
  const uint32_t n = wz3_compose_instances(job->matrix, job->has_instances, NULL, job->n_instances, matrices);
  return wz3_job_world_bounds(minimum, maximum, matrices, n, out);
}

static int wz3__caster_may_affect_prepared(const wz3_light_job *light, const wz3_bounds *bounds, const double (*planes)[4], uint32_t n_planes)
{
  if (light->directional) return 1;
  return !wz3_bounds_outside_planes(bounds, planes, n_planes);
}

WZ3_API wz3_result wz3_draw_list_append_mesh(wz3_draw_list *list, const wz3_draw_source *source, const wz3_mesh_job *job, uint32_t job_index,
                                             const wz3_geometry *geometry, const wz3_viewport_plan *plan, wz3_pass_sort_ids *ids,
                                             wz3_draw_stats *stats)
{
  /* BuildPaintJobs gives animated and shader-instanced jobs an enormous bbox: they bypass the camera frustum test and
     every per-light spatial rejection */
  const int unbounded = geometry->animated || (job->has_instances && job->n_instances > 0);
  wz3_bounds world;
  int has_world = 0;
  uint8_t light_visible[WZ3_MAX_LIGHTS];
  const uint32_t n_lights = plan->n_lights;
  const uint32_t shadow_first = list->n_shadow_groups;
  uint32_t shadow_count = 0;
  int have_first_shadow = 0;
  wz3_draw_item first_shadow;
  memset(&first_shadow, 0, sizeof first_shadow);
  /* a geometry without groups is one group over every index with slot 0 (no material) */
  wz3_geometry_group whole;
  memset(&whole, 0, sizeof whole);
  whole.material = WZ3_GROUP_MATERIAL_NONE; whole.count = geometry->n_indices; whole.has_bounds = 1;
  memcpy(whole.bounds_min, geometry->bounds_min, sizeof whole.bounds_min);
  memcpy(whole.bounds_max, geometry->bounds_max, sizeof whole.bounds_max);
  const uint32_t n_groups = geometry->n_groups ? geometry->n_groups : 1u;
  for (uint32_t g = 0; g < n_groups; g++) {
    const wz3_geometry_group *group = geometry->n_groups ? &geometry->groups[g] : &whole;
    wz3_bounds group_world;
    int has_group_world = 0;
    if (!unbounded) {
      const int own = group->has_bounds;
      has_group_world = wz3__world_bounds(job, own ? group->bounds_min : geometry->bounds_min, own ? group->bounds_max : geometry->bounds_max, &group_world);
    }
    const int ordinary_visible = unbounded || !plan->n_view_planes || !has_group_world ||
                                 !wz3_bounds_outside_planes(&group_world, (const double (*)[4])plan->view_planes, plan->n_view_planes);
    uint8_t group_light_visible[WZ3_MAX_LIGHTS];
    int group_light_ready = 0;
    const int32_t material_op = group->material >= 0 ? group->material : -1;
    const wz3_material *material = material_op >= 0 && source->material ? source->material(source->user, material_op) : NULL;
    const uint32_t n_passes = material && material->n_passes ? material->n_passes : 1u;
    const int insert_kind = wz3_material_insert_kind(material);
    for (uint32_t p = 0; p < n_passes; p++) {
      const wz3_material_pass *pass = (material && material->n_passes) ? &material->passes[p] : NULL;
      const int32_t pass_index = pass ? (int32_t)p : -1;
      const int32_t sort_material = wz3_pass_sort_id(ids, pass ? material_op : -1, pass_index);
      if (sort_material < 0) return WZ3_ERR_OUT_OF_MEMORY;
      const uint8_t usage = pass ? pass->usage : (uint8_t)WZ3_USAGE_LIGHT;
      const int32_t pass_render_pass = pass ? pass->render_pass : 0;
      const int32_t material_pass = (int32_t)(((uint32_t)pass_render_pass + (uint32_t)group->pass) & 255u);
      const int64_t adjusted = (int64_t)material_pass + (int64_t)job->pass_adjust;
      const int32_t render_pass = (int32_t)(adjusted < 0 ? 0 : adjusted > 255 ? 255 : adjusted);
      if (usage == WZ3_USAGE_SHADOW) {
        if (!wz3__draw_push_group(list, g)) return WZ3_ERR_OUT_OF_MEMORY;
        shadow_count++;
        if (!have_first_shadow) {
          have_first_shadow = 1;
          first_shadow.material_op = material_op;
          first_shadow.pass_index = pass_index;
          first_shadow.key.render_pass = render_pass;
          first_shadow.key.usage = usage;
          first_shadow.key.sort_material = sort_material;
          first_shadow.key.insert_kind = (uint8_t)insert_kind;
        }
      } else if (usage == WZ3_USAGE_LIGHT) {
        if (stats) stats->candidate_view++;
        if (!ordinary_visible) { if (stats) stats->culled_view++; continue; }
        if (!group_light_ready) {
          group_light_ready = 1;
          for (uint32_t i = 0; i < n_lights; i++) group_light_visible[i] = (uint8_t)wz3_light_intersects_bounds(plan->lights[i], has_group_world ? &group_world : NULL);
        }
        for (uint32_t i = 0; i < n_lights; i++) {
          if (stats) stats->candidate_light++;
          if (!group_light_visible[i]) { if (stats) stats->culled_light++; continue; }
          wz3_draw_item *item = wz3__draw_push(list);
          if (!item) return WZ3_ERR_OUT_OF_MEMORY;
          item->kind = WZ3_DRAW_PASS; item->job = job_index; item->material_op = material_op;
          item->pass_index = pass_index; item->group = (int32_t)g;
          item->key.render_pass = render_pass; item->key.usage = usage; item->key.light_index = (int32_t)i;
          item->key.sort_material = sort_material; item->key.insert_kind = (uint8_t)insert_kind;
        }
      } else {
        if (stats) stats->candidate_view++;
        if (!ordinary_visible) { if (stats) stats->culled_view++; continue; }
        wz3_draw_item *item = wz3__draw_push(list);
        if (!item) return WZ3_ERR_OUT_OF_MEMORY;
        item->kind = WZ3_DRAW_PASS; item->job = job_index; item->material_op = material_op;
        item->pass_index = pass_index; item->group = (int32_t)g;
        item->key.render_pass = render_pass; item->key.usage = usage;
        item->key.sort_material = sort_material; item->key.insert_kind = (uint8_t)insert_kind;
      }
    }
  }
  if (have_first_shadow && shadow_count) {
    if (!unbounded) has_world = wz3__world_bounds(job, geometry->bounds_min, geometry->bounds_max, &world);
    for (uint32_t i = 0; i < n_lights; i++) light_visible[i] = (uint8_t)wz3_light_intersects_bounds(plan->lights[i], has_world ? &world : NULL);
    for (uint32_t i = 0; i < n_lights; i++) {
      const wz3_light_job *light = plan->lights[i];
      if (!(light->flags & 2u)) continue;
      if (stats) stats->candidate_shadow++;
      const wz3_bounds *bounds = has_world ? &world : NULL;
      const int visible = light_visible[i] &&
                          wz3__caster_may_affect_prepared(light, bounds, (const double (*)[4])plan->shadow_planes[i], plan->n_shadow_planes[i]);
      if (!visible) {
        if (stats) { if (light_visible[i]) stats->culled_shadow_frustum++; else stats->culled_shadow_sphere++; }
        continue;
      }
      wz3_draw_item *item = wz3__draw_push(list);
      if (!item) return WZ3_ERR_OUT_OF_MEMORY;
      *item = first_shadow;
      item->kind = WZ3_DRAW_SHADOW; item->job = job_index; item->group = -1;
      item->shadow_first = shadow_first; item->shadow_count = shadow_count;
      item->key.light_index = (int32_t)i;
      item->shadow_zfail = (uint8_t)wz3_caster_uses_zfail(bounds, (const double (*)[4])plan->zfail_planes[i], plan->n_zfail_planes[i]);
    }
  } else {
    /* no shadow item: the group references this job added are unused */
    list->n_shadow_groups = shadow_first;
  }
  return WZ3_OK;
}

WZ3_API wz3_result wz3_draw_list_build(wz3_draw_list *list, const wz3_draw_source *source, const wz3_frame *frame,
                                       const wz3_ipp_node *viewport, wz3_pass_sort_ids *ids, wz3_viewport_plan *plan_out, wz3_draw_stats *stats)
{
  wz3_viewport_plan local;
  wz3_viewport_plan *plan = plan_out ? plan_out : &local;
  uint32_t indices[WZ3_MAX_LIGHTS];
  const wz3_light_job *lights[WZ3_MAX_LIGHTS];
  const uint32_t n = wz3_select_active_lights(frame, viewport, WZ3_MAX_LIGHTS, indices);
  for (uint32_t i = 0; i < n; i++)
    lights[i] = indices[i] == (uint32_t)-1 ? &viewport->default_light : &frame->light_jobs[viewport->first_light + indices[i]];
  wz3_viewport_plan_make(&viewport->camera, lights, n, plan);
  wz3_draw_list_clear(list);
  for (uint32_t position = 0; position < viewport->n_effect; position++) {
    const uint32_t index = wz3_head_inserted_job(viewport->n_effect, position);
    wz3_draw_item *item = wz3__draw_push(list);
    if (!item) return WZ3_ERR_OUT_OF_MEMORY;
    const wz3_effect_job *effect_job = &frame->effect_jobs[viewport->first_effect + index];
    const wz3_effect *effect = source->effect ? source->effect(source->user, effect_job->effect_op) : NULL;
    if (!effect) { list->n_items--; continue; }
    wz3_effect_render_item_make(effect, effect_job, (int32_t)index, &item->key);
    item->key.light_index = -1;
    item->kind = WZ3_DRAW_EFFECT;
    item->job = index;
    item->material_op = -1; item->pass_index = -1;
  }
  for (uint32_t position = 0; position < viewport->n_mesh; position++) {
    const uint32_t index = wz3_head_inserted_job(viewport->n_mesh, position);
    const wz3_mesh_job *job = &frame->mesh_jobs[viewport->first_mesh + index];
    if (job->mesh_op < 0) continue;
    const wz3_geometry *geometry = source->geometry ? source->geometry(source->user, job) : NULL;
    if (!geometry) continue;
    const wz3_result r = wz3_draw_list_append_mesh(list, source, job, index, geometry, plan, ids, stats);
    if (r != WZ3_OK) return r;
  }
  return WZ3_OK;
}
