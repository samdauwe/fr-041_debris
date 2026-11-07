/* OpenGL command-trace check for glr.h.
 *
 *   test_glr_trace <fixtures dir> [--print]
 *
 * Replays the scripted Renderer lifetime of tools/dump_glr_trace.mjs (constructor, resize, render, letterboxing) against
 * a recording GL table and compares, call by call, with the trace the real JavaScript Renderer produced. Also checks
 * the shader text against the JS sources, and that every failure path (loader, compile, link, framebuffer, allocation)
 * is reported and leaves no GL object or memory behind.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

/* counting allocator with fault injection */
static long g_live, g_alloc_count, g_fail_alloc = -1;
static void *test_malloc(size_t n)
{
  if (g_alloc_count++ == g_fail_alloc) return NULL;
  size_t *p = (size_t *)malloc(n + 16);
  if (!p) return NULL;
  g_live++;
  return (char *)p + 16;
}
#if defined(__GNUC__)
__attribute__((noinline)) /* inlined into a NULL-checked caller, gcc misreports the header arithmetic */
#endif
static void test_free(void *q)
{
  if (!q) return;
  free((void *)((uintptr_t)q - 16));
  g_live--;
}
#define WZ3_MALLOC(n) test_malloc(n)
#define WZ3_FREE(p)   test_free(p)
#define GLR_MALLOC(n) test_malloc(n)
#define GLR_FREE(p)   test_free(p)

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
      if (g_failures <= 30) {                       \
        printf("FAIL %s:%d: ", __FILE__, __LINE__); \
        printf(__VA_ARGS__);                        \
        printf("\n");                               \
      }                                             \
    }                                               \
  } while (0)

static char *read_all(const char *dir, const char *name)
{
  char path[1024];
  snprintf(path, sizeof path, "%s/%s", dir, name);
  FILE *f = fopen(path, "rb");
  if (!f) {
    printf("cannot open %s\n", path);
    exit(2);
  }
  fseek(f, 0, SEEK_END);
  const long n = ftell(f);
  fseek(f, 0, SEEK_SET);
  char *data = (char *)malloc((size_t)n + 1);
  if (!data || fread(data, 1, (size_t)n, f) != (size_t)n) exit(2);
  fclose(f);
  data[n] = 0;
  return data;
}

/* ---- the recording GL (gl_mock.h) ---- */

#include "support/gl_mock.h"

#define g_live_objects glm.live_objects
#define g_fail_compile glm.fail_compile
#define g_fail_link    glm.fail_link
#define g_fail_fb      glm.fail_fb
#define g_fb_checks    glm.fb_checks
#define g_missing      glm.missing
#define mock_loader    glm_loader
#define g_trace_count  glm.kept_lines

static void reset_mock(void)
{
  glm_reset();
  glm.keep = 1;
}

/* the scripted lifetime of tools/dump_glr_trace.mjs */
static glr *run_script(glr_result *res)
{
  glr *r = glr_create(mock_loader, NULL, res);
  if (!r) return NULL;
  static const struct {
    int32_t w, h;
    double aspect;
  } script[] = { { 1280, 720, 2 }, { 1280, 720, 2 }, { 800, 600, 2 }, { 640, 360, 0 }, { 1921, 1081, 2 } };
  for (size_t i = 0; i < sizeof script / sizeof script[0]; i++) {
    *res = glr_resize(r, script[i].w, script[i].h, script[i].aspect);
    if (*res != GLR_OK) return r;
    *res = glr_render(r, NULL);
    if (*res != GLR_OK) return r;
  }
  return r;
}

static void test_trace(const char *fixture)
{
  size_t want = 0, mismatches = 0;
  reset_mock();
  glr_result res;
  glr *r = run_script(&res);
  CHECK(r != NULL && res == GLR_OK, "script: result %d, %s", (int)res, r ? glr_last_error(r) : "no renderer");
  const char *p = fixture;
  while (*p) {
    const char *nl = strchr(p, '\n');
    const size_t len = nl ? (size_t)(nl - p) : strlen(p);
    if (len > 2 && p[0] == 'T' && p[1] == ' ') {
      const char *line = p + 2;
      const size_t n = len - 2;
      if (!glm_line(want) || strlen(glm_line(want)) != n || strncmp(glm_line(want), line, n)) {
        if (++mismatches <= 5) printf("call %zu differs\n  JS: %.*s\n  C : %s\n", want, (int)n, line, glm_line(want) ? glm_line(want) : "(end of trace)");
      }
      want++;
    }
    if (!nl) break;
    p = nl + 1;
  }
  CHECK(mismatches == 0, "%zu GL calls differ from the JS renderer", mismatches);
  CHECK(want == g_trace_count, "JS recorded %zu calls, C %zu", want, (size_t)g_trace_count);
  printf("glr trace: %zu GL calls compared, %zu mismatches\n", want, mismatches);
  glr_destroy(r);
  CHECK(g_live_objects == 0, "%ld GL objects leaked after destroy", g_live_objects);
}

static void test_shader_sources(const char *fixture)
{
  static const char *names[] = { "material_vertex", "material_fragment", "shadow_vertex", "shadow_fragment", "fullscreen_vertex", "fullscreen_fragment" };
  for (size_t i = 0; i < 6; i++) {
    size_t length = 0;
    uint32_t h = 0x811c9dc5u;
    for (unsigned k = 0; k < glr__shader_sources[i].n_parts; k++) {
      const size_t n = strlen(glr__shader_sources[i].parts[k]);
      CHECK(n < 4095, "%s part %u has %zu characters", names[i], k, n);
      length += n;
      h = glm_fnv(h, glr__shader_sources[i].parts[k], n);
    }
    char key[64];
    snprintf(key, sizeof key, "S %s ", names[i]);
    const char *line = strstr(fixture, key);
    unsigned long want_length = 0, want_hash = 0;
    if (line) sscanf(line + strlen(key), "%lu %lx", &want_length, &want_hash);
    CHECK(line && length == want_length && h == (uint32_t)want_hash, "%s: C text %zu/%08x, JS %lu/%08lx", names[i], length, h, want_length, want_hash);
    CHECK(length == glr__shader_sources[i].length, "%s: recorded length", names[i]);
  }
  printf("glr shaders: 6 sources identical to src/renderer.js\n");
}

/* every failure path reports its error and leaves no GL object behind */
static void test_failures(void)
{
  glr_result res;
  reset_mock();
  g_missing = "glTexImage3D";
  CHECK(glr_create(mock_loader, NULL, &res) == NULL && res == GLR_ERR_LOADER, "missing loader function: %d", (int)res);
  CHECK(glr_create(NULL, NULL, &res) == NULL && res == GLR_ERR_ARGUMENT, "NULL loader");
  for (int program = 0; program < 3; program++) {
    for (int stage = 0; stage < 2; stage++) {
      reset_mock();
      g_fail_compile = program * 2 + stage;
      glr *r = glr_create(mock_loader, NULL, &res);
      CHECK(r == NULL && res == GLR_ERR_SHADER, "compile failure %d: %d", g_fail_compile, (int)res);
      CHECK(g_live_objects == 0, "compile failure %d leaves %ld GL objects", g_fail_compile, g_live_objects);
    }
    reset_mock();
    g_fail_link = program;
    glr *r = glr_create(mock_loader, NULL, &res);
    CHECK(r == NULL && res == GLR_ERR_LINK, "link failure %d: %d", program, (int)res);
    CHECK(g_live_objects == 0, "link failure %d leaves %ld GL objects", program, g_live_objects);
  }
  for (int fb = 0; fb < 2; fb++) {
    reset_mock();
    g_fail_fb = fb;
    glr *r = glr_create(mock_loader, NULL, &res);
    CHECK(r == NULL && res == GLR_ERR_FRAMEBUFFER, "framebuffer %d: %d", fb, (int)res);
    CHECK(g_live_objects == 0, "framebuffer failure %d leaves %ld GL objects", fb, g_live_objects);
  }
  /* a resize that cannot complete the framebuffer is reported too */
  reset_mock();
  glr *r = glr_create(mock_loader, NULL, &res);
  g_fail_fb = g_fb_checks;
  CHECK(r && glr_resize(r, 640, 360, 2) == GLR_ERR_FRAMEBUFFER, "resize framebuffer failure");
  glr_destroy(r);
  CHECK(g_live_objects == 0, "%ld GL objects leaked after a failed resize", g_live_objects);
  /* allocation failure of every library allocation: count them, then fail each one in turn */
  const long live = g_live;
  reset_mock();
  g_alloc_count = 0;
  g_fail_alloc = -1;
  glr *t = glr_create(mock_loader, NULL, &res);
  const long points = g_alloc_count;
  CHECK(t != NULL && points > 0, "allocation count run");
  glr_destroy(t);
  for (long fail = 0; fail < points; fail++) {
    reset_mock();
    g_alloc_count = 0;
    g_fail_alloc = fail;
    t = glr_create(mock_loader, NULL, &res);
    g_fail_alloc = -1;
    CHECK(t == NULL && res == GLR_ERR_OUT_OF_MEMORY && g_live == live && g_live_objects == 0, "allocation %ld: %d, %ld blocks, %ld objects", fail, (int)res,
          g_live - live, g_live_objects);
    if (t) glr_destroy(t);
  }
  printf("glr failures: loader, 6 compile, 3 link, 2 framebuffer, resize and %ld allocation points\n", points);
}

int suite_glr_trace(int argc, char **argv)
{
  if (argc < 2) {
    printf("usage: test_glr_trace <fixtures dir> [--print]\n");
    return 2;
  }
  char *fixture = read_all(argv[1], "glr_trace.txt");
  if (argc > 2 && !strcmp(argv[2], "--print")) {
    reset_mock();
    glr_result res;
    glr *r = run_script(&res);
    for (size_t i = 0; i < (size_t)g_trace_count; i++) printf("T %s\n", glm_line(i));
    glr_destroy(r);
    return 0;
  }
  test_shader_sources(fixture);
  test_trace(fixture);
  test_failures();
  free(fixture);
  glm_reset();
  CHECK(g_live == 0, "%ld library blocks leaked", g_live);
  printf("glr trace: %d checks, %d failures\n", g_checks, g_failures);
  return g_failures ? 1 : 0;
}
