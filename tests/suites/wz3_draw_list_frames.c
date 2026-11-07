/* Production draw-list check for wz3.h against src/renderer.js.
 *
 *   test_wz3_draw_list_frames <assets dir> <fixtures dir> [--print]
 *
 * Generates the geometry of the Debris party document (the meshes a scene draws, prepared by wz3_mesh_prepare /
 * wz3_minmesh_prepare), then replays tools/dump_draw_list_frames.mjs: for every viewport of every listed frame the draw
 * list of Renderer.drawViewportScene up to the insert tracker - light selection, culling, shadow volumes, effect items,
 * mesh items, sort - must hash exactly like the JavaScript port's.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define WZ3_IMPLEMENTATION
#include <debris/core/wz3.h>
#include "support/gate_common.h"

typedef struct store {
  uint8_t *wanted;
  wz3_geometry *geometry;
  uint8_t *have;
  uint32_t prepared;
} store;

static wz3_result take(store *s, uint32_t op, wz3_result r, wz3_geometry *g)
{
  if (r != WZ3_OK) return r;
  s->geometry[op] = *g;
  s->have[op] = 1;
  s->prepared++;
  return WZ3_OK;
}

static wz3_result sink_mesh(void *user, uint32_t op, const wz3_mesh *mesh)
{
  store *s = (store *)user;
  if (!mesh || !s->wanted[op]) return WZ3_OK;
  wz3_geometry g;
  const wz3_result r = wz3_mesh_prepare(mesh, &g);
  return take(s, op, r, &g);
}

static wz3_result sink_minmesh(void *user, uint32_t op, const wz3_minmesh *mesh)
{
  store *s = (store *)user;
  if (!mesh || !s->wanted[op]) return WZ3_OK;
  wz3_geometry g;
  const wz3_result r = wz3_minmesh_prepare(mesh, &g);
  return take(s, op, r, &g);
}

typedef struct context {
  const wz3_rt *rt;
  const store *store;
} context;

static const wz3_material *source_material(void *user, int32_t op)
{
  const context *c = (const context *)user;
  return op >= 0 ? wz3_rt_material(c->rt, (uint32_t)op) : NULL;
}

static const wz3_effect *source_effect(void *user, uint32_t op)
{
  const context *c = (const context *)user;
  return wz3_rt_effect(c->rt, op);
}

static const wz3_geometry *source_geometry(void *user, const wz3_mesh_job *job)
{
  const context *c = (const context *)user;
  return job->mesh_op >= 0 && c->store->have[job->mesh_op] ? &c->store->geometry[job->mesh_op] : NULL;
}

static void hash_item(hash *h, const wz3_draw_list *list, const wz3_draw_item *it)
{
  if (it->kind == WZ3_DRAW_EFFECT) {
    hu(h, 2);
    hi(h, (int32_t)it->job);
    hi(h, it->key.render_pass);
    hu(h, it->key.usage);
    hi(h, it->key.sort_material);
    return;
  }
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

/* what a scene geometry offers the draw list (the `B` lines) */
static uint32_t geometry_facts(const wz3_geometry *g)
{
  hash h;
  hash_init(&h);
  if (!g->n_groups) {
    /* an empty mesh draws one group over every index with slot 0 (normalizePreparedGeometry) */
    hu(&h, 1);
    hi(&h, 0);
    hi(&h, 0);
    hi(&h, -1);
    hu(&h, 1);
    hfs(&h, g->bounds_min, 3);
    hfs(&h, g->bounds_max, 3);
  } else {
    hu(&h, g->n_groups);
    for (uint32_t i = 0; i < g->n_groups; i++) {
      const wz3_geometry_group *group = &g->groups[i];
      hi(&h, group->slot);
      hi(&h, group->pass);
      hi(&h, group->material >= 0 ? group->material : -1);
      hu(&h, group->has_bounds);
      if (group->has_bounds) {
        hfs(&h, group->bounds_min, 3);
        hfs(&h, group->bounds_max, 3);
      }
    }
  }
  hfs(&h, g->bounds_min, 3);
  hfs(&h, g->bounds_max, 3);
  hu(&h, g->animated);
  return h.h;
}

static struct {
  unsigned viewports, items, shadow, effects, culled, animated_jobs, default_lights, light_items;
} g_seen;

typedef struct frame_state {
  const wz3_frame *frame;
  wz3_draw_source source;
  wz3_pass_sort_ids *ids;
  wz3_draw_list *list;
  hash *h;
  unsigned viewports, items;
  const store *geometry;
} frame_state;

static void visit(frame_state *fs, uint32_t node_index)
{
  const wz3_ipp_node *node = &fs->frame->nodes[node_index];
  if (node->type == WZ3_IPP_LAYER2D) {
    if (node->input >= 0) visit(fs, (uint32_t)node->input);
    return;
  }
  fs->viewports++;
  g_seen.viewports++;
  uint32_t indices[WZ3_MAX_LIGHTS];
  const uint32_t n = wz3_select_active_lights(fs->frame, node, WZ3_MAX_LIGHTS, indices);
  wz3_draw_stats stats;
  memset(&stats, 0, sizeof stats);
  wz3_viewport_plan plan;
  const wz3_result res = wz3_draw_list_build(fs->list, &fs->source, fs->frame, node, fs->ids, &plan, &stats);
  CHECK(res == WZ3_OK, "build: %s", wz3_result_string(res));
  hash *h = fs->h;
  hu(h, n);
  for (uint32_t i = 0; i < n; i++) {
    hi(h, indices[i] == (uint32_t)-1 ? -1 : (int32_t)indices[i]);
    if (indices[i] == (uint32_t)-1) g_seen.default_lights++;
  }
  hu(h, stats.candidate_view);
  hu(h, stats.culled_view);
  hu(h, stats.candidate_light);
  hu(h, stats.culled_light);
  hu(h, stats.candidate_shadow);
  hu(h, stats.culled_shadow_frustum);
  hu(h, stats.culled_shadow_sphere);
  g_seen.culled += stats.culled_view + stats.culled_light + stats.culled_shadow_frustum + stats.culled_shadow_sphere;
  hu(h, fs->list->n_items);
  for (uint32_t i = 0; i < fs->list->n_items; i++) {
    const wz3_draw_item *it = &fs->list->items[i];
    hash_item(h, fs->list, it);
    if (it->kind == WZ3_DRAW_SHADOW) g_seen.shadow++;
    if (it->kind == WZ3_DRAW_EFFECT) g_seen.effects++;
    if (it->kind == WZ3_DRAW_PASS && it->key.usage == WZ3_USAGE_LIGHT) g_seen.light_items++;
  }
  for (uint32_t j = 0; j < node->n_mesh; j++) {
    const wz3_mesh_job *job = &fs->frame->mesh_jobs[node->first_mesh + j];
    const wz3_geometry *g = source_geometry(fs->source.user, job);
    if (g && g->animated) g_seen.animated_jobs++;
  }
  wz3_draw_list_sort(fs->list);
  for (uint32_t i = 0; i < fs->list->n_items; i++) hash_item(h, fs->list, &fs->list->items[i]);
  wz3_insert_tracker tracker;
  wz3_insert_tracker_init(&tracker, fs->list->items, fs->list->n_items);
  for (uint32_t i = 0; i < fs->list->n_items; i++) hu(h, (uint32_t)wz3_insert_tracker_transition(&tracker, &fs->list->items[i]));
  hu(h, (uint32_t)wz3_insert_tracker_finish(&tracker));
  fs->items += fs->list->n_items;
  g_seen.items += fs->list->n_items;
}

int suite_wz3_draw_list_frames(int argc, char **argv)
{
  if (argc < 3) {
    printf("usage: test_wz3_draw_list_frames <assets dir> <fixtures dir> [--print]\n");
    return 2;
  }
  const int print = argc > 3 && !strcmp(argv[3], "--print");
  size_t kx_size;
  uint8_t *kx_bytes = gate_read_all(argv[1], "debris_party.kx", &kx_size);
  char *fixture = (char *)gate_read_all(argv[2], "draw_list_frames.txt", NULL);
  wz3_kx *kx = NULL;
  size_t at;
  CHECK(wz3_kx_parse(&kx, kx_bytes, kx_size, &at) == WZ3_OK, "parse");
  if (!kx) return 1;
  store s;
  memset(&s, 0, sizeof s);
  s.wanted = (uint8_t *)calloc(kx->n_ops, 1);
  s.have = (uint8_t *)calloc(kx->n_ops, 1);
  s.geometry = (wz3_geometry *)calloc(kx->n_ops, sizeof *s.geometry);
  CHECK(s.wanted && s.have && s.geometry, "allocation");
  wz3_kx_scene_geometry(kx, s.wanted);
  wz3_rt_options opt;
  memset(&opt, 0, sizeof opt);
  opt.generic_exec = 1;
  opt.release_consumed = 1;
  opt.frame_jobs = 1;
  opt.geometry = WZ3_GEOMETRY_GENERATE;
  opt.host.user = &s;
  opt.host.mesh = sink_mesh;
  opt.host.minmesh = sink_minmesh;
  wz3_result res;
  wz3_rt *rt = wz3_rt_create(kx, &opt, &res);
  CHECK(rt && res == WZ3_OK, "runtime create");
  if (!rt) return 1;
  wz3_rt_set_aspect(rt, 2.0);
  res = wz3_rt_precalc(rt);
  CHECK(res == WZ3_OK, "precalc: %s", wz3_result_string(res));
  printf("draw list frames: %u scene geometries prepared\n", s.prepared);

  context ctx = { rt, &s };
  wz3_pass_sort_ids ids;
  wz3_pass_sort_ids_init(&ids);
  wz3_draw_list list;
  wz3_draw_list_init(&list);
  unsigned compared = 0, mismatches = 0, geometry_lines = 0;
  size_t line_number = 0;
  for (char *line = fixture; *line && res == WZ3_OK;) {
    char *nl = strchr(line, '\n');
    if (nl) *nl = 0;
    line_number++;
    long sample;
    unsigned viewports, items, op;
    unsigned long expected;
    const char *eq = strstr(line, " = ");
    if (eq && sscanf(line, "B %u", &op) == 1) {
      expected = strtoul(eq + 3, NULL, 16);
      geometry_lines++;
      if (op >= kx->n_ops || !s.have[op])
        CHECK(0, "line %zu: operator %u has no prepared geometry", line_number, op);
      else if (geometry_facts(&s.geometry[op]) != (uint32_t)expected) {
        mismatches++;
        CHECK(0, "geometry of operator %u: facts %x, JavaScript %lx", op, geometry_facts(&s.geometry[op]), expected);
      }
      if (!nl) break;
      line = nl + 1;
      continue;
    }
    if (sscanf(line, "D %ld %u %u", &sample, &viewports, &items) != 3 || !eq) {
      CHECK(0, "line %zu: bad line", line_number);
      break;
    }
    expected = strtoul(eq + 3, NULL, 16);
    res = wz3_rt_frame_at_sample(rt, (int32_t)sample, 44100);
    CHECK(res == WZ3_OK, "frame %ld: %s", sample, wz3_result_string(res));
    if (res != WZ3_OK) break;
    frame_state fs;
    memset(&fs, 0, sizeof fs);
    hash h;
    hash_init(&h);
    fs.frame = wz3_rt_frame_jobs(rt);
    fs.source.user = &ctx;
    fs.source.material = source_material;
    fs.source.effect = source_effect;
    fs.source.geometry = source_geometry;
    fs.ids = &ids;
    fs.list = &list;
    fs.h = &h;
    for (uint32_t i = 0; i < fs.frame->n_outputs; i++) visit(&fs, fs.frame->outputs[i]);
    compared++;
    if (print)
      printf("D %ld %u %u = %x\n", sample, fs.viewports, fs.items, h.h);
    else {
      CHECK(fs.viewports == viewports && fs.items == items, "frame %ld: %u viewports %u items, JavaScript %u / %u", sample, fs.viewports, fs.items, viewports,
            items);
      if (h.h != (uint32_t)expected) {
        mismatches++;
        CHECK(0, "frame %ld hash %x, JavaScript %lx", sample, h.h, expected);
      }
    }
    if (!nl) break;
    line = nl + 1;
  }
  CHECK(compared >= 80 && geometry_lines >= 100, "%u frames and %u geometries compared", compared, geometry_lines);
  CHECK(g_seen.items > 50000 && g_seen.shadow > 1000 && g_seen.effects > 500 && g_seen.culled > 1000 && g_seen.animated_jobs > 100 &&
          g_seen.light_items > 10000,
        "coverage: %u items, %u shadow, %u effects, %u culled, %u animated jobs, %u light items", g_seen.items, g_seen.shadow, g_seen.effects, g_seen.culled,
        g_seen.animated_jobs, g_seen.light_items);
  wz3_draw_list_free(&list);
  wz3_pass_sort_ids_free(&ids);
  for (uint32_t op = 0; op < kx->n_ops; op++)
    if (s.have[op]) wz3_geometry_free(&s.geometry[op]);
  wz3_rt_destroy(rt);
  free(s.wanted);
  free(s.have);
  free(s.geometry);
  wz3_kx_destroy(kx);
  free(kx_bytes);
  free(fixture);
  printf("draw list frames: %u frames and %u geometries compared, %u mismatches, %u viewports, %u items, %u default lights, %d checks, %d failures\n", compared,
         geometry_lines, mismatches, g_seen.viewports, g_seen.items, g_seen.default_lights, g_checks, g_failures);
  return g_failures ? 1 : 0;
}
