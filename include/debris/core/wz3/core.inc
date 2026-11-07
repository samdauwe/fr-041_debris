/* ======================================================================== */
/*  core                                                                    */
/* ======================================================================== */

#define WZ3__PI 3.14159265358979323846

static inline float wz3__bits2float(uint32_t u)
{
  float f;
  memcpy(&f, &u, sizeof f);
  return f;
}

WZ3_API const char *wz3_result_string(wz3_result r)
{
  switch (r) {
  case WZ3_OK:                 return "ok";
  case WZ3_ERR_ARGUMENT:       return "bad argument";
  case WZ3_ERR_FORMAT:         return "malformed document";
  case WZ3_ERR_TRUNCATED:      return "truncated document";
  case WZ3_ERR_LIMIT:          return "document exceeds a sanity limit";
  case WZ3_ERR_OUT_OF_MEMORY:  return "out of memory";
  case WZ3_ERR_UNSUPPORTED:    return "operator class not supported by this layer";
  case WZ3_ERR_OPERATOR:       return "operator produced no result";
  case WZ3_ERR_CANCELLED:      return "cancelled";
  }
  return "unknown error";
}

WZ3_API void wz3_rng_init(wz3_rng *r) { r->seed = 0x74382381u; }

WZ3_API uint32_t wz3_rng_u32(wz3_rng *r)
{
  uint32_t first = r->seed * 0x343fdu + 0x269ec3u;
  uint32_t second = first * 0x343fdu + 0x269ec3u;
  r->seed = second;
  return ((second >> 10) & 0xffffu) | ((first << 6) & 0xffff0000u);
}

WZ3_API void wz3_rng_seed(wz3_rng *r, int32_t seed)
{
  /* the signed overflow happens before the division in the C expression */
  uint32_t useed = (uint32_t)seed;
  int32_t seed121 = (int32_t)(useed * 121u);
  int32_t mixed = (int32_t)(useed + useed * 17u + (uint32_t)seed121 + (uint32_t)(seed121 / 17));
  r->seed = (uint32_t)mixed;
  wz3_rng_u32(r); r->seed ^= (uint32_t)mixed;
  wz3_rng_u32(r); r->seed ^= (uint32_t)mixed;
  wz3_rng_u32(r); r->seed ^= (uint32_t)mixed;
  wz3_rng_u32(r);
}

WZ3_API uint32_t wz3_rng_int(wz3_rng *r, uint32_t max)
{
  return max ? wz3_rng_u32(r) % max : 0;
}

WZ3_API float wz3_rng_float(wz3_rng *r, float max)
{
  /* the product is formed in binary64 before the single rounding */
  return (float)(((double)(wz3_rng_u32(r) & 0x3fffffffu) * (double)max) / 1073741824.0);
}

WZ3_API int32_t wz3_ftol(double value)
{
  if (!isfinite(value) || value < -2147483648.0 || value >= 2147483648.0)
    return INT32_MIN;
  double fl = floor(value);
  double fraction = value - fl;
  double r;
  if (fraction < 0.5) r = fl;
  else if (fraction > 0.5) r = fl + 1.0;
  else r = (((int64_t)fl) & 1) ? fl + 1.0 : fl;
  return (int32_t)r;
}

/* The released sFPow is a handwritten x87 routine: FYL2X and F2XM1 keep their
   results internally while the surrounding arithmetic runs at 24 bits. */
WZ3_API double wz3_pow(double base, double exponent)
{
  if (base == 0.0 || isnan(base)) return base;
  if (!(base > 0.0) || !isfinite(base) || !isfinite(exponent)) return 0.0;

  double logarithm = exponent * wz3__log2(base);
  /* FIST stores a signed dword; an out-of-range result is x87's indefinite */
  if (!isfinite(logarithm) || logarithm < -2147483648.0 - 0.5 || logarithm >= 2147483647.0 + 0.5) return 0.0;
  double lower = floor(logarithm);
  double fraction_from_lower = logarithm - lower;
  double integral = fraction_from_lower < 0.5 ? lower
                  : fraction_from_lower > 0.5 ? lower + 1.0
                  : (((int64_t)lower) & 1) ? lower + 1.0 : lower;
  double scale_exponent = integral;
  if (integral >= 2147466241.0) {
    /* ADD EAX,0x3fff overflows for the top valid FIST results; FLD then reads
       the wrapped low 15 bits as the handcrafted exponent */
    scale_exponent = (double)((((int64_t)(integral + 16383.0)) & 0x7fff) - 0x3fff);
  } else {
    if (integral <= -16383.0) return 0.0;
    if (integral >= 16384.0) return INFINITY;
  }

  /* FISUB and the add after F2XM1 each round at PC=24 */
  float fractional = (float)(logarithm - integral);
  float mantissa = (float)((wz3__pow(2.0, (double)fractional) - 1.0) + 1.0);
  return ldexp((double)mantissa, (int)scale_exponent);
}

/* ---- matrices ---------------------------------------------------------- */

static const float wz3__sse_one_over_two_pi = (float)(1.0 / (double)(float)(WZ3__PI * 2.0));
static const float wz3__sse_half_pi = (float)((double)(float)WZ3__PI * 0.5);
static const float wz3__sse_pi = (float)WZ3__PI;
static const float wz3__sse_two_pi = (float)(WZ3__PI * 2.0);
static const float wz3__sse_sin_c0 = (float)-1.854219680e-4;
static const float wz3__sse_sin_c1 = (float)8.314273770e-3;
static const float wz3__sse_sin_c2 = (float)-1.666585317e-1;

static float wz3__sin_poly(float v)
{
  float sq = v * v;
  float p = wz3__sse_sin_c0 * sq;
  p = p + wz3__sse_sin_c1;
  p = p * sq;
  p = p + wz3__sse_sin_c2;
  p = p * sq;
  p = p + 1.0f;
  return p * v;
}

/* SSE_SinCos4 range-reduces and evaluates this Remez polynomial with a
   rounded single-precision result after every instruction. */
WZ3_API void wz3_sincos(float angle, float *sine_out, float *cosine_out)
{
  float input = angle;
  float scaled = input * wz3__sse_one_over_two_pi;
  /* cvttps truncates, then one is subtracted for each negative lane */
  float period = truncf(scaled);
  period = period - (input < 0.0f ? 1.0f : 0.0f);
  float reduced = input - period * wz3__sse_two_pi;

  float pi_delta = wz3__sse_pi - reduced;
  int sine_negative = signbit(pi_delta) != 0;
  float quadrant = fabsf(pi_delta);
  int cosine_negative = quadrant < wz3__sse_half_pi;
  float other = wz3__sse_pi - quadrant;
  quadrant = (quadrant != quadrant || other != other) ? NAN : (quadrant < other ? quadrant : other);

  float sine = wz3__sin_poly(quadrant);
  float cosine = wz3__sin_poly(wz3__sse_half_pi - quadrant);
  if (sine_negative) sine = -sine;
  if (cosine_negative) cosine = -cosine;
  *sine_out = sine;
  *cosine_out = cosine;
}

WZ3_API void wz3_mat4_identity(float out[16])
{
  memset(out, 0, 16 * sizeof(float));
  out[0] = out[5] = out[10] = out[15] = 1.0f;
}

/* out = a * b in the original column-vector convention. Released
   sMatrix::Mul4 keeps x87 at 24 bits with this exact schedule. */
WZ3_API void wz3_mat4_mul(const float a[16], const float b[16], float out[16])
{
  float t[16];
  for (int column = 0; column < 4; column++) {
    const int c = column * 4;
    const float x = b[c], y = b[c + 1], z = b[c + 2], w = b[c + 3];
    t[c]     = (((a[4] * y + a[12] * w) + a[8] * z) + a[0] * x);
    t[c + 1] = (((a[1] * x + a[5] * y) + a[13] * w) + a[9] * z);
    t[c + 2] = (((a[6] * y + a[14] * w) + a[10] * z) + a[2] * x);
    t[c + 3] = (((a[15] * w + a[11] * z) + a[3] * x) + a[7] * y);
  }
  memcpy(out, t, sizeof t);
}

/* sMatrix::MulA(a,b) is b * a in the column-vector representation. */
WZ3_API void wz3_mat4_mul_a(const float a[16], const float b[16], float out[16])
{
  float t[16];
  for (int column = 0; column < 4; column++) {
    const int c = column * 4;
    const float x = a[c], y = a[c + 1], z = a[c + 2];
    t[c]     = ((b[8] * z + b[4] * y) + b[0] * x);
    t[c + 1] = ((b[1] * x + b[9] * z) + b[5] * y);
    t[c + 2] = ((b[2] * x + b[10] * z) + b[6] * y);
    t[c + 3] = 0.0f;
  }
  t[12] = t[12] + b[12];
  t[13] = t[13] + b[13];
  t[14] = t[14] + b[14];
  t[15] = 1.0f;
  memcpy(out, t, sizeof t);
}

/* sMatrix::Mul3(a,b): b * a for the rotational 3x3 portion. */
WZ3_API void wz3_mat4_mul3(const float a[16], const float b[16], float out[16])
{
  float t[16];
  wz3_mat4_identity(t);
  for (int column = 0; column < 3; column++) {
    const int c = column * 4;
    const float x = a[c], y = a[c + 1], z = a[c + 2];
    t[c]     = ((b[0] * x + b[4] * y) + b[8] * z);
    t[c + 1] = ((b[1] * x + b[5] * y) + b[9] * z);
    t[c + 2] = ((b[2] * x + b[6] * y) + b[10] * z);
    t[c + 3] = 0.0f;
  }
  memcpy(out, t, sizeof t);
}

WZ3_API void wz3_mat4_euler(float x, float y, float z, float out[16])
{
  float sx, cx, sy, cy, sz, cz;
  wz3_sincos(x, &sx, &cx);
  wz3_sincos(y, &sy, &cy);
  wz3_sincos(z, &sz, &cz);

  out[0] = cy * cz;
  out[1] = cy * sz;
  out[2] = -sy;
  out[3] = 0.0f;
  out[4] = -(cx * sz) + (sx * sy) * cz;
  out[5] = cx * cz + (sx * sy) * sz;
  out[6] = sx * cy;
  out[7] = 0.0f;
  out[8] = sx * sz + (cx * sy) * cz;
  out[9] = -(sx * cz) + (cx * sy) * sz;
  out[10] = cx * cy;
  out[11] = 0.0f;
  out[12] = out[13] = out[14] = 0.0f;
  out[15] = 1.0f;
}

WZ3_API void wz3_mat4_euler_turns(const float turns[3], float out[16])
{
  float x = turns[0], y = turns[1], z = turns[2];
  if (fabs((double)x) < 1e-6 && fabs((double)y) < 1e-6 && fabs((double)z) < 1e-6) {
    wz3_mat4_identity(out);
    return;
  }
  wz3_mat4_euler(x * wz3__sse_two_pi, y * wz3__sse_two_pi, z * wz3__sse_two_pi, out);
}

WZ3_API void wz3_mat4_srt(const float srt[9], float out[16])
{
  wz3_mat4_euler_turns(srt + 3, out);
  const float sx = srt[0], sy = srt[1], sz = srt[2];
  out[0] = out[0] * sx; out[1] = out[1] * sx; out[2] = out[2] * sx;
  out[4] = out[4] * sy; out[5] = out[5] * sy; out[6] = out[6] * sy;
  out[8] = out[8] * sz; out[9] = out[9] * sz; out[10] = out[10] * sz;
  out[12] = srt[6];
  out[13] = srt[7];
  out[14] = srt[8];
  out[15] = 1.0f;
}

WZ3_API void wz3_vec3_cross(const float a[3], const float b[3], float out[3])
{
  /* binary64 products and difference, one rounding to float */
  float t[3];
  t[0] = (float)((double)a[1] * b[2] - (double)a[2] * b[1]);
  t[1] = (float)((double)a[2] * b[0] - (double)a[0] * b[2]);
  t[2] = (float)((double)a[0] * b[1] - (double)a[1] * b[0]);
  memcpy(out, t, sizeof t);
}

WZ3_API void wz3_vec3_normalize(const float v[3], float out[3])
{
  double length = sqrt((double)v[0] * v[0] + (double)v[1] * v[1] + (double)v[2] * v[2]);
  if (length > 1e-20) {
    float t0 = (float)(v[0] / length), t1 = (float)(v[1] / length), t2 = (float)(v[2] / length);
    out[0] = t0; out[1] = t1; out[2] = t2;
  } else {
    out[0] = 1.0f; out[1] = out[2] = 0.0f;
  }
}

/* sMatrix::InitDir's UnitSafe3: the epsilon applies to the squared length. */
WZ3_API void wz3_vec3_normalize_safe(const float v[3], float out[3])
{
  double ls = (double)v[0] * v[0] + (double)v[1] * v[1] + (double)v[2] * v[2];
  if (ls > 1e-20) {
    double inverse = 1.0 / sqrt(ls);
    float t0 = (float)(v[0] * inverse), t1 = (float)(v[1] * inverse), t2 = (float)(v[2] * inverse);
    out[0] = t0; out[1] = t1; out[2] = t2;
  } else {
    out[0] = 1.0f; out[1] = out[2] = 0.0f;
  }
}

WZ3_API void wz3_mat4_direction(const float direction[3], float out[16])
{
  static const float up[3] = { 0.0f, 1.0f, 0.0f };
  float k[3], i[3], j[3], c[3];
  wz3_vec3_normalize_safe(direction, k);
  wz3_vec3_cross(up, k, c);
  wz3_vec3_normalize_safe(c, i);
  wz3_vec3_cross(k, i, j);
  out[0] = i[0]; out[1] = i[1]; out[2] = i[2]; out[3] = 0.0f;
  out[4] = j[0]; out[5] = j[1]; out[6] = j[2]; out[7] = 0.0f;
  out[8] = k[0]; out[9] = k[1]; out[10] = k[2]; out[11] = 0.0f;
  out[12] = out[13] = out[14] = 0.0f;
  out[15] = 1.0f;
}

WZ3_API void wz3_mat4_transform_point(const float m[16], const float v[4], float out[4])
{
  const double x = v[0], y = v[1], z = v[2], w = v[3];
  float t[4];
  t[0] = (float)(m[0] * x + m[4] * y + m[8] * z + m[12] * w);
  t[1] = (float)(m[1] * x + m[5] * y + m[9] * z + m[13] * w);
  t[2] = (float)(m[2] * x + m[6] * y + m[10] * z + m[14] * w);
  t[3] = (float)(m[3] * x + m[7] * y + m[11] * z + m[15] * w);
  memcpy(out, t, sizeof t);
}

