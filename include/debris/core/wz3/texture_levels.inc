/* ======================================================================== */
/*  texture preparation: UpdateTexture level conversion                     */
/* ======================================================================== */

static int32_t wz3__tex_power(int32_t v) { int32_t p = 0; while (v > 1) { v >>= 1; p++; } return p; }

static int32_t wz3__tex_level_count(const wz3_bitmap *b)
{
  const int32_t w = b->width, h = b->height;
  if ((w & (w - 1)) || (h & (h - 1))) return 1;
  const int32_t wp = wz3__tex_power(w), hp = wz3__tex_power(h);
  const int32_t shorter = wp < hp ? wp : hp;
  const int32_t requested = b->mip_count;
  if (requested == 0) return shorter + 1;
  int32_t n = requested < shorter ? requested : shorter;
  return n < 1 ? 1 : n;
}

static inline int32_t wz3__tex_q8_channel(float v)
{
  int32_t t = wz3__ftoi((double)(v * 127.0f));
  return t < -128 ? -128 : t > 127 ? 127 : t;
}

/* One level of words -> bytes (unorm: bits 7..14; snorm: Q8W8V8U8 + UnitSafe3). */
static void wz3__tex_convert(uint8_t *dst, const uint16_t *src, size_t pixels, int snorm)
{
  if (!snorm) {
    for (size_t i = 0; i < pixels * 4; i++) dst[i] = (uint8_t)((src[i] >> 7) & 0xff);
    return;
  }
  for (size_t i = 0; i < pixels * 4; i += 4) {
    float x = (float)((int32_t)src[i + 2] - 0x4000);
    float y = (float)((int32_t)src[i + 1] - 0x4000);
    float z = (float)((int32_t)src[i] - 0x4000);
    const float length_squared = (x * x + y * y) + z * z;
    if (length_squared > 1e-20f) {
      const float inverse = (float)(1.0 / sqrt((double)length_squared));
      x = x * inverse; y = y * inverse; z = z * inverse;
    } else {
      x = 1.0f; y = 0.0f; z = 0.0f;
    }
    dst[i] = (uint8_t)(wz3__tex_q8_channel(x) & 0xff);
    dst[i + 1] = (uint8_t)(wz3__tex_q8_channel(y) & 0xff);
    dst[i + 2] = (uint8_t)(wz3__tex_q8_channel(z) & 0xff);
    dst[i + 3] = (uint8_t)((((int32_t)src[i + 3] - 0x4000) >> 7) & 0xff);
  }
}

/* Source words of the next level; filter selects the box filter over the top-left word. */
static void wz3__tex_reduce(uint16_t *dst, const uint16_t *src, int32_t stride, int32_t width, int32_t height,
                            int filter)
{
  const int32_t nw = width >> 1, nh = height >> 1;
  for (int32_t y = 0; y < nh; y++) {
    const uint16_t *row0 = src + (size_t)(y * 2) * (size_t)stride * 4;
    const uint16_t *row1 = row0 + (size_t)stride * 4;
    for (int32_t x = 0; x < nw; x++) {
      const uint16_t *p0 = row0 + (size_t)x * 8;
      const uint16_t *p1 = row1 + (size_t)x * 8;
      uint16_t *d = dst + ((size_t)y * (size_t)nw + (size_t)x) * 4;
      for (int c = 0; c < 4; c++)
        d[c] = filter ? (uint16_t)(((uint32_t)p0[c] + p0[4 + c] + p1[c] + p1[4 + c] + 2u) >> 2) : p0[c];
    }
  }
}

static void wz3__tex_gather_block(uint8_t block[64], const uint16_t *src, int32_t stride, int32_t x, int32_t y,
                                  int32_t width, int32_t height)
{
  int d = 0;
  for (int by = 0; by < 4; by++) {
    const uint16_t *row = src + (size_t)((y + by) & (height - 1)) * (size_t)stride * 4;
    for (int bx = 0; bx < 4; bx++) {
      const uint16_t *p = row + (size_t)((x + bx) & (width - 1)) * 4;
      block[d++] = (uint8_t)((p[2] >> 7) & 0xff);
      block[d++] = (uint8_t)((p[1] >> 7) & 0xff);
      block[d++] = (uint8_t)((p[0] >> 7) & 0xff);
      block[d++] = (uint8_t)((p[3] >> 7) & 0xff);
    }
  }
}

WZ3_API void wz3_texture_release(wz3_texture_image *t)
{
  if (!t) return;
  WZ3_FREE(t->data);
  memset(t, 0, sizeof *t);
}

WZ3_API wz3_result wz3_texture_prepare(wz3_texture_image *out, const wz3_bitmap *b, int allow_bc3)
{
  if (!out) return WZ3_ERR_ARGUMENT;
  memset(out, 0, sizeof *out);
  if (!b || !b->data || b->width < 1 || b->height < 1 || b->width > 16384 || b->height > 16384)
    return WZ3_ERR_ARGUMENT;

  const int32_t format = b->format;
  const int32_t threshold_word = b->mip_threshold;
  if (format == 9 && (threshold_word < 0 || threshold_word >= 64)) return WZ3_ERR_UNSUPPORTED;
  const wz3_texture_kind kind = format == 9 ? (allow_bc3 ? WZ3_TEXTURE_BC3 : WZ3_TEXTURE_RGBA8)
                                            : format == 5 ? WZ3_TEXTURE_SNORM8 : WZ3_TEXTURE_RGBA8;
  const int32_t wanted = wz3__tex_level_count(b);

  /* level table */
  int32_t w = b->width, h = b->height;
  uint32_t n = 0;
  size_t total = 0;
  for (;;) {
    wz3_texture_level *l = &out->levels[n];
    l->width = w;
    l->height = h;
    l->offset = total;
    l->size = kind == WZ3_TEXTURE_BC3 ? (size_t)((w + 3) >> 2) * (size_t)((h + 3) >> 2) * 16
                                      : (size_t)w * (size_t)h * 4;
    total += l->size;
    n++;
    if (w <= 1 || h <= 1 || (int32_t)n >= wanted || n >= WZ3_TEXTURE_MAX_LEVELS) break;
    w >>= 1;
    h >>= 1;
  }
  out->kind = kind;
  out->width = b->width;
  out->height = b->height;
  out->n_levels = n;
  out->size = total;
  out->data = (uint8_t *)WZ3_MALLOC(total);
  uint16_t *ws[2] = {NULL, NULL};
  if (n > 1 && format != 8) {
    const size_t quarter = (size_t)(b->width >> 1) * (size_t)(b->height >> 1) * 4;
    ws[0] = (uint16_t *)WZ3_MALLOC(quarter * sizeof(uint16_t));
    ws[1] = n > 2 ? (uint16_t *)WZ3_MALLOC(quarter * sizeof(uint16_t)) : NULL;
    if (!ws[0] || (n > 2 && !ws[1])) {
      WZ3_FREE(ws[0]);
      WZ3_FREE(ws[1]);
      wz3_texture_release(out);
      return WZ3_ERR_OUT_OF_MEMORY;
    }
  }
  if (!out->data) {
    WZ3_FREE(ws[0]);
    WZ3_FREE(ws[1]);
    memset(out, 0, sizeof *out);
    return WZ3_ERR_OUT_OF_MEMORY;
  }

  const int reverse = (threshold_word & 16) != 0;
  const int32_t threshold = threshold_word & 15;
  const uint16_t *src = b->data;
  int32_t stride = b->width;

  for (uint32_t level = 0; level < n; level++) {
    const wz3_texture_level *l = &out->levels[level];
    uint8_t *dst = out->data + l->offset;
    if (format == 8 && level > 0) {
      /* GPU-generated chain: 8-bit box filter of the previous level */
      const wz3_texture_level *p = &out->levels[level - 1];
      const uint8_t *prev = out->data + p->offset;
      for (int32_t y = 0; y < l->height; y++)
        for (int32_t x = 0; x < l->width; x++)
          for (int c = 0; c < 4; c++) {
            const uint8_t *q = prev + ((size_t)(2 * y) * (size_t)p->width + (size_t)(2 * x)) * 4 + c;
            const uint32_t sum = (uint32_t)q[0] + q[4] + q[(size_t)p->width * 4] + q[(size_t)p->width * 4 + 4];
            dst[((size_t)y * (size_t)l->width + (size_t)x) * 4 + (size_t)c] = (uint8_t)((sum + 2u) >> 2);
          }
    } else if (format == 9) {
      uint8_t block[64], encoded[16];
      size_t index = 0;
      for (int32_t y = 0; y < l->height; y += 4) {
        for (int32_t x = 0; x < l->width; x += 4, index++) {
          wz3__tex_gather_block(block, src, stride, x, y, l->width, l->height);
          wz3_bc3_compress_block(block, encoded);
          if (kind == WZ3_TEXTURE_BC3) {
            memcpy(dst + index * 16, encoded, 16);
          } else {
            uint8_t decoded[64];
            wz3_bc3_decompress_block(encoded, decoded);
            for (int by = 0; by < 4 && y + by < l->height; by++)
              for (int bx = 0; bx < 4 && x + bx < l->width; bx++)
                memcpy(dst + ((size_t)(y + by) * (size_t)l->width + (size_t)(x + bx)) * 4,
                       decoded + (by * 4 + bx) * 4, 4);
          }
        }
      }
    } else {
      wz3__tex_convert(dst, src, (size_t)l->width * (size_t)l->height, format == 5);
    }
    if (level + 1 < n && format != 8) {
      uint16_t *next = ws[level & 1u];
      int filter = threshold <= (int32_t)level;
      if (reverse) filter = !filter;
      wz3__tex_reduce(next, src, stride, l->width, l->height, filter);
      src = next;
      stride = l->width >> 1;
    }
  }
  WZ3_FREE(ws[0]);
  WZ3_FREE(ws[1]);
  return WZ3_OK;
}

