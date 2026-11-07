/* player_replay.h: runtime snapshots and the replay that a seek needs, SDL-free so that it can be tested headless.
 * Include wz3.h and player_core.h first.
 *
 *   #define PLAYER_REPLAY_IMPLEMENTATION in exactly one source file.
 *
 * The runtime can only move forward: a backward seek restores the nearest snapshot at or before the target and replays the
 * frames from there at the deterministic 30 fps cadence of the reference player (pseek_next_sample), so that the stateful effects see
 * every time slice. psnapshots keeps a bounded list (the first snapshot always stays, then the most recent ones).
 */
#ifndef PLAYER_REPLAY_H
#define PLAYER_REPLAY_H

#include <stddef.h>
#include <stdint.h>

#ifndef PLAYER_REPLAY_API
#ifdef PLAYER_REPLAY_STATIC
#define PLAYER_REPLAY_API static
#else
#define PLAYER_REPLAY_API extern
#endif
#endif

#define PSNAPSHOT_LIMIT 16

typedef struct psnapshot {
  int64_t sample;
  void *data;
  size_t size;
} psnapshot;

typedef struct psnapshots {
  psnapshot items[PSNAPSHOT_LIMIT];
  int count;
  int64_t last_sample; /* sample of the most recent snapshot taken */
} psnapshots;

PLAYER_REPLAY_API void psnapshots_clear(psnapshots *s);
/* Snapshots the runtime (it grows with the state, so the size is asked right before). WZ3_ERR_OUT_OF_MEMORY leaves the list as it was. */
PLAYER_REPLAY_API wz3_result psnapshots_take(psnapshots *s, const wz3_rt *rt, int64_t sample);
/* The latest snapshot at or before `sample`, or NULL. */
PLAYER_REPLAY_API const psnapshot *psnapshots_before(const psnapshots *s, int64_t sample);
PLAYER_REPLAY_API size_t psnapshots_bytes(const psnapshots *s);

/* One runtime frame at `sample`, then every deferred effect (advanceEffectFrame). */
PLAYER_REPLAY_API wz3_result preplay_step(wz3_rt *rt, int64_t sample, int sample_rate, uint32_t *effects_ran);
/* Frames from *sample to `target` at the replay cadence; *sample ends at the target. */
PLAYER_REPLAY_API wz3_result preplay_advance(wz3_rt *rt, int64_t *sample, int64_t target, int sample_rate, int fps, uint32_t *effects_ran);

typedef struct preplay_check {
  int equal;                      /* restore + replay reproduced linear playback byte for byte */
  int effects_equal;              /* ... and the next slice draws the same chain lines and water (the effect state is not in the snapshot) */
  uint32_t effects_hash, effects; /* what the effects of that slice produced (FNV-1a), and how many ran */
  int64_t middle;                 /* sample of the snapshot restored */
  size_t snapshot_size;
} preplay_check;
/* Linear playback to `target` against restoring a snapshot taken half way and replaying: the same runtime state, and the same effects in the slice after it. */
PLAYER_REPLAY_API wz3_result preplay_seek_check(wz3_rt *rt, int64_t target, int sample_rate, int fps, preplay_check *out);
/* Runs every deferred effect of the frame built last exactly once, like a draw, and hashes what they produce (matrices, chain line points, water arrays). */
PLAYER_REPLAY_API wz3_result preplay_effects_hash(wz3_rt *rt, uint32_t *hash, uint32_t *count);
/* Two snapshots hold the same runtime state: same size, same bytes. */
PLAYER_REPLAY_API int preplay_same_state(const void *a, size_t a_size, const void *b, size_t b_size);

#endif /* PLAYER_REPLAY_H */

#ifdef PLAYER_REPLAY_IMPLEMENTATION
#ifndef PLAYER_REPLAY_IMPLEMENTATION_DONE
#define PLAYER_REPLAY_IMPLEMENTATION_DONE

#include <stdlib.h>
#include <string.h>

#ifndef PLAYER_REPLAY_MALLOC
#define PLAYER_REPLAY_MALLOC(n)     malloc(n)
#define PLAYER_REPLAY_REALLOC(p, n) realloc(p, n)
#define PLAYER_REPLAY_FREE(p)       free(p)
#endif

PLAYER_REPLAY_API void psnapshots_clear(psnapshots *s)
{
  for (int i = 0; i < s->count; i++) PLAYER_REPLAY_FREE(s->items[i].data);
  memset(s, 0, sizeof *s);
}

PLAYER_REPLAY_API wz3_result psnapshots_take(psnapshots *s, const wz3_rt *rt, int64_t sample)
{
  const size_t size = wz3_rt_snapshot_size(rt);
  void *data = PLAYER_REPLAY_MALLOC(size ? size : 1);
  if (!data) return WZ3_ERR_OUT_OF_MEMORY;
  const wz3_result r = wz3_rt_snapshot(rt, data, size);
  if (r != WZ3_OK) {
    PLAYER_REPLAY_FREE(data);
    return r;
  }
  if (s->count == PSNAPSHOT_LIMIT) { /* keep the first, drop the oldest after it */
    PLAYER_REPLAY_FREE(s->items[1].data);
    memmove(&s->items[1], &s->items[2], (size_t)(PSNAPSHOT_LIMIT - 2) * sizeof s->items[0]);
    s->count--;
  }
  s->items[s->count].sample = sample;
  s->items[s->count].data = data;
  s->items[s->count].size = size;
  s->count++;
  s->last_sample = sample;
  return WZ3_OK;
}

PLAYER_REPLAY_API const psnapshot *psnapshots_before(const psnapshots *s, int64_t sample)
{
  const psnapshot *best = NULL;
  for (int i = 0; i < s->count; i++)
    if (s->items[i].sample <= sample && (!best || s->items[i].sample >= best->sample)) best = &s->items[i];
  return best;
}

PLAYER_REPLAY_API size_t psnapshots_bytes(const psnapshots *s)
{
  size_t bytes = 0;
  for (int i = 0; i < s->count; i++) bytes += s->items[i].size;
  return bytes;
}

PLAYER_REPLAY_API wz3_result preplay_step(wz3_rt *rt, int64_t sample, int sample_rate, uint32_t *effects_ran)
{
  wz3_result r = wz3_rt_frame_at_sample(rt, (int32_t)sample, sample_rate);
  if (r != WZ3_OK) return r;
  uint32_t ran;
  r = wz3_rt_effects_advance(rt, effects_ran ? effects_ran : &ran);
  return r;
}

PLAYER_REPLAY_API wz3_result preplay_advance(wz3_rt *rt, int64_t *sample, int64_t target, int sample_rate, int fps, uint32_t *effects_ran)
{
  while (*sample < target) {
    *sample = pseek_next_sample(*sample, target, sample_rate, fps);
    const wz3_result r = preplay_step(rt, *sample, sample_rate, effects_ran);
    if (r != WZ3_OK) return r;
  }
  return WZ3_OK;
}

typedef struct preplay_blob {
  unsigned char *data;
  size_t size;
} preplay_blob;

static wz3_result preplay_blob_snapshot(const wz3_rt *rt, preplay_blob *b)
{
  const size_t size = wz3_rt_snapshot_size(rt);
  unsigned char *data = (unsigned char *)PLAYER_REPLAY_REALLOC(b->data, size ? size : 1);
  if (!data) return WZ3_ERR_OUT_OF_MEMORY;
  b->data = data;
  b->size = size;
  return wz3_rt_snapshot(rt, data, size);
}

PLAYER_REPLAY_API int preplay_same_state(const void *a, size_t a_size, const void *b, size_t b_size)
{
  return a_size == b_size && (a_size == 0 || !memcmp(a, b, a_size));
}

static void preplay_hash_bytes(uint32_t *h, const void *p, size_t n)
{
  const uint8_t *b = (const uint8_t *)p;
  for (size_t i = 0; i < n; i++) *h = (*h ^ b[i]) * 0x01000193u;
}

static void preplay_hash_output(uint32_t *h, const wz3_effect_output *o)
{
  const uint32_t head[2] = { o->kind, o->op };
  preplay_hash_bytes(h, head, sizeof head);
  preplay_hash_bytes(h, o->matrix, sizeof o->matrix);
  preplay_hash_bytes(h, &o->time, sizeof o->time);
  if (o->kind == WZ3_EFFECT_OUT_CHAIN_LINE && o->points) {
    const uint32_t tail[2] = { (uint32_t)o->ripped, o->color };
    preplay_hash_bytes(h, o->points, 32 * 3 * sizeof(float));
    preplay_hash_bytes(h, &o->thickness, sizeof o->thickness);
    preplay_hash_bytes(h, tail, sizeof tail);
  } else if (o->kind == WZ3_EFFECT_OUT_WATER && o->water) {
    const wz3_water_geometry *w = o->water;
    const uint32_t head2[2] = { w->version, w->wave_count };
    preplay_hash_bytes(h, head2, sizeof head2);
    preplay_hash_bytes(h, w->positions, (size_t)w->n_vertices * 3 * sizeof(float));
    preplay_hash_bytes(h, w->normals, (size_t)w->n_vertices * 3 * sizeof(float));
    preplay_hash_bytes(h, w->uvs, (size_t)w->n_vertices * 2 * sizeof(float));
  }
}

/* the traversal of the renderer (IPP roots in order, Layer2D inputs followed, a viewport's effects in draw order) */
static wz3_result preplay_hash_node(wz3_rt *rt, uint32_t node_index, uint32_t *h, uint32_t *count)
{
  const wz3_frame *frame = wz3_rt_frame_jobs(rt);
  const wz3_ipp_node node = frame->nodes[node_index];
  if (node.type == WZ3_IPP_LAYER2D) return node.input >= 0 ? preplay_hash_node(rt, (uint32_t)node.input, h, count) : WZ3_OK;
  if (node.type != WZ3_IPP_VIEWPORT || !node.n_effect) return WZ3_OK;
  uint32_t *order = (uint32_t *)PLAYER_REPLAY_MALLOC(sizeof *order * node.n_effect);
  if (!order) return WZ3_ERR_OUT_OF_MEMORY;
  wz3_frame_effect_order(rt, &node, order);
  wz3_result r = WZ3_OK;
  for (uint32_t i = 0; i < node.n_effect && r == WZ3_OK; i++) {
    const wz3_effect_output *out = NULL;
    r = wz3_rt_effect_run(rt, &frame->effect_jobs[node.first_effect + order[i]], &out);
    if (r == WZ3_OK && out) preplay_hash_output(h, out);
    (*count)++;
  }
  PLAYER_REPLAY_FREE(order);
  return r;
}

PLAYER_REPLAY_API wz3_result preplay_effects_hash(wz3_rt *rt, uint32_t *hash, uint32_t *count)
{
  if (!rt || !hash) return WZ3_ERR_ARGUMENT;
  uint32_t h = 0x811c9dc5u, n = 0;
  const wz3_frame *frame = wz3_rt_frame_jobs(rt);
  wz3_result r = frame ? WZ3_OK : WZ3_ERR_ARGUMENT;
  for (uint32_t i = 0; frame && i < frame->n_outputs && r == WZ3_OK; i++) r = preplay_hash_node(rt, frame->outputs[i], &h, &n);
  *hash = h;
  if (count) *count = n;
  return r;
}

/* the slice after `target` as a draw sees it: the frame, then the effects once, hashed */
static wz3_result preplay_next_slice_hash(wz3_rt *rt, int64_t target, int sample_rate, int fps, uint32_t *hash, uint32_t *count)
{
  const int64_t next = pseek_next_sample(target, target + sample_rate / fps, sample_rate, fps);
  wz3_result r = wz3_rt_frame_at_sample(rt, (int32_t)next, sample_rate);
  return r != WZ3_OK ? r : preplay_effects_hash(rt, hash, count);
}

PLAYER_REPLAY_API wz3_result preplay_seek_check(wz3_rt *rt, int64_t target, int sample_rate, int fps, preplay_check *out)
{
  preplay_blob half = { NULL, 0 }, linear = { NULL, 0 }, replayed = { NULL, 0 };
  memset(out, 0, sizeof *out);
  int64_t sample = 0;
  uint32_t linear_hash = 0, linear_effects = 0, replay_hash = 0, replay_effects = 0;
  wz3_result r = preplay_advance(rt, &sample, target / 2, sample_rate, fps, NULL);
  const int64_t middle = sample;
  if (r == WZ3_OK) r = preplay_blob_snapshot(rt, &half);
  if (r == WZ3_OK) r = preplay_advance(rt, &sample, target, sample_rate, fps, NULL);
  if (r == WZ3_OK) r = preplay_blob_snapshot(rt, &linear);
  if (r == WZ3_OK) r = preplay_next_slice_hash(rt, target, sample_rate, fps, &linear_hash, &linear_effects);
  if (r == WZ3_OK) r = wz3_rt_restore(rt, half.data, half.size);
  sample = middle;
  if (r == WZ3_OK) r = preplay_advance(rt, &sample, target, sample_rate, fps, NULL);
  if (r == WZ3_OK) r = preplay_blob_snapshot(rt, &replayed);
  if (r == WZ3_OK) r = preplay_next_slice_hash(rt, target, sample_rate, fps, &replay_hash, &replay_effects);
  if (r == WZ3_OK) {
    out->equal = preplay_same_state(linear.data, linear.size, replayed.data, replayed.size);
    out->effects_equal = linear_hash == replay_hash && linear_effects == replay_effects;
    out->effects_hash = replay_hash;
    out->effects = replay_effects;
    out->middle = middle;
    out->snapshot_size = linear.size;
  }
  PLAYER_REPLAY_FREE(half.data);
  PLAYER_REPLAY_FREE(linear.data);
  PLAYER_REPLAY_FREE(replayed.data);
  return r;
}

#endif /* PLAYER_REPLAY_IMPLEMENTATION_DONE */
#endif /* PLAYER_REPLAY_IMPLEMENTATION */
