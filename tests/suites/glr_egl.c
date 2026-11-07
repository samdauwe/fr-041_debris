/* Real-driver check of glr.h.
 *
 * Creates a headless OpenGL ES 3 context through EGL (Mesa llvmpipe when no GPU is available), compiles and links the
 * real shaders of src/renderer.js, builds the render targets and lookup textures, and checks pixels of the letterboxed
 * presentation against values computed here. Exits 77 (skipped) when no context can be created.
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

static int g_checks, g_failures;
#define CHECK(cond, ...)                            \
  do {                                              \
    g_checks++;                                     \
    if (!(cond)) {                                  \
      g_failures++;                                 \
      if (g_failures <= 20) {                       \
        printf("FAIL %s:%d: ", __FILE__, __LINE__); \
        printf(__VA_ARGS__);                        \
        printf("\n");                               \
      }                                             \
    }                                               \
  } while (0)

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

/* a texture whose texel (x, y) is a function of its position, so that every presented pixel is checkable */
static void pattern(int x, int y, uint8_t out[4])
{
  out[0] = (uint8_t)(x & 255);
  out[1] = (uint8_t)(y & 255);
  out[2] = (uint8_t)((x * 7 + y * 13) & 255);
  out[3] = 255;
}

static GLuint make_pattern_texture(int width, int height)
{
  uint8_t *data = (uint8_t *)malloc((size_t)width * height * 4);
  for (int y = 0; y < height; y++)
    for (int x = 0; x < width; x++) pattern(x, y, data + ((size_t)y * width + x) * 4);
  GLuint tex = 0;
  glGenTextures(1, &tex);
  glBindTexture(GL_TEXTURE_2D, tex);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, data);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  free(data);
  return tex;
}

/* One-to-one presentation: the texture has the size of the region, so output pixel (x, y) equals texel (x, y). */
static void check_region(const uint8_t *image, int cw, int ch, wz3_region region, int has_pattern, const char *label)
{
  long bad_inside = 0, bad_outside = 0;
  for (int row = 0; row < ch; row++) {
    const int gl_y = ch - 1 - row; /* image rows run top-down, GL rows bottom-up */
    for (int x = 0; x < cw; x++) {
      const uint8_t *px = image + ((size_t)row * cw + x) * 4;
      uint8_t want[4] = { 0, 0, 0, 255 };
      const int inside = x >= region.x && x < region.x + region.width && gl_y >= region.y && gl_y < region.y + region.height;
      if (inside && has_pattern) pattern(x - region.x, gl_y - region.y, want);
      if (memcmp(px, want, 4)) {
        if (inside)
          bad_inside++;
        else
          bad_outside++;
      }
    }
  }
  CHECK(bad_inside == 0 && bad_outside == 0, "%s: %ld wrong pixels inside the region, %ld in the letterbox", label, bad_inside, bad_outside);
}

int suite_glr_egl(int argc, char **argv)
{
  (void)argc;
  (void)argv;
  env e;
  if (!make_context(&e, 1921, 1081)) {
    printf("SKIP: no EGL / OpenGL ES 3 context\n");
    return 77;
  }
  printf("GL: %s / %s\n", (const char *)glGetString(GL_RENDERER), (const char *)glGetString(GL_VERSION));

  glr_result res;
  glr *r = glr_create(load_gl, NULL, &res);
  CHECK(r != NULL && res == GLR_OK, "glr_create: %d %s", (int)res, r ? "" : "(no renderer)");
  if (!r) {
    printf("glr_create failed: %d\n", (int)res);
    return 1;
  }
  CHECK(glGetError() == GL_NO_ERROR, "GL error after create");

  struct {
    int32_t w, h;
    double aspect;
  } sizes[] = { { 1000, 700, 2 }, { 1921, 1081, 2 }, { 640, 360, 0 }, { 301, 997, 2 }, { 1280, 720, 2 } };
  for (size_t i = 0; i < sizeof sizes / sizeof sizes[0]; i++) {
    char label[96];
    const int32_t cw = sizes[i].w, ch = sizes[i].h;
    CHECK(glr_resize(r, cw, ch, sizes[i].aspect) == GLR_OK, "resize %dx%d", cw, ch);
    glr_stats st;
    glr_get_stats(r, &st);
    wz3_region region = st.presentation;
    CHECK(st.render_width == region.width && st.render_height == region.height, "render size");
    if (sizes[i].aspect > 0) CHECK(region.x == 0 || region.y == 0, "letterbox touches one edge %dx%d", cw, ch);

    uint8_t *image = (uint8_t *)malloc((size_t)cw * ch * 4);
    GLuint tex = make_pattern_texture(region.width, region.height);
    glr_present(r, tex);
    CHECK(glr_read_pixels(r, image, cw, ch), "read pixels");
    snprintf(label, sizeof label, "present %dx%d aspect %g (region %d,%d %dx%d)", cw, ch, sizes[i].aspect, region.x, region.y, region.width, region.height);
    check_region(image, cw, ch, region, 1, label);

    /* the master target is cleared to opaque black and shown through the same path */
    CHECK(glr_render(r, NULL) == GLR_OK, "render");
    CHECK(glr_read_pixels(r, image, cw, ch), "read pixels");
    snprintf(label, sizeof label, "render %dx%d", cw, ch);
    check_region(image, cw, ch, region, 0, label);
    CHECK(glGetError() == GL_NO_ERROR, "GL error after %dx%d", cw, ch);
    glDeleteTextures(1, &tex);
    free(image);
  }
  {
    /* 1000x700 at aspect 2 is a 1000x500 image centred vertically */
    glr_stats st;
    glr_resize(r, 1000, 700, 2);
    glr_get_stats(r, &st);
    CHECK(st.presentation.x == 0 && st.presentation.y == 100 && st.presentation.width == 1000 && st.presentation.height == 500,
          "hand-checked letterbox: %d,%d %dx%d", st.presentation.x, st.presentation.y, st.presentation.width, st.presentation.height);
  }
  {
    /* loader screen: black, a gray track, a white segment moving across it; every phase stays inside the track */
    const int32_t cw = 640, ch = 360;
    uint8_t *image = (uint8_t *)malloc((size_t)cw * ch * 4);
    glr_resize(r, cw, ch, 2);
    const int bw = cw / 3, bh = ch / 60, x0 = (cw - bw) / 2, y0 = (ch - bh) / 2;
    for (int step = 0; step < 8; step++) {
      glr_present_loading(r, step / 8.0);
      CHECK(glr_read_pixels(r, image, cw, ch), "read loading");
      int white = 0, gray = 0, other = 0;
      for (int row = 0; row < ch; row++)
        for (int x = 0; x < cw; x++) {
          const uint8_t *px = image + ((size_t)row * cw + x) * 4;
          const int gl_y = ch - 1 - row;
          const int in_track = x >= x0 && x < x0 + bw && gl_y >= y0 && gl_y < y0 + bh;
          if (!in_track) {
            if (px[0] | px[1] | px[2]) other++;
            continue;
          }
          if (px[0] == 255 && px[1] == 255 && px[2] == 255)
            white++;
          else if (px[0] >= 50 && px[0] <= 52 && px[1] == px[0] && px[2] == px[0])
            gray++;
          else
            other++;
        }
      CHECK(other == 0 && white + gray == bw * bh && (step == 0 ? white >= 0 : white > 0), "loading phase %d: %d white, %d gray, %d stray", step, white, gray,
            other);
      { /* the segment is where the travel formula puts it (a quarter of the track wide, clipped to the track) */
        const int seg = bw / 4;
        int from = (int)(step / 8.0 * (double)(bw + seg)) - seg, to = from + seg;
        from = from < 0 ? 0 : from;
        to = to > bw ? bw : to;
        CHECK(white == (to > from ? to - from : 0) * bh, "loading phase %d: %d white pixels, expected %d", step, white, (to > from ? to - from : 0) * bh);
      }
    }
    /* the determinate bar: the white run is exactly the floor of the fraction of the track, whole rows, nothing outside the track */
    static const double fractions[] = { 0.0, 0.25, 0.5, 0.999, 1.0, -3.0, 7.0, 0.0 / 0.0 };
    static const int expected_white[] = { 0, 640 / 3 / 4, 640 / 3 / 2, 212, 213, 0, 213, 0 }; /* bw = 213 */
    for (size_t k = 0; k < sizeof fractions / sizeof fractions[0]; k++) {
      glr_present_progress(r, fractions[k]);
      CHECK(glr_read_pixels(r, image, cw, ch), "read progress");
      int white = 0, gray = 0, other = 0, run_end = 0;
      for (int row = 0; row < ch; row++)
        for (int x = 0; x < cw; x++) {
          const uint8_t *px = image + ((size_t)row * cw + x) * 4;
          const int gl_y = ch - 1 - row;
          const int in_track = x >= x0 && x < x0 + bw && gl_y >= y0 && gl_y < y0 + bh;
          if (!in_track) {
            if (px[0] | px[1] | px[2]) other++;
            continue;
          }
          if (px[0] == 255 && px[1] == 255 && px[2] == 255) {
            white++;
            if (x - x0 + 1 > run_end) run_end = x - x0 + 1;
          } else if (px[0] >= 50 && px[0] <= 52 && px[1] == px[0] && px[2] == px[0])
            gray++;
          else
            other++;
        }
      CHECK(bw == 213 && other == 0 && white == expected_white[k] * bh && white + gray == bw * bh && run_end == expected_white[k],
            "progress %g: %d white pixels, run %d wide (expected %d), %d gray, %d stray", fractions[k], white, run_end, expected_white[k], gray, other);
    }
    free(image);
  }
  {
    /* overlays (credits, statistics): premultiplied RGBA, top row first, top-left corner at (x, y) from the top-left of the drawable */
    const int32_t cw = 640, ch = 360, pw = 40, ph = 20;
    uint8_t *image = (uint8_t *)malloc((size_t)cw * ch * 4), *patch = (uint8_t *)calloc((size_t)pw * ph, 4);
    for (int j = 0; j < ph; j++)
      for (int i = 0; i < pw; i++) {
        uint8_t *px = patch + ((size_t)j * pw + i) * 4;
        if (j < ph / 2) {
          px[0] = 255;
          px[3] = 255;
        } /* the top half: opaque red */
        else {
          px[1] = 128;
          px[3] = 128;
        } /* the bottom half: half transparent green, premultiplied */
      }
    glr_resize(r, cw, ch, 2);
    CHECK(glr_render(r, NULL) == GLR_OK, "render");
    CHECK(glr_present_overlay(r, patch, pw, ph, 100, 50) == GLR_OK, "overlay");
    CHECK(glr_read_pixels(r, image, cw, ch), "read overlay");
    long wrong = 0;
    for (int row = 0; row < ch; row++)
      for (int x = 0; x < cw; x++) {
        const uint8_t *px = image + ((size_t)row * cw + x) * 4;
        uint8_t want[4] = { 0, 0, 0, 255 };
        if (x >= 100 && x < 100 + pw && row >= 50 && row < 50 + ph) {
          if (row < 50 + ph / 2) {
            want[0] = 255;
          } else
            want[1] = 128;
        }
        if (abs(px[0] - want[0]) > 1 || abs(px[1] - want[1]) > 1 || abs(px[2] - want[2]) > 1 || px[3] != 255) wrong++;
      }
    CHECK(wrong == 0, "overlay: %ld wrong pixels (position, orientation or blend)", wrong);
    /* clipped at the top-left corner, and at the bottom-right one */
    CHECK(glr_render(r, NULL) == GLR_OK && glr_present_overlay(r, patch, pw, ph, -10, -5) == GLR_OK &&
            glr_present_overlay(r, patch, pw, ph, cw - 10, ch - 5) == GLR_OK,
          "clipped overlays");
    CHECK(glr_read_pixels(r, image, cw, ch), "read clipped overlay");
    CHECK(image[0] == 255 && image[1] == 0 && image[((size_t)(ch - 1) * cw + (cw - 1)) * 4] == 255 && image[((size_t)(ch - 1) * cw + (cw - 1)) * 4 + 1] == 0,
          "clipped overlays keep the visible part (rows 5.. of the patch top-left, rows ..4 bottom-right)");
    /* a fully transparent overlay changes nothing; the overlay leaves the GL state a frame needs */
    memset(patch, 0, (size_t)pw * ph * 4);
    CHECK(glr_render(r, NULL) == GLR_OK, "render");
    CHECK(glr_present_overlay(r, patch, pw, ph, 0, 0) == GLR_OK && glr_read_pixels(r, image, cw, ch), "empty overlay");
    wrong = 0;
    for (size_t i = 0; i < (size_t)cw * ch; i++) wrong += image[i * 4] | image[i * 4 + 1] | image[i * 4 + 2];
    CHECK(wrong == 0, "a transparent overlay changed %ld pixels", wrong);
    CHECK(glr_present_overlay(NULL, patch, pw, ph, 0, 0) == GLR_ERR_ARGUMENT && glr_present_overlay(r, NULL, pw, ph, 0, 0) == GLR_ERR_ARGUMENT &&
            glr_present_overlay(r, patch, 0, ph, 0, 0) == GLR_ERR_ARGUMENT && glr_present_overlay(r, patch, pw, 0, 0, 0) == GLR_ERR_ARGUMENT &&
            glr_present_overlay(r, patch, 8193, ph, 0, 0) == GLR_ERR_ARGUMENT,
          "overlay arguments");
    CHECK(glGetError() == GL_NO_ERROR, "GL error after the overlays");
    free(image);
    free(patch);
  }
  glr_destroy(r);
  CHECK(glGetError() == GL_NO_ERROR, "GL error after destroy");
  eglMakeCurrent(e.display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
  eglDestroySurface(e.display, e.surface);
  eglDestroyContext(e.display, e.context);
  eglTerminate(e.display);
  printf("glr egl: %d checks, %d failures\n", g_checks, g_failures);
  return g_failures ? 1 : 0;
}
