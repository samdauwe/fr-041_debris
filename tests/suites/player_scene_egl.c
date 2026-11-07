/* Real-driver check of the player's resident scene: the pictures of the player's loading path against the lazy reference.
 *
 *   test_player_scene_egl <party.kx> [screenshot dir]
 *
 * Headless OpenGL ES 3 through EGL (Mesa llvmpipe without a GPU), texture offset -2 and the empty-coverage text callback of the frame
 * tests. Reference: the host of the JavaScript-derived frame tests (frame_world.h), which evaluates every bitmap on first use and keeps
 * it. Subject: pscene as the player runs it - the textures evaluated up front (pscene_eval_textures), the Render operators, textures
 * and geometry uploaded by pscene_warm_step, every bitmap given back after its upload. The frames of both must be identical, byte for
 * byte, and leave the GL error flag clear; the warm renderer draws its first frame without a texture or buffer upload.
 * Exits 77 (skipped) when no context can be created.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>

#define WZ3_IMPLEMENTATION
#include <debris/core/wz3.h>
#define GLR_IMPLEMENTATION
#include <debris/render/glr.h>
#include "support/gate_common.h"
#include "support/frame_world.h"
#define PLAYER_LOAD_IMPLEMENTATION
#include <debris/player/player_load.h>
#define PLAYER_SCENE_IMPLEMENTATION
#include <debris/player/player_scene.h>

static void *load_gl(const char *name) { return (void *)eglGetProcAddress(name); }

typedef struct env {
  EGLDisplay display;
  EGLSurface surface;
  EGLContext context;
} env;

static int make_context(env *e, int width, int height)
{
  e->display = EGL_NO_DISPLAY;
  PFNEGLGETPLATFORMDISPLAYEXTPROC get_platform = (PFNEGLGETPLATFORMDISPLAYEXTPROC)eglGetProcAddress("eglGetPlatformDisplayEXT");
  if (get_platform) e->display = get_platform(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, NULL);
  if (e->display == EGL_NO_DISPLAY) e->display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
  EGLint major, minor;
  if (e->display == EGL_NO_DISPLAY || !eglInitialize(e->display, &major, &minor)) return 0;
  if (!eglBindAPI(EGL_OPENGL_ES_API)) return 0;
  const EGLint config_attribs[] = {
    EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT, EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
    EGL_NONE
  };
  EGLConfig config;
  EGLint n = 0;
  if (!eglChooseConfig(e->display, config_attribs, &config, 1, &n) || n < 1) return 0;
  const EGLint surface_attribs[] = { EGL_WIDTH, width, EGL_HEIGHT, height, EGL_NONE };
  e->surface = eglCreatePbufferSurface(e->display, config, surface_attribs);
  const EGLint context_attribs[] = { EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE };
  e->context = eglCreateContext(e->display, config, EGL_NO_CONTEXT, context_attribs);
  if (e->surface == EGL_NO_SURFACE || e->context == EGL_NO_CONTEXT) return 0;
  return eglMakeCurrent(e->display, e->surface, e->surface, e->context) == EGL_TRUE;
}

enum { W = 640, H = 360 };
static const long samples[] = { 0, 3037500, 6075000, 15187500, 17717175 };
#define N_SAMPLES (sizeof samples / sizeof samples[0])

static void write_ppm(const char *dir, const char *tag, long sample, const uint8_t *rgba)
{
  char path[1024];
  snprintf(path, sizeof path, "%s/%s_%ld.ppm", dir, tag, sample);
  FILE *f = fopen(path, "wb");
  if (!f) return;
  fprintf(f, "P6\n%d %d\n255\n", W, H);
  for (int i = 0; i < W * H; i++) fwrite(rgba + (size_t)i * 4, 1, 3, f);
  fclose(f);
}

static glr *make_renderer(void)
{
  glr_options options;
  memset(&options, 0, sizeof options);
  char error[1024] = "";
  options.error = error;
  options.error_size = sizeof error;
  glr_result res;
  glr *r = glr_create(load_gl, &options, &res);
  CHECK(r != NULL && res == GLR_OK, "glr_create: %s", error);
  return r;
}

/* the reference frames: lazy evaluation, bitmaps kept */
static int reference_frames(const wz3_kx *kx, uint8_t *images)
{
  world w;
  if (!world_open(&w, kx, 1)) return 0;
  glr_host host;
  memset(&host, 0, sizeof host);
  host.user = &w;
  host.rt = w.rt;
  host.n_ops = kx->n_ops;
  host.geometry = host_geometry;
  host.shadow_data = host_shadow;
  host.bitmap = host_bitmap;
  w.host = &host;
  glr *r = make_renderer();
  if (!r) return 0;
  w.renderer = r;
  for (uint32_t op = 0; op < kx->n_ops; op++) {
    if (kx->ops[op].class_id != 0x3f) continue;
    const wz3_bitmap *bitmap = NULL;
    CHECK(wz3_graph_eval_bitmap(w.graph, op, &bitmap) == WZ3_OK && bitmap, "reference Render operator %u", op);
  }
  for (size_t k = 0; k < N_SAMPLES; k++) {
    CHECK(wz3_rt_frame_at_sample(w.rt, (int32_t)samples[k], 44100) == WZ3_OK, "reference frame %ld", samples[k]);
    CHECK(glr_resize(r, W, H, 2.0) == GLR_OK && glr_render_frame(r, &host) == GLR_OK, "reference render %ld", samples[k]);
    CHECK(glr_read_pixels(r, images + k * (size_t)W * H * 4, W, H), "reference pixels");
  }
  CHECK(glGetError() == GL_NO_ERROR, "GL error in the reference run");
  w.renderer = NULL;
  glr_destroy(r);
  world_free(&w, kx->n_ops);
  return 1;
}

int suite_player_scene_egl(int argc, char **argv)
{
  if (argc < 2) {
    printf("usage: test_player_scene_egl <party.kx> [screenshot dir]\n");
    return 2;
  }
  env e;
  if (!make_context(&e, W, H)) {
    printf("SKIP: no EGL / OpenGL ES 3 context\n");
    return 77;
  }
  printf("GL: %s / %s\n", (const char *)glGetString(GL_RENDERER), (const char *)glGetString(GL_VERSION));
  size_t size = 0;
  uint8_t *bytes = NULL;
  FILE *kf = fopen(argv[1], "rb");
  if (kf && fseek(kf, 0, SEEK_END) == 0) {
    const long n = ftell(kf);
    rewind(kf);
    bytes = n > 0 ? (uint8_t *)malloc((size_t)n) : NULL;
    if (bytes && fread(bytes, 1, (size_t)n, kf) == (size_t)n) size = (size_t)n;
  }
  if (kf) fclose(kf);
  if (!size) {
    printf("cannot read %s\n", argv[1]);
    return 2;
  }
  wz3_kx *kx = NULL;
  size_t at;
  CHECK(wz3_kx_parse(&kx, bytes, size, &at) == WZ3_OK, "parse");
  if (!kx) return 1;
  uint8_t *reference = (uint8_t *)malloc(N_SAMPLES * (size_t)W * H * 4), *got = (uint8_t *)malloc((size_t)W * H * 4);
  if (!reference || !got) return 1;
  if (!reference_frames(kx, reference)) return 1;
  printf("player scene egl: %zu reference frames drawn\n", N_SAMPLES);

  pscene ps;
  pscene_options o;
  memset(&o, 0, sizeof o);
  o.texture_offset = -2;
  o.text = text_empty;
  o.bitmap_render = 1;
  o.release_uploaded = 1;
  CHECK(pscene_create(&ps, kx, &o) == WZ3_OK, "pscene_create");
  wz3_rt_options ro;
  memset(&ro, 0, sizeof ro);
  ro.generic_exec = 1;
  ro.release_consumed = 1;
  ro.frame_jobs = 1;
  ro.host.user = &ps;
  ro.host.mesh = pscene_mesh;
  ro.host.minmesh = pscene_minmesh;
  ro.host.bitmap = pscene_bitmap;
  wz3_result res;
  wz3_rt *rt = wz3_rt_create(kx, &ro, &res);
  CHECK(rt != NULL, "runtime");
  if (!rt) return 1;
  pscene_attach(&ps, rt);
  wz3_rt_set_aspect(rt, 2.0);
  CHECK(wz3_rt_precalc(rt) == WZ3_OK, "precalc");
  CHECK(pscene_plan(&ps) == WZ3_OK && pscene_eval_textures(&ps, NULL, NULL, NULL) == WZ3_OK, "plan and texture evaluation");
  glr *r = make_renderer();
  if (!r) return 1;
  pscene_set_renderer(&ps, r);
  uint32_t done = 0, total = 0;
  do {
    CHECK(pscene_warm_step(&ps, &done, &total) == GLR_OK, "warm step %u", done);
    if (glGetError() != GL_NO_ERROR) {
      CHECK(0, "GL error during warm-up item %u", done);
      break;
    }
  } while (done < total);
  CHECK(ps.released == ps.n_textures && ps.bitmap_failures == 0, "%u of %u bitmaps given back, %u failed", ps.released, ps.n_textures, ps.bitmap_failures);
  printf("player scene egl: %u items warm (%u textures, %u geometries)\n", total, ps.n_textures, total - ps.n_textures - 3);
  for (size_t k = 0; k < N_SAMPLES; k++) {
    CHECK(wz3_rt_frame_at_sample(rt, (int32_t)samples[k], 44100) == WZ3_OK, "frame %ld", samples[k]);
    const uint64_t requests = ps.bitmap_requests;
    CHECK(glr_resize(r, W, H, 2.0) == GLR_OK && glr_render_frame(r, pscene_glr_host(&ps)) == GLR_OK, "render %ld: %s", samples[k], glr_last_error(r));
    CHECK(ps.bitmap_requests == requests, "frame %ld asked for %llu bitmaps", samples[k], (unsigned long long)(ps.bitmap_requests - requests));
    const GLenum flag = glGetError();
    CHECK(flag == GL_NO_ERROR, "GL error 0x%x after frame %ld", flag, samples[k]);
    CHECK(glr_read_pixels(r, got, W, H), "pixels");
    const uint8_t *want = reference + k * (size_t)W * H * 4;
    size_t differing = 0, first = 0;
    for (size_t i = 0; i < (size_t)W * H * 4; i++)
      if (got[i] != want[i]) {
        if (!differing++) first = i;
      }
    CHECK(differing == 0, "frame %ld: %zu bytes differ from the lazy reference (first at pixel %zu)", samples[k], differing, first / 4);
    printf("player scene egl: sample %ld: %s\n", samples[k], differing ? "DIFFERENT" : "identical to the reference");
    if (argc > 2) write_ppm(argv[2], "player", samples[k], got);
  }
  pscene_set_renderer(&ps, NULL);
  glr_destroy(r);
  CHECK(glGetError() == GL_NO_ERROR, "GL error after destroy");
  pscene_destroy(&ps);
  wz3_rt_destroy(rt);
  wz3_kx_destroy(kx);
  free(bytes);
  free(reference);
  free(got);
  eglMakeCurrent(e.display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
  eglDestroySurface(e.display, e.surface);
  eglDestroyContext(e.display, e.context);
  eglTerminate(e.display);
  printf("player scene egl: %d checks, %d failures\n", g_checks, g_failures);
  return g_failures ? 1 : 0;
}
