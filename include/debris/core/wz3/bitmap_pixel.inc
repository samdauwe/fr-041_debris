/* ======================================================================== */
/*  GenBitmap: pixel arithmetic                                             */
/*                                                                          */
/*  The source keeps a pixel in one sU64 and runs MMX word instructions on  */
/*  it; every lane operation is spelled out here (pmulhw, paddsw, ...).     */
/* ======================================================================== */

#define WZ3__MAX  0x7fff
#define WZ3__HALF 0x4000

static inline int32_t wz3__s16(uint32_t v) { return (int16_t)(uint16_t)v; }
static inline int32_t wz3__sat16(int32_t v) { return v < -0x8000 ? -0x8000 : v > 0x7fff ? 0x7fff : v; }
static inline int32_t wz3__mulhi16(uint32_t a, uint32_t b) { return (wz3__s16(a) * wz3__s16(b)) >> 16; }
static inline uint32_t wz3__w16(int32_t v) { return (uint32_t)v & 0xffffu; }

static inline int32_t wz3__clamp15i(int32_t v) { return v < 0 ? 0 : v > WZ3__MAX ? WZ3__MAX : v; }

/* clamp15 of a real: truncates toward zero first */
static inline int32_t wz3__clamp15d(double v)
{
  double t = trunc(v);
  return !(t > 0.0) ? 0 : t > (double)WZ3__MAX ? WZ3__MAX : (int32_t)t;
}

static inline int32_t wz3__fixed_mul(int32_t a, int32_t b)
{
  return (int32_t)(((int64_t)a * (int64_t)b) >> 16); /* sMulShift */
}

static inline int32_t wz3__power_of_two(uint32_t v)
{
  int32_t n = 0;
  while (v >>= 1) n++;
  return n;
}

WZ3_API void wz3_packed_color(uint32_t color, uint16_t out[4])
{
  out[0] = (uint16_t)((((color >> 16) & 255u) * 257u) >> 1);
  out[1] = (uint16_t)((((color >> 8) & 255u) * 257u) >> 1);
  out[2] = (uint16_t)(((color & 255u) * 257u) >> 1);
  out[3] = (uint16_t)(((color >> 24) * 257u) >> 1);
}

/* Fade64 lane: two signed 16-bit weights from the signed 32-bit fade, pmulhw,
   word add, then doubling. The one-LSB loss at both endpoints is intentional. */
static inline uint16_t wz3__fade_channel(uint32_t a, uint32_t b, int32_t fade)
{
  int32_t f = fade >> 1;
  int32_t weight_b = wz3__sat16(f);
  int32_t weight_a = wz3__sat16(0x8000 - f);
  uint32_t sum = wz3__w16(wz3__mulhi16(a, (uint32_t)weight_a) + wz3__mulhi16(b, (uint32_t)weight_b));
  return (uint16_t)((sum << 1) & 0xffffu);
}

static inline void wz3__fade_pixel(uint16_t *out, const uint16_t *a, const uint16_t *b, int32_t fade)
{
  for (int c = 0; c < 4; c++) out[c] = wz3__fade_channel(a[c], b[c], fade);
}

static inline void wz3__add_scale_pixel(uint16_t *out, const uint16_t *base, const uint16_t *scale, int32_t fade)
{
  int32_t weight = wz3__sat16(fade >> 1);
  for (int c = 0; c < 4; c++) {
    int32_t value = wz3__s16(base[c]) + wz3__mulhi16(scale[c], (uint32_t)weight) * 2;
    out[c] = (uint16_t)wz3__w16(wz3__sat16(value));
  }
}

/* BI_ALPHA/BI_GRAY: the MMX kernel halves the two adjacent word sums first. */
static inline uint16_t wz3__gray_mmx(uint32_t r, uint32_t g, uint32_t b)
{
  uint32_t rg = ((r + g) & 0xffffu) >> 1;
  uint32_t gb = ((g + b) & 0xffffu) >> 1;
  return (uint16_t)(((rg + gb) & 0xffffu) >> 1);
}

/* gammaLookup: GetGamma's arithmetic-shift interpolation over a table. */
static inline int32_t wz3__gamma_lookup(const int32_t *table, int32_t length, uint32_t value, int shift)
{
  int32_t index = (int32_t)(value >> shift);
  int32_t fraction = (int32_t)(value & ((1u << shift) - 1u));
  int32_t a = table[index < length - 1 ? index : length - 1];
  int32_t b = table[index + 1 < length - 1 ? index + 1 : length - 1];
  return a + (((b - a) * fraction) >> shift);
}

