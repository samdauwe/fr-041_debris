/* wz3.h - werkkzeug3 pure layers - single-header C99 library (stb style)

   The CPU-only, renderer-free layers of Farbrausch's werkkzeug3 engine as used
   by fr-041 debris, transcribed from the original sources (vendor/wz3 of the reference repository) and checked against
   the JavaScript port of the demo:

     core        float32/x87-faithful math, sGetRnd generator, sFPow, matrices
     kdoc        bounds-checked reader of the compact "KX" operator document
     GenBitmap   procedural 16-bit RGBA bitmaps, the legacy lookup textures, BC3 texture compression
     GenMesh     meshes, MinMeshes (skinned, animated), splines, Font3D text, shadow topology
     runtime     events, animation VM and the precalculation of all operators of a document
     frame       the per-frame jobs of the scene: materials, lights, draw lists, effects, render and texture plans

   Nothing here touches a GPU, window or audio device; the OpenGL side is glr.h.

   The implementation is split into the .inc fragments of the wz3 directory, included below in a fixed order, so that this file stays the
   readable public interface.

   USAGE
     In exactly one translation unit:

         #define WZ3_IMPLEMENTATION
         #include <debris/core/wz3.h>

         wz3_kx *kx;
         wz3_kx_parse(&kx, bytes, size, NULL);          // a debris .kx payload
         wz3_graph *g = wz3_graph_create(kx, NULL, NULL);
         const wz3_bitmap *bmp;
         wz3_graph_eval_bitmap(g, op_index, &bmp);      // bmp->data: RGBA u16
         ...
         wz3_graph_destroy(g); wz3_kx_destroy(kx);

   CONFIGURATION (before including the implementation)
     WZ3_MALLOC(sz) / WZ3_FREE(p)   allocator override; both or neither
     WZ3_STATIC                     make every API function static

   DESIGN NOTES
     - No global or static mutable state, no threads, no exceptions. Objects
       are independent; a given object must be used from one thread at a time.
       The original's globals (texture size offset, default bitmap format, the
       random seed, the Perlin tables) live in explicit objects here.
     - Failures are return codes (wz3_result) or NULL results. A bitmap
       generator returns NULL where the original returns a null bitmap
       (degenerate parameters, size mismatch) and on allocation failure.
     - Bitmaps are interleaved 16-bit R,G,B,A pixels with 15-bit range (the
       source's sU64 BGRA pixels, MMX word arithmetic spelled out per lane).
     - The player runs the x87 in single precision with round-to-nearest-even.
       The code reproduces every rounding point with binary32 arithmetic and
       so needs FLT_EVAL_METHOD == 0 and no FMA contraction (the header
       requests this for GCC/Clang/MSVC). Do not compile with -ffast-math.
       Transcendentals (sin, cos, atan, log, pow, ...) are computed by the
       header itself (fdlibm ports, bit-exact with V8), so results do not
       depend on the platform's libm; only the IEEE-exact sqrt, floor, ceil,
       fabs, ldexp, frexp and fmod are imported.
     - Parsed documents own a copy of the bytes they need.

   LICENSE: BSD, as the werkkzeug3 sources (third_party/licenses/werkkzeug3-LICENSE.txt).
*/

#ifndef WZ3_H_INCLUDED
#define WZ3_H_INCLUDED

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef WZ3_API
#ifdef WZ3_STATIC
#define WZ3_API static
#else
#define WZ3_API extern
#endif
#endif

typedef enum wz3_result {
  WZ3_OK = 0,
  WZ3_ERR_ARGUMENT,     /* NULL/out-of-range argument */
  WZ3_ERR_FORMAT,       /* malformed document */
  WZ3_ERR_TRUNCATED,    /* document ends early */
  WZ3_ERR_LIMIT,        /* document exceeds a sanity limit */
  WZ3_ERR_OUT_OF_MEMORY,
  WZ3_ERR_UNSUPPORTED,  /* operator class not handled by this layer */
  WZ3_ERR_OPERATOR,     /* operator evaluation produced no result */
  WZ3_ERR_CANCELLED     /* the host asked to stop (wz3_rt_host.cancelled) */
} wz3_result;

WZ3_API const char *wz3_result_string(wz3_result r);

/* ======================================================================== */
/*  core                                                                    */
/* ======================================================================== */

/* sGetRnd: MSVCRT's two-step generator. The default seed is 0x74382381. */
typedef struct wz3_rng { uint32_t seed; } wz3_rng;
WZ3_API void     wz3_rng_init(wz3_rng *r);
WZ3_API void     wz3_rng_seed(wz3_rng *r, int32_t seed);   /* sSetRndSeed */
WZ3_API uint32_t wz3_rng_u32(wz3_rng *r);                  /* sGetRnd() */
WZ3_API uint32_t wz3_rng_int(wz3_rng *r, uint32_t max);    /* sGetRnd(max); 0 if max == 0 */
WZ3_API float    wz3_rng_float(wz3_rng *r, float max);     /* sGetRndF(max) */

/* sFPow: the handwritten x87 pow (0 base returns the base for any exponent). */
WZ3_API double  wz3_pow(double base, double exponent);
/* sFtol: x87 fistp, round to nearest even; out-of-range/NaN -> INT32_MIN. */
WZ3_API int32_t wz3_ftol(double value);

/* Matrices are 16 floats in sMatrix order: the columns i, j, k, l (WebGL
   column-major); vectors are x,y,z[,w]. Every operation rounds like the
   released player's 24-bit x87 schedule. */
WZ3_API void wz3_mat4_identity(float out[16]);
WZ3_API void wz3_mat4_mul(const float a[16], const float b[16], float out[16]);   /* a * b */
WZ3_API void wz3_mat4_mul_a(const float a[16], const float b[16], float out[16]); /* sMatrix::MulA: b * a */
WZ3_API void wz3_mat4_mul3(const float a[16], const float b[16], float out[16]);  /* 3x3 part */
WZ3_API void wz3_mat4_euler(float x, float y, float z, float out[16]);            /* radians */
WZ3_API void wz3_mat4_euler_turns(const float turns[3], float out[16]);
WZ3_API void wz3_mat4_srt(const float srt[9], float out[16]);        /* scale, rotate(turns), translate */
WZ3_API void wz3_mat4_direction(const float dir[3], float out[16]);  /* sMatrix::InitDir */
WZ3_API void wz3_mat4_transform_point(const float m[16], const float v[4], float out[4]);
WZ3_API void wz3_sincos(float angle, float *sine, float *cosine);    /* SSE_SinCos4 polynomial */
WZ3_API void wz3_vec3_cross(const float a[3], const float b[3], float out[3]);
WZ3_API void wz3_vec3_normalize(const float v[3], float out[3]);
WZ3_API void wz3_vec3_normalize_safe(const float v[3], float out[3]);

/* ======================================================================== */
/*  GenBitmap                                                               */
/* ======================================================================== */

typedef struct wz3_bitmap {
  int32_t width, height;
  int32_t format, mip_count, mip_threshold;  /* texture settings, set by Bitmap_Format */
  int32_t stripped;
  uint16_t *data;                            /* width*height*4 words: R,G,B,A in 0..0x7fff */
} wz3_bitmap;

/* Bitmap_Text asks the host to rasterize text: 8-bit coverage, width*height. */
typedef struct wz3_text_request {
  int32_t width, height;
  float x, y, text_width, text_height;
  uint32_t color, flags;
  float external_space, internal_space, line_skip;
  const char *text, *font;
  int32_t page;
} wz3_text_request;
typedef struct wz3_text_result {
  uint8_t *coverage;       /* host allocated with WZ3_MALLOC; ownership passes to the library */
  int32_t line_height;
} wz3_text_result;
typedef int (*wz3_text_fn)(void *user, const wz3_text_request *req, wz3_text_result *out);
/* Built-in rasterizer (exact-area coverage over embedded Arimo outlines, no host font needed);
   wz3_bitmap_ctx_init installs it. Returns 1 on success. */
WZ3_API int wz3_text_default(void *user, const wz3_text_request *req, wz3_text_result *out);

/* Bitmap_Import: decode an image file to RGBA bytes (width*height*4); the
   library crops to power-of-two sizes. Ownership passes to the library. */
typedef int (*wz3_import_fn)(void *user, const uint8_t *blob, size_t blob_size, const char *filename,
                             int32_t *width, int32_t *height, uint8_t **rgba);

/* Per-document generation settings (the original keeps these as globals). */
typedef struct wz3_bitmap_ctx {
  int32_t texture_offset;     /* added to size exponents unless bit 7 of xs is set */
  int32_t default_format;     /* format of new bitmaps (the player uses 1) */
  wz3_text_fn text;           void *text_user;
  wz3_import_fn import_image; void *import_user;
} wz3_bitmap_ctx;
WZ3_API void wz3_bitmap_ctx_init(wz3_bitmap_ctx *ctx);

WZ3_API wz3_bitmap *wz3_bitmap_create(int32_t width, int32_t height);
WZ3_API wz3_bitmap *wz3_bitmap_copy(const wz3_bitmap *b);
WZ3_API void        wz3_bitmap_destroy(wz3_bitmap *b);
WZ3_API uint32_t    wz3_bitmap_hash(const wz3_bitmap *b);   /* FNV-1a over the little-endian words */

/* Pack 0xAARRGGBB into the four 15-bit words. */
WZ3_API void wz3_packed_color(uint32_t argb, uint16_t out[4]);

/* Generators. xs/ys are size exponents (2^xs by 2^ys, plus ctx->texture_offset
   unless bit 7 of xs is set); ctx may be NULL for the defaults. All other
   parameters follow the operator's data words. */
WZ3_API wz3_bitmap *wz3_bitmap_flat(const wz3_bitmap_ctx *ctx, uint32_t xs, uint32_t ys, uint32_t color);
WZ3_API wz3_bitmap *wz3_bitmap_perlin(const wz3_bitmap_ctx *ctx, uint32_t xs, uint32_t ys, int32_t frequency,
                                      int32_t octaves, float fadeoff, int32_t seed, int32_t mode,
                                      float amplitude, float gamma, uint32_t color0, uint32_t color1);
WZ3_API wz3_bitmap *wz3_bitmap_cell(const wz3_bitmap_ctx *ctx, uint32_t xs, uint32_t ys, uint32_t color0,
                                    uint32_t color1, uint32_t color2, int32_t count, int32_t seed,
                                    float amplitude, float gamma, int32_t mode, float min_distance,
                                    int32_t percent, float aspect);
WZ3_API wz3_bitmap *wz3_bitmap_gradient(const wz3_bitmap_ctx *ctx, uint32_t xs, uint32_t ys, uint32_t color0,
                                        uint32_t color1, float position, float angle, float length,
                                        int32_t mode);
WZ3_API wz3_bitmap *wz3_bitmap_bricks(const wz3_bitmap_ctx *ctx, uint32_t xs, uint32_t ys, uint32_t color0,
                                      uint32_t color1, uint32_t mortar_color, float mortar_x, float mortar_y,
                                      int32_t tiles_x, int32_t tiles_y, int32_t seed, int32_t heads,
                                      int32_t flags, float side, float color_balance);

/* Operators on existing bitmaps never modify their inputs. */
WZ3_API wz3_bitmap *wz3_bitmap_format(const wz3_bitmap *in, int32_t format, int32_t mip_count, int32_t mip_threshold);
WZ3_API wz3_bitmap *wz3_bitmap_color(const wz3_bitmap *in, int32_t mode, uint32_t color);
/* Merges inputs[1..] into a copy of inputs[0]; the first NULL ends the list. */
WZ3_API wz3_bitmap *wz3_bitmap_merge(int32_t mode, const wz3_bitmap *const *inputs, int32_t count);
WZ3_API wz3_bitmap *wz3_bitmap_range(const wz3_bitmap *in, int32_t mode, uint32_t color0, uint32_t color1);
WZ3_API wz3_bitmap *wz3_bitmap_glowrect(const wz3_bitmap *in, float cx, float cy, float rx, float ry,
                                        float sx, float sy, uint32_t color, float alpha, float power,
                                        int32_t wrap, int32_t bug);
WZ3_API wz3_bitmap *wz3_bitmap_hscb(const wz3_bitmap *in, float hue, float saturation, float contrast,
                                    float brightness);
WZ3_API wz3_bitmap *wz3_bitmap_blur(const wz3_bitmap *in, int32_t flags, float sx, float sy, float amplitude);
WZ3_API wz3_bitmap *wz3_bitmap_sharpen(const wz3_bitmap *in, int32_t order, float sx, float sy, float amplitude);
WZ3_API wz3_bitmap *wz3_bitmap_mask(const wz3_bitmap *mask, const wz3_bitmap *b, const wz3_bitmap *c, int32_t mode);
WZ3_API wz3_bitmap *wz3_bitmap_rotate(const wz3_bitmap_ctx *ctx, const wz3_bitmap *in, float angle, float sx,
                                      float sy, float tx, float ty, int32_t border, int32_t new_width,
                                      int32_t new_height);
WZ3_API wz3_bitmap *wz3_bitmap_rotate_mul(const wz3_bitmap_ctx *ctx, const wz3_bitmap *in, float angle,
                                          float sx, float sy, float tx, float ty, int32_t border,
                                          uint32_t color, int32_t mode, int32_t count, uint32_t fade);
WZ3_API wz3_bitmap *wz3_bitmap_distort(const wz3_bitmap *sample, const wz3_bitmap *displacement,
                                       float distance, int32_t border);
WZ3_API wz3_bitmap *wz3_bitmap_normals(const wz3_bitmap *in, float distance, int32_t mode);
WZ3_API wz3_bitmap *wz3_bitmap_bump(const wz3_bitmap *in, const wz3_bitmap *normals, int32_t subcode,
                                    float px, float py, float pz, float da, float db, uint32_t diffuse,
                                    uint32_t ambient, float outer, float falloff, float amp,
                                    uint32_t specular, float spec_power, float spec_amp);
WZ3_API wz3_bitmap *wz3_bitmap_color_balance(const wz3_bitmap *in, const float values[9]);
WZ3_API wz3_bitmap *wz3_bitmap_bulge(const wz3_bitmap *in, float warp);

/* Per-letter metrics Bitmap_Text publishes for font pages (uv are 0..1). */
typedef struct wz3_letter {
  double u0, v0, u1, v1;
  double pre_space, width;
} wz3_letter;

/* `blob` is the operator's cached glyph coverage (may be NULL). With flag bit
   0x80 and a matching blob no host rasterizer is needed. `letters`, if not
   NULL, receives 256 entries when the operator selects a font page. */
WZ3_API wz3_bitmap *wz3_bitmap_text(const wz3_bitmap_ctx *ctx, const uint8_t *blob, size_t blob_size,
                                    const wz3_bitmap *in, float x, float y, float width, float height,
                                    uint32_t color, int32_t flags, float external_space,
                                    float internal_space, float line_skip, const char *text,
                                    const char *font, wz3_letter letters[256]);
WZ3_API wz3_bitmap *wz3_bitmap_import(const wz3_bitmap_ctx *ctx, const uint8_t *blob, size_t blob_size,
                                      const char *filename);

/* ======================================================================== */
/*  generated lookup textures (_start.cpp)                                  */
/* ======================================================================== */

#define WZ3_CUBE_NORMALIZER_BYTES (6 * 64 * 64 * 4) /* six 64x64 faces, RGBA bytes */
#define WZ3_ATTENUATION_VOLUME_BYTES (32 * 32 * 32 * 4)
WZ3_API void wz3_make_cube_normalizer(uint8_t out[WZ3_CUBE_NORMALIZER_BYTES]);
WZ3_API void wz3_make_attenuation_volume(uint8_t out[WZ3_ATTENUATION_VOLUME_BYTES]);

/* ======================================================================== */
/*  texture preparation (UpdateTexture + rygdxt)                            */
/* ======================================================================== */

/* BC3 (DXT5) block codec, bit-exact with rygdxt quality 0. Input pixels are
   16 BGRA bytes (the native Pixel layout); output is the 16 byte block. */
WZ3_API void wz3_bc3_compress_block(const uint8_t bgra[64], uint8_t out[16]);
/* Decodes to a 4x4 block of RGBA bytes with rygdxt's own palette rounding. */
WZ3_API void wz3_bc3_decompress_block(const uint8_t in[16], uint8_t rgba[64]);

typedef enum wz3_texture_kind {
  WZ3_TEXTURE_RGBA8 = 0,   /* RGBA bytes */
  WZ3_TEXTURE_SNORM8 = 1,  /* Q8W8V8U8: signed bytes, stored two's complement */
  WZ3_TEXTURE_BC3 = 2      /* 16 byte blocks, row major */
} wz3_texture_kind;

#define WZ3_TEXTURE_MAX_LEVELS 16
typedef struct wz3_texture_level {
  int32_t width, height;
  size_t offset, size;     /* into wz3_texture_image.data */
} wz3_texture_level;

typedef struct wz3_texture_image {
  wz3_texture_kind kind;
  int32_t width, height;
  uint32_t n_levels;
  wz3_texture_level levels[WZ3_TEXTURE_MAX_LEVELS];
  size_t size;
  uint8_t *data;           /* all levels back to back; release with wz3_texture_release */
} wz3_texture_image;

/* Builds the bytes the original UpdateTexture hands to the GPU: format 1 and
   other plain formats take bits 7..14 of each word, format 5 becomes signed
   normals, format 9 is BC3 (or, with allow_bc3 == 0, BC3 round-tripped to
   RGBA8). Mip levels follow Bitmap_Format's count/threshold on the 15-bit
   words; format 8 uses an 8-bit box filter (the GPU generated chain).
   Returns WZ3_ERR_UNSUPPORTED for DXT quality bits other than production's 0. */
WZ3_API wz3_result wz3_texture_prepare(wz3_texture_image *out, const wz3_bitmap *bitmap, int allow_bc3);
WZ3_API void wz3_texture_release(wz3_texture_image *t);

/* ======================================================================== */
/*  texture upload plan and sampler state (the texture cache of engine.cpp):     */
/*  what a renderer does with a bitmap, as data - no graphics API involved   */
/* ======================================================================== */

/* The OpenGL registry values the plans name (identical in WebGL 2, GLES 3 and desktop GL). */
enum {
  WZ3_GL_TEXTURE_2D = 0x0DE1, WZ3_GL_UNPACK_ALIGNMENT = 0x0CF5, WZ3_GL_BYTE = 0x1400, WZ3_GL_UNSIGNED_BYTE = 0x1401,
  WZ3_GL_RGBA = 0x1908, WZ3_GL_NEAREST = 0x2600, WZ3_GL_LINEAR = 0x2601, WZ3_GL_NEAREST_MIPMAP_NEAREST = 0x2700,
  WZ3_GL_LINEAR_MIPMAP_NEAREST = 0x2701, WZ3_GL_LINEAR_MIPMAP_LINEAR = 0x2703, WZ3_GL_TEXTURE_MAG_FILTER = 0x2800,
  WZ3_GL_TEXTURE_MIN_FILTER = 0x2801, WZ3_GL_TEXTURE_WRAP_S = 0x2802, WZ3_GL_TEXTURE_WRAP_T = 0x2803, WZ3_GL_REPEAT = 0x2901,
  WZ3_GL_RGBA8 = 0x8058, WZ3_GL_CLAMP_TO_EDGE = 0x812F, WZ3_GL_TEXTURE_MAX_LEVEL = 0x813D,
  WZ3_GL_COMPRESSED_RGBA_S3TC_DXT5 = 0x83F3, WZ3_GL_TEXTURE_MAX_ANISOTROPY = 0x84FE, WZ3_GL_RGBA8_SNORM = 0x8F97
};

/* TextureCache's dxt5Mode: 'auto' uses S3TC when the device has it, 's3tc' insists on it, 'rgba8' never compresses. */
typedef enum wz3_dxt5_mode { WZ3_DXT5_AUTO = 0, WZ3_DXT5_S3TC, WZ3_DXT5_RGBA8 } wz3_dxt5_mode;
/* How a bitmap is stored on the GPU (the cache's storageKind). */
typedef enum wz3_texture_storage { WZ3_STORAGE_RGBA8 = 0, WZ3_STORAGE_DXT5, WZ3_STORAGE_DXT5_RGBA8 } wz3_texture_storage;

typedef struct wz3_texture_policy { int32_t s3tc; } wz3_texture_policy;   /* s3tc: format 9 is uploaded compressed */
/* The constructor's rules: an unknown mode is WZ3_ERR_ARGUMENT, 's3tc' without hardware support WZ3_ERR_UNSUPPORTED. */
WZ3_API wz3_result wz3_texture_policy_make(wz3_texture_policy *out, int mode, int hardware_s3tc);

/* A bitmap as the cache sees it. has_data: the pixel array exists and holds exactly width*height*4 words (a bitmap whose
   graph ownership was released keeps its size but has none). data_id identifies the pixel array: another id re-uploads. */
typedef struct wz3_texture_desc {
  int32_t width, height, format, mip_count, mip_threshold;
  int32_t has_data;
  uint64_t data_id;
} wz3_texture_desc;
WZ3_API void wz3_texture_desc_from_bitmap(wz3_texture_desc *out, const wz3_bitmap *bitmap);   /* NULL describes no data */

typedef enum wz3_texture_command_kind {
  WZ3_TEXCMD_PIXEL_STORE = 0,   /* pixelStorei(pname, value) */
  WZ3_TEXCMD_IMAGE,             /* texImage2D(TEXTURE_2D, level, internal_format, width, height, 0, format, type, bytes of level `level`) */
  WZ3_TEXCMD_COMPRESSED,        /* compressedTexImage2D(TEXTURE_2D, level, internal_format, width, height, 0, bytes of level `level`) */
  WZ3_TEXCMD_GENERATE_MIPMAP,   /* generateMipmap(TEXTURE_2D) */
  WZ3_TEXCMD_PARAMETER          /* texParameteri(TEXTURE_2D, pname, value) */
} wz3_texture_command_kind;

/* One call, in execution order. The bytes of level `level` are wz3_texture_image.levels[level] of wz3_texture_prepare(bitmap,
   plan.allow_bc3), whose `size` equals `size` here. */
typedef struct wz3_texture_command {
  uint8_t kind;                 /* wz3_texture_command_kind */
  int32_t level, width, height;
  uint32_t internal_format, format, type;
  uint32_t pname;
  int32_t value;
  size_t size;
} wz3_texture_command;

/* What the cache remembers about the texture it made for a bitmap; a bitmap that still matches is not uploaded again. */
typedef struct wz3_texture_entry {
  int32_t present;
  uint64_t data_id;
  int32_t width, height, mipmapped, mip_levels, mip_threshold, format, storage;
  uint64_t byte_length;
} wz3_texture_entry;

#define WZ3_TEXTURE_MAX_COMMANDS (WZ3_TEXTURE_MAX_LEVELS + 8)
typedef struct wz3_texture_plan {
  wz3_texture_entry entry;      /* what the cache records once the plan ran */
  int32_t allow_bc3;            /* the argument of wz3_texture_prepare */
  uint32_t n_commands;
  wz3_texture_command commands[WZ3_TEXTURE_MAX_COMMANDS];
} wz3_texture_plan;

/* The accounting of resourceStats(). */
typedef struct wz3_texture_stats {
  uint32_t entries, dxt5_compressed, dxt5_fallback;
  int32_t fallback;             /* the shared 1x1 texture exists */
  uint64_t estimated_bytes;
} wz3_texture_stats;
typedef struct wz3_texture_report { uint32_t textures, bitmap_textures, dxt5_compressed, dxt5_fallback; uint64_t estimated_bytes; } wz3_texture_report;
WZ3_API void wz3_texture_stats_report(const wz3_texture_stats *stats, wz3_texture_report *out);

/* nativeMipLevelCount: 1 for a size that is not a power of two, the whole chain for a count of 0, else the count clamped to
   the shorter side; 0 for an empty bitmap. */
WZ3_API int32_t wz3_texture_mip_levels(int32_t width, int32_t height, int32_t mip_count);

/* The calls that make a texture of `desc`: no data is WZ3_ERR_ARGUMENT (bind the fallback instead); DXT quality bits other than
   production's 0 (format 9, mip_threshold >> 6 != 0) and sizes above 16384 are WZ3_ERR_UNSUPPORTED. Nothing is touched on error. */
WZ3_API wz3_result wz3_texture_plan_make(wz3_texture_plan *out, const wz3_texture_policy *policy, const wz3_texture_desc *desc);

typedef enum wz3_texture_action {
  WZ3_TEXTURE_HIT = 0,          /* the entry still matches: bind its texture */
  WZ3_TEXTURE_FALLBACK,         /* no usable data: bind the fallback texture (the entry, if any, is retired) */
  WZ3_TEXTURE_UPLOAD            /* run the plan on a fresh texture, then replace the entry's old texture */
} wz3_texture_action;
/* TextureCache.get for one bitmap: updates `entry` and `stats` exactly like the cache and tells what to do. *delete_old is set
   when the texture the entry used to name must be deleted (after the upload succeeded, for WZ3_TEXTURE_UPLOAD). On an error the
   entry, the stats and the plan stay as they were. */
WZ3_API wz3_result wz3_texture_get(wz3_texture_action *action, int32_t *delete_old, wz3_texture_plan *plan, wz3_texture_entry *entry,
                                   wz3_texture_stats *stats, const wz3_texture_policy *policy, const wz3_texture_desc *desc);
/* estimatedUploadBytes: 0 when the entry still matches, 4 for a bitmap without data while the fallback does not exist yet. */
WZ3_API uint64_t wz3_texture_estimate_bytes(const wz3_texture_policy *policy, const wz3_texture_desc *desc, const wz3_texture_entry *entry,
                                            const wz3_texture_stats *stats);
/* fallbackTexture: the first call fills `plan` (one white RGBA8 texel, linear filtering; entry unused) and returns 1; later calls return 0. */
WZ3_API int wz3_texture_fallback(wz3_texture_stats *stats, wz3_texture_plan *plan);
WZ3_API void wz3_texture_fallback_texel(uint8_t out[4]);

/* Sampling parameters are texture state: bind() sets only what differs from what the texture already has. */
enum { WZ3_SAMPLER_MAX_LEVEL = 0, WZ3_SAMPLER_MIN_FILTER, WZ3_SAMPLER_MAG_FILTER, WZ3_SAMPLER_WRAP_S, WZ3_SAMPLER_WRAP_T,
       WZ3_SAMPLER_ANISOTROPY, WZ3_SAMPLER_PARAMS };
typedef struct wz3_sampler_state { uint8_t known[WZ3_SAMPLER_PARAMS]; double value[WZ3_SAMPLER_PARAMS]; } wz3_sampler_state;
/* One texParameteri (texParameterf for the anisotropy). */
typedef struct wz3_sampler_call { uint32_t pname; double value; int32_t is_float; } wz3_sampler_call;
enum { WZ3_SAMPLER_MAX_CALLS = 8 };
typedef struct wz3_sampler_caps { int32_t anisotropy; double max_anisotropy; } wz3_sampler_caps;
/* available: EXT_texture_filter_anisotropic exists; reported_max is its MAX_TEXTURE_MAX_ANISOTROPY_EXT (NaN or 0 read as 1). */
WZ3_API void wz3_sampler_caps_make(wz3_sampler_caps *out, int available, double reported_max);
/* The state of a texture right after its plan ran (or, for the fallback, after its two calls). */
WZ3_API void wz3_sampler_state_after_plan(wz3_sampler_state *out, const wz3_texture_plan *plan);
WZ3_API void wz3_sampler_state_fallback(wz3_sampler_state *out);

/* bind(bitmap, unit, options): the options an entry binds with. */
typedef struct wz3_sampler_request {
  int32_t has_clamp, clamp;
  int32_t has_filter_mode, filter_mode;   /* the low three bits select the D3D filter */
  int32_t material11;                     /* filterProfile 'material11' (otherwise 'material20') */
  int32_t has_nearest, nearest;           /* used only without a filter mode */
} wz3_sampler_request;
/* Updates `state` and returns the number of calls written to `out` (at most WZ3_SAMPLER_MAX_CALLS), in the order TextureCache.bind
   makes them. mipmapped: the bound texture has more than one level (always 0 for the fallback). */
WZ3_API uint32_t wz3_sampler_bind(wz3_sampler_state *state, const wz3_sampler_caps *caps, int mipmapped, const wz3_sampler_request *request,
                                  wz3_sampler_call out[WZ3_SAMPLER_MAX_CALLS]);

/* Texture size offset of a quality setting: the value added to every bitmap size exponent (wz3_bitmap_ctx.texture_offset). A
   number (JavaScript's Number() grammar of the text, truncated like `| 0`) is clamped to -3..0; 'high', 'ultra' and 'full'
   are 0, 'normal', 'reduced' and 'medium' -1, 'low' -2, in any letter case; everything else (and NULL) is 0. *recognised, if
   not NULL, is 1 for a number or a name. Decimal numbers are read with strtod, so the C locale is assumed. */
WZ3_API int32_t wz3_texture_quality_offset(const char *quality, int *recognised);

/* ======================================================================== */
/*  kdoc: the compact KX operator document                                  */
/* ======================================================================== */

#define WZ3_OPC_BLOB          0x00080000u
#define WZ3_OPC_KENV          0x00800000u
#define WZ3_OPC_VARIABLEINPUT 0x01000000u
#define WZ3_OPC_SKIPEXEC      0x02000000u
#define WZ3_OPC_ALTEXEC       0x04000000u
#define WZ3_OPC_STRIPPEDIN    0x08000000u
#define WZ3_OPC_DONTCALLLINK  0x10000000u
#define WZ3_OPC_ALTINIT       0x20000000u
#define WZ3_OPC_KOP           0x40000000u
#define WZ3_OPC_FLEXINPUT     0x80000000u

enum { /* operator output classes */
  WZ3_KC_BITMAP = 1, WZ3_KC_MINMESH = 2, WZ3_KC_SCENE = 3, WZ3_KC_MATERIAL = 5, WZ3_KC_MESH = 6,
  WZ3_KC_IPP = 8, WZ3_KC_EFFECT = 9, WZ3_KC_DEMO = 11, WZ3_KC_SPLINE = 13, WZ3_KC_ANY = 255
};

#define WZ3_MAX_OP_ROOT 16

/* One serialized data word. The class's packing character selects the member:
   g/f/e -> f (float), i/s -> i, b/c/m -> u. */
typedef union wz3_word { float f; int32_t i; uint32_t u; } wz3_word;

typedef struct wz3_kx_class {
  uint32_t convention;
  uint16_t id;
  const char *packing;       /* one character per serialized field */
  uint32_t packing_length;
} wz3_kx_class;

typedef struct wz3_kx_op {
  uint16_t class_index;      /* index into kx->classes */
  uint16_t class_id;
  uint32_t n_inputs;   const uint32_t *inputs;   /* absolute operator indices */
  uint32_t n_links;    const int32_t *links;     /* operator index, -1 = none */
  const wz3_word *params;                        /* classes[class_index].packing_length words */
  uint32_t n_strings;  const char *const *strings;
  uint32_t n_splines;  const int32_t *splines;   /* document spline index, -1 = none */
  uint32_t blob_size;  const uint8_t *blob;
  uint32_t anim_size;  const uint8_t *anim;      /* animation bytecode */
} wz3_kx_op;

typedef struct wz3_kx_event {
  uint32_t operation;
  int32_t start, end;
  float velocity, modulation;
  uint8_t select;
  float scale[3], rotate[3], translate[3];
  uint32_t color;
  int32_t spline;            /* -1 = none */
  float start_interval, end_interval;
  uint8_t flags;
} wz3_kx_event;

typedef struct wz3_kx_channel {
  uint32_t n_keys;
  const uint8_t *key_time;   /* time = key_time[i] / 255 */
  const float *key_value;
} wz3_kx_channel;

typedef struct wz3_kx_spline {
  uint32_t interpolation;
  uint32_t n_channels;
  wz3_kx_channel channels[4];
} wz3_kx_spline;

/* Byte offsets of the document sections, relative to the start of the KX. */
typedef struct wz3_kx_sections {
  uint32_t header, song, samples, document, graph, parameters, animations, events, splines, blobs, end;
} wz3_kx_sections;

typedef struct wz3_kx {
  uint32_t flags;            /* 1: has name, 2: has samples, 4: buzz timing */
  char name[33];
  const uint8_t *song;       uint32_t song_size;     /* the embedded V2M */
  const uint8_t *samples;    uint32_t sample_size;
  uint32_t song_bpm_fixed;   /* 16.16 */
  uint32_t song_length;
  uint32_t roots[WZ3_MAX_OP_ROOT];
  uint32_t n_classes;    wz3_kx_class *classes;
  uint32_t n_ops;        wz3_kx_op *ops;
  uint32_t n_events;     wz3_kx_event *events;
  uint32_t n_splines;    wz3_kx_spline *splines;
  wz3_kx_sections sections;
  void *internal_[6];        /* private allocations */
} wz3_kx;

/* Parses the KX that starts at `data` (the debris .kx payload; trailing bytes
   are ignored). Everything is bounds checked; on success *out owns a private
   copy of the bytes it references. On failure *error_offset (if not NULL)
   receives the byte position where parsing stopped. */
WZ3_API wz3_result wz3_kx_parse(wz3_kx **out, const void *data, size_t size, size_t *error_offset);
WZ3_API void       wz3_kx_destroy(wz3_kx *kx);

/* Static class metadata of the released player (name, output class). */
typedef struct wz3_class_info {
  uint16_t id;
  const char *name;
  uint8_t output_class;
} wz3_class_info;
WZ3_API const wz3_class_info *wz3_class_lookup(uint16_t id);   /* NULL if unknown */

/* ======================================================================== */
/*  graph: bitmap operator evaluation                                       */
/* ======================================================================== */

typedef struct wz3_graph wz3_graph;

/* Only the bitmap classes (0x21..0x3f) are evaluable. Bitmap_Render (0x3f) renders the scene of its input into a bitmap, which
   needs the renderer: the host supplies it through `render`. Without a callback the operator reports WZ3_ERR_UNSUPPORTED (and so
   does everything that consumes it), unless `render_placeholder` is set, which is the JavaScript port's behaviour without a
   renderer hook: a blank bitmap of the requested size. A callback that returns WZ3_ERR_UNSUPPORTED falls back to that too.
   With release_consumed set, an intermediate result is freed as soon as its last bitmap consumer ran (results you asked for
   stay until wz3_graph_release). */
typedef struct wz3_bitmap_render_request {
  uint32_t op;                /* the Render operator */
  int32_t has_input;          /* the operator has an input (production: always) */
  uint32_t input;             /* its first input: the operator whose output the host renders */
  uint32_t xs, ys;            /* data words 0 and 1: the size exponents as authored */
  int32_t width, height;      /* the bitmap size: the exponents plus the texture offset (unless bit 7 of xs is set), clamped to 0..12 */
} wz3_bitmap_render_request;
/* *out is a bitmap the library takes over (wz3_bitmap_create, wz3_bitmap_from_rgba8); NULL or an error fails the operator. */
typedef wz3_result (*wz3_bitmap_render_fn)(void *user, const wz3_bitmap_render_request *request, wz3_bitmap **out);
typedef struct wz3_graph_options {
  wz3_bitmap_ctx ctx;
  int release_consumed;
  wz3_bitmap_render_fn render;
  void *render_user;
  int render_placeholder;
} wz3_graph_options;
/* bitmapFromRGBA: converts width*height RGBA bytes (`stride` pixels per row; bottom_up: the first row is the bottom one, like glReadPixels)
   to a bitmap of 15-bit words. NULL on invalid arguments or out of memory. */
WZ3_API wz3_bitmap *wz3_bitmap_from_rgba8(int32_t width, int32_t height, const uint8_t *rgba, int32_t stride, int bottom_up);

WZ3_API wz3_graph *wz3_graph_create(const wz3_kx *kx, const wz3_graph_options *options, wz3_result *result);
WZ3_API void       wz3_graph_destroy(wz3_graph *g);
/* Evaluates (once; cached) operator `op`; *out stays valid until released. */
WZ3_API wz3_result wz3_graph_eval_bitmap(wz3_graph *g, uint32_t op, const wz3_bitmap **out);
WZ3_API void       wz3_graph_release(wz3_graph *g, uint32_t op);
/* 1 when `op` is a Bitmap_Render operator or consumes one through bitmap-class inputs: its pixels need the renderer (or the
   placeholder), so a host schedules it after the geometry exists. 0 for everything else and for an invalid index. */
WZ3_API int        wz3_graph_render_dependent(const wz3_graph *g, uint32_t op);

/* ======================================================================== */
/*  runtime: environment, animation VM, events, splines (kdoc.cpp)        */
/* ======================================================================== */

typedef struct wz3_rt wz3_rt;
typedef struct wz3_mesh wz3_mesh;
typedef struct wz3_minmesh wz3_minmesh;

/* What the host provides to, and receives from, the runtime. Every member is optional and every callback runs on the
   thread that called wz3_rt_precalc (or wz3_rt_frame, which precalculates on first use); the runtime never starts
   threads. Meshes are owned by the runtime: a pointer handed to a sink is valid for the duration of the call only
   (with release_consumed the mesh is freed once its last consumer ran), so a sink copies or converts what it needs -
   typically wz3_mesh_prepare() into host-owned buffers. A sink that returns an error stops the precalculation with that
   error; the runtime frees what it had just produced, leaks nothing, and the operator stays uncalculated. */
typedef struct wz3_rt_host {
  void *user;                                                  /* first argument of every callback below */
  wz3_result (*mesh)(void *user, uint32_t op, const wz3_mesh *mesh);
  /* mesh is NULL when the operator produced none (a missing optional input) */
  wz3_result (*minmesh)(void *user, uint32_t op, const wz3_minmesh *mesh);
  /* The pixels of the bitmap operator feeding Explode (its second input), or NULL when there are none -
     the JS port then derives the shard delays from the distance to the centre instead. */
  const wz3_bitmap *(*bitmap)(void *user, uint32_t op);
  /* After operator `op` was calculated: done of total operators reachable from the root (done starts at the count a
     cancelled earlier run completed). Called once per operator, so the time between two calls is the cost of the operator
     named in the second (its inputs reported before it). */
  void (*progress)(void *user, uint32_t op, uint32_t done, uint32_t total);
  /* Polled before each operator; non-zero stops with WZ3_ERR_CANCELLED. The runtime stays consistent: calling
     wz3_rt_precalc again continues where it stopped and ends in the same state as an uninterrupted run. The latency is
     the cost of one operator. */
  int (*cancelled)(void *user);
} wz3_rt_host;

typedef enum wz3_geometry_policy {
  WZ3_GEOMETRY_GENERATE = 0,  /* Mesh and MinMesh operators run; sinks and consumers see the geometry */
  WZ3_GEOMETRY_PLACEHOLDER    /* operators have an output but no geometry (scene traversal, fuzzing); the sinks are not called */
} wz3_geometry_policy;

typedef struct wz3_rt_options {
  /* Classes without a runtime handler execute their inputs like Exec_Misc_Nop. The frame-job handlers
     replace this for the classes a scene draws; the default keeps the whole graph traversable. */
  int generic_exec;
  /* Free a mesh as soon as its last consumer has been initialised. Required for the full Debris
     graph (retaining every output would need ~3 GB); the runtime is then precalculated once. */
  int release_consumed;
  /* Run materials, scene traversal, IPP and effects as real operators (wz3_rt_frame_jobs()); without it
     those classes keep the generic traversal. */
  int frame_jobs;
  wz3_geometry_policy geometry;
  wz3_rt_host host;
} wz3_rt_options;

/* Mesh summary used by the tests: counts and one hash over topology, attributes and bounds. */
WZ3_API int wz3_mesh_summary(const wz3_mesh *m, uint32_t *vertices, uint32_t *edges, uint32_t *faces, uint32_t *hash);

/* MinMesh summary used by the tests: counts and one hash over every attribute, polygon, cluster and bone record. */
WZ3_API int wz3_minmesh_summary(const wz3_minmesh *m, uint32_t *vertices, uint32_t *faces, uint32_t *clusters, uint32_t *bones, uint32_t *hash);

/* Posed geometry. The bone matrices at `time` (16 floats per bone) need the spline caches of the runtime, so a spline
   that is no longer alive (or not a spline) leaves its bone at the rest pose; the rt must be the one the mesh came from. */
WZ3_API int      wz3_rt_minmesh_pose(const wz3_rt *rt, const wz3_minmesh *m, double time, float *matrices);
/* positions and normals: 3 floats per vertex, tangents: 4 (w = 1); normals/tangents must have been calculated. */
WZ3_API int      wz3_minmesh_skin(const wz3_minmesh *m, const float *matrices, float *positions, float *normals, float *tangents);
/* Hash over the skinned positions, normals and tangents at `time` (normals calculated on a copy first); 0 if unanimated. */
WZ3_API uint32_t wz3_rt_minmesh_frame_hash(const wz3_rt *rt, const wz3_minmesh *m, double time);

/* Hash of the vertex normals/tangents and polygon normals calcNormals derives on a copy (0 when out of memory). */
WZ3_API uint32_t wz3_minmesh_normals_hash(const wz3_minmesh *m);

/* options may be NULL (generic_exec = 1) */
WZ3_API wz3_rt    *wz3_rt_create(const wz3_kx *kx, const wz3_rt_options *options, wz3_result *result);
WZ3_API void       wz3_rt_destroy(wz3_rt *rt);
/* Init pass over the current root (animation at time 0, caches of the Misc runtime classes). */
WZ3_API wz3_result wz3_rt_precalc(wz3_rt *rt);
/* One frame: beat is the 16.16 song position, milliseconds the wall clock of the frame. */
WZ3_API wz3_result wz3_rt_frame(wz3_rt *rt, int32_t beat, int32_t milliseconds);
WZ3_API wz3_result wz3_rt_frame_at_sample(wz3_rt *rt, int32_t sample, int32_t sample_rate);
/* Audio sample -> 16.16 beat (buzz timing quantizes to 1/8 of the 16.16 step). */
WZ3_API int32_t    wz3_sample_to_beat(int32_t sample, uint32_t song_bpm_fixed, int buzz_timing, int32_t sample_rate);

/* Everything that survives from one frame to the next (animated parameter words, event ranges, instance
   chains, time bookkeeping) in a flat buffer, so a seek can restore a nearby state and advance from it.
   Take and restore snapshots between frames; the buffer is portable across runs of the same document. */
WZ3_API size_t     wz3_rt_snapshot_size(const wz3_rt *rt);
WZ3_API wz3_result wz3_rt_snapshot(const wz3_rt *rt, void *buffer, size_t size);   /* WZ3_ERR_LIMIT: too small */
WZ3_API wz3_result wz3_rt_restore(wz3_rt *rt, const void *buffer, size_t size);   /* WZ3_ERR_FORMAT: foreign */

/* The environment of a Bitmap_Render a frame of its own at beat 0 with the given aspect (width / height of
   the bitmap) in which operator `input` - the first input of the Render operator - runs. Afterwards wz3_rt_frame_jobs() describes that
   frame and *root is the node its output (lastOutput) names, -1 when it produced none; the host draws that node and then calls
   wz3_rt_render_end, which restores everything wz3_rt_snapshot covers and the aspect, so the frame in progress of the player is not
   disturbed (its frame jobs are gone). Only after wz3_rt_precalc; calls do not nest (WZ3_ERR_ARGUMENT). The restore allocates: when it runs
   out of memory render_end returns the error and keeps the saved state, so calling it again finishes the job; until it succeeded the runtime
   must not be used for frames. */
WZ3_API wz3_result wz3_rt_render_begin(wz3_rt *rt, uint32_t input, double aspect, int32_t *root);
WZ3_API wz3_result wz3_rt_render_end(wz3_rt *rt);

/* ======================================================================== */
/*  frame jobs: materials, scene traversal, IPP, effects (genmaterial.cpp,      */
/*  genscene.cpp, genoverlay.cpp, geneffectdebris.cpp). Pure data; operators are referred to  */
/*  by index, so a renderer keys its caches on (operator, revision).         */
/* ======================================================================== */

typedef enum wz3_usage {
  WZ3_USAGE_BASE = 0, WZ3_USAGE_PRELIGHT, WZ3_USAGE_SHADOW, WZ3_USAGE_LIGHT, WZ3_USAGE_POSTLIGHT,
  WZ3_USAGE_POSTLIGHT2, WZ3_USAGE_AMBIENT, WZ3_USAGE_OTHER
} wz3_usage;
typedef enum wz3_program {
  WZ3_PROGRAM_STATIC = 0, WZ3_PROGRAM_INSTANCES, WZ3_PROGRAM_SPRITES, WZ3_PROGRAM_THICK_LINES, WZ3_PROGRAM_SHADOW
} wz3_program;
typedef enum wz3_pass_state {   /* the legacy pass a material pass was compiled from */
  WZ3_PSTATE_M11_SINGLE = 0, WZ3_PSTATE_M11_BASE, WZ3_PSTATE_M11_LIGHT, WZ3_PSTATE_M11_POSTLIGHT,
  WZ3_PSTATE_M11_POSTLIGHT2, WZ3_PSTATE_SHADOW_VOLUME, WZ3_PSTATE_M20_ZFILL, WZ3_PSTATE_M20_TEXTURE,
  WZ3_PSTATE_M20_AMBIENT, WZ3_PSTATE_M20_LIGHT, WZ3_PSTATE_M20_ENVIRONMENT
} wz3_pass_state;

typedef struct wz3_material_pass {
  uint8_t usage, program, state;
  int32_t render_pass;
  double size, aspect;
  uint32_t owner;              /* operator whose parameters this pass reads (Material_Add keeps the upstream owner) */
} wz3_material_pass;

/* Material 1.1 / 2.0 record. params follow the operator's packing (floats and integers as doubles); initial is the
   parameter set at init, which the legacy code compiled state words and sampler setups from. version counts the
   animated parameter updates. textures[] are bitmap operator indices (-1 none): 1.1 link slots 0..6, 2.0 the same
   seven sources resolved through its input selectors. */
typedef struct wz3_material {
  uint8_t system;              /* 11 or 20 */
  uint32_t n_params;
  const double *params, *initial;
  uint32_t version;
  int32_t textures[7];
  uint32_t n_passes;
  const wz3_material_pass *passes;
  uint32_t multi_flags, shader_mask;   /* 1.1 */
} wz3_material;

typedef enum wz3_effect_type { WZ3_EFFECT_CHAIN_LINE = 1, WZ3_EFFECT_WATER, WZ3_EFFECT_GLARE, WZ3_EFFECT_COLOR_CORRECTION } wz3_effect_type;
typedef struct wz3_effect {
  uint8_t type, usage, need_current_render;
  int32_t material_op;         /* -1 none */
  int32_t pass;
} wz3_effect;

typedef struct wz3_camera {
  float camera_space[16], model_space[16];
  double far_clip, near_clip, center_x, center_y, zoom_x, zoom_y;
  uint32_t fog_color;
  double fog_start, fog_end;
} wz3_camera;

typedef struct wz3_light_job {
  uint8_t directional;
  uint32_t op;                 /* 0 for the viewport's default camera light */
  float position[3], direction[3];
  uint32_t flags, color;
  float amplify;
  double range;
  int32_t event;               /* event index, -1 none */
} wz3_light_job;

typedef struct wz3_mesh_job {
  int32_t mesh_op;             /* Mesh / MinMesh operator whose geometry is drawn; -1 when the object is no geometry */
  int32_t source_op;           /* the operator that scheduled it (the scene input; -1 for Particles without input) */
  float matrix[16];
  float time;
  int32_t pass_adjust;
  uint8_t has_instances;       /* Particles with the instance flag: n_instances matrices at frame->instances */
  uint32_t first_instance, n_instances;
} wz3_mesh_job;

typedef struct wz3_effect_job {
  uint32_t effect_op;
  float matrix[16];
  uint32_t variables;          /* offset into frame->effect_variables; 32 vectors of 4 floats */
  uint32_t markers;            /* index of a table of 32 matrices in frame->marker_tables */
  int32_t pass_adjust;
} wz3_effect_job;

enum { WZ3_IPP_VIEWPORT = 1, WZ3_IPP_LAYER2D = 2 };
typedef struct wz3_ipp_node {
  uint8_t type;
  int32_t size;
  uint32_t flags;
  /* viewport */
  uint32_t clear_color;
  wz3_camera camera;
  float crop[4];
  float eye_distance, focal_distance;
  int32_t paint_mode;
  uint32_t first_mesh, n_mesh, first_effect, n_effect, first_light, n_light, marker_table, ambient;
  wz3_light_job default_light;
  /* layer 2D */
  int32_t input;               /* node index, -1 none */
  int32_t material_op;
  double screen[4], uv[4];
  float z;
  int32_t clear_flags;
} wz3_ipp_node;

/* ======================================================================== */
/*  render-ready geometry                                                   */
/* ======================================================================== */

/* One draw range of a geometry: Mesh -> material slot index, MinMesh -> cluster index; pass is the render pass. */
typedef struct wz3_geometry_group {
  int32_t slot, pass;
  uint32_t start, count;           /* index range */
  /* what a renderer needs without the authoring data (set by wz3_mesh_prepare / wz3_minmesh_prepare): the operator of the
     slot's material (WZ3_GROUP_MATERIAL_NONE for slot 0 / cluster 0, WZ3_GROUP_MATERIAL_DEFAULT for the built-in default) and
     the box of the vertices the group indexes (has_bounds == 0: none valid, the whole-mesh box applies). Not part of
     wz3_geometry_hash. */
  int32_t material;
  uint8_t has_bounds;
  float bounds_min[3], bounds_max[3];
} wz3_geometry_group;
enum { WZ3_GROUP_MATERIAL_NONE = -1, WZ3_GROUP_MATERIAL_DEFAULT = -2 };

/* The buffers the engine (engine.cpp) uploads for a mesh or MinMesh: vertex
   streams of n_vertices entries, a triangle index stream grouped by material, bounds. Old Mesh vertices have float
   colours and no second UV channel (uv1 aliases uv0, uv1_is_uv0 = 1); MinMesh vertices have byte colours and four bone
   lanes. Free with wz3_geometry_free. */
typedef struct wz3_geometry {
  uint32_t n_vertices, n_indices, n_groups;
  float *positions;                /* 3 per vertex */
  float *normals;                  /* 3 */
  float *tangents;                 /* 4 */
  float *uv0, *uv1;                /* 2 */
  float *colors;                   /* 4 floats (Mesh) or NULL */
  uint8_t *colors_u8;              /* 4 bytes RGBA (MinMesh) or NULL */
  float *bone_weights;             /* 4 (MinMesh) or NULL */
  uint16_t *bone_indices;          /* 4 */
  uint32_t *indices;
  wz3_geometry_group *groups;
  float bounds_min[3], bounds_max[3];
  uint8_t uv1_is_uv0;
  uint8_t animated;                /* a MinMesh with bones: its jobs are unbounded (meshHasAnimation) */
  void *block_;
  size_t block_bytes_;             /* size of the allocation behind block_ */
} wz3_geometry;
/* Recomputes every group's box from the position and index streams (the single group of a mesh takes the whole-mesh box);
   what the prepare functions do, exposed for geometry built by other producers. */
WZ3_API void wz3_geometry_group_bounds(wz3_geometry *g);
/* The geometry of an animated MinMesh at `time` (MinMesh.prepare({ time }) through normalizePreparedGeometry): `base` is
   the mesh's static wz3_minmesh_prepare geometry; the result aliases its uv, colour, bone, index and bounds-independent
   streams (so `base` must outlive it), owns the skinned positions, normals and tangents, and gets bounds recomputed from
   the positions (a single group takes them, several have none: animated jobs are unbounded). If the mesh has no valid
   normals they are derived on a copy first, so keep them calculated to avoid that cost per frame. Free with
   wz3_geometry_free. WZ3_ERR_ARGUMENT for a mesh without bones or a base of another mesh. */
WZ3_API wz3_result wz3_rt_minmesh_pose_geometry(const wz3_rt *rt, const wz3_minmesh *mesh, const wz3_geometry *base, double time, wz3_geometry *out);

/* Static geometry (derives normals on a private copy when the mesh has none). */
WZ3_API wz3_result wz3_mesh_prepare(const wz3_mesh *mesh, wz3_geometry *out);
WZ3_API wz3_result wz3_minmesh_prepare(const wz3_minmesh *mesh, wz3_geometry *out);
/* A private copy of a MinMesh the host owns after the sink returned (the runtime frees its own): normals calculated, so that
   wz3_rt_minmesh_pose_geometry poses it every frame without a copy. NULL when out of memory. Free with wz3_minmesh_release. */
WZ3_API wz3_minmesh *wz3_minmesh_retain(const wz3_minmesh *mesh);
WZ3_API void         wz3_minmesh_release(wz3_minmesh *mesh);
WZ3_API void       wz3_geometry_free(wz3_geometry *g);
/* Bytes of the single allocation behind a prepared geometry (0 for an empty or freed one). */
WZ3_API size_t     wz3_geometry_bytes(const wz3_geometry *g);
/* Marks (1 byte per operator, `marks` has kx->n_ops entries and is cleared first) the Mesh / MinMesh operators whose result is
   consumed by an operator that is no Mesh / MinMesh: the geometry a scene can draw. The other geometry operators are
   intermediates of the generators - on the production document 574 of 10,940 - so a host that prepares only the marked
   ones in its sink skips about 95% of the work. Returns the number marked. */
WZ3_API uint32_t   wz3_kx_scene_geometry(const wz3_kx *kx, uint8_t *marks);
/* FNV-1a over every stream, the index buffer, groups and bounds (the reference fixture). */
WZ3_API uint32_t   wz3_geometry_hash(const wz3_geometry *g);

/* What the last wz3_rt_frame() produced. Valid until the next frame; outputs are the committed IPP roots in input
   order (environment.frameOutputs). */
typedef struct wz3_frame {
  uint32_t n_outputs;  const uint32_t *outputs;
  uint32_t n_nodes;    const wz3_ipp_node *nodes;
  const wz3_mesh_job *mesh_jobs;
  const wz3_effect_job *effect_jobs;
  const wz3_light_job *light_jobs;
  const float *instances;          /* 16 floats per instance, sMatrix order */
  const float *effect_variables;
  const float *marker_tables;      /* 32 matrices per table */
} wz3_frame;

enum { WZ3_EFFECT_OUT_NONE = 0, WZ3_EFFECT_OUT_CHAIN_LINE, WZ3_EFFECT_OUT_WATER, WZ3_EFFECT_OUT_GLARE, WZ3_EFFECT_OUT_COLOR_CORRECTION };

/* Water grid (geneffectdebris.cpp): positions/normals/uvs are dynamic; indices are static. Vertex colour is
   constant 0xff,0xaa,0x88,0xff, both tangent-space vectors are zero and the shadow vertex map is the identity. */
typedef struct wz3_water_geometry {
  int32_t grid, width, height;
  uint32_t n_vertices, n_indices;
  float *positions, *normals, *uvs;
  uint16_t *indices;
  float bounds_min[3], bounds_max[3];
  uint32_t version, wave_count;
  uint8_t uv_changed;
} wz3_water_geometry;

/* Result of executing one deferred effect job (valid until the next wz3_rt_effect_run). */
typedef struct wz3_effect_output {
  uint8_t kind;
  uint32_t op;
  int32_t material_op;
  float matrix[16];
  const float *points;         /* chain line: 32 positions (3 floats), world space */
  double thickness;
  int32_t ripped;
  uint32_t color;
  int32_t spline_op;           /* water */
  float time;
  const wz3_water_geometry *water;
  uint32_t n_params;
  const double *params;        /* water / glare / colour correction: the operator's parameters */
} wz3_effect_output;

WZ3_API const wz3_material *wz3_rt_material(const wz3_rt *rt, uint32_t op);   /* NULL if op is no material */
WZ3_API const wz3_effect   *wz3_rt_effect(const wz3_rt *rt, uint32_t op);
WZ3_API const wz3_frame    *wz3_rt_frame_jobs(const wz3_rt *rt);
/* Width/height of the master target (KEnvironment::Aspect); 2 for the production letterbox. Default 1. */
WZ3_API void       wz3_rt_set_aspect(wz3_rt *rt, double aspect);
/* Engine::Paint: runs one deferred effect of a viewport with the variables/markers/matrix of its job. */
WZ3_API wz3_result wz3_rt_effect_run(wz3_rt *rt, const wz3_effect_job *job, const wz3_effect_output **out);
/* nativeEffectJobOrder: writes the execution order of a viewport's effect jobs (indices relative to
   first_effect, capacity n_effect); returns the count. */
WZ3_API uint32_t   wz3_frame_effect_order(const wz3_rt *rt, const wz3_ipp_node *viewport, uint32_t *order);
/* advanceEffectFrame: executes every deferred effect of the last frame's IPP tree and discards the output, so the
   stateful ones (chain lines, water) see every time slice while seeking. Returns the executed count in *n. */
WZ3_API wz3_result wz3_rt_effects_advance(wz3_rt *rt, uint32_t *n);

/* ======================================================================== */
/*  render planning (engine.cpp, pure parts): what to draw and how, without */
/*  a graphics API. Bounds are axis-aligned boxes in doubles; planes are     */
/*  (a, b, c, d) with a*x + b*y + c*z + d >= 0 inside.                       */
/* ======================================================================== */

typedef struct wz3_bounds { double min[3], max[3]; } wz3_bounds;

typedef struct wz3_camera_matrices { float view[16], projection[16], view_projection[16]; } wz3_camera_mats;

/* A viewport rectangle in drawing-buffer pixels (bottom-up) and its normalised form. */
typedef struct wz3_region { int32_t x, y, width, height; float uv[4]; } wz3_region;

/* General 4x4 inverse, rounded to floats; 0 when singular or not finite. */
WZ3_API int  wz3_mat4_inverse(const float a[16], float out[16]);
/* cameraMatrices(): view = inverse(camera space), the legacy projection (infinite far plane) and their product. */
WZ3_API void wz3_camera_matrices(const wz3_camera *camera, wz3_camera_mats *out);

/* composeInstanceMatrices: has_instances == 0 gives the base matrix, 0 instances give nothing, otherwise
   base * instance for each. Returns the number of matrices written to out (n_instances, 1 or 0). */
WZ3_API uint32_t wz3_compose_instances(const float base[16], int has_instances, const float *instances, uint32_t n_instances, float *out);
/* transformGeometryBounds: 0 when the box or the matrix is not finite/valid or the matrix is projective. */
WZ3_API int wz3_transform_bounds(const double minimum[3], const double maximum[3], const float *matrix, wz3_bounds *out);
/* meshJobWorldBounds: union over n matrices (16 floats each); 0 when any instance is unknown or n is 0. */
WZ3_API int wz3_job_world_bounds(const double minimum[3], const double maximum[3], const float *matrices, uint32_t n, wz3_bounds *out);
/* boundsIntersectsSphere / lightIntersectsWorldBounds: unknown data keeps the work (returns 1). bounds may be NULL. */
WZ3_API int wz3_bounds_intersect_sphere(const wz3_bounds *bounds, const double center[3], double radius);
WZ3_API int wz3_light_intersects_bounds(const wz3_light_job *light, const wz3_bounds *bounds);
/* viewFrustumPlanes: four side planes and the near plane (no far plane), normalised; returns the plane count (0: none). */
WZ3_API uint32_t wz3_view_frustum_planes(const float view_projection[16], double planes[5][4]);
WZ3_API int wz3_light_intersects_frustum(const wz3_light_job *light, const double (*planes)[4], uint32_t n_planes);
/* shadowViewFrustumPlanes: the five planes with the light inside; returns 5 or 0. */
WZ3_API uint32_t wz3_shadow_view_frustum_planes(const float view_projection[16], const float light_position[3], double planes[5][4]);
/* lightSphereBounds (Engine::CalcSphereBounds): the light's normalised screen rectangle (x0, y0, x1, y1); 0 if unknown. */
WZ3_API int wz3_light_sphere_bounds(const float position[3], double range, const float view[16], double zoom_x, double zoom_y, double rect[4]);
/* shadowZFailVolumePlanes (sFrustum::ZFailVolume): 6 planes, or 2 when the near plane passes the light; 0 if unknown.
   view and rectangle may be NULL (computed from the camera and the light). */
WZ3_API uint32_t wz3_shadow_zfail_planes(const wz3_camera *camera, const wz3_light_job *light, const float *view,
                                         const double *rectangle, double planes[6][4]);
WZ3_API int wz3_caster_uses_zfail(const wz3_bounds *bounds, const double (*planes)[4], uint32_t n_planes);
WZ3_API int wz3_bounds_outside_planes(const wz3_bounds *bounds, const double (*planes)[4], uint32_t n_planes);
WZ3_API int wz3_caster_may_affect_view(const wz3_light_job *light, const wz3_bounds *bounds, const float view_projection[16]);
/* selectActiveLights: indices into the node's light jobs of at most `limit` lights, strongest first; when none
   survives the viewport's fallback light is selected as index (uint32_t)-1. Returns the count. */
WZ3_API uint32_t wz3_select_active_lights(const wz3_frame *frame, const wz3_ipp_node *viewport, uint32_t limit, uint32_t *indices);

WZ3_API uint32_t wz3_viewport_clear_flags(uint32_t flags);
WZ3_API void wz3_fit_aspect_region(int32_t width, int32_t height, double aspect, wz3_region *out);
WZ3_API void wz3_viewport_region(const float crop[4], int32_t width, int32_t height, wz3_region *out);
/* lightScissorRegion: LightRect (x0, y0, x1, y1 in -1..1) into `viewport` (x, y, width, height); rect may be NULL. */
WZ3_API void wz3_light_scissor_region(const double *rect, const wz3_region *viewport, wz3_region *out);
/* layerQuadGeometry: clip-space corners (12 floats) and uvs (8 floats) of a Layer2D quad. */
WZ3_API void wz3_layer_quad_geometry(const wz3_ipp_node *layer, float positions[12], float uvs[8]);

/* ---- lighting and post-processing math (engine.cpp) ---- */

WZ3_API void   wz3_tangent_basis_normal(const double normal[3], const double tangent[3], const double mapped[3], double out[3]);
WZ3_API double wz3_material_light_attenuation(double normalized_distance);
/* sMatrix::TransR: transposed upper 3x3 applied to (vector - translation); point == 0 skips the translation. */
WZ3_API void   wz3_legacy_transr_vector(const float matrix[16], const double vector[3], int point, float out[3]);
typedef struct wz3_vertex_lighting { float normal[3], tangent[3], bitangent[3], light[3], halfway[3], attenuation[3]; } wz3_vertex_lighting;
/* material11.vsh / material20_light.vsh: light (xyz, w), attenuation (centre xyz, inverse range w); basis conditioned for skinned variants. */
WZ3_API void   wz3_legacy_vertex_lighting(const double position[3], const double normal[3], const double tangent[3], const double light[4],
                                          const double attenuation[4], const double camera[3], int condition_basis, wz3_vertex_lighting *out);
WZ3_API double wz3_material11_specular(double cosine, double power);
WZ3_API void   wz3_material11_light_constant(uint32_t material_color, uint32_t light_color, double amplify, float out[4]);
/* Exec_Effect_ColorCorrection: the seven pixel constants (28 floats) and the per-pixel result; parameters are the operator's words. */
WZ3_API void   wz3_color_correction_constants(const double *parameters, uint32_t n, float out[28]);
WZ3_API void   wz3_evaluate_color_correction(const double *parameters, uint32_t n, const double source[4], float out[4]);
typedef struct wz3_glare_plan {
  int32_t flags;
  uint8_t downsample[2], copy_downsample;
  float threshold[4], gray_scale;
  double gray_mix;
  double blur[2], amplify[2];
  float glare_color[4], original_color[4];
  double add_smooth;
} wz3_glare_plan;
WZ3_API void   wz3_glare_plan_make(const double *parameters, uint32_t n, wz3_glare_plan *out);
WZ3_API void   wz3_evaluate_glare_tone(const double *parameters, uint32_t n, const double source[4], float out[4]);
WZ3_API void   wz3_evaluate_glare_composite(const double *parameters, uint32_t n, const double glare[4], const double original[4], float out[4]);
/* buildChainRibbon: two vertices per point across the camera axis (view may be NULL = identity); writes 6 floats and 4 uv floats
   per point and up to 6 indices per segment (the ripped segment is left out); returns the index count. */
WZ3_API uint32_t wz3_build_chain_ribbon(const float *points, uint32_t count, double thickness, int32_t ripped, const float *view,
                                        float *positions, float *uvs, uint16_t *indices);

/* One entry of the draw list; sort with wz3_sort_render_items (stable, like the native radix sort). */
typedef struct wz3_render_item {
  int32_t render_pass;
  uint8_t usage;           /* wz3_usage */
  uint8_t is_effect;
  int32_t light_index;     /* ordinary light index of shadow and light passes */
  int32_t sort_material;
  int32_t sort_order;      /* default -(job operator id) */
  uint32_t sequence;       /* insertion order, set by the caller: equal keys keep it (the sort of a draw list is stable) */
  uint8_t insert_kind;     /* WZ3_INSERT_*: the generated-pass kind of the item's material (not of effects) */
} wz3_render_item;
WZ3_API void wz3_sort_render_items(wz3_render_item *items, uint32_t n);
/* effectRenderItem: the draw-list entry of a deferred effect job. */
WZ3_API void   wz3_effect_render_item(const wz3_rt *rt, const wz3_effect_job *job, int32_t sort_order, wz3_render_item *out);
/* The same from the effect record itself (wz3_rt_effect): pass and usage with the job's render pass adjustment. */
WZ3_API void   wz3_effect_render_item_make(const wz3_effect *effect, const wz3_effect_job *job, int32_t sort_order, wz3_render_item *out);

/* ======================================================================== */
/*  material views (genmaterial.cpp, engine.cpp):      */
/*  what a pass of a material means to a renderer, without a graphics API.   */
/* ======================================================================== */

/* sMBF_* words of wz3_material_view.base_flags */
enum {
  WZ3_MBF_ALPHATEST = 0x0001, WZ3_MBF_DOUBLESIDED = 0x0002, WZ3_MBF_INVERTCULL = 0x0004, WZ3_MBF_ZBIASBACK = 0x0008,
  WZ3_MBF_ZBIASFORE = 0x0010, WZ3_MBF_ZONLY = 0x0020, WZ3_MBF_ZEQUAL = 0x0080, WZ3_MBF_ZWRITE = 0x0100,
  WZ3_MBF_ZREAD = 0x0200, WZ3_MBF_SHADOWMASK = 0x040000, WZ3_MBF_STENCILTEST = 0x300000
};
/* wz3_material_view.state when the pass is no wz3_pass_state */
enum { WZ3_VIEW_STATE_DEFAULT = -1, WZ3_VIEW_STATE_NONE = -2 };
enum { WZ3_GENERATED_ALPHA_NONE = 0, WZ3_GENERATED_ALPHA_ZERO, WZ3_GENERATED_ALPHA_TEXTURE0 };

/* The state a pass is drawn with. Fields of the other system are zero (1.1: light_flags, slot_*, combiners, colors,
   generated_base_alpha, material11_combiner; 2.0: light_flags4, light_scales, detail_ops, use_prelight, light_scale,
   flags, runtime_*, environment_flags). texture_map[i] is the source texture of sampler slot i (-1: none). */
typedef struct wz3_material_view {
  uint8_t system;                          /* 11 or 20 */
  uint8_t usage;                           /* wz3_usage */
  uint8_t vertex_color_mode, material11_combiner, generated_base_alpha, use_prelight;
  int8_t  texture_map[4];
  int32_t state;                           /* wz3_pass_state, WZ3_VIEW_STATE_DEFAULT, WZ3_VIEW_STATE_NONE */
  uint32_t base_flags, special_flags, light_flags, flags, runtime_flags, environment_flags, runtime_environment_flags;
  uint32_t multi_flags, alpha_combiner, color, specular, specular_color;
  uint32_t texture_flags[4], extra_texture_flags[4], light_flags4[4], slot_texture_flags[4], combiners[13], colors[4];
  uint8_t detail_ops[2];
  /* bit i: the scale entry exists. The JavaScript view builds its scale arrays from slices of the parameter words, so a short
     parameter array leaves entries undefined (not 0); only the sampler setup tells the two apart (an undefined scale reads as 1). */
  uint8_t texture_scale_present, light_scale_present, slot_scale_present;
  double texture_scales[4], extra_texture_scales[4], light_scales[4], slot_texture_scales[4];
  double specular_power, specular_strength, uv_scale, alpha_cutoff, light_scale;
  float uv_transform1[8], uv_transform2[8];   /* two dot-product rows each: (a b c d) for u, then for v */
  const int32_t *textures;                 /* the owner's seven bitmap operators (-1 none); NULL for the default material */
} wz3_material_view;

/* materialView(material, pass): `material` is the operator's record (its animated parameters), `owner` the record the
   pass keeps (Material_Add; NULL: the same), `pass` may be NULL. A NULL material is the player's DefaultMat with its
   sole light pass. Parameter arrays shorter than the packing (64 words for 1.1, 42 for 2.0) read as missing (zero). */
WZ3_API void wz3_material_view_make(const wz3_material *material, const wz3_material *owner, const wz3_material_pass *pass, wz3_material_view *out);

/* renderMode(view): the fragment program family (0 constant ... 7 combiner). */
WZ3_API int wz3_material_render_mode(const wz3_material_view *view);

enum { WZ3_DEPTH_ALWAYS = 0, WZ3_DEPTH_EQUAL, WZ3_DEPTH_LESS, WZ3_DEPTH_LEQUAL };
enum { WZ3_CULL_BACK = 0, WZ3_CULL_FRONT, WZ3_CULL_NONE };
enum { WZ3_BLEND_ZERO = 0, WZ3_BLEND_ONE, WZ3_BLEND_SRC_ALPHA, WZ3_BLEND_ONE_MINUS_SRC_ALPHA, WZ3_BLEND_SRC_COLOR,
       WZ3_BLEND_ONE_MINUS_SRC_COLOR, WZ3_BLEND_DST_COLOR, WZ3_BLEND_ONE_MINUS_DST_COLOR, WZ3_BLEND_DST_ALPHA };
enum { WZ3_BLEND_EQ_ADD = 0, WZ3_BLEND_EQ_REVERSE_SUBTRACT };
typedef struct wz3_material_state {
  uint8_t depth_test, depth_write, depth_func, color_write, cull, stencil_test;
  uint8_t blend, blend_src, blend_dst, blend_equation;   /* blend == 0: blending off */
} wz3_material_state;
/* materialState(view): render state from base_flags. */
WZ3_API void wz3_material_state_make(const wz3_material_view *view, wz3_material_state *out);

/* materialSamplerSetup(view): the four sampler slots of a pass as bindMaterial hands them to the shader and to the texture
   cache. texture_op[i] is the bitmap operator the slot samples (-1: none, bind the fallback); the uniform words are the
   uM20SamplerFlags/Scales and uM11SamplerFlags/Scales arrays; bind_flags[i] selects the filter (low 3 bits) and clamping
   (bit 8) the slot binds with; texture_mask is uTextureMask (bit i: slot i has a texture operator). */
typedef struct wz3_sampler_setup {
  int32_t texture_op[4];
  int32_t m20_flags[4], m11_flags[4];
  float m20_scales[4], m11_scales[4];
  uint32_t bind_flags[4];
  int32_t texture_mask;
} wz3_sampler_setup;
WZ3_API void wz3_material_sampler_setup(const wz3_material_view *view, wz3_sampler_setup *out);
/* The request slot `i` of a setup binds with: clamp = bit 8, filter mode = the low three bits, profile of the view's system. */
WZ3_API void wz3_sampler_setup_request(const wz3_material_view *view, const wz3_sampler_setup *setup, int slot, wz3_sampler_request *out);

/* Material 2.0 UV helpers; params is the operator's parameter array (n words). */
WZ3_API void wz3_material20_uv_transforms(const double *params, uint32_t n, float transform1[8], float transform2[8]);
WZ3_API void wz3_material20_uv(const double *params, uint32_t n, int32_t source_index, const double uv[2], float out[2]);
/* transformDirection3: matrix may be NULL (identity); rounds to floats. */
WZ3_API void wz3_transform_direction3(const float *matrix, const double vector[3], float out[3]);
WZ3_API void wz3_material20_environment_bump_direction(const double normal[3], const double tangent[3], const double sample[3],
                                                       const float *model, const float *view, int reflection, float out[3]);
WZ3_API void wz3_material20_environment_eye(const float model[16], const double model_position[3], const double camera_world[3], float out[3]);
WZ3_API void wz3_material11_environment_normals(const double normal[3], const float *model, int condition_basis, float model_out[3], float world_out[3]);
/* eye may be NULL (only read when reflecting) */
WZ3_API void wz3_material20_environment_direction(const double normal[3], const double *eye, int reflection, float out[3]);
WZ3_API void wz3_material20_environment_uv(const double direction[3], float out[2]);
/* Material 1.1: SRT1 (9 words) and SRT2 (5 words, radians) as the view's two transform row pairs. */
WZ3_API void wz3_material11_uv_transforms(const double srt1[9], const double srt2[5], float transform1[8], float transform2[8]);
/* material11.vsh texture-coordinate block: inputs are 4-vectors in the named spaces, NULL = absent. */
typedef struct wz3_m11_uv_inputs {
  const double *uv0, *uv1, *eye_normal, *normal, *eye_position, *position, *world_position;
} wz3_m11_uv_inputs;
WZ3_API void wz3_material11_uv(const wz3_material_view *view, int32_t slot, const wz3_m11_uv_inputs *inputs, float out[2]);

/* The two source-defined colour combiners as CPU oracles. Texture samples are RGBA floats, NULL = white (absent). */
WZ3_API void wz3_evaluate_material20_prelight(const wz3_material_view *view, const float *sample0, const float *sample1, const float *sample2, float out[4]);
WZ3_API void wz3_evaluate_material11_combiner(const wz3_material_view *view, const float *const textures[4], const float *vertex, float out[4]);

/* ======================================================================== */
/*  draw list (engine.cpp up to the sorted item list):    */
/*  insert tracker, effect items, mesh render items, culling, sort identity  */
/* ======================================================================== */

enum { WZ3_INSERT_NONE = 0, WZ3_INSERT_MATERIAL11, WZ3_INSERT_MATERIAL20 };
enum { WZ3_INSERT_ACTION_NONE = 0, WZ3_INSERT_ACTION_CLEAR_DESTINATION_ALPHA, WZ3_INSERT_ACTION_ADD_DESTINATION_ALPHA };
enum { WZ3_DRAW_PASS = 0, WZ3_DRAW_SHADOW, WZ3_DRAW_EFFECT };
enum { WZ3_MAX_LIGHTS = 16 };   /* the number of lights a viewport uses (MAX_LIGHTS) */

/* One entry of a viewport's draw list. `key` holds what the sort and the insert tracker read. Passes and shadow volumes
   refer to a mesh job of the node (job is relative to first_mesh), effects to an effect job (relative to first_effect). */
typedef struct wz3_draw_item {
  wz3_render_item key;
  uint8_t kind;                    /* WZ3_DRAW_* */
  uint8_t shadow_zfail;            /* shadow volume: the caster may cross the near plane, so it needs caps (Carmack's reverse) */
  uint32_t job;
  int32_t material_op;             /* operator of the item's material, -1 for DefaultMat */
  int32_t pass_index;              /* index into the material's passes, -1 for the default pass */
  int32_t group;                   /* geometry group of a pass; -1 otherwise */
  uint32_t shadow_first, shadow_count;   /* shadow volume: range of wz3_draw_list.shadow_groups (group indices, repeated per shadow pass) */
} wz3_draw_item;

typedef struct wz3_draw_list {
  wz3_draw_item *items;     uint32_t n_items, cap_items;
  uint32_t *shadow_groups;  uint32_t n_shadow_groups, cap_shadow_groups;
} wz3_draw_list;

/* The culling counters of drawViewportScene (cullingStats). */
typedef struct wz3_draw_stats {
  uint32_t candidate_view, culled_view, candidate_light, culled_light;
  uint32_t candidate_shadow, culled_shadow_frustum, culled_shadow_sphere;
} wz3_draw_stats;

/* materialPassSortIdentity: native sorts passes by the address of their GenMaterialPass; here the first request for a
   (material operator, pass index) hands out the next number, exactly like the JavaScript port's WeakMap (the default
   pass is (-1, -1)). Keep one registry for the whole run and ask in draw-list order. */
typedef struct wz3_pass_sort_ids {
  uint64_t *keys;
  int32_t *ids;
  uint32_t count, capacity;
  int32_t next;
} wz3_pass_sort_ids;
WZ3_API void    wz3_pass_sort_ids_init(wz3_pass_sort_ids *s);
WZ3_API void    wz3_pass_sort_ids_free(wz3_pass_sort_ids *s);
WZ3_API int32_t wz3_pass_sort_id(wz3_pass_sort_ids *s, int32_t material_op, int32_t pass_index);   /* < 0: out of memory */

/* materialInsertKind: 2.0 materials and multi-pass 1.1 materials (generated passes) insert work between passes. */
WZ3_API int wz3_material_insert_kind(const wz3_material *material);
/* material11InsertAction: what a Material 1.1 insert does when a pass of this usage ends. */
WZ3_API int wz3_insert_action(int insert_kind, uint8_t usage);
/* renderUsageChanged: render pass, usage or light differs (or either item is missing). */
WZ3_API int wz3_render_usage_changed(const wz3_render_item *previous, const wz3_render_item *next);
/* createMaterialInsertTracker: feed the sorted list in order; transition() returns the action owed for the item that ended
   (WZ3_INSERT_ACTION_*), finish() the one for the last. Render passes of the items are 0..255 (the producers clamp). */
typedef struct wz3_insert_tracker {
  uint8_t kind[256];
  uint8_t has_previous;
  wz3_render_item previous;
} wz3_insert_tracker;
WZ3_API void wz3_insert_tracker_init(wz3_insert_tracker *t, const wz3_draw_item *items, uint32_t n);
WZ3_API int  wz3_insert_tracker_transition(wz3_insert_tracker *t, const wz3_draw_item *item);
WZ3_API int  wz3_insert_tracker_finish(wz3_insert_tracker *t);
/* forEachNativeHeadInsertedJob: AddPaintJob links jobs at the head of their list, so they are visited last to first. */
WZ3_API uint32_t wz3_head_inserted_job(uint32_t count, uint32_t position);

WZ3_API void wz3_draw_list_init(wz3_draw_list *list);
WZ3_API void wz3_draw_list_clear(wz3_draw_list *list);   /* keeps the storage */
WZ3_API void wz3_draw_list_free(wz3_draw_list *list);
/* The stable sort of drawViewportScene: sets every item's insertion order, then sorts by the draw-list key. */
WZ3_API void wz3_draw_list_sort(wz3_draw_list *list);

/* What a viewport computes once before it builds the list: camera matrices, the five view planes, and for each selected
   light its shadow-view frustum (none for directional lights) and z-fail volume. */
typedef struct wz3_viewport_plan {
  wz3_camera_mats matrices;
  uint32_t n_view_planes;  double view_planes[5][4];
  uint32_t n_lights;       const wz3_light_job *lights[WZ3_MAX_LIGHTS];
  uint32_t n_shadow_planes[WZ3_MAX_LIGHTS];  double shadow_planes[WZ3_MAX_LIGHTS][5][4];
  uint32_t n_zfail_planes[WZ3_MAX_LIGHTS];   double zfail_planes[WZ3_MAX_LIGHTS][6][4];
  uint8_t has_rect[WZ3_MAX_LIGHTS];          double rect[WZ3_MAX_LIGHTS][4];   /* lightSphereBounds */
} wz3_viewport_plan;
WZ3_API void wz3_viewport_plan_make(const wz3_camera *camera, const wz3_light_job *const *lights, uint32_t n_lights, wz3_viewport_plan *out);

/* The data a draw list reads besides the frame: material records by operator (NULL: no material, DefaultMat), effect
   records by operator and the prepared geometry of a mesh job (NULL: draw nothing). */
typedef struct wz3_draw_source {
  void *user;
  const wz3_material *(*material)(void *user, int32_t op);
  const wz3_effect *(*effect)(void *user, uint32_t op);
  const wz3_geometry *(*geometry)(void *user, const wz3_mesh_job *job);
} wz3_draw_source;

/* appendMeshRenderItems for the job `job_index` of the node (relative to first_mesh). */
WZ3_API wz3_result wz3_draw_list_append_mesh(wz3_draw_list *list, const wz3_draw_source *source, const wz3_mesh_job *job, uint32_t job_index,
                                             const wz3_geometry *geometry, const wz3_viewport_plan *plan, wz3_pass_sort_ids *ids,
                                             wz3_draw_stats *stats);
/* The unsorted draw list of one viewport node: lights, plan, effect items and mesh items in native head-inserted
   order. `plan` receives the viewport plan the list was built with (may be NULL). */
WZ3_API wz3_result wz3_draw_list_build(wz3_draw_list *list, const wz3_draw_source *source, const wz3_frame *frame,
                                       const wz3_ipp_node *viewport, wz3_pass_sort_ids *ids, wz3_viewport_plan *plan, wz3_draw_stats *stats);

/* ======================================================================== */
/*  shadow volumes (engine.cpp, and the shadow jobs of genmesh.cpp and          */
/*  genminmesh.cpp): the welded polygon/edge structure of a caster, and the index list that extrudes its silhouette from a light */
/* ======================================================================== */

/* does the material of this operator have a shadow pass (materialCastsShadow); material_op >= 0 */
typedef int (*wz3_shadow_casts_fn)(void *user, int32_t material_op);

/* The native shadow job a Mesh / MinMesh serialises while its half-edge structure still exists (EngMesh::PrepareJob): shadow
   vertices, fan triangles with one plane per source polygon, silhouette edges with their two planes. `key` lists the
   (start, count) ranges of the caster groups the job was made for; a topology request for other ranges does not use it. */
typedef struct wz3_shadow_recipe {
  uint32_t n_source, n_triangles, n_planes, n_edges, n_key;
  uint32_t trailing_plane_slots, source_non_manifold_edges;
  uint32_t *source_indices;      /* n_source: prepared vertex of each shadow vertex */
  uint32_t *faces;               /* 3 per triangle, shadow vertices */
  uint32_t *triangle_planes;     /* n_triangles */
  float *planes;                 /* 4 per plane; plane 0 is reserved */
  uint32_t *edge_vertices;       /* 2 per edge */
  uint32_t *edge_planes;         /* 2 per edge */
  uint32_t *key;                 /* 2 per range */
} wz3_shadow_recipe;

/* What a geometry offers the shadow code besides its positions and indices (normalizePreparedGeometry's shadow fields). Any
   array may be NULL. Free with wz3_shadow_data_free. */
typedef struct wz3_shadow_data {
  uint32_t n_vertices, n_triangles, n_face_normals;
  uint32_t *vertex_map;          /* shadowVertexMap: the vertex each vertex welds to */
  uint8_t *triangle_mask;        /* shadowTriangleMask: 0 for a triangle of a polygon that casts nothing */
  uint32_t *face_map;            /* shadowFaceMap: the source polygon of each triangle */
  float *face_normals;           /* shadowFaceNormals: 3 per source polygon (MinMesh) */
  wz3_shadow_recipe *recipe;
} wz3_shadow_data;
WZ3_API void wz3_shadow_data_free(wz3_shadow_data *d);
/* Shadow fields of a prepared geometry of a Mesh / MinMesh (call after wz3_mesh_prepare / wz3_minmesh_prepare; `casts` decides which
   slots / clusters cast). The recipe is NULL when nothing casts. */
WZ3_API wz3_result wz3_mesh_shadow_data(const wz3_mesh *mesh, const wz3_geometry *geometry, wz3_shadow_casts_fn casts, void *user, wz3_shadow_data *out);
WZ3_API wz3_result wz3_minmesh_shadow_data(const wz3_minmesh *mesh, const wz3_geometry *geometry, wz3_shadow_casts_fn casts, void *user, wz3_shadow_data *out);

/* One edge of a caster: its first two incidences (vertex pair and polygon), `count` the total. A native edge has both,
   the second reversed. */
typedef struct wz3_shadow_edge { uint32_t a0, b0, f0, a1, b1, f1, count; } wz3_shadow_edge;
typedef struct wz3_shadow_topology {
  uint8_t native, uses_shadow_vertex_map;
  uint32_t n_vertices, n_triangles, n_polygons, n_edges;
  float *positions;              /* 3 per vertex */
  float *volume_positions;       /* 6 per vertex: the point and its twin that is extruded */
  float *polygon_planes;         /* 4 per polygon */
  uint8_t *extrusions;           /* 2 per vertex: 0 stays, 1 is extruded */
  uint32_t *source_indices, *faces, *triangle_polygon;
  wz3_shadow_edge *edges;
  uint32_t boundary_edges, non_manifold_edges, winding_conflict_edges, max_edge_incidence;
  uint32_t source_non_manifold_edges, trailing_plane_slots;
  void *block_;
  size_t block_bytes_;
} wz3_shadow_topology;
/* prepareShadowTopology: `ranges` are the (start, count) pairs of the caster groups (the draw item's shadow groups, repeats and
   all); the recipe is used when its key equals them. positions_dynamic: the positions move every frame (planes are rebuilt from
   the current triangles). `data` may be NULL. Free with wz3_shadow_topology_free. */
WZ3_API wz3_result wz3_shadow_topology_prepare(const wz3_geometry *geometry, const wz3_shadow_data *data, const uint32_t *ranges, uint32_t n_ranges,
                                               int positions_dynamic, wz3_shadow_topology *out);
WZ3_API void wz3_shadow_topology_free(wz3_shadow_topology *t);
/* refreshAnimatedShadowPlanes / refreshShadowTopologyPositions: 0 when the face data (or a source index) is inconsistent */
WZ3_API int wz3_shadow_topology_refresh_planes(wz3_shadow_topology *t);
WZ3_API int wz3_shadow_topology_refresh_positions(wz3_shadow_topology *t, const float *positions, uint32_t n_vertices);
/* shadowGroupStructureMatches */
WZ3_API int wz3_shadow_group_structure_matches(const wz3_geometry_group *a, uint32_t na, const wz3_geometry_group *b, uint32_t nb);
enum { WZ3_SHADOW_KEEP = 0, WZ3_SHADOW_REFRESHED, WZ3_SHADOW_DROP };
/* refreshedShadowTopologies for one cached topology of a geometry that changed: `streams_identical` (indices, shadow maps, recipe and
   group structure are unchanged), `positions_changed`; refreshes the topology in place and says whether it may be kept. */
WZ3_API int wz3_shadow_topology_reuse(wz3_shadow_topology *t, int streams_identical, int positions_changed, const float *positions, uint32_t n_vertices);

/* buildShadowVolume: silhouette quads (and, with caps, one cap triangle per fan triangle on its facing side) of the volume extruded
   from `light_world` (model NULL: identity). Indices refer to volume_positions pairs: 2 * vertex + extrusion. */
typedef struct wz3_shadow_volume {
  const float *positions;        /* the topology's volume_positions */
  const uint8_t *extrusions;
  uint32_t *indices;
  uint32_t n_indices, silhouette_index_count, cap_index_count;
  uint8_t *face_front;           /* one per polygon */
  float light_position[3];       /* the light in the caster's space (TransR) */
  uint8_t wide;                  /* the JavaScript indices need 32 bits (more than 0xffff volume vertices) */
} wz3_shadow_volume;
WZ3_API wz3_result wz3_shadow_volume_build(const wz3_shadow_topology *t, const float light_world[3], const float *model, int include_caps, wz3_shadow_volume *out);
WZ3_API void wz3_shadow_volume_free(wz3_shadow_volume *v);

#ifdef __cplusplus
}
#endif

#endif /* WZ3_H_INCLUDED */

/* ===== implementation: define WZ3_IMPLEMENTATION in exactly one translation unit ===== */

#ifdef WZ3_IMPLEMENTATION

#ifdef __cplusplus
#error "wz3.h: the implementation is C99; compile it as a C translation unit"
#endif

#include "wz3/prologue_detmath.inc"
#include "wz3/core.inc"
#include "wz3/bitmap_pixel.inc"
#include "wz3/bitmap_objects.inc"
#include "wz3/lookup_textures.inc"
#include "wz3/texture_bc3.inc"
#include "wz3/texture_levels.inc"
#include "wz3/class_meta.inc"
#include "wz3/kdoc.inc"
#include "wz3/graph.inc"
#include "wz3/frame_types.inc"
#include "wz3/runtime.inc"
#include "wz3/mesh.inc"
#include "wz3/libtess.inc"
#include "wz3/font3d_glyphs.inc"
#include "wz3/minmesh.inc"
#include "wz3/frame.inc"
#include "wz3/scene.inc"
#include "wz3/effects.inc"
#include "wz3/render_plan.inc"
#include "wz3/material_view.inc"
#include "wz3/draw_list.inc"
#include "wz3/texture_plan.inc"
#include "wz3/geometry.inc"
#include "wz3/shadow.inc"
#include "wz3/runtime_tail.inc"
#include "wz3/epilogue.inc"

#endif /* WZ3_IMPLEMENTATION */
