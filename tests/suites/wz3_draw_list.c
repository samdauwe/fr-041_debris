/* Draw-list check for wz3.h against src/renderer.js.
 *
 *   test_wz3_draw_list <fixtures dir> [--print]
 *
 * Replays tools/dump_draw_list.mjs: the insert tracker, native job order, pass sort identity, per-group boxes of prepared
 * geometry and appendMeshRenderItems with its culling, then the stable sort and the insert tracker over the sorted list.
 * One sort-identity registry lives for the whole file, like the JavaScript counter. Then C-only checks: allocation failure
 * at every allocation of a list and the registry, and the list API's edge cases.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static long g_alloc_count, g_fail_at = -1, g_live;
static void *test_malloc(size_t n)
{
  if (g_alloc_count++ == g_fail_at) return NULL;
  size_t *p = (size_t *)malloc(n + 16);
  if (!p) return NULL;
  *p = n;
  g_live++;
  return (char *)p + 16;
}
#if defined(__GNUC__)
__attribute__((noinline))
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
#include "support/gate_common.h"

/* ---- the material pool of the `mat` lines ---- */
static wz3_material g_pool[8];
static wz3_material_pass g_pool_passes[8][8];
static const wz3_material *pool_material(void *user, int32_t op)
{
  (void)user;
  return op >= 0 && op < 8 ? &g_pool[op] : NULL;
}

static wz3_pass_sort_ids g_ids;

/* ---- geometry from a line ---- */
typedef struct geometry_in {
  wz3_geometry g;
  float positions[12 * 3];
  uint32_t indices[64];
  wz3_geometry_group groups[8];
} geometry_in;

static void rgeometry(reader *r, geometry_in *gi)
{
  memset(gi, 0, sizeof *gi);
  wz3_geometry *g = &gi->g;
  g->n_vertices = ru32(r);
  for (uint32_t i = 0; i < g->n_vertices * 3; i++) gi->positions[i] = rf32(r);
  g->n_indices = ru32(r);
  for (uint32_t i = 0; i < g->n_indices; i++) gi->indices[i] = ru32(r);
  for (int i = 0; i < 3; i++) g->bounds_min[i] = rf32(r);
  for (int i = 0; i < 3; i++) g->bounds_max[i] = rf32(r);
  g->n_groups = ru32(r);
  for (uint32_t i = 0; i < g->n_groups; i++) {
    gi->groups[i].material = ri32(r);
    gi->groups[i].slot = ri32(r);
    gi->groups[i].pass = ri32(r);
    gi->groups[i].start = ru32(r);
    gi->groups[i].count = ru32(r);
  }
  g->positions = gi->positions;
  g->indices = gi->indices;
  g->groups = gi->groups;
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

static void rlight(reader *r, wz3_light_job *l)
{
  memset(l, 0, sizeof *l);
  l->directional = (uint8_t)ru32(r);
  for (int i = 0; i < 3; i++) l->position[i] = rf32(r);
  l->range = rf64(r);
  l->flags = ru32(r);
  l->amplify = 1.0f;
}

static void hash_draw_item(hash *h, const wz3_draw_list *list, const wz3_draw_item *it)
{
  hu(h, it->kind == WZ3_DRAW_SHADOW ? 1u : 0u);
  hi(h, (int32_t)it->job);
  hi(h, it->group);
  hi(h, it->material_op);
  hi(h, it->pass_index);
  hi(h, it->key.render_pass);
  hu(h, it->key.usage);
  hi(h, it->key.light_index);
  hi(h, it->key.sort_material);
  hu(h, it->shadow_zfail);
  if (it->kind == WZ3_DRAW_SHADOW) {
    hu(h, it->shadow_count);
    for (uint32_t i = 0; i < it->shadow_count; i++) hi(h, (int32_t)list->shadow_groups[it->shadow_first + i]);
  }
}

static struct {
  unsigned shadow_items, zfail_items, culled, unbounded_jobs, default_group_jobs, sorted_actions, light_items;
} g_seen;

static uint32_t run_append(reader *r)
{
  wz3_camera camera;
  rcamera(r, &camera);
  wz3_light_job lights[WZ3_MAX_LIGHTS];
  const wz3_light_job *light_ptrs[WZ3_MAX_LIGHTS];
  const uint32_t n_lights = ru32(r);
  for (uint32_t i = 0; i < n_lights; i++) {
    rlight(r, &lights[i]);
    light_ptrs[i] = &lights[i];
  }
  wz3_viewport_plan plan;
  wz3_viewport_plan_make(&camera, light_ptrs, n_lights, &plan);
  const uint32_t n_jobs = ru32(r);
  static wz3_mesh_job jobs[4];
  static geometry_in geometries[4];
  for (uint32_t j = 0; j < n_jobs; j++) {
    memset(&jobs[j], 0, sizeof jobs[j]);
    const uint32_t animated = ru32(r), mode = ru32(r), count = ru32(r);
    jobs[j].pass_adjust = ri32(r);
    rmat(r, jobs[j].matrix);
    jobs[j].has_instances = mode != 0;
    jobs[j].n_instances = count;
    rgeometry(r, &geometries[j]);
    wz3_geometry_group_bounds(&geometries[j].g);
    geometries[j].g.animated = (uint8_t)animated;
    if (animated || (mode && count)) g_seen.unbounded_jobs++;
    if (!geometries[j].g.n_groups) g_seen.default_group_jobs++;
  }
  wz3_draw_source source;
  memset(&source, 0, sizeof source);
  source.material = pool_material;
  wz3_draw_list list;
  wz3_draw_list_init(&list);
  wz3_draw_stats stats;
  memset(&stats, 0, sizeof stats);
  for (uint32_t p = 0; p < n_jobs; p++) {
    const uint32_t j = wz3_head_inserted_job(n_jobs, p);
    const wz3_result res = wz3_draw_list_append_mesh(&list, &source, &jobs[j], j, &geometries[j].g, &plan, &g_ids, &stats);
    CHECK(res == WZ3_OK, "append: %s", wz3_result_string(res));
  }
  hash h;
  hash_init(&h);
  hu(&h, stats.candidate_view);
  hu(&h, stats.culled_view);
  hu(&h, stats.candidate_light);
  hu(&h, stats.culled_light);
  hu(&h, stats.candidate_shadow);
  hu(&h, stats.culled_shadow_frustum);
  hu(&h, stats.culled_shadow_sphere);
  hu(&h, list.n_items);
  for (uint32_t i = 0; i < list.n_items; i++) {
    hash_draw_item(&h, &list, &list.items[i]);
    if (list.items[i].kind == WZ3_DRAW_SHADOW) {
      g_seen.shadow_items++;
      g_seen.zfail_items += list.items[i].shadow_zfail;
    }
    if (list.items[i].key.usage == WZ3_USAGE_LIGHT) g_seen.light_items++;
  }
  g_seen.culled += stats.culled_view + stats.culled_light + stats.culled_shadow_frustum + stats.culled_shadow_sphere;
  wz3_draw_list_sort(&list);
  for (uint32_t i = 0; i < list.n_items; i++) hash_draw_item(&h, &list, &list.items[i]);
  wz3_insert_tracker tracker;
  wz3_insert_tracker_init(&tracker, list.items, list.n_items);
  for (uint32_t i = 0; i < list.n_items; i++) {
    const int action = wz3_insert_tracker_transition(&tracker, &list.items[i]);
    hu(&h, (uint32_t)action);
    if (action) g_seen.sorted_actions++;
  }
  hu(&h, (uint32_t)wz3_insert_tracker_finish(&tracker));
  wz3_draw_list_free(&list);
  return h.h;
}

static uint32_t run_case(const char *kind, reader *r)
{
  hash h;
  hash_init(&h);
  if (!strcmp(kind, "mat")) {
    const uint32_t op = ru32(r);
    wz3_material *m = &g_pool[op];
    memset(m, 0, sizeof *m);
    m->system = (uint8_t)ru32(r);
    m->multi_flags = ru32(r);
    m->n_passes = ru32(r);
    m->passes = g_pool_passes[op];
    for (uint32_t i = 0; i < m->n_passes; i++) {
      memset(&g_pool_passes[op][i], 0, sizeof g_pool_passes[op][i]);
      g_pool_passes[op][i].usage = (uint8_t)ru32(r);
      g_pool_passes[op][i].render_pass = ri32(r);
      g_pool_passes[op][i].owner = op;
    }
    return 0xfffffffeu;
  } else if (!strcmp(kind, "insertkind")) {
    wz3_material m;
    memset(&m, 0, sizeof m);
    const uint32_t system = ru32(r);
    m.multi_flags = ru32(r);
    m.system = (uint8_t)system;
    hu(&h, (uint32_t)wz3_material_insert_kind(system == 0 || system == 1 ? NULL : &m));
  } else if (!strcmp(kind, "insertaction")) {
    const uint32_t k = ru32(r), usage = ru32(r);
    hu(&h, (uint32_t)wz3_insert_action((int)k, (uint8_t)usage));
  } else if (!strcmp(kind, "head")) {
    const uint32_t n = ru32(r);
    for (uint32_t p = 0; p < n; p++) {
      const uint32_t index = wz3_head_inserted_job(n, p);
      hu(&h, index);
      hu(&h, index);
    }
  } else if (!strcmp(kind, "tracker")) {
    const uint32_t count = ru32(r);
    wz3_draw_item items[32];
    memset(items, 0, sizeof items);
    for (uint32_t i = 0; i < count; i++) {
      items[i].key.is_effect = (uint8_t)ru32(r);
      items[i].key.insert_kind = (uint8_t)ru32(r);
      items[i].key.render_pass = ri32(r);
      items[i].key.usage = (uint8_t)ru32(r);
      items[i].key.light_index = ri32(r);
    }
    wz3_insert_tracker t;
    wz3_insert_tracker_init(&t, items, count);
    for (uint32_t i = 0; i < count; i++) hu(&h, (uint32_t)wz3_insert_tracker_transition(&t, &items[i]));
    hu(&h, (uint32_t)wz3_insert_tracker_finish(&t));
  } else if (!strcmp(kind, "sortid")) {
    const uint32_t count = ru32(r);
    for (uint32_t i = 0; i < count; i++) {
      const int32_t op = ri32(r), pass = ri32(r);
      hi(&h, wz3_pass_sort_id(&g_ids, op, pass));
    }
  } else if (!strcmp(kind, "gbounds")) {
    static geometry_in gi;
    rgeometry(r, &gi);
    wz3_geometry_group_bounds(&gi.g);
    for (uint32_t i = 0; i < gi.g.n_groups; i++) {
      const wz3_geometry_group *g = &gi.g.groups[i];
      hu(&h, g->has_bounds);
      if (g->has_bounds) {
        hfs(&h, g->bounds_min, 3);
        hfs(&h, g->bounds_max, 3);
      }
    }
  } else if (!strcmp(kind, "append")) {
    return run_append(r);
  } else {
    return 0xffffffffu;
  }
  return h.h;
}

/* allocation failure at every allocation of a list built from a fixture-like scene must leave nothing behind */
static void test_oom(void)
{
  wz3_pass_sort_ids ids;
  wz3_draw_list list;
  wz3_material material;
  wz3_material_pass passes[4];
  memset(&material, 0, sizeof material);
  memset(passes, 0, sizeof passes);
  material.system = 11;
  material.n_passes = 4;
  material.passes = passes;
  passes[0].usage = WZ3_USAGE_BASE;
  passes[1].usage = WZ3_USAGE_LIGHT;
  passes[2].usage = WZ3_USAGE_SHADOW;
  passes[3].usage = WZ3_USAGE_SHADOW;
  g_pool[0] = material;
  static geometry_in gi;
  memset(&gi, 0, sizeof gi);
  gi.g.n_vertices = 3;
  gi.g.n_indices = 3;
  gi.g.n_groups = 1;
  gi.positions[3] = 1;
  gi.positions[7] = 1;
  gi.indices[1] = 1;
  gi.indices[2] = 2;
  gi.groups[0].material = 0;
  gi.groups[0].count = 3;
  gi.g.positions = gi.positions;
  gi.g.indices = gi.indices;
  gi.g.groups = gi.groups;
  gi.g.bounds_max[0] = gi.g.bounds_max[1] = 1;
  wz3_geometry_group_bounds(&gi.g);
  wz3_light_job lights[3];
  const wz3_light_job *light_ptrs[3];
  memset(lights, 0, sizeof lights);
  for (int i = 0; i < 3; i++) {
    lights[i].directional = 1;
    lights[i].flags = 2;
    lights[i].amplify = 1;
    light_ptrs[i] = &lights[i];
  }
  wz3_camera camera;
  memset(&camera, 0, sizeof camera);
  wz3_mat4_identity(camera.camera_space);
  camera.zoom_x = camera.zoom_y = 1;
  camera.near_clip = 0.125;
  wz3_viewport_plan plan;
  wz3_viewport_plan_make(&camera, light_ptrs, 3, &plan);
  wz3_mesh_job job;
  memset(&job, 0, sizeof job);
  wz3_mat4_identity(job.matrix);
  wz3_draw_source source;
  memset(&source, 0, sizeof source);
  source.material = pool_material;
  /* with a pre-grown list the shadow-group array is the only failing allocation: first count the allocations */
  long total = 0;
  for (int pass = 0; pass < 2; pass++) {
    for (long fail = 0; fail < (pass ? total : 1); fail++) {
      g_alloc_count = 0;
      g_fail_at = pass ? fail : -1;
      wz3_pass_sort_ids_init(&ids);
      wz3_draw_list_init(&list);
      wz3_result res = WZ3_OK;
      for (int rep = 0; rep < 300 && res == WZ3_OK; rep++) { /* enough items to grow both arrays and the registry */
        job.pass_adjust = rep;
        res = wz3_draw_list_append_mesh(&list, &source, &job, 0, &gi.g, &plan, &ids, NULL);
      }
      if (!pass)
        total = g_alloc_count;
      else
        CHECK(res == WZ3_ERR_OUT_OF_MEMORY, "failure %ld of %ld not reported: %s", fail, total, wz3_result_string(res));
      wz3_draw_list_free(&list);
      wz3_pass_sort_ids_free(&ids);
      CHECK(g_live == 0, "failure %ld leaked %ld blocks", fail, g_live);
    }
  }
  g_fail_at = -1;
  /* the registry alone */
  wz3_pass_sort_ids_init(&ids);
  for (int32_t op = -1; op < 600; op++) CHECK(wz3_pass_sort_id(&ids, op, op < 0 ? -1 : op % 7) == op + 2, "id of %d", (int)op);
  CHECK(wz3_pass_sort_id(&ids, 17, 3) == 19, "ids are stable");
  wz3_pass_sort_ids_free(&ids);
  CHECK(g_live == 0, "registry leaked %ld blocks", g_live);
}

/* a viewport without light jobs draws with its default light (Exec_IPP_Viewport's camera light) */
static const wz3_geometry *one_geometry(void *user, const wz3_mesh_job *job)
{
  (void)job;
  return (const wz3_geometry *)user;
}

static wz3_effect g_effects[2];
static const wz3_effect *pool_effect(void *user, uint32_t op)
{
  (void)user;
  return op < 2 ? &g_effects[op] : NULL;
}

static void test_default_light(void)
{
  static geometry_in gi;
  memset(&gi, 0, sizeof gi);
  gi.g.n_vertices = 3;
  gi.g.n_indices = 3;
  gi.g.n_groups = 1;
  gi.positions[3] = 1;
  gi.positions[7] = 1;
  gi.positions[2] = gi.positions[5] = gi.positions[8] = 5;
  gi.indices[1] = 1;
  gi.indices[2] = 2;
  gi.groups[0].material = WZ3_GROUP_MATERIAL_NONE;
  gi.groups[0].count = 3;
  gi.g.positions = gi.positions;
  gi.g.indices = gi.indices;
  gi.g.groups = gi.groups;
  gi.g.bounds_max[0] = gi.g.bounds_max[1] = 1;
  gi.g.bounds_min[2] = gi.g.bounds_max[2] = 5;
  wz3_geometry_group_bounds(&gi.g);
  static wz3_ipp_node nodes[1];
  static wz3_mesh_job jobs[2];
  static wz3_light_job lights[3];
  memset(nodes, 0, sizeof nodes);
  memset(jobs, 0, sizeof jobs);
  memset(lights, 0, sizeof lights);
  wz3_mat4_identity(nodes[0].camera.camera_space);
  nodes[0].camera.zoom_x = nodes[0].camera.zoom_y = 1;
  nodes[0].camera.near_clip = 0.125;
  nodes[0].type = WZ3_IPP_VIEWPORT;
  nodes[0].default_light.directional = 1;
  nodes[0].default_light.amplify = 1;
  nodes[0].default_light.flags = 2;
  nodes[0].first_mesh = 1;
  nodes[0].n_mesh = 1;
  wz3_mat4_identity(jobs[1].matrix);
  jobs[1].mesh_op = 5;
  wz3_frame frame;
  memset(&frame, 0, sizeof frame);
  frame.n_nodes = 1;
  frame.nodes = nodes;
  frame.mesh_jobs = jobs;
  frame.light_jobs = lights;
  wz3_draw_source source;
  memset(&source, 0, sizeof source);
  source.user = &gi.g;
  source.geometry = one_geometry;
  wz3_pass_sort_ids ids;
  wz3_pass_sort_ids_init(&ids);
  wz3_draw_list list;
  wz3_draw_list_init(&list);
  wz3_viewport_plan plan;
  wz3_draw_stats stats;
  memset(&stats, 0, sizeof stats);
  CHECK(wz3_draw_list_build(&list, &source, &frame, &nodes[0], &ids, &plan, &stats) == WZ3_OK, "build");
  CHECK(plan.n_lights == 1 && plan.lights[0] == &nodes[0].default_light, "the default light is selected");
  CHECK(list.n_items == 1 && list.items[0].kind == WZ3_DRAW_PASS && list.items[0].key.light_index == 0 && list.items[0].key.usage == WZ3_USAGE_LIGHT &&
          list.items[0].job == 0 && list.items[0].material_op == -1 && list.items[0].pass_index == -1,
        "one default-material light item (%u items)", list.n_items);
  CHECK(stats.candidate_view == 1 && stats.candidate_light == 1 && stats.culled_view == 0, "counters");
  /* a job without mesh, and one whose geometry is missing, draw nothing */
  jobs[1].mesh_op = -1;
  CHECK(wz3_draw_list_build(&list, &source, &frame, &nodes[0], &ids, NULL, NULL) == WZ3_OK && list.n_items == 0, "no mesh, no items");
  jobs[1].mesh_op = 5;
  source.geometry = NULL;
  CHECK(wz3_draw_list_build(&list, &source, &frame, &nodes[0], &ids, NULL, NULL) == WZ3_OK && list.n_items == 0, "no geometry, no items");
  /* with real light jobs the node's own lights are used and the default one is not */
  source.geometry = one_geometry;
  lights[1].amplify = 1;
  lights[1].directional = 1;
  lights[2] = lights[1];
  lights[0].amplify = 0; /* a light of an earlier node, never selected here */
  nodes[0].first_light = 1;
  nodes[0].n_light = 2;
  CHECK(wz3_draw_list_build(&list, &source, &frame, &nodes[0], &ids, &plan, NULL) == WZ3_OK, "build with lights");
  CHECK(plan.n_lights == 2 && plan.lights[0] == &lights[1] && plan.lights[1] == &lights[2] && list.n_items == 2, "two lights of the node, two items (%u)",
        list.n_items);
  /* effect jobs come first, last job first, and carry no light (renderUsageChanged compares it) */
  {
    static wz3_effect_job effect_jobs[3];
    memset(g_effects, 0, sizeof g_effects);
    memset(effect_jobs, 0, sizeof effect_jobs);
    g_effects[0].usage = WZ3_USAGE_LIGHT;
    g_effects[0].pass = 3;
    g_effects[1].usage = WZ3_USAGE_BASE;
    g_effects[1].pass = 250;
    effect_jobs[0].effect_op = 0;
    effect_jobs[1].effect_op = 1;
    effect_jobs[1].pass_adjust = 20;
    effect_jobs[2].effect_op = 0;
    frame.effect_jobs = effect_jobs;
    nodes[0].first_effect = 1;
    nodes[0].n_effect = 2;
    source.effect = pool_effect;
    source.user = &gi.g;
    CHECK(wz3_draw_list_build(&list, &source, &frame, &nodes[0], &ids, &plan, NULL) == WZ3_OK && list.n_items == 4, "build with effects (%u)", list.n_items);
    CHECK(list.items[0].kind == WZ3_DRAW_EFFECT && list.items[0].job == 1 && list.items[0].key.render_pass == 3 && list.items[0].key.usage == WZ3_USAGE_LIGHT &&
            list.items[0].key.is_effect && list.items[0].key.light_index == -1,
          "the last effect job comes first (job %u, pass %d)", list.items[0].job, list.items[0].key.render_pass);
    CHECK(list.items[1].kind == WZ3_DRAW_EFFECT && list.items[1].job == 0 && list.items[1].key.render_pass == 255 &&
            list.items[1].key.usage == WZ3_USAGE_BASE && list.items[1].key.light_index == -1,
          "then the first one, its pass clamped to 255");
    CHECK(list.items[2].kind == WZ3_DRAW_PASS, "mesh items follow");
  }
  wz3_draw_list_free(&list);
  wz3_pass_sort_ids_free(&ids);
}

int suite_wz3_draw_list(int argc, char **argv)
{
  if (argc < 2) {
    printf("usage: test_wz3_draw_list <fixtures dir> [--print]\n");
    return 2;
  }
  const int print = argc > 2 && !strcmp(argv[2], "--print");
  char *fixture = (char *)gate_read_all(argv[1], "draw_list.txt", NULL);
  wz3_pass_sort_ids_init(&g_ids);
  unsigned compared = 0, mismatches = 0;
  size_t line_number = 0;
  for (char *line = fixture; *line;) {
    char *nl = strchr(line, '\n');
    if (nl) *nl = 0;
    line_number++;
    char kind[32];
    if (sscanf(line, "%31s", kind) != 1) break;
    reader r;
    r.p = line + strlen(kind);
    const uint32_t got = run_case(kind, &r);
    const char *eq = strstr(line, " = ");
    if (eq) {
      const uint32_t expected = (uint32_t)strtoul(eq + 3, NULL, 16);
      compared++;
      if (print)
        printf("%s = %x\n", kind, got);
      else if (got != expected) {
        mismatches++;
        CHECK(0, "line %zu (%s) hash %x, JavaScript %x", line_number, kind, got, expected);
      }
    }
    if (!nl) break;
    line = nl + 1;
  }
  wz3_pass_sort_ids_free(&g_ids);
  free(fixture);
  CHECK(compared >= 1900, "%u lines", compared);
  CHECK(g_seen.shadow_items > 200 && g_seen.zfail_items > 20 && g_seen.culled > 500 && g_seen.unbounded_jobs > 100 && g_seen.default_group_jobs > 20 &&
          g_seen.sorted_actions > 100 && g_seen.light_items > 2000,
        "coverage: %u shadow items (%u z-fail), %u culled, %u unbounded jobs, %u default groups, %u actions, %u light items", g_seen.shadow_items,
        g_seen.zfail_items, g_seen.culled, g_seen.unbounded_jobs, g_seen.default_group_jobs, g_seen.sorted_actions, g_seen.light_items);
  test_oom();
  test_default_light();
  printf("draw list: %u lines compared, %u mismatches, %d checks, %d failures\n", compared, mismatches, g_checks, g_failures);
  return g_failures ? 1 : 0;
}
