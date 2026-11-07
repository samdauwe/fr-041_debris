/* Second translation unit of the linkage suite: the headers WITHOUT their implementation. */

#undef V2_STATIC
#undef WZ3_STATIC

#include <debris/core/v2.h>
#include <debris/core/wz3.h>

int linkage_v2_check(void)
{
  v2_synth *s = v2_create(48000);
  float buf[16] = { 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f };
  int silent = 1;
  if (!s) return 0;
  v2_render(s, buf, 8);
  for (int i = 0; i < 16; i++)
    if (buf[i] != 0.0f) silent = 0;
  v2_destroy(s);
  return silent && v2_sample_rate(NULL) == 0;
}

int linkage_wz3_check(void)
{
  wz3_rng r;
  wz3_rng_init(&r);
  wz3_rng_seed(&r, 1);
  const uint32_t a = wz3_rng_u32(&r);
  wz3_rng_seed(&r, 1);
  if (wz3_rng_u32(&r) != a) return 1;

  wz3_bitmap *b = wz3_bitmap_flat(NULL, 2, 2, 0xff000000u);
  if (!b || b->width != 4 || b->height != 4) {
    wz3_bitmap_destroy(b);
    return 2;
  }
  wz3_bitmap *c = wz3_bitmap_color(b, 0, 0xffffffffu);
  const int ok = c && wz3_bitmap_hash(c) != 0;
  wz3_bitmap_destroy(c);
  wz3_bitmap_destroy(b);
  if (!ok) return 3;
  return wz3_class_lookup(0x021) ? 0 : 4;
}
