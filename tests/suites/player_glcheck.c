/* Checks for player_glcheck.h: the bookkeeping of the soak's GL object accounting and error polling.
 *
 *   test_player_glcheck
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define PLAYER_GLCHECK_IMPLEMENTATION
#include <debris/player/player_glcheck.h>

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

int suite_player_glcheck(int argc, char **argv)
{
  (void)argc;
  (void)argv;
  pglcheck c, d;
  pglcheck_init(&c);
  pglcheck_init(NULL);
  CHECK(pglcheck_total(&c) == 0 && pglcheck_total(NULL) == 0 && c.errors == 0 && c.polls == 0, "starts empty");

  /* creation and deletion count per kind; id 0 is not an object */
  const uint32_t ids[4] = { 7, 0, 9, 11 };
  pglcheck_created(&c, PGL_TEXTURE, 4, ids);
  pglcheck_created(&c, PGL_BUFFER, 3, NULL);
  pglcheck_created(&c, PGL_PROGRAM, 1, NULL);
  CHECK(c.live[PGL_TEXTURE] == 3 && c.live[PGL_BUFFER] == 3 && c.live[PGL_PROGRAM] == 1 && c.created[PGL_TEXTURE] == 3 && pglcheck_total(&c) == 7,
        "live counts");
  pglcheck_deleted(&c, PGL_TEXTURE, 2, ids + 2);
  pglcheck_deleted(&c, PGL_TEXTURE, 1, ids + 1); /* deleting id 0 does nothing */
  CHECK(c.live[PGL_TEXTURE] == 1 && c.deleted[PGL_TEXTURE] == 2 && c.over_deleted == 0, "delete counts only real ids");
  pglcheck_deleted(&c, PGL_BUFFER, 3, NULL);
  CHECK(c.live[PGL_BUFFER] == 0 && c.over_deleted == 0, "balanced");
  /* a deletion beyond the creations is remembered and does not drive the count negative */
  pglcheck_deleted(&c, PGL_VERTEX_ARRAY, 2, NULL);
  CHECK(c.live[PGL_VERTEX_ARRAY] == 0 && c.over_deleted == 2, "over-deletion: %llu", (unsigned long long)c.over_deleted);
  /* zero, negative and out-of-range requests are ignored */
  pglcheck_created(&c, PGL_SHADER, 0, NULL);
  pglcheck_created(&c, PGL_SHADER, -5, NULL);
  pglcheck_created(&c, (pgl_kind)99, 3, NULL);
  pglcheck_deleted(&c, (pgl_kind)99, 3, NULL);
  pglcheck_created(NULL, PGL_SHADER, 1, NULL);
  pglcheck_deleted(NULL, PGL_SHADER, 1, NULL);
  CHECK(c.live[PGL_SHADER] == 0 && c.over_deleted == 2 && pglcheck_total(&c) == 2, "ignored requests");

  /* errors: the first one is kept with its poll, 0 entries are ignored */
  const uint32_t none[2] = { 0, 0 }, some[3] = { 0, 0x0502, 0x0501 };
  pglcheck_polled(&c, none, 2);
  CHECK(c.polls == 1 && c.errors == 0 && c.first_error == 0, "a clean poll");
  pglcheck_polled(&c, some, 3);
  pglcheck_polled(&c, NULL, 0);
  pglcheck_polled(&c, some, 1);
  CHECK(c.polls == 4 && c.errors == 2 && c.first_error == 0x0502 && c.first_error_poll == 2 && c.last_error == 0x0501,
        "errors: %llu, first 0x%x at poll %llu, last 0x%x", (unsigned long long)c.errors, c.first_error, (unsigned long long)c.first_error_poll, c.last_error);
  pglcheck_polled(NULL, some, 3);

  /* snapshots */
  d = c;
  CHECK(pglcheck_same_objects(&c, &d), "a copy is the same");
  pglcheck_created(&d, PGL_FRAMEBUFFER, 1, NULL);
  CHECK(!pglcheck_same_objects(&c, &d) && !pglcheck_same_objects(&c, NULL) && !pglcheck_same_objects(NULL, &d), "a framebuffer more is a difference");
  pglcheck_deleted(&d, PGL_FRAMEBUFFER, 1, NULL);
  CHECK(pglcheck_same_objects(&c, &d), "and gone again");

  /* names */
  CHECK(!strcmp(pglcheck_kind_name(PGL_TEXTURE), "textures") && !strcmp(pglcheck_kind_name(PGL_SHADER), "shaders") &&
          !strcmp(pglcheck_kind_name((pgl_kind)99), "?"),
        "kind names");
  for (int k = 0; k < PGL_KINDS; k++) CHECK(strcmp(pglcheck_kind_name((pgl_kind)k), "?") != 0, "kind %d has a name", k);
  CHECK(!strcmp(pglcheck_error_name(0x0502), "GL_INVALID_OPERATION") && !strcmp(pglcheck_error_name(0x0505), "GL_OUT_OF_MEMORY") &&
          !strcmp(pglcheck_error_name(0x0506), "GL_INVALID_FRAMEBUFFER_OPERATION") && !strcmp(pglcheck_error_name(0), "GL_NO_ERROR") &&
          !strcmp(pglcheck_error_name(0x1234), "unknown GL error"),
        "error names");

  /* the report fits any buffer */
  char text[512];
  pglcheck_format(&c, text, sizeof text);
  CHECK(strstr(text, "textures 1") && strstr(text, "programs 1") && strstr(text, "2 GL errors in 4 polls") && strstr(text, "2 over-deleted"), "report: %s",
        text);
  for (size_t size = 1; size < 90; size++) {
    char small[100];
    memset(small, 'x', sizeof small);
    pglcheck_format(&c, small, size);
    CHECK(memchr(small, 0, size) != NULL && small[size] == 'x', "a %zu byte buffer is terminated and not overrun", size);
  }
  pglcheck_format(&c, NULL, 10);
  pglcheck_format(&c, text, 0);
  pglcheck_format(NULL, text, sizeof text);
  CHECK(text[0] == 0, "no counters, no text");
  printf("player_glcheck: %d checks, %d failures\n", g_checks, g_failures);
  return g_failures ? 1 : 0;
}
