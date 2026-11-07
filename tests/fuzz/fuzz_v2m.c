/* libFuzzer harness for v2.h: open a mutated v2m, play it from a mutated offset and render. The synth must neither
 * crash, hang nor allocate on the render path whatever the song says.
 *
 *   ./fuzz_v2m -max_len=300000 -timeout=20 corpus/ seed/   (seeds: assets/debris_loader.v2m, the extracted party song)
 */
#define V2_IMPLEMENTATION
#include <debris/core/v2.h>

#include <stdint.h>
#include <stdlib.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
  v2_synth *s = v2_create(44100);
  if (!s) return 0;
  if (v2_open(s, data, size) == V2_OK) {
    static float out[2048 * 2];
    (void)v2_length_frames(s);
    v2_play(s, size > 4 ? (uint32_t)data[size - 1] * 400u : 0);
    for (int i = 0; i < 24; i++) v2_render(s, out, 2048);
    v2_stop(s, 100);
    v2_render(s, out, 2048);
  }
  v2_destroy(s);
  return 0;
}
