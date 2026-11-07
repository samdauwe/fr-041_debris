/* Renders a song through the player's audio layer (paudio): a loader pre-roll, the hand-over to the song, then FRAMES
 * frames in irregular chunks like an audio callback would ask for them. Command line compatible with v2_render, so
 * compare_pcm.cmake can diff it with the v2redux oracle.
 *
 *   player_pcm SONG.v2m OUTPUT.f32 FRAMES
 */
#define V2_IMPLEMENTATION
#include <debris/core/v2.h>
#define PLAYER_CORE_IMPLEMENTATION
#include <debris/player/player_core.h>

#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv)
{
  if (argc < 4) {
    fprintf(stderr, "usage: %s SONG.v2m OUTPUT.f32 FRAMES\n", argv[0]);
    return 2;
  }
  FILE *f = fopen(argv[1], "rb");
  if (!f) return 1;
  fseek(f, 0, SEEK_END);
  const long size = ftell(f);
  fseek(f, 0, SEEK_SET);
  unsigned char *song = (unsigned char *)malloc((size_t)size);
  if (!song || fread(song, 1, (size_t)size, f) != (size_t)size) return 1;
  fclose(f);
  const uint64_t frames = strtoull(argv[3], NULL, 0);
  float *out = (float *)malloc(frames * 2 * sizeof(float));
  if (!out) return 1;

  paudio a;
  paudio_init(&a, 44100);
  static float discard[4096 * 2];
  if (paudio_play_loader(&a, song, (size_t)size) != V2_OK) return 1; /* pre-roll: the hand-over must not leak state */
  for (int i = 0; i < 3; i++) paudio_render(&a, discard, 4096);
  if (paudio_play_party(&a, song, (size_t)size, 0) != V2_OK) return 1;
  uint64_t done = 0, rng = 88172645463325252ull;
  while (done < frames) {
    rng ^= rng << 13;
    rng ^= rng >> 7;
    rng ^= rng << 17;
    uint64_t n = 1 + (rng >> 33) % 4096;
    if (n > frames - done) n = frames - done;
    paudio_render(&a, out + done * 2, (uint32_t)n);
    done += n;
  }
  paudio_destroy(&a);
  f = fopen(argv[2], "wb");
  const int ok = f && fwrite(out, sizeof(float) * 2, (size_t)frames, f) == (size_t)frames && fclose(f) == 0;
  free(out);
  free(song);
  return ok ? 0 : 1;
}
