/* ======================================================================== */
/*  texture preparation: rygdxt BC3 codec                                   */
/*                                                                          */
/*  Mirrors vendor/wz3/base/_rygdxt.cpp quality 0 (production only). The    */
/*  float steps keep the source's single-precision rounding points.         */
/* ======================================================================== */

/* OMatch5/OMatch6: best {max, min} endpoint pair per 8-bit value, generated
   by tools/gen_bc3_tables.mjs of the reference repository and re-derived by the unit tests. */
static const uint8_t wz3__bc3_match5[512] = {
  0, 0, 0, 0, 0, 1, 0, 1, 1, 0, 1, 0, 1, 0, 1, 1, 1, 1, 1, 1, 2, 0, 2, 0,
  2, 0, 2, 1, 0, 5, 3, 0, 3, 0, 3, 0, 3, 1, 3, 1, 3, 1, 3, 2, 4, 0, 4, 0,
  3, 3, 4, 1, 5, 0, 5, 0, 5, 0, 5, 1, 5, 1, 5, 1, 3, 6, 6, 0, 6, 0, 6, 1,
  4, 5, 7, 0, 7, 0, 7, 0, 7, 1, 7, 1, 7, 1, 7, 2, 8, 0, 8, 0, 7, 3, 8, 1,
  9, 0, 9, 0, 9, 0, 9, 1, 9, 1, 9, 1, 7, 6, 10, 0, 10, 0, 10, 1, 8, 5, 11, 0,
  11, 0, 11, 0, 11, 1, 11, 1, 11, 1, 11, 2, 12, 0, 12, 0, 11, 3, 12, 1, 13, 0, 13, 0,
  13, 0, 13, 1, 13, 1, 13, 1, 11, 6, 14, 0, 14, 0, 14, 1, 12, 5, 15, 0, 15, 0, 15, 0,
  15, 1, 15, 1, 15, 1, 15, 2, 16, 0, 16, 0, 15, 3, 16, 1, 17, 0, 17, 0, 17, 0, 17, 1,
  17, 1, 17, 1, 15, 6, 18, 0, 18, 0, 18, 1, 16, 5, 19, 0, 19, 0, 19, 0, 19, 1, 19, 1,
  19, 1, 19, 2, 20, 0, 20, 0, 19, 3, 20, 1, 21, 0, 21, 0, 21, 0, 21, 1, 21, 1, 21, 1,
  19, 6, 22, 0, 22, 0, 22, 1, 20, 5, 23, 0, 23, 0, 23, 0, 23, 1, 23, 1, 23, 1, 23, 2,
  24, 0, 24, 0, 23, 3, 24, 1, 25, 0, 25, 0, 25, 0, 25, 1, 25, 1, 25, 1, 23, 6, 26, 0,
  26, 0, 26, 1, 24, 5, 27, 0, 27, 0, 27, 0, 27, 1, 27, 1, 27, 1, 27, 2, 28, 0, 28, 0,
  27, 3, 28, 1, 29, 0, 29, 0, 29, 0, 29, 1, 29, 1, 29, 1, 27, 6, 30, 0, 30, 0, 30, 1,
  28, 5, 31, 0, 31, 0, 31, 0, 31, 1, 31, 1, 31, 1, 31, 2, 30, 4, 31, 3, 31, 3, 28, 9,
  31, 4, 31, 4, 31, 4, 31, 5, 31, 5, 31, 5, 31, 6, 30, 8, 31, 7, 31, 7, 28, 13, 31, 8,
  31, 8, 31, 8, 31, 9, 31, 9, 31, 9, 31, 10, 30, 12, 31, 11, 31, 11, 28, 17, 31, 12, 31, 12,
  31, 12, 31, 13, 31, 13, 31, 13, 31, 14, 30, 16, 31, 15, 31, 15, 28, 21, 31, 16, 31, 16, 31, 16,
  31, 17, 31, 17, 31, 17, 31, 18, 30, 20, 31, 19, 31, 19, 28, 25, 31, 20, 31, 20, 31, 20, 31, 21,
  31, 21, 31, 21, 31, 22, 30, 24, 31, 23, 31, 23, 28, 29, 31, 24, 31, 24, 31, 24, 31, 25, 31, 25,
  31, 25, 31, 26, 30, 28, 31, 27, 31, 27, 31, 27, 31, 28, 31, 28, 31, 28, 31, 29, 31, 29, 31, 29,
  31, 30, 31, 30, 31, 31, 31, 31,
};
static const uint8_t wz3__bc3_match6[512] = {
  0, 0, 0, 1, 1, 0, 1, 0, 1, 1, 2, 0, 2, 0, 2, 1, 3, 0, 3, 1, 4, 0, 4, 0,
  4, 1, 5, 0, 5, 0, 5, 1, 6, 0, 6, 1, 7, 0, 7, 0, 7, 1, 8, 0, 0, 16, 8, 1,
  9, 0, 9, 1, 1, 17, 10, 0, 10, 1, 11, 0, 3, 16, 11, 1, 12, 0, 12, 1, 4, 17, 13, 0,
  13, 1, 14, 0, 6, 16, 14, 1, 15, 0, 15, 1, 7, 17, 16, 0, 15, 3, 16, 1, 17, 0, 17, 1,
  15, 6, 18, 0, 18, 1, 19, 0, 15, 9, 19, 1, 20, 0, 20, 1, 15, 12, 21, 0, 21, 1, 22, 0,
  15, 15, 22, 1, 23, 0, 23, 1, 15, 18, 24, 0, 24, 1, 25, 0, 17, 16, 25, 1, 26, 0, 26, 1,
  18, 17, 27, 0, 27, 1, 28, 0, 20, 16, 28, 1, 29, 0, 29, 1, 21, 17, 30, 0, 30, 1, 31, 0,
  23, 16, 31, 1, 31, 2, 32, 0, 32, 1, 33, 0, 31, 5, 33, 1, 34, 0, 34, 1, 31, 8, 35, 0,
  35, 1, 36, 0, 31, 11, 36, 1, 37, 0, 37, 1, 31, 14, 38, 0, 38, 1, 39, 0, 31, 17, 39, 1,
  40, 0, 40, 1, 32, 17, 41, 0, 41, 1, 42, 0, 34, 16, 42, 1, 43, 0, 43, 1, 35, 17, 44, 0,
  44, 1, 45, 0, 37, 16, 45, 1, 46, 0, 46, 1, 38, 17, 47, 0, 47, 1, 47, 2, 48, 0, 48, 1,
  47, 4, 49, 0, 49, 1, 50, 0, 47, 7, 50, 1, 51, 0, 51, 1, 47, 10, 52, 0, 52, 1, 53, 0,
  47, 13, 53, 1, 54, 0, 54, 1, 47, 16, 55, 0, 55, 1, 56, 0, 48, 16, 56, 1, 57, 0, 57, 1,
  49, 17, 58, 0, 58, 1, 59, 0, 51, 16, 59, 1, 60, 0, 60, 1, 52, 17, 61, 0, 61, 1, 62, 0,
  54, 16, 62, 1, 63, 0, 63, 1, 55, 17, 63, 2, 63, 3, 63, 4, 57, 16, 63, 5, 63, 6, 63, 7,
  58, 17, 63, 8, 63, 9, 63, 10, 60, 16, 63, 11, 63, 12, 63, 13, 61, 17, 63, 14, 63, 15, 54, 33,
  63, 16, 63, 17, 63, 18, 56, 32, 63, 19, 63, 20, 63, 21, 57, 33, 63, 22, 63, 23, 63, 24, 59, 32,
  63, 25, 63, 26, 63, 27, 60, 33, 63, 28, 63, 29, 63, 30, 62, 32, 63, 31, 63, 32, 55, 48, 63, 33,
  63, 34, 63, 35, 56, 49, 63, 36, 63, 37, 63, 38, 58, 48, 63, 39, 63, 40, 63, 41, 59, 49, 63, 42,
  63, 43, 63, 44, 61, 48, 63, 45, 63, 46, 63, 47, 62, 49, 63, 48, 63, 49, 63, 49, 63, 50, 63, 51,
  63, 52, 63, 52, 63, 53, 63, 54, 63, 55, 63, 55, 63, 56, 63, 57, 63, 58, 63, 58, 63, 59, 63, 60,
  63, 61, 63, 61, 63, 62, 63, 63,
};

typedef struct wz3__bc3 {
  int32_t colors[12];      /* four palette entries as B,G,R */
  int32_t endpoints[2];    /* max, min as RGB565 */
} wz3__bc3;

static inline int32_t wz3__bc3_mul8(int32_t a, int32_t b)
{
  const int32_t t = a * b + 128;
  return (t + (t >> 8)) >> 8;
}

/* arithmetic shift right regardless of the implementation's signed shift */
static inline int32_t wz3__bc3_asr(int32_t x, int n)
{
  return x >= 0 ? x >> n : ~(~x >> n);
}

/* Recomputes a match table (size 32 -> shifts 3,2; size 64 -> shifts 2,4). */
static inline void wz3__bc3_match_table(uint8_t out[512], int size, int shift_a, int shift_b)
{
  uint8_t expand[64];
  for (int i = 0; i < size; i++) expand[i] = (uint8_t)((i << shift_a) | (i >> shift_b));
  for (int value = 0; value < 256; value++) {
    int best = 256;
    for (int min = 0; min < size; min++) {
      for (int max = 0; max < size; max++) {
        int err = expand[max] + wz3__bc3_mul8(expand[min] - expand[max], 0x55) - value;
        if (err < 0) err = -err;
        if (err < best) {
          out[value * 2] = (uint8_t)max;
          out[value * 2 + 1] = (uint8_t)min;
          best = err;
        }
      }
    }
  }
}

static inline int32_t wz3__bc3_as565(const uint8_t *px)
{
  return (wz3__bc3_mul8(px[2], 31) << 11) | (wz3__bc3_mul8(px[1], 63) << 5) | wz3__bc3_mul8(px[0], 31);
}

static void wz3__bc3_evaluate(wz3__bc3 *st, int32_t c0, int32_t c1)
{
  int32_t *c = st->colors;
  c[0] = (c0 & 31) << 3 | (c0 & 31) >> 2;
  c[1] = ((c0 >> 5) & 63) << 2 | ((c0 >> 5) & 63) >> 4;
  c[2] = ((c0 >> 11) & 31) << 3 | ((c0 >> 11) & 31) >> 2;
  c[3] = (c1 & 31) << 3 | (c1 & 31) >> 2;
  c[4] = ((c1 >> 5) & 63) << 2 | ((c1 >> 5) & 63) >> 4;
  c[5] = ((c1 >> 11) & 31) << 3 | ((c1 >> 11) & 31) >> 2;
  for (int ch = 0; ch < 3; ch++) {
    c[6 + ch] = c[ch] + wz3__bc3_mul8(c[3 + ch] - c[ch], 0x55);
    c[9 + ch] = c[ch] + wz3__bc3_mul8(c[3 + ch] - c[ch], 0xaa);
  }
}

static inline float wz3__bc3_fabs(float v) { return v < 0.0f ? -v : v; }

static void wz3__bc3_optimize(wz3__bc3 *st, const uint8_t *block)
{
  int32_t mu[3], mn[3], mx[3];
  for (int ch = 0; ch < 3; ch++) {
    int32_t sum = block[ch], lo = block[ch], hi = block[ch];
    for (int i = 4; i < 64; i += 4) {
      const int32_t v = block[i + ch];
      sum += v;
      if (v < lo) lo = v;
      if (v > hi) hi = v;
    }
    mu[ch] = (sum + 8) >> 4;
    mn[ch] = lo;
    mx[ch] = hi;
  }
  int32_t cov[6] = {0, 0, 0, 0, 0, 0};
  for (int i = 0; i < 16; i++) {
    const int32_t r = block[i * 4 + 2] - mu[2];
    const int32_t g = block[i * 4 + 1] - mu[1];
    const int32_t b = block[i * 4] - mu[0];
    cov[0] += r * r;
    cov[1] += r * g;
    cov[2] += r * b;
    cov[3] += g * g;
    cov[4] += g * b;
    cov[5] += b * b;
  }
  float covf[6];
  for (int i = 0; i < 6; i++) covf[i] = (float)cov[i] / 255.0f;
  float vr = (float)(mx[2] - mn[2]);
  float vg = (float)(mx[1] - mn[1]);
  float vb = (float)(mx[0] - mn[0]);
  for (int iter = 0; iter < 4; iter++) {
    const float r = vr * covf[0] + vg * covf[1] + vb * covf[2];
    const float g = vr * covf[1] + vg * covf[3] + vb * covf[4];
    const float b = vr * covf[2] + vg * covf[4] + vb * covf[5];
    vr = r; vg = g; vb = b;
  }
  float magn = wz3__bc3_fabs(vr);
  if (wz3__bc3_fabs(vg) > magn) magn = wz3__bc3_fabs(vg);
  if (wz3__bc3_fabs(vb) > magn) magn = wz3__bc3_fabs(vb);
  int32_t ar, ag, ab;
  if (magn < 4.0f) {
    ar = 148; ag = 300; ab = 58;
  } else {
    magn = 512.0f / magn;
    ar = wz3__ftoi((double)(vr * magn));
    ag = wz3__ftoi((double)(vg * magn));
    ab = wz3__ftoi((double)(vb * magn));
  }
  int32_t mind = 0x7fffffff, maxd = -0x7fffffff;
  int min_i = 0, max_i = 0;
  for (int i = 0; i < 16; i++) {
    const int32_t dot = block[i * 4 + 2] * ar + block[i * 4 + 1] * ag + block[i * 4] * ab;
    if (dot < mind) { mind = dot; min_i = i * 4; }
    if (dot > maxd) { maxd = dot; max_i = i * 4; }
  }
  st->endpoints[0] = wz3__bc3_as565(block + max_i);
  st->endpoints[1] = wz3__bc3_as565(block + min_i);
}

static uint32_t wz3__bc3_match(const wz3__bc3 *st, const uint8_t *block)
{
  const int32_t *c = st->colors;
  const int32_t db = c[0] - c[3], dg = c[1] - c[4], dr = c[2] - c[5];
  int32_t dots[16], stops[4];
  for (int i = 0; i < 16; i++) dots[i] = block[i * 4 + 2] * dr + block[i * 4 + 1] * dg + block[i * 4] * db;
  for (int i = 0; i < 4; i++) stops[i] = c[i * 3 + 2] * dr + c[i * 3 + 1] * dg + c[i * 3] * db;
  const int32_t c0_point = wz3__bc3_asr(stops[1] + stops[3], 1);
  const int32_t half_point = wz3__bc3_asr(stops[3] + stops[2], 1);
  const int32_t c3_point = wz3__bc3_asr(stops[2] + stops[0], 1);
  uint32_t mask = 0;
  for (int i = 15; i >= 0; i--) {
    mask <<= 2;
    const int32_t dot = dots[i];
    if (dot < half_point) mask |= dot < c0_point ? 1u : 3u;
    else mask |= dot < c3_point ? 2u : 0u;
  }
  return mask;
}

static inline int32_t wz3__bc3_solve(int32_t numerator, float factor, int32_t maximum)
{
  const float v = (float)numerator * factor + 0.5f;
  int32_t t = wz3__ftoi((double)v);
  if (t < 0) t = 0;
  if (t > maximum) t = maximum;
  return t;
}

static int wz3__bc3_refine(wz3__bc3 *st, const uint8_t *block, uint32_t mask)
{
  static const int32_t w1_tab[4] = {3, 0, 2, 1};
  static const int32_t prods[4] = {0x090000, 0x000900, 0x040102, 0x010402};
  int32_t acc = 0;
  int32_t a1r = 0, a1g = 0, a1b = 0, a2r = 0, a2g = 0, a2b = 0;
  uint32_t cm = mask;
  for (int i = 0; i < 16; i++, cm >>= 2) {
    const uint32_t step = cm & 3u;
    const int32_t w1 = w1_tab[step];
    const int32_t r = block[i * 4 + 2], g = block[i * 4 + 1], b = block[i * 4];
    acc += prods[step];
    a1r += w1 * r; a1g += w1 * g; a1b += w1 * b;
    a2r += r; a2g += g; a2b += b;
  }
  a2r = 3 * a2r - a1r;
  a2g = 3 * a2g - a1g;
  a2b = 3 * a2b - a1b;
  const int32_t xx = acc >> 16, yy = (acc >> 8) & 0xff, xy = acc & 0xff;
  if (!yy || !xx || xx * yy == xy * xy) return 0;
  const float frb = 3.0f * 31.0f / 255.0f / (float)(xx * yy - xy * xy);
  const float fg = frb * 63.0f / 31.0f;
  const int32_t old_min = st->endpoints[1], old_max = st->endpoints[0];
  st->endpoints[0] = (wz3__bc3_solve(a1r * yy - a2r * xy, frb, 31) << 11) |
                     (wz3__bc3_solve(a1g * yy - a2g * xy, fg, 63) << 5) |
                     wz3__bc3_solve(a1b * yy - a2b * xy, frb, 31);
  st->endpoints[1] = (wz3__bc3_solve(a2r * xx - a1r * xy, frb, 31) << 11) |
                     (wz3__bc3_solve(a2g * xx - a1g * xy, fg, 63) << 5) |
                     wz3__bc3_solve(a2b * xx - a1b * xy, frb, 31);
  return old_min != st->endpoints[1] || old_max != st->endpoints[0];
}

static void wz3__bc3_alpha_block(const uint8_t *block, uint8_t *out)
{
  int32_t lo = block[3], hi = block[3];
  for (int i = 1; i < 16; i++) {
    const int32_t a = block[i * 4 + 3];
    if (a < lo) lo = a;
    if (a > hi) hi = a;
  }
  *out++ = (uint8_t)hi;
  *out++ = (uint8_t)lo;
  const int32_t dist = hi - lo;
  const int32_t bias = lo * 7 - (dist >> 1);
  const int32_t dist4 = dist * 4, dist2 = dist * 2;
  int32_t bits = 0, mask = 0;
  for (int i = 0; i < 16; i++) {
    int32_t a = block[i * 4 + 3] * 7 - bias;
    int32_t t = (dist4 - a) < 0 ? -1 : 0;
    int32_t index = t & 4;
    a -= dist4 & t;
    t = (dist2 - a) < 0 ? -1 : 0;
    index += t & 2;
    a -= dist2 & t;
    t = (dist - a) < 0 ? -1 : 0;
    index += t & 1;
    index = (-index) & 7;
    index ^= (2 > index) ? 1 : 0;
    mask |= index << bits;
    bits += 3;
    if (bits >= 8) {
      *out++ = (uint8_t)(mask & 0xff);
      mask >>= 8;
      bits -= 8;
    }
  }
}

static void wz3__bc3_color_block(const uint8_t *block, uint8_t *out)
{
  wz3__bc3 st;
  int constant = 1;
  for (int i = 4; i < 64; i++) {
    if (block[i] != block[i & 3]) { constant = 0; break; }
  }
  uint32_t mask;
  if (!constant) {
    wz3__bc3_optimize(&st, block);
    if (st.endpoints[0] != st.endpoints[1]) {
      wz3__bc3_evaluate(&st, st.endpoints[0], st.endpoints[1]);
      mask = wz3__bc3_match(&st, block);
    } else {
      mask = 0;
    }
    if (wz3__bc3_refine(&st, block, mask)) {
      if (st.endpoints[0] != st.endpoints[1]) {
        wz3__bc3_evaluate(&st, st.endpoints[0], st.endpoints[1]);
        mask = wz3__bc3_match(&st, block);
      } else {
        mask = 0;
      }
    }
  } else {
    const int r = block[2], g = block[1], b = block[0];
    mask = 0xaaaaaaaau;
    st.endpoints[0] = (wz3__bc3_match5[r * 2] << 11) | (wz3__bc3_match6[g * 2] << 5) | wz3__bc3_match5[b * 2];
    st.endpoints[1] = (wz3__bc3_match5[r * 2 + 1] << 11) | (wz3__bc3_match6[g * 2 + 1] << 5) |
                      wz3__bc3_match5[b * 2 + 1];
  }
  int32_t mx = st.endpoints[0], mn = st.endpoints[1];
  if (mx < mn) {
    const int32_t t = mx; mx = mn; mn = t;
    mask ^= 0x55555555u;
  }
  out[0] = (uint8_t)(mx & 0xff); out[1] = (uint8_t)(mx >> 8);
  out[2] = (uint8_t)(mn & 0xff); out[3] = (uint8_t)(mn >> 8);
  out[4] = (uint8_t)(mask & 0xff); out[5] = (uint8_t)((mask >> 8) & 0xff);
  out[6] = (uint8_t)((mask >> 16) & 0xff); out[7] = (uint8_t)(mask >> 24);
}

WZ3_API void wz3_bc3_compress_block(const uint8_t bgra[64], uint8_t out[16])
{
  wz3__bc3_alpha_block(bgra, out);
  wz3__bc3_color_block(bgra, out + 8);
}

WZ3_API void wz3_bc3_decompress_block(const uint8_t in[16], uint8_t rgba[64])
{
  wz3__bc3 st;
  int32_t alpha[8];
  const int32_t a0 = in[0], a1 = in[1];
  alpha[0] = a0;
  alpha[1] = a1;
  if (a0 > a1) {
    for (int i = 1; i <= 6; i++) alpha[i + 1] = ((7 - i) * a0 + i * a1 + 3) / 7;
  } else {
    for (int i = 1; i <= 4; i++) alpha[i + 1] = ((5 - i) * a0 + i * a1 + 2) / 5;
    alpha[6] = 0;
    alpha[7] = 255;
  }
  const uint32_t alpha_low = (uint32_t)in[2] | ((uint32_t)in[3] << 8) | ((uint32_t)in[4] << 16);
  const uint32_t alpha_high = (uint32_t)in[5] | ((uint32_t)in[6] << 8) | ((uint32_t)in[7] << 16);
  wz3__bc3_evaluate(&st, in[8] | (in[9] << 8), in[10] | (in[11] << 8));
  const uint32_t cmask = (uint32_t)in[12] | ((uint32_t)in[13] << 8) | ((uint32_t)in[14] << 16) |
                         ((uint32_t)in[15] << 24);
  for (int i = 0; i < 16; i++) {
    const uint32_t ci = (cmask >> (i * 2)) & 3u;
    const uint32_t bits = i < 8 ? alpha_low : alpha_high;
    const uint32_t ai = (bits >> ((i & 7) * 3)) & 7u;
    rgba[i * 4] = (uint8_t)st.colors[ci * 3 + 2];
    rgba[i * 4 + 1] = (uint8_t)st.colors[ci * 3 + 1];
    rgba[i * 4 + 2] = (uint8_t)st.colors[ci * 3];
    rgba[i * 4 + 3] = (uint8_t)alpha[ai];
  }
}

