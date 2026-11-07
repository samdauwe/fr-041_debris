/* Allocator override + "no allocation while rendering" test. */

#include <stdio.h>
#include <stdlib.h>

static size_t g_allocs, g_frees, g_fail_after = (size_t)-1;

static void *counting_malloc(size_t n)
{
  if (g_allocs >= g_fail_after) return NULL;
  g_allocs++;
  return malloc(n);
}

static void counting_free(void *p)
{
  if (p) g_frees++;
  free(p);
}

#define V2_MALLOC(n) counting_malloc(n)
#define V2_FREE(p)   counting_free(p)
#define V2_IMPLEMENTATION
#include <debris/core/v2.h>

#ifndef DEBRIS_ASSETS_DIR
#error "DEBRIS_ASSETS_DIR must name the repo assets directory"
#endif

static int failures;
#define CHECK(c)                                                            \
  do {                                                                      \
    if (!(c)) {                                                             \
      failures++;                                                           \
      fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #c); \
    }                                                                       \
  } while (0)

int suite_v2_alloc(int argc, char **argv)
{
  (void)argc;
  (void)argv;
  char path[1024];
  snprintf(path, sizeof path, "%s/debris_loader.v2m", DEBRIS_ASSETS_DIR);
  FILE *f = fopen(path, "rb");
  if (!f) {
    perror(path);
    return 2;
  }
  static unsigned char song[16384];
  size_t size = fread(song, 1, sizeof song, f);
  fclose(f);

  /* failing allocator: creation and open report it instead of crashing */
  g_fail_after = 0;
  CHECK(v2_create(44100) == NULL);
  g_fail_after = 1;
  v2_synth *s = v2_create(44100);
  CHECK(s != NULL);
  CHECK(v2_open(s, song, size) == V2_ERR_OUT_OF_MEMORY);
  v2_destroy(s);
  CHECK(g_allocs == g_frees);

  g_fail_after = (size_t)-1;
  g_allocs = g_frees = 0;
  s = v2_create(44100);
  CHECK(s && v2_open(s, song, size) == V2_OK);
  CHECK(g_allocs == 2); /* the instance and the song copy, nothing else */

  /* the render path, seeking, MIDI and monitors never allocate */
  size_t before = g_allocs;
  static float out[2 * 4096];
  int poly[16];
  uint32_t ons[16];
  float lv[16];
  v2_play(s, 0);
  for (int i = 0; i < 100; i++) v2_render(s, out, 4096);
  v2_play(s, 10000);
  v2_render(s, out, 4096);
  (void)v2_length_frames(s);
  v2_play(s, 0);
  v2_midi(s, (const uint8_t *)"\x90\x30\x40", 3);
  v2_render_direct(s, out, 4096);
  v2_set_channel_mask(s, 0xff);
  v2_set_seed(s, 5);
  v2_get_channel_poly(s, poly);
  v2_get_channel_note_ons(s, ons);
  v2_get_channel_levels(s, lv);
  v2_stop(s, 20);
  v2_render(s, out, 4096);
  CHECK(g_allocs == before);

  /* re-opening releases the previous song copy */
  CHECK(v2_open(s, song, size) == V2_OK);
  CHECK(g_allocs == before + 1 && g_frees == 1);

  v2_destroy(s);
  CHECK(g_allocs == g_frees);

  printf("%s\n", failures ? "alloc test FAILED" : "alloc test passed");
  return failures ? 1 : 0;
}
