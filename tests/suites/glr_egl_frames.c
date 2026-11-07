/* Real-driver check of glr_render_frame / glr_render_bitmap on the production party.
 *
 *   test_glr_egl_frames <assets dir> <fixtures dir> [screenshot dir]
 *
 * Headless OpenGL ES 3 through EGL (Mesa llvmpipe without a GPU). The Debris party document is run through the library runtime, the
 * three Bitmap_Render operators are drawn by glr and read back, and frames at several song positions are drawn at 640x360. Every
 * frame must leave the GL error flag clear and issue the number of draw calls of the JavaScript Renderer (tools/dump_glr_frames.mjs,
 * scenario bitmap_render); the images must show something (not a flat colour) and be identical when the frame is drawn twice.
 * With a screenshot dir the frames are written there as PPM files. Exits 77 (skipped) when no context can be created.
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

/* draw calls of the frames of one scenario of the JS fixture */
static unsigned fixture_draws(const char *fixture, const char *scenario, long sample)
{
  char key[96];
  snprintf(key, sizeof key, "S %s ", scenario);
  const char *start = strstr(fixture, key);
  if (!start) return 0;
  for (const char *line = strchr(start, '\n'); line; line = strchr(line + 1, '\n')) {
    const char *text = line + 1;
    if (text[0] == 'S') break;
    long s;
    unsigned calls, hash_value, draws, tris;
    if (text[0] == 'F' && sscanf(text, "F %ld %u %x %u %u", &s, &calls, &hash_value, &draws, &tris) == 5 && s == sample) return draws;
  }
  return 0;
}

static void write_ppm(const char *dir, long sample, const uint8_t *rgba, int w, int h)
{
  char path[1024];
  snprintf(path, sizeof path, "%s/glr_frame_%ld.ppm", dir, sample);
  FILE *f = fopen(path, "wb");
  if (!f) return;
  fprintf(f, "P6\n%d %d\n255\n", w, h);
  for (int i = 0; i < w * h; i++) fwrite(rgba + (size_t)i * 4, 1, 3, f);
  fclose(f);
}

int suite_glr_egl_frames(int argc, char **argv)
{
  if (argc < 3) {
    printf("usage: test_glr_egl_frames <assets dir> <fixtures dir> [screenshot dir]\n");
    return 2;
  }
  env e;
  const int W = 640, H = 360;
  if (!make_context(&e, W, H)) {
    printf("SKIP: no EGL / OpenGL ES 3 context\n");
    return 77;
  }
  printf("GL: %s / %s\n", (const char *)glGetString(GL_RENDERER), (const char *)glGetString(GL_VERSION));
  size_t kx_size;
  uint8_t *kx_bytes = gate_read_all(argv[1], "debris_party.kx", &kx_size);
  char *fixture = (char *)gate_read_all(argv[2], "glr_frames.txt", NULL);
  wz3_kx *kx = NULL;
  size_t at;
  CHECK(wz3_kx_parse(&kx, kx_bytes, kx_size, &at) == WZ3_OK, "parse");
  if (!kx) return 1;
  world w;
  if (!world_open(&w, kx, 1)) return 1;

  glr_host host;
  memset(&host, 0, sizeof host);
  host.user = &w;
  host.rt = w.rt;
  host.n_ops = kx->n_ops;
  host.geometry = host_geometry;
  host.shadow_data = host_shadow;
  host.bitmap = host_bitmap;
  w.host = &host;
  glr_options options;
  memset(&options, 0, sizeof options);
  char error[1024] = "";
  options.error = error;
  options.error_size = sizeof error;
  glr_result res;
  glr *r = glr_create(load_gl, &options, &res);
  CHECK(r != NULL && res == GLR_OK, "glr_create: %s", error);
  if (!r) return 1;
  w.renderer = r;
  CHECK(glGetError() == GL_NO_ERROR, "GL error after create");

  /* the Render operators first, in operator order: they draw through glr and become the textures of three materials */
  unsigned rendered = 0, nonblank = 0;
  for (uint32_t op = 0; op < kx->n_ops; op++) {
    if (kx->ops[op].class_id != 0x3f) continue;
    const wz3_bitmap *bitmap = NULL;
    CHECK(wz3_graph_eval_bitmap(w.graph, op, &bitmap) == WZ3_OK && bitmap && bitmap->width == 128 && bitmap->height == 128, "Render operator %u", op);
    CHECK(glGetError() == GL_NO_ERROR, "GL error after Render operator %u", op);
    rendered++;
    for (size_t i = 0; bitmap && i < (size_t)bitmap->width * bitmap->height * 4; i++)
      if (bitmap->data[i]) {
        nonblank++;
        break;
      }
  }
  CHECK(rendered == 3 && nonblank == 3, "%u Render operators, %u with content", rendered, nonblank);

  static const long samples[] = { 0, 3037500, 6075000, 15187500, 17717175 };
  uint8_t *image = (uint8_t *)malloc((size_t)W * H * 4), *again = (uint8_t *)malloc((size_t)W * H * 4);
  for (size_t k = 0; k < sizeof samples / sizeof samples[0]; k++) {
    const long sample = samples[k];
    CHECK(wz3_rt_frame_at_sample(w.rt, (int32_t)sample, 44100) == WZ3_OK, "frame %ld", sample);
    CHECK(glr_resize(r, W, H, 2.0) == GLR_OK && glr_render_frame(r, &host) == GLR_OK, "render %ld: %s", sample, glr_last_error(r));
    const GLenum error_flag = glGetError();
    CHECK(error_flag == GL_NO_ERROR, "GL error 0x%x after frame %ld", error_flag, sample);
    glr_stats st;
    glr_get_stats(r, &st);
    const unsigned expected = fixture_draws(fixture, "bitmap_render", sample);
    if (expected) CHECK(st.draw_calls == expected, "frame %ld: %u draw calls, the JavaScript Renderer made %u", sample, st.draw_calls, expected);
    CHECK(glr_read_pixels(r, image, W, H), "read pixels");
    /* something is drawn: many pixels differ from black and the image is not one flat colour */
    unsigned lit = 0, distinct = 0;
    uint32_t seen[64] = { 0 };
    for (int i = 0; i < W * H; i++) {
      const uint8_t *px = image + (size_t)i * 4;
      if (px[0] | px[1] | px[2]) lit++;
      const uint32_t key = ((uint32_t)px[0] >> 5) << 6 | ((uint32_t)px[1] >> 5) << 3 | ((uint32_t)px[2] >> 5);
      if (!(seen[key / 32] >> (key % 32) & 1u)) {
        seen[key / 32] |= 1u << (key % 32);
        distinct++;
      }
    }
    printf("glr egl frames: sample %ld: %u draw calls, %u triangles, %u lit pixels, %u colours\n", sample, st.draw_calls, st.triangles, lit, distinct);
    CHECK(st.draw_calls > 100 && st.triangles > 10000, "frame %ld draws too little", sample);
    /* song position 0 is the fade-in from black of the opening: its image is flat on purpose */
    if (sample > 0) {
      CHECK(lit > (unsigned)(W * H / 20), "frame %ld: only %u lit pixels", sample, lit);
      CHECK(distinct >= 4, "frame %ld: %u distinct colours", sample, distinct);
    }
    if (argc > 3) write_ppm(argv[3], sample, image, W, H);
    /* the same frame again: the caches are warm and the pixels identical */
    CHECK(glr_resize(r, W, H, 2.0) == GLR_OK && glr_render_frame(r, &host) == GLR_OK, "render again %ld", sample);
    CHECK(glr_read_pixels(r, again, W, H) && !memcmp(image, again, (size_t)W * H * 4), "frame %ld differs when drawn twice", sample);
    CHECK(glGetError() == GL_NO_ERROR, "GL error after the second frame %ld", sample);
  }
  free(image);
  free(again);
  w.renderer = NULL;
  glr_destroy(r);
  CHECK(glGetError() == GL_NO_ERROR, "GL error after destroy");
  world_free(&w, kx->n_ops);
  wz3_kx_destroy(kx);
  free(kx_bytes);
  free(fixture);
  eglMakeCurrent(e.display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
  eglDestroySurface(e.display, e.surface);
  eglDestroyContext(e.display, e.context);
  eglTerminate(e.display);
  printf("glr egl frames: %d checks, %d failures\n", g_checks, g_failures);
  return g_failures ? 1 : 0;
}
