/* ======================================================================== */
/*  GenBitmap: objects                                                      */
/* ======================================================================== */

WZ3_API void wz3_bitmap_ctx_init(wz3_bitmap_ctx *ctx)
{
  memset(ctx, 0, sizeof *ctx);
  ctx->default_format = 1;
  ctx->text = wz3_text_default;
}

static int32_t wz3__ctx_offset(const wz3_bitmap_ctx *ctx) { return ctx ? ctx->texture_offset : 0; }
static int32_t wz3__ctx_format(const wz3_bitmap_ctx *ctx) { return ctx ? ctx->default_format : 1; }

WZ3_API wz3_bitmap *wz3_bitmap_create(int32_t width, int32_t height)
{
  if (width < 1 || height < 1 || (int64_t)width * height > ((int64_t)1 << 26))
    return NULL;
  wz3_bitmap *b = (wz3_bitmap *)WZ3_MALLOC(sizeof *b);
  if (!b) return NULL;
  size_t words = (size_t)width * (size_t)height * 4;
  b->data = (uint16_t *)WZ3_MALLOC(words * sizeof(uint16_t));
  if (!b->data) {
    WZ3_FREE(b);
    return NULL;
  }
  memset(b->data, 0, words * sizeof(uint16_t));
  b->width = width;
  b->height = height;
  b->format = 1;
  b->mip_count = b->mip_threshold = 0;
  b->stripped = 0;
  return b;
}

WZ3_API void wz3_bitmap_destroy(wz3_bitmap *b)
{
  if (!b) return;
  WZ3_FREE(b->data);
  WZ3_FREE(b);
}

static void wz3__copy_settings(wz3_bitmap *to, const wz3_bitmap *from)
{
  to->format = from->format;
  to->mip_count = from->mip_count;
  to->mip_threshold = from->mip_threshold;
  to->stripped = from->stripped;
}

WZ3_API wz3_bitmap *wz3_bitmap_copy(const wz3_bitmap *src)
{
  if (!src) return NULL;
  wz3_bitmap *b = wz3_bitmap_create(src->width, src->height);
  if (!b) return NULL;
  memcpy(b->data, src->data, (size_t)src->width * (size_t)src->height * 4 * sizeof(uint16_t));
  wz3__copy_settings(b, src);
  return b;
}

/* A same-shaped bitmap whose pixels the caller overwrites completely. */
static wz3_bitmap *wz3__blank_like(const wz3_bitmap *src)
{
  wz3_bitmap *b = wz3_bitmap_create(src->width, src->height);
  if (b) wz3__copy_settings(b, src);
  return b;
}

WZ3_API uint32_t wz3_bitmap_hash(const wz3_bitmap *b)
{
  uint32_t hash = 0x811c9dc5u;
  size_t n = (size_t)b->width * (size_t)b->height * 4;
  for (size_t i = 0; i < n; i++) {
    uint32_t v = b->data[i];
    hash = (hash ^ (v & 255u)) * 0x01000193u;
    hash = (hash ^ (v >> 8)) * 0x01000193u;
  }
  return hash;
}

static void wz3__bitmap_dims(const wz3_bitmap_ctx *ctx, uint32_t x_exponent, uint32_t y_exponent, int32_t *width, int32_t *height)
{
  int dont_scale = (x_exponent & 0x80u) != 0;
  int32_t xs = (int32_t)(x_exponent & 0x7fu);
  int32_t ys = (int32_t)y_exponent;
  if (!dont_scale) {
    int32_t offset = wz3__ctx_offset(ctx);
    xs += offset;
    ys += offset;
  }
  xs = xs < 0 ? 0 : xs > 12 ? 12 : xs;
  ys = ys < 0 ? 0 : ys > 12 ? 12 : ys;
  *width = 1 << xs;
  *height = 1 << ys;
}

static wz3_bitmap *wz3__new_bitmap(const wz3_bitmap_ctx *ctx, uint32_t x_exponent, uint32_t y_exponent)
{
  int32_t width, height;
  wz3__bitmap_dims(ctx, x_exponent, y_exponent, &width, &height);
  wz3_bitmap *b = wz3_bitmap_create(width, height);
  if (b) b->format = wz3__ctx_format(ctx);
  return b;
}

/* ---- bilinear / point sampling (BilinearSetup, BilinearFilter) ------------ */

typedef struct wz3__bilinear {
  const uint16_t *data;
  int32_t width, height, x_mask, y_mask, x_max, y_max;
  int clamp_x, clamp_y;   /* BilinearSetup's "border" bits: clamp instead of wrap */
} wz3__bilinear;

static void wz3__bilinear_init(wz3__bilinear *c, const wz3_bitmap *b, int32_t border, int32_t width, int32_t height)
{
  c->data = b->data;
  c->width = width;
  c->height = height;
  c->x_mask = width - 1;
  c->y_mask = height - 1;
  c->x_max = (int32_t)((uint32_t)width * 0x10000u - 1u);
  c->y_max = (int32_t)((uint32_t)height * 0x10000u - 1u);
  c->clamp_x = (border & 1) != 0;
  c->clamp_y = (border & 2) != 0;
}

static inline int32_t wz3__bilinear_coord(int32_t v, int32_t max, int clamp)
{
  if (!clamp) return v & max;
  return v < 0 ? 0 : v > max ? max : v;
}

/* The MMX kernel drops the low fraction bit before pmulhw; its word-delta
   interpolation differs from real-valued bilinear by up to a word. */
static void wz3__bilinear_sample(const wz3__bilinear *c, int32_t u, int32_t v, uint16_t *out)
{
  u = wz3__bilinear_coord(u, c->x_max, c->clamp_x);
  v = wz3__bilinear_coord(v, c->y_max, c->clamp_y);
  int32_t x0 = (int32_t)((uint32_t)u >> 16);
  int32_t y0 = (int32_t)((uint32_t)v >> 16);
  int32_t x1 = (x0 + 1) & c->x_mask;
  int32_t y1 = (y0 + 1) & c->y_mask;
  int32_t fu = (u & 0xffff) >> 1;
  int32_t fv = (v & 0xffff) >> 1;
  const uint16_t *p00 = c->data + ((size_t)y0 * c->width + x0) * 4;
  const uint16_t *p10 = c->data + ((size_t)y0 * c->width + x1) * 4;
  const uint16_t *p01 = c->data + ((size_t)y1 * c->width + x0) * 4;
  const uint16_t *p11 = c->data + ((size_t)y1 * c->width + x1) * 4;
  for (int ch = 0; ch < 4; ch++) {
    int32_t top = p00[ch], bottom = p01[ch];
    int32_t top_delta = (int16_t)(uint16_t)(p10[ch] - top);
    int32_t bottom_delta = (int16_t)(uint16_t)(p11[ch] - bottom);
    int32_t h0 = (int32_t)wz3__w16(top + ((top_delta * fu) >> 16) * 2);
    int32_t h1 = (int32_t)wz3__w16(bottom + ((bottom_delta * fu) >> 16) * 2);
    /* paddw 0x8000 then psubusw 0x8000 clamps the rare negative underflow */
    int32_t vertical_delta = (int16_t)(uint16_t)(h1 - h0);
    int32_t biased = (int32_t)wz3__w16(h0 + 0x8000 + ((vertical_delta * fv) >> 16) * 2);
    out[ch] = (uint16_t)(biased < 0x8000 ? 0 : biased - 0x8000);
  }
}

/* ---- Bitmap_Inner: per-pixel blend kernels ------------------------------- */

enum {
  WZ3__BI_ADD = 0, WZ3__BI_SUB, WZ3__BI_MUL, WZ3__BI_DIFF, WZ3__BI_ALPHA, WZ3__BI_MULCOL, WZ3__BI_ADDCOL,
  WZ3__BI_SUBCOL, WZ3__BI_GRAY, WZ3__BI_INVERT, WZ3__BI_SCALECOL, WZ3__BI_MERGE, WZ3__BI_BRIGHTNESS,
  WZ3__BI_SUBR, WZ3__BI_MULMERGE, WZ3__BI_SHARPEN, WZ3__BI_HARDLIGHT, WZ3__BI_OVER, WZ3__BI_ADDSMOOTH,
  WZ3__BI_MIN, WZ3__BI_MAX, WZ3__BI_RANGE
};

/* dst = f(dst, src, orig) over `count` pixels. src/orig advance by 4 words per
   pixel, or stay put (stride 0) when they are constants. k0/k1 are the colour
   constants of the *COL and RANGE modes. Pixel-local aliasing of dst with src
   or orig is intended: all words of a pixel are read before any is written. */
static void wz3__inner(uint16_t *dst, const uint16_t *src, int src_stride, int mode,
                       const uint16_t *orig, int orig_stride, size_t count,
                       const uint16_t *k0, const uint16_t *k1)
{
  int32_t range_difference[4] = { 0, 0, 0, 0 };
  if (mode == WZ3__BI_RANGE)
    for (int c = 0; c < 4; c++) range_difference[c] = wz3__sat16(wz3__s16(k1[c]) - wz3__s16(k0[c]));

  for (size_t i = 0; i < count; i++) {
    uint32_t d[4], s[4], x[4], out[4];
    for (int c = 0; c < 4; c++) {
      d[c] = dst[i * 4 + c];
      s[c] = src[(size_t)src_stride * i + c];
      x[c] = orig[(size_t)orig_stride * i + c];
    }
    for (int c = 0; c < 4; c++) {
      switch (mode) {
      case WZ3__BI_ADD:     out[c] = wz3__w16(wz3__sat16(wz3__s16(x[c]) + wz3__s16(s[c]))); break;
      case WZ3__BI_SUB:     out[c] = x[c] > s[c] ? x[c] - s[c] : 0; break;
      case WZ3__BI_MUL:     out[c] = wz3__w16(wz3__mulhi16(x[c], s[c]) * 2); break;
      case WZ3__BI_DIFF:    out[c] = ((wz3__w16((int32_t)(x[c] - s[c])) + WZ3__MAX) & 0xffffu) >> 1; break;
      case WZ3__BI_MULCOL:  out[c] = wz3__w16(wz3__mulhi16(x[c], k0[c]) * 2); break;
      case WZ3__BI_ADDCOL:  out[c] = wz3__w16(wz3__sat16(wz3__s16(x[c]) + wz3__s16(k0[c]))); break;
      case WZ3__BI_SUBCOL:  out[c] = x[c] > k0[c] ? x[c] - k0[c] : 0; break;
      case WZ3__BI_INVERT:  out[c] = x[c] ^ WZ3__MAX; break;
      case WZ3__BI_SCALECOL: {
        /* the source reconstructs the signed product and shifts it logically,
           so a negative product saturates to 0x7fff */
        uint32_t product = (uint32_t)(wz3__s16(x[c]) * wz3__s16(k0[c]));
        uint32_t v = product >> 11;
        out[c] = v < WZ3__MAX ? v : WZ3__MAX;
        break;
      }
      case WZ3__BI_MERGE: {
        int32_t inverse = wz3__sat16(WZ3__MAX - wz3__s16(d[c]));
        int32_t sum = wz3__sat16(wz3__mulhi16(d[c], s[c]) + wz3__mulhi16((uint32_t)inverse, x[c]));
        out[c] = wz3__w16(sum * 2);
        break;
      }
      case WZ3__BI_SUBR:    out[c] = s[c] > x[c] ? s[c] - x[c] : 0; break;
      case WZ3__BI_MULMERGE: {
        uint32_t multiplied = wz3__w16(wz3__mulhi16(s[c], x[c]) * 2);
        int32_t inverse = wz3__sat16(WZ3__MAX - wz3__s16(d[c]));
        int32_t sum = wz3__sat16(wz3__mulhi16(d[c], multiplied) + wz3__mulhi16((uint32_t)inverse, x[c]));
        out[c] = wz3__w16(sum * 2);
        break;
      }
      case WZ3__BI_SHARPEN: {
        uint32_t difference = x[c] > d[c] ? x[c] - d[c] : 0;
        int32_t scaled = (wz3__s16(difference) * wz3__s16(k0[c])) >> 11;
        int32_t v = wz3__sat16(wz3__s16(x[c]) + scaled);
        out[c] = wz3__w16(v < 0 ? 0 : v);
        break;
      }
      case WZ3__BI_HARDLIGHT: {
        uint32_t doubled = (s[c] << 1) & 0xffffu;
        uint32_t sign_mask = wz3__s16(doubled) < 0 ? 0xffffu : 0u;
        uint32_t magnitude = doubled & WZ3__MAX;
        uint32_t value = wz3__w16(wz3__mulhi16(x[c], magnitude) * 2);
        value ^= sign_mask;
        value = (value + ((magnitude + x[c]) & sign_mask)) & 0xffffu;
        out[c] = value;
        break;
      }
      case WZ3__BI_OVER: {
        int32_t delta = wz3__sat16(wz3__s16(s[c]) - wz3__s16(x[c]));
        int32_t mixed = wz3__sat16(wz3__s16(x[c]) + wz3__mulhi16((uint32_t)delta, s[3]) * 2);
        out[c] = wz3__w16(mixed < 0 ? 0 : mixed);
        break;
      }
      case WZ3__BI_ADDSMOOTH:
        out[c] = wz3__w16(((wz3__mulhi16(x[c] ^ WZ3__MAX, s[c] ^ WZ3__MAX) * 2) ^ WZ3__MAX));
        break;
      case WZ3__BI_MIN:     out[c] = wz3__s16(x[c]) < wz3__s16(s[c]) ? x[c] : s[c]; break;
      case WZ3__BI_MAX:     out[c] = wz3__s16(x[c]) > wz3__s16(s[c]) ? x[c] : s[c]; break;
      case WZ3__BI_RANGE: {
        int32_t value = wz3__sat16(wz3__s16(k0[c]) + wz3__mulhi16(x[c], (uint32_t)range_difference[c]) * 2);
        out[c] = wz3__w16(value < 0 ? 0 : value);
        break;
      }
      default:              out[c] = x[c]; break;
      }
    }
    if (mode == WZ3__BI_ALPHA) {
      /* pand clears the high bit of the three retained colour words */
      out[0] = x[0] & WZ3__MAX; out[1] = x[1] & WZ3__MAX; out[2] = x[2] & WZ3__MAX;
      out[3] = wz3__gray_mmx(s[0], s[1], s[2]);
    } else if (mode == WZ3__BI_GRAY) {
      uint32_t gray = wz3__gray_mmx(x[0], x[1], x[2]);
      out[0] = out[1] = out[2] = gray;
      out[3] = WZ3__MAX;
    } else if (mode == WZ3__BI_BRIGHTNESS) {
      for (int c = 0; c < 4; c++) {
        uint32_t mask = wz3__s16(s[c]) > 0x3fff ? 0x7fffu : 0u;
        uint32_t product = wz3__w16(wz3__mulhi16(s[c] ^ mask, x[c] ^ mask) * 4);
        out[c] = product ^ mask;
      }
    }
    for (int c = 0; c < 4; c++) dst[i * 4 + c] = (uint16_t)out[c];
  }
}

static const uint16_t wz3__zero_pixel[4] = { 0, 0, 0, 0 };

/* ---- simple operators ------------------------------------------------------ */

WZ3_API wz3_bitmap *wz3_bitmap_flat(const wz3_bitmap_ctx *ctx, uint32_t xs, uint32_t ys, uint32_t color)
{
  wz3_bitmap *b = wz3__new_bitmap(ctx, xs, ys);
  if (!b) return NULL;
  uint16_t pixel[4];
  wz3_packed_color(color, pixel);
  size_t n = (size_t)b->width * (size_t)b->height;
  for (size_t i = 0; i < n; i++) memcpy(b->data + i * 4, pixel, sizeof pixel);
  return b;
}

WZ3_API wz3_bitmap *wz3_bitmap_format(const wz3_bitmap *in, int32_t format, int32_t mip_count, int32_t mip_threshold)
{
  wz3_bitmap *b = wz3_bitmap_copy(in);
  if (!b) return NULL;
  b->format = format;
  b->mip_count = mip_count;
  b->mip_threshold = mip_threshold;
  return b;
}

WZ3_API wz3_bitmap *wz3_bitmap_merge(int32_t mode, const wz3_bitmap *const *inputs, int32_t count)
{
  static const uint8_t modes[11] = {
    WZ3__BI_ADD, WZ3__BI_SUB, WZ3__BI_MUL, WZ3__BI_DIFF, WZ3__BI_ALPHA, WZ3__BI_BRIGHTNESS,
    WZ3__BI_HARDLIGHT, WZ3__BI_OVER, WZ3__BI_ADDSMOOTH, WZ3__BI_MIN, WZ3__BI_MAX
  };
  if (count < 1 || !inputs || !inputs[0]) return NULL;
  const wz3_bitmap *first = inputs[0];
  /* validate the whole list before producing anything */
  for (int32_t i = 1; i < count && inputs[i]; i++)
    if (inputs[i]->width != first->width || inputs[i]->height != first->height) return NULL;

  wz3_bitmap *b = wz3_bitmap_copy(first);
  if (!b) return NULL;
  size_t pixels = (size_t)b->width * (size_t)b->height;
  const uint16_t *original = first->data;
  int inner_mode = (mode >= 0 && mode < 11) ? modes[mode] : WZ3__BI_ADD;
  for (int32_t i = 1; i < count; i++) {
    if (!inputs[i]) break; /* the vararg source treats the first null as the end */
    wz3__inner(b->data, inputs[i]->data, 4, inner_mode, original, 4, pixels, wz3__zero_pixel, wz3__zero_pixel);
    original = b->data;
  }
  return b;
}

WZ3_API wz3_bitmap *wz3_bitmap_color(const wz3_bitmap *in, int32_t mode, uint32_t color)
{
  if (!in) return NULL;
  wz3_bitmap *b = wz3__blank_like(in);
  if (!b) return NULL;
  uint16_t k[4];
  wz3_packed_color(color, k);
  wz3__inner(b->data, k, 0, mode + WZ3__BI_MULCOL, in->data, 4, (size_t)b->width * (size_t)b->height, k, k);
  return b;
}

WZ3_API wz3_bitmap *wz3_bitmap_range(const wz3_bitmap *in, int32_t mode, uint32_t color0, uint32_t color1)
{
  if (!in) return NULL;
  wz3_bitmap *b = wz3__blank_like(in);
  if (!b) return NULL;
  size_t pixels = (size_t)b->width * (size_t)b->height;
  const uint16_t *source = in->data;
  uint16_t c0[4], c1[4];
  wz3_packed_color(color0, c0);
  wz3_packed_color(color1, c1);
  if (mode & 1) {
    wz3__inner(b->data, b->data, 4, WZ3__BI_GRAY, source, 4, pixels, wz3__zero_pixel, wz3__zero_pixel);
    source = b->data;
  }
  if (mode & 2) {
    wz3__inner(b->data, b->data, 4, WZ3__BI_INVERT, source, 4, pixels, wz3__zero_pixel, wz3__zero_pixel);
    source = b->data;
  }
  wz3__inner(b->data, c0, 0, WZ3__BI_RANGE, source, 4, pixels, c0, c1);
  return b;
}

/* ---- float -> int conversions ---------------------------------------------- */

/* C's truncating float->int conversion; out of range gives x86's indefinite. */
static inline int32_t wz3__ftoi(double v)
{
  if (!(v > -2147483649.0 && v < 2147483648.0)) return INT32_MIN;
  return (int32_t)v;
}

/* ---- GlowRect ---------------------------------------------------------------- */

typedef struct wz3__glow {
  wz3_bitmap *bitmap;
  float rx, ry, sx, sy;
  int circular;
  uint16_t color[4];
  int32_t gamma[1025];
  int32_t low[32];
} wz3__glow;

static void wz3__glow_tables(wz3__glow *g, float power, float alpha15, int old_falloff)
{
  const float new_falloff_exponent = power * 2.0f;
  for (int32_t index = 0; index <= 1024; index++) {
    float coordinate = (float)index / 1024.0f;
    float strength = old_falloff
      ? (float)(wz3_pow((double)(1.0f - coordinate), (double)power) * (double)alpha15)
      : (float)(1.0 - wz3_pow((double)coordinate, (double)new_falloff_exponent)) * alpha15;
    g->gamma[index] = wz3__clamp15d(strength) * 2;
  }
  /* values close to zero have slopes too steep for the coarser table */
  for (int32_t index = 0; index < 32; index++) {
    float coordinate = (float)index / 32768.0f;
    float strength = old_falloff
      ? (float)(wz3_pow((double)(1.0f - coordinate), (double)power) * (double)alpha15)
      : (float)(1.0 - wz3_pow((double)coordinate, (double)new_falloff_exponent)) * alpha15;
    g->low[index] = wz3__clamp15d(strength) * 2;
  }
}

static void wz3__glow_paint(wz3__glow *g, float center_x, float center_y, int32_t wrap_mode)
{
  wz3_bitmap *bitmap = g->bitmap;
  const float rx = g->rx, ry = g->ry, sx = g->sx, sy = g->sy;
  if (wrap_mode == 1) {
    if ((center_x + rx) + sx > 1.0f) wz3__glow_paint(g, center_x - 1.0f, center_y, 2);
    if ((center_x - rx) - sx < 0.0f) wz3__glow_paint(g, center_x + 1.0f, center_y, 2);
  }
  if (wrap_mode == 1 || wrap_mode == 2) {
    if ((center_y + ry) + sy > 1.0f) wz3__glow_paint(g, center_x, center_y - 1.0f, 0);
    if ((center_y - ry) - sy < 0.0f) wz3__glow_paint(g, center_x, center_y + 1.0f, 0);
  }
  const int circular = g->circular;
  const float w = (float)bitmap->width, h = (float)bitmap->height;
  const float pcx = center_x * w, pcy = center_y * h;
  const float psx = sx * w, psy = sy * h;
  const float threshold = 1.0f / 65536.0f;
  const float rx_scaled = rx * w, ry_scaled = ry * h;
  const float prx0 = rx_scaled > threshold ? rx_scaled : threshold;
  const float pry0 = ry_scaled > threshold ? ry_scaled : threshold;
  const float prx = circular ? 1.0f / (prx0 * prx0) : 1.0f / prx0;
  const float pry = circular ? 1.0f / (pry0 * pry0) : 1.0f / pry0;
  /* a conservative pad skips rows/columns that cannot pass the coverage test */
  const double coverage_limit = 1.0 - 1.0 / 32768.0;
  const double radius_x = (double)prx0 * (circular ? sqrt(coverage_limit) : coverage_limit);
  const double radius_y = (double)pry0 * (circular ? sqrt(coverage_limit) : coverage_limit);
  const double x_start_d = floor((double)pcx - (double)psx - radius_x) - 1.0;
  const double x_end_d = ceil((double)pcx + (double)psx + radius_x) + 2.0;
  const double y_start_d = floor((double)pcy - (double)psy - radius_y) - 1.0;
  const double y_end_d = ceil((double)pcy + (double)psy + radius_y) + 2.0;
  const int32_t x_start = x_start_d > 0.0 ? (x_start_d > (double)bitmap->width ? bitmap->width : (int32_t)x_start_d) : 0;
  const int32_t y_start = y_start_d > 0.0 ? (y_start_d > (double)bitmap->height ? bitmap->height : (int32_t)y_start_d) : 0;
  const int32_t x_end = x_end_d < (double)bitmap->width ? (x_end_d > 0.0 ? (int32_t)x_end_d : 0) : bitmap->width;
  const int32_t y_end = y_end_d < (double)bitmap->height ? (y_end_d > 0.0 ? (int32_t)y_end_d : 0) : bitmap->height;

  for (int32_t y = y_start; y < y_end; y++) {
    float fy = (float)(fabs((double)y - (double)pcy) - (double)psy);
    if (fy < 0.0f) fy = 0.0f;
    fy = fy * (circular ? fy * pry : pry);
    for (int32_t x = x_start; x < x_end; x++) {
      float fx = (float)(fabs((double)x - (double)pcx) - (double)psx);
      if (fx < 0.0f) fx = 0.0f;
      float a;
      if (circular) a = ((fx * fx) * prx) + fy;
      else {
        float t = fx * prx;
        a = t > fy ? t : fy;
      }
      if ((double)a < coverage_limit) {
        uint16_t *p = bitmap->data + ((size_t)y * (size_t)bitmap->width + (size_t)x) * 4;
        int32_t gamma_index = wz3_ftol((double)(a * 32768.0f));
        int32_t fade = gamma_index < 32 ? g->low[gamma_index]
                                        : wz3__gamma_lookup(g->gamma, 1025, (uint32_t)gamma_index, 5);
        for (int c = 0; c < 4; c++) p[c] = wz3__fade_channel(p[c], g->color[c], fade);
      }
    }
  }
}

WZ3_API wz3_bitmap *wz3_bitmap_glowrect(const wz3_bitmap *in, float cx, float cy, float rx, float ry,
                                        float sx, float sy, uint32_t color, float alpha, float power,
                                        int32_t wrap, int32_t bug)
{
  if (!in) return NULL;
  wz3__glow *g = (wz3__glow *)WZ3_MALLOC(sizeof *g);
  wz3_bitmap *b = wz3_bitmap_copy(in);
  if (!g || !b) {
    WZ3_FREE(g);
    wz3_bitmap_destroy(b);
    return NULL;
  }
  g->bitmap = b;
  g->rx = rx; g->ry = ry; g->sx = sx; g->sy = sy;
  g->circular = (bug & 2) == 0;
  float curve_power = power == 0.0f ? 1.0f / 65536.0f : power;
  curve_power = (float)(0.25 / (double)curve_power);
  wz3_packed_color(color, g->color);
  float alpha15 = alpha * 32768.0f;
  wz3__glow_tables(g, curve_power, alpha15, (bug & 1) != 0);
  wz3__glow_paint(g, cx, cy, wrap);
  WZ3_FREE(g);
  return b;
}

/* ---- HSCB ------------------------------------------------------------------- */

WZ3_API wz3_bitmap *wz3_bitmap_hscb(const wz3_bitmap *in, float hue, float saturation, float contrast,
                                    float brightness)
{
  if (!in) return NULL;
  wz3_bitmap *b = wz3__blank_like(in);
  if (!b) return NULL;
  int32_t table[1026];
  const float exponent = contrast * contrast;
  for (int32_t i = 0; i <= 1025; i++) {
    /* the literal 0.01 is nominally double, but the player keeps the x87 at
       single precision: base, pow result and both products are floats */
    float coordinate = (float)((double)(i * 32) + 0.01) / 32768.0f;
    float curved = (float)wz3_pow((double)coordinate, (double)exponent);
    table[i] = wz3__ftoi((double)((curved * 32768.0f) * brightness));
  }
  const int32_t sixth = 6 * 65536;
  int32_t hue_fixed = wz3__ftoi((double)((hue * 6.0f) * 65536.0f)) % sixth;
  int32_t sat_fixed = wz3__ftoi((double)(saturation * 65536.0f));
  const int adjust = hue_fixed != 0 || sat_fixed != 65536;
  const uint16_t *source = in->data;
  const size_t n = (size_t)b->width * (size_t)b->height * 4;
  for (size_t i = 0; i < n; i += 4) {
    int32_t r = wz3__gamma_lookup(table, 1026, source[i], 5);
    int32_t g = wz3__gamma_lookup(table, 1026, source[i + 1], 5);
    int32_t bl = wz3__gamma_lookup(table, 1026, source[i + 2], 5);
    if (adjust) {
      int32_t maximum = r > g ? (r > bl ? r : bl) : (g > bl ? g : bl);
      int32_t minimum = r < g ? (r < bl ? r : bl) : (g < bl ? g : bl);
      int32_t range = maximum - minimum;
      int32_t h = 0;
      if (range) {
        if (r == maximum) h = 65536 + (int32_t)(((int64_t)(g - bl) * 65536) / range);
        else if (g == maximum) h = 3 * 65536 + (int32_t)(((int64_t)(bl - r) * 65536) / range);
        else h = 5 * 65536 + (int32_t)(((int64_t)(r - g) * 65536) / range);
      }
      h += hue_fixed;
      if (h > sixth) h -= sixth;
      range = wz3__fixed_mul(range, sat_fixed);
      int32_t min2 = maximum - range;
      int32_t rh = h & 131071;
      int32_t m1 = min2 + (rh >= 65536 ? wz3__fixed_mul(rh - 65536, range) : 0);
      int32_t m2 = min2 + (rh < 65536 ? wz3__fixed_mul(65536 - rh, range) : 0);
      if (h < 2 * 65536) { r = maximum; g = m1; bl = m2; }
      else if (h < 4 * 65536) { r = m2; g = maximum; bl = m1; }
      else { r = m1; g = m2; bl = maximum; }
    }
    b->data[i] = (uint16_t)wz3__clamp15i(r);
    b->data[i + 1] = (uint16_t)wz3__clamp15i(g);
    b->data[i + 2] = (uint16_t)wz3__clamp15i(bl);
    b->data[i + 3] = source[i + 3];
  }
  return b;
}

/* ---- Blur / Sharpen ------------------------------------------------------------ */

static inline uint16_t wz3__blur_scale(int32_t accumulator, uint32_t amp_low, uint32_t amp_high, int32_t clip)
{
  /* BlurCore rebuilds the high half of a 32x32 multiply from three 16-bit
     products; each paddusw saturates on its own */
  int32_t shifted = accumulator >> 6;
  uint32_t low = (uint32_t)shifted & 0xffffu, high = (uint32_t)shifted >> 16;
  uint32_t out = accumulator > clip ? 0xffffu : 0u;
  uint32_t t = out + ((high * amp_low) & 0xffffu);  out = t < 0xffffu ? t : 0xffffu;
  t = out + ((low * amp_high) & 0xffffu);           out = t < 0xffffu ? t : 0xffffu;
  t = out + ((low * amp_low) >> 16);                out = t < 0xffffu ? t : 0xffffu;
  return (uint16_t)(out < WZ3__MAX ? out : WZ3__MAX);
}

/* paddd/psubd wrap at 32 bits; pmaddwd treats all words as signed */
static inline int32_t wz3__madd(const uint16_t *a, int32_t f0, const uint16_t *b, int32_t f1, int ch)
{
  return (int32_t)((uint32_t)(wz3__s16(a[ch]) * f0) + (uint32_t)(wz3__s16(b[ch]) * f1));
}

static void wz3__blur_core(const uint16_t *src, uint16_t *dst, int32_t kernel_size, int32_t resolution,
                           int32_t f0, int32_t f1, int32_t amplitude, int32_t amplitude_clip)
{
  const int32_t mask = resolution - 1;
  int32_t entering = (-((kernel_size + 1) >> 1)) & mask;
  int32_t leaving = entering;
  int32_t edge = f1 - f0;
  int32_t acc[4];
  for (int c = 0; c < 4; c++) acc[c] = (int32_t)(uint32_t)(wz3__s16(src[(size_t)leaving * 4 + c]) * edge);

  for (int32_t i = 0; i < kernel_size; i++) {
    int32_t next = (entering + 1) & mask;
    for (int c = 0; c < 4; c++)
      acc[c] = (int32_t)((uint32_t)acc[c] + (uint32_t)wz3__madd(src + (size_t)entering * 4, f0, src + (size_t)next * 4, f1, c));
    entering = next;
  }

  const uint32_t amp_low = (uint32_t)amplitude & 0xffffu, amp_high = (uint32_t)amplitude >> 16;
  for (int32_t coordinate = 0; coordinate < resolution; coordinate++) {
    int32_t entering_next = (entering + 1) & mask;
    for (int c = 0; c < 4; c++) {
      acc[c] = (int32_t)((uint32_t)acc[c] + (uint32_t)wz3__madd(src + (size_t)entering * 4, f0,
                                                                  src + (size_t)entering_next * 4, f1, c));
      dst[(size_t)coordinate * 4 + c] = wz3__blur_scale(acc[c], amp_low, amp_high, amplitude_clip);
    }
    entering = entering_next;

    int32_t leaving_next = (leaving + 1) & mask;
    for (int c = 0; c < 4; c++)
      acc[c] = (int32_t)((uint32_t)acc[c] - (uint32_t)wz3__madd(src + (size_t)leaving_next * 4, f0,
                                                                  src + (size_t)leaving * 4, f1, c));
    leaving = leaving_next;
  }
}

WZ3_API wz3_bitmap *wz3_bitmap_blur(const wz3_bitmap *in, int32_t flags, float sx, float sy, float requested_amplitude)
{
  if (!in) return NULL;
  wz3_bitmap *b = wz3_bitmap_copy(in);
  if (!b) return NULL;
  const int32_t order = flags & 15;
  if (!order) return b;

  const size_t words = (size_t)b->width * (size_t)b->height * 4;
  uint16_t *scratch = (uint16_t *)WZ3_MALLOC(words * sizeof(uint16_t));
  if (!scratch) {
    wz3_bitmap_destroy(b);
    return NULL;
  }
  uint16_t *original = b->data;
  uint16_t *current = original, *other = scratch;
  int32_t width = b->width, height = b->height;
  /* arguments are sF32; sFtol then observes round-to-nearest-even after each
     single-precision multiply; the amplitude uses ordinary truncation */
  int32_t size = wz3_ftol((double)((128.0f * sx) * (float)width));
  const int32_t size2 = wz3_ftol((double)((128.0f * sy) * (float)height));
  const int32_t fixed_amplitude = wz3__ftoi((double)((requested_amplitude * 65536.0f) * 64.0f));

  for (int axis = 0; axis < 2; axis++) {
    int32_t f1 = (size & 127) / 2, f0 = 64 - f1;
    int32_t kernel_size = (size / 128) * 2;
    if (flags & 0x10) kernel_size++;
    int32_t divisor = kernel_size * 64 + f1 * 2;
    if (divisor < 1) divisor = 1;
    int32_t amplitude = fixed_amplitude / divisor;
    int32_t amplitude_clip = amplitude > 128 ? (int32_t)((((int64_t)1 << 38) / amplitude) - 1) : 0x7fffffff;

    uint16_t *row_source = current, *row_scratch = other;
    for (int32_t y = 0; y < height; y++) {
      const size_t row = (size_t)y * (size_t)width * 4;
      const uint16_t *pass_source = row_source + row;
      uint16_t *pass_destination = row_scratch + row;
      for (int32_t pass = 0; pass < order; pass++) {
        wz3__blur_core(pass_source, pass_destination, kernel_size, width, f0, f1, amplitude, amplitude_clip);
        const uint16_t *swap = pass_source;
        pass_source = pass_destination;
        pass_destination = (uint16_t *)swap;
      }
    }

    uint16_t *blurred = (order & 1) ? row_scratch : row_source;
    uint16_t *transposed = blurred == original ? scratch : original;
    for (int32_t y = 0; y < height; y++)
      for (int32_t x = 0; x < width; x++)
        memcpy(transposed + ((size_t)x * (size_t)height + (size_t)y) * 4,
               blurred + ((size_t)y * (size_t)width + (size_t)x) * 4, 4 * sizeof(uint16_t));
    current = transposed;
    other = blurred;
    int32_t t = width; width = height; height = t;
    size = size2;
  }

  /* two transposes with the same pass parity land back in the original buffer */
  if (current != original) memcpy(original, current, words * sizeof(uint16_t));
  WZ3_FREE(scratch);
  return b;
}

WZ3_API wz3_bitmap *wz3_bitmap_sharpen(const wz3_bitmap *in, int32_t order, float sx, float sy, float amplitude)
{
  if (!in) return NULL;
  wz3_bitmap *b = wz3_bitmap_blur(in, order | 0x10, sx, sy, 1.0f);
  if (!b) return NULL;
  int32_t value = wz3__ftoi((double)(amplitude * 2048.0f));
  value = value < -WZ3__MAX ? -WZ3__MAX : value > WZ3__MAX ? WZ3__MAX : value;
  uint16_t k[4];
  for (int c = 0; c < 4; c++) k[c] = (uint16_t)(int16_t)value;
  wz3__inner(b->data, k, 0, WZ3__BI_SHARPEN, in->data, 4, (size_t)b->width * (size_t)b->height, k, k);
  return b;
}

WZ3_API wz3_bitmap *wz3_bitmap_mask(const wz3_bitmap *mask, const wz3_bitmap *bb, const wz3_bitmap *cc, int32_t mode)
{
  if (!mask || !bb || !cc) return NULL;
  size_t pixels = (size_t)mask->width * (size_t)mask->height;
  if (pixels != (size_t)bb->width * (size_t)bb->height || pixels != (size_t)cc->width * (size_t)cc->height)
    return NULL;
  wz3_bitmap *b = wz3__blank_like(mask);
  if (!b) return NULL;
  wz3__inner(b->data, b->data, 4, WZ3__BI_GRAY, mask->data, 4, pixels, wz3__zero_pixel, wz3__zero_pixel);
  if (mode == 0) {
    wz3__inner(b->data, bb->data, 4, WZ3__BI_MERGE, cc->data, 4, pixels, wz3__zero_pixel, wz3__zero_pixel);
  } else if (mode == 1) {
    wz3__inner(b->data, bb->data, 4, WZ3__BI_MUL, b->data, 4, pixels, wz3__zero_pixel, wz3__zero_pixel);
    wz3__inner(b->data, cc->data, 4, WZ3__BI_ADD, b->data, 4, pixels, wz3__zero_pixel, wz3__zero_pixel);
  } else if (mode == 2) {
    wz3__inner(b->data, bb->data, 4, WZ3__BI_MUL, b->data, 4, pixels, wz3__zero_pixel, wz3__zero_pixel);
    wz3__inner(b->data, cc->data, 4, WZ3__BI_SUBR, b->data, 4, pixels, wz3__zero_pixel, wz3__zero_pixel);
  } else if (mode == 3) {
    wz3__inner(b->data, bb->data, 4, WZ3__BI_MULMERGE, cc->data, 4, pixels, wz3__zero_pixel, wz3__zero_pixel);
  }
  return b;
}

/* ---- Rotate / RotateMul / Distort ---------------------------------------------- */

#define WZ3__TAU_F ((float)(WZ3__PI * 2.0))

WZ3_API wz3_bitmap *wz3_bitmap_rotate(const wz3_bitmap_ctx *ctx, const wz3_bitmap *in, float angle, float sx,
                                      float sy, float tx, float ty, int32_t border, int32_t new_width,
                                      int32_t new_height)
{
  if (!in || sx == 0.0f || sy == 0.0f) return NULL;
  const int32_t xs = in->width, ys = in->height;
  const int32_t offset = wz3__ctx_offset(ctx);
  int32_t txs = xs, tys = ys;
  if (new_width) { int32_t e = new_width - 1 + offset; txs = 1 << (e < 0 ? 0 : e > 30 ? 30 : e); }
  if (new_height) { int32_t e = new_height - 1 + offset; tys = 1 << (e < 0 ? 0 : e > 30 ? 30 : e); }
  wz3_bitmap *b = wz3_bitmap_create(txs, tys);
  if (!b) return NULL;
  wz3__copy_settings(b, in);

  const float a = angle * WZ3__TAU_F;
  const float cosine = (float)wz3__cos((double)a), sine = (float)wz3__sin((double)a);
  const float fxs = (float)xs, fys = (float)ys, ftxs = (float)txs, ftys = (float)tys;
  const int32_t m00 = wz3_ftol((double)(((cosine * 65536.0f) * fxs) / (sx * ftxs)));
  const int32_t m01 = wz3_ftol((double)(((sine * 65536.0f) * fys) / (sx * ftxs)));
  const int32_t m10 = wz3_ftol((double)(((-sine * 65536.0f) * fxs) / (sy * ftys)));
  const int32_t m11 = wz3_ftol((double)(((cosine * 65536.0f) * fys) / (sy * ftys)));
  /* the parenthesized half-offset is integer arithmetic in the C source */
  const int32_t center_u = (int32_t)((uint32_t)txs * (uint32_t)m00 + (uint32_t)tys * (uint32_t)m10) / 2;
  const int32_t center_v = (int32_t)((uint32_t)txs * (uint32_t)m01 + (uint32_t)tys * (uint32_t)m11) / 2;
  const float f_tx_scaled = (tx * fxs) * 65536.0f, f_ty_scaled = (ty * fys) * 65536.0f;
  const int32_t m20 = wz3_ftol((double)(float)((double)f_tx_scaled - (double)center_u));
  const int32_t m21 = wz3_ftol((double)(float)((double)f_ty_scaled - (double)center_v));

  wz3__bilinear ctxb;
  wz3__bilinear_init(&ctxb, in, border, xs, ys);
  const int32_t x_mask = xs - 1, y_mask = ys - 1;
  const int point = (border & 4) != 0;
  for (int32_t y = 0; y < tys; y++) {
    uint32_t u = (uint32_t)y * (uint32_t)m10 + (uint32_t)m20;
    uint32_t v = (uint32_t)y * (uint32_t)m11 + (uint32_t)m21;
    uint16_t *out = b->data + (size_t)y * (size_t)txs * 4;
    for (int32_t x = 0; x < txs; x++) {
      if (point) {
        /* PointFilter always wraps, independent of the border bits */
        const uint16_t *p = in->data + ((size_t)(((int32_t)v >> 16) & y_mask) * (size_t)xs +
                                         (size_t)(((int32_t)u >> 16) & x_mask)) * 4;
        memcpy(out, p, 4 * sizeof(uint16_t));
      } else {
        wz3__bilinear_sample(&ctxb, (int32_t)u, (int32_t)v, out);
      }
      u += (uint32_t)m00;
      v += (uint32_t)m01;
      out += 4;
    }
  }
  return b;
}

WZ3_API wz3_bitmap *wz3_bitmap_rotate_mul(const wz3_bitmap_ctx *ctx, const wz3_bitmap *in, float angle,
                                          float sx, float sy, float tx, float ty, int32_t border,
                                          uint32_t color, int32_t mode, int32_t count, uint32_t fade)
{
  if (!in || count <= 0) return NULL;
  wz3_bitmap *bitmap = wz3_bitmap_color(in, 0, color);
  wz3_bitmap *accumulated = bitmap ? wz3_bitmap_copy(bitmap) : NULL;
  if (!bitmap || !accumulated) {
    wz3_bitmap_destroy(bitmap);
    wz3_bitmap_destroy(accumulated);
    return NULL;
  }
  const float sign_x = sx < 0.0f ? -1.0f : 1.0f, sign_y = sy < 0.0f ? -1.0f : 1.0f;
  sx = sx * sign_x;
  sy = sy * sign_y;
  int32_t i = 1;
  if (mode & 16) {
    const float count_pow = (float)(int32_t)(1u << (count & 31));
    const float span = (float)((double)(int32_t)(1u << (count & 31)) - 1.0);
    angle = angle / count_pow;
    tx = ((tx - 0.5f) / span) + 0.5f;
    ty = ((ty - 0.5f) / span) + 0.5f;
    const float inverse_span = 1.0f / span;
    sx = (float)wz3_pow((double)sx, (double)inverse_span);
    sy = (float)wz3_pow((double)sy, (double)inverse_span);
  } else {
    const float count_float = (float)count;
    angle = angle / count_float;
    tx = ((tx - 0.5f) / count_float) + 0.5f;
    ty = ((ty - 0.5f) / count_float) + 0.5f;
    const float inverse_count = 1.0f / count_float;
    sx = (float)wz3_pow((double)sx, (double)inverse_count);
    sy = (float)wz3_pow((double)sy, (double)inverse_count);
  }
  sx = sx * sign_x;
  sy = sy * sign_y;

  while (count-- > 0) {
    wz3_bitmap *next, *pair[2];
    if (mode & 16) {
      if (fade != 0xffffffffu) {
        next = wz3_bitmap_color(bitmap, 0, fade);
        wz3_bitmap_destroy(bitmap);
        bitmap = next;
        if (!bitmap) break;
        uint32_t a = (fade >> 24) & 255u, r = (fade >> 16) & 255u, g = (fade >> 8) & 255u, bl = fade & 255u;
        a = a * a >> 8; r = r * r >> 8; g = g * g >> 8; bl = bl * bl >> 8;
        fade = (a << 24) | (r << 16) | (g << 8) | bl;
      }
      next = wz3_bitmap_rotate(ctx, bitmap, angle, sx, sy, tx, ty, border, 0, 0);
      wz3_bitmap_destroy(bitmap);
      bitmap = next;
      if (!bitmap) break;
      pair[0] = bitmap; pair[1] = accumulated;
      next = wz3_bitmap_merge(mode & 15, (const wz3_bitmap *const *)pair, 2);
      wz3_bitmap_destroy(bitmap);
      bitmap = next;
      if (!bitmap) break;
      wz3_bitmap_destroy(accumulated);
      accumulated = wz3_bitmap_copy(bitmap);
      if (!accumulated) break;
      angle = angle * 2.0f;
      tx = ((tx - 0.5f) * 2.0f) + 0.5f;
      ty = ((ty - 0.5f) * 2.0f) + 0.5f;
      sx = sx * sx;
      sy = sy * sy;
    } else {
      if (fade != 0xffffffffu) {
        next = wz3_bitmap_color(bitmap, 0, fade);
        wz3_bitmap_destroy(bitmap);
        bitmap = next;
        if (!bitmap) break;
      }
      const float iteration = (float)i;
      wz3_bitmap *rotated = wz3_bitmap_rotate(ctx, bitmap, angle * iteration, ((sx - 1.0f) * iteration) + 1.0f,
                                              ((sy - 1.0f) * iteration) + 1.0f,
                                              ((tx - 0.5f) * iteration) + 0.5f,
                                              ((ty - 0.5f) * iteration) + 0.5f, border, 0, 0);
      pair[0] = accumulated; pair[1] = rotated; /* a NULL (degenerate) rotation ends the list */
      next = wz3_bitmap_merge(mode & 15, (const wz3_bitmap *const *)pair, 2);
      wz3_bitmap_destroy(rotated);
      wz3_bitmap_destroy(accumulated);
      accumulated = next;
      if (!accumulated) break;
      i++;
    }
  }
  wz3_bitmap_destroy(bitmap);
  return accumulated;
}

WZ3_API wz3_bitmap *wz3_bitmap_distort(const wz3_bitmap *sample, const wz3_bitmap *displacement, float distance,
                                       int32_t border)
{
  if (!sample || !displacement) return NULL;
  if ((int64_t)sample->width * sample->height != (int64_t)displacement->width * displacement->height)
    return NULL;
  wz3_bitmap *b = wz3__blank_like(displacement);
  if (!b) return NULL;
  /* the native code validates only Size; the bilinear shape comes from the
     displacement input */
  wz3__bilinear c;
  wz3__bilinear_init(&c, sample, border, b->width, b->height);
  const int32_t bump_x = wz3__ftoi((double)((distance * (float)b->width) * 4.0f));
  const int32_t bump_y = wz3__ftoi((double)((distance * (float)b->height) * 4.0f));
  for (int32_t y = 0; y < b->height; y++) {
    for (int32_t x = 0; x < b->width; x++) {
      size_t i = ((size_t)y * (size_t)b->width + (size_t)x) * 4;
      int32_t u = (int32_t)((uint32_t)((int64_t)x * 65536 + (int64_t)(displacement->data[i] - WZ3__HALF) * bump_x));
      int32_t v = (int32_t)((uint32_t)((int64_t)y * 65536 + (int64_t)(displacement->data[i + 1] - WZ3__HALF) * bump_y));
      wz3__bilinear_sample(&c, u, v, b->data + i);
    }
  }
  return b;
}

/* ---- Normals ------------------------------------------------------------------- */

WZ3_API wz3_bitmap *wz3_bitmap_normals(const wz3_bitmap *in, float distance, int32_t mode)
{
  if (!in) return NULL;
  wz3_bitmap *b = wz3__blank_like(in);
  if (!b) return NULL;
  const int32_t xs = in->width, ys = in->height;
  const int32_t dist = wz3_ftol((double)(distance * 65536.0f));
  const int32_t shift_x = wz3__power_of_two((uint32_t)xs), shift_y = wz3__power_of_two((uint32_t)ys);
  /* native words are BGRA and filterbump reads word zero (blue); this port
     stores RGBA, so the equivalent word is 2 */
#define WZ3__HEIGHT_AT(px, py) ((int32_t)in->data[((((size_t)((py) & (ys - 1))) * (size_t)xs) + (size_t)((px) & (xs - 1))) * 4 + 2])
  for (int32_t y = 0; y < ys; y++) {
    for (int32_t x = 0; x < xs; x++) {
      int32_t vx, vy;
      if (mode & 4) {
        vx = 4 * (WZ3__HEIGHT_AT(x - 1, y) - WZ3__HEIGHT_AT(x, y));
        vy = 4 * (WZ3__HEIGHT_AT(x, y - 1) - WZ3__HEIGHT_AT(x, y));
      } else {
        vx = WZ3__HEIGHT_AT(x - 2, y) + 3 * WZ3__HEIGHT_AT(x - 1, y) - 3 * WZ3__HEIGHT_AT(x, y) - WZ3__HEIGHT_AT(x + 1, y);
        vy = WZ3__HEIGHT_AT(x, y - 2) + 3 * WZ3__HEIGHT_AT(x, y - 1) - 3 * WZ3__HEIGHT_AT(x, y) - WZ3__HEIGHT_AT(x, y + 1);
      }
      /* signed 32-bit multiply then SAR; clamp15 around the 0x4000 bias */
      vx = wz3__clamp15i(((int32_t)((uint32_t)vx * (uint32_t)(dist >> 4)) >> (20 - shift_x)) + WZ3__HALF) - WZ3__HALF;
      vy = wz3__clamp15i(((int32_t)((uint32_t)vy * (uint32_t)(dist >> 4)) >> (20 - shift_y)) + WZ3__HALF) - WZ3__HALF;
      int32_t vz = 0;
      if (mode & 1) {
        int32_t z2 = 0x3fff * 0x3fff - vx * vx - vy * vy;
        /* sFSqrt returns through the x87 stack at single precision before the
           truncating integer store */
        if (z2 > 0) vz = (int32_t)(float)sqrt((double)z2);
        else {
          float scale = (float)(16383.0 / sqrt((double)(vx * vx + vy * vy)));
          vx = (int32_t)(float)((double)vx * (double)scale);
          vy = (int32_t)(float)((double)vy * (double)scale);
        }
      }
      if (mode & 2) {
        int32_t swap = vx;
        vx = vy;
        vy = -swap;
      }
      uint16_t *p = b->data + ((size_t)y * (size_t)xs + (size_t)x) * 4;
      p[0] = (uint16_t)(vx + WZ3__HALF);
      p[1] = (uint16_t)(vy + WZ3__HALF);
      p[2] = (uint16_t)(vz + WZ3__HALF);
      p[3] = 0xffff;
    }
  }
#undef WZ3__HEIGHT_AT
  return b;
}

/* ---- Bump ------------------------------------------------------------------------ */

/* 1/sqrt(v) the way the source guards it: a zero or NaN root becomes 1 */
static inline float wz3__inv_sqrt_guard(float v)
{
  double root = sqrt((double)v);
  if (root == 0.0 || isnan(root)) root = 1.0;
  return (float)(1.0 / root);
}

WZ3_API wz3_bitmap *wz3_bitmap_bump(const wz3_bitmap *in, const wz3_bitmap *normals, int32_t subcode,
                                    float px, float py, float pz, float da, float db, uint32_t diffuse_color,
                                    uint32_t ambient_color, float outer, float falloff, float amp,
                                    uint32_t specular_color, float spec_power, float spec_amp)
{
  if (!in || outer == 0.0f || isnan(outer)) return NULL;
  if (normals && (normals->width != in->width || normals->height != in->height)) return NULL;
  wz3_bitmap *b = wz3__blank_like(in);
  if (!b) return NULL;
  const int32_t xs = in->width, ys = in->height;
  uint16_t diffuse[4], ambient[4], specular[4];
  wz3_packed_color(diffuse_color, diffuse);
  wz3_packed_color(ambient_color, ambient);
  wz3_packed_color(specular_color, specular);
  px = ((px * (float)xs) * 2.0f) - (float)(xs / 2);
  py = ((py * (float)ys) * 2.0f) - (float)(ys / 2);
  pz = pz * (float)xs;
  da = da * WZ3__TAU_F;
  db = db * (float)WZ3__PI;
  const float cosine_b = (float)wz3__cos((double)db);
  const float sine_a = (float)wz3__sin((double)da), cosine_a = (float)wz3__cos((double)da);
  const float dx = cosine_b * sine_a;
  const float dy = cosine_b * cosine_a;
  const float dz = (float)wz3__sin((double)db);
  if (subcode == 0) {
    px = px - ((dx * pz) / dz);
    py = py - ((dy * pz) / dz);
  }
  const float scaled_spec = spec_amp * 65536.0f;
  float directional_half_inverse = 0.0f;
  if (subcode == 2 && scaled_spec != 0.0f)
    directional_half_inverse = wz3__inv_sqrt_guard(((dx * dx) + (dy * dy)) + ((dz + 1.0f) * (dz + 1.0f)));

  for (int32_t y = 0; y < ys; y++) {
    for (int32_t x = 0; x < xs; x++) {
      const size_t i = ((size_t)y * (size_t)xs + (size_t)x) * 4;
      float lx = dx, ly = dy, lz = dz;
      if (subcode != 2) {
        lx = (float)((double)x - (double)px);
        ly = (float)((double)y - (double)py);
        lz = pz;
        float length2 = ((lx * lx) + (ly * ly)) + (lz * lz);
        float inverse = wz3__inv_sqrt_guard(length2);
        lx = lx * inverse; ly = ly * inverse; lz = lz * inverse;
      }
      float nx = 0.0f, ny = 0.0f, nz = 1.0f;
      if (normals) {
        nx = (float)((int32_t)normals->data[i] - WZ3__HALF) / 16384.0f;
        ny = (float)((int32_t)normals->data[i + 1] - WZ3__HALF) / 16384.0f;
        nz = (float)((int32_t)normals->data[i + 2] - WZ3__HALF) / 16384.0f;
      }
      float light = ((lx * nx) + (ly * ny)) + (lz * nz);
      float spec_factor = 0.0f;
      if (scaled_spec != 0.0f) {
        const float hx = lx, hy = ly, hz = lz + 1.0f;
        float inverse = subcode == 2 ? directional_half_inverse
                                     : wz3__inv_sqrt_guard(((hx * hx) + (hy * hy)) + (hz * hz));
        spec_factor = ((hx * nx) + (hy * ny)) + (hz * nz);
        if (spec_factor < 0.0f) spec_factor = 0.0f;
        spec_factor = (float)wz3_pow((double)(spec_factor * inverse), (double)spec_power);
      }
      float direction = 1.0f;
      if (subcode == 0) {
        direction = ((lx * dx) + (ly * dy)) + (lz * dz);
        direction = direction < outer ? 0.0f
          : (float)wz3_pow((double)((direction - outer) / (1.0f - outer)), (double)falloff);
      }
      const float strength = (direction * light) * amp;
      uint16_t lit[4];
      for (int c = 0; c < 4; c++) {
        float lighting = (float)ambient[c] + (float)diffuse[c] * strength;
        float value = ((float)in->data[i + c] * lighting) / 32768.0f;
        lit[c] = (uint16_t)wz3__clamp15i(wz3_ftol((double)value));
      }
      wz3__add_scale_pixel(b->data + i, lit, specular, wz3_ftol((double)(((direction * spec_factor) * scaled_spec))));
    }
  }
  return b;
}

/* ---- Perlin ------------------------------------------------------------------------ */

typedef struct wz3__perlin_tables {
  uint8_t permutation[512];
  float gradients[512];
} wz3__perlin_tables;

static void wz3__perlin_tables_init(wz3__perlin_tables *t)
{
  wz3_rng r;
  uint32_t keys[256];
  wz3_rng_init(&r);
  wz3_rng_seed(&r, 1);
  for (int i = 0; i < 256; i++) {
    keys[i] = wz3_rng_int(&r, 0x10000);
    t->permutation[i] = (uint8_t)i;
  }
  /* the source's exchange sort: equal keys never swap */
  for (int left = 0; left < 255; left++)
    for (int right = left + 1; right < 256; right++)
      if (keys[left] > keys[right]) {
        uint32_t k = keys[left]; keys[left] = keys[right]; keys[right] = k;
        uint8_t p = t->permutation[left]; t->permutation[left] = t->permutation[right]; t->permutation[right] = p;
      }
  memcpy(t->permutation + 256, t->permutation, 256);
  for (int i = 0; i < 256;) {
    int64_t x = (int64_t)wz3_rng_int(&r, 0x10000) - 0x8000;
    int64_t y = (int64_t)wz3_rng_int(&r, 0x10000) - 0x8000;
    if (x * x + y * y < (int64_t)0x8000 * 0x8000) {
      t->gradients[i * 2] = (float)x / 32768.0f;
      t->gradients[i * 2 + 1] = (float)y / 32768.0f;
      i++;
    }
  }
}

WZ3_API wz3_bitmap *wz3_bitmap_perlin(const wz3_bitmap_ctx *ctx, uint32_t xs, uint32_t ys, int32_t frequency,
                                      int32_t octaves, float fadeoff, int32_t seed, int32_t mode,
                                      float amplitude, float gamma, uint32_t color0, uint32_t color1)
{
  if (frequency < 0 || frequency > 30 || octaves < 0 || octaves > 64) return NULL;
  wz3_bitmap *b = wz3__new_bitmap(ctx, xs, ys);
  if (!b) return NULL;
  wz3__perlin_tables *tables = (wz3__perlin_tables *)WZ3_MALLOC(sizeof *tables);
  const int32_t width = b->width, height = b->height;
  const int32_t poly_length = width >> frequency;
  int32_t *polynomial = (int32_t *)WZ3_MALLOC(sizeof(int32_t) * (size_t)(poly_length > 0 ? poly_length : 1));
  int32_t *row = (int32_t *)WZ3_MALLOC(sizeof(int32_t) * (size_t)width);
  if (!tables || !polynomial || !row) {
    WZ3_FREE(tables); WZ3_FREE(polynomial); WZ3_FREE(row);
    wz3_bitmap_destroy(b);
    return NULL;
  }
  wz3__perlin_tables_init(tables);
  const uint8_t *permutation = tables->permutation;
  const float *gradients = tables->gradients;

  uint16_t c0[4], c1[4];
  wz3_packed_color(color0, c0);
  wz3_packed_color(color1, c1);
  const int32_t shift_x = 16 - wz3__power_of_two((uint32_t)width), shift_y = 16 - wz3__power_of_two((uint32_t)height);
  seed &= 255;
  mode &= 3;

  int32_t gamma_table[1025];
  for (int32_t i = 0; i < 1025; i++)
    gamma_table[i] = wz3__clamp15d(wz3_pow((double)i / 1024.0, (double)gamma) * 32768.0) * 2;
  amplitude = amplitude * ((mode & 1) ? 32768.0f : 16384.0f);
  const int32_t amplitude_fixed = wz3_ftol((double)amplitude);
  const int32_t offset = (mode & 1) ? 0 : WZ3__HALF;

  /* sine-shaped noise resolves through a fixed-point table */
  int32_t sine[257];
  if (mode & 2) {
    for (int32_t x = 0; x < 257; x++) {
      float angle = (WZ3__TAU_F * (float)x) / 256.0f;
      sine[x] = wz3_ftol((double)(float)(wz3__sin((double)angle) * 0.5 * 65536.0));
    }
  }

  /* one polynomial period shared by every group and octave */
  for (int32_t x = 0; x < poly_length; x++) {
    float coordinate = (float)x / (float)poly_length;
    float value = (coordinate * coordinate) * coordinate;
    float inner = 10.0f + coordinate * ((6.0f * coordinate) - 15.0f);
    value = (value * inner) * 16384.0f;
    polynomial[x] = wz3_ftol((double)value);
  }

  size_t destination = 0;
  for (int32_t y = 0; y < height; y++) {
    memset(row, 0, sizeof(int32_t) * (size_t)width);
    float octave_scale = 1.0f;
    for (int32_t octave = frequency; octave < frequency + octaves; octave++) {
      const int grouped = shift_x + octave < 16;
      const int32_t group_size = grouped ? (width < (1 << (16 - shift_x - octave)) ? width : (1 << (16 - shift_x - octave))) : 1;
      const int32_t groups = grouped ? width >> (16 - shift_x - octave) : width;
      const int32_t mask = (int32_t)(((1u << (octave & 31)) - 1u) & 255u);
      const int32_t py = (int32_t)((uint32_t)y << ((shift_y + octave) & 31));
      const int32_t vy = (py >> 16) & mask;
      const int32_t delta_x = (int32_t)(1u << ((shift_x + octave) & 31));
      const float ty = (float)(py & 0xffff) / 65536.0f;
      float ty_fade = (ty * ty) * ty;
      ty_fade = ty_fade * (10.0f + (ty * ((6.0f * ty) - 15.0f)));
      const float ty0 = ty * (1.0f - ty_fade);
      const float ty1 = (ty - 1.0f) * ty_fade;
      const int32_t vy0 = permutation[vy ^ seed];
      const int32_t vy1 = permutation[((vy + 1) & mask) ^ seed];
      const int32_t polynomial_shift = octave - frequency;
      const int32_t scale_fixed = wz3_ftol((double)(octave_scale * 16384.0f));

      /* with both axes on lattice points the gradient dot product is zero and
         the native path skips the octave row */
      if (shift_x + octave < 16 || (py & 0xffff)) {
        int32_t row_index = 0;
        for (int32_t vx = 0; vx < groups; vx++) {
          const int32_t v00 = permutation[((vx + 0) & mask) + vy0];
          const int32_t v01 = permutation[((vx + 1) & mask) + vy0];
          const int32_t v10 = permutation[((vx + 0) & mask) + vy1];
          const int32_t v11 = permutation[((vx + 1) & mask) + vy1];

          const float horizontal0 = gradients[v00 * 2] + ((gradients[v10 * 2] - gradients[v00 * 2]) * ty_fade);
          const float horizontal1 = gradients[v01 * 2] + ((gradients[v11 * 2] - gradients[v01 * 2]) * ty_fade);
          const float vertical0 = (gradients[v00 * 2 + 1] * ty0) + (gradients[v10 * 2 + 1] * ty1);
          const float vertical1 = (gradients[v01 * 2 + 1] * ty0) + (gradients[v11 * 2 + 1] * ty1);

          int32_t fa = wz3_ftol((double)(vertical0 * 65536.0f));
          int32_t fb = wz3_ftol((double)((vertical1 - horizontal1) * 65536.0f));
          const int32_t fa_delta = wz3_ftol((double)(horizontal0 * (float)delta_x));
          const int32_t fb_delta = wz3_ftol((double)(horizontal1 * (float)delta_x));

          for (int32_t x = 0; x < group_size; x++) {
            const int32_t poly_index = x << (polynomial_shift & 31);
            const int32_t poly = (poly_index >= 0 && poly_index < poly_length) ? polynomial[poly_index] : 0;
            int32_t noise = (int32_t)((uint32_t)fa + (uint32_t)(((int32_t)(((uint32_t)fb - (uint32_t)fa) * (uint32_t)poly)) >> 14));
            if (mode == 1) {
              noise = noise < 0 ? (int32_t)(0u - (uint32_t)noise) : noise;
            } else if (mode & 2) {
              if (mode == 3) noise &= 0x7fff;
              const int32_t index = (noise >> 8) & 255;
              noise = (int32_t)((uint32_t)sine[index] +
                                (uint32_t)(((int32_t)((uint32_t)(sine[index + 1] - sine[index]) * (uint32_t)(noise & 255))) >> 8));
            }
            if (row_index < width)
              row[row_index] = (int32_t)((uint32_t)row[row_index] +
                                         (uint32_t)(((int32_t)((uint32_t)noise * (uint32_t)scale_fixed)) >> 14));
            row_index++;
            fa = (int32_t)((uint32_t)fa + (uint32_t)fa_delta);
            fb = (int32_t)((uint32_t)fb + (uint32_t)fb_delta);
          }
        }
      }
      octave_scale = octave_scale * fadeoff;
    }
    for (int32_t x = 0; x < width; x++) {
      const int32_t raw = wz3__clamp15i((int32_t)((uint32_t)wz3__fixed_mul(row[x], amplitude_fixed) + (uint32_t)offset));
      const int32_t fade = wz3__gamma_lookup(gamma_table, 1025, (uint32_t)raw, 5);
      wz3__fade_pixel(b->data + destination, c0, c1, fade);
      destination += 4;
    }
  }
  WZ3_FREE(tables); WZ3_FREE(polynomial); WZ3_FREE(row);
  return b;
}

/* ---- Cell ----------------------------------------------------------------------------- */

static void wz3__cell_pixel(wz3_bitmap *b, size_t offset4, int32_t mode, double best, double second,
                            const int32_t *cell, float divisor, float amplitude, float gamma,
                            const uint16_t *c0, const uint16_t *c1, const uint16_t *cb)
{
  /* the PC=24 control word rounds FSQRT before the multiply; the sF32 local
     rounds that multiply again */
  float value = (float)sqrt(best) * divisor;
  if (mode & 1) {
    float v1 = (float)sqrt(second) * divisor;
    value = (value + v1) > 0.00001 ? (v1 - value) / (v1 + value) : 0.0f;
  }
  int32_t fade = wz3__clamp15d((double)(float)wz3_pow((double)(value * amplitude), (double)gamma) * 32768.0) * 2;
  if (mode & 4) fade = 0x10000 - fade;
  if (mode & 2) {
    uint16_t cell_color[4];
    if (cell[2] == 0xffff) memcpy(cell_color, cb, sizeof cell_color);
    else wz3__fade_pixel(cell_color, c0, c1, cell[2] * 4);
    wz3__fade_pixel(b->data + offset4, cell_color, cb, fade);
  } else {
    wz3__fade_pixel(b->data + offset4, c0, c1, fade);
  }
}

WZ3_API wz3_bitmap *wz3_bitmap_cell(const wz3_bitmap_ctx *ctx, uint32_t xs, uint32_t ys, uint32_t color0,
                                    uint32_t color1, uint32_t color2, int32_t count, int32_t seed,
                                    float amplitude, float gamma, int32_t mode, float min_distance,
                                    int32_t percent, float aspect_parameter)
{
  if (count < 1 || count > 256) return NULL;
  wz3_bitmap *b = wz3__new_bitmap(ctx, xs, ys);
  if (!b) return NULL;

  int32_t cells[256][3];
  int32_t bounds[256];
  wz3_rng random;
  wz3_rng_init(&random);
  wz3_rng_seed(&random, seed);
  for (int32_t i = 0; i < count; i++)
    for (int k = 0; k < 3; k++) cells[i][k] = (int32_t)wz3_rng_int(&random, 0x4000);
  int32_t n = count;

  const int32_t min_distance_fixed = wz3_ftol((double)(min_distance * 16384.0f));
  const int32_t min_distance2 = min_distance_fixed * min_distance_fixed;
  for (int32_t i = 1; i < n;) {
    if ((mode & 2) && (int32_t)wz3_rng_int(&random, 255) < percent) cells[i][2] = 0xffff;
    const int32_t px = (cells[i][0] & 0x3fff) - 0x2000, py = (cells[i][1] & 0x3fff) - 0x2000;
    int cut = 0;
    for (int32_t j = 0; j < i && !cut; j++) {
      const int32_t dx = ((cells[j][0] - px) & 0x3fff) - 0x2000;
      const int32_t dy = ((cells[j][1] - py) & 0x3fff) - 0x2000;
      cut = dx * dx + dy * dy < min_distance2;
    }
    if (cut) {
      /* drop the cell, moving the former last one into its slot */
      n--;
      cells[i][0] = cells[n][0]; cells[i][1] = cells[n][1]; cells[i][2] = cells[n][2];
    } else i++;
  }

  const int32_t shift_x = 14 - wz3__power_of_two((uint32_t)b->width), shift_y = 14 - wz3__power_of_two((uint32_t)b->height);
  uint16_t c0[4], c1[4], cb[4];
  wz3_packed_color(color1, c0);
  wz3_packed_color(color0, c1);
  wz3_packed_color(color2, cb);

  /* aspect, aspf and aspdiv are all sF32 in the released code */
  const float aspect = (float)wz3_pow(2.0, (double)aspect_parameter);
  const int32_t aspf = aspect >= 1.0f ? wz3__ftoi((double)(65536.0f / (aspect * aspect)))
                                      : wz3__ftoi((double)((aspect * aspect) * 65536.0f));
  const float divisor = aspect >= 1.0f ? aspect / 16384.0f : 1.0f / (16384.0f * aspect);
  const int tiled = b->width >= 16 && b->height >= 16 && (b->width & 15) == 0 && (b->height & 15) == 0;

  if (!tiled) {
    /* the released loop assumes complete 16x16 tiles; tiny direct-API fixtures
       use the exhaustive search */
    for (int32_t y = 0; y < b->height; y++) {
      for (int32_t x = 0; x < b->width; x++) {
        int32_t px = x << shift_x, py = y << shift_y;
        if (aspect < 1.0f) { int32_t t = px; px = py; py = t; }
        double best = INFINITY, second = INFINITY;
        int32_t best_index = -1;
        for (int32_t i = 0; i < n; i++) {
          const int32_t cx = aspect < 1.0f ? cells[i][1] : cells[i][0];
          const int32_t cy = aspect < 1.0f ? cells[i][0] : cells[i][1];
          const int32_t dx = ((cx - px) & 0x3fff) - 0x2000;
          const int32_t dy = ((cy - py) & 0x3fff) - 0x2000;
          const double distance = (double)(wz3__fixed_mul(dx * dx, aspf) + dy * dy);
          if (distance < best) { second = best; best = distance; best_index = i; }
          else if (distance > best && distance < second) second = distance;
        }
        wz3__cell_pixel(b, ((size_t)y * (size_t)b->width + (size_t)x) * 4, mode, best, second,
                        cells[best_index < 0 ? 0 : best_index], divisor, amplitude, gamma, c0, c1, cb);
      }
    }
    return b;
  }

  /* The released non-intro path sorts the cells per 16x16 tile by a
     conservative squared-distance lower bound; once the second-best exact
     distance is no greater than the next bound no later cell can matter. The
     insertion sort is stable and the order persists from tile to tile. */
  const int flip_xy = aspect < 1.0f;
  if (flip_xy)
    for (int32_t i = 0; i < n; i++) { int32_t t = cells[i][0]; cells[i][0] = cells[i][1]; cells[i][1] = t; }
  const int32_t tile = 16;
  for (int32_t by = 0; by < b->height; by += tile) {
    for (int32_t bx = 0; bx < b->width; bx += tile) {
      int32_t px0 = bx << shift_x, px1 = (bx + tile - 1) << shift_x;
      int32_t py0 = by << shift_y, py1 = (by + tile - 1) << shift_y;
      if (flip_xy) {
        int32_t t = px0; px0 = py0; py0 = t;
        t = px1; px1 = py1; py1 = t;
      }
      for (int32_t i = 0; i < n; i++) {
        int32_t dx = ((cells[i][0] - px0) & 0x3fff) - 0x2000;
        int32_t dy = ((cells[i][0] - px1) & 0x3fff) - 0x2000;
        int32_t bound = 0;
        if ((dx ^ dy) > 0) {
          dx = abs(dx) < abs(dy) ? abs(dx) : abs(dy);
          bound = wz3__fixed_mul(dx * dx, aspf);
        }
        dx = ((cells[i][1] - py0) & 0x3fff) - 0x2000;
        dy = ((cells[i][1] - py1) & 0x3fff) - 0x2000;
        if ((dx ^ dy) > 0) {
          dy = abs(dx) < abs(dy) ? abs(dx) : abs(dy);
          bound += dy * dy;
        }
        bounds[i] = bound;
      }
      for (int32_t i = 1; i < n; i++) {
        const int32_t cx = cells[i][0], cy = cells[i][1], cc = cells[i][2], bound = bounds[i];
        int32_t j = i;
        while (j && bounds[j - 1] > bound) {
          cells[j][0] = cells[j - 1][0]; cells[j][1] = cells[j - 1][1]; cells[j][2] = cells[j - 1][2];
          bounds[j] = bounds[j - 1];
          j--;
        }
        cells[j][0] = cx; cells[j][1] = cy; cells[j][2] = cc; bounds[j] = bound;
      }

      for (int32_t ty = 0; ty < tile; ty++) {
        const int32_t py = (by + ty) << shift_y;
        for (int32_t tx = 0; tx < tile; tx++) {
          const int32_t px = (bx + tx) << shift_x;
          const int32_t x = flip_xy ? py : px;
          const int32_t y = flip_xy ? px : py;
          int32_t best = 0x40000000, second = 0x40000000, best_index = -1;
          for (int32_t i = 0; i < n && second > bounds[i]; i++) {
            const int32_t dx = ((cells[i][0] - x) & 0x3fff) - 0x2000;
            const int32_t dy = ((cells[i][1] - y) & 0x3fff) - 0x2000;
            const int32_t distance = wz3__fixed_mul(dx * dx, aspf) + dy * dy;
            if (distance < best) { second = best; best = distance; best_index = i; }
            else if (distance > best && distance < second) second = distance;
          }
          wz3__cell_pixel(b, ((size_t)(by + ty) * (size_t)b->width + (size_t)(bx + tx)) * 4, mode,
                          (double)best, (double)second, cells[best_index < 0 ? 0 : best_index],
                          divisor, amplitude, gamma, c0, c1, cb);
        }
      }
    }
  }
  return b;
}

/* ---- Gradient --------------------------------------------------------------------------- */

WZ3_API wz3_bitmap *wz3_bitmap_gradient(const wz3_bitmap_ctx *ctx, uint32_t xs, uint32_t ys, uint32_t color0,
                                        uint32_t color1, float position, float angle, float length,
                                        int32_t mode)
{
  wz3_bitmap *b = wz3__new_bitmap(ctx, xs, ys);
  if (!b) return NULL;
  uint16_t c0[4], c1[4];
  wz3_packed_color(color0, c0);
  wz3_packed_color(color1, c1);
  const float l = 32768.0f / length;
  const float radians = angle * WZ3__TAU_F;
  const int32_t dx = wz3_ftol((double)((float)wz3__cos((double)radians) * l));
  const int32_t dy = wz3_ftol((double)((float)wz3__sin((double)radians) * l));
  const int32_t cdx = wz3__fixed_mul(dx, 0x10000 / b->width);
  const int32_t cdy = (int32_t)((uint32_t)wz3__fixed_mul(dy, 0x10000 / b->height) - (uint32_t)b->width * (uint32_t)cdx);
  /* dx/2 and dy/2 are signed integer divisions in the source */
  const int32_t half_dx = dx / 2, half_dy = dy / 2;
  int32_t coordinate = wz3__ftoi((double)((float)WZ3__HALF - (float)(half_dx + half_dy) * (position + 1.0f)));
  for (int32_t y = 0; y < b->height; y++) {
    for (int32_t x = 0; x < b->width; x++) {
      int32_t fade;
      if (mode == 1) {
        float phase = (((float)wz3__clamp15i(coordinate) * WZ3__TAU_F) / 32768.0f) + 8192.0f;
        fade = wz3_ftol((double)(((float)wz3__sin((double)phase) * (float)WZ3__MAX) + (float)WZ3__MAX));
      } else if (mode == 2) {
        float phase = ((float)wz3__clamp15i(coordinate) * WZ3__TAU_F) / 65536.0f;
        fade = wz3_ftol((double)((float)wz3__sin((double)phase) * 65535.0f));
      } else {
        fade = wz3__clamp15i(coordinate) * 2;
      }
      wz3__fade_pixel(b->data + ((size_t)y * (size_t)b->width + (size_t)x) * 4, c0, c1, fade);
      coordinate = (int32_t)((uint32_t)coordinate + (uint32_t)cdx);
    }
    coordinate = (int32_t)((uint32_t)coordinate + (uint32_t)cdy);
  }
  return b;
}

/* ---- Bricks ------------------------------------------------------------------------------- */

typedef struct wz3__brick { uint8_t head; uint16_t color[4]; } wz3__brick;

static int32_t wz3__brick_fade(wz3_rng *random, float color_balance)
{
  /* sFPow returns through the x87 stack at single precision and so does the
     following multiply */
  float f = wz3_rng_float(random, 1.0f);
  return wz3__ftoi((double)((float)wz3_pow((double)f, (double)color_balance) * 65536.0f));
}

WZ3_API wz3_bitmap *wz3_bitmap_bricks(const wz3_bitmap_ctx *ctx, uint32_t xs, uint32_t ys, uint32_t color0,
                                      uint32_t color1, uint32_t mortar_color, float mortar_x, float mortar_y,
                                      int32_t tiles_x, int32_t tiles_y, int32_t seed, int32_t heads,
                                      int32_t flags, float side, float color_balance)
{
  wz3_bitmap *b = wz3__new_bitmap(ctx, xs, ys);
  if (!b) return NULL;
  wz3_rng random;
  wz3_rng_init(&random);
  wz3_rng_seed(&random, seed);
  uint16_t c0[4], c1[4], mortar[4];
  wz3_packed_color(color0, c0);
  wz3_packed_color(color1, c1);
  wz3_packed_color(mortar_color, mortar);
  const int32_t fugue_x = wz3__ftoi((double)(mortar_x * 8192.0f));
  const int32_t fugue_y = wz3__ftoi((double)(mortar_y * 8192.0f));
  const int32_t side_step = wz3__ftoi((double)(side * 16384.0f));
  const int32_t multiply = 1 << ((flags >> 4) & 7);
  const int32_t base_x = tiles_x, base_y = tiles_y;
  if (base_x <= 0 || base_y <= 0) return b;
  if ((int64_t)base_x * multiply * base_y * multiply > ((int64_t)1 << 22)) {
    wz3_bitmap_destroy(b);
    return NULL;
  }

  const int32_t ex = base_x * multiply, ey = base_y * multiply;
  wz3__brick *base = (wz3__brick *)WZ3_MALLOC(sizeof(wz3__brick) * (size_t)base_x * (size_t)base_y);
  wz3__brick *cells = (wz3__brick *)WZ3_MALLOC(sizeof(wz3__brick) * (size_t)ex * (size_t)ey);
  if (!base || !cells) {
    WZ3_FREE(base); WZ3_FREE(cells);
    wz3_bitmap_destroy(b);
    return NULL;
  }
  memset(base, 0, sizeof(wz3__brick) * (size_t)base_x * (size_t)base_y);

  int32_t head = 0;
  for (int32_t y = 0; y < base_y; y++) {
    int32_t trigger = 0;
    for (int32_t x = 0; x < base_x; x++) {
      trigger--;
      base[y * base_x + x].head = (uint8_t)head;
      if (head == 0) head = 1;
      else if ((int32_t)wz3_rng_int(&random, 255) > heads || trigger >= 0) head = 0;
      else if (flags & 4) trigger = 2;
    }
    wz3__brick *first = &base[y * base_x], *last = &base[y * base_x + base_x - 1];
    if (!first->head && !last->head) first->head = 1;
    if ((flags & 4) && base_x >= 3) {
      if (first->head && first[1].head && first[2].head) first[1].head = 0;
      if (last->head && first->head && first[1].head) first->head = 0;
      if (last[-1].head && last->head && first->head) last->head = 0;
    }
  }
  for (int32_t yy = 0; yy < multiply; yy++)
    for (int32_t y = 0; y < base_y; y++)
      for (int32_t xx = 0; xx < multiply; xx++)
        for (int32_t x = 0; x < base_x; x++) {
          wz3__brick *dst = &cells[(size_t)((yy * base_y + y) * ex + xx * base_x + x)];
          memset(dst, 0, sizeof *dst);
          dst->head = base[y * base_x + x].head;
        }
  WZ3_FREE(base);
  const int32_t tx_count = ex, ty_count = ey;

  uint16_t current[4];
  wz3__fade_pixel(current, c0, c1, wz3__brick_fade(&random, color_balance));
  for (int32_t y = 0; y < ty_count; y++) {
    for (int32_t x = 0; x < tx_count; x++) {
      wz3__brick *cell = &cells[(size_t)y * (size_t)tx_count + (size_t)x];
      if (cell->head) wz3__fade_pixel(current, c0, c1, wz3__brick_fade(&random, color_balance));
      memcpy(cell->color, current, sizeof current);
    }
    wz3__brick *row = &cells[(size_t)y * (size_t)tx_count];
    if (!row[0].head) memcpy(row[0].color, row[tx_count - 1].color, sizeof row[0].color);
  }

  const int32_t step_x = 0x4000 / b->width, step_y = 0x4000 / b->height;
  const int32_t pixel_x = step_x * tx_count, pixel_y = step_y * ty_count;
  for (int32_t y = 0; y < b->height; y++) {
    int32_t fy = step_y * y * ty_count;
    const int32_t by = fy >> 14;
    fy &= 0x3fff;
    const int32_t side_offset = (by * side_step) & 0x7fff;
    for (int32_t x = 0; x < b->width; x++) {
      int32_t fx = step_x * x * tx_count + side_offset;
      int32_t bx = fx >> 14;
      while (bx >= tx_count) bx -= tx_count;
      fx &= 0x3fff;
      int32_t fade = 0x4000;
      const int left_head = cells[(size_t)by * (size_t)tx_count + (size_t)bx].head;
      const int right_head = cells[(size_t)by * (size_t)tx_count + (size_t)((bx + 1) % tx_count)].head;
#define WZ3__MAX0(v) ((v) > 0 ? (v) : 0)
#define WZ3__MIN(a, c) ((a) < (c) ? (a) : (c))
      if (flags & 8) {
        if (left_head && fx < fugue_x) fade = pixel_x ? 0x4000 * WZ3__MAX0(fx - (fugue_x - pixel_x)) / pixel_x : 0;
        if (right_head && 0x4000 - fx < fugue_x)
          fade = pixel_x ? 0x4000 * WZ3__MAX0(0x4000 - fx - (fugue_x - pixel_x)) / pixel_x : 0;
        if (fy < fugue_y) fade = WZ3__MIN(fade, pixel_y ? 0x4000 * WZ3__MAX0(fy - (fugue_y - pixel_y)) / pixel_y : 0);
        if (0x4000 - fy < fugue_y)
          fade = WZ3__MIN(fade, pixel_y ? 0x4000 * WZ3__MAX0(0x4000 - fy - (fugue_y - pixel_y)) / pixel_y : 0);
      } else {
        if (left_head && fx < fugue_x) fade = fugue_x ? 0x4000 * fx / fugue_x : fade;
        if (right_head && 0x4000 - fx < fugue_x) fade = fugue_x ? 0x4000 * (0x4000 - fx) / fugue_x : fade;
        if (fy < fugue_y) fade = WZ3__MIN(fade, fugue_y ? 0x4000 * fy / fugue_y : fade);
        if (0x4000 - fy < fugue_y) fade = WZ3__MIN(fade, fugue_y ? 0x4000 * (0x4000 - fy) / fugue_y : fade);
      }
#undef WZ3__MAX0
#undef WZ3__MIN
      wz3__fade_pixel(b->data + ((size_t)y * (size_t)b->width + (size_t)x) * 4, mortar,
                      cells[(size_t)by * (size_t)tx_count + (size_t)bx].color, fade * 4);
    }
  }
  WZ3_FREE(cells);
  return b;
}

/* ---- ColorBalance / Bulge ------------------------------------------------------------------- */

WZ3_API wz3_bitmap *wz3_bitmap_color_balance(const wz3_bitmap *in, const float values[9])
{
  if (!in) return NULL;
  wz3_bitmap *b = wz3__blank_like(in);
  if (!b) return NULL;
  int32_t tables[3][257];
  const float scale = (float)(100.0 / 255.0);
  for (int channel = 0; channel < 3; channel++) {
    const float shadow = values[channel], midtone = values[channel + 3], highlight = values[channel + 6];
    const float exponent_argument = ((shadow * 0.5f) + midtone) + (highlight * 0.5f);
    const float exponent = (float)wz3_pow(0.5, (double)exponent_argument);
    const float minimum = (-(shadow < 0.0f ? shadow : 0.0f)) * scale;
    const float maximum = 1.0f - ((highlight > 0.0f ? highlight : 0.0f) * scale);
    const float multiplier = 1.0f / (maximum - minimum);
    for (int32_t i = 0; i <= 256; i++) {
      const float coordinate = (float)i / 256.0f;
      float bounded = maximum < coordinate ? maximum : coordinate;
      bounded = minimum > bounded ? minimum : bounded;
      const float normalized = (bounded - minimum) * multiplier;
      const float mapped = (float)wz3_pow((double)normalized, (double)exponent);
      tables[channel][i] = wz3__clamp15d((double)(mapped * 32768.0f));
    }
  }
  const size_t n = (size_t)b->width * (size_t)b->height * 4;
  for (size_t i = 0; i < n; i += 4) {
    for (int c = 0; c < 3; c++)
      b->data[i + c] = (uint16_t)wz3__gamma_lookup(tables[c], 257, in->data[i + c], 7);
    b->data[i + 3] = in->data[i + 3];
  }
  return b;
}

WZ3_API wz3_bitmap *wz3_bitmap_bulge(const wz3_bitmap *in, float warp)
{
  if (!in) return NULL;
  wz3_bitmap *b = wz3__blank_like(in);
  if (!b) return NULL;
  wz3__bilinear c;
  wz3__bilinear_init(&c, in, 0, in->width, in->height);
  const float x_scale = (float)((double)in->width * 65536.0), y_scale = (float)((double)in->height * 65536.0);
  const float inv_x = 1.0f / (float)in->width, inv_y = 1.0f / (float)in->height;
  for (int32_t y = 0; y < in->height; y++) {
    for (int32_t x = 0; x < in->width; x++) {
      const float fx = (float)x * inv_x, fy = (float)y * inv_y;
      float u = (fx - 0.5f) * 2.0f, v = (0.5f - fy) * 2.0f;
      const float radius2 = (u * u) + (v * v);
      if (radius2 <= 1.0f) {
        const float root = (float)sqrt((double)(1.0f - radius2));
        const float denominator = 1.0f + (warp * root);
        const float inverse = 1.0f / denominator;
        u = u * inverse;
        v = v * inverse;
      }
      u = (1.0f + u) * 0.5f;
      v = (1.0f - v) * 0.5f;
      wz3__bilinear_sample(&c, wz3__ftoi((double)(u * x_scale)), wz3__ftoi((double)(v * y_scale)),
                           b->data + ((size_t)y * (size_t)in->width + (size_t)x) * 4);
    }
  }
  return b;
}

/* ---- Text / Import --------------------------------------------------------------------------- */

/* TEXTGLYPHS_BEGIN generated by tools/sync_text_glyphs.py of the reference repository from Arimo Regular 1.33; do not edit */
/* 96 glyphs, 2079 outline points; Arimo is OFL-1.1, see vendor/font3d/LICENSE.txt
   (googlefonts/arimo 4a6255f, sha256 41b22bc8f0b51f93...) */
#define WZ3__FONT_UNITS_PER_EM 2048
#define WZ3__FONT_ASCENT 1854
#define WZ3__FONT_DESCENT 434
#define WZ3__FONT_CAP_HEIGHT 1409
typedef struct wz3__glyph {
  uint16_t code, advance;
  uint16_t first_point, n_points, first_end, n_ends;
} wz3__glyph;
static const wz3__glyph wz3__glyphs[96] = {
  {32,569,0,0,0,0},
  {33,569,0,8,0,2},
  {34,727,8,8,2,2},
  {35,1139,16,32,4,2},
  {36,1139,48,55,6,3},
  {37,1821,103,48,9,5},
  {38,1366,151,59,14,3},
  {39,391,210,4,17,1},
  {40,682,214,14,18,1},
  {41,682,228,14,19,1},
  {42,797,242,15,20,1},
  {43,1196,257,12,21,1},
  {44,569,269,10,22,1},
  {45,682,279,4,23,1},
  {46,569,283,4,24,1},
  {47,569,287,4,25,1},
  {48,1139,291,24,26,2},
  {49,1139,315,11,28,1},
  {50,1139,326,33,29,1},
  {51,1139,359,41,30,1},
  {52,1139,400,20,31,2},
  {53,1139,420,30,33,1},
  {54,1139,450,36,34,2},
  {55,1139,486,16,36,1},
  {56,1139,502,47,37,3},
  {57,1139,549,38,40,2},
  {58,569,587,8,42,2},
  {59,569,595,14,44,2},
  {60,1196,609,7,46,1},
  {61,1196,616,8,47,2},
  {62,1196,624,7,49,1},
  {63,1139,631,32,50,2},
  {64,2079,663,93,52,2},
  {65,1366,756,17,54,2},
  {66,1366,773,30,56,3},
  {67,1479,803,27,59,1},
  {68,1479,830,20,60,2},
  {69,1366,850,12,62,1},
  {70,1251,862,10,63,1},
  {71,1593,872,31,64,1},
  {72,1479,903,12,65,1},
  {73,569,915,4,66,1},
  {74,1024,919,17,67,1},
  {75,1366,936,12,68,1},
  {76,1139,948,6,69,1},
  {77,1706,954,29,70,1},
  {78,1479,983,15,71,1},
  {79,1593,998,29,72,2},
  {80,1366,1027,18,74,2},
  {81,1593,1045,40,76,2},
  {82,1479,1085,23,78,2},
  {83,1366,1108,50,80,1},
  {84,1251,1158,8,81,1},
  {85,1479,1166,20,82,1},
  {86,1366,1186,13,83,1},
  {87,1933,1199,34,84,1},
  {88,1366,1233,12,85,1},
  {89,1366,1245,9,86,1},
  {90,1251,1254,10,87,1},
  {91,569,1264,8,88,1},
  {92,569,1272,4,89,1},
  {93,569,1276,8,90,1},
  {94,961,1284,7,91,1},
  {95,1139,1291,4,92,1},
  {96,682,1295,6,93,1},
  {97,1139,1301,51,94,2},
  {98,1139,1352,38,96,2},
  {99,1024,1390,26,98,1},
  {100,1139,1416,36,99,2},
  {101,1139,1452,26,101,2},
  {102,569,1478,22,103,1},
  {103,1139,1500,45,104,2},
  {104,1139,1545,25,106,1},
  {105,455,1570,8,107,2},
  {106,455,1578,19,109,2},
  {107,1024,1597,12,111,1},
  {108,455,1609,4,112,1},
  {109,1706,1613,41,113,1},
  {110,1139,1654,25,114,1},
  {111,1139,1679,23,115,2},
  {112,1139,1702,36,117,2},
  {113,1139,1738,36,119,2},
  {114,682,1774,23,121,1},
  {115,1024,1797,42,122,1},
  {116,569,1839,21,123,1},
  {117,1139,1860,25,124,1},
  {118,1024,1885,12,125,1},
  {119,1479,1897,31,126,1},
  {120,1024,1928,12,127,1},
  {121,1024,1940,25,128,1},
  {122,1024,1965,10,129,1},
  {123,684,1975,35,130,1},
  {124,532,2010,4,131,1},
  {125,684,2014,34,132,1},
  {126,1196,2048,25,133,1},
  {180,682,2073,6,134,1},
};
static const uint16_t wz3__glyph_ends[135] = {
  3,7,11,15,43,47,85,93,102,113,117,128,139,150,187,199,209,213,227,241,
  256,268,278,282,286,290,302,314,325,358,399,410,419,449,472,485,501,527,537,548,
  573,586,590,594,598,608,615,619,623,630,658,662,737,755,763,772,786,794,802,829,
  839,849,861,871,902,914,918,935,947,953,982,997,1013,1026,1037,1044,1071,1084,1098,1107,
  1157,1165,1185,1198,1232,1244,1253,1263,1271,1275,1283,1290,1294,1300,1336,1351,1377,1389,1415,1439,
  1451,1470,1477,1499,1532,1544,1569,1573,1577,1581,1596,1608,1612,1653,1678,1689,1701,1725,1737,1761,
  1773,1796,1838,1859,1884,1896,1927,1939,1964,1974,2009,2013,2047,2072,2078,
};
/* x, y, on-curve per point */
static const int16_t wz3__glyph_points[6237] = {
  383,1409,1,359,397,1,211,397,1,187,1409,1,379,201,1,379,0,1,185,0,1,185,201,1,640,1409,1,618,966,1,476,966,1,456,1409,1,
  271,1409,1,249,966,1,108,966,1,87,1409,1,320,408,1,236,0,1,126,0,1,210,408,1,9,408,1,9,516,1,234,516,1,312,885,1,
  60,885,1,60,993,1,334,993,1,423,1401,1,533,1401,1,445,993,1,808,993,1,896,1401,1,1006,1401,1,918,993,1,1129,993,1,1129,885,1,
  896,885,1,818,516,1,1078,516,1,1078,408,1,795,408,1,707,0,1,597,0,1,683,408,1,345,516,1,707,516,1,785,885,1,425,885,1,
  518,20,1,93,38,0,22,379,1,192,416,1,217,297,0,376,177,0,518,168,1,518,664,1,430,686,0,308,726,0,274,743,1,173,796,0,
  86,937,0,86,1046,1,86,1201,0,311,1376,0,518,1385,1,518,1516,1,642,1516,1,642,1385,1,829,1376,0,1033,1225,0,1075,1065,1,901,1032,1,
  881,1126,0,759,1231,0,642,1242,1,642,797,1,818,756,0,897,720,1,1011,667,0,1106,517,0,1106,396,1,1106,231,0,864,31,0,642,20,1,
  642,-142,1,518,-142,1,258,1048,1,258,988,0,283,948,1,308,907,0,407,851,0,518,823,1,518,1244,1,258,1230,0,934,394,1,934,459,0,
  882,543,0,833,571,1,784,598,0,642,635,1,642,167,1,783,176,0,934,292,0,394,1421,1,707,1421,0,707,975,1,707,758,0,548,524,0,
  390,524,1,232,524,0,73,756,0,73,975,1,73,1198,0,227,1421,0,1451,1409,1,527,0,1,372,0,1,1294,1409,1,560,975,1,560,1151,0,
  484,1313,0,394,1313,1,300,1313,0,220,1154,0,220,975,1,220,802,0,300,637,0,392,637,1,560,637,0,1748,434,1,1748,219,0,1586,-12,0,
  1428,-12,1,1113,-12,0,1113,434,1,1113,662,0,1266,885,0,1432,885,1,1596,885,0,1748,656,0,1600,434,1,1600,613,0,1523,774,0,1432,774,1,
  1341,774,0,1260,616,0,1260,434,1,1260,263,0,1339,98,0,1430,98,1,1600,98,0,72,371,1,72,649,0,415,800,1,382,862,0,334,1032,0,
  334,1102,1,334,1252,0,517,1417,0,685,1417,1,836,1417,0,1021,1265,0,1021,1133,1,1021,1063,0,958,942,0,887,886,1,791,812,0,612,741,1,
  721,538,0,905,329,1,1018,496,0,1076,739,1,1221,696,1,1159,449,0,1009,227,1,1105,129,0,1217,129,1,1289,129,0,1334,145,1,1334,10,1,
  1280,-12,0,1193,-12,1,1016,-12,0,895,115,1,740,-20,0,523,-20,1,308,-20,0,72,187,0,869,1133,1,869,1205,0,769,1296,0,683,1296,1,
  587,1296,0,487,1193,0,487,1102,1,487,990,0,552,858,1,683,911,0,745,950,1,869,1029,0,795,217,1,598,450,0,476,674,1,240,573,0,
  240,373,1,240,252,0,395,111,0,529,111,1,600,111,0,742,166,0,288,1409,1,266,966,1,125,966,1,104,1409,1,496,-424,1,307,-220,0,
  127,241,0,127,528,1,127,820,0,308,1281,0,496,1484,1,670,1484,1,483,1276,0,308,808,0,308,530,1,308,253,0,481,-213,0,670,-424,1,
  12,-424,1,200,-214,0,374,251,0,374,530,1,374,809,0,199,1275,0,12,1484,1,186,1484,1,375,1280,0,555,819,0,555,532,1,555,240,0,
  374,-221,0,186,-424,1,668,762,1,549,690,1,399,948,1,243,692,1,124,764,1,313,1012,1,33,1085,1,78,1219,1,345,1112,1,333,1409,1,
  469,1409,1,456,1114,1,720,1217,1,765,1085,1,483,1012,1,671,608,1,671,180,1,524,180,1,524,608,1,100,608,1,100,754,1,524,754,1,
  524,1182,1,671,1182,1,671,754,1,1095,754,1,1095,608,1,385,51,1,385,-55,0,347,-197,0,307,-262,1,184,-262,1,278,-126,0,278,0,1,
  190,0,1,190,219,1,385,219,1,591,624,1,591,464,1,91,464,1,91,624,1,382,219,1,382,0,1,187,0,1,187,219,1,569,1484,1,
  162,-20,1,0,-20,1,411,1484,1,1059,705,1,1059,352,0,810,-20,0,567,-20,1,324,-20,0,80,350,0,80,705,1,80,1068,0,317,1430,0,
  573,1430,1,822,1430,0,1059,1064,0,876,705,1,876,1010,0,735,1284,0,573,1284,1,407,1284,0,262,1014,0,262,705,1,262,405,0,409,127,0,
  569,127,1,728,127,0,876,411,0,1039,153,1,1039,0,1,156,0,1,156,153,1,515,153,1,515,1237,1,197,1010,1,197,1180,1,530,1409,1,
  696,1409,1,696,153,1,1036,153,1,1036,0,1,103,0,1,103,127,1,154,244,0,299,424,0,463,568,0,543,630,1,583,661,0,654,723,0,
  686,754,1,750,816,0,829,952,0,829,1038,1,829,1154,0,693,1282,0,572,1282,1,457,1282,0,308,1157,0,295,1044,1,111,1061,1,131,1230,0,
  378,1430,0,572,1430,1,785,1430,0,1014,1229,0,1014,1044,1,1014,882,0,865,719,1,793,639,0,582,468,1,358,285,0,301,153,1,1049,389,1,
  1049,194,0,801,-20,0,571,-20,1,357,-20,0,102,173,0,78,362,1,264,379,1,300,129,0,571,129,1,707,129,0,862,263,0,862,395,1,
  862,510,0,685,639,0,518,639,1,416,639,1,416,795,1,514,795,1,662,795,0,825,924,0,825,1038,1,825,1151,0,692,1282,0,561,1282,1,
  442,1282,0,295,1160,0,283,1049,1,102,1063,1,122,1236,0,369,1430,0,563,1430,1,775,1430,0,1010,1233,0,1010,1057,1,1010,922,0,859,753,0,
  715,723,1,715,719,1,873,702,0,1049,524,0,881,319,1,881,0,1,711,0,1,711,319,1,47,319,1,47,459,1,692,1409,1,881,1409,1,
  881,461,1,1079,461,1,1079,319,1,213,461,1,711,461,1,711,1023,1,711,1069,0,712,1160,0,715,1206,1,692,1162,0,658,1107,0,644,1087,1,
  1053,459,1,1053,236,0,788,-20,0,553,-20,1,356,-20,0,114,152,0,82,315,1,264,336,1,321,127,0,557,127,1,702,127,0,866,302,0,
  866,455,1,866,588,0,701,752,0,561,752,1,488,752,0,362,706,0,299,651,1,123,651,1,170,1409,1,971,1409,1,971,1256,1,334,1256,1,
  307,809,1,424,899,0,598,899,1,737,899,0,941,788,0,1053,590,0,1049,461,1,1049,238,0,807,-20,0,594,-20,1,356,-20,0,104,334,0,
  104,672,1,104,1038,0,366,1430,0,608,1430,1,927,1430,0,1010,1143,1,838,1112,1,786,1284,0,606,1284,1,452,1284,0,283,997,0,283,725,1,
  332,816,0,510,911,0,625,911,1,820,911,0,1049,667,0,866,453,1,866,606,0,716,772,0,582,772,1,456,772,0,301,625,0,301,496,1,
  301,388,0,375,220,0,504,125,0,588,125,1,718,125,0,866,300,0,1036,1263,1,928,1098,0,775,840,0,731,746,1,642,559,0,553,195,0,
  553,0,1,365,0,1,365,140,0,423,429,0,487,588,1,607,885,0,862,1256,1,105,1256,1,105,1409,1,1036,1409,1,1050,393,1,1050,198,0,
  802,-20,0,570,-20,1,344,-20,0,89,194,0,89,391,1,89,529,0,247,717,0,370,737,1,370,741,1,255,768,0,122,948,0,122,1069,1,
  122,1230,0,363,1430,0,566,1430,1,774,1430,0,1015,1234,0,1015,1067,1,1015,946,0,881,766,0,765,743,1,765,739,1,900,717,0,1050,532,0,
  828,1057,1,828,1296,0,566,1296,1,306,1296,0,306,1057,1,306,936,0,443,809,0,568,809,1,695,809,0,828,926,0,863,410,1,863,541,0,
  707,674,0,566,674,1,429,674,0,275,531,0,275,406,1,275,115,0,572,115,1,719,115,0,863,256,0,1042,733,1,1042,370,0,777,-20,0,
  532,-20,1,367,-20,0,168,119,0,125,274,1,297,301,1,351,125,0,535,125,1,690,125,0,860,413,0,864,680,1,824,590,0,630,481,0,
  514,481,1,388,481,0,200,599,0,96,813,0,96,956,1,96,1177,0,344,1430,0,565,1430,1,800,1430,0,1042,1082,0,846,907,1,846,1077,0,
  690,1284,0,559,1284,1,429,1284,0,279,1107,0,279,956,1,279,802,0,429,623,0,557,623,1,635,623,0,766,695,0,846,824,0,382,1082,1,
  382,875,1,187,875,1,187,1082,1,382,207,1,382,0,1,187,0,1,187,207,1,385,1082,1,385,875,1,190,875,1,190,1082,1,385,51,1,
  385,-55,0,347,-197,0,307,-262,1,184,-262,1,278,-126,0,278,0,1,190,0,1,190,207,1,385,207,1,1096,307,1,1096,154,1,101,571,1,
  101,776,1,1096,1194,1,1096,1040,1,238,674,1,1095,1004,1,1095,856,1,100,856,1,100,1004,1,1095,492,1,1095,344,1,100,344,1,100,492,1,
  1096,571,1,101,154,1,101,307,1,959,674,1,101,1040,1,101,1194,1,1096,776,1,1063,1032,1,1063,919,0,966,761,0,844,671,1,764,612,1,
  622,510,0,621,377,1,446,377,1,449,469,0,514,595,0,614,684,0,667,722,1,724,763,0,822,843,0,881,947,0,881,1024,1,881,1138,0,
  726,1270,0,586,1270,1,446,1270,0,282,1130,0,268,1008,1,84,1020,1,110,1218,0,370,1430,0,584,1430,1,807,1430,0,1063,1219,0,633,201,1,
  633,0,1,438,0,1,438,201,1,1902,755,1,1902,569,0,1845,419,1,1787,268,0,1582,104,0,1455,104,1,1356,104,0,1248,192,0,1248,280,1,
  1249,297,0,1250,333,0,1251,350,1,1245,350,1,1179,227,0,984,104,0,871,104,1,714,104,0,541,308,0,541,489,1,541,652,0,606,794,1,
  671,935,0,902,1101,0,1043,1101,1,1261,1101,0,1344,919,1,1350,919,1,1389,1079,1,1545,1079,1,1429,573,1,1392,412,0,1392,320,1,1392,226,0,
  1473,226,1,1554,226,0,1621,295,1,1689,364,0,1766,606,0,1766,753,1,1766,931,0,1689,1071,1,1612,1209,0,1322,1358,0,1128,1358,1,887,1358,0,
  700,1251,1,514,1144,0,302,741,0,302,491,1,302,297,0,381,151,1,459,3,0,756,-155,0,954,-155,1,1099,-155,0,1397,-80,0,1557,7,1,
  1612,-105,1,1467,-192,0,1128,-283,0,954,-283,1,713,-283,0,533,-187,1,352,-92,0,161,261,0,161,491,1,161,770,0,286,1000,1,411,1229,0,
  852,1484,0,1126,1484,1,1367,1484,0,1542,1394,1,1717,1303,0,1902,973,0,1296,747,1,1296,849,0,1164,974,0,1054,974,1,953,974,0,874,910,1,
  796,847,0,706,622,0,706,491,1,706,371,0,801,235,0,900,235,1,978,235,0,1121,321,0,1176,394,1,1215,446,0,1268,580,0,1296,707,0,
  364,412,1,202,0,1,4,0,1,579,1409,1,796,1409,1,1362,0,1,1167,0,1,1006,412,1,685,1265,1,668,1209,0,621,1072,0,601,1020,1,
  422,561,1,949,561,1,768,1026,1,754,1061,0,708,1189,0,1258,397,1,1258,209,0,984,0,0,740,0,1,168,0,1,168,1409,1,680,1409,1,
  1176,1409,0,1176,1067,1,1176,942,0,1036,772,0,908,743,1,1076,723,0,1258,538,0,984,1044,1,984,1158,0,828,1256,0,680,1256,1,359,1256,1,
  359,810,1,680,810,1,984,810,0,1065,412,1,1065,661,0,715,661,1,359,661,1,359,153,1,730,153,1,905,153,0,1065,283,0,800,137,1,
  1096,137,0,1245,430,1,1401,352,1,1314,170,0,999,-20,0,791,-20,1,577,-20,0,269,158,0,104,487,0,104,711,1,104,936,0,268,1258,0,
  576,1430,0,790,1430,1,1244,1430,0,1388,1081,1,1207,1021,1,1158,1144,0,941,1274,0,792,1274,1,558,1274,0,298,973,0,298,711,1,298,539,0,
  421,280,0,646,137,0,1381,719,1,1381,496,0,1208,174,0,899,0,0,695,0,1,168,0,1,168,1409,1,634,1409,1,992,1409,0,1381,1050,0,
  1189,719,1,1189,981,0,902,1256,0,630,1256,1,359,1256,1,359,153,1,673,153,1,830,153,0,1061,291,0,1189,545,0,1278,156,1,1278,0,1,
  168,0,1,168,1409,1,1237,1409,1,1237,1253,1,359,1253,1,359,801,1,1177,801,1,1177,647,1,359,647,1,359,156,1,359,571,1,359,0,1,
  168,0,1,168,1409,1,1169,1409,1,1169,1253,1,359,1253,1,359,729,1,1145,729,1,1145,571,1,813,135,1,951,135,0,1190,219,0,1264,291,1,
  1264,545,1,843,545,1,843,705,1,1440,705,1,1440,219,1,1328,105,0,1003,-20,0,813,-20,1,593,-20,0,275,158,0,103,487,0,103,711,1,
  103,1054,0,471,1430,0,804,1430,1,1038,1430,0,1330,1272,0,1409,1098,1,1227,1044,1,1167,1164,0,956,1274,0,799,1274,1,555,1274,0,297,979,0,
  297,711,1,297,444,0,571,135,0,359,653,1,359,0,1,168,0,1,168,1409,1,359,1409,1,359,813,1,1121,813,1,1121,1409,1,1312,1409,1,
  1312,0,1,1121,0,1,1121,653,1,380,1409,1,380,0,1,189,0,1,189,1409,1,457,-20,1,99,-20,0,32,350,1,219,381,1,237,265,0,
  363,135,0,458,135,1,562,135,0,682,278,0,682,416,1,682,1253,1,411,1253,1,411,1409,1,872,1409,1,872,420,1,872,215,0,650,-20,0,
  359,540,1,359,0,1,168,0,1,168,1409,1,359,1409,1,359,703,1,1038,1409,1,1263,1409,1,663,797,1,1343,0,1,1106,0,1,543,680,1,
  1071,156,1,1071,0,1,168,0,1,168,1409,1,359,1409,1,359,156,1,338,940,1,338,0,1,168,0,1,168,1409,1,419,1409,1,794,432,1,
  814,373,0,851,238,0,857,208,1,863,235,0,891,330,0,919,418,0,925,432,1,1293,1409,1,1538,1409,1,1538,0,1,1366,0,1,1366,940,1,
  1366,1096,0,1375,1240,1,1350,1151,0,1307,1010,0,1287,960,1,923,0,1,789,0,1,420,960,1,331,1240,1,333,1165,0,338,1015,0,338,936,1,
  338,0,1,168,0,1,168,1409,1,390,1409,1,1152,201,1,1140,398,0,1140,485,1,1140,1409,1,1312,1409,1,1312,0,1,1082,0,1,328,1200,1,
  330,1134,0,338,1002,0,1495,711,1,1495,485,0,1324,157,0,1010,-20,0,795,-20,1,579,-20,0,266,156,0,97,484,0,97,711,1,97,937,0,
  264,1258,0,577,1430,0,797,1430,1,1013,1430,0,1326,1260,0,1495,939,0,1300,711,1,1300,974,0,1037,1274,0,797,1274,1,555,1274,0,291,978,0,
  291,711,1,291,535,0,411,276,0,637,135,0,795,135,1,1039,135,0,1300,436,0,1258,985,1,1258,785,0,997,549,0,773,549,1,359,549,1,
  359,0,1,168,0,1,168,1409,1,761,1409,1,998,1409,0,1258,1187,0,1066,983,1,1066,1256,0,738,1256,1,359,1256,1,359,700,1,746,700,1,
  1066,700,0,1495,711,1,1495,513,0,1360,207,0,1106,18,0,928,-6,1,969,-132,0,1102,-244,0,1204,-244,1,1261,-244,0,1319,-231,1,1319,-365,1,
  1228,-387,0,1141,-387,1,990,-387,0,795,-216,0,733,-16,1,531,-6,0,247,178,0,97,498,0,97,711,1,97,937,0,264,1258,0,577,1430,0,
  797,1430,1,1019,1430,0,1331,1255,0,1495,933,0,1300,711,1,1300,974,0,1037,1274,0,797,1274,1,555,1274,0,291,978,0,291,711,1,291,535,0,
  411,276,0,637,135,0,795,135,1,1039,135,0,1300,436,0,359,585,1,359,0,1,168,0,1,168,1409,1,831,1409,1,1069,1409,0,1328,1196,0,
  1328,1006,1,1328,849,0,1145,635,0,984,607,1,1384,0,1,1164,0,1,798,585,1,1136,1004,1,1136,1127,0,969,1256,0,812,1256,1,359,1256,1,
  359,736,1,820,736,1,971,736,0,1136,877,0,1272,389,1,1272,194,0,967,-20,0,690,-20,1,175,-20,0,93,338,1,278,375,1,310,248,0,
  518,129,0,697,129,1,882,129,0,1083,256,0,1083,379,1,1083,448,0,1020,534,0,963,562,1,906,590,0,748,628,0,652,650,1,485,687,0,
  312,761,0,262,806,1,159,900,0,159,1053,1,159,1234,0,436,1430,0,694,1430,1,934,1430,0,1188,1283,0,1239,1106,1,1051,1073,1,1020,1185,0,
  846,1286,0,692,1286,1,523,1286,0,345,1174,0,345,1063,1,345,998,0,414,913,0,479,884,1,540,855,0,738,811,1,819,792,0,977,751,0,
  1047,720,1,1088,703,0,1163,654,0,1191,622,1,1229,580,0,1272,466,0,720,1253,1,720,0,1,530,0,1,530,1253,1,46,1253,1,46,1409,1,
  1204,1409,1,1204,1253,1,731,-20,1,558,-20,0,300,108,0,158,346,0,158,512,1,158,1409,1,349,1409,1,349,528,1,349,335,0,545,135,0,
  730,135,1,920,135,0,1131,342,0,1131,541,1,1131,1409,1,1321,1409,1,1321,530,1,1321,359,0,1176,113,0,911,-20,0,1357,1409,1,782,0,1,
  584,0,1,9,1409,1,210,1409,1,600,417,1,624,356,0,663,230,0,684,168,1,704,230,0,748,366,0,768,417,1,1156,1409,1,897,890,1,
  652,0,1,424,0,1,9,1409,1,208,1409,1,461,514,1,484,430,0,525,257,0,544,168,1,552,205,0,579,326,0,609,452,0,621,495,1,
  877,1409,1,1060,1409,1,1305,532,1,1333,425,0,1377,242,0,1393,168,1,1408,227,0,1441,364,0,1470,477,0,1476,501,1,1727,1409,1,1926,1409,1,
  1511,0,1,1283,0,1,1039,895,1,1027,937,0,992,1087,0,969,1196,1,960,1143,0,931,1020,0,904,916,0,689,616,1,257,0,1,46,0,1,
  582,732,1,87,1409,1,298,1409,1,690,856,1,1071,1409,1,1282,1409,1,800,739,1,1323,0,1,1112,0,1,777,584,1,777,0,1,587,0,1,
  587,584,1,45,1409,1,255,1409,1,684,738,1,1111,1409,1,1321,1409,1,1187,156,1,1187,0,1,65,0,1,65,143,1,923,1253,1,138,1253,1,
  138,1409,1,1140,1409,1,1140,1270,1,282,156,1,553,-296,1,553,-425,1,146,-425,1,146,1484,1,553,1484,1,553,1355,1,320,1355,1,320,-296,1,
  158,1484,1,569,-20,1,407,-20,1,0,1484,1,423,1484,1,423,-425,1,16,-425,1,16,-296,1,249,-296,1,249,1355,1,16,1355,1,16,1484,1,
  10,673,1,378,1409,1,581,1409,1,951,673,1,787,673,1,478,1306,1,172,673,1,1162,-277,1,1162,-407,1,-31,-407,1,-31,-277,1,530,1221,1,
  530,1201,1,436,1201,1,106,1479,1,106,1508,1,313,1508,1,414,-20,1,251,-20,0,87,152,0,87,302,1,87,470,0,308,650,0,554,656,1,
  797,660,1,797,719,1,797,851,0,685,965,0,565,965,1,444,965,0,334,883,0,323,793,1,135,810,1,181,1102,0,569,1102,1,773,1102,0,
  979,915,0,979,738,1,979,272,1,979,192,0,1021,111,0,1080,111,1,1108,111,0,1139,118,1,1139,6,1,1074,-10,0,1000,-10,1,900,-10,0,
  809,95,0,803,207,1,797,207,1,728,83,0,545,-20,0,797,534,1,600,530,1,474,529,0,408,504,1,342,480,0,272,380,0,272,299,1,
  272,211,0,367,115,0,455,115,1,555,115,0,631,160,1,708,205,0,797,362,0,797,445,1,1053,546,1,1053,-20,0,655,-20,1,532,-20,0,
  369,69,0,318,168,1,316,168,1,316,148,0,312,72,0,308,7,0,306,0,1,132,0,1,138,54,0,138,223,1,138,1484,1,318,1484,1,
  318,1061,1,318,1029,0,316,952,0,314,908,1,318,908,1,368,1012,0,533,1102,0,655,1102,1,860,1102,0,1053,826,0,864,540,1,864,767,0,
  744,963,0,609,963,1,457,963,0,318,755,0,318,529,1,318,316,0,454,113,0,607,113,1,743,113,0,864,314,0,553,-20,1,326,-20,0,
  87,267,0,87,542,1,87,815,0,327,1102,0,551,1102,1,717,1102,0,936,930,0,964,779,1,779,765,1,765,855,0,651,961,0,546,961,1,
  403,961,0,275,771,0,275,546,1,275,330,0,411,122,0,548,122,1,644,122,0,773,226,0,788,334,1,970,322,1,949,166,0,725,-20,0,
  835,0,1,833,11,0,828,72,0,825,146,0,825,174,1,821,174,1,771,70,0,606,-20,0,484,-20,1,279,-20,0,86,256,0,86,536,1,
  86,1102,0,484,1102,1,607,1102,0,771,1012,0,821,914,1,823,914,1,821,1035,1,821,1484,1,1001,1484,1,1001,223,1,1001,54,0,1007,0,1,
  275,542,1,275,315,0,395,119,0,530,119,1,683,119,0,821,331,0,821,554,1,821,769,0,683,969,0,532,969,1,396,969,0,275,768,0,
  276,503,1,276,317,0,430,115,0,578,115,1,695,115,0,836,209,0,861,281,1,1019,236,1,922,-20,0,578,-20,1,338,-20,0,87,266,0,
  87,548,1,87,816,0,338,1102,0,571,1102,1,1048,1102,0,1048,527,1,1048,503,1,862,641,1,847,812,0,703,969,0,568,969,1,437,969,0,
  284,794,0,278,641,1,361,951,1,361,0,1,181,0,1,181,951,1,29,951,1,29,1082,1,181,1082,1,181,1204,1,181,1352,0,311,1482,0,
  445,1482,1,520,1482,0,572,1470,1,572,1333,1,524,1341,0,492,1341,1,423,1341,0,361,1271,0,361,1179,1,361,1082,1,572,1082,1,572,951,1,
  548,-425,1,371,-425,0,161,-286,0,131,-158,1,312,-132,1,349,-288,0,553,-288,1,822,-288,0,822,27,1,822,201,1,820,201,1,769,97,0,
  591,-8,0,472,-8,1,273,-8,0,86,256,0,86,539,1,86,826,0,287,1099,0,492,1099,1,607,1099,0,776,994,0,822,897,1,824,897,1,
  824,917,0,828,1002,0,834,1078,0,836,1082,1,1007,1082,1,1001,1028,0,1001,858,1,1001,31,1,1001,-425,0,822,541,1,822,738,0,661,965,0,
  536,965,1,398,965,0,272,765,0,272,541,1,272,319,0,390,125,0,533,125,1,661,125,0,822,348,0,317,897,1,375,1003,0,538,1102,0,
  663,1102,1,839,1102,0,1006,919,0,1006,721,1,1006,0,1,825,0,1,825,686,1,825,833,0,722,963,0,602,963,1,475,963,0,322,787,0,
  322,638,1,322,0,1,142,0,1,142,1484,1,322,1484,1,322,1098,1,322,1058,0,318,969,0,315,904,0,314,897,1,317,1484,1,317,1312,1,
  137,1312,1,137,1484,1,317,1082,1,317,0,1,137,0,1,137,1082,1,317,1484,1,317,1312,1,137,1312,1,137,1484,1,77,-425,1,-1,-425,0,
  -50,-416,1,-50,-277,1,-34,-279,0,-4,-283,0,12,-283,1,81,-283,0,137,-211,0,137,-107,1,137,1082,1,317,1082,1,317,-134,1,317,-287,0,
  197,-425,0,318,385,1,318,0,1,138,0,1,138,1484,1,318,1484,1,318,557,1,793,1082,1,1004,1082,1,565,617,1,1027,0,1,816,0,1,
  450,494,1,318,1484,1,318,0,1,138,0,1,138,1484,1,321,627,1,321,0,1,142,0,1,142,851,1,142,1040,0,136,1082,1,306,1082,1,
  309,1049,0,314,941,0,314,897,1,317,897,1,375,1012,0,525,1102,0,633,1102,1,756,1102,0,899,1004,0,927,897,1,930,897,1,986,1006,0,
  1145,1102,0,1258,1102,1,1422,1102,0,1571,924,0,1571,721,1,1571,0,1,1393,0,1,1393,686,1,1393,843,0,1307,963,0,1195,963,1,1077,963,0,
  946,788,0,946,627,1,946,0,1,768,0,1,768,686,1,768,843,0,682,963,0,570,963,1,455,963,0,321,787,0,322,627,1,322,0,1,
  142,0,1,142,851,1,142,1040,0,136,1082,1,306,1082,1,309,1053,0,313,954,0,314,897,1,317,897,1,379,1009,0,542,1102,0,663,1102,1,
  841,1102,0,1006,920,0,1006,721,1,1006,0,1,825,0,1,825,686,1,825,841,0,725,963,0,602,963,1,472,963,0,322,785,0,1053,542,1,
  1053,258,0,803,-20,0,565,-20,1,328,-20,0,86,269,0,86,542,1,86,1102,0,571,1102,1,819,1102,0,1053,829,0,864,542,1,864,766,0,
  731,969,0,574,969,1,416,969,0,275,762,0,275,542,1,275,328,0,414,113,0,563,113,1,725,113,0,864,321,0,1053,546,1,1053,-20,0,
  655,-20,1,405,-20,0,319,168,1,314,168,1,318,161,0,318,-2,1,318,-425,1,138,-425,1,138,861,1,138,1028,0,132,1082,1,306,1082,1,
  307,1078,0,312,1008,0,316,927,0,316,908,1,320,908,1,368,1008,0,526,1101,0,655,1101,1,855,1101,0,1053,833,0,864,542,1,864,768,0,
  742,962,0,609,962,1,461,962,0,318,757,0,318,528,1,318,315,0,454,113,0,607,113,1,741,113,0,864,310,0,86,536,1,86,1102,0,
  484,1102,1,607,1102,0,769,1015,0,823,914,1,823,934,0,827,1019,0,833,1093,0,835,1096,1,1008,1096,1,1004,1067,0,1001,919,0,1001,801,1,
  1001,-425,1,821,-425,1,821,14,1,825,178,1,823,178,1,769,71,0,611,-20,0,484,-20,1,278,-20,0,86,258,0,821,554,1,821,765,0,
  683,969,0,532,969,1,395,969,0,275,765,0,275,542,1,275,315,0,396,119,0,530,119,1,683,119,0,821,337,0,322,564,1,322,0,1,
  142,0,1,142,830,1,142,944,0,136,1082,1,306,1082,1,310,990,0,314,880,0,314,861,1,318,861,1,348,959,0,384,1013,1,419,1063,0,
  509,1102,0,575,1102,1,612,1102,0,648,1092,1,648,927,1,614,937,0,552,937,1,440,937,0,322,744,0,950,299,1,950,146,0,719,-20,0,
  511,-20,1,309,-20,0,90,113,0,57,254,1,216,285,1,261,117,0,511,117,1,648,117,0,775,201,0,775,285,1,775,349,0,687,429,0,
  589,455,1,473,486,0,336,526,0,262,555,0,240,568,1,174,606,0,100,716,0,100,796,1,100,944,0,311,1099,0,513,1099,1,692,1099,0,
  903,973,0,931,834,1,769,814,1,738,963,0,513,963,1,275,963,0,275,814,1,275,759,0,341,688,0,434,665,1,559,635,0,785,564,0,
  834,529,1,891,488,0,950,377,0,554,8,1,466,-16,0,372,-16,1,156,-16,0,156,229,1,156,951,1,31,951,1,31,1082,1,163,1082,1,
  216,1324,1,336,1324,1,336,1082,1,536,1082,1,536,951,1,336,951,1,336,268,1,336,190,0,387,127,0,450,127,1,484,127,0,554,141,1,
  822,185,1,760,73,0,597,-20,0,476,-20,1,298,-20,0,133,157,0,133,361,1,133,1082,1,314,1082,1,314,396,1,314,234,0,415,119,0,
  537,119,1,667,119,0,817,297,0,817,455,1,817,1082,1,997,1082,1,997,231,1,997,42,0,1003,0,1,833,0,1,832,5,0,827,89,0,
  825,185,1,1017,1082,1,613,0,1,400,0,1,7,1082,1,199,1082,1,437,378,1,451,338,0,506,141,1,525,200,0,564,329,0,580,376,1,
  826,1082,1,704,765,1,508,0,1,300,0,1,-3,1082,1,175,1082,1,361,333,1,369,301,0,391,203,0,401,149,1,412,196,0,435,290,0,
  447,336,1,644,1082,1,837,1082,1,1026,339,1,1038,292,0,1061,196,0,1072,149,1,1083,197,0,1106,302,0,1116,342,1,1308,1082,1,1484,1082,1,
  1174,0,1,965,0,1,776,765,1,766,807,0,749,892,0,740,934,1,736,912,0,715,807,0,510,444,1,217,0,1,23,0,1,408,556,1,
  41,1082,1,240,1082,1,510,661,1,778,1082,1,979,1082,1,612,558,1,1002,0,1,801,0,1,191,-425,1,119,-425,0,67,-414,1,67,-279,1,
  105,-285,0,151,-285,1,235,-285,0,378,-160,0,434,5,1,5,1082,1,197,1082,1,425,484,1,450,420,0,507,258,0,523,196,1,548,267,0,
  599,407,0,623,477,1,830,1082,1,1020,1082,1,604,0,1,570,-86,0,508,-215,0,479,-257,1,365,-425,0,922,139,1,922,0,1,83,0,1,
  83,137,1,688,943,1,117,943,1,117,1082,1,901,1082,1,901,945,1,295,139,1,513,-425,1,386,-425,0,249,-271,0,249,-132,1,249,229,1,
  249,346,0,144,461,0,34,466,1,34,593,1,249,600,0,249,829,1,249,1191,1,249,1332,0,382,1484,0,513,1484,1,648,1484,1,648,1355,1,
  585,1355,1,494,1355,0,417,1248,0,417,1140,1,417,784,1,417,690,0,311,553,0,223,532,1,223,530,1,312,509,0,417,373,0,417,276,1,
  417,-81,1,417,-187,0,494,-296,0,585,-296,1,648,-296,1,648,-425,1,349,1484,1,349,-434,1,183,-434,1,183,1484,1,34,-296,1,94,-296,1,
  185,-296,0,264,-187,0,264,-81,1,264,276,1,264,374,0,368,510,0,457,530,1,457,532,1,371,552,0,264,687,0,264,784,1,264,1140,1,
  264,1248,0,185,1355,0,94,1355,1,34,1355,1,34,1484,1,166,1484,1,297,1484,0,430,1332,0,430,1191,1,430,829,1,430,600,0,647,593,1,
  647,466,1,430,459,0,430,229,1,430,-132,1,430,-270,0,293,-425,0,166,-425,1,34,-425,1,844,553,1,775,553,0,630,597,0,557,623,1,
  428,668,0,340,668,1,273,668,0,157,627,0,92,580,1,92,723,1,203,807,0,355,807,1,409,807,0,471,794,1,503,787,0,599,758,0,
  664,735,1,695,723,0,815,690,0,860,690,1,989,690,0,1104,782,1,1104,633,1,1046,591,0,929,553,0,496,1479,1,166,1201,1,72,1201,1,
  72,1221,1,289,1508,1,496,1508,1,
};
/* TEXTGLYPHS_END */

/* ---- Bitmap_Text rasterizer ------------------------------------------------------------------ */
/* Distance from the y given to the canvas adapter (textBaseline 'top') to the baseline, in em/2048. */
#define WZ3__TEXT_TOP_BASELINE_NUM 1589
#define WZ3__TEXT_TOP_BASELINE_DEN 2048
/* The original asks GDI for Arial; the JS port asks a canvas. Neither is reproducible, so this is
   an analytic exact-area coverage rasterizer over the Arimo outlines (metric compatible with Arial)
   with the same layout rules as the JavaScript port's canvas adapter: text is drawn
   into a 4x supersampled alpha surface and box filtered. No hinting and no kerning (GDI's
   TextOut does not kern either). */

static inline int32_t wz3__text_trunc(double v)
{
  if (v >= 1.0e9) return 1000000000;
  if (v <= -1.0e9) return -1000000000;
  return (int32_t)v;
}
static inline int32_t wz3__text_round(double v) { return wz3__text_trunc(v + 0.5 < 0.0 ? v + 0.5 - 1.0 : v + 0.5); }

static inline float wz3__tx_floor(float v)
{
  const int32_t i = (int32_t)v;
  const float f = (float)i;
  return f > v ? f - 1.0f : f;
}
static inline float wz3__tx_ceil(float v)
{
  const int32_t i = (int32_t)v;
  const float f = (float)i;
  return f < v ? f + 1.0f : f;
}
static inline float wz3__tx_min(float a, float b) { return a < b ? a : b; }
static inline float wz3__tx_max(float a, float b) { return a > b ? a : b; }

/* Signed-area accumulation: a[] holds per-pixel coverage deltas; a running sum along each row turns
   them into coverage. Lines must lie inside x in [0, stride-2], y arbitrary. */
static void wz3__acc_line(float *a, int32_t stride, int32_t rows, float x0, float y0, float x1, float y1)
{
  if (y0 == y1) return;
  float dir = 1.0f;
  if (y0 > y1) {
    float t = x0; x0 = x1; x1 = t;
    t = y0; y0 = y1; y1 = t;
    dir = -1.0f;
  }
  const float dxdy = (x1 - x0) / (y1 - y0);
  float x = x0;
  if (y0 < 0.0f) x -= y0 * dxdy;
  const int32_t first = y0 < 0.0f ? 0 : (int32_t)y0;
  int32_t last = (int32_t)wz3__tx_ceil(y1);
  if (last > rows) last = rows;
  for (int32_t y = first; y < last; y++) {
    float *line = a + (size_t)y * (size_t)stride;
    const float dy = wz3__tx_min((float)(y + 1), y1) - wz3__tx_max((float)y, y0);
    const float xnext = x + dxdy * dy;
    const float d = dy * dir;
    const float xa = x < xnext ? x : xnext;
    const float xb = x < xnext ? xnext : x;
    const float xa_floor = wz3__tx_floor(xa);
    const int32_t xa_i = (int32_t)xa_floor;
    const float xb_ceil = wz3__tx_ceil(xb);
    const int32_t xb_i = (int32_t)xb_ceil;
    if (xb_i <= xa_i + 1) {
      const float xmf = 0.5f * (x + xnext) - xa_floor;
      line[xa_i] += d - d * xmf;
      line[xa_i + 1] += d * xmf;
    } else {
      const float s = 1.0f / (xb - xa);
      const float xaf = xa - xa_floor;
      const float a0 = 0.5f * s * (1.0f - xaf) * (1.0f - xaf);
      const float xbf = xb - xb_ceil + 1.0f;
      const float am = 0.5f * s * xbf * xbf;
      line[xa_i] += d * a0;
      if (xb_i == xa_i + 2) {
        line[xa_i + 1] += d * (1.0f - a0 - am);
      } else {
        const float a1 = s * (1.5f - xaf);
        line[xa_i + 1] += d * (a1 - a0);
        for (int32_t xi = xa_i + 2; xi < xb_i - 1; xi++) line[xi] += d * s;
        const float a2 = a1 + (float)(xb_i - xa_i - 3) * s;
        line[xb_i - 1] += d * (1.0f - a2 - am);
      }
      line[xb_i] += d * am;
    }
    x = xnext;
  }
}

/* clips to the columns [0, width]: pieces left of the surface keep their winding at column 0,
   pieces right of it cannot influence any visible pixel */
static void wz3__acc_segment(float *a, int32_t stride, int32_t rows, float width, float x0, float y0, float x1,
                             float y1)
{
  if (x0 <= 0.0f && x1 <= 0.0f) { wz3__acc_line(a, stride, rows, 0.0f, y0, 0.0f, y1); return; }
  if (x0 >= width && x1 >= width) return;
  if (x0 < 0.0f || x1 < 0.0f || x0 > width || x1 > width) {
    const float bound = (x0 < 0.0f || x1 < 0.0f) ? 0.0f : width;
    const float t = (bound - x0) / (x1 - x0);
    const float ym = y0 + (y1 - y0) * t;
    wz3__acc_segment(a, stride, rows, width, x0, y0, bound, ym);
    wz3__acc_segment(a, stride, rows, width, bound, ym, x1, y1);
    return;
  }
  wz3__acc_line(a, stride, rows, x0, y0, x1, y1);
}

typedef struct wz3__tx_surface {
  float *a;
  int32_t stride, rows;
  float width;   /* stride - 2 */
  float ox, oy;  /* origin of the surface in canvas space */
} wz3__tx_surface;

static void wz3__tx_quad(wz3__tx_surface *s, float x0, float y0, float x1, float y1, float x2, float y2)
{
  const float ddx = x0 - 2.0f * x1 + x2, ddy = y0 - 2.0f * y1 + y2;
  const float deviation = sqrtf(ddx * ddx + ddy * ddy) * 0.25f;
  int32_t n = 1;
  while (n < 64 && deviation > 0.02f * (float)(n * n)) n++;
  float px = x0, py = y0;
  for (int32_t i = 1; i <= n; i++) {
    const float t = (float)i / (float)n, u = 1.0f - t;
    const float qx = (u * u) * x0 + (2.0f * u * t) * x1 + (t * t) * x2;
    const float qy = (u * u) * y0 + (2.0f * u * t) * y1 + (t * t) * y2;
    const float ex = i == n ? x2 : qx, ey = i == n ? y2 : qy;
    wz3__acc_segment(s->a, s->stride, s->rows, s->width, px - s->ox, py - s->oy, ex - s->ox, ey - s->oy);
    px = ex; py = ey;
  }
}

static const wz3__glyph *wz3__find_glyph(uint32_t code)
{
  size_t lo = 0, hi = sizeof wz3__glyphs / sizeof wz3__glyphs[0];
  while (lo < hi) {
    const size_t mid = (lo + hi) / 2;
    if (wz3__glyphs[mid].code < code) lo = mid + 1;
    else hi = mid;
  }
  return lo < sizeof wz3__glyphs / sizeof wz3__glyphs[0] && wz3__glyphs[lo].code == code ? &wz3__glyphs[lo] : NULL;
}

/* UTF-8 decode: ASCII and two byte sequences (U+00B4 occurs in the party document), else '?' */
static uint32_t wz3__utf8_next(const char **p)
{
  const uint8_t *s = (const uint8_t *)*p;
  uint32_t c = s[0];
  if (c < 0x80) { *p += 1; return c; }
  if ((c & 0xe0) == 0xc0 && (s[1] & 0xc0) == 0x80) { *p += 2; return ((c & 0x1f) << 6) | (s[1] & 0x3f); }
  /* anything else (including single Latin-1 bytes) consumes one byte */
  *p += 1;
  return c >= 0xa0 ? c : 0x3f;
}

static double wz3__text_advance_units(const char *text, size_t length)
{
  double sum = 0.0;
  const char *p = text, *end = text + length;
  while (p < end) {
    const wz3__glyph *g = wz3__find_glyph(wz3__utf8_next(&p));
    sum += g ? (double)g->advance : (double)wz3__find_glyph(' ')->advance;
  }
  return sum;
}

/* One glyph into the 4x alpha surface: origin (ox, baseline) in surface pixels, scale sx/sy per font unit. */
static int wz3__text_draw_glyph(uint8_t *alpha, int32_t aw, int32_t ah, const wz3__glyph *g, float ox, float oy,
                                float sx, float sy)
{
  if (!g->n_points) return 1;
  const int16_t *pt = wz3__glyph_points + (size_t)g->first_point * 3;
  float minx = 1e30f, miny = 1e30f, maxx = -1e30f, maxy = -1e30f;
  for (uint32_t i = 0; i < g->n_points; i++) {
    const float x = ox + (float)pt[i * 3] * sx, y = oy - (float)pt[i * 3 + 1] * sy;
    if (x < minx) minx = x;
    if (x > maxx) maxx = x;
    if (y < miny) miny = y;
    if (y > maxy) maxy = y;
  }
  int32_t bx0 = (int32_t)wz3__tx_floor(minx), by0 = (int32_t)wz3__tx_floor(miny);
  int32_t bx1 = (int32_t)wz3__tx_ceil(maxx), by1 = (int32_t)wz3__tx_ceil(maxy);
  if (bx0 < 0) bx0 = 0;
  if (by0 < 0) by0 = 0;
  if (bx1 > aw) bx1 = aw;
  if (by1 > ah) by1 = ah;
  if (bx1 <= bx0 || by1 <= by0) return 1;
  const int32_t cw = bx1 - bx0, ch = by1 - by0;
  wz3__tx_surface s;
  s.stride = cw + 2;
  s.rows = ch;
  s.width = (float)cw;
  s.ox = (float)bx0;
  s.oy = (float)by0;
  s.a = (float *)WZ3_MALLOC((size_t)s.stride * (size_t)ch * sizeof(float));
  if (!s.a) return 0;
  memset(s.a, 0, (size_t)s.stride * (size_t)ch * sizeof(float));

  const uint16_t *ends = wz3__glyph_ends + g->first_end;
  uint32_t start = 0;
  for (uint32_t c = 0; c < g->n_ends; c++) {
    const uint32_t end = (uint32_t)ends[c] - g->first_point; /* inclusive, relative to the glyph */
    const uint32_t n = end - start + 1;
    if (n >= 2) {
      float cx[2 * 512], cy[2 * 512];
      uint8_t on[2 * 512];
      uint32_t count = 0;
      /* expand to a closed list in which two off-curve points get an implied on-curve midpoint */
      for (uint32_t i = 0; i < n && count + 2 < 2 * 512; i++) {
        const uint32_t cur = start + i, nxt = start + (i + 1) % n;
        const float x = ox + (float)pt[cur * 3] * sx, y = oy - (float)pt[cur * 3 + 1] * sy;
        const int is_on = pt[cur * 3 + 2];
        cx[count] = x; cy[count] = y; on[count] = (uint8_t)is_on; count++;
        if (!is_on && !pt[nxt * 3 + 2]) {
          const float nx = ox + (float)pt[nxt * 3] * sx, ny = oy - (float)pt[nxt * 3 + 1] * sy;
          cx[count] = (x + nx) * 0.5f; cy[count] = (y + ny) * 0.5f; on[count] = 1; count++;
        }
      }
      /* rotate so the first point is on-curve */
      uint32_t first_on = 0;
      while (first_on < count && !on[first_on]) first_on++;
      if (first_on < count) {
        float px = cx[first_on], py = cy[first_on];
        for (uint32_t i = 1; i <= count; i++) {
          const uint32_t k = (first_on + i) % count;
          if (on[k]) {
            wz3__acc_segment(s.a, s.stride, s.rows, s.width, px - s.ox, py - s.oy, cx[k] - s.ox, cy[k] - s.oy);
            px = cx[k]; py = cy[k];
          } else {
            const uint32_t k2 = (first_on + i + 1) % count;
            wz3__tx_quad(&s, px, py, cx[k], cy[k], cx[k2], cy[k2]);
            px = cx[k2]; py = cy[k2];
            i++;
          }
        }
      }
    }
    start = end + 1;
  }

  for (int32_t y = 0; y < ch; y++) {
    const float *line = s.a + (size_t)y * (size_t)s.stride;
    uint8_t *row = alpha + (size_t)(by0 + y) * (size_t)aw + (size_t)bx0;
    float acc = 0.0f;
    for (int32_t x = 0; x < cw; x++) {
      acc += line[x];
      float cov = acc < 0.0f ? -acc : acc;
      if (cov > 1.0f) cov = 1.0f;
      const uint32_t src = (uint32_t)wz3__text_trunc((double)(cov * 255.0f + 0.5f));
      if (src) {
        const uint32_t dst = row[x];
        row[x] = (uint8_t)(src + (dst * (255u - src) + 127u) / 255u);
      }
    }
  }
  WZ3_FREE(s.a);
  return 1;
}

WZ3_API int wz3_text_default(void *user, const wz3_text_request *req, wz3_text_result *out)
{
  (void)user;
  if (!req || !out || req->width < 1 || req->height < 1 || req->width > 4096 || req->height > 4096) return 0;
  const int32_t aw = req->width * 4, ah = req->height * 4;
  const size_t surface = (size_t)aw * (size_t)ah;
  uint8_t *alpha = (uint8_t *)WZ3_MALLOC(surface);
  uint8_t *coverage = (uint8_t *)WZ3_MALLOC((size_t)req->width * (size_t)req->height);
  if (!alpha || !coverage) {
    WZ3_FREE(alpha);
    WZ3_FREE(coverage);
    return 0;
  }
  memset(alpha, 0, surface);

  /* FontBegin takes integer sizes: truncate exactly like the adapter */
  int32_t pixel_size = wz3__text_trunc((double)req->text_width * (double)ah);
  if (pixel_size < 1) pixel_size = 1;
  const int32_t requested_glyph_width = wz3__text_trunc((double)req->text_height * (double)aw);
  const double per_unit = (double)pixel_size / (double)WZ3__FONT_UNITS_PER_EM;
  double horizontal_scale = 1.0;
  if (requested_glyph_width > 0) {
    static const char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";
    const double average = wz3__text_advance_units(alphabet, sizeof alphabet - 1) * per_unit / 52.0;
    if (average > 0.0) horizontal_scale = (double)requested_glyph_width / average;
  }
  const int32_t ascent_px = wz3__text_round((double)pixel_size * WZ3__FONT_ASCENT / WZ3__FONT_UNITS_PER_EM);
  const int32_t descent_px = wz3__text_round((double)pixel_size * WZ3__FONT_DESCENT / WZ3__FONT_UNITS_PER_EM);
  int32_t line_height = ascent_px + descent_px;
  if (line_height < 1) line_height = 1;
  /* canvas 'top' baseline: the pen sits this far below the y it is given */
  const double baseline_offset = (double)pixel_size * WZ3__TEXT_TOP_BASELINE_NUM / WZ3__TEXT_TOP_BASELINE_DEN;
  const int32_t line_advance = wz3__text_trunc((double)line_height * (double)req->line_skip);
  const double external = (double)req->external_space * 4.0 + 8.0;

  const char *text = req->text ? req->text : "";
  int32_t lines = 1;
  for (const char *c = text; *c; c++) if (*c == '\n') lines++;
  int32_t y = wz3__text_trunc((double)req->y * (double)ah + external);
  if (req->flags & 2u) y -= wz3__text_trunc((double)line_height * (double)lines / 2.0);

  const char *line = text;
  int ok = 1;
  for (int32_t li = 0; li < lines && ok; li++) {
    const char *eol = line;
    while (*eol && *eol != '\n') eol++;
    const size_t length = (size_t)(eol - line);
    const double width_units = wz3__text_advance_units(line, length);
    const int32_t measured = wz3__text_round(width_units * per_unit * horizontal_scale);
    int32_t x = wz3__text_trunc((double)req->x * (double)aw + external);
    if (req->flags & 1u) x -= wz3__text_trunc((double)measured / 2.0);
    const float sx = (float)(per_unit * horizontal_scale), sy = (float)per_unit;
    double pen = 0.0;
    const char *p = line;
    while (p < eol && ok) {
      const wz3__glyph *g = wz3__find_glyph(wz3__utf8_next(&p));
      if (!g) g = wz3__find_glyph(' ');
      ok = wz3__text_draw_glyph(alpha, aw, ah, g, (float)((double)x + pen * horizontal_scale),
                                (float)((double)y + baseline_offset), sx, sy);
      pen += (double)g->advance * per_unit;
    }
    y += line_advance;
    line = *eol ? eol + 1 : eol;
  }
  if (!ok) {
    WZ3_FREE(alpha);
    WZ3_FREE(coverage);
    return 0;
  }

  for (int32_t py = 0; py < req->height; py++)
    for (int32_t px = 0; px < req->width; px++) {
      uint32_t sum = 0;
      for (int32_t ay = 0; ay < 4; ay++) {
        const uint8_t *row = alpha + (size_t)(py * 4 + ay) * (size_t)aw + (size_t)(px * 4);
        sum += (uint32_t)row[0] + row[1] + row[2] + row[3];
      }
      coverage[(size_t)py * (size_t)req->width + (size_t)px] = (uint8_t)(sum >> 4);
    }
  WZ3_FREE(alpha);
  out->coverage = coverage;
  out->line_height = line_height;
  return 1;
}

static inline uint32_t wz3__u16le(const uint8_t *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8); }
static inline int32_t wz3__s16le(const uint8_t *p) { return (int16_t)(uint16_t)wz3__u16le(p); }

WZ3_API wz3_bitmap *wz3_bitmap_text(const wz3_bitmap_ctx *ctx, const uint8_t *blob, size_t blob_size,
                                    const wz3_bitmap *in, float x, float y, float width, float height,
                                    uint32_t color, int32_t flags, float external_space,
                                    float internal_space, float line_skip, const char *text,
                                    const char *font, wz3_letter letters[256])
{
  if (!in) return NULL;
  const size_t pixels = (size_t)in->width * (size_t)in->height;
  const int32_t page = (flags & 0x70) >> 4;
  uint8_t *coverage = NULL;
  int have_metrics = 0;
  int32_t line_height = 0;
  int32_t mx[256], my[256], mb[256], mw[256], ma[256];
  uint16_t col[4];
  wz3_packed_color(color, col);
  if (letters) memset(letters, 0, 256 * sizeof(wz3_letter));

  if ((flags & 0x80) && blob && blob_size >= 8 && blob[0] == 3 && blob[1] == (page ? 1 : 0) &&
      wz3__u16le(blob + 2) == (uint32_t)in->width && wz3__u16le(blob + 4) == (uint32_t)in->height) {
    /* cached glyph coverage from the document (5 bits per pixel) */
    line_height = (int32_t)wz3__u16le(blob + 6);
    size_t offset = 8;
    if (page) {
      if (blob_size < offset + 256 * 10) return NULL;
      for (int i = 0; i < 256; i++, offset += 10) {
        mx[i] = (int32_t)wz3__u16le(blob + offset);
        my[i] = (int32_t)wz3__u16le(blob + offset + 2);
        mb[i] = wz3__s16le(blob + offset + 4);
        mw[i] = wz3__s16le(blob + offset + 6);
        ma[i] = wz3__s16le(blob + offset + 8);
      }
      have_metrics = 1;
    }
    coverage = (uint8_t *)WZ3_MALLOC(pixels);
    if (!coverage) return NULL;
    memset(coverage, 0, pixels);
    for (size_t i = 0; i < pixels && offset + i < blob_size; i++) {
      uint32_t five = blob[offset + i];
      coverage[i] = (uint8_t)((five << 5) | (five << 2) | (five >> 1));
    }
  } else {
    if (!ctx || !ctx->text) return NULL;
    wz3_text_request req;
    wz3_text_result res;
    memset(&res, 0, sizeof res);
    req.width = in->width; req.height = in->height;
    req.x = x; req.y = y; req.text_width = width; req.text_height = height;
    req.color = color; req.flags = (uint32_t)flags;
    req.external_space = external_space; req.internal_space = internal_space; req.line_skip = line_skip;
    req.text = text ? text : ""; req.font = font ? font : ""; req.page = page;
    if (!ctx->text(ctx->text_user, &req, &res) || !res.coverage) {
      if (res.coverage) WZ3_FREE(res.coverage);
      return NULL;
    }
    coverage = res.coverage;
    line_height = res.line_height;
  }

  wz3_bitmap *b = wz3__blank_like(in);
  if (!b) {
    WZ3_FREE(coverage);
    return NULL;
  }
  for (size_t i = 0; i < pixels; i++)
    wz3__fade_pixel(b->data + i * 4, in->data + i * 4, col, (int32_t)(coverage[i] | (coverage[i] << 8)));
  WZ3_FREE(coverage);

  if (page && letters && have_metrics) {
    const double alias_width = (double)in->width * 4.0, alias_height = (double)in->height * 4.0;
    const double inside = (double)internal_space * alias_width;
    for (int i = 0; i < 256; i++) {
      letters[i].u0 = mx[i] / alias_width;
      letters[i].v0 = my[i] / alias_height;
      letters[i].u1 = (mx[i] + mw[i] + inside * 2) / alias_width;
      letters[i].v1 = (my[i] + line_height + inside * 2) / alias_height;
      letters[i].pre_space = (mb[i] - inside) / alias_width;
      letters[i].width = (mb[i] + mw[i] + ma[i] + inside * 2) / alias_width;
    }
  }
  return b;
}

/* The released debris KX has exactly one Import: fvs32.jpg, a 661-byte
   baseline JPEG whose decoded lower-power image is 16x16. The decoded pixels
   are embedded so graph initialization needs no image decoder; other imports
   use the host's decoder. */
static const char wz3__debris_import_rgba[] =
  "hE6w/7GO4v++wPn/ssfy/7LH9P+yx/T/ssf0/7LH9P+yx/T/ssf0/7TG9P+0xvT/ssf0/8C/+P+3jNv/jEul/7aN3P/Otv//k57e/4ek3v+IpeH/h6Tg/4ek4P+HpOD/hqPd/4mj3P+Ko9v/iqPb/4mj3v+Und7/4cX//7qM1//Sxu7/m5zS/0Ngrv83Yr7/NmO+/zZjvv83Yr7/N2O8/zljuf86Y7P/PmKs/z5isP86Yrr/QWCy/5ic2f/Gvu//y8zh/5Shzf88Yrf/MGPO/y9kzv8vZM7/MGPM/zJkxf82ZLr/NF6y/9j///83YsD/M2LK/zlhuf+QodX/xM3s/8vL4/+Uoc3/PWO4/zBjzv8wY87/MGPM/zJjyf82ZLr/O16g/9////83Y7z/M2PJ/zJjzP89Yrr/k6HQ/8jM5//Ly+P/laDN/z5hs/8zY8f/M2PH/zZiwf85Y7X/OF+k/+P///9BY6L/OWK6/zNjx/8zY8f/PWKz/5Wgzf/Ly+X/y8vj/5egyf9DYan/OmK6/zpiuf8+Yqz/Q16N/+n///9DY6D/PmKs/zxitf86Yrr/OWO5/0Fiqf+XoMn/ysrk/8zM5P+YoMT/SmCZ/+L///9DYqX/5v///wACI//t////RGKe/9////9DYaf/4P///93///9EYaP/l6DH/8vL5f/Ly+P/m6C9/wAAJf/u////AAEp/05hif8AASz/SWGR/wACOf9HYpn/AAEw/wABM//g////RWCj/5egx//Ly+X/y8vj/5ygu/8AACD/UmCD//D////t////S2GS/+f///9LYZL/7f///1Jhgv/t////QWGq/0Nhqf+XoMn/y8vl/8vL4/+boL7/UGKK/wABMP/p////SmKS/wABNf9KYpL/AAEn/1Fgf/8AAR7/6////zxitf89YLL/laDN/8vL5f/Ly+P/mKDE/0pgmf8AAkL/R2Ga/+n///9LYIv/7v////H////x////7f///0VhoP83Yr7/PGG5/5Kg0f/IzOn/y8vj/5Why/9AYa7/OmO1/wACQv9MYYz/9f////j////0////6////0Biqv83Yr7/M2PJ/zpiuv+QodX/xM3u/9LG7P+bnc7/QWGq/zpksP9EYp7/AAEr/wABFf8AABD/AAEb/0thkP8+Yq7/N2O8/zZkuv89Yav/lJ/V/8LB6/+0kND/49H//6u/8f+Fp9T/iqXU/46jzv+Ro8f/k6PF/5Gjyf+Oo9D/iqPb/4al3P+Dp9f/qcHv/93W//+Vfrb/glKe/5h7u/+xv+L/rczo/7HI8f+yx/T/s8j1/7LH9P+yx/T/ssf0/7LH9P+wyfH/rczo/67B4f+RgLb/eleX/w==";

/* decodes standard base64 into out; returns the byte count */
static size_t wz3__base64(const char *in, uint8_t *out, size_t capacity)
{
  size_t n = 0;
  uint32_t acc = 0;
  int bits = 0;
  for (; *in && *in != '='; in++) {
    int v;
    char ch = *in;
    if (ch >= 'A' && ch <= 'Z') v = ch - 'A';
    else if (ch >= 'a' && ch <= 'z') v = ch - 'a' + 26;
    else if (ch >= '0' && ch <= '9') v = ch - '0' + 52;
    else if (ch == '+') v = 62;
    else if (ch == '/') v = 63;
    else continue;
    acc = (acc << 6) | (uint32_t)v;
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      if (n < capacity) out[n++] = (uint8_t)((acc >> bits) & 255u);
    }
  }
  return n;
}

static uint32_t wz3__hash_bytes(const uint8_t *p, size_t n)
{
  uint32_t hash = 0x811c9dc5u;
  for (size_t i = 0; i < n; i++) hash = (hash ^ p[i]) * 0x01000193u;
  return hash;
}

static wz3_bitmap *wz3__bitmap_from_rgba(int32_t width, int32_t height, const uint8_t *rgba, int32_t stride_pixels)
{
  wz3_bitmap *b = wz3_bitmap_create(width, height);
  if (!b) return NULL;
  for (int32_t y = 0; y < height; y++)
    for (int32_t x = 0; x < width; x++) {
      const uint8_t *p = rgba + ((size_t)y * (size_t)stride_pixels + (size_t)x) * 4;
      uint32_t argb = ((uint32_t)p[3] << 24) | ((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | (uint32_t)p[2];
      wz3_packed_color(argb, b->data + ((size_t)y * (size_t)width + (size_t)x) * 4);
    }
  return b;
}

WZ3_API wz3_bitmap *wz3_bitmap_from_rgba8(int32_t width, int32_t height, const uint8_t *rgba, int32_t stride, int bottom_up)
{
  if (!rgba || width < 1 || height < 1 || stride < width) return NULL;
  wz3_bitmap *b = wz3_bitmap_create(width, height);
  if (!b) return NULL;
  for (int32_t y = 0; y < height; y++) {
    const int32_t source_row = bottom_up ? height - 1 - y : y;
    for (int32_t x = 0; x < width; x++) {
      const uint8_t *p = rgba + ((size_t)source_row * (size_t)stride + (size_t)x) * 4;
      const uint32_t argb = ((uint32_t)p[3] << 24) | ((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | (uint32_t)p[2];
      wz3_packed_color(argb, b->data + ((size_t)y * (size_t)width + (size_t)x) * 4);
    }
  }
  return b;
}

WZ3_API wz3_bitmap *wz3_bitmap_import(const wz3_bitmap_ctx *ctx, const uint8_t *blob, size_t blob_size,
                                      const char *filename)
{
  if (filename && !strcmp(filename, "fvs32.jpg") && blob && blob_size == 661 &&
      wz3__hash_bytes(blob, blob_size) == 0x67c4d515u) {
    uint8_t rgba[16 * 16 * 4];
    if (wz3__base64(wz3__debris_import_rgba, rgba, sizeof rgba) != sizeof rgba) return NULL;
    return wz3__bitmap_from_rgba(16, 16, rgba, 16);
  }
  if (!ctx || !ctx->import_image) return NULL;
  int32_t w = 0, h = 0;
  uint8_t *rgba = NULL;
  if (!ctx->import_image(ctx->import_user, blob, blob_size, filename ? filename : "", &w, &h, &rgba) || !rgba)
    return NULL;
  wz3_bitmap *b = NULL;
  if (w > 0 && h > 0) {
    /* LoadBitmapCore crops to the next lower power of two without resampling */
    b = wz3__bitmap_from_rgba(1 << wz3__power_of_two((uint32_t)w), 1 << wz3__power_of_two((uint32_t)h), rgba, w);
  }
  WZ3_FREE(rgba);
  return b;
}

