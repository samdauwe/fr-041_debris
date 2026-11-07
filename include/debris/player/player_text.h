/* player_text.h: text for the player's own screens (credits, statistics), SDL-free so that it can be tested headless.
 * Include wz3.h first.
 *
 *   #define PLAYER_TEXT_IMPLEMENTATION in exactly one source file.
 *
 * ptext_render() rasterizes text with the library's built-in glyph rasterizer (wz3_text_default: exact-area coverage over the embedded
 * Arimo outlines, the printable ASCII range) into a tight premultiplied RGBA image with a one pixel drop shadow, ready for
 * glr_present_overlay(). Lines are separated by '\n'. The glyph rasterizer is deterministic, so the same text at the same size is the
 * same image everywhere.
 */
#ifndef PLAYER_TEXT_H
#define PLAYER_TEXT_H

#include <stdint.h>

#ifndef PLAYER_TEXT_API
#ifdef PLAYER_TEXT_STATIC
#define PLAYER_TEXT_API static
#else
#define PLAYER_TEXT_API extern
#endif
#endif

typedef struct ptext_image {
  uint8_t *rgba; /* premultiplied, top row first; owned: ptext_free */
  int32_t width, height;
} ptext_image;

/* `pixel_size` is the em size in pixels (6..256), `rgb` the text colour 0xRRGGBB. Returns 1, or 0 for no printable text, a text of more than
   200 lines or 2000 bytes, a size out of range, or out of memory (*out is then empty). */
PLAYER_TEXT_API int ptext_render(ptext_image *out, const char *text, int32_t pixel_size, uint32_t rgb);
PLAYER_TEXT_API void ptext_free(ptext_image *image);

#endif /* PLAYER_TEXT_H */

#ifdef PLAYER_TEXT_IMPLEMENTATION
#ifndef PLAYER_TEXT_IMPLEMENTATION_DONE
#define PLAYER_TEXT_IMPLEMENTATION_DONE

#include <stdlib.h>
#include <string.h>

#ifndef PLAYER_TEXT_MALLOC
#define PLAYER_TEXT_MALLOC(n) malloc(n)
#define PLAYER_TEXT_FREE(p)   free(p)
#endif

PLAYER_TEXT_API void ptext_free(ptext_image *image)
{
  if (!image) return;
  PLAYER_TEXT_FREE(image->rgba);
  image->rgba = NULL;
  image->width = image->height = 0;
}

PLAYER_TEXT_API int ptext_render(ptext_image *out, const char *text, int32_t pixel_size, uint32_t rgb)
{
  if (!out) return 0;
  out->rgba = NULL;
  out->width = out->height = 0;
  if (!text || pixel_size < 6 || pixel_size > 256) return 0;
  const size_t length = strlen(text);
  if (length == 0 || length > 2000) return 0;
  int32_t lines = 1;
  size_t widest = 0, current = 0;
  int printable = 0;
  for (size_t i = 0; i < length; i++) {
    if (text[i] == '\n') {
      lines++;
      current = 0;
    } else {
      current++;
      if (current > widest) widest = current;
      printable |= text[i] != ' ';
    }
  }
  if (!printable || lines > 200) return 0;
  /* the surface: an em per character is wider than any glyph, 1.5 em per line holds ascent, descent and the margin of the rasterizer */
  const int64_t sw = (int64_t)widest * pixel_size + 16, sh = (int64_t)lines * pixel_size * 3 / 2 + 16;
  if (sw > 4096 || sh > 4096) return 0;
  wz3_text_request req;
  memset(&req, 0, sizeof req);
  req.width = (int32_t)sw;
  req.height = (int32_t)sh;
  req.x = 0.0f;
  req.y = 0.0f;
  req.text_width = ((float)pixel_size + 0.25f) / (float)sh; /* the em size as a fraction of the height (truncated by the rasterizer) */
  req.text_height = 0.0f;                                   /* natural glyph widths */
  req.line_skip = 1.0f;
  req.text = text;
  req.font = "";
  wz3_text_result res;
  memset(&res, 0, sizeof res);
  if (!wz3_text_default(NULL, &req, &res) || !res.coverage) return 0;
  const uint8_t *cov = res.coverage;
  int32_t x0 = req.width, y0 = req.height, x1 = -1, y1 = -1;
  for (int32_t y = 0; y < req.height; y++)
    for (int32_t x = 0; x < req.width; x++)
      if (cov[(size_t)y * (size_t)req.width + (size_t)x]) {
        if (x < x0) x0 = x;
        if (x > x1) x1 = x;
        if (y < y0) y0 = y;
        if (y > y1) y1 = y;
      }
  if (x1 < x0) {
    WZ3_FREE(res.coverage);
    return 0;
  }
  const int32_t w = x1 - x0 + 2, h = y1 - y0 + 2; /* + the shadow's extra column and row */
  uint8_t *rgba = (uint8_t *)PLAYER_TEXT_MALLOC((size_t)w * (size_t)h * 4u);
  if (!rgba) {
    WZ3_FREE(res.coverage);
    return 0;
  }
  const uint32_t cr = (rgb >> 16) & 255u, cg = (rgb >> 8) & 255u, cb = rgb & 255u;
  for (int32_t y = 0; y < h; y++) {
    for (int32_t x = 0; x < w; x++) {
      const int32_t sx = x0 + x, sy = y0 + y;
      const uint32_t c = (sx <= x1 && sy <= y1) ? cov[(size_t)sy * (size_t)req.width + (size_t)sx] : 0u;
      const uint32_t s =
        (x > 0 && y > 0 && sx - 1 >= x0 && sy - 1 >= y0 && sx - 1 <= x1 && sy - 1 <= y1) ? cov[(size_t)(sy - 1) * (size_t)req.width + (size_t)(sx - 1)] : 0u;
      const uint32_t shadow = s * 153u / 255u; /* the shadow is black at 60 % */
      const uint32_t a = c + (shadow * (255u - c) + 127u) / 255u;
      uint8_t *px = rgba + ((size_t)y * (size_t)w + (size_t)x) * 4u;
      px[0] = (uint8_t)((cr * c + 127u) / 255u);
      px[1] = (uint8_t)((cg * c + 127u) / 255u);
      px[2] = (uint8_t)((cb * c + 127u) / 255u);
      px[3] = (uint8_t)(a > 255u ? 255u : a);
    }
  }
  WZ3_FREE(res.coverage);
  out->rgba = rgba;
  out->width = w;
  out->height = h;
  return 1;
}

#endif /* PLAYER_TEXT_IMPLEMENTATION_DONE */
#endif /* PLAYER_TEXT_IMPLEMENTATION */
