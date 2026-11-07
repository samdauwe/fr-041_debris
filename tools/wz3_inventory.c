/* wz3_inventory: what the renderer has to deal with, measured on a KX document.
 *
 *   wz3_inventory [--texture-offset N] [--frame-stride N] [--keep-geometry] [--no-frames] [--no-textures] [--render-placeholder] DOCUMENT.kx
 *
 * 1. operators by class, and the cost of the runtime precalculation per class with every Mesh / MinMesh prepared for
 *    the renderer (wz3_mesh_prepare) through the host sink: time, vertices, indices, bytes, slowest operators and - with
 *    --keep-geometry - the peak memory when all of it stays resident;
 * 2. the frames of the whole song at the player's 30 fps cadence (--frame-stride N: every Nth): jobs per frame, instances,
 *    distinct meshes per frame and per 30 s window (the geometry working set), runtime time per frame, snapshot size;
 * 3. the textures the materials reference: how many, which bitmap operators they need, whether the graph can evaluate
 *    them (Bitmap_Render cannot), evaluation time, and the bytes the GPU would hold (wz3_texture_prepare, BC3).
 *
 * Times are CPU seconds of this process (clock()); memory is the peak resident set. The exit status is non-zero when a
 * consistency check fails (progress, sinks and operator counts disagree). */
#define _POSIX_C_SOURCE 200809L
#define WZ3_IMPLEMENTATION
#include <debris/core/wz3.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#if defined(__unix__) || defined(__APPLE__)
#include <sys/resource.h>
#define HAVE_RUSAGE 1
#endif

#define MAX_CLASS      512
#define SONG_RATE      44100
#define FRAME_SAMPLES  (SONG_RATE / 30)
#define WINDOW_SECONDS 30
#define SLOWEST        8

static double peak_rss_mib(void)
{
#ifdef HAVE_RUSAGE
  struct rusage u;
  if (getrusage(RUSAGE_SELF, &u)) return 0.0;
#if defined(__APPLE__)
  return (double)u.ru_maxrss / (1024.0 * 1024.0);
#else
  return (double)u.ru_maxrss / 1024.0;
#endif
#else
  return 0.0;
#endif
}

static double now_seconds(void) { return (double)clock() / (double)CLOCKS_PER_SEC; }
static double mib(uint64_t bytes) { return (double)bytes / 1048576.0; }

static const char *class_name(uint16_t id)
{
  const wz3_class_info *info = wz3_class_lookup(id);
  return info ? info->name : "?";
}

static int read_file(const char *path, unsigned char **data, size_t *size)
{
  FILE *f = fopen(path, "rb");
  if (!f) return 0;
  long n;
  if (fseek(f, 0, SEEK_END) || (n = ftell(f)) < 0 || fseek(f, 0, SEEK_SET)) {
    fclose(f);
    return 0;
  }
  unsigned char *d = (unsigned char *)malloc(n ? (size_t)n : 1);
  if (!d || (n && fread(d, 1, (size_t)n, f) != (size_t)n)) {
    free(d);
    fclose(f);
    return 0;
  }
  fclose(f);
  *data = d;
  *size = (size_t)n;
  return 1;
}

/* ---- 1. geometry precalculation ---- */

typedef struct inventory {
  const wz3_kx *kx;
  double last, prepare_since_last, prepare_total;
  double class_seconds[MAX_CLASS];
  uint32_t class_ops[MAX_CLASS];
  double slowest_seconds[SLOWEST];
  uint32_t slowest_op[SLOWEST];
  uint32_t progress_calls, progress_total;
  uint32_t meshes, minmeshes, null_minmeshes;
  uint64_t vertices, indices, groups, bytes, max_bytes;
  uint32_t *op_bytes;      /* prepared bytes per operator (0: no geometry) */
  uint8_t *scene_geometry; /* per operator: its mesh is consumed by an operator that is no mesh operator */
  wz3_geometry *kept;      /* --keep-geometry */
  int keep;
} inventory;

static wz3_result take_geometry(inventory *inv, uint32_t op, wz3_geometry *g)
{
  const uint64_t b = wz3_geometry_bytes(g);
  inv->vertices += g->n_vertices;
  inv->indices += g->n_indices;
  inv->groups += g->n_groups;
  inv->bytes += b;
  if (b > inv->max_bytes) inv->max_bytes = b;
  inv->op_bytes[op] = b > 0xffffffffu ? 0xffffffffu : (uint32_t)b;
  if (inv->keep)
    inv->kept[op] = *g;
  else
    wz3_geometry_free(g);
  return WZ3_OK;
}

static wz3_result on_mesh(void *user, uint32_t op, const wz3_mesh *mesh)
{
  inventory *inv = (inventory *)user;
  const double t = now_seconds();
  wz3_geometry g;
  const wz3_result r = wz3_mesh_prepare(mesh, &g);
  if (r != WZ3_OK) return r;
  inv->meshes++;
  const wz3_result taken = take_geometry(inv, op, &g);
  inv->prepare_since_last += now_seconds() - t;
  return taken;
}

static wz3_result on_minmesh(void *user, uint32_t op, const wz3_minmesh *mesh)
{
  inventory *inv = (inventory *)user;
  if (!mesh) {
    inv->null_minmeshes++;
    return WZ3_OK;
  }
  const double t = now_seconds();
  wz3_geometry g;
  const wz3_result r = wz3_minmesh_prepare(mesh, &g);
  if (r != WZ3_OK) return r;
  inv->minmeshes++;
  const wz3_result taken = take_geometry(inv, op, &g);
  inv->prepare_since_last += now_seconds() - t;
  return taken;
}

static void on_progress(void *user, uint32_t op, uint32_t done, uint32_t total)
{
  inventory *inv = (inventory *)user;
  const double t = now_seconds();
  const double own = t - inv->last - inv->prepare_since_last; /* the operator itself, without its geometry preparation */
  inv->prepare_total += inv->prepare_since_last;
  inv->prepare_since_last = 0.0;
  inv->last = t;
  const uint16_t id = inv->kx->ops[op].class_id;
  if (id < MAX_CLASS) {
    inv->class_seconds[id] += own;
    inv->class_ops[id]++;
  }
  for (int i = 0; i < SLOWEST; i++) {
    if (own <= inv->slowest_seconds[i]) continue;
    memmove(&inv->slowest_seconds[i + 1], &inv->slowest_seconds[i], (size_t)(SLOWEST - 1 - i) * sizeof inv->slowest_seconds[0]);
    memmove(&inv->slowest_op[i + 1], &inv->slowest_op[i], (size_t)(SLOWEST - 1 - i) * sizeof inv->slowest_op[0]);
    inv->slowest_seconds[i] = own;
    inv->slowest_op[i] = op;
    break;
  }
  inv->progress_calls++;
  inv->progress_total = total;
  (void)done;
}

static int report_classes(const wz3_kx *kx)
{
  uint32_t per_output[16] = { 0 };
  uint32_t renders = 0, forward_inputs = 0;
  for (uint32_t i = 0; i < kx->n_ops; i++) {
    const wz3_kx_op *op = &kx->ops[i];
    for (uint32_t k = 0; k < op->n_inputs; k++)
      if (op->inputs[k] >= i) forward_inputs++;
    const uint16_t id = op->class_id;
    const wz3_class_info *info = wz3_class_lookup(id);
    if (info && info->output_class < 16) per_output[info->output_class]++;
    if (info && !strcmp(info->name, "Render") && info->output_class == WZ3_KC_BITMAP) renders++;
  }
  printf("document: %u operators, %u classes, %u events, %u splines, song %.1f s\n", kx->n_ops, kx->n_classes, kx->n_events, kx->n_splines,
         kx->song_bpm_fixed ? (double)kx->song_length * 60.0 / (double)kx->song_bpm_fixed : 0.0);
  printf("operators by output: bitmap %u, mesh %u, minmesh %u, scene %u, material %u, ipp %u, effect %u, spline %u, demo %u\n", per_output[WZ3_KC_BITMAP],
         per_output[WZ3_KC_MESH], per_output[WZ3_KC_MINMESH], per_output[WZ3_KC_SCENE], per_output[WZ3_KC_MATERIAL], per_output[WZ3_KC_IPP],
         per_output[WZ3_KC_EFFECT], per_output[WZ3_KC_SPLINE], per_output[WZ3_KC_DEMO]);
  printf("Bitmap_Render (class Render, 0x3f) operators: %u\n", renders);
  if (forward_inputs) {
    fprintf(stderr, "check failed: %u inputs refer to a later operator (the texture closure assumes document order)\n", forward_inputs);
    return 1;
  }
  return 0;
}

static int run_precalc(wz3_rt *rt, inventory *inv, int keep)
{
  const double t0 = now_seconds();
  inv->last = t0;
  const wz3_result r = wz3_rt_precalc(rt);
  const double total = now_seconds() - t0;
  if (r != WZ3_OK) {
    fprintf(stderr, "precalc: %s\n", wz3_result_string(r));
    return 1;
  }
  printf("\nprecalc: %.2f s for %u of %u operators (%.2f s of it preparing geometry), peak %.0f MiB%s\n", total, inv->progress_calls, inv->progress_total,
         inv->prepare_total, peak_rss_mib(), keep ? " with all geometry resident" : "");
  printf("geometry: %u meshes, %u minmeshes (%u without geometry): %llu vertices, %llu indices, %llu groups, %.1f MiB prepared (largest %.2f MiB)\n",
         inv->meshes, inv->minmeshes, inv->null_minmeshes, (unsigned long long)inv->vertices, (unsigned long long)inv->indices, (unsigned long long)inv->groups,
         mib(inv->bytes), mib(inv->max_bytes));
  uint32_t scene_count = 0;
  uint64_t scene_bytes = 0;
  for (uint32_t i = 0; i < inv->kx->n_ops; i++)
    if (inv->scene_geometry[i]) {
      scene_count++;
      scene_bytes += inv->op_bytes[i];
    }
  printf("scene geometry: %u of %u are consumed by a non-geometry operator (%.1f MiB prepared); the rest are generator intermediates\n", scene_count,
         inv->meshes + inv->minmeshes, mib(scene_bytes));
  printf("by class (operator time without geometry preparation):\n");
  for (int pass = 0; pass < 8; pass++) {
    int best = -1;
    for (int c = 0; c < MAX_CLASS; c++)
      if (inv->class_ops[c] && (best < 0 || inv->class_seconds[c] > inv->class_seconds[best])) best = c;
    if (best < 0 || inv->class_seconds[best] <= 0.0) break;
    printf("  0x%03x %-24s %6u ops %7.3f s\n", best, class_name((uint16_t)best), inv->class_ops[best], inv->class_seconds[best]);
    inv->class_seconds[best] = -inv->class_seconds[best]; /* printed */
  }
  printf("slowest operators (cancel latency is the cost of one):\n");
  for (int i = 0; i < SLOWEST && inv->slowest_seconds[i] > 0.0; i++)
    printf("  op %-6u %-24s %.1f ms\n", inv->slowest_op[i], class_name(inv->kx->ops[inv->slowest_op[i]].class_id), inv->slowest_seconds[i] * 1000.0);
  if (inv->progress_calls != inv->progress_total) {
    fprintf(stderr, "check failed: %u progress calls, %u operators reachable\n", inv->progress_calls, inv->progress_total);
    return 1;
  }
  return 0;
}

/* ---- 2. frames ---- */

static int run_frames(wz3_rt *rt, const wz3_kx *kx, const inventory *inv, int stride)
{
  const double seconds = kx->song_bpm_fixed ? (double)kx->song_length * 60.0 / (double)kx->song_bpm_fixed : 0.0;
  const int64_t duration = (int64_t)(seconds * SONG_RATE);
  uint32_t *drawn_frame = (uint32_t *)calloc(kx->n_ops, sizeof(uint32_t));
  uint32_t *drawn_window = (uint32_t *)calloc(kx->n_ops, sizeof(uint32_t));
  uint8_t *ever = (uint8_t *)calloc(kx->n_ops, 1);
  if (!drawn_frame || !drawn_window || !ever) {
    fprintf(stderr, "out of memory\n");
    free(drawn_frame);
    free(drawn_window);
    free(ever);
    return 1;
  }
  uint64_t frames = 0, viewports = 0, mesh_jobs = 0, effect_jobs = 0, light_jobs = 0, instances = 0, distinct_sum = 0;
  uint32_t max_viewports = 0, max_mesh_jobs = 0, max_effect_jobs = 0, max_lights = 0, max_instances = 0, max_distinct = 0;
  uint32_t window = 0, window_count = 0, max_window = 0, ever_count = 0, outside_scene = 0;
  uint64_t window_bytes = 0, max_window_bytes = 0, ever_bytes = 0;
  double max_ms = 0.0, run_seconds = 0.0;
  size_t max_snapshot = 0;
  const double wall0 = now_seconds();
  for (int64_t sample = 0; sample <= duration; sample += (int64_t)FRAME_SAMPLES * stride) {
    const double t = now_seconds();
    uint32_t advanced;
    wz3_result r = wz3_rt_frame_at_sample(rt, (int32_t)sample, SONG_RATE);
    if (r == WZ3_OK) r = wz3_rt_effects_advance(rt, &advanced);
    if (r != WZ3_OK) {
      fprintf(stderr, "frame at %.2f s: %s\n", (double)sample / SONG_RATE, wz3_result_string(r));
      free(drawn_frame);
      free(drawn_window);
      free(ever);
      return 1;
    }
    const double ms = (now_seconds() - t) * 1000.0;
    run_seconds += ms / 1000.0;
    if (ms > max_ms) max_ms = ms;
    frames++;
    const uint32_t frame_stamp = (uint32_t)frames, win = (uint32_t)(sample / ((int64_t)SONG_RATE * WINDOW_SECONDS)) + 1;
    if (win != window) {
      window = win;
      window_count = 0;
      window_bytes = 0;
    }
    const wz3_frame *f = wz3_rt_frame_jobs(rt);
    uint32_t vp = 0, mj = 0, ej = 0, lj = 0, inst = 0, distinct = 0;
    for (uint32_t n = 0; n < f->n_nodes; n++) {
      const wz3_ipp_node *node = &f->nodes[n];
      if (node->type != WZ3_IPP_VIEWPORT) continue;
      vp++;
      mj += node->n_mesh;
      ej += node->n_effect;
      lj += node->n_light;
      for (uint32_t j = 0; j < node->n_mesh; j++) {
        const wz3_mesh_job *job = &f->mesh_jobs[node->first_mesh + j];
        inst += job->has_instances ? job->n_instances : 1;
        if (job->mesh_op < 0 || (uint32_t)job->mesh_op >= kx->n_ops) continue;
        const uint32_t op = (uint32_t)job->mesh_op;
        if (drawn_frame[op] != frame_stamp) {
          drawn_frame[op] = frame_stamp;
          distinct++;
        }
        if (drawn_window[op] != window) {
          drawn_window[op] = window;
          window_count++;
          window_bytes += inv->op_bytes[op];
        }
        if (!ever[op]) {
          ever[op] = 1;
          ever_count++;
          ever_bytes += inv->op_bytes[op];
          if (!inv->scene_geometry[op]) outside_scene++;
        }
      }
    }
    viewports += vp;
    mesh_jobs += mj;
    effect_jobs += ej;
    light_jobs += lj;
    instances += inst;
    distinct_sum += distinct;
    if (vp > max_viewports) max_viewports = vp;
    if (mj > max_mesh_jobs) max_mesh_jobs = mj;
    if (ej > max_effect_jobs) max_effect_jobs = ej;
    if (lj > max_lights) max_lights = lj;
    if (inst > max_instances) max_instances = inst;
    if (distinct > max_distinct) max_distinct = distinct;
    if (window_count > max_window) max_window = window_count;
    if (window_bytes > max_window_bytes) max_window_bytes = window_bytes;
    if (frames % 64 == 1) {
      const size_t s = wz3_rt_snapshot_size(rt);
      if (s > max_snapshot) max_snapshot = s;
    }
  }
  const double n = frames ? (double)frames : 1.0;
  printf("\nframes: %llu (every %d of the 30 fps cadence over %.1f s), %.2f s wall, runtime %.3f ms average / %.2f ms worst per frame\n",
         (unsigned long long)frames, stride, (double)duration / SONG_RATE, now_seconds() - wall0, run_seconds * 1000.0 / n, max_ms);
  printf("per frame (average / max): viewports %.1f / %u, mesh jobs %.1f / %u, instances %.1f / %u, effect jobs %.1f / %u, lights %.1f / %u\n",
         (double)viewports / n, max_viewports, (double)mesh_jobs / n, max_mesh_jobs, (double)instances / n, max_instances, (double)effect_jobs / n,
         max_effect_jobs, (double)light_jobs / n, max_lights);
  printf("distinct meshes per frame %.1f average / %u max; per %d s window up to %u (%.1f MiB prepared); over the song %u (%.1f MiB)\n",
         (double)distinct_sum / n, max_distinct, WINDOW_SECONDS, max_window, mib(max_window_bytes), ever_count, mib(ever_bytes));
  printf("snapshot: up to %.2f MiB\n", (double)max_snapshot / 1048576.0);
  free(drawn_frame);
  free(drawn_window);
  free(ever);
  if (outside_scene) {
    fprintf(stderr, "check failed: %u drawn meshes are not consumed by a non-geometry operator\n", outside_scene);
    return 1;
  }
  return 0;
}

/* ---- 3. textures ---- */

static int g_render_placeholder; /* --render-placeholder: Bitmap_Render evaluates to the blank bitmap of the JavaScript port without a renderer */

static int run_textures(wz3_rt *rt, const wz3_kx *kx, int texture_offset)
{
  uint8_t *needed = (uint8_t *)calloc((size_t)kx->n_ops * 2, 1), *is_texture = (uint8_t *)calloc(kx->n_ops, 1);
  uint8_t *failed_flag = needed ? needed + kx->n_ops : NULL;
  uint32_t *consumers = (uint32_t *)calloc(kx->n_ops, sizeof(uint32_t));
  if (!needed || !is_texture || !consumers) {
    fprintf(stderr, "out of memory\n");
    free(needed);
    free(is_texture);
    free(consumers);
    return 1;
  }
  uint32_t materials = 0, references = 0, textures = 0;
  for (uint32_t i = 0; i < kx->n_ops; i++) {
    const wz3_material *m = wz3_rt_material(rt, i);
    if (!m) continue;
    materials++;
    for (int s = 0; s < 7; s++) {
      if (m->textures[s] < 0 || (uint32_t)m->textures[s] >= kx->n_ops) continue;
      references++;
      if (!is_texture[m->textures[s]]) {
        is_texture[m->textures[s]] = 1;
        textures++;
      }
    }
  }
  /* the bitmap operators the textures need: ancestors through bitmap-class inputs */
  uint32_t n_needed = 0, renders_needed = 0;
  for (uint32_t i = kx->n_ops; i-- > 0;) { /* inputs have lower indices than their consumers */
    if (is_texture[i]) needed[i] = 1;
    if (!needed[i]) continue;
    n_needed++;
    const wz3_class_info *info = wz3_class_lookup(kx->ops[i].class_id);
    if (info && !strcmp(info->name, "Render") && info->output_class == WZ3_KC_BITMAP) renders_needed++;
    for (uint32_t k = 0; k < kx->ops[i].n_inputs; k++) {
      const uint32_t in = kx->ops[i].inputs[k];
      const wz3_class_info *ii = in < kx->n_ops ? wz3_class_lookup(kx->ops[in].class_id) : NULL;
      if (ii && ii->output_class == WZ3_KC_BITMAP) {
        needed[in] = 1;
        consumers[in]++;
      }
    }
  }
  printf("\ntextures: %u materials reference %u bitmap operators (%u references); they need %u bitmap operators, %u of them Bitmap_Render\n", materials,
         textures, references, n_needed, renders_needed);

  wz3_graph_options go;
  memset(&go, 0, sizeof go);
  wz3_bitmap_ctx_init(&go.ctx);
  go.ctx.texture_offset = texture_offset;
  go.render_placeholder = g_render_placeholder;
  wz3_result res;
  wz3_graph *g = wz3_graph_create(kx, &go, &res);
  if (!g) {
    fprintf(stderr, "graph: %s\n", wz3_result_string(res));
    free(needed);
    free(is_texture);
    free(consumers);
    return 1;
  }
  const double t0 = now_seconds();
  uint32_t evaluated = 0, failed = 0, failed_textures = 0, uploads = 0, unexpected = 0, failed_list[16], plan_bad = 0, plan_commands = 0;
  wz3_texture_policy policy;
  wz3_texture_policy_make(&policy, WZ3_DXT5_AUTO, 1);
  uint64_t gpu_bytes = 0, levels = 0, max_bytes = 0, bc3 = 0, rgba = 0, snorm = 0;
  double prepare_seconds = 0.0;
  int32_t max_w = 0, max_h = 0;
  for (uint32_t i = 0; i < kx->n_ops; i++) {
    if (!needed[i]) continue;
    const wz3_bitmap *bm = NULL;
    const wz3_result er = wz3_graph_eval_bitmap(g, i, &bm);
    if (er != WZ3_OK || !bm) {
      failed++;
      failed_flag[i] = 1;
      { /* expected: Bitmap_Render (it needs the renderer) and whatever consumes its output */
        const wz3_class_info *info = wz3_class_lookup(kx->ops[i].class_id);
        int legitimate = info && !strcmp(info->name, "Render");
        for (uint32_t k = 0; k < kx->ops[i].n_inputs; k++)
          if (kx->ops[i].inputs[k] < kx->n_ops && failed_flag[kx->ops[i].inputs[k]]) legitimate = 1;
        if (!legitimate) unexpected++;
      }
      if (is_texture[i]) {
        if (failed_textures < 16) failed_list[failed_textures] = i;
        failed_textures++;
      }
      if (failed <= 6) printf("  op %u (%s) cannot be evaluated: %s\n", i, class_name(kx->ops[i].class_id), wz3_result_string(er));
    } else {
      evaluated++;
      if (is_texture[i]) {
        const double tp = now_seconds();
        wz3_texture_image img;
        if (wz3_texture_prepare(&img, bm, 1) == WZ3_OK) {
          /* the upload plan must describe exactly the levels that were prepared */
          wz3_texture_desc desc;
          wz3_texture_plan plan;
          wz3_texture_desc_from_bitmap(&desc, bm);
          int consistent = wz3_texture_plan_make(&plan, &policy, &desc) == WZ3_OK && plan.allow_bc3 == (img.kind == WZ3_TEXTURE_BC3);
          uint32_t uploaded = 0, generate = 0;
          for (uint32_t c = 0; consistent && c < plan.n_commands; c++) {
            const wz3_texture_command *cmd = &plan.commands[c];
            if (cmd->kind == WZ3_TEXCMD_GENERATE_MIPMAP) generate = 1;
            if (cmd->kind != WZ3_TEXCMD_IMAGE && cmd->kind != WZ3_TEXCMD_COMPRESSED) continue;
            uploaded++;
            consistent = cmd->level >= 0 && (uint32_t)cmd->level < img.n_levels && cmd->size == img.levels[cmd->level].size &&
                         cmd->width == img.levels[cmd->level].width && cmd->height == img.levels[cmd->level].height;
          }
          if (!consistent || uploaded != (generate ? 1u : img.n_levels)) plan_bad++;
          plan_commands += plan.n_commands;
          uploads++;
          gpu_bytes += img.size;
          levels += img.n_levels;
          if (img.size > max_bytes) max_bytes = img.size;
          if (img.width > max_w) max_w = img.width;
          if (img.height > max_h) max_h = img.height;
          if (img.kind == WZ3_TEXTURE_BC3)
            bc3++;
          else if (img.kind == WZ3_TEXTURE_SNORM8)
            snorm++;
          else
            rgba++;
          wz3_texture_release(&img);
        }
        prepare_seconds += now_seconds() - tp;
      }
    }
    for (uint32_t k = 0; k < kx->ops[i].n_inputs; k++) {
      const uint32_t in = kx->ops[i].inputs[k];
      if (in < kx->n_ops && needed[in] && consumers[in] && --consumers[in] == 0) wz3_graph_release(g, in);
    }
    if (consumers[i] == 0) wz3_graph_release(g, i);
  }
  printf(
    "textures at texture offset %d: %u operators evaluated, %u failed (%u of them textures) in %.2f s, plus %.2f s preparing the GPU data, peak %.0f MiB\n",
    texture_offset, evaluated, failed, failed_textures, now_seconds() - t0 - prepare_seconds, prepare_seconds, peak_rss_mib());
  printf("GPU data: %u textures (%llu BC3, %llu RGBA8, %llu signed), %llu mip levels, %.1f MiB (largest %.2f MiB, up to %dx%d)\n", uploads,
         (unsigned long long)bc3, (unsigned long long)rgba, (unsigned long long)snorm, (unsigned long long)levels, mib(gpu_bytes), mib(max_bytes), max_w,
         max_h);
  printf("upload plan: %u textures planned with %u GL calls, %u inconsistent with the prepared levels\n", uploads, plan_commands, plan_bad);
  for (uint32_t f = 0; f < failed_textures && f < 16; f++) {
    printf("  unevaluable texture op %u (%s) is used by materials:", failed_list[f], class_name(kx->ops[failed_list[f]].class_id));
    for (uint32_t i = 0; i < kx->n_ops; i++) {
      const wz3_material *m = wz3_rt_material(rt, i);
      for (int s = 0; m && s < 7; s++)
        if (m->textures[s] == (int32_t)failed_list[f]) {
          printf(" %u(slot %d)", i, s);
          break;
        }
    }
    printf("\n");
  }
  wz3_graph_destroy(g);
  free(needed);
  free(is_texture);
  free(consumers);
  if (unexpected) {
    fprintf(stderr, "check failed: %u bitmap operators failed that neither are Bitmap_Render nor depend on it\n", unexpected);
    return 1;
  }
  if (plan_bad) {
    fprintf(stderr, "check failed: %u upload plans do not match the prepared texture levels\n", plan_bad);
    return 1;
  }
  return 0;
}

int main(int argc, char **argv)
{
  int texture_offset = 0, stride = 1, keep = 0, frames = 1, textures = 1;
  const char *path = NULL;
  for (int i = 1; i < argc; i++) {
    if (!strcmp(argv[i], "--texture-offset") && i + 1 < argc)
      texture_offset = atoi(argv[++i]);
    else if (!strcmp(argv[i], "--render-placeholder"))
      g_render_placeholder = 1;
    else if (!strcmp(argv[i], "--frame-stride") && i + 1 < argc) {
      stride = atoi(argv[++i]);
      if (stride < 1) stride = 1;
    } else if (!strcmp(argv[i], "--keep-geometry"))
      keep = 1;
    else if (!strcmp(argv[i], "--no-frames"))
      frames = 0;
    else if (!strcmp(argv[i], "--no-textures"))
      textures = 0;
    else if (argv[i][0] != '-' && !path)
      path = argv[i];
    else {
      path = NULL;
      break;
    }
  }
  if (!path) {
    fprintf(stderr, "usage: %s [--texture-offset N] [--frame-stride N] [--keep-geometry] [--no-frames] [--no-textures] [--render-placeholder] DOCUMENT.kx\n",
            argv[0]);
    return 2;
  }
  unsigned char *bytes;
  size_t size;
  if (!read_file(path, &bytes, &size)) {
    fprintf(stderr, "cannot read %s\n", path);
    return 1;
  }
  wz3_kx *kx;
  wz3_result res = wz3_kx_parse(&kx, bytes, size, NULL);
  if (res != WZ3_OK) {
    fprintf(stderr, "%s: %s\n", path, wz3_result_string(res));
    free(bytes);
    return 1;
  }

  int rc = report_classes(kx);
  inventory inv;
  memset(&inv, 0, sizeof inv);
  inv.kx = kx;
  inv.keep = keep;
  inv.op_bytes = (uint32_t *)calloc(kx->n_ops, sizeof(uint32_t));
  inv.scene_geometry = (uint8_t *)calloc(kx->n_ops, 1);
  if (inv.scene_geometry) wz3_kx_scene_geometry(kx, inv.scene_geometry);
  inv.kept = keep ? (wz3_geometry *)calloc(kx->n_ops, sizeof(wz3_geometry)) : NULL;
  wz3_rt_options o;
  memset(&o, 0, sizeof o);
  o.release_consumed = 1;
  o.frame_jobs = 1;
  o.host.user = &inv;
  o.host.mesh = on_mesh;
  o.host.minmesh = on_minmesh;
  o.host.progress = on_progress;
  wz3_rt *rt = inv.op_bytes && inv.scene_geometry && (!keep || inv.kept) ? wz3_rt_create(kx, &o, &res) : NULL;
  if (!rt) {
    fprintf(stderr, "runtime: %s\n", wz3_result_string(inv.op_bytes ? res : WZ3_ERR_OUT_OF_MEMORY));
    rc = 1;
  } else {
    wz3_rt_set_aspect(rt, 2.0);
    rc |= run_precalc(rt, &inv, keep);
    if (!rc && frames) rc |= run_frames(rt, kx, &inv, stride);
    if (!rc && textures) rc |= run_textures(rt, kx, texture_offset);
    printf("\npeak resident set: %.0f MiB\n", peak_rss_mib());
  }
  if (inv.kept)
    for (uint32_t i = 0; i < kx->n_ops; i++)
      if (inv.kept[i].block_) wz3_geometry_free(&inv.kept[i]);
  free(inv.kept);
  free(inv.op_bytes);
  free(inv.scene_geometry);
  wz3_rt_destroy(rt);
  wz3_kx_destroy(kx);
  free(bytes);
  return rc;
}
