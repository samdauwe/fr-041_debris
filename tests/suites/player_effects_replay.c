/* Checks for the stateful effects across a seek: restore + replay must give the effects the history linear playback gave them.
 *
 *   test_player_effects_replay <party.kx>
 *
 * The chain lines and the water simulation keep state from slice to slice. The player draws a slice once (the renderer runs every effect of
 * the frame) and replays a seek at the 30 fps cadence through the same runtime, so a backward seek restores the nearest snapshot and
 * replays up to the target. Linear playback runs once through the demo at that cadence, hashing what every effect produces; then, for
 * targets spread over the demo where effects exist, the runtime (still in its state from the end of the demo) restores a snapshot,
 * replays to the target and must produce the same effect output for the slice after it as the linear run did. Both the player's path
 * (frame + effect run at draw time) and a seek's replay (frame + effects advance) are exercised. Finally a second effect run in the same slice is
 * measured: it does change the output (the chain lines and the water keep per-run state), which is why the live loop of the player takes a frame
 * without advancing the effects and lets the renderer run them, once, like the browser does (the slices of a seek's replay are not drawn, so
 * they advance).
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define V2_IMPLEMENTATION
#include <debris/core/v2.h>
#define WZ3_IMPLEMENTATION
#include <debris/core/wz3.h>
#define PLAYER_CORE_IMPLEMENTATION
#include <debris/player/player_core.h>
#define PLAYER_REPLAY_IMPLEMENTATION
#include <debris/player/player_replay.h>

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

#define RATE 44100 /* not an enum: it is divided into doubles */
enum { FPS = 30, MAX_TARGETS = 16, MAX_LONG_TARGETS = 8, MAX_SHORT_TARGETS = 6 };

typedef struct fnv {
  uint32_t h;
} fnv;
static void hb(fnv *f, const void *p, size_t n)
{
  const uint8_t *b = (const uint8_t *)p;
  for (size_t i = 0; i < n; i++) f->h = (f->h ^ b[i]) * 0x01000193u;
}
static void hu(fnv *f, uint32_t v) { hb(f, &v, 4); }

static void hash_output(fnv *f, const wz3_effect_output *o)
{
  hu(f, o->kind);
  hu(f, o->op);
  hb(f, o->matrix, sizeof o->matrix);
  hb(f, &o->time, sizeof o->time);
  if (o->kind == WZ3_EFFECT_OUT_CHAIN_LINE && o->points) {
    hb(f, o->points, 32 * 3 * sizeof(float));
    hb(f, &o->thickness, sizeof o->thickness);
    hu(f, (uint32_t)o->ripped);
    hu(f, o->color);
  } else if (o->kind == WZ3_EFFECT_OUT_WATER && o->water) {
    const wz3_water_geometry *w = o->water;
    hu(f, w->version);
    hu(f, w->wave_count);
    hb(f, w->positions, (size_t)w->n_vertices * 3 * sizeof(float));
    hb(f, w->normals, (size_t)w->n_vertices * 3 * sizeof(float));
    hb(f, w->uvs, (size_t)w->n_vertices * 2 * sizeof(float));
  }
}

/* runs the deferred effects of the frame like the renderer does (IPP roots in order, Layer2D inputs followed, effects in draw order);
   hashes what they produce */
static wz3_result run_node(wz3_rt *rt, uint32_t node_index, fnv *f, uint32_t *count)
{
  const wz3_frame *frame = wz3_rt_frame_jobs(rt);
  const wz3_ipp_node node = frame->nodes[node_index];
  if (node.type == WZ3_IPP_LAYER2D) return node.input >= 0 ? run_node(rt, (uint32_t)node.input, f, count) : WZ3_OK;
  if (node.type != WZ3_IPP_VIEWPORT || !node.n_effect) return WZ3_OK;
  uint32_t *order = (uint32_t *)malloc(sizeof *order * node.n_effect);
  if (!order) return WZ3_ERR_OUT_OF_MEMORY;
  wz3_frame_effect_order(rt, &node, order);
  wz3_result r = WZ3_OK;
  for (uint32_t i = 0; i < node.n_effect && r == WZ3_OK; i++) {
    const wz3_effect_output *out = NULL;
    r = wz3_rt_effect_run(rt, &frame->effect_jobs[node.first_effect + order[i]], &out);
    if (r == WZ3_OK && out) hash_output(f, out);
    (*count)++;
  }
  free(order);
  return r;
}

/* one slice as the player draws it: the runtime frame, then every effect once */
static wz3_result live_slice(wz3_rt *rt, int64_t sample, uint32_t *hash, uint32_t *count)
{
  wz3_result r = wz3_rt_frame_at_sample(rt, (int32_t)sample, RATE);
  fnv f = { 0x811c9dc5u };
  uint32_t n = 0;
  const wz3_frame *frame = r == WZ3_OK ? wz3_rt_frame_jobs(rt) : NULL;
  for (uint32_t i = 0; frame && i < frame->n_outputs && r == WZ3_OK; i++) r = run_node(rt, frame->outputs[i], &f, &n);
  if (hash) *hash = f.h;
  if (count) *count = n;
  return r;
}

/* the effects of the frame already built, run once more (what a renderer drawing the same slice twice, or a replay slice followed by a draw, does) */
static wz3_result run_again(wz3_rt *rt, uint32_t *hash)
{
  fnv f = { 0x811c9dc5u };
  uint32_t n = 0;
  const wz3_frame *frame = wz3_rt_frame_jobs(rt);
  wz3_result r = WZ3_OK;
  for (uint32_t i = 0; i < frame->n_outputs && r == WZ3_OK; i++) r = run_node(rt, frame->outputs[i], &f, &n);
  *hash = f.h;
  return r;
}

typedef struct target {
  int64_t sample, next;
  uint32_t hash, effects;
} target;

int suite_player_effects_replay(int argc, char **argv)
{
  if (argc < 2) {
    printf("usage: test_player_effects_replay <party.kx>\n");
    return 2;
  }
  FILE *file = fopen(argv[1], "rb");
  if (!file) {
    printf("cannot read %s\n", argv[1]);
    return 2;
  }
  fseek(file, 0, SEEK_END);
  const long size = ftell(file);
  fseek(file, 0, SEEK_SET);
  unsigned char *bytes = (unsigned char *)malloc((size_t)size);
  if (!bytes || fread(bytes, 1, (size_t)size, file) != (size_t)size) return 2;
  fclose(file);
  wz3_kx *kx = NULL;
  CHECK(wz3_kx_parse(&kx, bytes, (size_t)size, NULL) == WZ3_OK, "parse");
  if (!kx) return 1;
  wz3_rt_options o;
  memset(&o, 0, sizeof o);
  o.generic_exec = 1;
  o.release_consumed = 1;
  o.frame_jobs = 1;
  o.geometry = WZ3_GEOMETRY_PLACEHOLDER; /* the effects do not look at the meshes */
  wz3_result res;
  wz3_rt *rt = wz3_rt_create(kx, &o, &res);
  CHECK(rt != NULL, "runtime");
  if (!rt) return 1;
  wz3_rt_set_aspect(rt, 2.0);
  CHECK(wz3_rt_precalc(rt) == WZ3_OK, "precalc");
  const int64_t end = (int64_t)((double)kx->song_length * 60.0 / (double)kx->song_bpm_fixed * RATE);

  static psnapshots snaps;
  CHECK(psnapshots_take(&snaps, rt, 0) == WZ3_OK, "snapshot 0");
  /* a seek's replay of the whole demo on a fresh runtime, every slice advancing the effects (a sanitizer sees any memory error of the effect
     order buffer here; the double free the soak found in it needs the player's real geometry and is gated by the soak itself) */
  {
    int64_t at = 0;
    CHECK(preplay_advance(rt, &at, end, RATE, FPS, NULL) == WZ3_OK && at == end, "replay of the whole demo");
    const psnapshot *start = psnapshots_before(&snaps, 0);
    CHECK(start && wz3_rt_restore(rt, start->data, start->size) == WZ3_OK, "back to the first snapshot");
  }
  target targets[MAX_TARGETS];
  int n_targets = 0, pending = -1, n_short = 0, short_for_snapshots = -1;
  int64_t sample = 0, last_target = -100 * (int64_t)RATE;
  uint64_t slices = 0, slices_with_effects = 0, water_slices = 0, repeated = 0, repeated_differs = 0;
  while (sample < end) {
    sample = pseek_next_sample(sample, end, RATE, FPS);
    uint32_t hash, count;
    CHECK(live_slice(rt, sample, &hash, &count) == WZ3_OK, "slice at %lld", (long long)sample);
    slices++;
    slices_with_effects += count > 0;
    if (count > 0 && slices % 97 == 0) { /* a second run in the same slice changes nothing the picture shows */
      uint32_t again;
      CHECK(run_again(rt, &again) == WZ3_OK, "second run at %lld", (long long)sample);
      repeated++;
      repeated_differs += again != hash;
    }
    if (pending >= 0) { /* the slice after a target: what the replay must reproduce */
      targets[pending].next = sample;
      targets[pending].hash = hash;
      targets[pending].effects = count;
      pending = -1;
    }
    if (sample - snaps.last_sample >= 30 * (int64_t)RATE) CHECK(psnapshots_take(&snaps, rt, sample) == WZ3_OK, "snapshot at %lld", (long long)sample);
    if (n_targets < MAX_LONG_TARGETS + MAX_SHORT_TARGETS && pending < 0 && count > 0 && snaps.last_sample >= 60 * (int64_t)RATE &&
        short_for_snapshots != snaps.count && n_short < MAX_SHORT_TARGETS && sample - snaps.last_sample >= RATE / 10) {
      /* a target just after a snapshot: the replay is a few slices only, so the effects keep almost all of the state of wherever the seek began */
      targets[n_targets].sample = sample;
      pending = n_targets++;
      short_for_snapshots = snaps.count;
      n_short++;
    }
    if (n_targets < MAX_TARGETS && pending < 0 && count > 0 && sample - last_target >= 35 * (int64_t)RATE && sample > 40 * (int64_t)RATE) {
      targets[n_targets].sample = sample;
      pending = n_targets++;
      last_target = sample;
    }
    (void)water_slices;
  }
  printf("effects replay: %llu slices (%llu with deferred effects), %d seek targets, %d snapshots; a second run in the same slice repeated %llu times, %llu "
         "differed\n",
         (unsigned long long)slices, (unsigned long long)slices_with_effects, n_targets, snaps.count, (unsigned long long)repeated,
         (unsigned long long)repeated_differs);
  CHECK(slices_with_effects > 100 && n_targets >= 4, "the document has effects to test: %llu slices, %d targets", (unsigned long long)slices_with_effects,
        n_targets);
  CHECK(repeated > 50 && repeated_differs > 0, "a second effect run in a slice is observable (%llu of %llu differ): the premise of the live loop's single run",
        (unsigned long long)repeated_differs, (unsigned long long)repeated);

  /* the runtime is at the end of the demo; every target is a backward seek from there (or from the previous replay) */
  int mismatches = 0;
  for (int k = 0; k < n_targets; k++) {
    const psnapshot *snap = psnapshots_before(&snaps, targets[k].sample);
    CHECK(snap != NULL, "a snapshot before target %d", k);
    if (!snap) continue;
    CHECK(wz3_rt_restore(rt, snap->data, snap->size) == WZ3_OK, "restore for target %d", k);
    int64_t at = snap->sample;
    CHECK(preplay_advance(rt, &at, targets[k].sample, RATE, FPS, NULL) == WZ3_OK && at == targets[k].sample, "replay to target %d", k);
    uint32_t hash, count;
    CHECK(live_slice(rt, targets[k].next, &hash, &count) == WZ3_OK, "slice after target %d", k);
    const int same = hash == targets[k].hash && count == targets[k].effects;
    printf("effects replay: seek to %.2f s (snapshot at %.1f s, %u effects next): %s\n", (double)targets[k].sample / RATE, (double)snap->sample / RATE,
           targets[k].effects, same ? "same as linear" : "DIFFERENT from linear");
    mismatches += !same;
    CHECK(same, "target %d (%.2f s): effect output %08x with %u effects after restore + replay, %08x with %u in linear playback", k,
          (double)targets[k].sample / RATE, hash, count, targets[k].hash, targets[k].effects);
  }
  CHECK(mismatches == 0, "%d of %d seeks differ", mismatches, n_targets);

  /* negative control: without the replay (restore, then draw the slice after the target directly) the effects must differ for at least some
     targets, otherwise the comparison above could not have told a broken seek from a good one */
  int skipped_differs = 0;
  for (int k = 0; k < n_targets; k++) {
    const psnapshot *snap = psnapshots_before(&snaps, targets[k].sample);
    if (!snap || wz3_rt_restore(rt, snap->data, snap->size) != WZ3_OK) continue;
    uint32_t hash, count;
    if (live_slice(rt, targets[k].next, &hash, &count) != WZ3_OK) continue;
    skipped_differs += hash != targets[k].hash || count != targets[k].effects;
  }
  printf("effects replay: control: %d of %d seeks differ without the replay\n", skipped_differs, n_targets);
  CHECK(skipped_differs > 0, "the control found no state in the effects: the comparison is blind");
  psnapshots_clear(&snaps);
  wz3_rt_destroy(rt);
  wz3_kx_destroy(kx);
  free(bytes);
  printf("effects replay: %d checks, %d failures\n", g_checks, g_failures);
  return g_failures ? 1 : 0;
}
