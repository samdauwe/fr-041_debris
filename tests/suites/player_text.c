/* Checks for player_text.h: the text of the player's own screens.
 *
 *   test_player_text
 *
 * The image is tight (the first and last row and column hold ink), premultiplied (no channel above alpha), deterministic, scales with the
 * size and the number of lines, refuses what it cannot draw, and every allocation of a render is failed in turn without leaking.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static long g_live, g_count, g_fail_at = -1;
static void *t_malloc(size_t n)
{
  if (g_count++ == g_fail_at) return NULL;
  size_t *p = (size_t *)malloc(n + 16);
  if (!p) return NULL;
  g_live++;
  return (char *)p + 16;
}
#if defined(__GNUC__)
__attribute__((noinline))
#endif
static void t_free(void *q)
{
  if (!q) return;
  free((void *)((uintptr_t)q - 16));
  g_live--;
}
#define WZ3_MALLOC(n)         t_malloc(n)
#define WZ3_FREE(p)           t_free(p)
#define PLAYER_TEXT_MALLOC(n) t_malloc(n)
#define PLAYER_TEXT_FREE(p)   t_free(p)

#define WZ3_IMPLEMENTATION
#include <debris/core/wz3.h>
#define PLAYER_TEXT_IMPLEMENTATION
#include <debris/player/player_text.h>

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

static uint64_t ink(const ptext_image *im)
{
  uint64_t n = 0;
  for (size_t i = 0; i < (size_t)im->width * (size_t)im->height; i++) n += im->rgba[i * 4 + 3];
  return n;
}

static void check_tight_and_premultiplied(const ptext_image *im, const char *what)
{
  int top = 0, left = 0;
  for (int32_t x = 0; x < im->width; x++) top |= im->rgba[x * 4 + 3] != 0;
  for (int32_t y = 0; y < im->height; y++) left |= im->rgba[(size_t)y * (size_t)im->width * 4 + 3] != 0;
  CHECK(top && left, "%s: the first row or column is empty", what);
  int over = 0, ink_seen = 0;
  for (size_t i = 0; i < (size_t)im->width * (size_t)im->height; i++) {
    const uint8_t *px = im->rgba + i * 4;
    over |= px[0] > px[3] || px[1] > px[3] || px[2] > px[3];
    ink_seen |= px[3] != 0;
  }
  CHECK(!over && ink_seen, "%s: a channel above alpha, or no ink", what);
}

int suite_player_text(int argc, char **argv)
{
  (void)argc;
  (void)argv;
  ptext_image a, b, big, two, white, red;
  CHECK(ptext_render(&a, "debris", 24, 0xffffff) == 1 && a.rgba && a.width > 40 && a.height > 12 && a.width > a.height, "debris at 24 px: %dx%d", a.width,
        a.height);
  check_tight_and_premultiplied(&a, "debris");
  /* deterministic */
  CHECK(ptext_render(&b, "debris", 24, 0xffffff) == 1 && a.width == b.width && a.height == b.height &&
          !memcmp(a.rgba, b.rgba, (size_t)a.width * (size_t)a.height * 4),
        "the same text twice is the same image");
  /* size and lines */
  CHECK(ptext_render(&big, "debris", 48, 0xffffff) == 1 && big.width > a.width * 3 / 2 && big.height > a.height * 3 / 2 && ink(&big) > ink(&a) * 2,
        "48 px is about twice 24 px: %dx%d vs %dx%d", big.width, big.height, a.width, a.height);
  CHECK(ptext_render(&two, "debris\ndebris", 24, 0xffffff) == 1 && two.width >= a.width && two.width <= a.width + 2 && two.height > a.height * 3 / 2 &&
          two.height < a.height * 3,
        "two lines are taller, not wider: %dx%d vs %dx%d", two.width, two.height, a.width, a.height);
  check_tight_and_premultiplied(&two, "two lines");
  /* colour: the ink takes the colour (premultiplied); a drop shadow is black */
  CHECK(ptext_render(&white, "I", 40, 0xffffff) == 1 && ptext_render(&red, "I", 40, 0xff0000) == 1 && white.width == red.width && white.height == red.height,
        "colours keep the shape");
  int solid_red = 0, stray = 0;
  for (size_t i = 0; i < (size_t)red.width * (size_t)red.height; i++) {
    const uint8_t *px = red.rgba + i * 4;
    solid_red += px[0] == 255 && px[1] == 0 && px[2] == 0 && px[3] == 255;
    stray += px[1] || px[2];
  }
  CHECK(solid_red > 10 && stray == 0, "red text: %d solid red pixels, %d with green or blue", solid_red, stray);
  int shadow = 0;
  for (size_t i = 0; i < (size_t)white.width * (size_t)white.height; i++) {
    const uint8_t *px = white.rgba + i * 4;
    shadow += px[3] > 0 && px[0] == 0 && px[1] == 0 && px[2] == 0;
  }
  CHECK(shadow > 10, "%d pure shadow pixels", shadow);
  /* spaces and unknown characters draw as spaces; text with only spaces draws nothing */
  ptext_image s;
  CHECK(ptext_render(&s, "   ", 24, 0xffffff) == 0 && !s.rgba && s.width == 0 && ptext_render(&s, "\n\n", 24, 0xffffff) == 0, "no printable text");
  CHECK(ptext_render(&s, "", 24, 0xffffff) == 0 && ptext_render(NULL, "x", 24, 0xffffff) == 0 && ptext_render(&s, NULL, 24, 0xffffff) == 0, "nothing to draw");
  CHECK(ptext_render(&s, "x", 5, 0xffffff) == 0 && ptext_render(&s, "x", 257, 0xffffff) == 0, "size out of range");
  CHECK(ptext_render(&s, "x", 6, 0xffffff) == 1, "the smallest size");
  ptext_free(&s);
  CHECK(ptext_render(&s, "x", 256, 0xffffff) == 1 && s.height > 100, "the largest size");
  ptext_free(&s);
  {
    char longtext[2105];
    memset(longtext, 'x', 2001);
    longtext[2001] = 0;
    CHECK(ptext_render(&s, longtext, 12, 0xffffff) == 0, "a text of 2001 bytes");
    memset(longtext, 'x', 300);
    longtext[300] = 0;
    CHECK(ptext_render(&s, longtext, 24, 0xffffff) == 0, "a line wider than the rasterizer's surface");
  }
  CHECK(ptext_render(&s, "caf\xc3\xa9 \xc2\xb4 \xe2\x82\xac", 20, 0xc0c0c0) == 1, "UTF-8 beyond ASCII falls back to a space or the rasterizer's question mark");
  ptext_free(&s);
  {
    char many[2 * 201 + 1];
    for (int i = 0; i < 201; i++) {
      many[i * 2] = 'x';
      many[i * 2 + 1] = '\n';
    }
    many[401] = 0;
    CHECK(ptext_render(&s, many, 6, 0xffffff) == 0, "201 lines");
  }
  ptext_free(&a);
  ptext_free(&b);
  ptext_free(&big);
  ptext_free(&two);
  ptext_free(&white);
  ptext_free(&red);
  ptext_free(NULL);
  CHECK(g_live == 0, "frees leave %ld blocks", g_live);

  /* every allocation of a render fails in turn: reported, nothing leaks */
  long points = 0;
  for (long k = 0; k < 64; k++) {
    g_count = 0;
    g_fail_at = k;
    ptext_image im;
    const int ok = ptext_render(&im, "credits\nstats", 20, 0xffffff);
    g_fail_at = -1;
    if (ok) {
      ptext_free(&im);
      break;
    }
    points++;
    CHECK(im.rgba == NULL && im.width == 0 && g_live == 0, "allocation %ld failed: image %p, %ld blocks left", k, (void *)im.rgba, g_live);
  }
  CHECK(points >= 2 && points < 64, "%ld allocation points", points);
  printf("player_text: %d checks, %d failures\n", g_checks, g_failures);
  return g_failures ? 1 : 0;
}
