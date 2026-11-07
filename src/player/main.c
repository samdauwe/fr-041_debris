/* debris: the player. SDL3 window and OpenGL ES 3 (or GL 3.3 core) context, V2 audio and the werkkzeug3 runtime.
 *
 *   debris [--kx PATH] [--loader PATH] [--size WxH] [--fullscreen] [--no-vsync] [--no-audio]
 *          [--texture-quality high|normal|low|-3..0] [--single-thread] [--licenses]
 *
 * Without --kx / --loader the assets are looked up in <exe dir>/assets and ./assets.
 *
 * Playing: the loader tune plays while one worker thread precalculates the runtime (generates the meshes, prepares the geometry
 * a scene draws and evaluates the textures the materials sample: player_load.h, player_scene.h) and the main thread draws the
 * loader bar from the worker's progress and uploads the results in slices. Quitting while it loads cancels the worker at its
 * next operator. With --single-thread (always on the web, where a page has no worker thread) the main thread does that work in
 * slices of LOAD_SLICE_NS between two pictures of the loader bar: the runtime stops at an operator when the slice is over and
 * continues in the next one, with the same result. Then the loader synth is destroyed, the party song starts and the visual
 * clock follows the frames the audio device has played (player_core.h).
 *
 * The live loop is live_begin() / live_frame() / live_finish(): a native process runs them in a loop, the browser calls live_frame()
 * from requestAnimationFrame.
 *
 * Keys: Space pause, Left/Right seek -/+10 s, F fullscreen, C credits, F10 statistics overlay, F11 statistics on stdout,
 * Esc quit. Keys pressed while loading are ignored: there is no clock to pause or seek yet.
 *
 * Options for tests and measurements (the test runner starts the player with them; see tests/suites/player_cli.c):
 *   --time SECONDS [--screenshot PATH.ppm] [--overlay credits,stats] [--seek-check]
 *       render the frame at that song time without audio, replaying the runtime at the deterministic 30 fps cadence of the
 *       JavaScript port, and exit. --seek-check proves that restoring a snapshot and replaying reproduces the runtime state of
 *       linear playback.
 *   --times T1,T2,... --screenshot-dir DIR    the same for a list of times (the golden pictures)
 *   --self-check [--script T:KEY,...] [--expect-position SECONDS]
 *       verify the clock (monotonic, never ahead of wall time, not stalled) and the letterboxing; --script injects key presses
 *       T seconds after the party started (space, left, right, f11, fullscreen, credits, stats, resize), independent of how
 *       long the load took; --expect-position compares the final song position (0.5 s tolerance).
 *   --play-for SECONDS / --exit-after SECONDS   quit that long after the party / the process started
 *   --stats     print a statistics line per second
 *   --gl-check  count the GL objects glr creates and deletes and poll glGetError after every frame; errors, and objects left
 *               after glr_destroy, are failures
 *   --soak LOOPS [--soak-limit SECONDS]
 *       play the whole demo LOOPS times in deterministic 30 fps steps without audio, with a backward and a forward seek,
 *       window resizes and the screens of the player in every loop; fails on GL errors, on GL objects that differ between loop
 *       1 and later loops, or on RSS growth. --soak-limit stops it after that many wall seconds.
 *   --licenses  print the credits and the notices of everything the player is built from
 */
#define V2_IMPLEMENTATION
#include <debris/core/v2.h>
#define WZ3_IMPLEMENTATION
#include <debris/core/wz3.h>
#define GLR_IMPLEMENTATION
#include <debris/render/glr.h>
#define PLAYER_CORE_IMPLEMENTATION
#include <debris/player/player_core.h>
#define PLAYER_ARGS_IMPLEMENTATION
#include <debris/player/player_args.h>
#define PLAYER_LOAD_IMPLEMENTATION
#include <debris/player/player_load.h>
#define PLAYER_SCENE_IMPLEMENTATION
#include <debris/player/player_scene.h>
#define PLAYER_REPLAY_IMPLEMENTATION
#include <debris/player/player_replay.h>
#define PLAYER_TEXT_IMPLEMENTATION
#include <debris/player/player_text.h>
#define PLAYER_CREDITS_IMPLEMENTATION
#include <debris/player/player_credits.h>
#include "licenses_data.h"
#define PLAYER_GLCHECK_IMPLEMENTATION
#include <debris/player/player_glcheck.h>

#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#if defined(__EMSCRIPTEN__)
#include <GLES3/gl3.h>
#include <emscripten.h>
#endif

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(__linux__)
#include <unistd.h>
#endif

#define SONG_RATE                 44100
#define PRESENTATION_ASPECT       2.0
#define REPLAY_FPS                30
#define SEEK_STEP_SECONDS         10
#define SNAPSHOT_INTERVAL_SECONDS 30
#define REPLAY_SLICE_NS           8000000ull /* seek replay time per loop iteration */
#define WARM_SLICE_NS             8000000ull /* texture and geometry upload time per loop iteration of the loading screen */
#define CLOCK_EXTRAPOLATE_MS      100
#define AUDIO_CHUNK_FRAMES        4096
#define FRAME_RING                512
#define CURSOR_IDLE_NS            2000000000ull /* the pointer hides after this long without mouse activity */
#define LOAD_SLICE_NS             12000000ull   /* runtime and texture evaluation time per loop iteration when loading without a worker thread */

typedef enum phase { PHASE_LOADING, PHASE_PLAYING, PHASE_SEEKING } phase;

/* ---- --gl-check: what glr asks of the driver, counted independently of glr's own bookkeeping ----
   glr loads its GL functions through one loader; with --gl-check the loader hands out wrappers for the object creating and deleting calls
   (the wrappers need a global: the function table has no user pointer). The bookkeeping itself is player_glcheck.h. */
static pglcheck g_glcheck;

#define PGL_WRAP_GEN(fn, kind)                    \
  static void (*real_##fn)(GLRsizei, GLRuint *);  \
  static void wrap_##fn(GLRsizei n, GLRuint *ids) \
  {                                               \
    real_##fn(n, ids);                            \
    pglcheck_created(&g_glcheck, kind, n, ids);   \
  }
#define PGL_WRAP_DELETE(fn, kind)                       \
  static void (*real_##fn)(GLRsizei, const GLRuint *);  \
  static void wrap_##fn(GLRsizei n, const GLRuint *ids) \
  {                                                     \
    pglcheck_deleted(&g_glcheck, kind, n, ids);         \
    real_##fn(n, ids);                                  \
  }
PGL_WRAP_GEN(glGenTextures, PGL_TEXTURE)
PGL_WRAP_GEN(glGenBuffers, PGL_BUFFER)
PGL_WRAP_GEN(glGenVertexArrays, PGL_VERTEX_ARRAY)
PGL_WRAP_GEN(glGenFramebuffers, PGL_FRAMEBUFFER)
PGL_WRAP_GEN(glGenRenderbuffers, PGL_RENDERBUFFER)
PGL_WRAP_DELETE(glDeleteTextures, PGL_TEXTURE)
PGL_WRAP_DELETE(glDeleteBuffers, PGL_BUFFER)
PGL_WRAP_DELETE(glDeleteVertexArrays, PGL_VERTEX_ARRAY)
PGL_WRAP_DELETE(glDeleteFramebuffers, PGL_FRAMEBUFFER)
PGL_WRAP_DELETE(glDeleteRenderbuffers, PGL_RENDERBUFFER)
static GLRuint (*real_glCreateProgram)(void);
static GLRuint wrap_glCreateProgram(void)
{
  const GLRuint id = real_glCreateProgram();
  pglcheck_created(&g_glcheck, PGL_PROGRAM, 1, &id);
  return id;
}
static GLRuint (*real_glCreateShader)(GLRenum);
static GLRuint wrap_glCreateShader(GLRenum type)
{
  const GLRuint id = real_glCreateShader(type);
  pglcheck_created(&g_glcheck, PGL_SHADER, 1, &id);
  return id;
}
static void (*real_glDeleteProgram)(GLRuint);
static void wrap_glDeleteProgram(GLRuint id)
{
  pglcheck_deleted(&g_glcheck, PGL_PROGRAM, 1, &id);
  real_glDeleteProgram(id);
}
static void (*real_glDeleteShader)(GLRuint);
static void wrap_glDeleteShader(GLRuint id)
{
  pglcheck_deleted(&g_glcheck, PGL_SHADER, 1, &id);
  real_glDeleteShader(id);
}

static void *proc_address(const char *name)
{
#if defined(__EMSCRIPTEN__)
  /* The GL functions are linked directly: no glGetProcAddress table, and nothing glr does not call is linked. */
#define PGL_WEB_FN(ret, fn, args) \
  if (!strcmp(name, #fn)) return (void *)fn;
  GLR_GL_FUNCTIONS(PGL_WEB_FN)
#undef PGL_WEB_FN
  return NULL;
#else
  const SDL_FunctionPointer f = SDL_GL_GetProcAddress(name);
  void *p = NULL;
  memcpy(&p, &f, sizeof p < sizeof f ? sizeof p : sizeof f);
  return p;
#endif
}

static void *counting_loader(const char *name)
{
  void *p = proc_address(name);
  if (!p) return p;
#define PGL_HOOK(fn)                    \
  do {                                  \
    if (!strcmp(name, #fn)) {           \
      memcpy(&real_##fn, &p, sizeof p); \
      return (void *)wrap_##fn;         \
    }                                   \
  } while (0)
  PGL_HOOK(glGenTextures);
  PGL_HOOK(glGenBuffers);
  PGL_HOOK(glGenVertexArrays);
  PGL_HOOK(glGenFramebuffers);
  PGL_HOOK(glGenRenderbuffers);
  PGL_HOOK(glDeleteTextures);
  PGL_HOOK(glDeleteBuffers);
  PGL_HOOK(glDeleteVertexArrays);
  PGL_HOOK(glDeleteFramebuffers);
  PGL_HOOK(glDeleteRenderbuffers);
  PGL_HOOK(glCreateProgram);
  PGL_HOOK(glCreateShader);
  PGL_HOOK(glDeleteProgram);
  PGL_HOOK(glDeleteShader);
#undef PGL_HOOK
  return p;
}

/* The statistics of a frame that glr counted, for the overlay and the logs. */
typedef struct frame_stats {
  uint32_t draws, triangles;
} frame_stats;

typedef struct soak_loop {
  uint64_t frames;
  double p50, p95, max_ms;
  int64_t objects;
  double rss_mib;
  pglcheck gl;
} soak_loop;


typedef struct app {
  poptions opt;
  unsigned char *kx_bytes, *loader_bytes;
  size_t kx_size, loader_size;
  wz3_kx *kx;
  wz3_rt *rt;
  SDL_Window *window;
  SDL_GLContext gl;
  glr *renderer;
  SDL_AudioStream *stream;
  uint32_t device_latency_frames;
  int dummy_audio;
  paudio audio;
  pclock clock;
  /* worker */
  SDL_Thread *worker;
  SDL_AtomicInt worker_done;
  SDL_AtomicInt progress_done, progress_total; /* operators calculated / reachable, written by the worker */
  SDL_AtomicInt texture_done, texture_total;   /* textures evaluated / to evaluate on the worker */
  SDL_AtomicInt cancel_load;                   /* set by the main thread: the worker stops at the next operator */
  wz3_result precalc_result;
  pscene scene;                                /* geometry, shadow data, textures; the worker fills it, the GL thread reads it after the join */
  int warming;                                 /* the worker is done and the GL thread is uploading */
  int worker_joined;
  int load_stage;                              /* without a worker: 0 runtime, 1 scene plan, 2 textures, 3 done */
  uint64_t slice_deadline_ns;                  /* without a worker: the host stops the load at the next operator once this has passed (0: never) */
  uint64_t live_start_ns;                      /* when the live loop began */
  int live_rc;                                 /* exit status of the live loop, valid once live_frame() returns 0 */
  uint32_t warm_done, warm_total;
  uint64_t render_errors;                      /* frames glr reported a failure for */
  /* the window, statistics and the screens of the player itself */
  int minimized;
  int show_credits, show_stats;
  int resize_step;                   /* where the resize script key is in its cycle of sizes */
  ptext_image credits_title, credits_subtitle, credits_roles, stats_image;
  int credits_px_height;             /* the drawable height the credits images were made for */
  uint64_t stats_ns, stats_print_ns; /* when the overlay text / the --stats line was last made */
  int resizes_seen, resize_checks_failed;
  frame_stats last_frame;
  /* state of the visual timeline */
  phase phase;
  int paused;
  int64_t sample, duration_samples;
  uint64_t t0_ns;         /* wall time of the party start (no-audio clock, self-check); moves with pauses and seeks */
  uint64_t play_start_ns; /* wall time the party started; --script and --play-for count from here */
  /* seek */
  psnapshots snaps;
  int64_t replay_sample, replay_target;
  int resume_after_seek;
  /* statistics and self-check */
  float frame_ms[FRAME_RING];
  uint64_t frames, last_frame_ns;
  uint32_t effects_ran;
  int64_t last_clock;
  int violations;
  int next_script;
  uint64_t cursor_active_ns; /* last mouse activity */
  int cursor_hidden;
} app;

static int fail(const char *what, const char *detail)
{
  fprintf(stderr, "debris: %s%s%s\n", what, detail ? ": " : "", detail ? detail : "");
  return 1;
}

/* ---- files and assets ---- */

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

static int load_asset(const char *explicit_path, const char *name, unsigned char **data, size_t *size)
{
  if (explicit_path) return read_file(explicit_path, data, size);
  char path[1024];
  const char *base = SDL_GetBasePath();
  if (base) {
    snprintf(path, sizeof path, "%sassets/%s", base, name);
    if (read_file(path, data, size)) return 1;
  }
  snprintf(path, sizeof path, "assets/%s", name);
  return read_file(path, data, size);
}

static int write_ppm(const char *path, const uint8_t *rgba, int w, int h)
{
  FILE *f = fopen(path, "wb");
  if (!f) return 0;
  fprintf(f, "P6\n%d %d\n255\n", w, h);
  for (int i = 0; i < w * h; i++) {
    if (fwrite(rgba + (size_t)i * 4, 1, 3, f) != 3) {
      fclose(f);
      return 0;
    }
  }
  return fclose(f) == 0;
}

/* ---- audio device ---- */

static void SDLCALL audio_callback(void *user, SDL_AudioStream *stream, int additional, int total)
{
  app *a = (app *)user;
  float buf[AUDIO_CHUNK_FRAMES * 2];
  (void)total;
  if (a->audio.paused || a->audio.stage == PAUDIO_SILENT) return; /* no data: the device plays silence */
  while (additional >= (int)(2 * sizeof(float))) {
    int frames = additional / (int)(2 * sizeof(float));
    if (frames > AUDIO_CHUNK_FRAMES) frames = AUDIO_CHUNK_FRAMES;
    paudio_render(&a->audio, buf, (uint32_t)frames);
    if (!SDL_PutAudioStreamData(stream, buf, frames * (int)(2 * sizeof(float)))) return;
    additional -= frames * (int)(2 * sizeof(float));
  }
}

static int audio_open(app *a)
{
  SDL_AudioSpec spec;
  SDL_zero(spec);
  spec.format = SDL_AUDIO_F32;
  spec.channels = 2;
  spec.freq = SONG_RATE;
  a->stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, audio_callback, a);
  if (!a->stream) return 0;
  SDL_AudioSpec device;
  int device_frames = 0;
  if (SDL_GetAudioDeviceFormat(SDL_GetAudioStreamDevice(a->stream), &device, &device_frames) && device.freq > 0 && device_frames > 0)
    a->device_latency_frames = (uint32_t)((double)device_frames * SONG_RATE / device.freq);
  const char *driver = SDL_GetCurrentAudioDriver();
  a->dummy_audio = driver && !strcmp(driver, "dummy");
  return SDL_ResumeAudioStreamDevice(a->stream);
}

/* The song position the audio device has reached: frames put - frames still queued - device buffer. */
static int64_t audio_played_frames(app *a)
{
  int64_t played;
  if (a->stream) {
    SDL_LockAudioStream(a->stream);
    const int64_t put = (int64_t)(a->audio.party_base + a->audio.party_frames);
    const int64_t queued = SDL_GetAudioStreamQueued(a->stream) / (int)(2 * sizeof(float));
    SDL_UnlockAudioStream(a->stream);
    played = put - queued - (int64_t)a->device_latency_frames;
  } else {
    played = (int64_t)((double)(SDL_GetTicksNS() - a->t0_ns) * SONG_RATE / 1e9) + (int64_t)a->audio.party_base;
  }
  return played < 0 ? 0 : played;
}

/* ---- runtime ---- */

static int host_cancelled(void *user)
{
  app *a = (app *)user;
  return SDL_GetAtomicInt(&a->cancel_load) || (a->slice_deadline_ns && SDL_GetTicksNS() >= a->slice_deadline_ns);
}
static void host_progress(void *user, uint32_t op, uint32_t done, uint32_t total)
{
  app *a = (app *)user;
  (void)op;
  SDL_SetAtomicInt(&a->progress_done, (int)done);
  SDL_SetAtomicInt(&a->progress_total, (int)total);
}
static void host_texture_progress(void *user, uint32_t done, uint32_t total)
{
  app *a = (app *)user;
  if ((int)done > SDL_GetAtomicInt(&a->texture_done)) SDL_SetAtomicInt(&a->texture_done, (int)done); /* a resumed evaluation counts from 0 again */
  SDL_SetAtomicInt(&a->texture_total, (int)total);
}

/* Runs the precalculation until it is done or, with a deadline, until the host asks it to stop; a stopped load resumes where it was in
   the next call and ends in the state of an uninterrupted one. Returns 1 when finished (precalc_result), 0 when there is more. */
static int precalc_run(app *a, uint64_t budget_ns)
{
  a->slice_deadline_ns = budget_ns ? SDL_GetTicksNS() + budget_ns : 0;
  wz3_result r = WZ3_OK;
  if (a->load_stage == 0 && (r = wz3_rt_precalc(a->rt)) == WZ3_OK) a->load_stage = 1;
  if (r == WZ3_OK && a->load_stage == 1 && (r = pscene_plan(&a->scene)) == WZ3_OK) a->load_stage = 2;
  if (r == WZ3_OK && a->load_stage == 2 && (r = pscene_eval_textures(&a->scene, host_texture_progress, host_cancelled, a)) == WZ3_OK) a->load_stage = 3;
  a->slice_deadline_ns = 0;
  if (r == WZ3_ERR_CANCELLED && !SDL_GetAtomicInt(&a->cancel_load)) return 0;
  a->precalc_result = r;
  return 1;
}

static int SDLCALL precalc_main(void *user)
{
  app *a = (app *)user;
  precalc_run(a, 0);
  SDL_SetAtomicInt(&a->worker_done, 1);
  return 0;
}

static wz3_result host_mesh(void *user, uint32_t op, const wz3_mesh *mesh) { return pscene_mesh(&((app *)user)->scene, op, mesh); }
static wz3_result host_minmesh(void *user, uint32_t op, const wz3_minmesh *mesh) { return pscene_minmesh(&((app *)user)->scene, op, mesh); }
static const wz3_bitmap *host_bitmap(void *user, uint32_t op) { return pscene_bitmap(&((app *)user)->scene, op); }

static int runtime_create(app *a)
{
  pscene_options so;
  memset(&so, 0, sizeof so);
  so.texture_offset = a->opt.texture_offset;
  so.bitmap_render = 1;
  so.release_uploaded = 1;
  wz3_result res = pscene_create(&a->scene, a->kx, &so);
  if (res != WZ3_OK) return fail("scene", wz3_result_string(res));
  wz3_rt_options o;
  memset(&o, 0, sizeof o);
  o.generic_exec = 1;
  o.release_consumed = 1;
  o.frame_jobs = 1;
  o.host.user = a;
  o.host.mesh = host_mesh;
  o.host.minmesh = host_minmesh;
  o.host.bitmap = host_bitmap;
  o.host.progress = host_progress;
  o.host.cancelled = host_cancelled;
  a->rt = wz3_rt_create(a->kx, &o, &res);
  if (!a->rt) return fail("runtime", wz3_result_string(res));
  pscene_attach(&a->scene, a->rt);
  wz3_rt_set_aspect(a->rt, PRESENTATION_ASPECT);
  return 0;
}

/* A failed snapshot only makes a later backward seek replay from an older one: say so once per interval and go on. */
static void snapshot_take(app *a, int64_t sample)
{
  const wz3_result r = psnapshots_take(&a->snaps, a->rt, sample);
  if (r == WZ3_OK) return;
  fprintf(stderr, "debris: snapshot at %.1f s failed: %s\n", (double)sample / SONG_RATE, wz3_result_string(r));
  a->snaps.last_sample = sample;
}

/* One runtime frame at `sample`; the stateful effects see every time slice (advanceEffectFrame). For a seek's replay, whose slices are not drawn. */
static wz3_result runtime_step(app *a, int64_t sample) { return preplay_step(a->rt, sample, SONG_RATE, &a->effects_ran); }

/* One runtime frame at `sample` that is about to be drawn: the renderer runs the deferred effects itself, once per slice, like the browser's
   draw does, so running them here as well would move the chain lines and the water twice as fast (test_player_effects_replay measures it). */
static wz3_result runtime_frame(app *a, int64_t sample) { return wz3_rt_frame_at_sample(a->rt, (int32_t)sample, SONG_RATE); }

/* ---- rendering ---- */

/* glGetError after a frame (--gl-check): every error in the queue is counted and the first few are said. */
static void poll_gl_errors(app *a)
{
  if (!a->opt.gl_check) return;
  static GLRenum (*get_error)(void);
  if (!get_error) {
    void *p = proc_address("glGetError");
    memcpy(&get_error, &p, sizeof p);
  }
  if (!get_error) return;
  uint32_t codes[16];
  int n = 0;
  while (n < 16) {
    const uint32_t code = get_error();
    if (!code) break;
    codes[n++] = code;
  }
  const uint64_t before = g_glcheck.errors;
  pglcheck_polled(&g_glcheck, codes, n);
  for (int i = 0; i < n && before + (uint64_t)i < 5; i++)
    fprintf(stderr, "debris: GL error %s (0x%x) after frame %llu\n", pglcheck_error_name(codes[i]), codes[i], (unsigned long long)a->frames);
}

/* One frame of the runtime's frame jobs. glr reports an allocation failure after drawing what it could: say so (once per 100 frames),
   keep playing, and return the result so the golden path can refuse the picture. */
static glr_result render_frame(app *a)
{
  const glr_result r = glr_render_frame(a->renderer, pscene_glr_host(&a->scene));
  if (r != GLR_OK && a->render_errors++ % 100 == 0)
    fprintf(stderr, "debris: frame %llu: %s\n", (unsigned long long)a->frames,
            r == GLR_ERR_OUT_OF_MEMORY ? "out of memory while drawing" : glr_last_error(a->renderer));
  glr_stats st;
  glr_get_stats(a->renderer, &st);
  a->last_frame.draws = st.draw_calls;
  a->last_frame.triangles = st.triangles;
  return r;
}

/* The window's drawable in pixels into the renderer: after a resize, after the move to another display (HiDPI), and after a Render operator
   left it at the size of its bitmap. The letterboxed presentation must then fit the drawable at the production's 2:1 (--self-check says
   when it does not). `force` resizes even when the size is the one the renderer has. Returns 0 on failure (reported). */
static int fit_renderer(app *a, int force)
{
  int w, h;
  SDL_GetWindowSizeInPixels(a->window, &w, &h);
  glr_stats before;
  glr_get_stats(a->renderer, &before);
  if (!force && before.width == (w < 1 ? 1 : w) && before.height == (h < 1 ? 1 : h)) return 1;
  if (glr_resize(a->renderer, w, h, PRESENTATION_ASPECT) != GLR_OK) {
    fail("renderer resize", glr_last_error(a->renderer));
    return 0;
  }
  if (before.width != (w < 1 ? 1 : w) || before.height != (h < 1 ? 1 : h)) a->resizes_seen++;
  glr_stats st;
  glr_get_stats(a->renderer, &st);
  const wz3_region p = st.presentation;
  const int fits = p.x >= 0 && p.y >= 0 && p.x + p.width <= st.width && p.y + p.height <= st.height && p.width >= 1 && p.height >= 1;
  const int aspect_ok = (p.width == st.width || p.height == st.height) && abs(p.width - 2 * p.height) <= 2;
  const int centred = abs((st.width - p.width) - 2 * p.x) <= 1 && abs((st.height - p.height) - 2 * p.y) <= 1;
  if (st.width >= 4 && st.height >= 2 && (!fits || !aspect_ok || !centred)) {
    a->resize_checks_failed++;
    if (a->opt.self_check) {
      a->violations++;
      fprintf(stderr, "self-check: drawable %dx%d presents at %d,%d %dx%d (not a centred 2:1 fit)\n", st.width, st.height, p.x, p.y, p.width, p.height);
    }
  }
  a->credits_px_height = 0; /* the screens of the player are laid out for the drawable */
  return 1;
}

/* ---- the screens of the player itself: credits and statistics ---- */

static int clamp_int(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

static void make_credits(app *a, int h)
{
  if (a->credits_px_height == h) return;
  ptext_free(&a->credits_title);
  ptext_free(&a->credits_subtitle);
  ptext_free(&a->credits_roles);
  ptext_render(&a->credits_title, pcredits_title(), clamp_int(h / 10, 14, 256), 0xffffff); /* a failure leaves the image empty: not drawn */
  ptext_render(&a->credits_subtitle, pcredits_subtitle(), clamp_int(h / 34, 8, 256), 0xc8c8c8);
  ptext_render(&a->credits_roles, pcredits_roles(), clamp_int(h / 38, 8, 256), 0xa0a0a0);
  a->credits_px_height = h;
}

static void overlay_image(app *a, const ptext_image *im, int x, int y)
{
  if (im->rgba) glr_present_overlay(a->renderer, im->rgba, im->width, im->height, x, y);
}

/* The title above and the credits below the loader bar (the same arrangement as the launcher of the browser port). */
static void draw_credits(app *a, int w, int h)
{
  make_credits(a, h);
  const ptext_image *t = &a->credits_title, *s = &a->credits_subtitle, *r = &a->credits_roles;
  const int title_y = h / 2 - h / 10 - t->height;
  overlay_image(a, t, (w - t->width) / 2, title_y);
  overlay_image(a, s, (w - s->width) / 2, title_y + t->height + h / 80);
  overlay_image(a, r, (w - r->width) / 2, h / 2 + h / 10);
}

static void frame_percentiles(const app *a, double *fps, double *p50, double *p95, double *max_ms)
{
  const uint64_t n = a->frames < FRAME_RING ? a->frames : FRAME_RING;
  *fps = *p50 = *p95 = *max_ms = 0;
  if (!n) return;
  float sorted[FRAME_RING];
  double sum = 0;
  memcpy(sorted, a->frame_ms, (size_t)n * sizeof sorted[0]);
  for (uint64_t i = 0; i < n; i++) sum += sorted[i];
  for (uint64_t i = 1; i < n; i++) { /* insertion sort: 512 entries, rarely called */
    const float v = sorted[i];
    uint64_t j = i;
    for (; j > 0 && sorted[j - 1] > v; j--) sorted[j] = sorted[j - 1];
    sorted[j] = v;
  }
  *fps = sum > 0 ? (double)n * 1000.0 / sum : 0;
  *p50 = sorted[n / 2];
  *p95 = sorted[(n * 95) / 100 < n ? (n * 95) / 100 : n - 1];
  *max_ms = sorted[n - 1];
}

/* The statistics text: frame rate and times, what the last frame drew, the song position, the drawable (and the GL objects with --gl-check). */
static void format_stats(const app *a, char *out, size_t size)
{
  double fps, p50, p95, max_ms;
  frame_percentiles(a, &fps, &p50, &p95, &max_ms);
  glr_stats st;
  glr_get_stats(a->renderer, &st);
  size_t used = (size_t)snprintf(out, size,
                                 "%.1f fps  frame %.2f ms (p95 %.2f, max %.2f)\ndraws %u  triangles %u  culled %u of %u objects, %u of %u lights\nsong %.1f s  "
                                 "drawable %dx%d  picture %dx%d",
                                 fps, p50, p95, max_ms, a->last_frame.draws, a->last_frame.triangles, st.culling.culled_view, st.culling.candidate_view,
                                 st.culling.culled_light, st.culling.candidate_light, (double)a->sample / SONG_RATE, st.width, st.height, st.render_width,
                                 st.render_height);
  if (used >= size) used = size - 1;
  if (a->opt.gl_check && used + 1 < size) {
    char objects[200];
    pglcheck_format(&g_glcheck, objects, sizeof objects);
    snprintf(out + used, size - used, "\nGL %s", objects);
  }
}

static void draw_stats(app *a)
{
  const uint64_t now = SDL_GetTicksNS();
  if (!a->stats_image.rgba || now - a->stats_ns >= 500000000ull) { /* the text is remade twice a second */
    char text[512];
    format_stats(a, text, sizeof text);
    glr_stats st;
    glr_get_stats(a->renderer, &st);
    ptext_free(&a->stats_image);
    ptext_render(&a->stats_image, text, clamp_int(st.height / 40, 10, 64), 0xffff80);
    a->stats_ns = now;
  }
  overlay_image(a, &a->stats_image, 8, 8);
}

static void draw_overlays(app *a, int credits)
{
  glr_stats st;
  glr_get_stats(a->renderer, &st);
  if (credits || a->show_credits) draw_credits(a, st.width, st.height);
  if (a->show_stats) draw_stats(a);
}

/* The first-frame work the GL thread owes after the precalculation: Render operators, GL textures, GL geometry. `budget_ns` bounds one
   call (0: run to the end); returns 1 when everything is warm, 0 when there is more, -1 on failure (reported). */
static int warm_scene(app *a, uint64_t budget_ns)
{
  const uint64_t start = SDL_GetTicksNS();
  if (!a->warming) {
    pscene_set_renderer(&a->scene, a->renderer);
    a->warming = 1;
  }
  do {
    const glr_result r = pscene_warm_step(&a->scene, &a->warm_done, &a->warm_total);
    if (r != GLR_OK) {
      fail("upload", r == GLR_ERR_OUT_OF_MEMORY ? "out of memory" : glr_last_error(a->renderer));
      return -1;
    }
    if (a->warm_done >= a->warm_total) return fit_renderer(a, 1) ? 1 : -1;
  } while (!budget_ns || SDL_GetTicksNS() - start < budget_ns);
  return fit_renderer(a, 1) ? 0 : -1;
}

static int create_window(app *a)
{
  SDL_WindowFlags flags = SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY;
  if (a->opt.fullscreen) flags |= SDL_WINDOW_FULLSCREEN;
  if (a->opt.fixed_time >= 0) flags |= SDL_WINDOW_HIDDEN;
  static const struct {
    int profile, major, minor;
  } contexts[] = { { SDL_GL_CONTEXT_PROFILE_ES, 3, 0 }, { SDL_GL_CONTEXT_PROFILE_CORE, 3, 3 } };
  for (size_t i = 0; i < sizeof contexts / sizeof contexts[0] && !a->gl; i++) {
    SDL_GL_ResetAttributes();
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, contexts[i].profile);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, contexts[i].major);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, contexts[i].minor);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 0);
    a->window = SDL_CreateWindow("fr-041: debris", a->opt.width, a->opt.height, flags);
    if (!a->window) return fail("window", SDL_GetError());
    a->gl = SDL_GL_CreateContext(a->window);
    if (!a->gl) {
      SDL_DestroyWindow(a->window);
      a->window = NULL;
    }
  }
  if (!a->gl) return fail("OpenGL context (ES 3.0 and 3.3 core failed)", SDL_GetError());
  SDL_GL_SetSwapInterval(a->opt.vsync ? 1 : 0);
  char error[1024] = "";
  glr_options renderer_options;
  memset(&renderer_options, 0, sizeof renderer_options);
  renderer_options.error = error;
  renderer_options.error_size = sizeof error;
  glr_result res;
  pglcheck_init(&g_glcheck);
  a->renderer = glr_create(a->opt.gl_check ? counting_loader : proc_address, &renderer_options, &res);
  if (!a->renderer) return fail("renderer", error);
  int w, h;
  SDL_GetWindowSizeInPixels(a->window, &w, &h);
  if (glr_resize(a->renderer, w, h, PRESENTATION_ASPECT) != GLR_OK) return fail("renderer resize", glr_last_error(a->renderer));
  poll_gl_errors(a);
  return 0;
}

/* ---- timeline ---- */

static double document_seconds(const wz3_kx *kx) { return kx->song_bpm_fixed ? (double)kx->song_length * 60.0 / (double)kx->song_bpm_fixed : 0.0; }

static void start_party(app *a)
{
  if (a->opt.audio) {
    SDL_LockAudioStream(a->stream);
    SDL_ClearAudioStream(a->stream);
    paudio_play_party(&a->audio, a->kx->song, a->kx->song_size, 0);
    SDL_UnlockAudioStream(a->stream);
    a->duration_samples = (int64_t)a->audio.length_frames;
  } else {
    a->audio.party_base = 0;
    a->audio.party_frames = 0;
    a->duration_samples = (int64_t)(document_seconds(a->kx) * SONG_RATE);
  }
  a->t0_ns = SDL_GetTicksNS();
  a->play_start_ns = a->t0_ns;
  pclock_init(&a->clock, SONG_RATE, CLOCK_EXTRAPOLATE_MS);
  pclock_reset(&a->clock, 0, a->t0_ns);
  pclock_set_running(&a->clock, 1, a->t0_ns);
  a->sample = a->last_clock = 0;
  snapshot_take(a, 0);
  if (runtime_frame(a, 0) != WZ3_OK) fprintf(stderr, "debris: the first frame failed\n"); /* the picture before the clock moves */
  a->phase = PHASE_PLAYING;
}

/* Pausing rewinds the audio to the picture so that resuming loses and repeats nothing. */
static void set_paused(app *a, int paused)
{
  if (a->phase != PHASE_PLAYING || paused == a->paused) return;
  const uint64_t now = SDL_GetTicksNS();
  if (paused) {
    const int64_t at = pclock_now(&a->clock, now);
    pclock_set_running(&a->clock, 0, now);
    if (a->stream) {
      SDL_LockAudioStream(a->stream);
      paudio_seek(&a->audio, (uint32_t)((double)at * 1000.0 / SONG_RATE));
      a->audio.party_base = (uint64_t)at;
      a->audio.paused = 1;
      SDL_ClearAudioStream(a->stream);
      SDL_UnlockAudioStream(a->stream);
    }
  } else {
    if (a->stream) {
      SDL_LockAudioStream(a->stream);
      a->audio.paused = 0;
      SDL_UnlockAudioStream(a->stream);
    }
    a->t0_ns = now - (uint64_t)((double)a->sample * 1e9 / SONG_RATE);
    pclock_reset(&a->clock, a->sample, now);
    pclock_set_running(&a->clock, 1, now);
  }
  a->paused = paused;
}

static void begin_seek(app *a, int delta_seconds)
{
  if (a->phase != PHASE_PLAYING) return;
  int64_t target = a->sample + (int64_t)delta_seconds * SONG_RATE;
  if (target < 0) target = 0;
  if (a->duration_samples > 0 && target > a->duration_samples) target = a->duration_samples;
  int64_t start = a->sample;
  if (target < a->sample) { /* a backward seek restores the nearest snapshot first, before anything else changes */
    const psnapshot *snap = psnapshots_before(&a->snaps, target);
    if (!snap || wz3_rt_restore(a->rt, snap->data, snap->size) != WZ3_OK) {
      fprintf(stderr, "debris: seek restore failed; staying at %.1f s\n", (double)a->sample / SONG_RATE);
      return;
    }
    start = snap->sample;
  }
  a->resume_after_seek = !a->paused;
  if (a->stream) {
    SDL_LockAudioStream(a->stream);
    paudio_seek(&a->audio, (uint32_t)((double)target * 1000.0 / SONG_RATE));
    a->audio.party_base = (uint64_t)target;
    a->audio.paused = 1;
    SDL_ClearAudioStream(a->stream);
    SDL_UnlockAudioStream(a->stream);
  }
  pclock_set_running(&a->clock, 0, SDL_GetTicksNS());
  a->replay_sample = start;
  a->replay_target = target;
  a->phase = PHASE_SEEKING;
}

static void finish_seek(app *a)
{
  const uint64_t now = SDL_GetTicksNS();
  a->sample = a->replay_target;
  a->t0_ns = now - (uint64_t)((double)a->sample * 1e9 / SONG_RATE);
  pclock_reset(&a->clock, a->sample, now);
  a->last_clock = a->sample;
  if (a->resume_after_seek) {
    if (a->stream) {
      SDL_LockAudioStream(a->stream);
      a->audio.paused = 0;
      SDL_UnlockAudioStream(a->stream);
    }
    pclock_set_running(&a->clock, 1, now);
  }
  a->phase = PHASE_PLAYING;
}

static void print_stats(const app *a)
{
  if (!a->frames) return;
  double fps, p50, p95, max_ms;
  frame_percentiles(a, &fps, &p50, &p95, &max_ms);
  glr_stats st;
  glr_get_stats(a->renderer, &st);
  printf("frames %llu  %.1f fps  frame ms p50 %.2f p95 %.2f max %.2f  position %.2f s  snapshots %d (%.1f MB)\n", (unsigned long long)a->frames, fps, p50, p95,
         max_ms, (double)a->sample / SONG_RATE, a->snaps.count, (double)psnapshots_bytes(&a->snaps) / (1024.0 * 1024.0));
  printf("drawable %dx%d, picture %dx%d at %d,%d; last frame %u draws, %u triangles; culled %u of %u objects, %u of %u lights, %u of %u shadows\n", st.width,
         st.height, st.render_width, st.render_height, st.presentation.x, st.presentation.y, a->last_frame.draws, a->last_frame.triangles,
         st.culling.culled_view, st.culling.candidate_view, st.culling.culled_light, st.culling.candidate_light,
         st.culling.culled_shadow_frustum + st.culling.culled_shadow_sphere, st.culling.candidate_shadow);
  printf("geometry %u prepared (%.1f MiB), %u generator intermediates skipped; %u textures, %u freed after upload\n", a->scene.geometry.prepared,
         (double)a->scene.geometry.bytes / (1024.0 * 1024.0), a->scene.geometry.skipped, a->scene.n_textures, a->scene.released);
  if (a->opt.gl_check) {
    char objects[256];
    pglcheck_format(&g_glcheck, objects, sizeof objects);
    printf("GL objects: %s\n", objects);
  }
  fflush(stdout);
}

/* ---- the fixed-time (golden) path ---- */

/* Linear playback to the target versus restoring a snapshot taken half way and replaying: same runtime state. */
static int seek_check(app *a, int64_t target)
{
  preplay_check check;
  const wz3_result res = preplay_seek_check(a->rt, target, SONG_RATE, REPLAY_FPS, &check);
  if (res != WZ3_OK)
    fprintf(stderr, "debris: seek-check: %s\n", wz3_result_string(res));
  else if (!check.equal)
    fprintf(stderr, "debris: seek-check: restore + replay differs from linear playback\n");
  else if (!check.effects_equal)
    fprintf(stderr, "debris: seek-check: the chain lines or the water after restore + replay differ from linear playback\n");
  else {
    printf("seek-check: %.1f s linear == restore at %.1f s + replay (%zu byte snapshots); the next slice's %u effects are the same (%08x)\n",
           (double)target / SONG_RATE, (double)check.middle / SONG_RATE, check.snapshot_size, check.effects, check.effects_hash);
    return 0;
  }
  return 1;
}

/* The picture of song position `target`, replayed from `*at` at the deterministic cadence and drawn: the golden-capture path of the reference player. 0 on success. */
static int render_time(app *a, int64_t target, int64_t *at)
{
  wz3_result res = preplay_advance(a->rt, at, target, SONG_RATE, REPLAY_FPS, &a->effects_ran);
  if (res != WZ3_OK) return fail("frame", wz3_result_string(res));
  if (target == 0) {
    res = runtime_step(a, 0);
    if (res != WZ3_OK) return fail("frame", wz3_result_string(res));
  }
  if (render_frame(a) != GLR_OK) return fail("frame", "the renderer could not draw it");
  a->sample = target;
  if (a->opt.overlay_credits || a->opt.overlay_stats) { /* the screens of the player over the picture (the golden pictures have none) */
    a->show_credits = a->opt.overlay_credits;
    a->show_stats = a->opt.overlay_stats;
    draw_overlays(a, 0);
  }
  poll_gl_errors(a);
  return 0;
}

/* The drawable as a PPM, with what it holds said on stdout (for the golden tests and for a glance at a log). 0 on success. */
static int capture_picture(app *a, const char *path)
{
  int w, h;
  SDL_GetWindowSizeInPixels(a->window, &w, &h);
  uint8_t *rgba = (uint8_t *)malloc((size_t)w * (size_t)h * 4);
  if (!rgba) return fail("screenshot", "out of memory");
  const int ok = glr_read_pixels(a->renderer, rgba, w, h) && write_ppm(path, rgba, w, h);
  if (ok) {
    uint64_t lit = 0, colours = 0;
    static uint8_t seen[32768 / 8]; /* 5 bits per channel */
    memset(seen, 0, sizeof seen);
    for (size_t i = 0; i < (size_t)w * (size_t)h; i++) {
      const uint8_t *px = rgba + i * 4;
      lit += (px[0] | px[1] | px[2]) != 0;
      const unsigned key = ((unsigned)(px[0] >> 3) << 10) | ((unsigned)(px[1] >> 3) << 5) | (unsigned)(px[2] >> 3);
      if (!(seen[key >> 3] >> (key & 7) & 1u)) {
        seen[key >> 3] |= (uint8_t)(1u << (key & 7));
        colours++;
      }
    }
    printf("screenshot: %dx%d, %llu lit pixels, %llu colours\n", w, h, (unsigned long long)lit, (unsigned long long)colours);
  }
  free(rgba);
  return ok ? 0 : fail("screenshot", path);
}

static int run_fixed_time(app *a)
{
  const int64_t target = (int64_t)(a->opt.fixed_time * SONG_RATE);
  wz3_result res = wz3_rt_precalc(a->rt);
  if (res != WZ3_OK) return fail("precalc", wz3_result_string(res));
  if (a->opt.seek_check) return seek_check(a, target);
  res = pscene_plan(&a->scene);
  if (res == WZ3_OK) res = pscene_eval_textures(&a->scene, NULL, NULL, NULL);
  if (res != WZ3_OK) return fail("textures", wz3_result_string(res));
  if (warm_scene(a, 0) < 0) return 1;
  if (a->opt.n_times) { /* a list of times in one run (the golden set): each continues from the one before */
    int64_t at = 0;
    for (int k = 0; k < a->opt.n_times; k++) {
      char path[1100];
      const int64_t t = (int64_t)(a->opt.times[k] * SONG_RATE);
      snprintf(path, sizeof path, "%s/frame_%07lld.ppm", a->opt.screenshot_dir, (long long)(a->opt.times[k] * 1000.0 + 0.5));
      if (render_time(a, t, &at) || capture_picture(a, path)) return 1;
    }
    return 0;
  }
  int64_t sample = 0;
  if (render_time(a, target, &sample)) return 1;
  return a->opt.screenshot_path ? capture_picture(a, a->opt.screenshot_path) : 0;
}

/* ---- the live loop ---- */

static SDL_Keycode script_keycode(pkey key)
{
  switch (key) {
  case PKEY_SPACE: return SDLK_SPACE;
  case PKEY_LEFT: return SDLK_LEFT;
  case PKEY_RIGHT: return SDLK_RIGHT;
  case PKEY_F11: return SDLK_F11;
  case PKEY_FULLSCREEN: return SDLK_F;
  case PKEY_CREDITS: return SDLK_C;
  case PKEY_STATS: return SDLK_F10;
  case PKEY_RESIZE: return SDLK_UNKNOWN; /* has no key: the window is resized by the script itself */
  }
  return SDLK_UNKNOWN;
}

static void toggle_fullscreen(app *a)
{
  const int on = !(SDL_GetWindowFlags(a->window) & SDL_WINDOW_FULLSCREEN);
  if (!SDL_SetWindowFullscreen(a->window, on))
    fprintf(stderr, "debris: fullscreen is not available here: %s\n", SDL_GetError());
  else
    SDL_SyncWindow(a->window);
}

/* The window steps through sizes of other shapes (smaller, taller, wider, back) so that the 2:1 picture is letterboxed and pillarboxed. */
static void cycle_window_size(app *a)
{
  if (SDL_GetWindowFlags(a->window) & SDL_WINDOW_FULLSCREEN) return;
  const int W = a->opt.width, H = a->opt.height;
  const int sizes[4][2] = { { W * 3 / 4, H * 3 / 4 }, { H, H * 13 / 10 }, { W * 3 / 2, H * 3 / 4 }, { W, H } };
  const int step = a->resize_step++ % 4;
  if (!SDL_SetWindowSize(a->window, clamp_int(sizes[step][0], 64, 16384), clamp_int(sizes[step][1], 64, 16384))) {
    fprintf(stderr, "debris: the window cannot be resized: %s\n", SDL_GetError());
    return;
  }
  SDL_SyncWindow(a->window);
}

static int handle_key(app *a, SDL_Keycode key)
{
  switch (key) {
  case SDLK_ESCAPE: return 1;
  case SDLK_SPACE: set_paused(a, !a->paused); break;
  case SDLK_LEFT: begin_seek(a, -SEEK_STEP_SECONDS); break;
  case SDLK_RIGHT: begin_seek(a, SEEK_STEP_SECONDS); break;
  case SDLK_F: toggle_fullscreen(a); break;
  case SDLK_C: a->show_credits = !a->show_credits; break;
  case SDLK_F10:
    a->show_stats = !a->show_stats;
    ptext_free(&a->stats_image);
    break;
  case SDLK_F11: print_stats(a); break;
  default: break;
  }
  return 0;
}

static int handle_script_key(app *a, pkey key)
{
  if (key == PKEY_RESIZE) {
    cycle_window_size(a);
    return 0;
  }
  return handle_key(a, script_keycode(key));
}

static void cursor_activity(app *a, uint64_t now)
{
  a->cursor_active_ns = now;
  if (a->cursor_hidden) {
    SDL_ShowCursor();
    a->cursor_hidden = 0;
  }
}

/* Only while the pointer is over the window: SDL ignores the hidden state elsewhere, and the player must not hide a pointer it cannot show again. */
static void cursor_idle(app *a, uint64_t now)
{
  if (a->cursor_hidden || now - a->cursor_active_ns < CURSOR_IDLE_NS) return;
  if (!(SDL_GetWindowFlags(a->window) & SDL_WINDOW_MOUSE_FOCUS)) return;
  SDL_HideCursor();
  a->cursor_hidden = 1;
}

static int live_begin(app *a)
{
  a->live_start_ns = SDL_GetTicksNS();
  a->cursor_active_ns = a->live_start_ns;
  a->phase = PHASE_LOADING;
  if (a->opt.audio) {
    if (paudio_play_loader(&a->audio, a->loader_bytes, a->loader_size) != V2_OK) fprintf(stderr, "debris: loader song failed, continuing silent\n");
  }
  if (a->opt.single_thread) return 0; /* the loop loads in slices */
  a->worker = SDL_CreateThread(precalc_main, "precalc", a);
  if (!a->worker) return fail("worker thread", SDL_GetError());
  return 0;
}

static int live_stop(app *a, int rc)
{
  a->live_rc = rc;
  return 0;
}

/* One iteration of the live loop: events, the load or the clock, the picture. Returns 1 to go on and 0 when the loop is over (live_rc). */
static int live_frame(app *a)
{
  int quit = 0;
  const uint64_t start_ns = a->live_start_ns;
  SDL_Event e;
  while (SDL_PollEvent(&e)) {
    if (e.type == SDL_EVENT_QUIT)
      quit = 1;
    else if (e.type == SDL_EVENT_KEY_DOWN && !e.key.repeat)
      quit |= handle_key(a, e.key.key);
    else if (e.type == SDL_EVENT_MOUSE_MOTION || e.type == SDL_EVENT_MOUSE_BUTTON_DOWN || e.type == SDL_EVENT_MOUSE_BUTTON_UP ||
             e.type == SDL_EVENT_MOUSE_WHEEL || e.type == SDL_EVENT_WINDOW_MOUSE_LEAVE || e.type == SDL_EVENT_WINDOW_FOCUS_LOST)
      cursor_activity(a, SDL_GetTicksNS());
  }
  cursor_idle(a, SDL_GetTicksNS());
  a->minimized = (SDL_GetWindowFlags(a->window) & SDL_WINDOW_MINIMIZED) != 0; /* nothing to draw into; the show goes on */
  if (!fit_renderer(a, 0)) return live_stop(a, 1);                            /* the drawable changed (resize, fullscreen, another display's pixel density) */
  const uint64_t now = SDL_GetTicksNS();
  if (a->phase != PHASE_LOADING) { /* key scripts and --play-for count from the party start, so the load time cannot matter */
    const double played = (double)(now - a->play_start_ns) / 1e9;
    while (a->next_script < a->opt.n_script && played >= a->opt.script[a->next_script].time) quit |= handle_script_key(a, a->opt.script[a->next_script++].key);
    if (a->opt.play_for >= 0 && played >= a->opt.play_for) quit = 1;
  }
  if (a->opt.exit_after >= 0 && (double)(now - start_ns) / 1e9 >= a->opt.exit_after) quit = 1;

  if (a->phase == PHASE_LOADING) {
    if (a->opt.single_thread) {
      if (!a->worker_joined && precalc_run(a, LOAD_SLICE_NS)) {
        if (a->precalc_result != WZ3_OK) return live_stop(a, fail("precalc", wz3_result_string(a->precalc_result)));
        a->worker_joined = 1;
      }
    } else if (a->worker && SDL_GetAtomicInt(&a->worker_done)) {
      SDL_WaitThread(a->worker, NULL);
      a->worker = NULL;
      if (a->precalc_result != WZ3_OK) return live_stop(a, fail("precalc", wz3_result_string(a->precalc_result)));
      a->worker_joined = 1;
    }
    if (a->worker_joined) { /* the GL thread's share: Render operators, textures and geometry into GL, in slices */
      const int warm = warm_scene(a, WARM_SLICE_NS);
      if (warm < 0) return live_stop(a, 1);
      if (warm > 0) {
        start_party(a);
        return 1;
      }
    }
    if (!a->minimized) {
      glr_present_progress(a->renderer, ploader_stages((uint32_t)SDL_GetAtomicInt(&a->progress_done), (uint32_t)SDL_GetAtomicInt(&a->progress_total),
                                                       (uint32_t)SDL_GetAtomicInt(&a->texture_done), (uint32_t)SDL_GetAtomicInt(&a->texture_total),
                                                       a->warm_done, a->warm_total));
      draw_overlays(a, 1); /* the credits stay on the loader screen */
    }
  } else if (a->phase == PHASE_SEEKING) {
    while (SDL_GetTicksNS() - now < REPLAY_SLICE_NS && a->replay_sample < a->replay_target) {
      a->replay_sample = pseek_next_sample(a->replay_sample, a->replay_target, SONG_RATE, REPLAY_FPS);
      const wz3_result res = runtime_step(a, a->replay_sample);
      if (res != WZ3_OK) return live_stop(a, fail("seek replay", wz3_result_string(res)));
    }
    if (a->replay_sample >= a->replay_target) finish_seek(a);
    if (!a->minimized) {
      glr_present_loading(a->renderer, (double)(now - start_ns) / 1e9 * 0.7);
      draw_overlays(a, 0);
    }
  } else {
    if (!a->paused) {
      /* what the device has played, and the time it was asked: read the time after the audio, so that the loop's own delay (events, a slow
           frame under a sanitizer) can only make the clock lag the wall time, never lead it */
      const int64_t played_frames = audio_played_frames(a);
      const uint64_t sampled = SDL_GetTicksNS();
      pclock_report(&a->clock, played_frames, sampled);
      int64_t clock = pclock_now(&a->clock, sampled);
      if (a->duration_samples > 0 && clock >= a->duration_samples) {
        clock = a->duration_samples;
        quit = 1;
      }
      if (a->opt.self_check) {
        const int64_t wall = (int64_t)((double)(sampled - a->t0_ns) * SONG_RATE / 1e9);
        if (clock < a->last_clock && a->violations++ < 5)
          fprintf(stderr, "self-check: clock went backwards %lld -> %lld\n", (long long)a->last_clock, (long long)clock);
        /* SDL3 does not expose the output latency of a real device (its buffers beyond the one it reports), so the
             bound there is one device buffer. The dummy driver consumes in wall time but in timer steps; when a loop iteration is slow
             (a sanitizer, a loaded machine) the step it just took is noticed late, so it gets 5 ms: still far below the 100 ms a clock
             that extrapolates wrongly would lead by. */
        const int64_t slack = SONG_RATE / 1000 + (a->dummy_audio ? 4 * SONG_RATE / 1000 : (int64_t)a->device_latency_frames);
        if (clock > wall + slack && a->violations++ < 5)
          fprintf(stderr, "self-check: clock leads wall time by %lld frames (allowed %lld)\n", (long long)(clock - wall), (long long)slack);
      }
      a->last_clock = clock;
      if (clock > a->sample) {
        const wz3_result res = runtime_frame(a, clock);
        if (res != WZ3_OK) return live_stop(a, fail("frame", wz3_result_string(res)));
        a->sample = clock;
      }
      if (a->sample - a->snaps.last_sample >= (int64_t)SNAPSHOT_INTERVAL_SECONDS * SONG_RATE) snapshot_take(a, a->sample);
    }
    if (!a->minimized) {
      render_frame(a);
      draw_overlays(a, 0);
    }
  }
  if (a->minimized)
    SDL_Delay(10);
  else {
    SDL_GL_SwapWindow(a->window);
    poll_gl_errors(a);
  }
  a->frame_ms[a->frames % FRAME_RING] = (float)((double)(SDL_GetTicksNS() - (a->last_frame_ns ? a->last_frame_ns : now)) / 1e6);
  a->last_frame_ns = SDL_GetTicksNS();
  a->frames++;
  if (a->opt.stats && a->last_frame_ns - a->stats_print_ns >= 1000000000ull && a->phase != PHASE_LOADING) {
    a->stats_print_ns = a->last_frame_ns;
    print_stats(a);
  }
  return quit ? live_stop(a, 0) : 1;
}

/* What follows the live loop: stops a load that is still running and reports the self-check. Returns the exit status. */
static int live_finish(app *a)
{
  if (a->worker) { /* quit while loading: the worker stops at its next operator */
    SDL_SetAtomicInt(&a->cancel_load, 1);
    SDL_WaitThread(a->worker, NULL);
    a->worker = NULL;
    if (a->opt.self_check)
      printf("load: %s at %d of %d operators\n", a->precalc_result == WZ3_ERR_CANCELLED ? "cancelled" : "finished", SDL_GetAtomicInt(&a->progress_done),
             SDL_GetAtomicInt(&a->progress_total));
  } else if (a->opt.single_thread && a->phase == PHASE_LOADING && a->opt.self_check) {
    printf("load: %s at %d of %d operators\n", a->load_stage < 3 ? "cancelled" : "finished", SDL_GetAtomicInt(&a->progress_done),
           SDL_GetAtomicInt(&a->progress_total));
  }
  if (a->opt.self_check) {
    const int64_t wall = (int64_t)((double)(SDL_GetTicksNS() - a->t0_ns) * SONG_RATE / 1e9);
    if (a->phase == PHASE_PLAYING && !a->paused && a->sample < wall / 2 && wall > SONG_RATE) {
      a->violations++;
      fprintf(stderr, "self-check: the clock stalled (%lld of %lld frames)\n", (long long)a->sample, (long long)wall);
    }
    if (a->opt.expect_position >= 0 && fabs((double)a->sample / SONG_RATE - a->opt.expect_position) > 0.5) {
      a->violations++;
      fprintf(stderr, "self-check: final position %.2f s, expected %.2f s\n", (double)a->sample / SONG_RATE, a->opt.expect_position);
    }
    printf("self-check: %d violations, position %.2f s after %llu frames, %d resizes\n", a->violations, (double)a->sample / SONG_RATE,
           (unsigned long long)a->frames, a->resizes_seen);
  }
  return a->violations ? 1 : 0;
}

#if defined(__EMSCRIPTEN__)
/* The browser drives the loop (requestAnimationFrame); when the demo is over the last picture stays and the page is told. */
static void web_frame(void *arg)
{
  app *a = (app *)arg;
  if (live_frame(a)) return;
  emscripten_cancel_main_loop();
  if (!a->live_rc) live_finish(a);
  EM_ASM(
    {
      if (Module.onEnded) Module.onEnded($0);
    },
    a->live_rc);
}
#endif

static int run_live(app *a)
{
  if (live_begin(a)) return 1;
#if defined(__EMSCRIPTEN__)
  emscripten_set_main_loop_arg(web_frame, a, 0, 1); /* does not return */
  return 0;
#else
  while (live_frame(a)) {}
  return a->live_rc ? a->live_rc : live_finish(a);
#endif
}

/* ---- the soak (--soak N): the whole demo N times in deterministic steps, with seeks, resizes and screens, GL objects and errors counted ---- */

static double rss_mib(void)
{
#if defined(__linux__)
  FILE *f = fopen("/proc/self/statm", "r");
  if (!f) return 0.0;
  unsigned long pages = 0, resident = 0;
  const int ok = fscanf(f, "%lu %lu", &pages, &resident) == 2;
  fclose(f);
  const long page = sysconf(_SC_PAGESIZE);
  return ok && page > 0 ? (double)resident * (double)page / (1024.0 * 1024.0) : 0.0;
#else
  return 0.0;
#endif
}

/* Seek synchronously: restore the snapshot before a backward target, replay the 30 fps grid up to it. */
static int soak_seek(app *a, int64_t target)
{
  if (target < 0) target = 0;
  if (target > a->duration_samples) target = a->duration_samples;
  int64_t from = a->sample;
  if (target < a->sample) {
    const psnapshot *snap = psnapshots_before(&a->snaps, target);
    if (!snap || wz3_rt_restore(a->rt, snap->data, snap->size) != WZ3_OK) return fail("soak seek", "restore failed");
    from = snap->sample;
  }
  while (from < target) {
    from = pseek_next_sample(from, target, SONG_RATE, REPLAY_FPS);
    const wz3_result res = runtime_step(a, from);
    if (res != WZ3_OK) return fail("soak seek", wz3_result_string(res));
  }
  a->sample = target;
  return 0;
}

#define SOAK_BUCKETS 800 /* a frame time histogram: quarter milliseconds up to 200 ms */

static double histogram_percentile(const uint32_t *hist, uint64_t total, double fraction)
{
  if (!total) return 0.0;
  const uint64_t want = (uint64_t)((double)total * fraction);
  uint64_t seen = 0;
  for (int i = 0; i < SOAK_BUCKETS; i++) {
    seen += hist[i];
    if (seen > want) return (double)i / 4.0;
  }
  return (double)SOAK_BUCKETS / 4.0;
}

static int run_soak(app *a)
{
  wz3_result res = wz3_rt_precalc(a->rt);
  if (res == WZ3_OK) res = pscene_plan(&a->scene);
  if (res == WZ3_OK) res = pscene_eval_textures(&a->scene, NULL, NULL, NULL);
  if (res != WZ3_OK) return fail("load", wz3_result_string(res));
  if (warm_scene(a, 0) < 0) return 1;
  a->duration_samples = (int64_t)(document_seconds(a->kx) * SONG_RATE);
  if (a->duration_samples < SONG_RATE) return fail("soak", "the document has no timeline");
  a->show_stats = 1; /* the screens of the player are part of what is soaked */
  a->sample = 0;
  snapshot_take(a, 0);
  if (runtime_frame(a, 0) != WZ3_OK) return fail("soak", "the first frame failed");
  a->phase = PHASE_PLAYING;
  const uint64_t wall0 = SDL_GetTicksNS();
  const double baseline_rss = rss_mib();
  soak_loop first;
  memset(&first, 0, sizeof first);
  uint32_t *hist = (uint32_t *)calloc(SOAK_BUCKETS, sizeof *hist);
  if (!hist) return fail("soak", "out of memory");
  int loop = 0, quit = 0, limit_hit = 0, seeked_back = 0, seeked_forward = 0, resizes = 0, equal_frames = 1;
  uint64_t loop_frames = 0, hist_total = 0;
  double loop_max_ms = 0.0;
  uint64_t last_ns = SDL_GetTicksNS();
  while (!quit && loop < a->opt.soak_loops) {
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
      if (e.type == SDL_EVENT_QUIT || (e.type == SDL_EVENT_KEY_DOWN && e.key.key == SDLK_ESCAPE)) quit = 1;
    }
    if (!fit_renderer(a, 0)) return 1;
    const uint64_t now = SDL_GetTicksNS();
    if (a->opt.soak_limit > 0 && (double)(now - wall0) / 1e9 >= a->opt.soak_limit) {
      limit_hit = 1;
      break;
    }
    const int64_t d = a->duration_samples;
    if (!seeked_back && a->sample >= d / 4) {
      seeked_back = 1;
      if (soak_seek(a, a->sample - 10 * (int64_t)SONG_RATE)) return 1;
    }
    if (resizes < 4 && a->sample >= d / 2 + (int64_t)resizes * SONG_RATE) {
      resizes++;
      cycle_window_size(a);
    }
    if (!seeked_forward && a->sample >= d * 3 / 4) {
      seeked_forward = 1;
      if (soak_seek(a, a->sample + 10 * (int64_t)SONG_RATE)) return 1;
    }
    a->show_credits = ((a->sample / (60 * (int64_t)SONG_RATE)) & 1) != 0; /* the credits come and go every minute of the demo */
    if (a->sample < d) {
      const int64_t next = pseek_next_sample(a->sample, d, SONG_RATE, REPLAY_FPS);
      res = runtime_frame(a, next);
      if (res != WZ3_OK) return fail("frame", wz3_result_string(res));
      a->sample = next;
      if (a->sample - a->snaps.last_sample >= (int64_t)SNAPSHOT_INTERVAL_SECONDS * SONG_RATE) snapshot_take(a, a->sample);
    }
    if (!a->minimized) {
      render_frame(a);
      draw_overlays(a, 0);
      SDL_GL_SwapWindow(a->window);
      poll_gl_errors(a);
    }
    const uint64_t after = SDL_GetTicksNS();
    const double ms = (double)(after - last_ns) / 1e6;
    last_ns = after;
    a->frame_ms[a->frames % FRAME_RING] = (float)ms;
    a->frames++;
    loop_frames++;
    hist[(int)(ms * 4.0) < SOAK_BUCKETS ? (int)(ms * 4.0) : SOAK_BUCKETS - 1]++;
    hist_total++;
    if (ms > loop_max_ms) loop_max_ms = ms;
    if (a->opt.stats && after - a->stats_print_ns >= 1000000000ull) {
      a->stats_print_ns = after;
      print_stats(a);
    }

    if (a->sample >= d) { /* the end of a loop: account for it, then back to the start */
      loop++;
      soak_loop now_loop;
      memset(&now_loop, 0, sizeof now_loop);
      now_loop.frames = loop_frames;
      now_loop.p50 = histogram_percentile(hist, hist_total, 0.5);
      now_loop.p95 = histogram_percentile(hist, hist_total, 0.95);
      now_loop.max_ms = loop_max_ms;
      now_loop.rss_mib = rss_mib();
      now_loop.gl = g_glcheck;
      now_loop.objects = pglcheck_total(&g_glcheck);
      char objects[256];
      pglcheck_format(&g_glcheck, objects, sizeof objects);
      printf("soak loop %d: %llu frames, frame ms p50 %.2f p95 %.2f max %.1f, RSS %.0f MiB, GL objects %lld (%s)\n", loop, (unsigned long long)loop_frames,
             now_loop.p50, now_loop.p95, now_loop.max_ms, now_loop.rss_mib, (long long)now_loop.objects, objects);
      fflush(stdout);
      if (loop == 1)
        first = now_loop;
      else {
        if (!pglcheck_same_objects(&first.gl, &now_loop.gl)) {
          a->violations++;
          char before[256];
          pglcheck_format(&first.gl, before, sizeof before);
          fprintf(stderr, "soak: GL objects after loop %d differ from loop 1:\n  loop 1: %s\n  loop %d: %s\n", loop, before, loop, objects);
        }
        if (now_loop.rss_mib > 0.0 && now_loop.rss_mib > first.rss_mib + 128.0) {
          a->violations++;
          fprintf(stderr, "soak: RSS grew from %.0f MiB (loop 1) to %.0f MiB (loop %d)\n", first.rss_mib, now_loop.rss_mib, loop);
        }
        if (now_loop.frames != first.frames) equal_frames = 0;
      }
      if (loop < a->opt.soak_loops) {
        const psnapshot *start = psnapshots_before(&a->snaps, 0);
        if (!start || wz3_rt_restore(a->rt, start->data, start->size) != WZ3_OK) return fail("soak", "cannot restart the demo");
        a->sample = 0;
      }
      seeked_back = seeked_forward = 0;
      resizes = 0;
      loop_frames = 0;
      loop_max_ms = 0.0;
      memset(hist, 0, SOAK_BUCKETS * sizeof *hist);
      hist_total = 0;
    }
  }
  free(hist);
  if (!equal_frames) {
    a->violations++;
    fprintf(stderr, "soak: the loops did not draw the same number of frames\n");
  }
  if (g_glcheck.errors) {
    a->violations++;
    fprintf(stderr, "soak: %llu GL errors, the first was %s at poll %llu\n", (unsigned long long)g_glcheck.errors, pglcheck_error_name(g_glcheck.first_error),
            (unsigned long long)g_glcheck.first_error_poll);
  }
  if (g_glcheck.over_deleted) {
    a->violations++;
    fprintf(stderr, "soak: %llu GL objects deleted that were never created\n", (unsigned long long)g_glcheck.over_deleted);
  }
  printf("soak: %d of %d loops in %.1f s%s, %llu frames, %.0f MiB RSS at the end (%.0f MiB before the first frame), %llu GL errors, %d violations\n", loop,
         a->opt.soak_loops, (double)(SDL_GetTicksNS() - wall0) / 1e9,
         limit_hit ? " (stopped by --soak-limit)"
         : quit    ? " (quit)"
                   : "",
         (unsigned long long)a->frames, rss_mib(), baseline_rss, (unsigned long long)g_glcheck.errors, a->violations);
  if (loop < 2) printf("soak: fewer than two complete loops: the objects-stop-growing check needs two\n");
  return a->violations ? 1 : 0;
}

/* ---- main ---- */

int main(int argc, char **argv)
{
  app *a = (app *)calloc(1, sizeof *a);
  if (!a) return fail("out of memory", NULL);
  int rc = 1;
  char error[256];
  if (!poptions_parse(argc, argv, &a->opt, error, sizeof error)) {
    fprintf(stderr,
            "debris: %s\n"
            "usage: %s [--kx PATH] [--loader PATH] [--size WxH] [--fullscreen] [--no-vsync] [--no-audio]\n"
            "          [--texture-quality high|normal|low|-3..0]\n"
            "          [--time SECONDS [--screenshot PATH.ppm] [--overlay credits,stats] [--seek-check]] [--exit-after SECONDS] [--play-for SECONDS]\n"
            "          [--self-check [--script T:KEY,...] [--expect-position SECONDS]]\n"
            "          [--stats] [--gl-check] [--soak LOOPS [--soak-limit SECONDS]] [--licenses] [--single-thread]\n"
            "          [--times T1,T2,... --screenshot-dir DIR]\n",
            error, argv[0]);
    free(a);
    return 2;
  }
  if (a->opt.licences) { /* nothing to load: the credits and the notices of what the player is built from */
    const int ok = pcredits_print(stdout, player_licenses, player_n_licenses);
    free(a);
    return ok ? 0 : 1;
  }
  if (a->opt.soak_loops) a->opt.gl_check = 1; /* the soak is about objects and errors */
#if defined(__EMSCRIPTEN__)
  a->opt.single_thread = 1;                   /* a page cannot have a worker thread without COOP/COEP headers, which static hosting does not send */
#endif
  paudio_init(&a->audio, SONG_RATE);
  if (!v2_fp_env_ok()) {
    rc = fail("floating point environment", "subnormals are flushed (fast-math build?)");
    goto done;
  }
  if (!load_asset(a->opt.kx_path, "debris_party.kx", &a->kx_bytes, &a->kx_size)) {
    rc = fail("cannot read the KX document", a->opt.kx_path ? a->opt.kx_path : "assets/debris_party.kx");
    goto done;
  }
  if (a->opt.audio && !load_asset(a->opt.loader_path, "debris_loader.v2m", &a->loader_bytes, &a->loader_size)) {
    rc = fail("cannot read the loader song", a->opt.loader_path ? a->opt.loader_path : "assets/debris_loader.v2m");
    goto done;
  }
  size_t error_offset = 0;
  wz3_result res = wz3_kx_parse(&a->kx, a->kx_bytes, a->kx_size, &error_offset);
  if (res != WZ3_OK) {
    rc = fail("KX document", wz3_result_string(res));
    goto done;
  }
  if (!SDL_Init(SDL_INIT_VIDEO | (a->opt.audio ? SDL_INIT_AUDIO : 0))) {
    rc = fail("SDL", SDL_GetError());
    goto done;
  }
  if (create_window(a)) goto done;
  if (runtime_create(a)) goto done;
  if (a->opt.fixed_time >= 0) {
    rc = run_fixed_time(a);
    goto done;
  }
  if (a->opt.soak_loops) {
    rc = run_soak(a);
    goto done;
  }
  if (a->opt.audio && !audio_open(a)) {
    fprintf(stderr, "debris: no audio device (%s), continuing silent\n", SDL_GetError());
    a->opt.audio = 0;
    a->stream = NULL;
  }
  rc = run_live(a);

done:
  if (a->worker) { /* the worker uses rt: stop it and join before destroying rt */
    SDL_SetAtomicInt(&a->cancel_load, 1);
    SDL_WaitThread(a->worker, NULL);
  }
  if (a->stream) SDL_DestroyAudioStream(a->stream); /* stops the callback before paudio goes away */
  paudio_destroy(&a->audio);
  psnapshots_clear(&a->snaps);
  ptext_free(&a->credits_title);
  ptext_free(&a->credits_subtitle);
  ptext_free(&a->credits_roles);
  ptext_free(&a->stats_image);
  pscene_destroy(&a->scene);
  wz3_rt_destroy(a->rt);
  wz3_kx_destroy(a->kx);
  glr_destroy(a->renderer);
  if (a->opt.gl_check && a->renderer) { /* what glr asked of the driver must balance: nothing is left once the renderer is gone */
    char objects[256];
    pglcheck_format(&g_glcheck, objects, sizeof objects);
    printf("gl-check: %llu GL errors in %llu polls; after glr_destroy: %lld live objects (%s)\n", (unsigned long long)g_glcheck.errors,
           (unsigned long long)g_glcheck.polls, (long long)pglcheck_total(&g_glcheck), objects);
    if (g_glcheck.errors || g_glcheck.over_deleted || pglcheck_total(&g_glcheck) != 0) {
      fprintf(stderr, "debris: gl-check failed: %llu errors, %llu over-deleted, %lld objects leaked\n", (unsigned long long)g_glcheck.errors,
              (unsigned long long)g_glcheck.over_deleted, (long long)pglcheck_total(&g_glcheck));
      rc = 1;
    }
  }
  if (a->gl) SDL_GL_DestroyContext(a->gl);
  if (a->window) SDL_DestroyWindow(a->window);
  SDL_Quit();
  free(a->kx_bytes);
  free(a->loader_bytes);
  free(a);
  return rc;
}
