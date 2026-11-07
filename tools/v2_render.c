/* v2_render: offline renderer built on v2.h, command-line compatible with
   tools/oracles/v2_render_oracle.cpp so the two outputs can be diffed:

     v2_render INPUT.v2m OUTPUT.f32 FRAMES [CHANNEL_MASK]

   Extras:
     --rate HZ            render at another sample rate (default 44100)
     --chunk N            render in chunks of N frames (default: one call)
   Output is raw interleaved stereo little-endian float32. */

#define V2_IMPLEMENTATION
#include <debris/core/v2.h>

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int read_file(const char *path, unsigned char **data, size_t *size)
{
  FILE *f = fopen(path, "rb");
  if (!f) return 0;
  long len;
  if (fseek(f, 0, SEEK_END) || (len = ftell(f)) < 0 || fseek(f, 0, SEEK_SET)) {
    fclose(f);
    return 0;
  }
  *data = (unsigned char *)malloc(len ? (size_t)len : 1);
  if (!*data || (len && fread(*data, 1, (size_t)len, f) != (size_t)len)) {
    free(*data);
    fclose(f);
    return 0;
  }
  *size = (size_t)len;
  fclose(f);
  return 1;
}

static int write_file(const char *path, const void *data, size_t size)
{
  FILE *f = fopen(path, "wb");
  if (!f) return 0;
  int ok = !size || fwrite(data, 1, size, f) == size;
  return fclose(f) == 0 && ok;
}

static int parse_u32(const char *text, uint32_t *value)
{
  errno = 0;
  char *end = NULL;
  unsigned long long v = strtoull(text, &end, 0);
  if (errno || !end || *end || v > 0xffffffffull) return 0;
  *value = (uint32_t)v;
  return 1;
}

static int usage(const char *argv0)
{
  fprintf(stderr, "usage: %s [--rate HZ] [--chunk N] INPUT.v2m OUTPUT.f32 FRAMES [CHANNEL_MASK]\n", argv0);
  return 2;
}

int main(int argc, char **argv)
{
  uint32_t rate = 44100, chunk = 0, frames = 0, mask = 0xffffu;
  int i = 1;

  while (i < argc && !strncmp(argv[i], "--", 2)) {
    if (i + 1 >= argc) return usage(argv[0]);
    if (!strcmp(argv[i], "--rate")) {
      if (!parse_u32(argv[i + 1], &rate)) return usage(argv[0]);
    } else if (!strcmp(argv[i], "--chunk")) {
      if (!parse_u32(argv[i + 1], &chunk)) return usage(argv[0]);
    } else {
      return usage(argv[0]);
    }
    i += 2;
  }
  if (argc - i < 3 || argc - i > 4) return usage(argv[0]);
  if (!parse_u32(argv[i + 2], &frames) || (argc - i == 4 && !parse_u32(argv[i + 3], &mask))) {
    fprintf(stderr, "invalid frame count or channel mask\n");
    return 2;
  }

  unsigned char *song;
  size_t song_size;
  if (!read_file(argv[i], &song, &song_size)) {
    perror(argv[i]);
    return 1;
  }

  v2_synth *synth = v2_create(rate);
  if (!synth) {
    fprintf(stderr, "invalid sample rate %u (supported: %u..%u)\n", (unsigned)rate, V2_MIN_SAMPLE_RATE, V2_MAX_SAMPLE_RATE);
    free(song);
    return 2;
  }
  v2_result r = v2_open(synth, song, song_size);
  free(song);
  if (r != V2_OK) {
    fprintf(stderr, "v2 rejected the input: %s\n", v2_result_string(r));
    v2_destroy(synth);
    return 1;
  }

  v2_set_channel_mask(synth, mask);
  v2_play(synth, 0);

  size_t pcm_floats = (size_t)frames * 2;
  float *pcm = (float *)malloc((pcm_floats ? pcm_floats : 1) * sizeof(float));
  if (!pcm) {
    fprintf(stderr, "out of memory\n");
    v2_destroy(synth);
    return 1;
  }

  if (!chunk) chunk = frames ? frames : 1;
  for (uint32_t done = 0; done < frames;) {
    uint32_t n = frames - done < chunk ? frames - done : chunk;
    v2_render(synth, pcm + (size_t)done * 2, n);
    done += n;
  }

  int ok = write_file(argv[i + 1], pcm, (size_t)frames * 2 * sizeof(float));
  if (!ok) perror(argv[i + 1]);
  free(pcm);
  v2_destroy(synth);
  return ok ? 0 : 1;
}
