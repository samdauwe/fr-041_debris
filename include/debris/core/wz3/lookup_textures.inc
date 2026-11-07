/* ======================================================================== */
/*  generated lookup textures                                               */
/* ======================================================================== */

WZ3_API void wz3_make_cube_normalizer(uint8_t out[WZ3_CUBE_NORMALIZER_BYTES])
{
  enum { size = 64 };
  static const float faces[6][2][3] = {
    {{ 0, 0,-1},{ 0, 1, 0}},
    {{ 0, 0, 1},{ 0, 1, 0}},
    {{ 1, 0, 0},{ 0, 0,-1}},
    {{ 1, 0, 0},{ 0, 0, 1}},
    {{ 1, 0, 0},{ 0, 1, 0}},
    {{-1, 0, 0},{ 0, 1, 0}},
  };
  size_t offset = 0;
  const float half = ((float)size - 1.0f) * 0.5f;
  for (int face = 0; face < 6; face++) {
    const float *u = faces[face][0];
    const float *v = faces[face][1];
    const float n[3] = {
      (u[1] * v[2]) + -(u[2] * v[1]),
      (u[2] * v[0]) + -(u[0] * v[2]),
      (u[0] * v[1]) + -(u[1] * v[0]),
    };
    for (int y = 0; y < size; y++) {
      for (int x = 0; x < size; x++) {
        const float us = (float)x - half;
        const float vs = -(float)y + half;
        float p[3];
        for (int c = 0; c < 3; c++) {
          p[c] = (n[c] * half) + (u[c] * us);
          p[c] = p[c] + (v[c] * vs);
        }
        const float dot = ((p[0] * p[0]) + (p[1] * p[1])) + (p[2] * p[2]);
        const float inverse = (float)(1.0 / (double)sqrtf(dot));
        for (int c = 0; c < 3; c++) p[c] = p[c] * inverse;
        /* logical RGBA corresponding to the native memory bytes z, y, x, 0 */
        out[offset++] = (uint8_t)wz3_ftol((double)(128.0f + (p[0] * 127.0f)));
        out[offset++] = (uint8_t)wz3_ftol((double)(128.0f + (p[1] * 127.0f)));
        out[offset++] = (uint8_t)wz3_ftol((double)(128.0f + (p[2] * 127.0f)));
        out[offset++] = 0;
      }
    }
  }
}

WZ3_API void wz3_make_attenuation_volume(uint8_t out[WZ3_ATTENUATION_VOLUME_BYTES])
{
  enum { size = 32 };
  const float scale = 2.0f / ((float)size - 2.0f);
  const float middle = (float)size / 2.0f;
  size_t offset = 0;
  for (int z = 0; z < size; z++) {
    const float vz = ((float)z - middle) * scale;
    for (int y = 0; y < size; y++) {
      const float vy = ((float)y - middle) * scale;
      for (int x = 0; x < size; x++) {
        const float vx = ((float)x - middle) * scale;
        const float dot = ((vx * vx) + (vy * vy)) + (vz * vz);
        const float t = 1.0f + -dot;
        const float attenuation = t > 0.0f ? t : 0.0f;
        const uint8_t byte = (uint8_t)wz3_ftol((double)(attenuation * 255.0f));
        for (int channel = 0; channel < 4; channel++) out[offset++] = byte;
      }
    }
  }
}

