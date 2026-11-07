/* v2.h - V2 synthesizer + sequencer - single-header C99 library (stb style)

   A sample-rate and audio-hardware agnostic, pull-based player for Farbrausch
   V2 modules (.v2m, format version 6, as used by fr-041 debris). It is a
   transcription of the public-domain "v2redux" player (itself a port of kb's
   V2 synth.asm / v2seq) and renders bit-identical float32 PCM at 44100 Hz
   (tests/oracle compares it with v2redux, see doc/REFERENCE.md).

   USAGE
     In exactly one translation unit:

         #define V2_IMPLEMENTATION
         #include <debris/core/v2.h>

     Every other file just includes <debris/core/v2.h>.

         v2_synth *s = v2_create(48000);
         if (v2_open(s, file_bytes, file_size) != V2_OK) { ... }
         v2_play(s, 0);
         ...
         v2_render(s, interleaved_stereo_floats, frame_count);   // audio callback
         ...
         v2_destroy(s);

   CONFIGURATION (define before including the implementation)
     V2_MALLOC(sz) / V2_FREE(p)  allocator override; both or neither
                                 (default: malloc / free). Only v2_create(),
                                 v2_open() and v2_destroy()/v2_close() allocate;
                                 v2_render() and all DSP code never do.
     V2_RONAN                    1 (default): include the Ronan speech synth,
                                 which processes channel 15 like the original.
                                 0: channel 15 is mixed unprocessed.
     V2_STATIC                   make every API function static.

   DESIGN NOTES
     - No global or static mutable state: every function takes a v2_synth* and
       any number of instances may coexist.
     - Nothing here creates threads. An instance is not internally
       synchronized; use it from one thread at a time. Different instances are
       fully independent, and an instance may migrate between threads.
     - Errors are integer return codes (v2_result); there are no exceptions.
     - Only V2M format version 6 is rendered (the version debris ships).
       Older files are detected and rejected with V2_ERR_UNSUPPORTED_VERSION.
     - Determinism: the output is a pure function of (file, seed, sample rate,
       call sequence). The DSP relies on strict IEEE-754 binary32 arithmetic:
       compile without -ffast-math and without FMA contraction
       (-ffp-contract=off; the header requests this for GCC/Clang/MSVC), and
       with subnormals enabled (v2_open() verifies this at run time).
       Output is chunk-size invariant: any partition of N frames renders the
       same bits. Bit-exactness against the reference holds at 44100 Hz; other
       rates run the same (rate-aware) engine but are not reference-checked.
       Supported rates are 44100..96000 Hz: the V2 filters are specified at a
       44.1 kHz base and go unstable below it.

   LICENSE: derived from v2redux (CC0 1.0) and kb's V2 code, which he placed in
   the public domain; the original notices are preserved in
   third_party/licenses/v2redux-LICENSE.txt. This file is offered under the same terms.
*/

#ifndef V2_H_INCLUDED
#define V2_H_INCLUDED

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef V2_API
#ifdef V2_STATIC
#define V2_API static
#else
#define V2_API extern
#endif
#endif

#define V2_VERSION_MAJOR 1
#define V2_VERSION_MINOR 0
#define V2_VERSION_PATCH 0

#define V2_MIN_SAMPLE_RATE 44100u /* the filters are scaled from a 44.1 kHz base and blow up below it */
#define V2_MAX_SAMPLE_RATE 96000u /* the control frame buffers are sized for 96 kHz */
#define V2_NUM_CHANNELS    16

typedef struct v2_synth v2_synth; /* opaque: synth + sequencer + loaded song */

typedef enum v2_result {
  V2_OK = 0,
  V2_ERR_BAD_FILE,            /* structurally invalid / not a v2m */
  V2_ERR_UNSUPPORTED_VERSION, /* valid v2m, but not format version 6 */
  V2_ERR_FP_ENV,              /* FP environment flushes subnormals (-ffast-math) */
  V2_ERR_OUT_OF_MEMORY,
  V2_ERR_BAD_ARGUMENT
} v2_result;

/* Static description of a result code. Never NULL. */
V2_API const char *v2_result_string(v2_result r);

/* One-time FP environment self-check (also run by v2_open()). Returns 1 if
   subnormal arithmetic is not flushed to zero. */
V2_API int v2_fp_env_ok(void);

/* ---- instance lifetime ------------------------------------------------ */

/* Allocates (V2_MALLOC) and initializes an instance rendering at
   `sample_rate` Hz, V2_MIN_SAMPLE_RATE..V2_MAX_SAMPLE_RATE (44.1..96 kHz).
   NULL on failure. */
V2_API v2_synth *v2_create(uint32_t sample_rate);
V2_API void      v2_destroy(v2_synth *s); /* NULL is a no-op */

/* Caller-provided storage variant: v2_synth_size() bytes, 16-byte aligned.
   Release with v2_close() only (do not v2_destroy() it). */
V2_API size_t    v2_synth_size(void);
V2_API v2_synth *v2_synth_init(void *mem, size_t mem_size, uint32_t sample_rate);

V2_API uint32_t  v2_sample_rate(const v2_synth *s);

/* ---- song ------------------------------------------------------------- */

/* Parses and copies the song (V2_MALLOC); the caller's buffer may be freed
   afterwards. Replaces any previously opened song. On success the engine is
   reset and ready; call v2_play() to start the sequencer. */
V2_API v2_result v2_open(v2_synth *s, const void *data, size_t size);
V2_API void      v2_close(v2_synth *s); /* releases the song; instance stays usable */

/* Format version (0..6) detected by the last v2_open(), also after
   V2_ERR_UNSUPPORTED_VERSION; -1 if none. */
V2_API int v2_file_version(const v2_synth *s);

/* ---- playback --------------------------------------------------------- */

/* Starts playback at from_ms into the song. Implies a full synth reset. */
V2_API void v2_play(v2_synth *s, uint32_t from_ms);

/* Stops playback; with fade_ms the output is faded out first. Stopping
   silences immediately (no tail). */
V2_API void v2_stop(v2_synth *s, uint32_t fade_ms);

/* 0 once the song end was reached (the synth tail keeps ringing and is still
   produced by v2_render()) or after v2_stop(). */
V2_API int v2_is_playing(const v2_synth *s);

/* Pull-render `frames` interleaved stereo float32 frames (0 dB = 1.0).
   Renders silence when nothing is open or playing. Never allocates. */
V2_API void v2_render(v2_synth *s, float *out, uint32_t frames);

/* Song length in frames at the instance's sample rate, derived from the
   sequence timing without rendering audio. Implies a reset: call v2_play()
   before the next v2_render(). 0 if no song is open. */
V2_API uint64_t v2_length_frames(v2_synth *s);

/* Replaces the historical rdtsc noise / S&H seeding. Default 0 is the
   deterministic reference. Takes effect at the next v2_play(). */
V2_API void v2_set_seed(v2_synth *s, uint64_t seed);

/* ---- interactive control and display monitors (outside the determinism
        contract; the default all-audible mask renders the reference) ------ */

/* Bit ch set = channel audible (default 0xffff). A muted channel keeps
   rendering and advancing state but feeds no output bus. Persists across
   v2_open()/v2_play(). */
V2_API void v2_set_channel_mask(v2_synth *s, uint32_t mask);

/* Active voices per channel into out[0..15]; returns the total. */
V2_API int  v2_get_channel_poly(const v2_synth *s, int out[16]);

/* Monotonic per-channel note-on counters (counts muted channels too). */
V2_API void v2_get_channel_note_ons(const v2_synth *s, uint32_t out[16]);

/* Per-channel output peak since the previous call; read-and-clear. */
V2_API void v2_get_channel_levels(v2_synth *s, float out[16]);

/* ---- direct engine access (bypasses the sequencer) --------------------- */

/* Feeds raw MIDI to the engine of the opened song (its patches and global
   settings): note on/off, controller, program change. The stream ends at
   `len` bytes or at the first 0xFD byte. */
V2_API void v2_midi(v2_synth *s, const uint8_t *msg, size_t len);

/* Renders the engine directly, regardless of the sequencer state. */
V2_API void v2_render_direct(v2_synth *s, float *out, uint32_t frames);

#ifdef __cplusplus
}
#endif

#endif /* V2_H_INCLUDED */

/* ========================================================================== */
/*  IMPLEMENTATION                                                            */
/* ========================================================================== */

#ifdef V2_IMPLEMENTATION

#ifdef __cplusplus
#error "v2.h: the implementation is C99; compile it as a C translation unit"
#endif

#include <float.h>
#include <math.h>
#include <string.h>

#if (defined(V2_MALLOC) && !defined(V2_FREE)) || (!defined(V2_MALLOC) && defined(V2_FREE))
#error "v2.h: define both V2_MALLOC and V2_FREE, or neither"
#endif
#ifndef V2_MALLOC
#include <stdlib.h>
#define V2_MALLOC(sz) malloc(sz)
#define V2_FREE(p)    free(p)
#endif

#ifndef V2_RONAN
#define V2_RONAN 1
#endif

#if defined(FLT_EVAL_METHOD) && FLT_EVAL_METHOD != 0
#error "v2.h needs FLT_EVAL_METHOD == 0 (plain binary32 arithmetic, no x87 excess precision)"
#endif
#ifdef __FAST_MATH__
#error "v2.h must not be compiled with -ffast-math"
#endif

/* Contracting a*b+c into an FMA changes the rounding the engine depends on. */
#if defined(__clang__)
#pragma clang fp contract(off)
#elif defined(_MSC_VER)
#pragma fp_contract(off)
#elif defined(__GNUC__)
#pragma GCC push_options
#pragma GCC optimize("fp-contract=off")
#endif

/* -------------------------------------------------------------------------- */
/*  Owned transcendentals                                                     */
/*                                                                            */
/*  libm differs per OS, so no approximating libm call is made in the audio   */
/*  path. Each kernel mirrors one x87 asm kernel of the original synth: a     */
/*  double-precision core with explicit binary32 roundings where the asm      */
/*  rounds at PC=24. Only exact libm functions are used (ldexp, frexp, trunc, */
/*  fmod, sqrt, fabs, lrint, rint).                                           */
/* -------------------------------------------------------------------------- */

/* 2^(k/32), k=0..31, double-double */
static const double v2__exp2tab[32][2] = {
  { 1.0, 0.0 },
  { 1.0218971486541166, 5.109225028973444e-17 },
  { 1.0442737824274138, 8.551889705537965e-17 },
  { 1.0671404006768237, -7.899853966841582e-17 },
  { 1.0905077326652577, -3.046782079812471e-17 },
  { 1.1143867425958924, 1.0410278456845571e-16 },
  { 1.1387886347566916, 8.912812676025408e-17 },
  { 1.1637248587775775, 3.8292048369240935e-17 },
  { 1.189207115002721, 3.982015231465646e-17 },
  { 1.215247359980469, -7.712630692681488e-17 },
  { 1.241857812073484, 4.658027591836937e-17 },
  { 1.2690509571917332, 2.667932131342186e-18 },
  { 1.2968395546510096, 2.5382502794888315e-17 },
  { 1.3252366431597413, -2.8587312100388614e-17 },
  { 1.3542555469368927, 7.70094837980299e-17 },
  { 1.383909881963832, -6.770511658794786e-17 },
  { 1.4142135623730951, -9.667293313452913e-17 },
  { 1.4451808069770467, -3.0237581349939873e-17 },
  { 1.4768261459394993, -3.483994556892796e-17 },
  { 1.5091644275934228, -1.016455327754295e-16 },
  { 1.5422108254079407, 7.949834809697621e-17 },
  { 1.5759808451078865, -1.0136916471278304e-17 },
  { 1.6104903319492543, 2.4707192569797888e-17 },
  { 1.645755478153965, -1.0125679913674773e-16 },
  { 1.681792830507429, 8.199010020581497e-17 },
  { 1.718619298122478, -1.851380418263111e-17 },
  { 1.7562521603732995, 2.960140695448873e-17 },
  { 1.7947090750031072, 1.8227458427912087e-17 },
  { 1.8340080864093424, 3.283107224245627e-17 },
  { 1.8741676341103, -6.122763413004143e-17 },
  { 1.9152065613971474, -1.0619946056195963e-16 },
  { 1.9571441241754002, 8.960767791036668e-17 },
};

/* 2^t - 1 = sum c[n] t^n, |t| <= 1/64 */
static const double v2__exp2m1c[7] = {
  0.6931471805599453,
  0.24022650695910072,
  0.05550410866482158,
  0.009618129107628477,
  0.0013333558146428443,
  0.0001540353039338161,
  1.5252733804059841e-05,
};

/* log2(1 + j/32), j=0..32, double-double */
static const double v2__log2tab[33][2] = {
  { 0.0, 0.0 },
  { 0.044394119358453436, 1.3338680039226223e-18 },
  { 0.0874628412503394, 6.765321226991275e-18 },
  { 0.12928301694496647, -1.147571414337692e-17 },
  { 0.16992500144231237, -1.0448980122780218e-17 },
  { 0.20945336562894978, -1.747801539116594e-18 },
  { 0.2479275134435855, 3.8662183541602335e-18 },
  { 0.28540221886224837, -2.726283638197372e-17 },
  { 0.32192809488736235, -3.717019964142682e-19 },
  { 0.3575520046180837, 1.8984820907705057e-17 },
  { 0.3923174227787603, -1.6328502208352762e-17 },
  { 0.42626475470209796, -1.9932012137193316e-17 },
  { 0.45943161863729726, -3.8053583859449705e-19 },
  { 0.4918530963296747, -1.0820682119194486e-17 },
  { 0.5235619560570128, 3.838472289082233e-17 },
  { 0.5545888516776374, -1.2269989151629687e-17 },
  { 0.5849625007211562, -5.224490061390109e-18 },
  { 0.6147098441152082, -2.2208024293925304e-17 },
  { 0.6438561897747247, -7.434039928285364e-19 },
  { 0.6724253419714956, -2.6214744450027748e-17 },
  { 0.7004397181410922, -2.2038346320583612e-17 },
  { 0.7279204545631992, -2.476475356878588e-17 },
  { 0.7548875021634686, -1.5673470184170328e-17 },
  { 0.7813597135246596, -7.522378350087652e-19 },
  { 0.8073549220576041, 4.4407139084295174e-17 },
  { 0.8328900141647416, 5.415287952402795e-17 },
  { 0.8579809951275721, 3.2653869625311436e-17 },
  { 0.8826430493618412, 2.2296523086165164e-17 },
  { 0.9068905956085185, 4.991495917345345e-17 },
  { 0.9307373375628862, 4.094087911381388e-17 },
  { 0.9541963103868752, -3.7239566747188146e-17 },
  { 0.9772799234999164, 3.395815896151496e-17 },
  { 1.0, 0.0 },
};

static const double v2__two_over_ln2 = 2.8853900817779268;

/* Taylor coefficients for |t| <= pi/4 */
static const double v2__sinc[8] = {
  -0.16666666666666666,
  0.008333333333333333,
  -0.0001984126984126984,
  2.7557319223985893e-06,
  -2.505210838544172e-08,
  1.6059043836821613e-10,
  -7.647163731819816e-13,
  2.8114572543455206e-15,
};
static const double v2__cosc[8] = {
  -0.5,
  0.041666666666666664,
  -0.001388888888888889,
  2.48015873015873e-05,
  -2.755731922398589e-07,
  2.08767569878681e-09,
  -1.1470745597729725e-11,
  4.779477332387385e-14,
};

/* atan(j/8), j=0..8, double-double */
static const double v2__atantab[9][2] = {
  { 0.0, 0.0 },
  { 0.12435499454676144, -3.1253241424539383e-18 },
  { 0.24497866312686414, 1.0698755618734451e-17 },
  { 0.35877067027057225, -2.4623815582638635e-17 },
  { 0.4636476090008061, 2.2698777452961687e-17 },
  { 0.5585993153435624, -5.4556305485916264e-18 },
  { 0.6435011087932844, 1.5834785051444286e-17 },
  { 0.7188299996216245, -2.1478388444456983e-17 },
  { 0.7853981633974483, 3.061616997868383e-17 },
};

static const double v2__pio2_hi = 1.5707963267948966, v2__pio2_lo = 6.123233995736766e-17;
/* Cody-Waite 3-part pi/2 for sin range reduction */
static const double v2__pio2a = 1.5707963267341256;
static const double v2__pio2b = 6.077100506303966e-11;
static const double v2__pio2c = 2.0222662487959506e-21;
static const double v2__two_over_pi = 0.6366197723675814;

/* The asm's `dd 0.083333333333`: a 24-bit float that is NOT 1/12. */
static const float v2__fci12 = 0.083333333333f;

/* 2^r for r in (-1, 1) */
static double v2__exp2_core(double r)
{
  int k = (int)lrint(r * 32.0);
  double t = r - (double)k * 0.03125;
  double q = t * (v2__exp2m1c[0] + t * (v2__exp2m1c[1] + t * (v2__exp2m1c[2] +
             t * (v2__exp2m1c[3] + t * (v2__exp2m1c[4] + t * (v2__exp2m1c[5] +
             t * v2__exp2m1c[6]))))));
  int kk = k + 32;
  const double *T = v2__exp2tab[kk & 31];
  double m = T[0] + (T[0] * q + (T[1] + T[1] * q));
  return ldexp(m, (kk >> 5) - 1);
}

/* log2(x) for finite float x > 0 */
static double v2__log2_core(float xf)
{
  int ex;
  double m = frexp((double)xf, &ex);
  m *= 2.0;
  ex -= 1;
  int j = (int)lrint((m - 1.0) * 32.0);
  if (j < 0) j = 0; /* keeps the table lookup in range for non-finite input */
  if (j > 32) j = 32;
  double Tj = 1.0 + (double)j * 0.03125;
  double s = (m - Tj) / (m + Tj);
  double s2 = s * s;
  double p = v2__two_over_ln2 * s * (1.0 + s2 * (1.0 / 3.0 + s2 * (1.0 / 5.0 +
             s2 * (1.0 / 7.0))));
  return (double)ex + (v2__log2tab[j][0] + (p + v2__log2tab[j][1]));
}

static double v2__sin_poly(double t)
{
  double w = t * t;
  return t + t * w * (v2__sinc[0] + w * (v2__sinc[1] + w * (v2__sinc[2] + w * (v2__sinc[3] +
         w * (v2__sinc[4] + w * (v2__sinc[5] + w * (v2__sinc[6] + w * v2__sinc[7])))))));
}

static double v2__cos_poly(double t)
{
  double w = t * t;
  return 1.0 + w * (v2__cosc[0] + w * (v2__cosc[1] + w * (v2__cosc[2] + w * (v2__cosc[3] +
         w * (v2__cosc[4] + w * (v2__cosc[5] + w * (v2__cosc[6] + w * v2__cosc[7])))))));
}

static double v2__sin_core(double x)
{
  int n = (int)lrint(x * v2__two_over_pi);
  double t = x - (double)n * v2__pio2a;
  t -= (double)n * v2__pio2b;
  t -= (double)n * v2__pio2c;
  switch (n & 3) {
  default:
  case 0: return v2__sin_poly(t);
  case 1: return v2__cos_poly(t);
  case 2: return -v2__sin_poly(t);
  case 3: return -v2__cos_poly(t);
  }
}

static double v2__cos_core(double x)
{
  int n = (int)lrint(x * v2__two_over_pi);
  double t = x - (double)n * v2__pio2a;
  t -= (double)n * v2__pio2b;
  t -= (double)n * v2__pio2c;
  switch ((n + 1) & 3) {
  default:
  case 0: return v2__sin_poly(t);
  case 1: return v2__cos_poly(t);
  case 2: return -v2__sin_poly(t);
  case 3: return -v2__cos_poly(t);
  }
}

static double v2__atan01(double a)
{
  int j = (int)lrint(a * 8.0);
  if (j < 0) j = 0;
  if (j > 8) j = 8;
  double c = (double)j * 0.125;
  double t = (a - c) / (1.0 + a * c);
  double w = t * t;
  double p = t + t * w * (-1.0 / 3.0 + w * (1.0 / 5.0 + w * (-1.0 / 7.0 +
             w * (1.0 / 9.0 + w * (-1.0 / 11.0)))));
  return v2__atantab[j][0] + (p + v2__atantab[j][1]);
}

static double v2__atan_core(double x)
{
  double a = fabs(x);
  double r;
  if (a > 1.0)
    r = v2__pio2_hi - (v2__atan01(1.0 / a) - v2__pio2_lo);
  else
    r = v2__atan01(a);
  return x < 0.0 ? -r : r;
}

/* asm pow2: 2^y with a 24-bit-rounded mantissa and exact scaling */
static float v2__pow2f(float y)
{
  float yi = truncf(y);
  double r = (double)y - (double)yi;
  float m24 = (float)v2__exp2_core(r);
  return ldexpf(m24, (int)yi);
}

/* asm pow: base^e via fyl2x (64-bit) feeding the pow2 tail */
static float v2__powf24(float base, float e)
{
  double t = (double)e * v2__log2_core(base);
  double ti = trunc(t);
  float m24 = (float)v2__exp2_core(t - ti);
  return ldexpf(m24, (int)ti);
}

/* x87 fistp: round to nearest even; out-of-range/NaN yields 0x80000000. */
static int32_t v2__fistp(float x)
{
  float r = rintf(x);
  if (!(r >= -2147483648.0f && r < 2147483648.0f))
    return (int32_t)0x80000000u;
  return (int32_t)r;
}

/* syOscChgPitch freq tail: round(2^((pitch+note-60) * fci12) * basefreq) */
static int32_t v2__oscfreqi(float pno, float base)
{
  float t = pno * v2__fci12;
  float m = v2__pow2f(t);
  return v2__fistp(m * base);
}

/* asm fld1;fpatan and fsin/fcos stand-ins */
static float v2__sinf24(float x) { return (float)v2__sin_core((double)x); }
static float v2__cosf24(float x) { return (float)v2__cos_core((double)x); }

/* dist OVERDRIVE setup tail: gain2 = (param1/128) / atan(gain1) */
static float v2__od_gain2(float p1g, float gain1)
{
  return (float)((double)p1g / v2__atan_core((double)gain1));
}

/* -------------------------------------------------------------------------- */
/*  Engine constants and helpers                                              */
/* -------------------------------------------------------------------------- */

#define V2__MAX_FRAME 280 /* samples per control frame, upper bound (96 kHz) */
#define V2__POLY      64
#define V2__CHANS     16

static const float v2__fclowest   = 1.220703125e-4f; /* 2^-13: EGs clamp to 0 below this */
static const float v2__fcpi_2     = 1.5707963267948966192313216916398f;
static const float v2__fcpi       = 3.1415926535897932384626433832795f;
static const float v2__fc1p5pi    = 4.7123889803846898576939650749193f;
static const float v2__fc2pi      = 6.28318530717958647692528676655901f;
static const float v2__fc32bit    = 2147483648.0f; /* 2^31 */
static const float v2__fcoscbase  = 261.6255653f;
static const float v2__fcsrbase   = 44100.0f;
static const float v2__fcboostfreq = 150.0f;
static const float v2__fcframebase = 128.0f;
static const float v2__fcdcflt    = 126.0f;
static const float v2__fccfframe  = 11.0f;
static const float v2__fcfmmax    = 2.0f;
static const float v2__fcattackmul = -0.09375f;
static const float v2__fcattackadd = 7.0f;
static const float v2__fcsusmul   = 0.0019375f;
static const float v2__fcgain     = 0.6f;
static const float v2__fcgainh    = 0.6f;
static const float v2__fcmdlfomul = 1973915.49f;
static const float v2__fcrms8192  = 0.0110485434560398050687631931578883f; /* 1/sqrt(8192) */
static const float v2__fcmoogsixth = 1.0f / 6.0f;
static const float v2__fccpdfalloff = 0.9998f;
static const float v2__fcdcoffset = 3.814697265625e-6f; /* 2^-18, denormal bias */

static inline float v2__bits2float(uint32_t u)
{
  float f;
  memcpy(&f, &u, sizeof f);
  return f;
}

static inline float v2__sqr(float x) { return x * x; }
static inline float v2__maxf(float a, float b) { return a > b ? a : b; }
static inline float v2__clampf(float x, float lo, float hi) { return (x < lo) ? lo : (x > hi) ? hi : x; }
static inline int32_t v2__mini(int32_t a, int32_t b) { return a < b ? a : b; }

/* Fast arctangent: two rational approximations. */
static float v2__fastatan(float x)
{
  float sign = 1.0f;
  if (x < 0.0f) {
    sign = -1.0f;
    x = -x;
  }

  /* Coefficients are doubles in the asm; every operation rounds to binary32.
     The |x|>=1 table is only selected for x in [1,2) (asm cmovge quirk). */
  static const double coeffs[2][6] = {
    /*  cx1          cx3        cxm0        cxm2        cxm4   bias */
    {          1.0, 0.43157974,        1.0, 0.76443945, 0.05831938,   0.0 },
    { -0.431597974,       -1.0, 0.05831938, 0.76443945,        1.0,
      1.5707963267948966192313216916398 },
  };
  const double *c = coeffs[(x >= 1.0f && x < 2.0f) ? 1 : 0];
  float x2 = x * x;
  float A = (float)((double)x2 * c[1]);
  float B = (float)((double)x2 * c[4]);
  A = (float)((double)A + c[0]);
  B = (float)((double)B + c[3]);
  float num = A * x;
  float den = B * x2;
  den = (float)((double)den + c[2]);
  float r = num / den;
  r = (float)((double)r + c[5]);
  return r * sign;
}

/* Fast sine for x in [-pi/2, pi/2]: odd minimax polynomial. */
static float v2__fastsin(float x)
{
  float x2 = x * x;
  float t = (float)(-0.00018542 * (double)x2);
  t = (float)((double)t + 0.0083143);
  t = t * x2;
  t = (float)((double)t - 0.16666);
  t = t * x2;
  t = 1.0f + t;
  return t * x;
}

/* Fast sine with the original's (deliberately kept) range reduction, which
   is only valid for x >= 0 yet is also fed negative FM phases. */
static float v2__fastsinrc(float x)
{
  x = fmodf(x, v2__fc2pi);
  if (x > v2__fc1p5pi)
    x -= v2__fc2pi;
  else if (x > v2__fcpi_2)
    x = (float)(3.14159265358979323846264 - (double)x); /* x87 fldpi, not float pi */
  return v2__fastsin(x);
}

static float v2__calcfreq(float x)  { return v2__pow2f((x - 1.0f) * 10.0f); }
static float v2__calcfreq2(float x) { return v2__pow2f((x - 1.0f) * v2__fccfframe); }

static inline uint32_t v2__urandom(uint32_t *seed)
{
  *seed = *seed * 196314165u + 907633515u;
  return *seed;
}

/* uniform random float in [-1,1) */
static inline float v2__frandom(uint32_t *seed)
{
  uint32_t bits = v2__urandom(seed);
  float f = v2__bits2float((bits >> 9) | 0x40000000u);
  return f - 3.0f;
}

/* 32-bit value into [0,1) with 23 bits of precision */
static inline float v2__utof23(uint32_t x)
{
  float f = v2__bits2float((x >> 9) | 0x3f800000u);
  return f - 1.0f;
}

/* float [0,1) into 0.32 fixed point; V2 does `fistp; shl 1` */
static inline uint32_t v2__ftou32(float v)
{
  return 2u * (uint32_t)v2__fistp(v * v2__fc32bit);
}

/* -------------------------------------------------------------------------- */
/*  Building blocks                                                           */
/* -------------------------------------------------------------------------- */

typedef struct v2__st { float ch[2]; } v2__st; /* stereo sample */

/* "LRC" state-variable filter; `freq` is 2*(1-cos(2pi f/sr)) */
typedef struct v2__lrc { float l, b; } v2__lrc;

static inline float v2__lrc_step(v2__lrc *f, float in, float freq, float reso)
{
  f->l += freq * f->b;
  float h = in - f->b * reso - f->l;
  f->b += freq * h;
  return h;
}

/* 2x-oversampled step with the denormal-avoidance bias; returns high-pass. */
static inline float v2__lrc_step_2x(float *l, float *b, float in, float freq, float reso, float dco)
{
  in += dco;
  *l += freq * *b - dco;
  *b += freq * (in - *b * reso - *l);
  *l += freq * *b;
  float h = in - *b * reso - *l;
  *b += freq * h;
  return h;
}

typedef struct v2__moog { float b[5]; } v2__moog;

static float v2__moog_step(v2__moog *m, float realin, float f, float p, float q)
{
  float in = realin + v2__fcdcoffset;
  float t1, t2, t3, b4;

  in -= q * m->b[4];
  t1 = m->b[1]; m->b[1] = (in + m->b[0]) * p + m->b[1] * f;
  t2 = m->b[2]; m->b[2] = (t1 + m->b[1]) * p + m->b[2] * f;
  t3 = m->b[3]; m->b[3] = (t2 + m->b[2]) * p + m->b[3] * f;
                b4      = (t3 + m->b[3]) * p + m->b[4] * f;

  b4 -= b4 * b4 * b4 * v2__fcmoogsixth;
  b4 -= v2__fcdcoffset;
  m->b[4] = b4;
  m->b[0] = realin;
  return b4;
}

/* DC filter state: a one-pole high-pass with a very low cut-off */
typedef struct v2__dcf { float xm1, ym1; } v2__dcf;

static inline float v2__dcf_step(v2__dcf *d, float in, float R)
{
  float y = (R * d->ym1 - d->xm1 + in + v2__fcdcoffset) - v2__fcdcoffset;
  d->xm1 = in;
  d->ym1 = y;
  return y;
}

/* Constant-length delay line over caller-owned storage */
typedef struct v2__delay {
  uint32_t pos, len;
  float *buf;
} v2__delay;

static void v2__delay_reset(v2__delay *d)
{
  memset(d->buf, 0, sizeof(float) * d->len);
  d->pos = 0;
}

static void v2__delay_init(v2__delay *d, float *buf, uint32_t len)
{
  d->buf = buf;
  d->len = len;
  v2__delay_reset(d);
}

static inline float v2__delay_fetch(const v2__delay *d) { return d->buf[d->pos]; }

static inline void v2__delay_feed(v2__delay *d, float v)
{
  d->buf[d->pos] = v;
  if (++d->pos == d->len)
    d->pos = 0;
}

/* Portable replacement for the original's libc rand(): glibc TYPE_3 additive
   generator at its default seed (seed 1 -> 1804289383, 846930886, ...). */
typedef struct v2__rand {
  uint32_t ring[31];
  int32_t head;
} v2__rand;

static void v2__rand_init(v2__rand *rn, uint32_t seed)
{
  uint32_t r[344];
  int32_t i, j;
  if (seed == 0) seed = 1;
  r[0] = seed;
  for (i = 1; i < 31; i++) {
    int64_t v = (int64_t)16807 * (int32_t)r[i - 1] % 2147483647;
    if (v < 0) v += 2147483647;
    r[i] = (uint32_t)v;
  }
  for (i = 31; i < 34; i++)  r[i] = r[i - 31];
  for (i = 34; i < 344; i++) r[i] = r[i - 31] + r[i - 3];
  for (j = 0; j < 31; j++) rn->ring[j] = r[313 + j];
  rn->head = 0;
}

static uint32_t v2__rand_next(v2__rand *rn)
{
  uint32_t v = rn->ring[rn->head] + rn->ring[(rn->head + 28) % 31];
  rn->ring[rn->head] = v;
  rn->head = (rn->head + 1) % 31;
  return v >> 1;
}

/* Player seed knob: 0 keeps the reference value, otherwise splitmix64 mixing. */
static inline uint32_t v2__seed_mix(uint64_t user_seed, uint32_t base, int32_t idx)
{
  if (!user_seed) return base;
  uint64_t z = user_seed + (uint64_t)(idx + 1) * 0x9e3779b97f4a7c15ull;
  z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
  z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
  return base ^ (uint32_t)(z >> 32);
}

/* -------------------------------------------------------------------------- */
/*  Per-synth instance data (sample-rate constants + scratch buffers)         */
/* -------------------------------------------------------------------------- */

typedef struct v2__inst {
  /* sample-rate dependent */
  float sr_samplesperms;
  float sr_obasefrq;
  float sr_linfreq;
  float sr_boostcos, sr_boostsin;
  float sr_dcfilter;
  int32_t sr_framesize;
  float sr_iframe;

  v2__rand rng;
  uint64_t user_seed;

  /* bit ch set = muted: renders fully but feeds no output bus */
  uint32_t chan_mute;
  /* per-channel peak meter, written during render, read-and-cleared by host */
  float chan_peak[V2__CHANS];

  float vcebuf[V2__MAX_FRAME];
  float vcebuf2[V2__MAX_FRAME];
  v2__st levelbuf[V2__MAX_FRAME];
  v2__st chanbuf[V2__MAX_FRAME];
  float aux1buf[V2__MAX_FRAME];
  float aux2buf[V2__MAX_FRAME];
  v2__st mixbuf[V2__MAX_FRAME];
  v2__st auxabuf[V2__MAX_FRAME];
  v2__st auxbbuf[V2__MAX_FRAME];
} v2__inst;

static void v2__inst_calc_sample_rate(v2__inst *in, int32_t samplerate)
{
  float sr = (float)samplerate;

  /* The asm computes 1/sr once and multiplies; a/sr rounds differently. */
  float recip = 1.0f / sr;

  in->sr_samplesperms = sr / 1000.0f;
  in->sr_obasefrq = (v2__fcoscbase * v2__fc32bit) * recip;
  in->sr_linfreq = v2__fcsrbase * recip;
  in->sr_dcfilter = 1.0f - v2__fcdcflt * recip;

  in->sr_framesize = (int32_t)(v2__fcframebase * sr / v2__fcsrbase + 0.5f);
  in->sr_iframe = 1.0f / (float)in->sr_framesize;

  float boost = recip * v2__fc2pi * v2__fcboostfreq;
  in->sr_boostcos = v2__cosf24(boost);
  in->sr_boostsin = v2__sinf24(boost);
}

/* -------------------------------------------------------------------------- */
/*  Patch parameter blocks                                                    */
/*                                                                            */
/*  A patch stores one byte per parameter; the engine widens them to floats   */
/*  and indexes them both by name and by position (the mod matrix targets     */
/*  positions), hence the struct/array unions.                                */
/* -------------------------------------------------------------------------- */

#define V2__NOSC 3
#define V2__NFLT 2
#define V2__NENV 2
#define V2__NLFO 2

typedef struct v2__p_osc  { float mode, ring, pitch, detune, color, gain; } v2__p_osc;
typedef struct v2__p_env  { float ar, dr, sl, sr, rr, vol; } v2__p_env;
typedef struct v2__p_flt  { float mode, cutoff, reso; } v2__p_flt;
typedef struct v2__p_lfo  { float mode, sync, egmode, rate, phase, pol, amp; } v2__p_lfo;
typedef struct v2__p_dist { float mode, ingain, param1, param2; } v2__p_dist;
typedef struct v2__p_boost { float amount; } v2__p_boost;
typedef struct v2__p_moddel { float amount, fb, llength, rlength, mrate, mdepth, mphase; } v2__p_moddel;
typedef struct v2__p_comp {
  float mode, stereo, autogain, lookahead, threshold, ratio, attack, release, outgain;
} v2__p_comp;
typedef struct v2__p_reverb { float revtime, highcut, lowcut, vol; } v2__p_reverb;

typedef struct v2__p_voice_s {
  float panning, transp;
  v2__p_osc osc[V2__NOSC];
  v2__p_flt flt[V2__NFLT];
  float routing; /* 0 single, 1 serial, 2 parallel */
  float fltbal;
  v2__p_dist dist;
  v2__p_env env[V2__NENV];
  v2__p_lfo lfo[V2__NLFO];
  float oscsync; /* 0 none, 1 osc, 2 full */
} v2__p_voice_s;

typedef struct v2__p_chan_s {
  float chanvol;
  float auxarcv, auxbrcv, auxasnd, auxbsnd;
  float aux1, aux2;
  float fxroute;
  v2__p_boost boost;
  v2__p_dist dist;
  v2__p_moddel chorus;
  v2__p_comp comp;
} v2__p_chan_s;

typedef struct v2__p_globals_s {
  v2__p_reverb rvb;
  v2__p_moddel del;
  float vlowcut, vhighcut;
  v2__p_comp cpr;
  float guicolor;
} v2__p_globals_s;

#define V2__NVOICEPARM 59
#define V2__NCHANPARM  29
#define V2__NGLOBPARM  23

typedef union v2__p_voice   { v2__p_voice_s   s; float f[V2__NVOICEPARM]; } v2__p_voice;
typedef union v2__p_chan    { v2__p_chan_s    s; float f[V2__NCHANPARM]; }  v2__p_chan;
typedef union v2__p_globals { v2__p_globals_s s; float f[V2__NGLOBPARM]; }  v2__p_globals;

/* a patch is voice[59] + chan[29] + maxpoly + modnum + 3*modnum bytes */
typedef char v2__chk_voice_size[(sizeof(v2__p_voice) == V2__NVOICEPARM * sizeof(float)) ? 1 : -1];
typedef char v2__chk_chan_size[(sizeof(v2__p_chan) == V2__NCHANPARM * sizeof(float)) ? 1 : -1];
typedef char v2__chk_glob_size[(sizeof(v2__p_globals) == V2__NGLOBPARM * sizeof(float)) ? 1 : -1];
typedef char v2__chk_stereo_size[(sizeof(v2__st) == 2 * sizeof(float)) ? 1 : -1];

/* -------------------------------------------------------------------------- */
/*  Oscillator                                                                */
/* -------------------------------------------------------------------------- */

enum { /* oscillator modes */
  V2__OSC_OFF = 0, V2__OSC_TRI_SAW, V2__OSC_PULSE, V2__OSC_SIN,
  V2__OSC_NOISE, V2__OSC_FM_SIN, V2__OSC_AUXA, V2__OSC_AUXB
};

typedef struct v2__osc {
  int32_t mode;
  int ring;         /* ring modulation on/off */
  uint32_t cnt;     /* wave counter */
  int32_t freq;     /* wave counter increment */
  uint32_t brpt;    /* break point for tri/pulse wave */
  float nffrq, nfres; /* noise filter freq/resonance */
  uint32_t nseed;   /* noise random seed */
  float gain;
  v2__lrc nf;       /* noise filter */
  float note;
  float pitch;
  v2__inst *inst;
} v2__osc;

static void v2__osc_init(v2__osc *o, v2__inst *inst, int32_t idx)
{
  static const uint32_t seeds[3] = { 0xdeadbeefu, 0xbaadf00du, 0xd3adc0deu };

  o->cnt = 0;
  o->nf.l = o->nf.b = 0.0f;
  o->nseed = v2__seed_mix(inst->user_seed, seeds[idx], idx);
  o->inst = inst;
}

static void v2__osc_chgpitch(v2__osc *o)
{
  v2__inst *inst = o->inst;
  o->nffrq = inst->sr_linfreq * v2__calcfreq((o->pitch + 64.0f) * 0.0078125f);
  o->freq = v2__oscfreqi(o->pitch + o->note - 60.0f, inst->sr_obasefrq);
}

static void v2__osc_set(v2__osc *o, const v2__p_osc *para)
{
  o->mode = (int32_t)para->mode;
  o->ring = (((int32_t)para->ring) & 1) != 0;

  o->pitch = (para->pitch - 64.0f) + (para->detune - 64.0f) / 128.0f;
  v2__osc_chgpitch(o);
  o->gain = para->gain / 128.0f;

  float col = para->color / 128.0f;
  o->brpt = v2__ftou32(col);
  o->nfres = 1.0f - sqrtf(col);
}

static inline void v2__osc_output(const v2__osc *o, float *dest, float x)
{
  if (o->ring)
    *dest *= x;
  else
    *dest += x;
}

/* Oscillator state machine for the analytic (box-filtered) tri/saw/pulse:
   transition code = carry:old_up:new_up. */
enum {
  V2__OSMTC_DOWN = 0,
  V2__OSMTC_UP_DOWN = 2,
  V2__OSMTC_UP = 3,
  V2__OSMTC_DOWN_UP_DOWN = 4,
  V2__OSMTC_DOWN_UP = 5,
  V2__OSMTC_UP_DOWN_UP = 7
};

static inline uint32_t v2__osm_init(const v2__osc *o)
{
  return (o->cnt - (uint32_t)o->freq) < o->brpt ? 3u : 0u;
}

static inline uint32_t v2__osm_tick(v2__osc *o, uint32_t *state)
{
  *state = ((*state << 1) | (o->cnt < o->brpt ? 1u : 0u)) & 3u;
  uint32_t code = *state | (o->cnt < (uint32_t)o->freq ? 4u : 0u);
  o->cnt += (uint32_t)o->freq;
  return code;
}

/* V2 renders a generalized triangle wave (break point brpt) as the average of
   the continuous waveform over each sample interval (a box filter), split
   into six cases by where the interval starts, ends and wraps. */
static void v2__osc_render_trisaw(v2__osc *o, float *dest, int32_t n)
{
  /* The cancellation in the wrap cases is amplified by 1/f; the asm keeps
     binary32 with a wide exponent, so the double-evaluated helpers below are
     rounded to float exactly where the reference rounds. */
  float f = v2__utof23((uint32_t)o->freq);
  float omf = (float)(1.0 - (double)f);
  float rcpf = (float)(1.0 / (double)f);
  float col = v2__utof23(o->brpt);
  const float gain = o->gain;

  /* c1 = gain/col, c2 = -gain/(1-col): scaled integration constants */
  float c1 = gain / col;
  float c2 = (float)((double)(-gain) / (1.0 - (double)col));

  uint32_t state = v2__osm_init(o);

  for (int32_t i = 0; i < n; i++) {
    float p = v2__utof23(o->cnt) - col;
    float y = 0.0f;

    switch (v2__osm_tick(o, &state)) {
    case V2__OSMTC_UP:
      y = c1 * (p + p - f);
      break;
    case V2__OSMTC_DOWN:
      y = c2 * (p + p - f);
      break;
    case V2__OSMTC_UP_DOWN:
      y = rcpf * (c2 * (p * p) - c1 * ((p - f) * (p - f)));
      break;
    case V2__OSMTC_DOWN_UP: {
      float t = p + 1.0f - f; /* (p+1)-f, in the asm's association */
      y = -((c2 * (t * t) - c1 * (p * p)) + gain) * rcpf;
      break;
    }
    case V2__OSMTC_UP_DOWN_UP:
      y = -((c1 * (omf * (p + p + omf))) + gain) * rcpf;
      break;
    case V2__OSMTC_DOWN_UP_DOWN:
      y = -((c2 * (omf * (p + p + omf))) + gain) * rcpf;
      break;
    default:
      break;
    }

    v2__osc_output(o, dest + i, y + gain);
  }
}

static void v2__osc_render_pulse(v2__osc *o, float *dest, int32_t n)
{
  const float gain = o->gain;
  float f = v2__utof23((uint32_t)o->freq);
  float gdf = gain / f;
  float col = v2__utof23(o->brpt);

  float cc121 = gdf * 2.0f * (col - 1.0f) + gain;
  float cc212 = gdf * 2.0f * col - gain;

  uint32_t state = v2__osm_init(o);

  for (int32_t i = 0; i < n; i++) {
    float p = v2__utof23(o->cnt);
    float out = 0.0f;

    switch (v2__osm_tick(o, &state)) {
    case V2__OSMTC_UP:           out = gain; break;
    case V2__OSMTC_DOWN:         out = -gain; break;
    case V2__OSMTC_UP_DOWN:      out = gdf * 2.0f * (col - p) + gain; break;
    case V2__OSMTC_DOWN_UP:      out = gdf * 2.0f * p - gain; break;
    case V2__OSMTC_UP_DOWN_UP:   out = cc121; break;
    case V2__OSMTC_DOWN_UP_DOWN: out = cc212; break;
    default: break;
    }

    v2__osc_output(o, dest + i, out);
  }
}

/* The name is a lie: this is a cosine, evaluated with the fast minimax sine. */
static void v2__osc_render_sin(v2__osc *o, float *dest, int32_t n)
{
  for (int32_t i = 0; i < n; i++) {
    uint32_t phase = o->cnt + 0x40000000u;
    o->cnt += (uint32_t)o->freq;

    if (phase & 0x80000000u)
      phase = ~phase;

    float t = v2__bits2float((phase >> 8) | 0x3f800000u);
    t = t * v2__fcpi - v2__fc1p5pi;

    v2__osc_output(o, dest + i, o->gain * v2__fastsin(t));
  }
}

static void v2__osc_render_noise(v2__osc *o, float *dest, int32_t n)
{
  v2__lrc flt = o->nf;
  uint32_t seed = o->nseed;

  for (int32_t i = 0; i < n; i++) {
    float nz = v2__frandom(&seed);
    float h = v2__lrc_step(&flt, nz, o->nffrq, o->nfres);
    float x = o->nfres * (flt.l + h) + flt.b;
    v2__osc_output(o, dest + i, o->gain * x);
  }

  o->nf = flt;
  o->nseed = seed;
}

/* The carrier is a sine; the modulator is whatever the previous oscillators
   left in the voice buffer. */
static void v2__osc_render_fmsin(v2__osc *o, float *dest, int32_t n)
{
  for (int32_t i = 0; i < n; i++) {
    float mod = dest[i] * v2__fcfmmax;
    /* the asm phase is in [1,2) (no -1.0), which matters for fastsinrc */
    float t = (mod + v2__bits2float((o->cnt >> 9) | 0x3f800000u)) * v2__fc2pi;
    o->cnt += (uint32_t)o->freq;

    float out = o->gain * v2__fastsinrc(t);
    if (o->ring)
      dest[i] *= out;
    else
      dest[i] = out;
  }
}

/* The aux oscillators read the cross-channel feedback busses. */
static void v2__osc_render_aux(v2__osc *o, float *dest, const v2__st *src, int32_t n)
{
  for (int32_t i = 0; i < n; i++) {
    float aux = (src[i].ch[0] + src[i].ch[1]) * o->gain * v2__fcgain;
    if (o->ring)
      aux *= dest[i];
    dest[i] = aux;
  }
}

static void v2__osc_render(v2__osc *o, float *dest, int32_t n)
{
  switch (o->mode & 7) {
  case V2__OSC_OFF:                                           break;
  case V2__OSC_TRI_SAW: v2__osc_render_trisaw(o, dest, n);    break;
  case V2__OSC_PULSE:   v2__osc_render_pulse(o, dest, n);     break;
  case V2__OSC_SIN:     v2__osc_render_sin(o, dest, n);       break;
  case V2__OSC_NOISE:   v2__osc_render_noise(o, dest, n);     break;
  case V2__OSC_FM_SIN:  v2__osc_render_fmsin(o, dest, n);     break;
  case V2__OSC_AUXA:    v2__osc_render_aux(o, dest, o->inst->auxabuf, n); break;
  case V2__OSC_AUXB:    v2__osc_render_aux(o, dest, o->inst->auxbbuf, n); break;
  }
}

/* -------------------------------------------------------------------------- */
/*  Envelope generator                                                        */
/* -------------------------------------------------------------------------- */

enum { V2__ENV_OFF = 0, V2__ENV_RELEASE, V2__ENV_ATTACK, V2__ENV_DECAY, V2__ENV_SUSTAIN };

typedef struct v2__env {
  float out;
  int32_t state;
  float val; /* 0..128 */
  float atd; /* attack delta */
  float dcf; /* decay factor */
  float sul; /* sustain level */
  float suf; /* sustain factor */
  float ref; /* release factor */
  float gain;
  v2__inst *inst;
} v2__env;

static void v2__env_init(v2__env *e, v2__inst *inst)
{
  e->state = V2__ENV_OFF;
  e->inst = inst;
}

static void v2__env_set(v2__env *e, const v2__p_env *para)
{
  /* ar: 2^7 (128) down to 2^-4 */
  e->atd = v2__pow2f(para->ar * v2__fcattackmul + v2__fcattackadd);
  /* dcf/ref: 0 (5 ms thanks to volramping) up to almost 1 */
  e->dcf = 1.0f - v2__calcfreq2(1.0f - para->dr / 128.0f);
  e->ref = 1.0f - v2__calcfreq2(1.0f - para->rr / 128.0f);
  e->sul = para->sl;
  /* suf: 1/128 up to 128 */
  e->suf = v2__pow2f(v2__fcsusmul * (para->sr - 64.0f));
  e->gain = para->vol / 128.0f;
}

static void v2__env_tick(v2__env *e, int gate)
{
  if (e->state <= V2__ENV_RELEASE && gate)
    e->state = V2__ENV_ATTACK;
  else if (e->state >= V2__ENV_ATTACK && !gate)
    e->state = V2__ENV_RELEASE;

  switch (e->state) {
  case V2__ENV_OFF:
    e->val = 0.0f;
    break;
  case V2__ENV_ATTACK:
    e->val += e->atd;
    if (e->val >= 128.0f) {
      e->val = 128.0f;
      e->state = V2__ENV_DECAY;
    }
    break;
  case V2__ENV_DECAY:
    e->val *= e->dcf;
    if (e->val <= e->sul) {
      e->val = e->sul;
      e->state = V2__ENV_SUSTAIN;
    }
    break;
  case V2__ENV_SUSTAIN:
    e->val *= e->suf;
    if (e->val > 128.0f)
      e->val = 128.0f;
    break;
  case V2__ENV_RELEASE:
    e->val *= e->ref;
    break;
  }

  /* avoid underflow to denormals */
  if (e->val <= v2__fclowest) {
    e->val = 0.0f;
    e->state = V2__ENV_OFF;
  }

  e->out = e->val * e->gain;
}

/* -------------------------------------------------------------------------- */
/*  Filter                                                                    */
/* -------------------------------------------------------------------------- */

enum { /* filter modes */
  V2__FLT_BYPASS = 0, V2__FLT_LOW, V2__FLT_BAND, V2__FLT_HIGH,
  V2__FLT_NOTCH, V2__FLT_ALL, V2__FLT_MOOGL, V2__FLT_MOOGH
};

typedef struct v2__flt {
  int32_t mode;
  float cfreq;
  float res;
  float moogf, moogp, moogq;
  v2__lrc lrc;
  v2__moog moog;
  v2__inst *inst;
} v2__flt;

static void v2__flt_init(v2__flt *f, v2__inst *inst)
{
  f->lrc.l = f->lrc.b = 0.0f;
  memset(&f->moog, 0, sizeof f->moog);
  f->inst = inst;
}

static void v2__flt_set(v2__flt *f, const v2__p_flt *para)
{
  f->mode = (int32_t)para->mode;
  float fr = v2__calcfreq(para->cutoff / 128.0f) * f->inst->sr_linfreq;
  float r = para->reso / 128.0f;

  if (f->mode < V2__FLT_MOOGL) {
    f->res = 1.0f - r;
    f->cfreq = fr;
  } else {
    /* The asm's moog operand order is replicated exactly: a 4-pole recursive
       filter amplifies any 1-ULP coefficient difference. */
    static const float fc0p8 = 0.8f, fc5p6 = 5.6f;
    fr *= 0.25f;
    float t = 1.0f - fr;

    f->moogp = fr + fc0p8 * (fr * t);
    f->moogf = 1.0f - (f->moogp + f->moogp);
    float qx = fc5p6 * (t * t) + (1.0f - t);
    float qb = r * (1.0f + 0.5f * (t * qx));
    float q2 = qb + qb;
    f->moogq = q2 + q2;
  }
}

/* `step` is the sample stride: 2 filters the interleaved L or R channel. */
static void v2__flt_render(v2__flt *f, float *dest, const float *src, int32_t n, int32_t step)
{
  const float dco = v2__fcdcoffset;
  const float cfreq = f->cfreq, res = f->res;
  int32_t m7 = f->mode & 7;
  int32_t i;

  switch (m7) {
  case V2__FLT_BYPASS:
    if (dest != src)
      memmove(dest, src, (size_t)n * sizeof(float)); /* ignores step, like the original */
    break;

  case V2__FLT_LOW: {
    float l = f->lrc.l, b = f->lrc.b;
    for (i = 0; i < n; i++) {
      v2__lrc_step_2x(&l, &b, src[i * step], cfreq, res, dco);
      dest[i * step] = l;
    }
    f->lrc.l = l; f->lrc.b = b;
    break;
  }
  case V2__FLT_BAND: {
    float l = f->lrc.l, b = f->lrc.b;
    for (i = 0; i < n; i++) {
      v2__lrc_step_2x(&l, &b, src[i * step], cfreq, res, dco);
      dest[i * step] = b;
    }
    f->lrc.l = l; f->lrc.b = b;
    break;
  }
  case V2__FLT_HIGH: {
    float l = f->lrc.l, b = f->lrc.b;
    for (i = 0; i < n; i++) {
      float h = v2__lrc_step_2x(&l, &b, src[i * step], cfreq, res, dco);
      dest[i * step] = h;
    }
    f->lrc.l = l; f->lrc.b = b;
    break;
  }
  case V2__FLT_NOTCH: {
    float l = f->lrc.l, b = f->lrc.b;
    for (i = 0; i < n; i++) {
      float h = v2__lrc_step_2x(&l, &b, src[i * step], cfreq, res, dco);
      dest[i * step] = l + h;
    }
    f->lrc.l = l; f->lrc.b = b;
    break;
  }
  case V2__FLT_ALL: {
    /* (h + b) + l, the asm's summation order */
    float l = f->lrc.l, b = f->lrc.b;
    for (i = 0; i < n; i++) {
      float h = v2__lrc_step_2x(&l, &b, src[i * step], cfreq, res, dco);
      dest[i * step] = h + b + l;
    }
    f->lrc.l = l; f->lrc.b = b;
    break;
  }
  case V2__FLT_MOOGL: {
    /* the moog is 2x oversampled: step twice */
    v2__moog m = f->moog;
    for (i = 0; i < n; i++) {
      float in = src[i * step];
      v2__moog_step(&m, in, f->moogf, f->moogp, f->moogq);
      dest[i * step] = v2__moog_step(&m, in, f->moogf, f->moogp, f->moogq);
    }
    f->moog = m;
    break;
  }
  case V2__FLT_MOOGH: {
    v2__moog m = f->moog;
    for (i = 0; i < n; i++) {
      float in = src[i * step];
      v2__moog_step(&m, in, f->moogf, f->moogp, f->moogq);
      dest[i * step] = in - v2__moog_step(&m, in, f->moogf, f->moogp, f->moogq);
    }
    f->moog = m;
    break;
  }
  }
}

/* -------------------------------------------------------------------------- */
/*  Low frequency oscillator                                                  */
/* -------------------------------------------------------------------------- */

enum { V2__LFO_SAW = 0, V2__LFO_TRI, V2__LFO_PULSE, V2__LFO_SIN, V2__LFO_SH };

typedef struct v2__lfo {
  float out;
  int32_t mode;
  int sync;
  int eg;         /* one-shot mode */
  int32_t freq;
  uint32_t cntr;
  uint32_t cphase; /* counter value at key sync */
  float gain;
  float dc;
  uint32_t nseed;
  uint32_t last;  /* last counter value, for the S&H transition */
  v2__inst *inst;
} v2__lfo;

static void v2__lfo_init(v2__lfo *l, v2__inst *inst)
{
  l->cntr = l->last = 0;
  l->inst = inst;
  l->nseed = v2__seed_mix(inst->user_seed, v2__rand_next(&inst->rng), 1);
}

static void v2__lfo_set(v2__lfo *l, const v2__p_lfo *para)
{
  l->mode = (int32_t)para->mode;
  l->sync = (int32_t)para->sync != 0;
  l->eg = (int32_t)para->egmode != 0;
  /* fistp, not a truncating cast; the 0.5 compensates for the 128-sample frame */
  l->freq = v2__fistp(v2__calcfreq(para->rate * 0.0078125f) * (v2__fc32bit * 0.5f));
  l->cphase = v2__ftou32(para->phase / 128.0f);

  switch ((int32_t)para->pol) {
  case 0: /* + */
    l->gain = para->amp;
    l->dc = 0.0f;
    break;
  case 1: /* - */
    l->gain = -para->amp;
    l->dc = 0.0f;
    break;
  case 2: /* +/- */
    l->gain = para->amp;
    l->dc = -0.5f * para->amp;
    break;
  }
}

static void v2__lfo_keyon(v2__lfo *l)
{
  if (l->sync) {
    l->cntr = l->cphase;
    l->last = ~0u;
  }
}

static void v2__lfo_tick(v2__lfo *l)
{
  float v;
  uint32_t x;

  switch (l->mode & 7) {
  case V2__LFO_SAW:
  default:
    v = v2__utof23(l->cntr);
    break;
  case V2__LFO_TRI:
    x = (l->cntr << 1) ^ (uint32_t)((int32_t)l->cntr >> 31);
    v = v2__utof23(x);
    break;
  case V2__LFO_PULSE:
    x = (uint32_t)((int32_t)l->cntr >> 31);
    v = v2__utof23(x);
    break;
  case V2__LFO_SIN:
    v = v2__utof23(l->cntr);
    v = v2__fastsinrc(v * v2__fc2pi) * 0.5f + 0.5f;
    break;
  case V2__LFO_SH:
    if (l->cntr < l->last)
      l->nseed = v2__urandom(&l->nseed);
    l->last = l->cntr;
    v = v2__utof23(l->nseed);
    break;
  }

  l->out = v * l->gain + l->dc;
  l->cntr += (uint32_t)l->freq;
  if (l->cntr < (uint32_t)l->freq && l->eg) /* one-shot: clamp at wrap-around */
    l->cntr = ~0u;
}

/* -------------------------------------------------------------------------- */
/*  Distortion                                                                */
/* -------------------------------------------------------------------------- */

enum { /* distortion modes; the filter modes continue the filter numbering */
  V2__DIST_OFF = 0, V2__DIST_OVERDRIVE, V2__DIST_CLIP, V2__DIST_BITCRUSHER, V2__DIST_DECIMATOR,
  V2__DIST_FLT_BASE = V2__DIST_DECIMATOR,
  V2__DIST_FLT_LOW  = V2__DIST_FLT_BASE + V2__FLT_LOW,
  V2__DIST_FLT_BAND = V2__DIST_FLT_BASE + V2__FLT_BAND,
  V2__DIST_FLT_HIGH = V2__DIST_FLT_BASE + V2__FLT_HIGH,
  V2__DIST_FLT_NOTCH = V2__DIST_FLT_BASE + V2__FLT_NOTCH,
  V2__DIST_FLT_ALL  = V2__DIST_FLT_BASE + V2__FLT_ALL
};

typedef struct v2__dist {
  int32_t mode;
  float gain1;    /* input gain for all fx */
  float gain2;    /* output gain for od/clip */
  float offs;     /* offset for od/clip */
  float crush1;   /* 1/crush_factor */
  int32_t crush2; /* crush_factor */
  int32_t crxor;  /* xor value for crush */
  uint32_t dcount;
  uint32_t dfreq;
  float dvall, dvalr; /* last decimator values */
  v2__flt fltl, fltr;
  v2__inst *inst;
} v2__dist;

static void v2__dist_init(v2__dist *d, v2__inst *inst)
{
  d->dcount = 0;
  d->dvall = d->dvalr = 0.0f;
  v2__flt_init(&d->fltl, inst);
  v2__flt_init(&d->fltr, inst);
  d->inst = inst;
}

static void v2__dist_set(v2__dist *d, const v2__p_dist *para)
{
  float x;

  d->mode = (int32_t)para->mode;
  d->gain1 = v2__pow2f((para->ingain - 32.0f) / 16.0f);

  switch (d->mode) {
  case V2__DIST_OFF:
    break;
  case V2__DIST_OVERDRIVE:
    d->gain2 = v2__od_gain2(para->param1 / 128.0f, d->gain1);
    d->offs = d->gain1 * 2.0f * ((para->param2 / 128.0f) - 0.5f);
    break;
  case V2__DIST_CLIP:
    d->gain2 = para->param1 / 128.0f;
    d->offs = d->gain1 * 2.0f * ((para->param2 / 128.0f) - 0.5f);
    break;
  case V2__DIST_BITCRUSHER:
    x = para->param1 * 256.0f + 1.0f;
    d->crush2 = (int32_t)x;
    d->crush1 = d->gain1 * (32768.0f / x);
    d->crxor = ((int32_t)para->param2) << 9;
    break;
  case V2__DIST_DECIMATOR:
    d->dfreq = v2__ftou32(v2__calcfreq(para->param1 / 128.0f));
    break;
  default: { /* filters */
    v2__p_flt setup;
    setup.cutoff = para->param1;
    setup.reso = para->param2;
    setup.mode = (float)(d->mode - V2__DIST_FLT_BASE);
    v2__flt_set(&d->fltl, &setup);
    v2__flt_set(&d->fltr, &setup);
    break;
  }
  }
}

static inline void v2__dist_decimator_tick(v2__dist *d, float l, float r)
{
  d->dcount += d->dfreq;
  if (d->dcount < d->dfreq) { /* carry */
    d->dvall = l;
    d->dvalr = r;
  }
}

static void v2__dist_render_mono(v2__dist *d, float *dest, const float *src, int32_t n)
{
  int32_t i;
  switch (d->mode) {
  case V2__DIST_OFF:
    if (dest != src)
      memmove(dest, src, (size_t)n * sizeof(float));
    break;

  case V2__DIST_OVERDRIVE:
    for (i = 0; i < n; i++)
      dest[i] = d->gain2 * v2__fastatan(src[i] * d->gain1 + d->offs);
    break;

  case V2__DIST_CLIP:
    for (i = 0; i < n; i++)
      dest[i] = d->gain2 * v2__clampf(src[i] * d->gain1 + d->offs, -1.0f, 1.0f);
    break;

  case V2__DIST_BITCRUSHER:
    for (i = 0; i < n; i++) {
      float scaled = src[i] * d->crush1;
      int32_t t = v2__fistp(scaled); /* the asm uses fistp (round to nearest) */
      int32_t m = (int32_t)((uint32_t)t * (uint32_t)d->crush2);
      m = (m < -0x7fff) ? -0x7fff : (m > 0x7fff) ? 0x7fff : m;
      dest[i] = (float)(m ^ d->crxor) / 32768.0f;
    }
    break;

  case V2__DIST_DECIMATOR:
    for (i = 0; i < n; i++) {
      v2__dist_decimator_tick(d, src[i], 0.0f);
      dest[i] = d->dvall;
    }
    break;

  default: /* filters */
    v2__flt_render(&d->fltl, dest, src, n, 1);
    break;
  }
}

static void v2__dist_render_stereo(v2__dist *d, v2__st *dest, const v2__st *src, int32_t n)
{
  int32_t i;
  switch (d->mode) {
  case V2__DIST_DECIMATOR:
    for (i = 0; i < n; i++) {
      v2__dist_decimator_tick(d, src[i].ch[0], src[i].ch[1]);
      dest[i].ch[0] = d->dvall;
      dest[i].ch[1] = d->dvalr;
    }
    break;

  case V2__DIST_FLT_LOW:
  case V2__DIST_FLT_BAND:
  case V2__DIST_FLT_HIGH:
  case V2__DIST_FLT_NOTCH:
  case V2__DIST_FLT_ALL:
    v2__flt_render(&d->fltl, &dest[0].ch[0], &src[0].ch[0], n, 2);
    v2__flt_render(&d->fltr, &dest[0].ch[1], &src[0].ch[1], n, 2);
    break;

  default:
    /* everything else is presumed stateless (the moog filters deliberately
       run as one mono filter over the interleaved words, like the original) */
    v2__dist_render_mono(d, &dest[0].ch[0], &src[0].ch[0], n * 2);
    break;
  }
}

/* -------------------------------------------------------------------------- */
/*  DC filter (stereo high-pass with a very low cut-off)                      */
/* -------------------------------------------------------------------------- */

typedef struct v2__dcfilter {
  v2__dcf fl, fr;
  v2__inst *inst;
} v2__dcfilter;

static void v2__dcfilter_init(v2__dcfilter *d, v2__inst *inst)
{
  d->inst = inst;
  d->fl.xm1 = d->fl.ym1 = 0.0f;
  d->fr.xm1 = d->fr.ym1 = 0.0f;
}

static void v2__dcfilter_render_mono(v2__dcfilter *d, float *dest, const float *src, int32_t n)
{
  float R = d->inst->sr_dcfilter;
  v2__dcf l = d->fl;
  for (int32_t i = 0; i < n; i++)
    dest[i] = v2__dcf_step(&l, src[i], R);
  d->fl = l;
}

static void v2__dcfilter_render_stereo(v2__dcfilter *d, v2__st *dest, const v2__st *src, int32_t n)
{
  float R = d->inst->sr_dcfilter;
  v2__dcf l = d->fl;
  v2__dcf r = d->fr;
  for (int32_t i = 0; i < n; i++) {
    dest[i].ch[0] = v2__dcf_step(&l, src[i].ch[0], R);
    dest[i].ch[1] = v2__dcf_step(&r, src[i].ch[1], R);
  }
  d->fl = l;
  d->fr = r;
}

/* -------------------------------------------------------------------------- */
/*  Voice                                                                     */
/* -------------------------------------------------------------------------- */

enum { V2__FLTR_SINGLE = 0, V2__FLTR_SERIAL, V2__FLTR_PARALLEL };
enum { V2__SYNC_NONE = 0, V2__SYNC_OSC, V2__SYNC_FULL };

typedef struct v2__voice {
  int32_t note;
  float velo;
  int gate;

  float curvol;
  float volramp;

  float xpose;
  int32_t fmode;
  float lvol, rvol;
  float f1gain, f2gain;
  int32_t keysync;

  v2__osc osc[V2__NOSC];
  v2__flt vcf[V2__NFLT];
  v2__env env[V2__NENV];
  v2__lfo lfo[V2__NLFO];
  v2__dist dist;
  v2__dcfilter dcf;

  v2__inst *inst;
} v2__voice;

static void v2__voice_init(v2__voice *v, v2__inst *inst)
{
  int32_t i;
  for (i = 0; i < V2__NOSC; i++) v2__osc_init(&v->osc[i], inst, i);
  for (i = 0; i < V2__NFLT; i++) v2__flt_init(&v->vcf[i], inst);
  for (i = 0; i < V2__NENV; i++) v2__env_init(&v->env[i], inst);
  for (i = 0; i < V2__NLFO; i++) v2__lfo_init(&v->lfo[i], inst);
  v2__dist_init(&v->dist, inst);
  v2__dcfilter_init(&v->dcf, inst);
  v->inst = inst;
}

static void v2__voice_tick(v2__voice *v)
{
  int32_t i;
  for (i = 0; i < V2__NENV; i++) v2__env_tick(&v->env[i], v->gate);
  for (i = 0; i < V2__NLFO; i++) v2__lfo_tick(&v->lfo[i]);

  /* volume ramping slope */
  v->volramp = (v->env[0].out / 128.0f - v->curvol) * v->inst->sr_iframe;
}

static void v2__voice_render(v2__voice *v, v2__st *dest, int32_t n)
{
  v2__inst *inst = v->inst;
  float *voice = inst->vcebuf;
  float *voice2 = inst->vcebuf2;
  int32_t i;

  memset(voice, 0, (size_t)n * sizeof(float));

  for (i = 0; i < V2__NOSC; i++)
    v2__osc_render(&v->osc[i], voice, n);

  switch (v->fmode) {
  case V2__FLTR_SINGLE:
    v2__flt_render(&v->vcf[0], voice, voice, n, 1);
    break;
  case V2__FLTR_PARALLEL:
    v2__flt_render(&v->vcf[1], voice2, voice, n, 1);
    v2__flt_render(&v->vcf[0], voice, voice, n, 1);
    for (i = 0; i < n; i++)
      voice[i] = voice[i] * v->f1gain + voice2[i] * v->f2gain;
    break;
  case V2__FLTR_SERIAL:
  default:
    v2__flt_render(&v->vcf[0], voice, voice, n, 1);
    v2__flt_render(&v->vcf[1], voice, voice, n, 1);
    break;
  }

  v2__dist_render_mono(&v->dist, voice, voice, n);
  v2__dcfilter_render_mono(&v->dcf, voice, voice, n);

  /* voice buffer (mono) -> += channel buffer (stereo) */
  const float dco = v2__fcdcoffset;
  float cv = v->curvol;
  for (i = 0; i < n; i++) {
    float out = voice[i] * cv;
    cv += v->volramp;

    dest[i].ch[0] += v->lvol * out + dco;
    dest[i].ch[1] += v->rvol * out + dco;
  }
  v->curvol = cv;
}

static void v2__voice_update_note(v2__voice *v)
{
  float n = v->xpose + (float)v->note;
  for (int32_t i = 0; i < V2__NOSC; i++)
    v->osc[i].note = n;
}

static void v2__voice_set(v2__voice *v, const v2__p_voice_s *para)
{
  int32_t i;

  v->xpose = para->transp - 64.0f;
  v2__voice_update_note(v);

  v->fmode = (int32_t)para->routing;
  v->keysync = (int32_t)para->oscsync;

  /* equal power panning */
  float p = para->panning / 128.0f;
  v->lvol = sqrtf(1.0f - p);
  v->rvol = sqrtf(p);

  /* The asm branches on the sign of the rounded fistp(fltbal-64), not on the
     float sign of x. */
  float x = (para->fltbal - 64.0f) / 64.0f;
  int32_t xi = v2__fistp(para->fltbal - 64.0f);
  if (xi >= 0) {
    v->f2gain = 1.0f;
    v->f1gain = 1.0f - x;
  } else {
    v->f1gain = 1.0f;
    v->f2gain = 1.0f + x;
  }

  for (i = 0; i < V2__NOSC; i++) v2__osc_set(&v->osc[i], &para->osc[i]);
  for (i = 0; i < V2__NENV; i++) v2__env_set(&v->env[i], &para->env[i]);
  for (i = 0; i < V2__NFLT; i++) v2__flt_set(&v->vcf[i], &para->flt[i]);
  for (i = 0; i < V2__NLFO; i++) v2__lfo_set(&v->lfo[i], &para->lfo[i]);
  v2__dist_set(&v->dist, &para->dist);
}

static void v2__voice_noteon(v2__voice *v, int32_t note, int32_t vel)
{
  int32_t i;

  v->note = note;
  v2__voice_update_note(v);

  v->velo = (float)vel;
  v->gate = 1;

  for (i = 0; i < V2__NENV; i++)
    v->env[i].state = V2__ENV_ATTACK;

  switch (v->keysync) {
  case V2__SYNC_FULL:
    for (i = 0; i < V2__NENV; i++)
      v->env[i].val = 0.0f;
    v->curvol = 0.0f;

    for (i = 0; i < V2__NOSC; i++)
      v2__osc_init(&v->osc[i], v->inst, i);
    for (i = 0; i < V2__NFLT; i++)
      v2__flt_init(&v->vcf[i], v->inst);
    v2__dist_init(&v->dist, v->inst);
    /* fall through */
  case V2__SYNC_OSC:
    for (i = 0; i < V2__NOSC; i++)
      v->osc[i].cnt = 0;
    /* fall through */
  case V2__SYNC_NONE:
  default:
    break;
  }

  for (i = 0; i < V2__NOSC; i++) v2__osc_chgpitch(&v->osc[i]);
  for (i = 0; i < V2__NLFO; i++) v2__lfo_keyon(&v->lfo[i]);

  v2__dcfilter_init(&v->dcf, v->inst);
}

/* -------------------------------------------------------------------------- */
/*  Bass boost (fixed low shelving EQ)                                        */
/* -------------------------------------------------------------------------- */

typedef struct v2__boost {
  int enabled;
  float a1, a2;
  float b0, b1, b2;
  float x1[2], x2[2], y1[2], y2[2];
  v2__inst *inst;
} v2__boost;

static void v2__boost_set(v2__boost *b, const v2__p_boost *para)
{
  b->enabled = ((int32_t)para->amount) != 0;
  if (!b->enabled)
    return;

  /* A = 10^(dBgain/40), or a rough approximation */
  float A = v2__pow2f(para->amount / 128.0f);

  /* beta = sqrt((A^2+1) - (A-1)^2) is sqrt(2A) algebraically, but the asm
     rounds step by step; the stepwise form and the summation orders below
     are load-bearing for bit-exactness. */
  float beta = sqrtf((A * A + 1.0f) - (A - 1.0f) * (A - 1.0f));

  float bs = beta * b->inst->sr_boostsin;
  float Am1 = A - 1.0f;
  float Ap1 = A + 1.0f;
  float cAm1 = Am1 * b->inst->sr_boostcos;
  float cAp1 = Ap1 * b->inst->sr_boostcos;

  float ia0 = 1.0f / ((bs + cAm1) + Ap1);

  b->b1 = 2.0f * A * (Am1 - cAp1) * ia0;
  b->a1 = -2.0f * (Am1 + cAp1) * ia0;
  b->a2 = (Ap1 + cAm1 - bs) * ia0;
  float Aia0 = A * ia0;
  b->b0 = (Ap1 - cAm1 + bs) * Aia0;
  b->b2 = (Ap1 - cAm1 - bs) * Aia0;
}

static void v2__boost_render(v2__boost *b, v2__st *buf, int32_t n)
{
  if (!b->enabled)
    return;

  for (int32_t ch = 0; ch < 2; ch++) {
    float xm1 = b->x1[ch], xm2 = b->x2[ch];
    float ym1 = b->y1[ch], ym2 = b->y2[ch];

    for (int32_t i = 0; i < n; i++) {
      float x = buf[i].ch[ch] + v2__fcdcoffset;
      float y = b->b0 * x + ((b->b1 * xm1 - b->a1 * ym1) + (b->b2 * xm2 - b->a2 * ym2));
      ym2 = ym1; ym1 = y;
      xm2 = xm1; xm1 = x;
      buf[i].ch[ch] = y;
    }

    b->x1[ch] = xm1; b->x2[ch] = xm2;
    b->y1[ch] = ym1; b->y2[ch] = ym2;
  }
}

/* -------------------------------------------------------------------------- */
/*  Modulating delay (chorus/flanger and the global stereo delay)             */
/* -------------------------------------------------------------------------- */

typedef struct v2__moddel {
  float *db[2];     /* left/right delay buffer */
  uint32_t dbufmask; /* size-1, size is a power of two */
  uint32_t dbptr;   /* write position */
  uint32_t dboffs[2];
  uint32_t mcnt;    /* modulation counter */
  int32_t mfreq;
  uint32_t mphase;
  uint32_t mmaxoffs;
  float fbval;
  float dryout;
  float wetout;
  v2__inst *inst;
} v2__moddel;

static void v2__moddel_reset(v2__moddel *m)
{
  m->dbptr = 0;
  m->mcnt = 0;
  memset(m->db[0], 0, ((size_t)m->dbufmask + 1) * sizeof(float));
  memset(m->db[1], 0, ((size_t)m->dbufmask + 1) * sizeof(float));
}

/* buflen must be a power of two */
static void v2__moddel_init(v2__moddel *m, v2__inst *inst, float *buf1, float *buf2, uint32_t buflen)
{
  m->db[0] = buf1;
  m->db[1] = buf2;
  m->dbufmask = buflen - 1;
  m->inst = inst;
  v2__moddel_reset(m);
}

static void v2__moddel_set(v2__moddel *m, const v2__p_moddel *para)
{
  m->wetout = (para->amount - 64.0f) / 64.0f;
  m->dryout = 1.0f - fabsf(m->wetout);
  m->fbval = (para->fb - 64.0f) / 64.0f;

  /* every int conversion here is a fistp in the asm */
  float lenscale = ((float)m->dbufmask - 1023.0f) * 0.0078125f;
  m->dboffs[0] = (uint32_t)v2__fistp(para->llength * lenscale);
  m->dboffs[1] = (uint32_t)v2__fistp(para->rlength * lenscale);

  m->mfreq = v2__fistp(v2__calcfreq(para->mrate * 0.0078125f) * v2__fcmdlfomul * m->inst->sr_linfreq);
  m->mmaxoffs = (uint32_t)v2__fistp(para->mdepth * 0.0078125f * 1023.0f);
  m->mphase = 2u * (uint32_t)v2__fistp((para->mphase - 64.0f) * 0.0078125f * v2__fc32bit);
}

static inline float v2__moddel_process_chan(v2__moddel *m, float in, int32_t ch, float dry)
{
  /* modulation is a triangle wave */
  uint32_t counter = m->mcnt + (ch ? m->mphase : 0u);
  counter = (counter < 0x80000000u) ? counter * 2u : 0xffffffffu - counter * 2u;

  /* effective offset in 32.32 fixed point */
  uint64_t offs32_32 = (uint64_t)counter * m->mmaxoffs;
  uint32_t offs_int = (uint32_t)(offs32_32 >> 32) + m->dboffs[ch];
  /* the right channel reads one sample further back (asm `dec ebx`) */
  uint32_t idx = m->dbptr - offs_int - (ch ? 1u : 0u);

  float *delaybuf = m->db[ch];
  float x = v2__utof23((uint32_t)(offs32_32 & 0xffffffffu));
  float in1 = delaybuf[idx & m->dbufmask];
  float in2 = delaybuf[(idx + 1) & m->dbufmask];
  float delayed = in1 + (1.0f - x) * (in2 - in1);

  delaybuf[m->dbptr] = in + delayed * m->fbval;
  return in * dry + delayed * m->wetout;
}

static inline void v2__moddel_process(v2__moddel *m, v2__st *out, float l, float r, float dry)
{
  out->ch[0] = v2__moddel_process_chan(m, l, 0, dry);
  out->ch[1] = v2__moddel_process_chan(m, r, 1, dry);

  m->mcnt += (uint32_t)m->mfreq;
  m->dbptr = (m->dbptr + 1) & m->dbufmask;
}

static void v2__moddel_render_aux2main(v2__moddel *m, v2__st *dest, int32_t n)
{
  if (m->wetout == 0.0f)
    return;

  for (int32_t i = 0; i < n; i++) {
    v2__st x;
    float in = m->inst->aux2buf[i] + v2__fcdcoffset;
    v2__moddel_process(m, &x, in, in, 0.0f);
    dest[i].ch[0] += x.ch[0];
    dest[i].ch[1] += x.ch[1];
  }
}

static void v2__moddel_render_chan(v2__moddel *m, v2__st *chanbuf, int32_t n)
{
  if (m->wetout == 0.0f)
    return;

  const float dco = v2__fcdcoffset;
  float dry = m->dryout;
  for (int32_t i = 0; i < n; i++)
    v2__moddel_process(m, &chanbuf[i], chanbuf[i].ch[0] + dco, chanbuf[i].ch[1] + dco, dry);
}

/* -------------------------------------------------------------------------- */
/*  Stereo compressor                                                         */
/* -------------------------------------------------------------------------- */

#define V2__COMPDLEN 5700 /* lookahead delay line (stereo samples) */
#define V2__RMSLEN   8192 /* RMS ring buffer, power of two */

enum { V2__COMP_MODE_OFF = 0, V2__COMP_MODE_PEAK, V2__COMP_MODE_RMS };
enum { /* internal mode bits */
  V2__CB_PEAK = 0, V2__CB_RMS = 1,
  V2__CB_MONO = 0, V2__CB_STEREO = 2,
  V2__CB_ON = 0,   V2__CB_OFF = 4
};

typedef struct v2__comp {
  int32_t mode;     /* bit 0: peak/rms, bit 1: stereo, bit 2: off */
  int32_t oldmode;  /* last mode set() saw; drives the reset */
  float invol;      /* input gain (1/threshold; internal threshold is 0 dB) */
  float ratio;
  float outvol;
  float attack;     /* lpf coefficient, 0..1 */
  float release;
  uint32_t dblen;   /* lookahead length */
  uint32_t dbcnt;   /* lookahead write offset */
  float curgain[2];
  float peakval[2];
  float rmsval[2];
  uint32_t rmscnt;
  v2__st dbuf[V2__COMPDLEN];
  v2__st rmsbuf[V2__RMSLEN];
  v2__inst *inst;
} v2__comp;

static void v2__comp_init(v2__comp *c, v2__inst *inst)
{
  /* The asm's syCompInit only sets mode=2. A compressor that is peak/mono/on
     from its very first set() never takes the mode-change reset and starts
     with curgain = 0 (the first note fades in over ~25 ms); that is the
     shipped behavior, so oldmode/curgain deliberately start at zero. */
  c->mode = V2__CB_STEREO;
  memset(c->dbuf, 0, sizeof c->dbuf);
  c->inst = inst;
  c->oldmode = 0;
  for (int32_t i = 0; i < 2; i++) {
    c->peakval[i] = 0.0f;
    c->rmsval[i] = 0.0f;
    c->curgain[i] = 0.0f;
  }
  memset(c->rmsbuf, 0, sizeof c->rmsbuf);
  c->rmscnt = 0;
}

static void v2__comp_reset(v2__comp *c)
{
  for (int32_t i = 0; i < 2; i++) {
    c->peakval[i] = 0.0f;
    c->rmsval[i] = 0.0f;
    c->curgain[i] = 1.0f;
  }
  memset(c->rmsbuf, 0, sizeof c->rmsbuf);
  c->rmscnt = 0;
}

static void v2__comp_set(v2__comp *c, const v2__p_comp *para)
{
  switch ((int32_t)para->mode) {
  case V2__COMP_MODE_OFF:  c->mode = V2__CB_OFF; break;
  case V2__COMP_MODE_PEAK: c->mode = V2__CB_PEAK | V2__CB_ON; break;
  case V2__COMP_MODE_RMS:  c->mode = V2__CB_RMS | V2__CB_ON; break;
  default: break;
  }

  if (para->stereo != 0.0f)
    c->mode |= V2__CB_STEREO;

  /* compares against a persistent oldmode, not last frame's mode */
  if (c->mode != c->oldmode) {
    c->oldmode = c->mode;
    v2__comp_reset(c);
  }

  /* sample-rate scaled lookahead, bounded by the delay line */
  int32_t dblen = v2__fistp(para->lookahead * c->inst->sr_samplesperms);
  if (dblen < 0) dblen = 0;
  if (dblen > V2__COMPDLEN - 1) dblen = V2__COMPDLEN - 1;
  c->dblen = (uint32_t)dblen;

  float thresh = 8.0f * v2__calcfreq(para->threshold / 128.0f);
  c->invol = 1.0f / thresh;
  if (para->autogain != 0.0f)
    thresh = 1.0f;
  c->outvol = thresh * v2__pow2f((para->outgain - 64.0f) / 16.0f);
  c->ratio = para->ratio / 128.0f;

  /* attack: 0 .. 200 ms, release: 5 ms .. 5 s */
  c->attack = v2__pow2f(-para->attack * 12.0f / 128.0f);
  c->release = v2__pow2f(-para->release * 16.0f / 128.0f);
}

static inline float v2__comp_peak(v2__comp *c, float in, int32_t ch)
{
  c->peakval[ch] = v2__maxf(c->peakval[ch] * v2__fccpdfalloff + v2__fcdcoffset, fabsf(in));
  return c->peakval[ch];
}

/* Op-for-op port of syCompLD{Mono,Stereo}RMS: the oldest sample is subtracted
   first, the denormal bias goes on the accumulator (stereo) or on the input
   (mono, via the caller), and the output is sqrt(rv) * 1/sqrt(8192). */
static inline float v2__comp_rms(v2__comp *c, float in, int32_t ch, float accumdc)
{
  c->rmsval[ch] -= c->rmsbuf[c->rmscnt].ch[ch];
  c->rmsval[ch] += accumdc;
  float insq = v2__sqr(in);
  c->rmsval[ch] += insq;
  c->rmsbuf[c->rmscnt].ch[ch] = insq;
  return sqrtf(c->rmsval[ch]) * v2__fcrms8192;
}

static void v2__comp_render(v2__comp *c, v2__st *buf, int32_t n)
{
  if (c->mode & V2__CB_OFF)
    return;

  /* step 1: level detection */
  v2__st *levels = c->inst->levelbuf;
  int32_t i, ch;
  switch (c->mode & (V2__CB_RMS | V2__CB_STEREO)) {
  case V2__CB_PEAK | V2__CB_MONO:
    for (i = 0; i < n; i++)
      levels[i].ch[0] = levels[i].ch[1] = c->invol * v2__comp_peak(c, 0.5f * (buf[i].ch[0] + buf[i].ch[1]), 0);
    break;

  case V2__CB_RMS | V2__CB_MONO:
    for (i = 0; i < n; i++) {
      levels[i].ch[0] = levels[i].ch[1] =
        c->invol * v2__comp_rms(c, 0.5f * (buf[i].ch[0] + buf[i].ch[1]) + v2__fcdcoffset, 0, 0.0f);
      c->rmscnt = (c->rmscnt + 1) & (V2__RMSLEN - 1);
    }
    break;

  case V2__CB_PEAK | V2__CB_STEREO:
    for (i = 0; i < n; i++) {
      levels[i].ch[0] = c->invol * v2__comp_peak(c, buf[i].ch[0], 0);
      levels[i].ch[1] = c->invol * v2__comp_peak(c, buf[i].ch[1], 1);
    }
    break;

  case V2__CB_RMS | V2__CB_STEREO:
    for (i = 0; i < n; i++) {
      levels[i].ch[0] = c->invol * v2__comp_rms(c, buf[i].ch[0], 0, v2__fcdcoffset);
      levels[i].ch[1] = c->invol * v2__comp_rms(c, buf[i].ch[1], 1, v2__fcdcoffset);
      c->rmscnt = (c->rmscnt + 1) & (V2__RMSLEN - 1);
    }
    break;
  }

  /* step 2: compress */
  for (ch = 0; ch < 2; ch++) {
    float gain = c->curgain[ch];
    uint32_t dbind = c->dbcnt;

    for (i = 0; i < n; i++) {
      /* lookahead delay line; the ring wraps when the index EXCEEDS dblen */
      float v = c->outvol * c->dbuf[dbind].ch[ch];
      c->dbuf[dbind].ch[ch] = c->invol * buf[i].ch[ch];
      if (++dbind > c->dblen)
        dbind = 0;

      float dgain = 1.0f;
      float lvl = levels[i].ch[ch];
      if (lvl >= 1.0f)
        dgain = 1.0f / (1.0f + c->ratio * (lvl - 1.0f));

      gain += (dgain < gain ? c->attack : c->release) * (dgain - gain);
      buf[i].ch[ch] = v * gain;
    }

    c->curgain[ch] = gain;
    if (ch == 1)
      c->dbcnt = dbind;
  }
}

/* -------------------------------------------------------------------------- */
/*  Stereo reverb                                                             */
/* -------------------------------------------------------------------------- */

typedef struct v2__reverb {
  float gainc[4];   /* comb feedback gains */
  float gaina[2];   /* allpass feedback gains */
  float gainin;
  float damp;       /* high cut */
  float lowcut;
  v2__delay combd[2][4];
  float combl[2][4];
  v2__delay alld[2][2];
  float hpf[2];     /* low cut filter memory */
  v2__inst *inst;

  float bcombl0[1309], bcombl1[1635], bcombl2[1811], bcombl3[1926];
  float balll0[220], balll1[74];
  float bcombr0[1327], bcombr1[1631], bcombr2[1833], bcombr3[1901];
  float ballr0[205], ballr1[77];
} v2__reverb;

static void v2__reverb_reset(v2__reverb *r)
{
  for (int32_t ch = 0; ch < 2; ch++) {
    for (int32_t i = 0; i < 4; i++) {
      v2__delay_reset(&r->combd[ch][i]);
      r->combl[ch][i] = 0.0f;
    }
    for (int32_t i = 0; i < 2; i++)
      v2__delay_reset(&r->alld[ch][i]);
    r->hpf[ch] = 0.0f;
  }
}

#define V2__DELAY_INIT(d, arr) v2__delay_init(&(d), (arr), (uint32_t)(sizeof(arr) / sizeof((arr)[0])))

static void v2__reverb_init(v2__reverb *r, v2__inst *inst)
{
  V2__DELAY_INIT(r->combd[0][0], r->bcombl0);
  V2__DELAY_INIT(r->combd[0][1], r->bcombl1);
  V2__DELAY_INIT(r->combd[0][2], r->bcombl2);
  V2__DELAY_INIT(r->combd[0][3], r->bcombl3);
  V2__DELAY_INIT(r->alld[0][0], r->balll0);
  V2__DELAY_INIT(r->alld[0][1], r->balll1);
  V2__DELAY_INIT(r->combd[1][0], r->bcombr0);
  V2__DELAY_INIT(r->combd[1][1], r->bcombr1);
  V2__DELAY_INIT(r->combd[1][2], r->bcombr2);
  V2__DELAY_INIT(r->combd[1][3], r->bcombr3);
  V2__DELAY_INIT(r->alld[1][0], r->ballr0);
  V2__DELAY_INIT(r->alld[1][1], r->ballr1);

  r->inst = inst;
  v2__reverb_reset(r);
}

static void v2__reverb_set(v2__reverb *r, const v2__p_reverb *para)
{
  static const float gaincdef[4] = { 0.966384599f, 0.958186359f, 0.953783929f, 0.950933178f };
  static const float gainadef[2] = { 0.994260075f, 0.998044717f };

  float e = r->inst->sr_linfreq * v2__sqr(64.0f / (para->revtime + 1.0f));
  for (int32_t i = 0; i < 4; i++)
    r->gainc[i] = v2__powf24(gaincdef[i], e);
  for (int32_t i = 0; i < 2; i++)
    r->gaina[i] = v2__powf24(gainadef[i], e);

  r->damp = r->inst->sr_linfreq * (para->highcut / 128.0f);
  r->gainin = para->vol / 128.0f;
  r->lowcut = r->inst->sr_linfreq * v2__sqr(v2__sqr(para->lowcut / 128.0f));
}

static void v2__reverb_render(v2__reverb *r, v2__st *dest, int32_t n)
{
  const float *inbuf = r->inst->aux1buf;

  for (int32_t i = 0; i < n; i++) {
    float in = inbuf[i] * r->gainin + v2__fcdcoffset;

    for (int32_t ch = 0; ch < 2; ch++) {
      /* parallel comb filters */
      float cur = 0.0f;
      for (int32_t j = 0; j < 4; j++) {
        float dv = r->gainc[j] * v2__delay_fetch(&r->combd[ch][j]);
        float nv = (j & 1) ? (dv - in) : (dv + in); /* alternate phase on combs */
        float lp = r->combl[ch][j] + r->damp * (nv - r->combl[ch][j]);
        r->combl[ch][j] = lp;
        v2__delay_feed(&r->combd[ch][j], lp);
        cur += lp;
      }

      /* serial allpass filters */
      for (int32_t j = 0; j < 2; j++) {
        float dv = v2__delay_fetch(&r->alld[ch][j]);
        float dz = cur + r->gaina[j] * dv;
        v2__delay_feed(&r->alld[ch][j], dz);
        cur = dv - r->gaina[j] * dz;
      }

      /* low cut and output */
      r->hpf[ch] += r->lowcut * (cur - r->hpf[ch]);
      dest[i].ch[ch] += cur - r->hpf[ch];
    }
  }
}

/* -------------------------------------------------------------------------- */
/*  Channel                                                                   */
/* -------------------------------------------------------------------------- */

enum { V2__FXR_DIST_THEN_CHORUS = 0, V2__FXR_CHORUS_THEN_DIST };

typedef struct v2__chan {
  float chgain;
  float a1gain, a2gain;
  float aasnd, absnd;
  float aarcv, abrcv;
  int32_t fxr;
  v2__dcfilter dcf1;
  v2__boost boost;
  v2__dist dist;
  v2__dcfilter dcf2;
  v2__moddel chorus;
  v2__comp comp;
  v2__inst *inst;
} v2__chan;

static void v2__chan_init(v2__chan *c, v2__inst *inst, float *delbuf1, float *delbuf2, uint32_t buflen)
{
  c->inst = inst;
  v2__dcfilter_init(&c->dcf1, inst);
  c->boost.inst = inst;
  v2__dist_init(&c->dist, inst);
  v2__dcfilter_init(&c->dcf2, inst);
  v2__moddel_init(&c->chorus, inst, delbuf1, delbuf2, buflen);
  v2__comp_init(&c->comp, inst);
}

static void v2__chan_set(v2__chan *c, const v2__p_chan_s *para)
{
  c->aarcv = para->auxarcv / 128.0f;
  c->abrcv = para->auxbrcv / 128.0f;
  c->aasnd = v2__fcgain * (para->auxasnd / 128.0f);
  c->absnd = v2__fcgain * (para->auxbsnd / 128.0f);
  c->chgain = v2__fcgain * (para->chanvol / 128.0f);
  /* the asm forms ((aux/128) * fcgainh) * chgain, chgain LAST */
  c->a1gain = (para->aux1 / 128.0f) * v2__fcgainh * c->chgain;
  c->a2gain = (para->aux2 / 128.0f) * v2__fcgainh * c->chgain;
  c->fxr = (int32_t)para->fxroute;
  v2__dist_set(&c->dist, &para->dist);
  v2__moddel_set(&c->chorus, &para->chorus);
  v2__comp_set(&c->comp, &para->comp);
  v2__boost_set(&c->boost, &para->boost);
}

static void v2__accumulate(v2__st *dest, const v2__st *src, int32_t n, float gain)
{
  if (gain == 0.0f)
    return;
  for (int32_t i = 0; i < n; i++) {
    dest[i].ch[0] += gain * src[i].ch[0];
    dest[i].ch[1] += gain * src[i].ch[1];
  }
}

static void v2__accumulate_monomix(float *dest, const v2__st *src, int32_t n, float gain)
{
  if (gain == 0.0f)
    return;
  for (int32_t i = 0; i < n; i++)
    dest[i] += gain * (src[i].ch[0] + src[i].ch[1]);
}

static void v2__chan_process(v2__chan *c, int32_t n, int muted, int32_t chan_index)
{
  v2__inst *inst = c->inst;
  v2__st *chan = inst->chanbuf;

  /* AuxA/B receive (stereo) */
  v2__accumulate(chan, inst->auxabuf, n, c->aarcv);
  v2__accumulate(chan, inst->auxbbuf, n, c->abrcv);

  v2__dcfilter_render_stereo(&c->dcf1, chan, chan, n);
  v2__comp_render(&c->comp, chan, n);
  v2__boost_render(&c->boost, chan, n);

  if (c->fxr == V2__FXR_DIST_THEN_CHORUS) {
    v2__dist_render_stereo(&c->dist, chan, chan, n);
    v2__dcfilter_render_stereo(&c->dcf2, chan, chan, n);
    v2__moddel_render_chan(&c->chorus, chan, n);
  } else {
    v2__moddel_render_chan(&c->chorus, chan, n);
    v2__dist_render_stereo(&c->dist, chan, chan, n);
    v2__dcfilter_render_stereo(&c->dcf2, chan, chan, n);
  }

  /* per-channel output meter (display only), measured regardless of mute */
  if (chan_index >= 0) {
    float pk = 0.0f;
    for (int32_t i = 0; i < n; i++) {
      float a = fabsf(chan[i].ch[0]);
      float b = fabsf(chan[i].ch[1]);
      float m = a > b ? a : b;
      if (m > pk) pk = m;
    }
    pk *= c->chgain;
    if (pk > inst->chan_peak[chan_index]) inst->chan_peak[chan_index] = pk;
  }

  /* Muted channels render fully but feed no output bus (not the mix, not the
     reverb/delay/aux sends), so unmuting resumes seamlessly and in phase. */
  if (!muted) {
    v2__accumulate_monomix(inst->aux1buf, chan, n, c->a1gain);
    v2__accumulate_monomix(inst->aux2buf, chan, n, c->a2gain);
    v2__accumulate(inst->auxabuf, chan, n, c->aasnd);
    v2__accumulate(inst->auxbbuf, chan, n, c->absnd);
    v2__accumulate(inst->mixbuf, chan, n, c->chgain);
  }
}

/* -------------------------------------------------------------------------- */
/*  Ronan: the speech synthesizer driving channel 15                          */
/*                                                                            */
/*  "ronan heisst in wirklichkeit lisa. ich war nur zu faul zum renamen." -kb */
/*  A formant synthesizer: a phoneme sequencer interpolates per-frame         */
/*  resonator coefficients that filter the channel's glottal source.          */
/* -------------------------------------------------------------------------- */

#if V2_RONAN

#define V2__NPHONEMES 69
#define V2__PTABSIZE  1311
#define V2__PHONEME_FIELDS (V2__PTABSIZE / V2__NPHONEMES) /* 19 */

typedef struct v2__phoneme_s {
  float f1f, f1b;
  float f2f, f2b;
  float f3f, f3b;
  float fnf;
  float a_voicing, a_aspiration, a_frication, a_bypass;
  float a_1, a_2, a_3, a_4, a_n, a_56;
  float duration, rank;
} v2__phoneme_s;

typedef union v2__phoneme { v2__phoneme_s s; float f[V2__PHONEME_FIELDS]; } v2__phoneme;
typedef char v2__chk_phoneme_size[(sizeof(v2__phoneme_s) == V2__PHONEME_FIELDS * sizeof(float)) ? 1 : -1];

/* Phonemes are stored delta-coded (per field, cumulative over the phoneme
   index) and scaled by multipliers[field]. */
static const uint8_t v2__ph_multipliers[V2__PHONEME_FIELDS] = {
  10, 10, 20, 10, 20, 10, 10, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1
};

static const uint8_t v2__ph_raw[V2__PTABSIZE] = {
  /* f1f */
    49, 30,241,  0, 15,226,226,  0,  0,  0, 21,235,  9,  0,247,  0, 45,217, 24,  9,238,235,  0,  0, 30,247,247, 18, 30,196,  9,247,  0,  0, 27,  0,  2,  0,  0, 13,244,  0,244,  0, 12, 30,214,238,  0,  0, 30,235, 21,  0,247,  0,235, 21,235,  0, 51,211,  3,247,  0,  6,  3,  0,  0,
  /* f1b */
   234,  7,249,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,254,  0, 12,246,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0, 14,242,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
  /* f2f */
    68, 15,247, 21,199,253,253,  0,  0, 51, 12,244,247,  0,  9,  0, 12, 15,214,253,  0,  3,  0,  0,  0, 30,  6,220,226, 45, 12,229,  0,  0,  0,229,  3, 39,208,  3, 30,223,  9,  0,247, 24,241,244,  0,  0, 36,253,244,  0, 27, 24,235,  0,  0,  0,235,232, 27,223, 36, 51,217, 15,241,
  /* f2b */
   179,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  8, 13,241,250,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
  /* f3f */
   116,  0,  0,  0,  0,  0,  0,  0,  0,  9,250,  6,250,  0,  6,  0,247, 35,221,  0,  3,  3,  0,  0,250,  3, 18,235,  0,  9,250,  3,  0,  0,250,  0,241, 21,  9,241,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  3,208,  0, 51,253,  6,  0,  0,  0,247,241, 18,229, 30, 18,235,  0,  0,
  /* f3b */
   143,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,253, 14,240,  5,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,248, 15,249,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
  /* fnf */
    12,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  9,  9,247,247,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
  /* a_voicing */
    35,  0,  0,  0,  0,  0,  0,  0,  0,194,  0, 62,230, 26,  0,  0,  0,  0,194, 62,194, 62,  0,  0,194, 62,  0,  0,  0,  0,  0,194,  0,  0, 62,  0,  0,  0,246, 10,  0,  0,  0,  0,  0,  0,  0,194,  0,  0,  0, 62,  0,244,206,  0,  0,  0,  0,  0, 62,  0,  0,  0,194, 62,234, 22,  0,
  /* a_aspiration */
   194,  0,  0,  0,  0,  0,  0,  0,  0, 60,  0,196,  0,  0,  0,  0,  0,  0,  0,  0, 32,224,  0,  0, 60,196,  0,  0,  0,  0,  0, 60,  0,  0,196,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0, 60,  0,  0,196,  0,  0,  0, 32, 28,  0,  0,  0,  0,196,  0,  0,  0, 60,196, 20,236,  0,
  /* a_frication */
     0,  0,  0,  0,  0,  0,  0,  0,  0, 60,  0,196, 60,196,  0,  0,  0,  0,  0,  0, 54,202,  0,  0, 60,196,  0,  0,  0,  0,  0, 60,  0,  0,196,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0, 60,  0,  0,196,  0,  0,  0, 60,  0,  0,  0,  0,  0,196,  0,  0,  0, 60,196, 60,196,  0,
  /* a_bypass */
   237,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0, 70,186,  0,  0,  0,  0,  0,  0, 70,186,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
  /* a_1 */
    83,  0,  0,  0,  0,  0,230,  0,202,  0,  0, 61,254,  0,  9,  0, 12,  0,176, 80,176, 65,  0,  0,  0, 15,  0,  0,  0,237,254,197,  0,  0, 66,  0,246,  9,241, 30,  0,  0,  0,  0,  0,  0,  0,176, 54,  0,202, 59, 13,  0,184,  0,  0,  0,  0,  0, 80,  0,235, 14,183, 80,235,  0,  0,
  /* a_2 */
    21,253,254,253,  7,252,181, 79,177,  0, 61,195, 61,  0,  7,246, 14,247,193, 75,211,226, 75,246,  1,  0,255, 15,255,177, 56,200, 80,246,242,  0,  4,  5,251, 17,  3,251,253,  0,  3,  2,251,184, 79,245,188, 70,251,  0,249,  3,195, 56,200,  0, 73,251,  2,244, 12,249,247,  2,254,
  /* a_3 */
     9,  5,253,  3,247,249,204, 73,183,  0, 72,184, 56,  0,  9,245, 14,254,190, 63,223,226, 70,245,253,  9,  1,253,252,197, 66,190, 80,246,242,  0,  7,  2,  0,243, 11,245,  6,  0,250, 13,249,198, 73,246,193, 66,255,  0,249, 14,184, 58, 10,246,  3,242, 19,241, 19,254,242, 12,244,
  /* a_4 */
     2,  5,254,  2,247,251,209, 68,188,  0, 61,195, 58,  0, 17,246,252,  0,195, 56,200,  0, 54,246,  8,  7,  2,251,252,204, 56,200, 59,246,  2,  0,205, 50,236, 15, 11,247,  5,  0,251, 11,250,204, 68,246,198, 63,193,  0, 70,247,195, 52, 28,246,240,242, 23,193, 49, 12,  5,246, 10,
  /* a_n */
   190,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0, 72,  0,  0,184,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0, 56,200,  0, 66,246, 10,246,  0,246,210,  0, 56,200, 46,210, 66,236,  0,
  /* a_56 */
   225,  0,  0,  0,  0,  0,  0,  0,  0, 46,246,  0, 10,236,246,240,  0,  0,  0,  0, 46,210,  0,  0, 26,230,  0,  0,  0, 26,246, 10,  0,246,240,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0, 46,236, 10,246,230,  0,  0,  0,  0,  0,  0,  0,  0, 26,  0,
  /* duration */
   235,  1,  1,  0,  9,251,  2,245,255,  4,  4,  0,252,  0,253,  0,  3,  3,254, 11,252,  0,245,  1,  3,  1,  0,  0,  0,254,255,  5,249,  3,  4,  0,  0,  0,  0,254,  0,  0,254,  2,  0,  0,  0,  2,249,  1,  4,250, 11,255,  2,  0,250,  9,242,  1,  4,  3,251,  4,  4,251,253,  0,  0,
  /* rank */
   254,  0,  0,  0,  0,  0, 24,  3,253,253,251,  8,250,  0,  9,253,232,  0, 29,227, 16,  8,  3,253,239,249,  0,  0,  0, 24,250,  3,  6,250,244,  0,  0,  0,  0,247,  0,  0,  0,  0,  0,  0,  0, 21,  6,250,  6,  1,236,  0,  8,  0,  5,251, 11,250,235,  0, 18,246,  8,248, 10,  0,  0,
};

/* syllable -> phoneme sequence (-1 terminated) */
typedef struct v2__syldef {
  char syl[4];
  int8_t ptab[4];
} v2__syldef;

static const v2__syldef v2__syls[] = {
  {"sil",{50,-1,-1,-1}}, {"ng",{38,-1,-1,-1}}, {"th",{57,-1,-1,-1}}, {"sh",{55,-1,-1,-1}},
  {"dh",{12,51,13,-1}}, {"zh",{67,51,67,-1}}, {"ch",{ 9,10,-1,-1}}, {"ih",{25,-1,-1,-1}},
  {"eh",{16,-1,-1,-1}}, {"ae",{ 1,-1,-1,-1}}, {"ah",{60,-1,-1,-1}}, {"oh",{39,-1,-1,-1}},
  {"uh",{42,-1,-1,-1}}, {"ax",{ 0,-1,-1,-1}}, {"iy",{17,-1,-1,-1}}, {"er",{19,-1,-1,-1}},
  {"aa",{ 4,-1,-1,-1}}, {"ao",{ 5,-1,-1,-1}}, {"uw",{61,-1,-1,-1}}, {"ey",{ 2,25,-1,-1}},
  {"ay",{28,25,-1,-1}}, {"oy",{41,25,-1,-1}}, {"aw",{45,46,-1,-1}}, {"ow",{40,46,-1,-1}},
  {"ia",{26,27,-1,-1}}, {"ea",{ 3,27,-1,-1}}, {"ua",{43,27,-1,-1}}, {"ll",{35,-1,-1,-1}},
  {"wh",{63,-1,-1,-1}}, {"ix",{ 0,-1,-1,-1}}, {"el",{34,-1,-1,-1}}, {"rx",{53,-1,-1,-1}},
  {"h",{24,-1,-1,-1}}, {"p",{47,48,49,-1}}, {"t",{56,58,59,-1}}, {"k",{31,32,33,-1}},
  {"b",{ 6, 7, 8,-1}}, {"d",{11,14,15,-1}}, {"g",{21,22,23,-1}}, {"m",{36,-1,-1,-1}},
  {"n",{37,-1,-1,-1}}, {"f",{20,-1,-1,-1}}, {"s",{54,-1,-1,-1}}, {"v",{62,51,62,-1}},
  {"z",{66,51,68,-1}}, {"l",{34,-1,-1,-1}}, {"r",{52,-1,-1,-1}}, {"w",{63,-1,-1,-1}},
  {"q",{51,-1,-1,-1}}, {"y",{65,-1,-1,-1}}, {"j",{29,30,51,30}}, {" ",{18,-1,-1,-1}},
};
#define V2__NSYLS ((int32_t)(sizeof(v2__syls) / sizeof(v2__syls[0])))

static const float v2__ron_pi = 3.1415926535897932384626433832795f;
static const float v2__ron_f4 = 3200, v2__ron_f5 = 4000, v2__ron_f6 = 6000;
static const float v2__ron_bn = 100, v2__ron_b4 = 200, v2__ron_b5 = 500, v2__ron_b6 = 800;
static const char  v2__ron_nix[] = "";

/* Returns the number of matched characters if the syllable `a` is a
   case-insensitive prefix of `b`, else 0. */
static int32_t v2__ron_prefix(const char *a, const char *b)
{
  int32_t l = 0;
  while (*a && *b) {
    if ((*a++ | 0x20) != (*b++ | 0x20))
      return 0;
    l++;
  }
  return *a ? 0 : l;
}

/* agner-fog x87 pow (fyl2x feeding a 2^frac*2^int split) on the owned cores */
static double v2__ron_pow(double a, double b)
{
  if (!(a > 0.0)) return 0.0;
  double t = b * v2__log2_core((float)a);
  double ti = trunc(t);
  return ldexp(v2__exp2_core(t - ti), (int)ti);
}

static double v2__ron_exp(double f)
{
  double t = f * 1.4426950408889634074; /* log2(e) */
  double ti = trunc(t);
  return ldexp(v2__exp2_core(t - ti), (int)ti);
}

static double v2__ron_cos(double x)
{
  int n = (int)lrint(x * v2__two_over_pi);
  double t = x - (double)n * v2__pio2a;
  t -= (double)n * v2__pio2b;
  t -= (double)n * v2__pio2c;
  switch (n & 3) {
  default:
  case 0: return v2__cos_poly(t);
  case 1: return -v2__sin_poly(t);
  case 2: return -v2__cos_poly(t);
  case 3: return v2__sin_poly(t);
  }
}

static float v2__ron_flerp(float a, float b, float x) { return a + x * (b - a); }

/* filter type 1: 2-pole resonator */
typedef struct v2__resdef { float a, b, c; } v2__resdef;

static void v2__resdef_set(v2__resdef *d, float f, float bw, float gain, float fcminuspi_sr, float fc2pi_sr)
{
  float r = (float)v2__ron_exp((double)(fcminuspi_sr * bw));
  d->c = -(r * r);
  d->b = r * (float)v2__ron_cos((double)(fc2pi_sr * f)) * 2.0f;
  d->a = gain * (1.0f - d->b - d->c);
}

typedef struct v2__resonator {
  v2__resdef *def;
  float p1, p2;
} v2__resonator;

static float v2__resonator_tick(v2__resonator *r, float in)
{
  float x = r->def->a * in + r->def->b * r->p1 + r->def->c * r->p2;
  r->p2 = r->p1;
  r->p1 = x;
  return x;
}

/* One interpolated parameter frame; indexed both by name and by position. */
typedef struct v2__ron_frame_s {
  v2__resdef rdef[7]; /* nasal, f1..f6 */
  float a_voicing, a_aspiration, a_frication, a_bypass;
} v2__ron_frame_s;

#define V2__RON_FRAME_FLOATS 25
typedef union v2__ron_frame { v2__ron_frame_s s; float f[V2__RON_FRAME_FLOATS]; } v2__ron_frame;
typedef char v2__chk_ron_frame_size[(sizeof(v2__ron_frame_s) == V2__RON_FRAME_FLOATS * sizeof(float)) ? 1 : -1];

/* everything reset() clears (the lab memsets the whole workspace) */
typedef struct v2__ron_state {
  v2__ron_frame cur;
  v2__ron_frame newframe;
  v2__resonator res[7];
  float lastin2;

  uint32_t nseed;
  float nout;

  int32_t framecount;  /* frame rate divider */
  int32_t spos;        /* position within the syllable (0..3) */
  int32_t scounter;    /* syllable duration divider */
  int32_t cursyl;
  int32_t durfactor;
  float invdur;
  const char *baseptr; /* start of text */
  const char *ptr;
  int32_t curp1, curp2; /* current/last phoneme */

  int32_t wait4on;
  int32_t wait4off;

  float hpb1, hpb2;
  v2__resonator peq1;
} v2__ron_state;

typedef struct v2__ronan {
  /* persistent across reset() */
  v2__phoneme phon[V2__NPHONEMES];
  const char *texts[64];
  float pitch;
  int32_t framerate;
  uint32_t samplerate;
  float fcminuspi_sr;
  float fc2pi_sr;
  v2__resdef d_peq1;

  v2__ron_state st;
} v2__ronan;

static float v2__ron_db2lin(float db1, float db2, float x)
{
  return (float)v2__ron_pow(2.0, (double)(v2__ron_flerp(db1, db2, x) - 70.0f) / 6.0);
}

static void v2__ron_setframe(const v2__ronan *r, const v2__phoneme *p1s, const v2__phoneme *p2s,
                             float x, v2__ron_frame *dest)
{
  const v2__phoneme_s *p1 = &p1s->s, *p2 = &p2s->s;

  const float p1f[7] = { p1->fnf, p1->f1f, p1->f2f, p1->f3f, v2__ron_f4, v2__ron_f5, v2__ron_f6 };
  const float p1b[7] = { v2__ron_bn, p1->f1b, p1->f2b, p1->f3b, v2__ron_b4, v2__ron_b5, v2__ron_b6 };
  const float p1a[7] = { p1->a_n, p1->a_1, p1->a_2, p1->a_3, p1->a_4, p1->a_56, p1->a_56 };

  const float p2f[7] = { p2->fnf, p2->f1f, p2->f2f, p2->f3f, v2__ron_f4, v2__ron_f5, v2__ron_f6 };
  const float p2b[7] = { v2__ron_bn, p2->f1b, p2->f2b, p2->f3b, v2__ron_b4, v2__ron_b5, v2__ron_b6 };
  const float p2a[7] = { p2->a_n, p2->a_1, p2->a_2, p2->a_3, p2->a_4, p2->a_56, p2->a_56 };

  for (int32_t i = 0; i < 7; i++)
    v2__resdef_set(&dest->s.rdef[i], v2__ron_flerp(p1f[i], p2f[i], x) * r->pitch,
                   v2__ron_flerp(p1b[i], p2b[i], x), v2__ron_db2lin(p1a[i], p2a[i], x),
                   r->fcminuspi_sr, r->fc2pi_sr);

  dest->s.a_voicing = v2__ron_db2lin(p1->a_voicing, p2->a_voicing, x);
  dest->s.a_aspiration = v2__ron_db2lin(p1->a_aspiration, p2->a_aspiration, x);
  dest->s.a_frication = v2__ron_db2lin(p1->a_frication, p2->a_frication, x);
  dest->s.a_bypass = v2__ron_db2lin(p1->a_bypass, p2->a_bypass, x);
}

static float v2__ron_noise(v2__ron_state *st)
{
  st->nseed = (st->nseed * 196314165u) + 907633515u;

  /* float in [2,4), slightly low-passed */
  float v = v2__bits2float((st->nseed >> 9) | 0x40000000u);
  st->nout = (v - 3.0f) + 0.75f * st->nout;
  return st->nout * 0.25f;
}

/* Zeroes all sequencer and DSP state; essential on a CC4 text select
   mid-song, or the phoneme sequencer would stay stalled. */
static void v2__ron_reset(v2__ronan *r)
{
  memset(&r->st, 0, sizeof r->st);

  for (int32_t i = 0; i < 7; i++) {
    r->st.res[i].def = &r->st.cur.s.rdef[i];
  }
  r->st.peq1.def = &r->d_peq1;
  v2__ron_setframe(r, &r->phon[18], &r->phon[18], 0, &r->st.cur);     /* off */
  v2__ron_setframe(r, &r->phon[18], &r->phon[18], 0, &r->st.newframe); /* off */
  r->st.curp1 = r->st.curp2 = 18;
  r->st.spos = 4;
}

static void v2__ron_set_sr(v2__ronan *r, int32_t sr)
{
  r->samplerate = (uint32_t)sr;
  r->fc2pi_sr = 2.0f * v2__ron_pi / (float)sr;
  r->fcminuspi_sr = -r->fc2pi_sr * 0.5f;
}

static void v2__ron_init(v2__ronan *r)
{
  /* decode the phoneme table */
  const uint8_t *ptr = v2__ph_raw;
  int32_t val = 0;
  for (int32_t f = 0; f < V2__PHONEME_FIELDS; f++)
    for (int32_t p = 0; p < V2__NPHONEMES; p++) {
      val += (int8_t)*ptr++;
      r->phon[p].f[f] = v2__ph_multipliers[f] * (float)val;
    }

  v2__ron_reset(r);

  r->framerate = 3;
  r->pitch = 1.0f;

  if (!r->texts[0])
    r->st.baseptr = r->st.ptr = v2__ron_nix;
  else
    r->st.baseptr = r->st.ptr = r->texts[0];

  r->st.lastin2 = 0;

  v2__resdef_set(&r->d_peq1, 12000, 4000, 2.0f, r->fcminuspi_sr, r->fc2pi_sr);
}

static void v2__ron_tick(v2__ronan *r)
{
  v2__ron_state *s = &r->st;

  if (s->wait4off || s->wait4on) return;
  if (!s->ptr) return;

  if (s->framecount <= 0) {
    s->framecount = r->framerate;
    /* let the current phoneme expire */
    if (s->scounter <= 0) {
      /* set to the next phoneme */
      s->spos++;
      if (s->spos >= 4 || v2__syls[s->cursyl].ptab[s->spos] == -1) {
        /* go to the next syllable */
        if (s->ptr == 0 || s->ptr[0] == 0) { /* empty text: silence */
          s->durfactor = 1;
          s->framecount = 1;
          s->cursyl = V2__NSYLS - 1;
          s->spos = 0;
          s->ptr = s->baseptr;
        } else if (*s->ptr == '!') { /* wait for note on */
          s->framecount = 0;
          s->scounter = 0;
          s->wait4on = 1;
          s->ptr++;
          return;
        } else if (*s->ptr == '_') { /* wait for note off */
          s->framecount = 0;
          s->scounter = 0;
          s->wait4off = 1;
          s->ptr++;
          return;
        }

        if (*s->ptr && *s->ptr != '!' && *s->ptr != '_') {
          uint32_t dur = 0;
          while (*s->ptr >= '0' && *s->ptr <= '9')
            dur = 10u * dur + (uint32_t)(*s->ptr++ - '0');
          s->durfactor = (int32_t)dur;
          if (!s->durfactor) s->durfactor = 1;

          int32_t fs, len = 1, len2;
          for (fs = 0; fs < V2__NSYLS - 1; fs++) {
            if ((len2 = v2__ron_prefix(v2__syls[fs].syl, s->ptr))) {
              len = len2;
              break;
            }
          }
          s->cursyl = fs;
          s->spos = 0;
          s->ptr += len;
        }
      }

      s->curp1 = s->curp2;
      s->curp2 = v2__syls[s->cursyl].ptab[s->spos];
      s->scounter = v2__fistp(r->phon[s->curp2].s.duration * (float)s->durfactor);
      if (!s->scounter) s->scounter = 1;
      s->invdur = 1.0f / ((float)s->scounter * (float)r->framerate);
    }
    s->scounter--;
  }

  s->framecount--;
  float x = (float)(int32_t)((uint32_t)s->scounter * (uint32_t)r->framerate + (uint32_t)s->framecount) * s->invdur;
  const v2__phoneme_s *p1 = &r->phon[s->curp1].s;
  const v2__phoneme_s *p2 = &r->phon[s->curp2].s;
  x = (float)v2__ron_pow((double)x, (double)(p1->rank / p2->rank));
  v2__ron_setframe(r, &r->phon[s->curp2],
                   (fabsf(p2->rank - p1->rank) > 8.0f) ? &r->phon[s->curp2] : &r->phon[s->curp1],
                   x, &s->newframe);
}

static void v2__ron_noteon(v2__ronan *r)  { r->st.wait4on = 0; }
static void v2__ron_noteoff(v2__ronan *r) { r->st.wait4off = 0; }

static void v2__ron_setctl(v2__ronan *r, uint32_t ctl, uint32_t val)
{
  /* controller 4, 0-62: select text; 63-127: frame rate; controller 5: pitch */
  switch (ctl) {
  case 4:
    if (val < 63) {
      v2__ron_reset(r);
      if (r->texts[val])
        r->st.ptr = r->st.baseptr = r->texts[val];
      else
        r->st.ptr = r->st.baseptr = v2__ron_nix;
    } else
      r->framerate = (int32_t)val - 63;
    break;
  case 5:
    r->pitch = (float)v2__ron_pow(2.0, (double)(((float)val - 64.0f) / 128.0f));
    break;
  }
}

/* Processes `len` interleaved stereo frames in place; the glottal source is
   taken from the left channel and the result is written to both. */
static void v2__ron_process(v2__ronan *r, float *buf, uint32_t len)
{
  v2__ron_state *s = &r->st;
  v2__ron_frame delta;

  {
    float mul = 1.0f / (float)len;
    for (uint32_t i = 0; i < V2__RON_FRAME_FLOATS; i++)
      delta.f[i] = (s->newframe.f[i] - s->cur.f[i]) * mul;
  }

  for (uint32_t i = 0; i < len; i++) {
    for (uint32_t j = 0; j < V2__RON_FRAME_FLOATS; j++)
      s->cur.f[j] += delta.f[j];

    float in = buf[2 * i];

    /* aspiration noise */
    in = in * s->cur.s.a_voicing + v2__ron_noise(s) * s->cur.s.a_aspiration;

    /* whole input through the nasal and f1 filters */
    float out = v2__resonator_tick(&s->res[0], in) + v2__resonator_tick(&s->res[1], in);

    /* differentiate, add frication noise */
    float lin = in;
    in = (v2__ron_noise(s) * s->cur.s.a_frication) + in - s->lastin2;
    s->lastin2 = lin;

    /* f2..f6 and bypass (phase inverted) */
    for (int32_t k = 2; k < 7; k++)
      out = v2__resonator_tick(&s->res[k], in) - out;

    out = in * s->cur.s.a_bypass - out;

    /* high pass filter */
    out = out - s->hpb1; s->hpb1 += 0.012f * out;
    out = out - s->hpb2; s->hpb2 += 0.012f * out;

    /* EQ */
    out = v2__resonator_tick(&s->peq1, out) - out;

    buf[2 * i] = buf[2 * i + 1] = out;
  }
}

static void v2__ron_set_lyrics(v2__ronan *r, const char *const *texts)
{
  for (int32_t i = 0; i < 64; i++) r->texts[i] = texts[i];
  r->st.baseptr = r->st.ptr = r->texts[0];
}

#else /* !V2_RONAN: channel 15 is mixed unprocessed */

typedef struct v2__ronan { char unused; } v2__ronan;
static void v2__ron_set_sr(v2__ronan *r, int32_t sr) { (void)r; (void)sr; }
static void v2__ron_init(v2__ronan *r) { (void)r; }
static void v2__ron_tick(v2__ronan *r) { (void)r; }
static void v2__ron_noteon(v2__ronan *r) { (void)r; }
static void v2__ron_noteoff(v2__ronan *r) { (void)r; }
static void v2__ron_setctl(v2__ronan *r, uint32_t ctl, uint32_t val) { (void)r; (void)ctl; (void)val; }
static void v2__ron_process(v2__ronan *r, float *buf, uint32_t len) { (void)r; (void)buf; (void)len; }
static void v2__ron_set_lyrics(v2__ronan *r, const char *const *texts) { (void)r; (void)texts; }

#endif /* V2_RONAN */

/* -------------------------------------------------------------------------- */
/*  Synth engine: MIDI handling, voice allocation, frame rendering            */
/* -------------------------------------------------------------------------- */

/* little-endian reads: the v2m format is little-endian, regardless of host */
static inline uint32_t v2__rd32(const uint8_t *p)
{
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* Patch layout: voice[59] chan[29] maxpoly modnum { source val dest }[modnum] */
#define V2__PATCH_CHAN_OFF    V2__NVOICEPARM
#define V2__PATCH_MAXPOLY_OFF (V2__NVOICEPARM + V2__NCHANPARM)
#define V2__PATCH_MODNUM_OFF  (V2__PATCH_MAXPOLY_OFF + 1)
#define V2__PATCH_MODS_OFF    (V2__PATCH_MODNUM_OFF + 1)

typedef struct v2__chaninfo {
  uint8_t pgm;
  uint8_t ctl[7];
} v2__chaninfo;

typedef struct v2__engine {
  const uint8_t *patches;   /* patch block of the song: offset table, then patches */
  int32_t npatches;
  uint32_t mrstat;          /* MIDI running status */
  uint32_t curalloc;
  int32_t samplerate;
  int32_t chanmap[V2__POLY];   /* voice -> channel, -1 free */
  uint32_t allocpos[V2__POLY];
  int32_t voicemap[V2__CHANS]; /* channel -> last voice */
  int32_t tickd;               /* finished samples left in the mix buffer */

  v2__chaninfo chans[V2__CHANS];
  v2__p_voice voicesv[V2__POLY];
  v2__voice voicesw[V2__POLY];
  v2__p_chan chansv[V2__CHANS];
  v2__chan chansw[V2__CHANS];

  v2__p_globals globals;

  v2__reverb reverb;
  v2__moddel delay;
  v2__dcfilter dcf;
  v2__comp compr;
  float lcfreq;    /* low cut */
  float lcbuf[2];
  float hcfreq;    /* high cut */
  float hcbuf[2];

  int initialized;

  float maindelbuf[2][32768];
  float chandelbuf[V2__CHANS][2][2048];

  v2__inst inst;
  v2__ronan ronan;
} v2__engine;

static const uint8_t *v2__engine_patch(const v2__engine *e, int32_t pgm)
{
  if (pgm < 0 || pgm >= e->npatches)
    return NULL;
  return e->patches + v2__rd32(e->patches + 4 * (size_t)pgm);
}

static void v2__engine_init(v2__engine *e, const uint8_t *patches, int32_t npatches, int32_t samplerate)
{
  int32_t i;

  /* the asm zeroes the WHOLE instance */
  memset(e, 0, sizeof *e);

  v2__rand_init(&e->inst.rng, 1);
  e->inst.user_seed = 0;

  e->samplerate = samplerate;
  v2__inst_calc_sample_rate(&e->inst, samplerate);
  v2__ron_set_sr(&e->ronan, samplerate);

  e->patches = patches;
  e->npatches = npatches;

  for (i = 0; i < V2__POLY; i++) {
    e->chanmap[i] = -1;
    v2__voice_init(&e->voicesw[i], &e->inst);
  }

  for (i = 0; i < V2__CHANS; i++) {
    e->chans[i].ctl[6] = 0x7f;
    v2__chan_init(&e->chansw[i], &e->inst, e->chandelbuf[i][0], e->chandelbuf[i][1], 2048);
  }

  v2__reverb_init(&e->reverb, &e->inst);
  v2__moddel_init(&e->delay, &e->inst, e->maindelbuf[0], e->maindelbuf[1], 32768);
  v2__ron_init(&e->ronan);
  v2__comp_init(&e->compr, &e->inst);
  v2__dcfilter_init(&e->dcf, &e->inst);

  e->initialized = 1;
}

static void v2__engine_set_globals(v2__engine *e, const uint8_t *para)
{
  if (!e->initialized)
    return;

  for (int32_t i = 0; i < V2__NGLOBPARM; i++)
    e->globals.f[i] = (float)para[i];

  v2__reverb_set(&e->reverb, &e->globals.s.rvb);
  v2__moddel_set(&e->delay, &e->globals.s.del);
  v2__comp_set(&e->compr, &e->globals.s.cpr);
  e->lcfreq = v2__sqr((e->globals.s.vlowcut + 1.0f) / 128.0f);
  e->hcfreq = v2__sqr((e->globals.s.vhighcut + 1.0f) / 128.0f);
}

static float v2__engine_modsource(const v2__engine *e, const v2__voice *voice, int32_t chan, int32_t source)
{
  switch (source) {
  case 0: /* velocity */
    return voice->velo;
  case 1: case 2: case 3: case 4: case 5: case 6: case 7: /* controller */
    return (float)e->chans[chan].ctl[source - 1];
  case 8: case 9: /* EG output */
    return voice->env[source - 8].out;
  case 10: case 11: /* LFO output */
    return voice->lfo[source - 10].out;
  default: /* note */
    return 2.0f * ((float)voice->note - 48.0f);
  }
}

static void v2__engine_store_voice(v2__engine *e, int32_t vind)
{
  int32_t chan = e->chanmap[vind];
  if (chan < 0)
    return;

  const uint8_t *patch = v2__engine_patch(e, e->chans[chan].pgm);
  if (!patch)
    return;

  v2__p_voice *vpara = &e->voicesv[vind];
  v2__voice *voice = &e->voicesw[vind];
  int32_t i;

  for (i = 0; i < V2__NVOICEPARM; i++)
    vpara->f[i] = (float)patch[i];

  /* modulation matrix */
  const int32_t modnum = patch[V2__PATCH_MODNUM_OFF];
  const uint8_t *mods = patch + V2__PATCH_MODS_OFF;
  for (i = 0; i < modnum; i++) {
    const uint8_t *mod = mods + 3 * i; /* source, val, dest */
    if (mod[2] >= V2__NVOICEPARM)
      continue;

    float scale = ((float)mod[1] - 64.0f) / 64.0f;
    vpara->f[mod[2]] = v2__clampf(vpara->f[mod[2]] + scale * v2__engine_modsource(e, voice, chan, mod[0]),
                                  0.0f, 128.0f);
  }

  v2__voice_set(voice, &vpara->s);
}

static void v2__engine_store_chan(v2__engine *e, int32_t chan)
{
  const uint8_t *patch = v2__engine_patch(e, e->chans[chan].pgm);
  if (!patch)
    return;

  v2__p_chan *cpara = &e->chansv[chan];
  const v2__voice *voice = &e->voicesw[e->voicemap[chan]];
  int32_t i;

  for (i = 0; i < V2__NCHANPARM; i++)
    cpara->f[i] = (float)patch[V2__PATCH_CHAN_OFF + i];

  const int32_t modnum = patch[V2__PATCH_MODNUM_OFF];
  const uint8_t *mods = patch + V2__PATCH_MODS_OFF;
  for (i = 0; i < modnum; i++) {
    const uint8_t *mod = mods + 3 * i;
    int32_t dest = (int32_t)mod[2] - V2__NVOICEPARM;
    if (dest < 0 || dest >= V2__NCHANPARM)
      continue;

    float scale = ((float)mod[1] - 64.0f) / 64.0f;
    cpara->f[dest] = v2__clampf(cpara->f[dest] + scale * v2__engine_modsource(e, voice, chan, mod[0]),
                                0.0f, 128.0f);
  }

  v2__chan_set(&e->chansw[chan], &cpara->s);
}

static void v2__engine_render_frame(v2__engine *e, int32_t n)
{
  v2__inst *inst = &e->inst;
  int32_t chan, voice;

  memset(inst->mixbuf, 0, (size_t)n * sizeof(v2__st));
  memset(inst->aux1buf, 0, (size_t)n * sizeof(float));
  memset(inst->aux2buf, 0, (size_t)n * sizeof(float));
  memset(inst->auxabuf, 0, (size_t)n * sizeof(v2__st));
  memset(inst->auxbbuf, 0, (size_t)n * sizeof(v2__st));

  for (chan = 0; chan < V2__CHANS; chan++) {
    /* any voices active on this channel? */
    voice = 0;
    while (voice < V2__POLY && e->chanmap[voice] != chan)
      voice++;
    if (voice == V2__POLY)
      continue;

    memset(inst->chanbuf, 0, (size_t)n * sizeof(v2__st));

    for (; voice < V2__POLY; voice++) {
      if (e->chanmap[voice] != chan)
        continue;
      v2__voice_render(&e->voicesw[voice], inst->chanbuf, n);
    }

    /* channel 15 -> Ronan (speech vocal tract) */
    if (chan == V2__CHANS - 1)
      v2__ron_process(&e->ronan, inst->chanbuf[0].ch, (uint32_t)n);

    v2__chan_process(&e->chansw[chan], n, ((inst->chan_mute >> chan) & 1) != 0, chan);
  }

  /* global filters */
  v2__st *mix = inst->mixbuf;
  v2__reverb_render(&e->reverb, mix, n);
  v2__moddel_render_aux2main(&e->delay, mix, n);
  v2__dcfilter_render_stereo(&e->dcf, mix, mix, n);

  /* low cut / high cut */
  const float lcf = e->lcfreq, hcf = e->hcfreq;
  for (int32_t i = 0; i < n; i++) {
    for (int32_t ch = 0; ch < 2; ch++) {
      float x = mix[i].ch[ch] - e->lcbuf[ch];
      e->lcbuf[ch] += lcf * x;

      if (hcf != 1.0f) {
        e->hcbuf[ch] += hcf * (x - e->hcbuf[ch]);
        x = e->hcbuf[ch];
      }

      mix[i].ch[ch] = x;
    }
  }

  /* sum compressor */
  v2__comp_render(&e->compr, mix, n);
}

static void v2__engine_tick(v2__engine *e)
{
  int32_t i;

  for (i = 0; i < V2__POLY; i++) {
    if (e->chanmap[i] < 0)
      continue;

    v2__engine_store_voice(e, i);
    v2__voice_tick(&e->voicesw[i]);

    /* EG1 finished: free the voice */
    if (e->voicesw[i].env[0].state == V2__ENV_OFF)
      e->chanmap[i] = -1;
  }

  for (i = 0; i < V2__CHANS; i++)
    v2__engine_store_chan(e, i);

  v2__ron_tick(&e->ronan);
  e->tickd = e->inst.sr_framesize;
  v2__engine_render_frame(e, e->inst.sr_framesize);
}

/* Renders interleaved stereo; chunk-size invariant. */
static void v2__engine_render(v2__engine *e, float *buf, int32_t nsamples)
{
  int32_t todo = nsamples;

  while (todo) {
    if (!e->tickd)
      v2__engine_tick(e);

    const v2__st *src = &e->inst.mixbuf[e->inst.sr_framesize - e->tickd];
    int32_t nread = v2__mini(todo, e->tickd);
    memcpy(buf, src, (size_t)nread * sizeof(v2__st));
    buf += 2 * nread;

    todo -= nread;
    e->tickd -= nread;
  }
}

/* Message data bytes by (status >> 4) & 7: off, on, aftertouch, control,
   program, channel pressure, bend, system. */
static const uint8_t v2__midi_len[8] = { 2, 2, 1, 2, 1, 2, 2, 0 };

/* Processes a MIDI stream ending at `end` or at the first 0xFD byte. */
static void v2__engine_midi(v2__engine *e, const uint8_t *cmd, const uint8_t *end)
{
  while (cmd < end && *cmd != 0xfd) {
    if (*cmd & 0x80)
      e->mrstat = *cmd++;

    if (e->mrstat < 0x80)
      break;

    const int32_t chan = (int32_t)(e->mrstat & 0xf);
    const int32_t type = (int32_t)((e->mrstat >> 4) & 7);
    if ((size_t)(end - cmd) < v2__midi_len[type])
      break;

    switch (type) {
    case 1: /* note on */
      if (cmd[1] != 0) {
        if (chan == V2__CHANS - 1)
          v2__ron_noteon(&e->ronan);

        const uint8_t *sound = v2__engine_patch(e, e->chans[chan].pgm);
        if (!sound) {
          cmd += 2;
          break;
        }

        /* current polyphony of this channel */
        int32_t npoly = 0, i;
        for (i = 0; i < V2__POLY; i++)
          npoly += (e->chanmap[i] == chan);

        /* Voice allocation. Below the polyphony limit take any free voice;
           at the limit only this channel's own voices may be stolen. */
        int32_t usevoice = -1;
        int32_t chanmask, chanfind;

        if (!npoly || npoly < (int32_t)sound[V2__PATCH_MAXPOLY_OFF]) {
          for (i = 0; i < V2__POLY; i++) {
            if (e->chanmap[i] < 0) {
              usevoice = i;
              break;
            }
          }
          chanmask = 0;
          chanfind = 0;
        } else {
          /* the asm compares the FULL chanmap entry, so free voices (-1)
             are never "this channel's own" */
          chanmask = ~0;
          chanfind = chan;
        }

        /* no voice yet: kill the oldest eligible one with its gate off */
        if (usevoice < 0) {
          uint32_t oldest = e->curalloc;
          for (i = 0; i < V2__POLY; i++) {
            if ((e->chanmap[i] & chanmask) == chanfind && !e->voicesw[i].gate && e->allocpos[i] < oldest) {
              oldest = e->allocpos[i];
              usevoice = i;
            }
          }
        }

        /* still none: take the oldest one, period */
        if (usevoice < 0) {
          uint32_t oldest = e->curalloc;
          for (i = 0; i < V2__POLY; i++) {
            if ((e->chanmap[i] & chanmask) == chanfind && e->allocpos[i] < oldest) {
              oldest = e->allocpos[i];
              usevoice = i;
            }
          }
        }

        if (usevoice < 0) { /* unreachable with a consistent allocation counter */
          cmd += 2;
          break;
        }

        e->chanmap[usevoice] = chan;
        e->voicemap[chan] = usevoice;
        e->allocpos[usevoice] = e->curalloc++;

        v2__engine_store_voice(e, usevoice);
        v2__voice_noteon(&e->voicesw[usevoice], cmd[0], cmd[1]);
        cmd += 2;
        break;
      }
      /* velocity 0 is a note off */
      /* fall through */

    case 0: /* note off */
      if (chan == V2__CHANS - 1)
        v2__ron_noteoff(&e->ronan);

      for (int32_t i = 0; i < V2__POLY; i++) {
        if (e->chanmap[i] != chan)
          continue;

        v2__voice *voice = &e->voicesw[i];
        if (voice->note == cmd[0] && voice->gate) {
          voice->gate = 0;
          /* the asm stops at the FIRST matching voice */
          break;
        }
      }
      cmd += 2;
      break;

    case 2: /* aftertouch: ignored */
      cmd++;
      break;

    case 3: { /* control change */
      int32_t ctrl = cmd[0];
      uint8_t val = cmd[1];
      if (ctrl >= 1 && ctrl <= 7) {
        e->chans[chan].ctl[ctrl - 1] = val;
        if (chan == V2__CHANS - 1)
          v2__ron_setctl(&e->ronan, (uint32_t)ctrl, val);
      } else if (ctrl == 120) { /* all sound off */
        for (int32_t i = 0; i < V2__POLY; i++) {
          if (e->chanmap[i] != chan)
            continue;
          v2__voice_init(&e->voicesw[i], &e->inst);
          e->chanmap[i] = -1;
        }
      } else if (ctrl == 123) { /* all notes off */
        if (chan == V2__CHANS - 1)
          v2__ron_noteoff(&e->ronan);
        for (int32_t i = 0; i < V2__POLY; i++)
          if (e->chanmap[i] == chan)
            e->voicesw[i].gate = 0;
      }
      cmd += 2;
      break;
    }

    case 4: { /* program change */
      uint8_t pgm = *cmd++ & 0x7f;
      /* only a real change kills the channel's voices */
      if (e->chans[chan].pgm != pgm) {
        e->chans[chan].pgm = pgm;
        for (int32_t i = 0; i < V2__POLY; i++)
          if (e->chanmap[i] == chan)
            e->chanmap[i] = -1;
      }
      for (int32_t i = 0; i < 6; i++)
        e->chans[chan].ctl[i] = 0;
      break;
    }

    case 5: /* channel pressure: ignored */
    case 6: /* bend: ignored */
      cmd += 2;
      break;

    case 7: /* system */
      if (chan == 0xf) /* reset */
        v2__engine_init(e, e->patches, e->npatches, e->samplerate);
      while (cmd < end && !(*cmd & 0x80)) /* skip stray data bytes */
        cmd++;
      break;
    }
  }
}

/* -------------------------------------------------------------------------- */
/*  Loader: container parsing, version detection, v6 validation               */
/*                                                                            */
/*  Layout: timediv maxtime gdnum | global events (10 B each) | 16 channels   */
/*  (notenum? notes pc pb 7 ctl streams) | globals | patches | speech         */
/* -------------------------------------------------------------------------- */

#define V2__NUM_PATCH_PARMS  89  /* patch parameter count of format v6 */
#define V2__MAX_FORMAT_VER   6
#define V2__MAX_SONG_BYTES   ((size_t)1 << 30)

/* sounddef.h v2parms[i].version: a parameter is present in a format-v file iff
   its entry is <= v. Needed to fingerprint the version of a file. */
static const uint8_t v2__patch_parm_ver[V2__NUM_PATCH_PARMS] = {
  0, 2,                      /* voice: panning, transpose */
  0, 2, 0, 0, 0, 0,          /* osc 1: mode, ringmod, transpose, detune, color, volume */
  0, 2, 0, 0, 0, 0,          /* osc 2 */
  0, 2, 0, 0, 0, 0,          /* osc 3 */
  0, 0, 0,                   /* vcf 1: mode, cutoff, reso */
  0, 0, 0,                   /* vcf 2 */
  0, 3,                      /* routing, balance */
  0, 0, 0, 0,                /* voice dist */
  0, 0, 0, 0, 0, 0,          /* amp EG */
  0, 0, 0, 0, 0, 0,          /* EG 2 */
  0, 0, 0, 0, 0, 5, 0,       /* LFO 1: ..., polarity (v5), amplify */
  0, 0, 0, 0, 0, 5, 0,       /* LFO 2 */
  0, 0, 6, 6, 6, 6, 0, 0, 0, 1, /* keysync, chanvol, aux recv/send (v6), reverb, delay, fxroute, boost */
  0, 0, 0, 0,                /* channel dist */
  0, 0, 0, 0, 0, 0, 0,       /* chorus/flanger */
  1, 1, 1, 1, 1, 1, 1, 1, 1, /* compressor */
  0,                         /* maxpoly */
};

#define V2__NUM_GLOBAL_PARMS 23
static const uint8_t v2__global_parm_ver[V2__NUM_GLOBAL_PARMS] = {
  0, 0, 4, 0,               /* reverb: time, highcut, lowcut (v4), volume */
  0, 0, 0, 0, 0, 0, 0,      /* stereo delay */
  0, 0,                     /* post filters */
  1, 1, 1, 1, 1, 1, 1, 1, 1, /* sum compressor */
  6,                        /* gui features */
};

static int32_t v2__patch_parm_count(int32_t ver)
{
  int32_t n = 0;
  for (int32_t i = 0; i < V2__NUM_PATCH_PARMS; i++)
    if (v2__patch_parm_ver[i] <= ver) n++;
  return n;
}

static int32_t v2__global_size(int32_t ver)
{
  int32_t n = 0;
  for (int32_t i = 0; i < V2__NUM_GLOBAL_PARMS; i++)
    if (v2__global_parm_ver[i] <= ver) n++;
  return n;
}

typedef struct v2__container {
  size_t len;
  uint32_t timediv, maxtime, gdnum;
  size_t gptr;                 /* global events */
  struct v2__cchan {
    uint32_t notenum, pcnum, pbnum;
    size_t noteptr, pcptr, pbptr;
    struct { uint32_t ccnum; size_t ccptr; } ctl[7];
  } chan[V2__CHANS];
  uint32_t globsize;
  size_t globpos;
  uint32_t patchsize;
  size_t patchpos;
  uint32_t spsize;             /* speech block, 0 if absent */
  size_t sppos;
  int32_t maxp;                /* highest used patch + 1 */
} v2__container;

/* bounds-checked cursor */
typedef struct v2__cursor {
  const uint8_t *p;
  size_t pos, len;
  int ok;
} v2__cursor;

static int v2__cur_skip(v2__cursor *c, uint64_t n)
{
  if (!c->ok || (uint64_t)(c->len - c->pos) < n)
    c->ok = 0;
  else
    c->pos += (size_t)n;
  return c->ok;
}

static uint32_t v2__cur_u32(v2__cursor *c)
{
  if (!c->ok || c->len - c->pos < 4) {
    c->ok = 0;
    return 0;
  }
  uint32_t v = v2__rd32(c->p + c->pos);
  c->pos += 4;
  return v;
}

static int v2__parse_container(const uint8_t *d, size_t len, v2__container *c)
{
  v2__cursor cur;
  cur.p = d; cur.pos = 0; cur.len = len; cur.ok = 1;
  memset(c, 0, sizeof *c);
  c->len = len;

  c->timediv = v2__cur_u32(&cur);
  c->maxtime = v2__cur_u32(&cur);
  c->gdnum = v2__cur_u32(&cur);
  if (!cur.ok) return 0;
  c->gptr = cur.pos;
  if (!v2__cur_skip(&cur, (uint64_t)10 * c->gdnum)) return 0;

  for (int32_t ch = 0; ch < V2__CHANS; ch++) {
    struct v2__cchan *cc = &c->chan[ch];
    cc->notenum = v2__cur_u32(&cur);
    if (!cur.ok) return 0;
    if (!cc->notenum)
      continue; /* an empty channel ends after its note count */
    cc->noteptr = cur.pos;
    if (!v2__cur_skip(&cur, (uint64_t)5 * cc->notenum)) return 0;
    cc->pcnum = v2__cur_u32(&cur);
    cc->pcptr = cur.pos;
    if (!v2__cur_skip(&cur, (uint64_t)4 * cc->pcnum)) return 0;
    cc->pbnum = v2__cur_u32(&cur);
    cc->pbptr = cur.pos;
    if (!v2__cur_skip(&cur, (uint64_t)5 * cc->pbnum)) return 0;
    for (int32_t cn = 0; cn < 7; cn++) {
      cc->ctl[cn].ccnum = v2__cur_u32(&cur);
      cc->ctl[cn].ccptr = cur.pos;
      if (!v2__cur_skip(&cur, (uint64_t)4 * cc->ctl[cn].ccnum)) return 0;
    }
  }

  c->globsize = v2__cur_u32(&cur);
  if (!cur.ok || c->globsize > 131072) return 0;
  c->globpos = cur.pos;
  if (!v2__cur_skip(&cur, c->globsize)) return 0;

  c->patchsize = v2__cur_u32(&cur);
  if (!cur.ok || c->patchsize == 0 || c->patchsize > 1048576) return 0;
  c->patchpos = cur.pos;
  if (!v2__cur_skip(&cur, c->patchsize)) return 0;

  /* optional trailing speech block; implausible sizes are ignored */
  if (cur.pos < cur.len) {
    c->spsize = v2__cur_u32(&cur);
    c->sppos = cur.pos;
    if (!cur.ok || c->spsize > 8192 || !v2__cur_skip(&cur, c->spsize)) {
      c->spsize = 0;
      c->sppos = 0;
    }
  }
  return 1;
}

/* Highest used patch + 1, from the per-channel program-change streams. Event
   times are stored struct-of-arrays (all low bytes, then mid, then high). */
static int32_t v2__count_used_patches(const uint8_t *d, const v2__container *c)
{
  int32_t maxp = 0;
  for (int32_t ch = 0; ch < V2__CHANS; ch++) {
    const struct v2__cchan *cc = &c->chan[ch];
    if (!cc->notenum)
      continue;

    uint8_t p = 0;
    int32_t pct = 0, np = 0, nt = 0;
    const int32_t nn = (int32_t)cc->notenum, pcn = (int32_t)cc->pcnum;
    const uint8_t *pcptr = d + cc->pcptr;
    const uint8_t *noteptr = d + cc->noteptr;

    for (int32_t pcp = 0; pcp < pcn; pcp++) {
      int32_t td = 0x10000 * pcptr[2 * pcn + pcp] + 0x100 * pcptr[1 * pcn + pcp] + pcptr[0 * pcn + pcp];
      pct = (int32_t)((uint32_t)pct + (uint32_t)td);
      if (pct > nt) {
        if (p >= maxp) maxp = p + 1;
      }
      p = (uint8_t)(p + pcptr[3 * pcn + pcp]);

      while (np < nn && nt <= pct) {
        td = 0x10000 * noteptr[2 * nn + np] + 0x100 * noteptr[1 * nn + np] + noteptr[0 * nn + np];
        nt = (int32_t)((uint32_t)nt + (uint32_t)td);
        np++;
      }
    }

    if (np < nn) {
      if (p >= maxp) maxp = p + 1;
    }
  }
  return maxp;
}

/* Match (globals size, per-patch size consistency) against the format tables.
   Returns the detected version or -1. */
static int32_t v2__detect_version(const uint8_t *d, const v2__container *c)
{
  const uint8_t *patchmap = d + c->patchpos;
  int32_t best = -1, matches = 0;

  for (int32_t v = 0; v <= V2__MAX_FORMAT_VER; v++) {
    if ((int32_t)c->globsize != v2__global_size(v))
      continue;

    const int32_t parm_count = v2__patch_parm_count(v);
    int32_t p;
    for (p = 0; p < c->maxp - 1; p++) {
      if ((size_t)(p + 2) * 4 > c->patchsize) { p = -1; break; }
      uint32_t o0 = v2__rd32(patchmap + 4 * (size_t)p);
      uint32_t o1 = v2__rd32(patchmap + 4 * ((size_t)p + 1));
      if (o1 <= o0 || o1 > c->patchsize) { p = -1; break; }
      int32_t dd = (int32_t)(o1 - o0) - (parm_count + 1);
      if (dd < 0 || dd % 3) break;
      dd /= 3;
      if ((size_t)o0 + (size_t)parm_count >= c->patchsize) { p = -1; break; }
      if (dd != patchmap[o0 + (uint32_t)parm_count]) /* modnum byte follows the params */
        break;
    }
    if (p == c->maxp - 1) {
      best = v;
      matches++;
    }
  }
  return (matches >= 1) ? best : -1;
}

/* The engine indexes patches straight out of the song data; make sure every
   patch the 7-bit program numbers can reach lies inside the patch block.
   Returns the usable patch count (0 = invalid). */
static int32_t v2__validate_patches(const uint8_t *patchmap, uint32_t patchsize)
{
  if (patchsize < 4)
    return 0;
  uint32_t n = v2__rd32(patchmap) / 4; /* the offset table size implies the count */
  if (n == 0 || (uint64_t)n * 4 > patchsize)
    return 0;
  if (n > 128)
    n = 128;
  for (uint32_t p = 0; p < n; p++) {
    uint32_t off = v2__rd32(patchmap + 4 * (size_t)p);
    if (off < n * 4 || off > patchsize || patchsize - off < V2__PATCH_MODS_OFF)
      return 0;
    uint32_t modnum = patchmap[off + V2__PATCH_MODNUM_OFF];
    if (patchsize - off - V2__PATCH_MODS_OFF < 3 * modnum)
      return 0;
  }
  return (int32_t)n;
}

/* -------------------------------------------------------------------------- */
/*  Sequencer (port of kb's V2MPlayer)                                        */
/*                                                                            */
/*  Song data is addressed by offsets into the owned song copy and read       */
/*  through a bounds-checked accessor; the original's field quirks (pitch     */
/*  bends indexed by the program-change count) are kept.                      */
/* -------------------------------------------------------------------------- */

enum { V2__SEQ_OFF = 0, V2__SEQ_STOPPED, V2__SEQ_PLAYING };

typedef struct v2__seq_chan_state {
  size_t noteptr; uint32_t notenr, notent; uint8_t lastnte, lastvel;
  size_t pcptr;   uint32_t pcnr, pcnt;     uint8_t lastpc;
  size_t pbptr;   uint32_t pbnr, pbnt;     uint8_t lastpb0, lastpb1;
  struct { size_t ccptr; uint32_t ccnt, ccnr; uint8_t lastcc; } ctl[7];
} v2__seq_chan_state;

typedef struct v2__seq_state {
  int32_t state;
  uint32_t time;
  uint32_t nexttime;
  uint32_t gnt, gnr;
  uint32_t usecs;
  uint32_t num, den, tpq;
  uint32_t bar, beat, tick;
  v2__seq_chan_state chan[V2__CHANS];
  uint32_t smpldelta;
  uint32_t smplrem;
} v2__seq_state;

#define V2__MIDIBUF 4096
#define V2__TPC     1000u /* sequencer ticks per second for play/stop times (ms) */

struct v2_synth {
  v2__engine eng;

  uint32_t sample_rate;
  int owned;                  /* allocated by v2_create() */

  /* song */
  uint8_t *song;              /* canonical copy: file + 4 zero bytes */
  size_t song_len;
  int32_t version;
  int32_t npatches;           /* patches reachable by program change */
  int valid;
  v2__container base;
  const char *speech[256];

  /* playback */
  v2__seq_state st;
  uint32_t chanmask;          /* bit ch set = audible */
  uint64_t seed;
  uint32_t noteon[V2__CHANS];
  float fadeval, fadedelta;
  uint8_t midibuf[V2__MIDIBUF];
};

static inline uint8_t v2__peek(const v2_synth *s, size_t off)
{
  return off < s->song_len ? s->song[off] : 0;
}

static inline uint32_t v2__peek32(const v2_synth *s, size_t off)
{
  return (uint32_t)v2__peek(s, off) | ((uint32_t)v2__peek(s, off + 1) << 8) |
         ((uint32_t)v2__peek(s, off + 2) << 16) | ((uint32_t)v2__peek(s, off + 3) << 24);
}

/* event time delta stored as three byte planes of width w */
static inline uint32_t v2__delta(const v2_synth *s, size_t p, uint32_t w)
{
  return (uint32_t)v2__peek(s, p) + ((uint32_t)v2__peek(s, p + w) << 8) +
         ((uint32_t)v2__peek(s, p + 2 * (size_t)w) << 16);
}

/* schedule the first event of a stream, tracking the earliest next time */
static inline void v2__upd1(const v2_synth *s, v2__seq_state *st, uint32_t n, uint32_t *v, size_t p, uint32_t w)
{
  if (n < w) {
    *v = st->time + v2__delta(s, p, w);
    if (*v < st->nexttime) st->nexttime = *v;
  }
}

/* advance to the following event; a zero delta keeps the current time */
static inline void v2__upd2(const v2_synth *s, v2__seq_state *st, uint32_t n, uint32_t *v, size_t p, uint32_t w)
{
  if (n < w) {
    uint32_t d = v2__delta(s, p, w);
    if (d) *v = st->time + d;
  }
}

static inline void v2__upd3(v2__seq_state *st, uint32_t n, const uint32_t *v, uint32_t w)
{
  if (n < w && *v < st->nexttime) st->nexttime = *v;
}

/* MIDI message writer: drops whole events that do not fit */
typedef struct v2__mw {
  uint8_t *p, *end;
  int laststat;
} v2__mw;

static void v2__mw_msg(v2__mw *w, int status, int ndata, uint8_t d0, uint8_t d1)
{
  size_t need = (size_t)ndata + (w->laststat != status ? 1 : 0);
  if ((size_t)(w->end - w->p) < need)
    return;
  if (w->laststat != status) {
    w->laststat = status;
    *w->p++ = (uint8_t)status;
  }
  if (ndata > 0) *w->p++ = d0;
  if (ndata > 1) *w->p++ = d1;
}

/* synthesized product (nexttime-time)*usecs/td2 as 32.32 fixed point */
static void v2__update_sample_delta(uint32_t nexttime, uint32_t time, uint32_t usecs, uint32_t td2,
                                    uint32_t *smplrem, uint32_t *smpldelta)
{
  uint64_t prod = (uint64_t)(uint32_t)(nexttime - time) * usecs;
  uint32_t quot = (uint32_t)(prod / td2);
  uint32_t rem = (uint32_t)(prod % td2);
  uint32_t newrem = *smplrem + rem;
  uint32_t carry = (newrem < *smplrem) ? 1u : 0u;
  *smplrem = newrem;
  *smpldelta = quot + carry;
}

static void v2__seq_reset(v2_synth *s)
{
  v2__seq_state *st = &s->st;
  const v2__container *b = &s->base;

  st->time = 0;
  st->nexttime = (uint32_t)-1;

  st->gnr = 0;
  v2__upd1(s, st, st->gnr, &st->gnt, b->gptr + st->gnr, b->gdnum);
  for (int32_t ch = 0; ch < V2__CHANS; ch++) {
    const struct v2__cchan *bc = &b->chan[ch];
    v2__seq_chan_state *sc = &st->chan[ch];

    memset(sc, 0, sizeof *sc);
    if (!bc->notenum) continue;
    sc->noteptr = bc->noteptr;
    v2__upd1(s, st, sc->notenr, &sc->notent, sc->noteptr, bc->notenum);
    sc->pcptr = bc->pcptr;
    v2__upd1(s, st, sc->pcnr, &sc->pcnt, sc->pcptr, bc->pcnum);
    sc->pbptr = bc->pbptr;
    v2__upd1(s, st, sc->pbnr, &sc->pbnt, sc->pbptr, bc->pcnum); /* sic: pcnum */
    for (int32_t cn = 0; cn < 7; cn++) {
      sc->ctl[cn].ccptr = bc->ctl[cn].ccptr;
      v2__upd1(s, st, sc->ctl[cn].ccnr, &sc->ctl[cn].ccnt, sc->ctl[cn].ccptr, bc->ctl[cn].ccnum);
    }
  }
  st->usecs = 5000u * s->sample_rate;
  st->num = 4;
  st->den = 4;
  st->tpq = 8;
  st->bar = 0;
  st->beat = 0;
  st->tick = 0;
  st->smplrem = 0;

  v2__engine_init(&s->eng, s->song + s->base.patchpos, s->npatches, (int32_t)s->sample_rate);
  if (s->seed)
    s->eng.inst.user_seed = s->seed;
  s->eng.inst.chan_mute = (~s->chanmask) & 0xffffu;
  v2__engine_set_globals(&s->eng, s->song + s->base.globpos);
  v2__ron_set_lyrics(&s->eng.ronan, s->speech);
}

static void v2__seq_tick(v2_synth *s)
{
  v2__seq_state *st = &s->st;
  const v2__container *b = &s->base;

  if (st->state != V2__SEQ_PLAYING)
    return;

  /* bar/beat/tick are bookkeeping only; done by division, not a counting loop */
  st->tick += st->nexttime - st->time;
  st->beat += st->tick / b->timediv;
  st->tick %= b->timediv;
  {
    uint32_t qpb = st->den ? (st->num * 4u / st->den) : 0u;
    if (qpb) {
      st->bar += st->beat / qpb;
      st->beat %= qpb;
    }
  }

  st->time = st->nexttime;
  st->nexttime = (uint32_t)-1;

  v2__mw mw;
  mw.p = s->midibuf;
  mw.end = s->midibuf + V2__MIDIBUF;
  mw.laststat = -1;

  /* new global (tempo) event? */
  if (st->gnr < b->gdnum && st->time == st->gnt) {
    const size_t g = b->gptr;
    st->usecs = v2__peek32(s, g + 3 * (size_t)b->gdnum + 4 * (size_t)st->gnr) * (s->sample_rate / 100);
    st->num = v2__peek(s, g + 7 * (size_t)b->gdnum + st->gnr);
    st->den = v2__peek(s, g + 8 * (size_t)b->gdnum + st->gnr);
    st->tpq = v2__peek(s, g + 9 * (size_t)b->gdnum + st->gnr);
    st->gnr++;
    v2__upd2(s, st, st->gnr, &st->gnt, g + st->gnr, b->gdnum);
  }
  v2__upd3(st, st->gnr, &st->gnt, b->gdnum);

  for (int32_t ch = 0; ch < V2__CHANS; ch++) {
    const struct v2__cchan *bc = &b->chan[ch];
    v2__seq_chan_state *sc = &st->chan[ch];
    if (!bc->notenum)
      continue;

    /* 1. program change */
    if (sc->pcnr < bc->pcnum && st->time == sc->pcnt) {
      sc->lastpc = (uint8_t)(sc->lastpc + v2__peek(s, sc->pcptr + 3 * (size_t)bc->pcnum));
      v2__mw_msg(&mw, 0xc0 | ch, 1, sc->lastpc, 0);
      sc->pcnr++;
      sc->pcptr++;
      v2__upd2(s, st, sc->pcnr, &sc->pcnt, sc->pcptr, bc->pcnum);
    }
    v2__upd3(st, sc->pcnr, &sc->pcnt, bc->pcnum);

    /* 2. control changes */
    for (int32_t cn = 0; cn < 7; cn++) {
      uint32_t ccnum = bc->ctl[cn].ccnum;
      if (sc->ctl[cn].ccnr < ccnum && st->time == sc->ctl[cn].ccnt) {
        sc->ctl[cn].lastcc = (uint8_t)(sc->ctl[cn].lastcc + v2__peek(s, sc->ctl[cn].ccptr + 3 * (size_t)ccnum));
        v2__mw_msg(&mw, 0xb0 | ch, 2, (uint8_t)(cn + 1), sc->ctl[cn].lastcc);
        sc->ctl[cn].ccnr++;
        sc->ctl[cn].ccptr++;
        v2__upd2(s, st, sc->ctl[cn].ccnr, &sc->ctl[cn].ccnt, sc->ctl[cn].ccptr, ccnum);
      }
      v2__upd3(st, sc->ctl[cn].ccnr, &sc->ctl[cn].ccnt, ccnum);
    }

    /* 3. pitch bends (indexed by the program-change count, like the original) */
    if (sc->pbnr < bc->pbnum && st->time == sc->pbnt) {
      sc->lastpb0 = (uint8_t)(sc->lastpb0 + v2__peek(s, sc->pbptr + 3 * (size_t)bc->pcnum));
      sc->lastpb1 = (uint8_t)(sc->lastpb1 + v2__peek(s, sc->pbptr + 4 * (size_t)bc->pcnum));
      v2__mw_msg(&mw, 0xe0 | ch, 2, sc->lastpb0, sc->lastpb1);
      sc->pbnr++;
      sc->pbptr++;
      v2__upd2(s, st, sc->pbnr, &sc->pbnt, sc->pbptr, bc->pbnum);
    }
    v2__upd3(st, sc->pbnr, &sc->pbnt, bc->pbnum);

    /* 4. notes */
    while (sc->notenr < bc->notenum && st->time == sc->notent) {
      sc->lastnte = (uint8_t)(sc->lastnte + v2__peek(s, sc->noteptr + 3 * (size_t)bc->notenum));
      sc->lastvel = (uint8_t)(sc->lastvel + v2__peek(s, sc->noteptr + 4 * (size_t)bc->notenum));
      if (sc->lastvel) s->noteon[ch]++;
      v2__mw_msg(&mw, 0x90 | ch, 2, sc->lastnte, sc->lastvel);
      sc->notenr++;
      sc->noteptr++;
      v2__upd2(s, st, sc->notenr, &sc->notent, sc->noteptr, bc->notenum);
    }
    v2__upd3(st, sc->notenr, &sc->notent, bc->notenum);
  }

  v2__engine_midi(&s->eng, s->midibuf, mw.p);

  if (st->nexttime == (uint32_t)-1)
    st->state = V2__SEQ_STOPPED;
}

static void v2__seq_stop(v2_synth *s, uint32_t fade_ms)
{
  if (!s->valid)
    return;

  if (fade_ms) {
    uint32_t ftsmpls = (uint32_t)(((int64_t)(int32_t)fade_ms * (int32_t)s->sample_rate) / (int32_t)V2__TPC);
    if (ftsmpls) {
      s->fadedelta = s->fadeval / (float)ftsmpls;
      return;
    }
  }
  s->st.state = V2__SEQ_OFF;
}

/* Timing-only fast-forward: the exact sample at which playback would stop. */
static uint64_t v2__seq_calc_song_samples(v2_synth *s)
{
  v2__seq_state *st = &s->st;

  if (!s->valid)
    return 0;
  v2__seq_stop(s, 0);
  v2__seq_reset(s);
  st->state = V2__SEQ_PLAYING;
  st->smpldelta = 0;
  st->smplrem = 0;
  uint64_t total = 0;
  while (st->state == V2__SEQ_PLAYING) {
    total += st->smpldelta;
    v2__seq_tick(s);
    if (st->state == V2__SEQ_PLAYING)
      v2__update_sample_delta(st->nexttime, st->time, st->usecs, 10000u * s->base.timediv,
                              &st->smplrem, &st->smpldelta);
    else
      break;
  }
  return total;
}

static void v2__seq_play(v2_synth *s, uint32_t from_ms)
{
  v2__seq_state *st = &s->st;

  if (!s->valid)
    return;

  v2__seq_stop(s, 0);
  v2__seq_reset(s);

  const uint32_t td2 = 10000u * s->base.timediv;
  const uint32_t destsmpl = (uint32_t)(((int64_t)(int32_t)from_ms * (int32_t)s->sample_rate) / (int32_t)V2__TPC);
  uint32_t cursmpl = 0;

  st->state = V2__SEQ_PLAYING;
  st->smpldelta = 0;
  st->smplrem = 0;
  while ((cursmpl + st->smpldelta) < destsmpl && st->state == V2__SEQ_PLAYING) {
    cursmpl += st->smpldelta;
    v2__seq_tick(s);
    if (st->state == V2__SEQ_PLAYING)
      v2__update_sample_delta(st->nexttime, st->time, st->usecs, td2, &st->smplrem, &st->smpldelta);
    else
      st->smpldelta = (uint32_t)-1;
  }
  st->smpldelta -= (destsmpl - cursmpl);
  s->fadeval = 1.0f;
  s->fadedelta = 0.0f;
}

static void v2__seq_render(v2_synth *s, float *buf, uint32_t len)
{
  v2__seq_state *st = &s->st;

  if (s->valid && st->state == V2__SEQ_PLAYING) {
    const uint32_t td2 = 10000u * s->base.timediv;
    uint32_t todo = len;
    while (todo) {
      uint32_t torender = (todo > st->smpldelta) ? st->smpldelta : todo;
      if (torender > 0x3fffffffu) torender = 0x3fffffffu;
      if (torender) {
        v2__engine_render(&s->eng, buf, (int32_t)torender);
        buf += 2 * (size_t)torender;
        todo -= torender;
        st->smpldelta -= torender;
      }
      if (!st->smpldelta) {
        v2__seq_tick(s);
        if (st->state == V2__SEQ_PLAYING)
          v2__update_sample_delta(st->nexttime, st->time, st->usecs, td2, &st->smplrem, &st->smpldelta);
        else
          st->smpldelta = (uint32_t)-1;
      }
    }
  } else if (st->state == V2__SEQ_OFF || !s->valid) {
    memset(buf, 0, (size_t)len * 2 * sizeof(float));
  } else {
    /* song ended: the synth tail keeps ringing */
    while (len) {
      uint32_t chunk = len > 0x3fffffffu ? 0x3fffffffu : len;
      v2__engine_render(&s->eng, buf, (int32_t)chunk);
      buf += 2 * (size_t)chunk;
      len -= chunk;
    }
  }
}

/* -------------------------------------------------------------------------- */
/*  Public API                                                                */
/* -------------------------------------------------------------------------- */

static const char v2__space[] = " ";

V2_API const char *v2_result_string(v2_result r)
{
  switch (r) {
  case V2_OK:                      return "ok";
  case V2_ERR_BAD_FILE:            return "not a valid v2m file";
  case V2_ERR_UNSUPPORTED_VERSION: return "unsupported v2m format version (only version 6 is supported)";
  case V2_ERR_FP_ENV:              return "floating point environment flushes subnormals (fast-math build?)";
  case V2_ERR_OUT_OF_MEMORY:       return "out of memory";
  case V2_ERR_BAD_ARGUMENT:        return "bad argument";
  }
  return "unknown error";
}

/* The engine's decaying envelopes keep modulating in the subnormal range, so
   a host that flushes subnormals (FTZ/DAZ, fast-math) breaks it. volatile
   defeats constant folding. */
V2_API int v2_fp_env_ok(void)
{
  volatile float a = 1.0e-30f;
  volatile float b = 1.0e-12f;
  volatile float sub = a * b;
  if (sub == 0.0f)
    return 0;
  volatile float back = sub * 1.0e12f;
  return back != 0.0f;
}

#define V2__ALIGN 8

V2_API size_t v2_synth_size(void)
{
  return sizeof(struct v2_synth);
}

V2_API v2_synth *v2_synth_init(void *mem, size_t mem_size, uint32_t sample_rate)
{
  if (!mem || mem_size < sizeof(struct v2_synth) || ((uintptr_t)mem % V2__ALIGN) != 0)
    return NULL;
  if (sample_rate < V2_MIN_SAMPLE_RATE || sample_rate > V2_MAX_SAMPLE_RATE)
    return NULL;

  v2_synth *s = (v2_synth *)mem;
  memset(s, 0, sizeof *s);
  s->sample_rate = sample_rate;
  s->version = -1;
  s->chanmask = 0xffffffffu;
  return s;
}

V2_API v2_synth *v2_create(uint32_t sample_rate)
{
  void *mem = V2_MALLOC(sizeof(struct v2_synth));
  if (!mem)
    return NULL;
  v2_synth *s = v2_synth_init(mem, sizeof(struct v2_synth), sample_rate);
  if (!s) {
    V2_FREE(mem);
    return NULL;
  }
  s->owned = 1;
  return s;
}

V2_API void v2_close(v2_synth *s)
{
  if (!s)
    return;
  if (s->song)
    V2_FREE(s->song);
  s->song = NULL;
  s->song_len = 0;
  s->valid = 0;
  s->version = -1;
  s->st.state = V2__SEQ_OFF;
}

V2_API void v2_destroy(v2_synth *s)
{
  if (!s)
    return;
  v2_close(s);
  if (s->owned)
    V2_FREE(s);
}

V2_API uint32_t v2_sample_rate(const v2_synth *s)
{
  return s ? s->sample_rate : 0;
}

V2_API int v2_file_version(const v2_synth *s)
{
  return s ? s->version : -1;
}

/* Texts for the Ronan speech synth: offsets into the speech block. Without a
   block every slot is " "; with one, slots past its text count stay empty. */
static void v2__build_speech(v2_synth *s)
{
  const v2__container *c = &s->base;
  int32_t i;

  for (i = 0; i < 256; i++)
    s->speech[i] = v2__space;

  if (!c->spsize || c->spsize >= 8192 || c->spsize < 4)
    return;

  const uint8_t *sp = s->song + c->sppos;
  uint32_t n = v2__rd32(sp);
  if (n > 256) n = 256;
  if (n > (c->spsize - 4) / 4) n = (c->spsize - 4) / 4;

  for (i = 0; i < 256; i++)
    s->speech[i] = NULL;
  for (i = 0; i < (int32_t)n; i++) {
    uint32_t off = v2__rd32(sp + 4 + 4 * (size_t)i);
    s->speech[i] = off < c->spsize ? (const char *)sp + off : v2__space;
  }
}

V2_API v2_result v2_open(v2_synth *s, const void *data, size_t size)
{
  v2__container c;

  if (!s)
    return V2_ERR_BAD_ARGUMENT;
  v2_close(s);

  if (!v2_fp_env_ok())
    return V2_ERR_FP_ENV;
  if (!data || size < 16 || size > V2__MAX_SONG_BYTES)
    return V2_ERR_BAD_FILE;

  const uint8_t *in = (const uint8_t *)data;
  if (!v2__parse_container(in, size, &c))
    return V2_ERR_BAD_FILE;

  c.maxp = v2__count_used_patches(in, &c);
  if (!c.maxp) /* no patch data (forgot program changes?) */
    return V2_ERR_BAD_FILE;

  int32_t ver = v2__detect_version(in, &c);
  if (ver < 0)
    return V2_ERR_BAD_FILE;
  s->version = ver;
  if (ver != V2__MAX_FORMAT_VER)
    return V2_ERR_UNSUPPORTED_VERSION;

  int32_t npatches = v2__validate_patches(in + c.patchpos, c.patchsize);
  if (!npatches || c.timediv == 0 || 10000u * c.timediv == 0)
    return V2_ERR_BAD_FILE;

  /* the song is kept as the file plus 4 zero bytes, so that the optional
     speech-size field and speech strings are always terminated */
  uint8_t *song = (uint8_t *)V2_MALLOC(size + 4);
  if (!song)
    return V2_ERR_OUT_OF_MEMORY;
  memcpy(song, in, size);
  memset(song + size, 0, 4);

  s->song = song;
  s->song_len = size + 4;
  if (!v2__parse_container(song, size + 4, &s->base)) {
    v2_close(s);
    s->version = ver;
    return V2_ERR_BAD_FILE;
  }
  s->npatches = npatches;
  v2__build_speech(s);

  memset(s->noteon, 0, sizeof s->noteon);
  s->fadeval = 1.0f;
  s->fadedelta = 0.0f;
  s->valid = 1;
  s->st.state = V2__SEQ_OFF;
  v2__seq_reset(s);
  return V2_OK;
}

V2_API void v2_play(v2_synth *s, uint32_t from_ms)
{
  if (s)
    v2__seq_play(s, from_ms);
}

V2_API void v2_stop(v2_synth *s, uint32_t fade_ms)
{
  if (s)
    v2__seq_stop(s, fade_ms);
}

V2_API int v2_is_playing(const v2_synth *s)
{
  return s && s->valid && s->st.state == V2__SEQ_PLAYING;
}

V2_API void v2_render(v2_synth *s, float *out, uint32_t frames)
{
  if (!s || !out)
    return;

  v2__seq_render(s, out, frames);

  if (s->fadedelta != 0.0f) {
    for (uint32_t i = 0; i < frames; i++) {
      out[2 * (size_t)i] *= s->fadeval;
      out[2 * (size_t)i + 1] *= s->fadeval;
      s->fadeval -= s->fadedelta;
      if (s->fadeval < 0.0f) s->fadeval = 0.0f;
    }
    if (s->fadeval == 0.0f)
      v2__seq_stop(s, 0);
  }
}

V2_API uint64_t v2_length_frames(v2_synth *s)
{
  return s ? v2__seq_calc_song_samples(s) : 0;
}

V2_API void v2_set_seed(v2_synth *s, uint64_t seed)
{
  if (s)
    s->seed = seed;
}

V2_API void v2_set_channel_mask(v2_synth *s, uint32_t mask)
{
  if (!s)
    return;
  s->chanmask = mask;
  if (s->valid)
    s->eng.inst.chan_mute = (~mask) & 0xffffu;
}

V2_API int v2_get_channel_poly(const v2_synth *s, int out[16])
{
  int total = 0;
  for (int32_t i = 0; i < V2__CHANS; i++)
    out[i] = 0;
  if (!s || !s->valid)
    return 0;

  for (int32_t i = 0; i < V2__POLY; i++) {
    int32_t chan = s->eng.chanmap[i];
    if (chan < 0)
      continue;
    out[chan]++;
    total++;
  }
  return total;
}

V2_API void v2_get_channel_note_ons(const v2_synth *s, uint32_t out[16])
{
  for (int32_t i = 0; i < V2__CHANS; i++)
    out[i] = (s && s->valid) ? s->noteon[i] : 0;
}

V2_API void v2_get_channel_levels(v2_synth *s, float out[16])
{
  for (int32_t i = 0; i < V2__CHANS; i++) {
    out[i] = 0.0f;
    if (s && s->valid) {
      out[i] = s->eng.inst.chan_peak[i];
      s->eng.inst.chan_peak[i] = 0.0f;
    }
  }
}

V2_API void v2_midi(v2_synth *s, const uint8_t *msg, size_t len)
{
  if (!s || !s->valid || !msg || !len)
    return;
  v2__engine_midi(&s->eng, msg, msg + len);
}

V2_API void v2_render_direct(v2_synth *s, float *out, uint32_t frames)
{
  if (!s || !out)
    return;
  if (!s->valid) {
    memset(out, 0, (size_t)frames * 2 * sizeof(float));
    return;
  }
  while (frames) {
    uint32_t chunk = frames > 0x3fffffffu ? 0x3fffffffu : frames;
    v2__engine_render(&s->eng, out, (int32_t)chunk);
    out += 2 * (size_t)chunk;
    frames -= chunk;
  }
}

#ifdef __GNUC__
#ifndef __clang__
#pragma GCC pop_options
#endif
#endif

#endif /* V2_IMPLEMENTATION */
