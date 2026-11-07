/* ======================================================================== */
/*  render planning (the pure parts of engine.cpp)                          */
/*                                                                          */
/*  Everything here works in binary64 like the JavaScript port, with a      */
/*  float rounding wherever a Float32Array is written.                      */
/* ======================================================================== */

static inline int wz3__finite(double x) { return x == x && x - x == 0.0; }
static inline double wz3__js_trunc(double x) { return (x < 4.5e15 && x > -4.5e15) ? (double)(int64_t)x : x; }
static inline double wz3__or_default(double x, double fallback) { return (x == x && x != 0.0) ? x : fallback; }   /* `Number(x) || fallback` */

/* Math.hypot(a, b, c) as V8 computes it: scale by the largest magnitude and sum the squares with Kahan compensation */
static double wz3__js_hypot3(double a, double b, double c)
{
  const double v[3] = {a, b, c};
  double magnitude[3] = {0.0, 0.0, 0.0}, max = 0.0;
  int nan = 0;
  for (int i = 0; i < 3; i++) {
    if (v[i] != v[i]) { nan = 1; continue; }
    magnitude[i] = fabs(v[i]);
    if (magnitude[i] > max) max = magnitude[i];
  }
  if (max == (double)INFINITY) return (double)INFINITY;
  if (nan) return a + b + c;
  if (max == 0.0) return 0.0;
  double sum = 0.0, compensation = 0.0;
  for (int i = 0; i < 3; i++) {
    const double n = magnitude[i] / max;
    const double summand = n * n - compensation;
    const double preliminary = sum + summand;
    compensation = (preliminary - sum) - summand;
    sum = preliminary;
  }
  return sqrt(sum) * max;
}

static int wz3__all_finite_f(const float *v, int n)
{
  for (int i = 0; i < n; i++) if (!wz3__finite((double)v[i])) return 0;
  return 1;
}

/* ---- camera matrices (engine.cpp) ---- */

WZ3_API int wz3_mat4_inverse(const float a[16], float out[16])
{
  const double a00 = a[0], a01 = a[1], a02 = a[2], a03 = a[3];
  const double a10 = a[4], a11 = a[5], a12 = a[6], a13 = a[7];
  const double a20 = a[8], a21 = a[9], a22 = a[10], a23 = a[11];
  const double a30 = a[12], a31 = a[13], a32 = a[14], a33 = a[15];
  const double b00 = a00 * a11 - a01 * a10, b01 = a00 * a12 - a02 * a10, b02 = a00 * a13 - a03 * a10;
  const double b03 = a01 * a12 - a02 * a11, b04 = a01 * a13 - a03 * a11, b05 = a02 * a13 - a03 * a12;
  const double b06 = a20 * a31 - a21 * a30, b07 = a20 * a32 - a22 * a30, b08 = a20 * a33 - a23 * a30;
  const double b09 = a21 * a32 - a22 * a31, b10 = a21 * a33 - a23 * a31, b11 = a22 * a33 - a23 * a32;
  double determinant = b00 * b11 - b01 * b10 + b02 * b09 + b03 * b08 - b04 * b07 + b05 * b06;
  if (!wz3__finite(determinant) || fabs(determinant) < 1e-30) return 0;
  determinant = 1 / determinant;
  out[0] = (float)((a11 * b11 - a12 * b10 + a13 * b09) * determinant);
  out[1] = (float)((a02 * b10 - a01 * b11 - a03 * b09) * determinant);
  out[2] = (float)((a31 * b05 - a32 * b04 + a33 * b03) * determinant);
  out[3] = (float)((a22 * b04 - a21 * b05 - a23 * b03) * determinant);
  out[4] = (float)((a12 * b08 - a10 * b11 - a13 * b07) * determinant);
  out[5] = (float)((a00 * b11 - a02 * b08 + a03 * b07) * determinant);
  out[6] = (float)((a32 * b02 - a30 * b05 - a33 * b01) * determinant);
  out[7] = (float)((a20 * b05 - a22 * b02 + a23 * b01) * determinant);
  out[8] = (float)((a10 * b10 - a11 * b08 + a13 * b06) * determinant);
  out[9] = (float)((a01 * b08 - a00 * b10 - a03 * b06) * determinant);
  out[10] = (float)((a30 * b04 - a31 * b02 + a33 * b00) * determinant);
  out[11] = (float)((a21 * b02 - a20 * b04 - a23 * b00) * determinant);
  out[12] = (float)((a11 * b07 - a10 * b09 - a12 * b06) * determinant);
  out[13] = (float)((a00 * b09 - a01 * b07 + a02 * b06) * determinant);
  out[14] = (float)((a31 * b01 - a30 * b03 - a32 * b00) * determinant);
  out[15] = (float)((a20 * b03 - a21 * b01 + a22 * b00) * determinant);
  return 1;
}

WZ3_API void wz3_camera_matrices(const wz3_camera *camera, wz3_camera_mats *out)
{
  if (!wz3_mat4_inverse(camera->camera_space, out->view)) wz3_mat4_identity(out->view);
  const double near = wz3__js_max(1e-5, fabs(wz3__or_default(camera->near_clip, 0.125)));
  memset(out->projection, 0, sizeof out->projection);
  out->projection[0] = (float)wz3__or_default(camera->zoom_x, 1.0);
  out->projection[5] = (float)wz3__or_default(camera->zoom_y, 1.0);
  out->projection[8] = (float)wz3__or_default(camera->center_x, 0.0);
  out->projection[9] = (float)wz3__or_default(camera->center_y, 0.0);
  out->projection[10] = 1.0f;
  out->projection[11] = 1.0f;
  out->projection[14] = (float)(-2.0 * near);
  wz3_mat4_mul(out->projection, out->view, out->view_projection);
}

/* ---- bounds ---- */

WZ3_API uint32_t wz3_compose_instances(const float base[16], int has_instances, const float *instances, uint32_t n_instances, float *out)
{
  if (!has_instances) { memcpy(out, base, 16 * sizeof(float)); return 1; }
  for (uint32_t index = 0; index < n_instances; index++) {
    const float *instance = instances + (size_t)index * 16;
    float *target = out + (size_t)index * 16;
    for (int column = 0; column < 4; column++) {
      const int source = column * 4;
      for (int row = 0; row < 4; row++)
        target[source + row] = (float)((double)base[row] * instance[source] + (double)base[4 + row] * instance[source + 1] +
                                       (double)base[8 + row] * instance[source + 2] + (double)base[12 + row] * instance[source + 3]);
    }
  }
  return n_instances;
}

WZ3_API int wz3_transform_bounds(const double minimum[3], const double maximum[3], const float *matrix, wz3_bounds *out)
{
  for (int i = 0; i < 3; i++) if (!wz3__finite(minimum[i]) || !wz3__finite(maximum[i])) return 0;
  if (minimum[0] > maximum[0] || minimum[1] > maximum[1] || minimum[2] > maximum[2]) return 0;
  if (!wz3__all_finite_f(matrix, 16)) return 0;
  const double m0 = matrix[0], m1 = matrix[1], m2 = matrix[2], m3 = matrix[3], m4 = matrix[4], m5 = matrix[5], m6 = matrix[6];
  const double m7 = matrix[7], m8 = matrix[8], m9 = matrix[9], m10 = matrix[10], m11 = matrix[11], m12 = matrix[12];
  const double m13 = matrix[13], m14 = matrix[14], m15 = matrix[15];
  if (fabs(m3) > 1e-8 || fabs(m7) > 1e-8 || fabs(m11) > 1e-8 || fabs(m15 - 1) > 1e-8) return 0;   /* projective: decline to cull */
  const double cx = (minimum[0] + maximum[0]) * 0.5, cy = (minimum[1] + maximum[1]) * 0.5, cz = (minimum[2] + maximum[2]) * 0.5;
  const double ex = wz3__js_max(0, (maximum[0] - minimum[0]) * 0.5);
  const double ey = wz3__js_max(0, (maximum[1] - minimum[1]) * 0.5);
  const double ez = wz3__js_max(0, (maximum[2] - minimum[2]) * 0.5);
  const double x = m0 * cx + m4 * cy + m8 * cz + m12;
  const double y = m1 * cx + m5 * cy + m9 * cz + m13;
  const double z = m2 * cx + m6 * cy + m10 * cz + m14;
  const double wx = fabs(m0) * ex + fabs(m4) * ey + fabs(m8) * ez;
  const double wy = fabs(m1) * ex + fabs(m5) * ey + fabs(m9) * ez;
  const double wz = fabs(m2) * ex + fabs(m6) * ey + fabs(m10) * ez;
  out->min[0] = x - wx; out->min[1] = y - wy; out->min[2] = z - wz;
  out->max[0] = x + wx; out->max[1] = y + wy; out->max[2] = z + wz;
  return 1;
}

WZ3_API int wz3_job_world_bounds(const double minimum[3], const double maximum[3], const float *matrices, uint32_t n, wz3_bounds *out)
{
  if (!n) return 0;
  wz3_bounds result, transformed;
  for (int axis = 0; axis < 3; axis++) { result.min[axis] = (double)INFINITY; result.max[axis] = -(double)INFINITY; }
  for (uint32_t i = 0; i < n; i++) {
    if (!wz3_transform_bounds(minimum, maximum, matrices + (size_t)i * 16, &transformed)) return 0;
    for (int axis = 0; axis < 3; axis++) {
      result.min[axis] = wz3__js_min(result.min[axis], transformed.min[axis]);
      result.max[axis] = wz3__js_max(result.max[axis], transformed.max[axis]);
    }
  }
  *out = result;
  return 1;
}

WZ3_API int wz3_bounds_intersect_sphere(const wz3_bounds *bounds, const double center[3], double radius)
{
  if (!bounds || !center || !wz3__finite(radius) || radius < 0 || !wz3__finite(center[0]) || !wz3__finite(center[1]) ||
      !wz3__finite(center[2])) return 1;
  double distance_squared = 0;
  for (int axis = 0; axis < 3; axis++) {
    const double delta = center[axis] < bounds->min[axis] ? center[axis] - bounds->min[axis]
                       : center[axis] > bounds->max[axis] ? center[axis] - bounds->max[axis] : 0.0;
    distance_squared += delta * delta;
  }
  const double conservative = radius + wz3__js_max(1e-6, fabs(radius) * 1e-7);
  return distance_squared <= conservative * conservative;
}

WZ3_API int wz3_light_intersects_bounds(const wz3_light_job *light, const wz3_bounds *bounds)
{
  if (light->directional) return 1;
  if (!wz3__finite(light->range) || light->range < 0) return 1;
  const double position[3] = {light->position[0], light->position[1], light->position[2]};
  return wz3_bounds_intersect_sphere(bounds, position, light->range);
}

/* ---- frusta ---- */

WZ3_API uint32_t wz3_view_frustum_planes(const float vp[16], double planes[5][4])
{
  if (!wz3__all_finite_f(vp, 16)) return 0;
  const double row0[4] = {vp[0], vp[4], vp[8], vp[12]}, row1[4] = {vp[1], vp[5], vp[9], vp[13]};
  const double row2[4] = {vp[2], vp[6], vp[10], vp[14]}, row3[4] = {vp[3], vp[7], vp[11], vp[15]};
  double candidates[5][4];
  for (int k = 0; k < 4; k++) {
    candidates[0][k] = row3[k] * 1 + row0[k] * 1;
    candidates[1][k] = row3[k] * 1 + row0[k] * -1;
    candidates[2][k] = row3[k] * 1 + row1[k] * 1;
    candidates[3][k] = row3[k] * 1 + row1[k] * -1;
    candidates[4][k] = row3[k] * 1 + row2[k] * 1;
  }
  uint32_t n = 0;
  for (int i = 0; i < 5; i++) {
    const double length = wz3__js_hypot3(candidates[i][0], candidates[i][1], candidates[i][2]);
    if (!(length > 1e-20) || !wz3__finite(length)) continue;
    const double inverse = 1 / length;
    for (int k = 0; k < 4; k++) planes[n][k] = candidates[i][k] * inverse;
    n++;
  }
  return n;
}

WZ3_API int wz3_light_intersects_frustum(const wz3_light_job *light, const double (*planes)[4], uint32_t n_planes)
{
  if (light->directional || !n_planes) return 1;
  const double p[3] = {light->position[0], light->position[1], light->position[2]};
  const double radius = light->range;
  if (!wz3__finite(radius) || radius < 0 || !wz3__finite(p[0]) || !wz3__finite(p[1]) || !wz3__finite(p[2])) return 1;
  for (uint32_t i = 0; i < n_planes; i++) {
    const double *plane = planes[i];
    const double distance = plane[0] * p[0] + plane[1] * p[1] + plane[2] * p[2] + plane[3];
    const double tolerance = 1e-7 * (1 + radius + fabs(plane[0] * p[0]) + fabs(plane[1] * p[1]) + fabs(plane[2] * p[2]) + fabs(plane[3]));
    if (distance < -radius - tolerance) return 0;
  }
  return 1;
}

WZ3_API uint32_t wz3_shadow_view_frustum_planes(const float vp[16], const float light[3], double planes[5][4])
{
  if (!wz3__all_finite_f(vp, 16) || !wz3__all_finite_f(light, 3)) return 0;
  const double row0[4] = {vp[0], vp[4], vp[8], vp[12]}, row1[4] = {vp[1], vp[5], vp[9], vp[13]};
  const double row2[4] = {vp[2], vp[6], vp[10], vp[14]}, row3[4] = {vp[3], vp[7], vp[11], vp[15]};
  for (int k = 0; k < 4; k++) {
    planes[0][k] = row3[k] * 1 + row0[k] * 1;
    planes[1][k] = row3[k] * 1 + row0[k] * -1;
    planes[2][k] = row3[k] * 1 + row1[k] * 1;
    planes[3][k] = row3[k] * 1 + row1[k] * -1;
    planes[4][k] = row3[k] * 1 + row2[k] * 1;
  }
  for (int i = 0; i < 5; i++) {
    const double length = wz3__js_hypot3(planes[i][0], planes[i][1], planes[i][2]);
    if (!(length > 1e-20)) continue;
    const double distance = planes[i][0] * light[0] + planes[i][1] * light[1] + planes[i][2] * light[2] + planes[i][3];
    if (distance < 0) planes[i][3] -= distance;
  }
  return 5;
}

WZ3_API int wz3_light_sphere_bounds(const float position[3], double radius, const float view[16], double zoom_x, double zoom_y, double rect[4])
{
  if (!wz3__all_finite_f(position, 3) || !wz3__all_finite_f(view, 16) || !wz3__finite(zoom_x) || !wz3__finite(zoom_y)) return 0;
  const double cx = (double)view[0] * position[0] + (double)view[4] * position[1] + (double)view[8] * position[2] + view[12];
  const double cy = (double)view[1] * position[0] + (double)view[5] * position[1] + (double)view[9] * position[2] + view[13];
  const double cz = (double)view[2] * position[0] + (double)view[6] * position[1] + (double)view[10] * position[2] + view[14];
  rect[0] = -1; rect[1] = -1; rect[2] = 1; rect[3] = 1;
  if (!wz3__finite(radius) || radius < 0) return 1;
  const double components[2] = {cx, cy}, zoom[2] = {zoom_x, zoom_y};
  for (int axis = 0; axis < 2; axis++) {
    const double ci = components[axis];
    const double distance_squared = ci * ci + cz * cz;
    double tangent = distance_squared - radius * radius;
    if (!(tangent > 0)) continue;
    tangent = sqrt(tangent);
    double sine = ci * tangent - cz * radius;
    double cosine = ci * radius + cz * tangent;
    if (cz * distance_squared > -radius * sine) rect[axis] = wz3__js_max(-1, sine / cosine * zoom[axis]);
    sine = cz * radius + ci * tangent;
    cosine = cz * tangent - ci * radius;
    if (cz * distance_squared > radius * sine) rect[axis + 2] = wz3__js_min(1, sine / cosine * zoom[axis]);
  }
  return 1;
}

static void wz3__plane_from_points(const double a[3], const double b[3], const double c[3], double out[4])
{
  const double abx = b[0] - a[0], aby = b[1] - a[1], abz = b[2] - a[2];
  const double acx = c[0] - a[0], acy = c[1] - a[1], acz = c[2] - a[2];
  const double x = aby * acz - abz * acy, y = abz * acx - abx * acz, z = abx * acy - aby * acx;
  out[0] = x; out[1] = y; out[2] = z;
  out[3] = -(x * a[0] + y * a[1] + z * a[2]);
}

WZ3_API uint32_t wz3_shadow_zfail_planes(const wz3_camera *camera, const wz3_light_job *light, const float *view_in,
                                         const double *rectangle, double planes[6][4])
{
  const float *cs = camera->camera_space;
  if (!wz3__all_finite_f(cs, 16) || !wz3__all_finite_f(light->position, 3)) return 0;
  float computed[16];
  const float *view = view_in;
  if (!view) {
    if (!wz3_mat4_inverse(cs, computed)) return 0;
    view = computed;
  }
  const double near = wz3__js_max(1e-5, fabs(wz3__or_default(camera->near_clip, 0.125)));
  const double zoom_x = wz3__or_default(camera->zoom_x, 1.0), zoom_y = wz3__or_default(camera->zoom_y, 1.0);
  double rect[4];
  if (rectangle) memcpy(rect, rectangle, sizeof rect);
  else if (!wz3_light_sphere_bounds(light->position, light->range, view, zoom_x, zoom_y, rect)) return 0;
  if (!wz3__finite(rect[0]) || !wz3__finite(rect[1]) || !wz3__finite(rect[2]) || !wz3__finite(rect[3]) ||
      !(rect[0] < rect[2]) || !(rect[1] < rect[3])) return 0;
  const double xs = near / zoom_x, ys = near / zoom_y;
  if (!wz3__finite(xs) || !wz3__finite(ys)) return 0;
  double points[4][3];
  for (int index = 0; index < 4; index++) {
    const double x = xs * ((index & 1) ? rect[2] : rect[0]);
    const double y = ys * ((index & 2) ? rect[3] : rect[1]);
    points[index][0] = (double)cs[12] + (double)cs[0] * x + (double)cs[4] * y + (double)cs[8] * near;
    points[index][1] = (double)cs[13] + (double)cs[1] * x + (double)cs[5] * y + (double)cs[9] * near;
    points[index][2] = (double)cs[14] + (double)cs[2] * x + (double)cs[6] * y + (double)cs[10] * near;
  }
  const double lp[3] = {light->position[0], light->position[1], light->position[2]};
  wz3__plane_from_points(points[0], points[1], points[2], planes[0]);
  const double near_distance = planes[0][0] * lp[0] + planes[0][1] * lp[1] + planes[0][2] * lp[2] + planes[0][3];
  const double on_epsilon = 1e-4;
  if (fabs(near_distance) >= on_epsilon) {
    wz3__plane_from_points(lp, points[1], points[0], planes[1]);
    wz3__plane_from_points(lp, points[3], points[1], planes[2]);
    wz3__plane_from_points(lp, points[2], points[3], planes[3]);
    wz3__plane_from_points(lp, points[0], points[2], planes[4]);
    if (near_distance < 0)
      for (int p = 0; p < 5; p++) for (int k = 0; k < 4; k++) planes[p][k] *= -1;
    const double x = (double)cs[12] - lp[0] + (double)cs[8] * near;
    const double y = (double)cs[13] - lp[1] + (double)cs[9] * near;
    const double z = (double)cs[14] - lp[2] + (double)cs[10] * near;
    planes[5][0] = x; planes[5][1] = y; planes[5][2] = z;
    planes[5][3] = -(x * lp[0] + y * lp[1] + z * lp[2]);
    return 6;
  }
  for (int k = 0; k < 4; k++) planes[1][k] = planes[0][k] * -1;
  planes[0][3] += on_epsilon;
  planes[1][3] -= on_epsilon;
  return 2;
}

WZ3_API int wz3_caster_uses_zfail(const wz3_bounds *bounds, const double (*planes)[4], uint32_t n_planes)
{
  if (!bounds || !n_planes) return 1;
  for (uint32_t i = 0; i < n_planes; i++) {
    const double *plane = planes[i];
    if (!wz3__finite(plane[0]) || !wz3__finite(plane[1]) || !wz3__finite(plane[2]) || !wz3__finite(plane[3])) return 1;
    const double x = plane[0] > 0 ? bounds->max[0] : bounds->min[0];
    const double y = plane[1] > 0 ? bounds->max[1] : bounds->min[1];
    const double z = plane[2] > 0 ? bounds->max[2] : bounds->min[2];
    if (plane[0] * x + plane[1] * y + plane[2] * z + plane[3] < -1e-6) return 0;
  }
  return 1;
}

WZ3_API int wz3_bounds_outside_planes(const wz3_bounds *bounds, const double (*planes)[4], uint32_t n_planes)
{
  if (!bounds || !n_planes) return 0;
  for (uint32_t i = 0; i < n_planes; i++) {
    const double *plane = planes[i];
    const double length = wz3__js_hypot3(plane[0], plane[1], plane[2]);
    if (!(length > 1e-20)) continue;
    const double x = plane[0] >= 0 ? bounds->max[0] : bounds->min[0];
    const double y = plane[1] >= 0 ? bounds->max[1] : bounds->min[1];
    const double z = plane[2] >= 0 ? bounds->max[2] : bounds->min[2];
    const double distance = plane[0] * x + plane[1] * y + plane[2] * z + plane[3];
    const double tolerance = 1e-6 * (1 + fabs(plane[0] * x) + fabs(plane[1] * y) + fabs(plane[2] * z) + fabs(plane[3]));
    if (distance < -tolerance) return 1;
  }
  return 0;
}

WZ3_API int wz3_caster_may_affect_view(const wz3_light_job *light, const wz3_bounds *bounds, const float vp[16])
{
  if (light->directional) return 1;
  double planes[5][4];
  if (!wz3_shadow_view_frustum_planes(vp, light->position, planes)) return 1;
  return !wz3_bounds_outside_planes(bounds, (const double (*)[4])planes, 5);
}

/* ---- light selection ---- */

WZ3_API uint32_t wz3_select_active_lights(const wz3_frame *frame, const wz3_ipp_node *viewport, uint32_t limit, uint32_t *indices)
{
  const float *camera = viewport->camera.camera_space;
  double position[3] = {0, 0, 0};
  if (wz3__finite(camera[12]) && wz3__finite(camera[13]) && wz3__finite(camera[14])) {
    position[0] = camera[12]; position[1] = camera[13]; position[2] = camera[14];
  }
  wz3_camera_mats matrices;
  wz3_camera_matrices(&viewport->camera, &matrices);
  double planes[5][4];
  const uint32_t n_planes = wz3_view_frustum_planes(matrices.view_projection, planes);
  /* candidates (stable insertion sort by importance, strongest first; ties keep their order) */
  typedef struct { uint32_t index; double importance; } candidate;
  candidate list[256];
  uint32_t count = 0;
  for (uint32_t i = 0; i < viewport->n_light && count < 256; i++) {
    const wz3_light_job *light = &frame->light_jobs[viewport->first_light + i];
    if (!((double)wz3__or_default((double)light->amplify, 0.0) >= 1.0 / 255.0)) continue;
    if (!wz3_light_intersects_frustum(light, (const double (*)[4])planes, n_planes)) continue;
    const double dx = (double)light->position[0] - position[0], dy = (double)light->position[1] - position[1];
    const double dz = (double)light->position[2] - position[2];
    const double distance = (wz3__finite(dx) && wz3__finite(dy) && wz3__finite(dz)) ? wz3__js_hypot3(dx, dy, dz) : (double)INFINITY;
    const double range = light->range;
    const double importance = wz3__finite(range) ? range / wz3__js_max(distance, 0.1)
                            : range == (double)INFINITY ? (double)INFINITY : -(double)INFINITY;
    uint32_t at = count;
    while (at > 0 && list[at - 1].importance < importance) { list[at] = list[at - 1]; at--; }
    list[at].index = i;
    list[at].importance = importance;
    count++;
  }
  if (count == 0) {
    if (limit) indices[0] = (uint32_t)-1;
    return limit ? 1 : 0;
  }
  if (count > limit) count = limit;
  for (uint32_t i = 0; i < count; i++) indices[i] = list[i].index;
  return count;
}

/* ---- viewport regions ---- */

WZ3_API uint32_t wz3_viewport_clear_flags(uint32_t flags) { return flags & 3u; }

WZ3_API void wz3_fit_aspect_region(int32_t width, int32_t height, double aspect, wz3_region *out)
{
  width = width < 1 ? 1 : width;
  height = height < 1 ? 1 : height;
  memset(out, 0, sizeof *out);
  if (!(aspect > 0) || !wz3__finite(aspect)) { out->x = 0; out->y = 0; out->width = width; out->height = height; return; }
  double inner_width = width;
  double inner_height = wz3__js_max(1, floor((double)width / aspect));
  if (inner_height > height) inner_height = height;
  inner_width = wz3__js_max(1, floor(inner_height * aspect));
  inner_width = wz3__js_min(width, inner_width);
  inner_height = wz3__js_min(height, inner_height);
  out->x = (int32_t)floor((width - inner_width) / 2);
  out->y = (int32_t)floor((height - inner_height) / 2);
  out->width = (int32_t)inner_width;
  out->height = (int32_t)inner_height;
}

WZ3_API void wz3_viewport_region(const float crop[4], int32_t width, int32_t height, wz3_region *out)
{
  width = width < 1 ? 1 : width;
  height = height < 1 ? 1 : height;
  double x0 = 0, y0 = 0, x1 = width, y1 = height;
  if (crop) {
    const double fx0 = crop[0], fy0 = crop[1], fx1 = crop[2], fy1 = crop[3];
    if (wz3__finite(fx0) && wz3__finite(fy0) && wz3__finite(fx1) && wz3__finite(fy1) && fx0 < fx1 && fy0 < fy1) {
      x0 = wz3__js_trunc(width * fx0);
      x1 = wz3__js_trunc(width * fx1);
      const double top = wz3__js_trunc(height * fy0), bottom = wz3__js_trunc(height * fy1);
      y0 = height - bottom;
      y1 = height - top;
    }
  }
  x0 = wz3__js_max(0, wz3__js_min(width, x0)); x1 = wz3__js_max(0, wz3__js_min(width, x1));
  y0 = wz3__js_max(0, wz3__js_min(height, y0)); y1 = wz3__js_max(0, wz3__js_min(height, y1));
  if (x1 < x0) x1 = x0;
  if (y1 < y0) y1 = y0;
  out->x = (int32_t)x0; out->y = (int32_t)y0; out->width = (int32_t)(x1 - x0); out->height = (int32_t)(y1 - y0);
  out->uv[0] = (float)(x0 / width); out->uv[1] = (float)(y0 / height); out->uv[2] = (float)(x1 / width); out->uv[3] = (float)(y1 / height);
}

WZ3_API void wz3_light_scissor_region(const double *rect, const wz3_region *viewport, wz3_region *out)
{
  const double x = viewport->x, y = viewport->y;
  const double width = wz3__js_max(0, viewport->width), height = wz3__js_max(0, viewport->height);
  memset(out, 0, sizeof *out);
  out->x = (int32_t)x; out->y = (int32_t)y; out->width = (int32_t)width; out->height = (int32_t)height;
  if (!rect || !wz3__finite(rect[0]) || !wz3__finite(rect[1]) || !wz3__finite(rect[2]) || !wz3__finite(rect[3])) return;
  const double left = wz3__js_max(0, wz3__js_min(width, wz3__js_trunc(width * (1 + rect[0]) * 0.5)));
  const double right = wz3__js_max(0, wz3__js_min(width, wz3__js_trunc(width * (1 + rect[2]) * 0.5)));
  const double top = wz3__js_max(0, wz3__js_min(height, wz3__js_trunc(height * (1 - rect[3]) * 0.5)));
  const double bottom = wz3__js_max(0, wz3__js_min(height, wz3__js_trunc(height * (1 - rect[1]) * 0.5)));
  out->x = (int32_t)(x + left);
  out->y = (int32_t)(y + height - bottom);
  out->width = (int32_t)wz3__js_max(0, right - left);
  out->height = (int32_t)wz3__js_max(0, bottom - top);
}

WZ3_API void wz3_layer_quad_geometry(const wz3_ipp_node *layer, float positions[12], float uvs[8])
{
  const double x0 = layer->screen[0] * 2 - 1, x1 = layer->screen[2] * 2 - 1;
  const double y0 = 1 - layer->screen[1] * 2, y1 = 1 - layer->screen[3] * 2;
  const double depth = wz3__finite((double)layer->z) ? (double)layer->z * 2 - 1 : -1;
  const double p[12] = {x0, y0, depth, x1, y0, depth, x1, y1, depth, x0, y1, depth};
  for (int i = 0; i < 12; i++) positions[i] = (float)p[i];
  const double *t = layer->uv;
  const double u[8] = {t[0], t[1], t[2], t[1], t[2], t[3], t[0], t[3]};
  for (int i = 0; i < 8; i++) uvs[i] = (float)u[i];
}

/* ---- draw-list order ---- */

static int32_t wz3__item_light_order(const wz3_render_item *item)
{
  if (item->is_effect) return 0;
  if (item->usage == WZ3_USAGE_SHADOW || item->usage == WZ3_USAGE_LIGHT) return item->light_index;
  return wz3__fj_effect_usage_order(item->usage) < wz3__fj_effect_usage_order(WZ3_USAGE_SHADOW) ? 0 : 0x0f;
}

static int wz3__item_compare(const void *pa, const void *pb)
{
  const wz3_render_item *a = (const wz3_render_item *)pa, *b = (const wz3_render_item *)pb;
  int64_t d = (int64_t)a->render_pass - b->render_pass;
  if (d) return d < 0 ? -1 : 1;
  d = (int64_t)wz3__item_light_order(a) - wz3__item_light_order(b);
  if (d) return d < 0 ? -1 : 1;
  d = (int64_t)wz3__fj_effect_usage_order(a->usage) - wz3__fj_effect_usage_order(b->usage);
  if (d) return d < 0 ? -1 : 1;
  d = (int64_t)a->sort_material - b->sort_material;
  if (d) return d < 0 ? -1 : 1;
  d = (int64_t)a->sort_order - b->sort_order;
  if (d) return d < 0 ? -1 : 1;
  return a->sequence < b->sequence ? -1 : a->sequence > b->sequence ? 1 : 0;
}

WZ3_API void wz3_sort_render_items(wz3_render_item *items, uint32_t n)
{
  qsort(items, n, sizeof *items, wz3__item_compare);
}

/* ---- lighting math ---- */

static void wz3__normalize3(double x, double y, double z, const double fallback[3], double out[3])
{
  const double length = wz3__js_hypot3(x, y, z);
  if (!(length > 1e-10)) { out[0] = fallback[0]; out[1] = fallback[1]; out[2] = fallback[2]; return; }
  out[0] = x / length; out[1] = y / length; out[2] = z / length;
}

WZ3_API void wz3_tangent_basis_normal(const double normal[3], const double tangent[3], const double mapped[3], double out[3])
{
  static const double up[3] = {0, 0, 1}, right[3] = {1, 0, 0}, forward[3] = {0, 1, 0};
  double n[3], t[3], b[3];
  wz3__normalize3(normal[0], normal[1], normal[2], up, n);
  const double projection = n[0] * tangent[0] + n[1] * tangent[1] + n[2] * tangent[2];
  wz3__normalize3(tangent[0] - n[0] * projection, tangent[1] - n[1] * projection, tangent[2] - n[2] * projection, right, t);
  wz3__normalize3(n[1] * t[2] - n[2] * t[1], n[2] * t[0] - n[0] * t[2], n[0] * t[1] - n[1] * t[0], forward, b);
  wz3__normalize3(n[0] * mapped[0] + b[0] * mapped[1] + t[0] * mapped[2], n[1] * mapped[0] + b[1] * mapped[1] + t[1] * mapped[2],
                  n[2] * mapped[0] + b[2] * mapped[1] + t[2] * mapped[2], n, out);
}

WZ3_API double wz3_material_light_attenuation(double d) { return wz3__finite(d) ? wz3__js_max(0, 1 - d * d) : 0.0; }

WZ3_API void wz3_legacy_transr_vector(const float m[16], const double v[3], int point, float out[3])
{
  const double dx = wz3__or_default(v[0], 0.0) - (point ? wz3__or_default((double)m[12], 0.0) : 0.0);
  const double dy = wz3__or_default(v[1], 0.0) - (point ? wz3__or_default((double)m[13], 0.0) : 0.0);
  const double dz = wz3__or_default(v[2], 0.0) - (point ? wz3__or_default((double)m[14], 0.0) : 0.0);
  out[0] = (float)((double)m[0] * dx + (double)m[1] * dy + (double)m[2] * dz);
  out[1] = (float)((double)m[4] * dx + (double)m[5] * dy + (double)m[6] * dz);
  out[2] = (float)((double)m[8] * dx + (double)m[9] * dy + (double)m[10] * dz);
}

WZ3_API void wz3_legacy_vertex_lighting(const double position[3], const double normal[3], const double tangent[3], const double light[4],
                                        const double attenuation[4], const double camera[3], int condition_basis, wz3_vertex_lighting *out)
{
  static const double up[3] = {0, 0, 1}, right[3] = {1, 0, 0}, forward[3] = {0, 1, 0};
  double p[3], n[3], t[3];
  for (int i = 0; i < 3; i++) {
    p[i] = wz3__or_default(position[i], 0.0);
    n[i] = wz3__or_default(normal[i], 0.0);
    t[i] = wz3__or_default(tangent[i], 0.0);
  }
  if (condition_basis) {
    double c[3];
    wz3__normalize3(n[0], n[1], n[2], up, c);
    n[0] = c[0]; n[1] = c[1]; n[2] = c[2];
    const double projection = n[0] * t[0] + n[1] * t[1] + n[2] * t[2];
    wz3__normalize3(t[0] - n[0] * projection, t[1] - n[1] * projection, t[2] - n[2] * projection, right, c);
    t[0] = c[0]; t[1] = c[1]; t[2] = c[2];
  }
  const double b[3] = {n[1] * t[2] - n[2] * t[1], n[2] * t[0] - n[0] * t[2], n[0] * t[1] - n[1] * t[0]};
  const double light_w = wz3__or_default(light[3], 0.0);
  const double l[3] = {wz3__or_default(light[0], 0.0) - p[0] * light_w, wz3__or_default(light[1], 0.0) - p[1] * light_w,
                       wz3__or_default(light[2], 0.0) - p[2] * light_w};
  double normalized_light[3], eye[3];
  wz3__normalize3(l[0], l[1], l[2], forward, normalized_light);
  wz3__normalize3(wz3__or_default(camera[0], 0.0) - p[0], wz3__or_default(camera[1], 0.0) - p[1], wz3__or_default(camera[2], 0.0) - p[2], forward, eye);
  const double h[3] = {normalized_light[0] + eye[0], normalized_light[1] + eye[1], normalized_light[2] + eye[2]};
  const double inverse_range = wz3__or_default(attenuation[3], 0.0);
  for (int i = 0; i < 3; i++) {
    out->normal[i] = (float)n[i];
    out->tangent[i] = (float)t[i];
    out->bitangent[i] = (float)b[i];
    out->attenuation[i] = (float)((p[i] - wz3__or_default(attenuation[i], 0.0)) * inverse_range);
  }
  out->light[0] = (float)(l[0] * n[0] + l[1] * n[1] + l[2] * n[2]);
  out->light[1] = (float)(l[0] * b[0] + l[1] * b[1] + l[2] * b[2]);
  out->light[2] = (float)(l[0] * t[0] + l[1] * t[1] + l[2] * t[2]);
  out->halfway[0] = (float)(h[0] * n[0] + h[1] * n[1] + h[2] * n[2]);
  out->halfway[1] = (float)(h[0] * b[0] + h[1] * b[1] + h[2] * b[2]);
  out->halfway[2] = (float)(h[0] * t[0] + h[1] * t[1] + h[2] * t[2]);
}

WZ3_API double wz3_material11_specular(double cosine, double power)
{
  const double use_power = wz3__js_max(8, wz3__js_min(60, wz3__or_default(power, 0.0)));
  const double inverse_scale = 1 / 7.336032;
  const double scale = inverse_scale + (use_power - 8) * 0.01565302;
  const double bias = inverse_scale - scale;
  double value = wz3__js_max(0, wz3__js_min(1, (wz3__js_max(0, wz3__or_default(cosine, 0.0)) * scale + bias) * 4));
  value = value * value * 2;
  value = value * value * 2;
  return wz3__js_max(0, wz3__js_min(1, value * value * 2));
}

WZ3_API void wz3_material11_light_constant(uint32_t material_color, uint32_t light_color, double amplify, float out[4])
{
  static const int shifts[4] = {16, 8, 0, 24};
  const double scale = (float)wz3__or_default(amplify, 0.0);
  for (int channel = 0; channel < 4; channel++) {
    const int shift = shifts[channel];
    const double material = (float)((double)((material_color >> shift) & 255u) / 255.0);
    const double light = (float)((double)((light_color >> shift) & 255u) / 255.0);
    const double value = (float)((double)(float)(material * light) * scale);
    out[channel] = (float)wz3__js_max(-1, wz3__js_min(1, value));
  }
}

/* ---- post-processing plans ---- */

static double wz3__param(const double *p, uint32_t n, uint32_t i, double fallback) { return i < n ? p[i] : fallback; }   /* parameters[i] ?? fallback */
static uint32_t wz3__param_u32(const double *p, uint32_t n, uint32_t i) { return i < n ? (uint32_t)wz3__rt_to_int32(p[i]) : 0u; }

static void wz3__color_argb(uint32_t value, float out[4])
{
  out[0] = (float)((double)((value >> 16) & 255u) / 255.0);
  out[1] = (float)((double)((value >> 8) & 255u) / 255.0);
  out[2] = (float)((double)(value & 255u) / 255.0);
  out[3] = (float)((double)(value >> 24) / 255.0);
}

WZ3_API void wz3_color_correction_constants(const double *parameters, uint32_t n, float out[28])
{
  memset(out, 0, 28 * sizeof(float));
  const double threshold0 = wz3__param(parameters, n, 1, 0.0), threshold1 = wz3__param(parameters, n, 2, 0.0);
  const double denominator = threshold1 - threshold0;
  const double threshold_offset = -threshold0 / denominator;
  const double threshold_scale = 1 / denominator;
  if (wz3__param_u32(parameters, n, 7) & 1u) {
    for (int k = 0; k < 4; k++) out[8 + k] = 1.0f;
    out[16] = (float)threshold_offset; out[17] = (float)threshold_scale; out[18] = 1.0f; out[19] = 0.0f;
    return;
  }
  wz3__color_argb(wz3__param_u32(parameters, n, 3), out + 0);
  wz3__color_argb(wz3__param_u32(parameters, n, 4), out + 4);
  float brightness[4];
  wz3__color_argb(wz3__param_u32(parameters, n, 5), brightness);
  const double amplify = wz3__param(parameters, n, 10, 0.0);
  for (int channel = 0; channel < 4; channel++) brightness[channel] = (float)((double)brightness[channel] * amplify);
  memcpy(out + 8, brightness, sizeof brightness);
  wz3__color_argb(wz3__param_u32(parameters, n, 6), out + 12);
  out[16] = (float)threshold_offset; out[17] = (float)threshold_scale; out[18] = 0.0f; out[19] = 0.0f;
  wz3__color_argb(wz3__param_u32(parameters, n, 8), out + 20);
  wz3__color_argb(wz3__param_u32(parameters, n, 9), out + 24);
}

static double wz3__saturate(double v) { return wz3__js_max(0, wz3__js_min(1, v)); }

WZ3_API void wz3_evaluate_color_correction(const double *parameters, uint32_t n, const double source[4], float out[4])
{
  float c[28];
  wz3_color_correction_constants(parameters, n, c);
  const double gray = (source[0] + source[1] + source[2]) / 3;
  const double range = wz3__js_max(0, wz3__js_min(1, gray * c[17] + c[16]));
  for (int channel = 0; channel < 3; channel++) {
    const double dark = gray * c[20 + channel] + source[channel] * c[channel];
    const double light = gray * c[24 + channel] + source[channel] * c[4 + channel];
    const double mixed = dark * range + light * (1 - range) + (double)c[18] * range;
    out[channel] = (float)wz3__saturate(wz3__js_max(0, wz3__js_min(1, mixed - c[12 + channel])) * c[8 + channel]);
  }
  out[3] = (float)source[3];
}

WZ3_API void wz3_glare_plan_make(const double *parameters, uint32_t n, wz3_glare_plan *out)
{
  memset(out, 0, sizeof *out);
  const int32_t flags = n > 1 ? wz3__rt_to_int32(parameters[1]) : 0;
  out->flags = flags;
  if ((flags & 3) == 1) { out->downsample[0] = 0; out->downsample[1] = 1; }
  else if ((flags & 3) == 2) { out->downsample[0] = 1; out->downsample[1] = 1; }
  else { out->downsample[0] = 1; out->downsample[1] = 0; }
  out->copy_downsample = (flags & 4) != 0;
  wz3__color_argb(wz3__param_u32(parameters, n, 2), out->threshold);
  const double inverse_red = (float)(1 - (double)out->threshold[0]);
  const double inverse_green = (float)(1 - (double)out->threshold[1]);
  const double inverse_blue = (float)(1 - (double)out->threshold[2]);
  const double gray_denominator = (float)((double)(float)(inverse_red + inverse_green) + inverse_blue);
  out->gray_scale = (float)(1 / gray_denominator);
  out->gray_mix = wz3__param(parameters, n, 9, 0.0);
  out->blur[0] = wz3__param(parameters, n, 5, 0.0); out->amplify[0] = wz3__param(parameters, n, 7, 0.0);
  out->blur[1] = wz3__param(parameters, n, 6, 0.0); out->amplify[1] = wz3__param(parameters, n, 8, 0.0);
  wz3__color_argb(wz3__param_u32(parameters, n, 3), out->glare_color);
  wz3__color_argb(wz3__param_u32(parameters, n, 4), out->original_color);
  out->add_smooth = wz3__param(parameters, n, 10, 0.0);
}

WZ3_API void wz3_evaluate_glare_tone(const double *parameters, uint32_t n, const double source[4], float out[4])
{
  wz3_glare_plan plan;
  wz3_glare_plan_make(parameters, n, &plan);
  const double color[3] = {source[0] - plan.threshold[0], source[1] - plan.threshold[1], source[2] - plan.threshold[2]};
  const double gray = (color[0] + color[1] + color[2]) * plan.gray_scale;
  const double inverse_mix = 1 - plan.gray_mix;
  out[0] = (float)wz3__saturate(color[0] * plan.gray_mix + gray * inverse_mix);
  out[1] = (float)wz3__saturate(color[1] * plan.gray_mix + gray * inverse_mix);
  out[2] = (float)wz3__saturate(color[2] * plan.gray_mix + gray * inverse_mix);
  out[3] = (float)wz3__saturate(source[3] * plan.gray_mix + gray * inverse_mix);
}

WZ3_API void wz3_evaluate_glare_composite(const double *parameters, uint32_t n, const double glare[4], const double original[4], float out[4])
{
  wz3_glare_plan plan;
  wz3_glare_plan_make(parameters, n, &plan);
  for (int channel = 0; channel < 4; channel++) {
    const double blurred = glare[channel] * plan.glare_color[channel];
    const double source = original[channel] * plan.original_color[channel];
    out[channel] = (float)wz3__saturate(blurred + source - blurred * source * plan.add_smooth);
  }
}

/* ---- effect geometry ---- */

WZ3_API uint32_t wz3_build_chain_ribbon(const float *points, uint32_t count, double thickness, int32_t ripped, const float *view,
                                        float *positions, float *uvs, uint16_t *indices)
{
  static const double right[3] = {1, 0, 0}, forward[3] = {0, 0, 1};
  double axis[3];
  if (view) wz3__normalize3(view[2], view[6], view[10], forward, axis);
  else wz3__normalize3(0, 0, 1, forward, axis);
  const double half = wz3__or_default(thickness, 0.0) * 0.5;
  for (uint32_t index = 0; index < count; index++) {
    const float *before = points + (size_t)(index > 0 ? index - 1 : 0) * 3;
    const float *after = points + (size_t)(index + 1 < count ? index + 1 : count - 1) * 3;
    const float *point = points + (size_t)index * 3;
    double tangent[3];
    wz3__normalize3((double)before[0] - after[0], (double)before[1] - after[1], (double)before[2] - after[2], right, tangent);
    const double side[3] = {tangent[1] * axis[2] - tangent[2] * axis[1], tangent[2] * axis[0] - tangent[0] * axis[2],
                            tangent[0] * axis[1] - tangent[1] * axis[0]};
    float *p = positions + (size_t)index * 6;
    for (int k = 0; k < 3; k++) {
      p[k] = (float)((double)point[k] + side[k] * half);
      p[3 + k] = (float)((double)point[k] - side[k] * half);
    }
    float *uv = uvs + (size_t)index * 4;
    uv[0] = uv[2] = (float)index;
    uv[1] = 0.0f; uv[3] = 1.0f;
  }
  uint32_t n = 0;
  for (uint32_t index = 0; index + 1 < count; index++) {
    if ((int32_t)index == ripped) continue;
    const uint32_t vertex = index * 2;
    indices[n++] = (uint16_t)(vertex + 3); indices[n++] = (uint16_t)(vertex + 2); indices[n++] = (uint16_t)vertex;
    indices[n++] = (uint16_t)(vertex + 3); indices[n++] = (uint16_t)vertex; indices[n++] = (uint16_t)(vertex + 1);
  }
  return n;
}

WZ3_API void wz3_effect_render_item_make(const wz3_effect *effect, const wz3_effect_job *job, int32_t sort_order, wz3_render_item *out)
{
  int64_t pass = (int64_t)effect->pass + (int64_t)job->pass_adjust;
  memset(out, 0, sizeof *out);
  out->is_effect = 1;
  out->render_pass = (int32_t)(pass < 0 ? 0 : pass > 255 ? 255 : pass);
  out->usage = effect->usage;
  out->sort_material = 0;
  out->sort_order = sort_order;
}

WZ3_API void wz3_effect_render_item(const wz3_rt *rt, const wz3_effect_job *job, int32_t sort_order, wz3_render_item *out)
{
  wz3_effect_render_item_make((const wz3_effect *)&((const wz3__fj_effect *)rt->ops[job->effect_op].cache.obj)->pub, job, sort_order, out);
}
