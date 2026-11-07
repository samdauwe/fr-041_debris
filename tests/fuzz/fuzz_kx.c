/* libFuzzer harness for the document path of wz3.h: KX parse, then (for documents the parser accepts) a runtime with
 * frame jobs, its init pass, a few frames, a snapshot round trip and the effect advance.
 *
 *   cmake -DDEBRIS_BUILD_FUZZERS=ON (clang), then
 *   ./fuzz_kx -max_len=600000 -timeout=20 -rss_limit_mb=4000 corpus/ seed/   (seed: assets/debris_party.kx)
 */
#define WZ3_IMPLEMENTATION
#include <debris/core/wz3.h>

#include <stdint.h>
#include <stdlib.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
  wz3_kx *kx = NULL;
  if (wz3_kx_parse(&kx, data, size, NULL) != WZ3_OK) return 0;
  wz3_rt_options o = { 0 };
  o.release_consumed = 1;
  o.frame_jobs = 1;
  o.geometry = WZ3_GEOMETRY_PLACEHOLDER;
  wz3_result res;
  wz3_rt *rt = wz3_rt_create(kx, &o, &res);
  if (rt) {
    if (wz3_rt_precalc(rt) == WZ3_OK) {
      uint32_t n;
      for (int i = 0; i < 4; i++) {
        if (wz3_rt_frame_at_sample(rt, i * 44100 / 3, 44100) != WZ3_OK) break;
        (void)wz3_rt_effects_advance(rt, &n);
      }
      const size_t sz = wz3_rt_snapshot_size(rt);
      void *buf = malloc(sz ? sz : 1);
      if (buf && wz3_rt_snapshot(rt, buf, sz) == WZ3_OK) (void)wz3_rt_restore(rt, buf, sz);
      free(buf);
    }
    wz3_rt_destroy(rt);
  }
  wz3_kx_destroy(kx);
  return 0;
}
