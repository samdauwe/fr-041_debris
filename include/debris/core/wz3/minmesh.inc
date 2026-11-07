/* ======================================================================== */
/*  GenMinMesh (genminmesh.cpp)                                             */
/*                                                                          */
/*  The compact polygon mesh of fr-041: vertices with all attributes and    */
/*  four bone lanes, polygons (fan-triangulated only by prepare), clusters  */
/*  (material, pass, animation) and an optional bone-matrix animation. The  */
/*  JS port keeps this in plain objects; here it is arrays of records with  */
/*  one shared corner pool. Float32 rounding follows the JS expression      */
/*  order (binary64 operation, then (float)).                               */
/* ======================================================================== */

#define WZ3__MM_EPSILON 1e-20

typedef struct wz3__mm_vertex {
  int32_t select, bone_count, temp_byte, merge_tag;
  uint32_t color;
  float position[3], normal[3], tangent[3], uv[2][2], weights[4];
  uint16_t matrices[4];
} wz3__mm_vertex;

typedef struct wz3__mm_face {
  int32_t select, count, cluster, temp;
  uint32_t flags;
  float normal[3];
  uint32_t first;                    /* offset of the corners in the shared pool */
} wz3__mm_face;

typedef struct wz3__mm_cluster {
  int32_t material, render_pass, id, anim_type, anim_matrix;
} wz3__mm_cluster;

enum { WZ3__MM_SPLINE_NONE = 0, WZ3__MM_SPLINE_EXTERNAL = 1, WZ3__MM_SPLINE_KINETIC = 2 };

typedef struct wz3__mm_bone {
  float base_pose[16], no_animation[16];
  int32_t parent;
  float factor, spread, offset;
  int32_t spline_kind, spline_op;    /* external: the operator that produced the spline */
  float kin_position[3], kin_speed[3], kin_gravity[3], kin_rotation[3], kin_axis[3], kin_angle;
  int32_t kin_has_axis;
} wz3__mm_bone;

struct wz3_minmesh {
  wz3__mm_vertex *verts;     uint32_t n_verts, cap_verts;
  wz3__mm_face *faces;       uint32_t n_faces, cap_faces;
  int32_t *corners;          uint32_t n_corners, cap_corners;
  wz3__mm_cluster *clusters; uint32_t n_clusters, cap_clusters;
  wz3__mm_bone *bones;       uint32_t n_bones;
  int completely_rigid, stripped, normals_valid;
  int oom;
};

static double wz3__mm_min(double a, double b) { return (a != a || b != b) ? NAN : (a < b ? a : b); }
static double wz3__mm_max(double a, double b) { return (a != a || b != b) ? NAN : (a > b ? a : b); }

static wz3__mm_cluster wz3__mm_make_cluster(int32_t material, int32_t render_pass, int32_t id, int32_t anim_type, int32_t anim_matrix)
{
  wz3__mm_cluster c;
  c.material = material; c.render_pass = render_pass; c.id = id; c.anim_type = anim_type; c.anim_matrix = anim_matrix;
  return c;
}

static int wz3__mm_push_cluster(wz3_minmesh *m, wz3__mm_cluster c)
{
  WZ3__MESH_RESERVE(m, clusters, n_clusters, cap_clusters, wz3__mm_cluster, m->n_clusters + 1);
  m->clusters[m->n_clusters++] = c;
  return 0;
}

static wz3_minmesh *wz3__mm_new(void)
{
  wz3_minmesh *m = (wz3_minmesh *)WZ3_MALLOC(sizeof *m);
  if (!m) return NULL;
  memset(m, 0, sizeof *m);
  if (wz3__mm_push_cluster(m, wz3__mm_make_cluster(WZ3__MAT_NULL, 0, 0, 0, -1)) ||
      wz3__mm_push_cluster(m, wz3__mm_make_cluster(WZ3__MAT_NULL, 0, 0, 0, -1))) {
    WZ3_FREE(m->clusters); WZ3_FREE(m);
    return NULL;
  }
  return m;
}

static void wz3__mm_free(wz3_minmesh *m)
{
  if (!m) return;
  WZ3_FREE(m->verts); WZ3_FREE(m->faces); WZ3_FREE(m->corners); WZ3_FREE(m->clusters); WZ3_FREE(m->bones);
  WZ3_FREE(m);
}

static void wz3__mm_vertex_init(wz3__mm_vertex *v) { memset(v, 0, sizeof *v); }

static int wz3__mm_push_vertex(wz3_minmesh *m, const wz3__mm_vertex *source, wz3__mm_vertex **out)
{
  WZ3__MESH_RESERVE(m, verts, n_verts, cap_verts, wz3__mm_vertex, m->n_verts + 1);
  wz3__mm_vertex *v = &m->verts[m->n_verts++];
  if (source) *v = *source; else wz3__mm_vertex_init(v);
  if (out) *out = v;
  return 0;
}

/* makeFace / cloneFace: `source` supplies select, cluster, temp, normal and flags */
static int wz3__mm_push_face(wz3_minmesh *m, const int32_t *vertices, int32_t count, int32_t cluster, int32_t select,
                             const wz3__mm_face *source)
{
  WZ3__MESH_RESERVE(m, faces, n_faces, cap_faces, wz3__mm_face, m->n_faces + 1);
  WZ3__MESH_RESERVE(m, corners, n_corners, cap_corners, int32_t, m->n_corners + (uint32_t)count);
  wz3__mm_face *f = &m->faces[m->n_faces++];
  memset(f, 0, sizeof *f);
  if (source) {
    f->select = source->select; f->cluster = source->cluster; f->temp = source->temp; f->flags = source->flags;
    memcpy(f->normal, source->normal, sizeof f->normal);
  } else {
    f->select = select; f->cluster = cluster;
  }
  f->count = count;
  f->first = m->n_corners;
  memcpy(m->corners + m->n_corners, vertices, (size_t)count * sizeof(int32_t));
  m->n_corners += (uint32_t)count;
  return 0;
}

static wz3_minmesh *wz3__mm_clone(const wz3_minmesh *s)
{
  wz3_minmesh *m = wz3__mm_new();
  if (!m) return NULL;
  m->n_clusters = 0;
  m->verts = (wz3__mm_vertex *)WZ3_MALLOC((size_t)(s->n_verts ? s->n_verts : 1) * sizeof(wz3__mm_vertex));
  m->faces = (wz3__mm_face *)WZ3_MALLOC((size_t)(s->n_faces ? s->n_faces : 1) * sizeof(wz3__mm_face));
  m->corners = (int32_t *)WZ3_MALLOC((size_t)(s->n_corners ? s->n_corners : 1) * sizeof(int32_t));
  WZ3_FREE(m->clusters);
  m->clusters = (wz3__mm_cluster *)WZ3_MALLOC((size_t)(s->n_clusters ? s->n_clusters : 1) * sizeof(wz3__mm_cluster));
  m->bones = s->n_bones ? (wz3__mm_bone *)WZ3_MALLOC((size_t)s->n_bones * sizeof(wz3__mm_bone)) : NULL;
  if (!m->verts || !m->faces || !m->corners || !m->clusters || (s->n_bones && !m->bones)) { wz3__mm_free(m); return NULL; }
  memcpy(m->verts, s->verts, (size_t)s->n_verts * sizeof(wz3__mm_vertex));
  memcpy(m->faces, s->faces, (size_t)s->n_faces * sizeof(wz3__mm_face));
  memcpy(m->corners, s->corners, (size_t)s->n_corners * sizeof(int32_t));
  memcpy(m->clusters, s->clusters, (size_t)s->n_clusters * sizeof(wz3__mm_cluster));
  m->n_verts = m->cap_verts = s->n_verts ? s->n_verts : 1; m->n_verts = s->n_verts;
  m->n_faces = s->n_faces; m->cap_faces = s->n_faces ? s->n_faces : 1;
  m->n_corners = s->n_corners; m->cap_corners = s->n_corners ? s->n_corners : 1;
  m->n_clusters = s->n_clusters; m->cap_clusters = s->n_clusters ? s->n_clusters : 1;
  m->n_bones = s->n_bones;
  for (uint32_t i = 0; i < s->n_bones; i++) {
    /* cloneMatrixRecord: factor is float32 (null -> 1), spread/offset are `|| 0` */
    m->bones[i] = s->bones[i];
    wz3__mm_bone *b = &m->bones[i];
    b->factor = (float)b->factor;
    b->spread = (b->spread != b->spread || b->spread == 0.0f) ? 0.0f : b->spread;
    b->offset = (b->offset != b->offset || b->offset == 0.0f) ? 0.0f : b->offset;
  }
  m->completely_rigid = s->completely_rigid;
  m->stripped = s->stripped;
  m->normals_valid = s->normals_valid;
  return m;
}

static const int32_t *wz3__mm_corners(const wz3_minmesh *m, const wz3__mm_face *f) { return m->corners + f->first; }

/* ---- vector helpers: binary64 operation, then one float32 rounding ---- */

static void wz3__mm_add3(float out[3], const float value[3], double scale)
{
  const float x = (float)((double)out[0] + (double)value[0] * scale);
  const float y = (float)((double)out[1] + (double)value[1] * scale);
  const float z = (float)((double)out[2] + (double)value[2] * scale);
  out[0] = x; out[1] = y; out[2] = z;
}

static void wz3__mm_sub3(const float a[3], const float b[3], float out[3])
{
  const float x = (float)((double)a[0] - (double)b[0]);
  const float y = (float)((double)a[1] - (double)b[1]);
  const float z = (float)((double)a[2] - (double)b[2]);
  out[0] = x; out[1] = y; out[2] = z;
}

static void wz3__mm_scale3(float out[3], double scale)
{
  out[0] = (float)((double)out[0] * scale);
  out[1] = (float)((double)out[1] * scale);
  out[2] = (float)((double)out[2] * scale);
}

static double wz3__mm_dot3(const float a[3], const float b[3])
{
  return (double)a[0] * (double)b[0] + (double)a[1] * (double)b[1] + (double)a[2] * (double)b[2];
}

static void wz3__mm_cross3(const float a[3], const float b[3], float out[3])
{
  const float x = (float)((double)a[1] * (double)b[2] - (double)a[2] * (double)b[1]);
  const float y = (float)((double)a[2] * (double)b[0] - (double)a[0] * (double)b[2]);
  const float z = (float)((double)a[0] * (double)b[1] - (double)a[1] * (double)b[0]);
  out[0] = x; out[1] = y; out[2] = z;
}

static void wz3__mm_normalize3(const float value[3], float out[3])
{
  const double length_sq = wz3__mm_dot3(value, value);
  if (length_sq < WZ3__MM_EPSILON) {
    out[0] = 1.0f; out[1] = 0.0f; out[2] = 0.0f;
  } else {
    const double inverse = 1.0 / sqrt(length_sq);
    const float x = (float)((double)value[0] * inverse), y = (float)((double)value[1] * inverse), z = (float)((double)value[2] * inverse);
    out[0] = x; out[1] = y; out[2] = z;
  }
}

static void wz3__mm_translation(float m[16], double x, double y, double z)
{
  wz3_mat4_identity(m);
  m[12] = (float)x; m[13] = (float)y; m[14] = (float)z;
}

static void wz3__mm_transform_point(const float m[16], const float v[3], float out[3])
{
  const double x = v[0], y = v[1], z = v[2];
  out[0] = (float)((double)m[0] * x + (double)m[4] * y + (double)m[8] * z + (double)m[12]);
  out[1] = (float)((double)m[1] * x + (double)m[5] * y + (double)m[9] * z + (double)m[13]);
  out[2] = (float)((double)m[2] * x + (double)m[6] * y + (double)m[10] * z + (double)m[14]);
}

/* matrixMul(a, b) = mat4MulA(b, a) */
static void wz3__mm_matrix_mul(const float a[16], const float b[16], float out[16]) { wz3_mat4_mul_a(b, a, out); }

static void wz3__mm_matrix_srt(const double values[9], float out[16])
{
  float srt[9];
  for (int i = 0; i < 9; i++) srt[i] = (float)values[i];
  wz3_mat4_srt(srt, out);
}

/* sMatrix::TransR: transpose the 3x3 part, then rotate the negated translation */
static void wz3__mm_rigid_inverse(const float m[16], float out[16])
{
  float t[16];
  memset(t, 0, sizeof t);
  t[0] = m[0]; t[1] = m[4]; t[2] = m[8];
  t[4] = m[1]; t[5] = m[5]; t[6] = m[9];
  t[8] = m[2]; t[9] = m[6]; t[10] = m[10];
  const double tx = m[12], ty = m[13], tz = m[14];
  t[12] = (float)(-((double)t[0] * tx + (double)t[4] * ty + (double)t[8] * tz));
  t[13] = (float)(-((double)t[1] * tx + (double)t[5] * ty + (double)t[9] * tz));
  t[14] = (float)(-((double)t[2] * tx + (double)t[6] * ty + (double)t[10] * tz));
  t[15] = 1.0f;
  memcpy(out, t, sizeof t);
}

static int wz3__mm_selected(int32_t selection, int32_t value)
{
  return selection == 0 || (selection == 1 && value) || (selection == 2 && !value);
}

static int32_t wz3__mm_select_logic(int32_t old_value, int new_value, int32_t mode)
{
  switch (mode & 3) {
  case 0: return new_value ? 1 : old_value;
  case 1: return new_value ? 0 : old_value;
  case 2: return new_value ? 1 : 0;
  default: return new_value ? 0 : 1;
  }
}

/* ---- topology helpers ---- */

static void wz3__mm_invalidate(wz3_minmesh *m) { m->normals_valid = 0; }

/* MinMesh.bounds(): faces' vertices only; zeros when no face has a vertex */
static void wz3__mm_bounds(const wz3_minmesh *m, float min[3], float max[3])
{
  double lo[3] = {INFINITY, INFINITY, INFINITY}, hi[3] = {-INFINITY, -INFINITY, -INFINITY};
  int found = 0;
  for (uint32_t i = 0; i < m->n_faces; i++) {
    const wz3__mm_face *f = &m->faces[i];
    const int32_t *c = wz3__mm_corners(m, f);
    for (int32_t k = 0; k < f->count; k++) {
      if (c[k] < 0 || (uint32_t)c[k] >= m->n_verts) continue;
      const float *p = m->verts[c[k]].position;
      found = 1;
      for (int a = 0; a < 3; a++) { lo[a] = wz3__mm_min(lo[a], p[a]); hi[a] = wz3__mm_max(hi[a], p[a]); }
    }
  }
  for (int a = 0; a < 3; a++) {
    min[a] = found ? (float)lo[a] : 0.0f;
    max[a] = found ? (float)hi[a] : 0.0f;
  }
}

/* GenMinMesh::Add: faces (with shifted vertex and cluster ids), vertices, then the other's clusters 1.. */
static int wz3__mm_add(wz3_minmesh *m, const wz3_minmesh *o)
{
  const int32_t vertex_offset = (int32_t)m->n_verts;
  const int32_t cluster_offset = (int32_t)m->n_clusters - 1;
  int32_t corners[16];
  for (uint32_t i = 0; i < o->n_faces; i++) {
    const wz3__mm_face *f = &o->faces[i];
    const int32_t *c = wz3__mm_corners(o, f);
    int32_t *buffer = f->count <= 16 ? corners : (int32_t *)WZ3_MALLOC((size_t)f->count * sizeof(int32_t));
    if (!buffer) { m->oom = 1; return -1; }
    for (int32_t k = 0; k < f->count; k++) buffer[k] = (int32_t)((uint32_t)c[k] + (uint32_t)vertex_offset);
    wz3__mm_face tmp = *f;
    if (tmp.cluster > 0) tmp.cluster += cluster_offset;
    const int r = wz3__mm_push_face(m, buffer, f->count, tmp.cluster, tmp.select, &tmp);
    if (buffer != corners) WZ3_FREE(buffer);
    if (r) return -1;
  }
  for (uint32_t i = 0; i < o->n_verts; i++) if (wz3__mm_push_vertex(m, &o->verts[i], NULL)) return -1;
  for (uint32_t i = 1; i < o->n_clusters; i++) if (wz3__mm_push_cluster(m, o->clusters[i])) return -1;
  wz3__mm_invalidate(m);
  return 0;
}

static int wz3__mm_same_cluster(const wz3__mm_cluster *a, const wz3__mm_cluster *b)
{
  return a->material == b->material && a->render_pass == b->render_pass && a->id == b->id && a->anim_type == b->anim_type &&
         a->anim_matrix == b->anim_matrix;
}

static int wz3__mm_merge_clusters(wz3_minmesh *m)
{
  const uint32_t n = m->n_clusters;
  int32_t *remap = (int32_t *)WZ3_MALLOC((size_t)(n ? n : 1) * sizeof(int32_t));
  wz3__mm_cluster *result = (wz3__mm_cluster *)WZ3_MALLOC((size_t)(n ? n : 1) * sizeof(wz3__mm_cluster));
  if (!remap || !result) { WZ3_FREE(remap); WZ3_FREE(result); m->oom = 1; return -1; }
  for (uint32_t i = 0; i < n; i++) remap[i] = -1;
  remap[0] = 0;
  for (uint32_t i = 0; i < m->n_faces; i++) {
    const int32_t c = m->faces[i].cluster;
    if (c >= 0 && (uint32_t)c < n) remap[c] = 0;
  }
  uint32_t count = 1;
  result[0] = m->clusters[0];
  for (uint32_t i = 1; i < n; i++) {
    if (remap[i] < 0) continue;
    uint32_t destination = 0;
    for (uint32_t k = 1; k < count && !destination; k++) if (wz3__mm_same_cluster(&result[k], &m->clusters[i])) destination = k;
    if (!destination) { destination = count; result[count++] = m->clusters[i]; }
    remap[i] = (int32_t)destination;
  }
  for (uint32_t i = 0; i < m->n_faces; i++) {
    const int32_t c = m->faces[i].cluster;
    m->faces[i].cluster = (c >= 0 && (uint32_t)c < n && remap[c] >= 0) ? remap[c] : 0;
  }
  WZ3_FREE(m->clusters);
  m->clusters = result;
  m->n_clusters = count;
  m->cap_clusters = n ? n : 1;
  WZ3_FREE(remap);
  wz3__mm_invalidate(m);
  return 0;
}

/* MinMesh.calcNormals */
static void wz3__mm_calc_normals(wz3_minmesh *m)
{
  if (m->normals_valid) return;
  for (uint32_t i = 0; i < m->n_verts; i++) {
    memset(m->verts[i].normal, 0, sizeof m->verts[i].normal);
    memset(m->verts[i].tangent, 0, sizeof m->verts[i].tangent);
  }
  for (uint32_t fi = 0; fi < m->n_faces; fi++) {
    wz3__mm_face *face = &m->faces[fi];
    const int32_t *c = wz3__mm_corners(m, face);
    memset(face->normal, 0, sizeof face->normal);
    if (face->count == 2) {
      if (c[0] < 0 || (uint32_t)c[0] >= m->n_verts || c[1] < 0 || (uint32_t)c[1] >= m->n_verts) continue;
      wz3__mm_vertex *first = &m->verts[c[0]], *second = &m->verts[c[1]];
      wz3__mm_add3(first->normal, second->normal, 1.0); memcpy(second->normal, first->normal, sizeof first->normal);
      wz3__mm_add3(first->tangent, second->tangent, 1.0); memcpy(second->tangent, first->tangent, sizeof first->tangent);
    } else if (face->count >= 3) {
      if (c[0] < 0 || (uint32_t)c[0] >= m->n_verts || c[1] < 0 || (uint32_t)c[1] >= m->n_verts || c[2] < 0 || (uint32_t)c[2] >= m->n_verts) continue;
      const wz3__mm_vertex *v0 = &m->verts[c[0]], *v1 = &m->verts[c[1]], *v2 = &m->verts[c[2]];
      float d0[3], d1[3], normal[3], tangent[3] = {0, 0, 0};
      wz3__mm_sub3(v1->position, v0->position, d0);
      wz3__mm_sub3(v2->position, v0->position, d1);
      wz3__mm_cross3(d0, d1, normal);
      wz3__mm_normalize3(normal, normal);
      memcpy(face->normal, normal, sizeof normal);
      const double b1 = (double)v1->uv[0][0] - (double)v0->uv[0][0], b2 = (double)v2->uv[0][0] - (double)v0->uv[0][0];
      const double c1 = (double)v1->uv[0][1] - (double)v0->uv[0][1], c2 = (double)v2->uv[0][1] - (double)v0->uv[0][1];
      const double determinant = b1 * c2 - c1 * b2;
      if (fabs(determinant) > WZ3__MM_EPSILON) {
        wz3__mm_add3(tangent, d0, c2 / determinant);
        wz3__mm_add3(tangent, d1, -c1 / determinant);
      }
      wz3__mm_add3(tangent, normal, -wz3__mm_dot3(tangent, normal));
      wz3__mm_normalize3(tangent, tangent);
      for (int32_t k = 0; k < face->count; k++) {
        if (c[k] < 0 || (uint32_t)c[k] >= m->n_verts) continue;
        wz3__mm_add3(m->verts[c[k]].normal, normal, 1.0);
        wz3__mm_add3(m->verts[c[k]].tangent, tangent, 1.0);
      }
    }
  }
  for (uint32_t i = 0; i < m->n_verts; i++) {
    wz3__mm_normalize3(m->verts[i].normal, m->verts[i].normal);
    wz3__mm_normalize3(m->verts[i].tangent, m->verts[i].tangent);
  }
  m->normals_valid = 1;
}

/* MinMesh.makeGrid; returns the first new vertex or -1 */
static int32_t wz3__mm_make_grid(wz3_minmesh *m, int32_t tx, int32_t ty, int32_t flags)
{
  tx = tx < 1 ? 1 : tx; ty = ty < 0 ? 0 : ty;
  int bits = 0;
  if (flags & 1) bits++;
  if (flags & 2) bits++;
  const int32_t vertex_start = (int32_t)m->n_verts;
  const int32_t regular_count = (tx + 1) * (ty + 1);
  const int32_t cluster = (flags & 32) ? 0 : 1;
  for (int32_t y = 0; y <= ty; y++) for (int32_t x = 0; x <= tx; x++) {
    wz3__mm_vertex *v;
    if (wz3__mm_push_vertex(m, NULL, &v)) return -1;
    const double yf = (double)y / (double)ty;
    v->position[0] = (float)((double)x / (double)tx - 0.5);
    v->position[1] = (float)(yf - 0.5);
    v->uv[0][0] = (float)((double)x / (double)tx);
    v->uv[0][1] = (float)(1.0 - yf);
    v->uv[1][0] = (float)((double)(x == tx ? 0 : x) / (double)tx);
    v->uv[1][1] = (float)(1.0 - (double)(y == ty ? 0 : y) / (double)ty);
    v->select = 1;
  }
  for (int i = 0; i < bits; i++) if (wz3__mm_push_vertex(m, NULL, NULL)) return -1;
  for (int32_t y = 0; y < ty; y++) for (int32_t x = 0; x < tx; x++) {
    int32_t idx[4] = {vertex_start + y * (tx + 1) + x, vertex_start + (y + 1) * (tx + 1) + x,
                      vertex_start + (y + 1) * (tx + 1) + x + 1, vertex_start + y * (tx + 1) + x + 1};
    if (flags & 16) {
      int32_t t = idx[0]; idx[0] = idx[3]; idx[3] = t;
      t = idx[1]; idx[1] = idx[2]; idx[2] = t;
    }
    if (wz3__mm_push_face(m, idx, 4, cluster, 1, NULL)) return -1;
  }
  int32_t center = vertex_start + regular_count, border = vertex_start;
  for (int side = 0; side < 2; side++) {
    if (flags & (side + 1)) {
      wz3__mm_vertex *cv = &m->verts[center];
      cv->uv[0][0] = 1.0f; cv->uv[0][1] = (float)(1 - side);
      for (int32_t x = 0; x < tx; x++) {
        const int32_t idx[3] = {center, border + side, border + 1 - side};
        if (wz3__mm_push_face(m, idx, 3, cluster, 1, NULL)) return -1;
        border++;
      }
    }
    border = vertex_start + ty * (tx + 1);
    if (flags & 1) center++;
  }
  if (flags & 4) for (int32_t x = 0; x <= tx; x++) {
    const int32_t idx[2] = {vertex_start + x, vertex_start + x + ty * (tx + 1)};
    if (wz3__mm_push_face(m, idx, 2, 0, 0, NULL)) return -1;
  }
  if (flags & 8) for (int32_t y = 0; y <= ty; y++) {
    const int32_t idx[2] = {vertex_start + y * (tx + 1), vertex_start + y * (tx + 1) + tx};
    if (wz3__mm_push_face(m, idx, 2, 0, 0, NULL)) return -1;
  }
  wz3__mm_invalidate(m);
  return vertex_start;
}

/* MinMesh.transform; operation 2 (spherical UV projection) needs atan2 and is reported through *unsupported */
static void wz3__mm_transform(wz3_minmesh *m, int32_t selection, const float matrix[16], int32_t source, int32_t destination,
                              int32_t operation, int *unsupported)
{
  if (operation == 2) { *unsupported = 1; return; }
  for (uint32_t i = 0; i < m->n_verts; i++) {
    wz3__mm_vertex *v = &m->verts[i];
    if (!wz3__mm_selected(selection, v->select)) continue;
    float temporary[3];
    if (source == 0) memcpy(temporary, v->position, sizeof temporary);
    else {
      if (source < 1 || source > 2) continue;
      temporary[0] = v->uv[source - 1][0]; temporary[1] = v->uv[source - 1][1]; temporary[2] = 0.0f;
    }
    wz3__mm_transform_point(matrix, temporary, temporary);
    if (operation == 1) wz3__mm_normalize3(temporary, temporary);
    if (destination == 0) memcpy(v->position, temporary, sizeof temporary);
    else if (destination >= 1 && destination <= 2) {
      v->uv[destination - 1][0] = temporary[0]; v->uv[destination - 1][1] = temporary[1];
    }
  }
  wz3__mm_invalidate(m);
}

static int wz3__mm_triangulate(wz3_minmesh *m)
{
  wz3_minmesh *t = wz3__mm_new();
  if (!t) { m->oom = 1; return -1; }
  for (uint32_t i = 0; i < m->n_faces; i++) {
    const wz3__mm_face *f = &m->faces[i];
    const int32_t *c = wz3__mm_corners(m, f);
    if (f->count <= 3) {
      if (wz3__mm_push_face(t, c, f->count, f->cluster, f->select, f)) { m->oom = 1; wz3__mm_free(t); return -1; }
    } else {
      for (int32_t k = 2; k < f->count; k++) {
        const int32_t tri[3] = {c[0], c[k - 1], c[k]};
        if (wz3__mm_push_face(t, tri, 3, f->cluster, f->select, f)) { m->oom = 1; wz3__mm_free(t); return -1; }
      }
    }
  }
  WZ3_FREE(m->faces); WZ3_FREE(m->corners);
  m->faces = t->faces; m->n_faces = t->n_faces; m->cap_faces = t->cap_faces;
  m->corners = t->corners; m->n_corners = t->n_corners; m->cap_corners = t->cap_corners;
  t->faces = NULL; t->corners = NULL;
  wz3__mm_free(t);
  wz3__mm_invalidate(m);
  return 0;
}

/* ---- operators ---- */

static wz3_minmesh *wz3__mm_op_grid(int32_t mode, int32_t tx, int32_t ty)
{
  wz3_minmesh *m = wz3__mm_new();
  if (!m) return NULL;
  int unsupported = 0;
  if (wz3__mm_make_grid(m, tx, ty, 0) < 0) { wz3__mm_free(m); return NULL; }
  if (mode & 1) if (wz3__mm_make_grid(m, tx, ty, 16) < 0) { wz3__mm_free(m); return NULL; }
  float matrix[16];
  wz3_mat4_identity(matrix);
  matrix[0] = -1.0f; matrix[4] = 0.0f; matrix[5] = 0.0f; matrix[6] = 1.0f;
  wz3__mm_transform(m, 0, matrix, 0, 0, 0, &unsupported);
  for (uint32_t i = 0; i < m->n_verts; i++) m->verts[i].select = 1;     /* selectAll(2|4|1) */
  for (uint32_t i = 0; i < m->n_faces; i++) m->faces[i].select = 1;
  wz3__mm_invalidate(m);
  return m;
}

static wz3_minmesh *wz3__mm_op_sphere(int32_t tx, int32_t ty)
{
  tx = tx < 3 ? 3 : tx; ty = ty < 1 ? 1 : ty;
  wz3_minmesh *m = wz3__mm_new();
  if (!m) return NULL;
  const int32_t start = wz3__mm_make_grid(m, tx, ty, 3 | 8);
  if (start < 0) { wz3__mm_free(m); return NULL; }
  const int32_t regular = (tx + 1) * (ty + 1);
  const double tau = 3.141592653589793 * 2.0, pi = 3.141592653589793;
  for (int32_t i = 0; i < regular; i++) {
    wz3__mm_vertex *v = &m->verts[start + i];
    const double fx = (1.0 - (double)v->uv[1][0]) * tau;
    const double fy = (0.5 / (double)(ty + 1) + (double)v->uv[0][1] * (double)ty / (double)(ty + 1)) * pi;
    v->position[0] = (float)(-wz3__sin(fy) * wz3__sin(fx) * 0.5);
    v->position[1] = (float)(wz3__cos(fy) * 0.5);
    v->position[2] = (float)(-wz3__sin(fy) * wz3__cos(fx) * 0.5);
  }
  m->verts[start + regular].position[1] = m->verts[start].position[1];
  m->verts[start + regular + 1].position[1] = m->verts[start + regular - 1].position[1];
  wz3__mm_invalidate(m);
  return m;
}

/* MinMesh_MatLink; `material` is the operator id of the material */
static wz3_minmesh *wz3__mm_op_mat_link(const wz3_minmesh *in, int32_t material, int32_t selection, int32_t render_pass)
{
  wz3_minmesh *m = wz3__mm_clone(in);
  if (!m) return NULL;
  int32_t cluster = -1;
  for (uint32_t i = 1; i < m->n_clusters && cluster < 0; i++) {
    const wz3__mm_cluster *c = &m->clusters[i];
    if (c->material == material && c->id == 0 && c->anim_type == 0 && c->anim_matrix == -1) cluster = (int32_t)i;
  }
  if (cluster < 0) {
    cluster = (int32_t)m->n_clusters;
    if (wz3__mm_push_cluster(m, wz3__mm_make_cluster(material, render_pass, 0, 0, -1))) { wz3__mm_free(m); return NULL; }
  }
  for (uint32_t i = 0; i < m->n_faces; i++) {
    wz3__mm_face *f = &m->faces[i];
    if (selection == 0 || (selection == 1 && f->select) || (selection == 2 && !f->select)) if (f->cluster != 0) f->cluster = cluster;
  }
  wz3__mm_invalidate(m);
  return m;
}

static wz3_minmesh *wz3__mm_op_add(const wz3_minmesh *const *inputs, uint32_t n)
{
  if (!n) return NULL;
  wz3_minmesh *m = wz3__mm_clone(inputs[0]);
  if (!m) return NULL;
  for (uint32_t i = 1; i < n; i++) if (wz3__mm_add(m, inputs[i])) { wz3__mm_free(m); return NULL; }
  if (wz3__mm_merge_clusters(m)) { wz3__mm_free(m); return NULL; }
  return m;
}

static wz3_minmesh *wz3__mm_op_select_cube(const wz3_minmesh *in, int32_t mode, const double p[6])
{
  wz3_minmesh *m = wz3__mm_clone(in);
  if (!m) return NULL;
  for (uint32_t i = 0; i < m->n_verts; i++) {
    wz3__mm_vertex *v = &m->verts[i];
    v->temp_byte = (fabs((double)v->position[0] - p[0]) <= p[3] / 2 && fabs((double)v->position[1] - p[1]) <= p[4] / 2 &&
                    fabs((double)v->position[2] - p[2]) <= p[5] / 2) ? 1 : 0;
  }
  switch (mode & 12) {
  case 0:
    for (uint32_t i = 0; i < m->n_verts; i++) m->verts[i].select = wz3__mm_select_logic(m->verts[i].select, m->verts[i].temp_byte, mode);
    break;
  case 4:
    for (uint32_t i = 0; i < m->n_faces; i++) {
      const int32_t *c = wz3__mm_corners(m, &m->faces[i]);
      int match = 1;
      for (int32_t k = 0; k < m->faces[i].count; k++) if (!m->verts[c[k]].temp_byte) { match = 0; break; }
      m->faces[i].select = wz3__mm_select_logic(m->faces[i].select, match, mode);
    }
    break;
  case 8:
    for (uint32_t i = 0; i < m->n_faces; i++) {
      const int32_t *c = wz3__mm_corners(m, &m->faces[i]);
      int match = 0;
      for (int32_t k = 0; k < m->faces[i].count; k++) if (m->verts[c[k]].temp_byte) { match = 1; break; }
      m->faces[i].select = wz3__mm_select_logic(m->faces[i].select, match, mode);
    }
    break;
  default: break;
  }
  wz3__mm_invalidate(m);
  return m;
}

static wz3_minmesh *wz3__mm_op_delete_faces(const wz3_minmesh *in)
{
  wz3_minmesh *m = wz3__mm_clone(in);
  if (!m) return NULL;
  for (uint32_t i = 0; i < m->n_faces; i++) if (m->faces[i].select) m->faces[i].cluster = 0;
  wz3__mm_invalidate(m);
  return m;
}

/* MinMesh_TransformEx; NULL with *unsupported set when the mask asks for the spherical projection */
static wz3_minmesh *wz3__mm_op_transform_ex(const wz3_minmesh *in, int32_t mask, const double srt[9], int *unsupported)
{
  wz3_minmesh *m = wz3__mm_clone(in);
  if (!m) return NULL;
  float matrix[16];
  wz3__mm_matrix_srt(srt, matrix);
  wz3__mm_transform(m, mask & 3, matrix, (int32_t)(((uint32_t)mask >> 2) & 7u), (int32_t)(((uint32_t)mask >> 5) & 7u),
                    (int32_t)(((uint32_t)mask >> 8) & 3u), unsupported);
  if (*unsupported) { wz3__mm_free(m); return NULL; }
  return m;
}

/* the source's Perlin noise: permutation ranked by sGetRnd, 16 gradients */
typedef struct wz3__mm_perlin { uint8_t permutation[512]; float gradients[64]; } wz3__mm_perlin;

static void wz3__mm_perlin_init(wz3__mm_perlin *s)
{
  wz3_rng random;
  uint32_t ranks[256];
  wz3_rng_init(&random);
  wz3_rng_seed(&random, 1);
  for (int i = 0; i < 256; i++) { ranks[i] = wz3_rng_int(&random, 0x10000u); s->permutation[i] = (uint8_t)i; }
  for (int i = 0; i < 255; i++) for (int j = i + 1; j < 256; j++) if (ranks[i] > ranks[j]) {
    const uint32_t r = ranks[i]; ranks[i] = ranks[j]; ranks[j] = r;
    const uint8_t p = s->permutation[i]; s->permutation[i] = s->permutation[j]; s->permutation[j] = p;
  }
  memcpy(s->permutation + 256, s->permutation, 256);
  static const uint8_t source[64] = {
    1, 1, 0, 0, 2, 1, 0, 0, 1, 2, 0, 0, 2, 2, 0, 0,
    1, 0, 1, 0, 2, 0, 1, 0, 1, 0, 2, 0, 2, 0, 2, 0,
    0, 1, 1, 0, 0, 2, 1, 0, 0, 1, 2, 0, 0, 2, 2, 0,
    1, 1, 0, 0, 2, 1, 0, 0, 0, 2, 1, 0, 0, 2, 2, 0,
  };
  static const float values[3] = {0.0f, 1.0f, -1.0f};
  for (int i = 0; i < 64; i++) s->gradients[i] = values[source[i]];
}

static void wz3__mm_lerp3(const float a[3], const float b[3], double amount, float out[3])
{
  const float x = (float)((double)a[0] + ((double)b[0] - (double)a[0]) * amount);
  const float y = (float)((double)a[1] + ((double)b[1] - (double)a[1]) * amount);
  const float z = (float)((double)a[2] + ((double)b[2] - (double)a[2]) * amount);
  out[0] = x; out[1] = y; out[2] = z;
}

static int32_t wz3__mm_to_int32(double v) { return wz3__rt_to_int32(v); }

static void wz3__mm_perlin3(const wz3__mm_perlin *s, const float position[3], float out[3])
{
  int32_t integer[3];
  double fade[3];
  for (int axis = 0; axis < 3; axis++) {
    const double p = position[axis];
    const double floored = floor(p);
    const double fraction = (float)(p - floored);
    integer[axis] = wz3__mm_to_int32(floored) & 255;
    fade[axis] = (float)(fraction * fraction * fraction * (10 + fraction * (6 * fraction - 15)));
  }
#define WZ3__MM_GRAD(gx, gy, gz) (&s->gradients[(s->permutation[s->permutation[s->permutation[(gx)] + (gy)] + (gz)] & 15) * 4])
  const int32_t x = integer[0], y = integer[1], z = integer[2];
  float a[3], b[3], c[3], d[3];
  wz3__mm_lerp3(WZ3__MM_GRAD(x, y, z), WZ3__MM_GRAD(x + 1, y, z), fade[0], a);
  wz3__mm_lerp3(WZ3__MM_GRAD(x, y + 1, z), WZ3__MM_GRAD(x + 1, y + 1, z), fade[0], b);
  wz3__mm_lerp3(a, b, fade[1], a);
  wz3__mm_lerp3(WZ3__MM_GRAD(x, y, z + 1), WZ3__MM_GRAD(x + 1, y, z + 1), fade[0], c);
  wz3__mm_lerp3(WZ3__MM_GRAD(x, y + 1, z + 1), WZ3__MM_GRAD(x + 1, y + 1, z + 1), fade[0], d);
  wz3__mm_lerp3(c, d, fade[1], c);
  wz3__mm_lerp3(a, c, fade[2], out);
#undef WZ3__MM_GRAD
}

static wz3_minmesh *wz3__mm_op_perlin(const wz3_minmesh *in, int32_t mask, const double values[12])
{
  wz3_minmesh *m = wz3__mm_clone(in);
  if (!m) return NULL;
  wz3__mm_perlin noise;
  wz3__mm_perlin_init(&noise);
  float matrix[16];
  wz3__mm_matrix_srt(values, matrix);
  for (uint32_t i = 0; i < m->n_verts; i++) {
    wz3__mm_vertex *v = &m->verts[i];
    if (!(mask == 0 || (mask == 1 && v->select) || (mask == 2 && !v->select))) continue;
    float p[3], sample[3];
    wz3__mm_transform_point(matrix, v->position, p);
    wz3__mm_perlin3(&noise, p, sample);
    v->position[0] = (float)((double)v->position[0] + (double)sample[0] * values[9]);
    v->position[1] = (float)((double)v->position[1] + (double)sample[1] * values[10]);
    v->position[2] = (float)((double)v->position[2] + (double)sample[2] * values[11]);
  }
  wz3__mm_invalidate(m);
  return m;
}

static wz3_minmesh *wz3__mm_op_bend2(const wz3_minmesh *in, const double p[8])
{
  wz3_minmesh *m = wz3__mm_clone(in);
  if (!m) return NULL;
  const double length = p[6];
  float turns[3] = {(float)p[3], (float)p[4], (float)p[5]};
  float forward[16], backward[16];
  wz3_mat4_euler_turns(turns, forward);
  forward[12] = (float)(-p[0]); forward[13] = (float)(-p[1]); forward[14] = (float)(-p[2]);
  wz3__mm_rigid_inverse(forward, backward);
  const double angle = (float)(p[7] * (3.141592653589793 * 2.0));
  for (uint32_t i = 0; i < m->n_verts; i++) {
    wz3__mm_vertex *v = &m->verts[i];
    float value[3];
    wz3__mm_transform_point(forward, v->position, value);
    const double original_y = value[1];
    if (original_y >= 0) value[1] = (float)((double)value[1] - wz3__mm_min(original_y, length));
    const double phase = (float)(wz3__mm_max(0, wz3__mm_min(1, (float)(original_y / length))) * angle);
    const double sine = (float)wz3__sin(phase), cosine = (float)wz3__cos(phase);
    const double x = value[0], y = value[1];
    value[0] = (float)(cosine * x - sine * y);
    value[1] = (float)(sine * x + cosine * y);
    wz3__mm_transform_point(backward, value, v->position);
  }
  wz3__mm_invalidate(m);
  return m;
}

static wz3_minmesh *wz3__mm_op_center(const wz3_minmesh *in, int32_t which)
{
  wz3_minmesh *m = wz3__mm_clone(in);
  if (!m) return NULL;
  float lo[3], hi[3];
  wz3__mm_bounds(m, lo, hi);
  const float offset[3] = {(which & 1) ? (float)(((double)hi[0] + (double)lo[0]) * 0.5) : 0.0f,
                           (which & 2) ? (float)(((double)hi[1] + (double)lo[1]) * 0.5) : 0.0f,
                           (which & 4) ? (float)(((double)hi[2] + (double)lo[2]) * 0.5) : 0.0f};
  for (uint32_t i = 0; i < m->n_verts; i++) {
    for (int a = 0; a < 3; a++) m->verts[i].position[a] = (float)((double)m->verts[i].position[a] - (double)offset[a]);
  }
  wz3__mm_invalidate(m);
  return m;
}

/* MinMesh_Multiply: always builds a fresh mesh from the input (non-owned path) */
static wz3_minmesh *wz3__mm_op_multiply(const wz3_minmesh *in, const double p[17])
{
  const double *srt = p;
  const int32_t seed = wz3__mm_to_int32(p[9]);
  const int32_t mode = wz3__mm_to_int32(p[10]);
  const double tu = p[11], tv = p[12];
  float local_rotation[3] = {(float)p[13], (float)p[14], (float)p[15]};
  float transform[16], local[16], step[16], local_step[16], combined[16];
  wz3_mat4_identity(transform); wz3_mat4_identity(local);
  wz3__mm_matrix_srt(srt, step);
  wz3_mat4_euler_turns(local_rotation, local_step);
  wz3_rng random;
  wz3_rng_init(&random);
  wz3_rng_seed(&random, seed);
  const int32_t count = seed < 0 ? 0 : seed;
  wz3_minmesh *out = wz3__mm_new();
  if (!out) return NULL;
  for (int32_t copy = 0; copy < count; copy++) {
    const uint32_t start = out->n_verts;
    if (wz3__mm_add(out, in)) { wz3__mm_free(out); return NULL; }
    wz3__mm_matrix_mul(transform, local, combined);
    for (uint32_t i = start; i < out->n_verts; i++) {
      wz3__mm_transform_point(combined, out->verts[i].position, out->verts[i].position);
      if (mode & 1) {
        out->verts[i].uv[0][0] = (float)((double)out->verts[i].uv[0][0] + (double)copy * tu);
        out->verts[i].uv[0][1] = (float)((double)out->verts[i].uv[0][1] + (double)copy * tv);
      }
    }
    if (mode & 2) {
      /* randomSRT: InitRandomSRT uses each component as the range around identity/zero */
      double values[9];
      for (int i = 0; i < 3; i++) values[i] = 1.0 + (double)wz3_rng_float(&random, 1.0f) * (srt[i] - 1.0);
      for (int i = 3; i < 9; i++) values[i] = ((double)wz3_rng_float(&random, 2.0f) - 1.0) * srt[i];
      wz3__mm_matrix_srt(values, transform);
    } else {
      wz3__mm_matrix_mul(step, transform, transform);
    }
    wz3__mm_matrix_mul(local_step, local, local);
  }
  if (wz3__mm_merge_clusters(out)) { wz3__mm_free(out); return NULL; }
  return out;
}

static wz3_minmesh *wz3__mm_op_triangulate(const wz3_minmesh *in)
{
  wz3_minmesh *m = wz3__mm_clone(in);
  if (!m) return NULL;
  if (wz3__mm_triangulate(m)) { wz3__mm_free(m); return NULL; }
  return m;
}

static wz3_minmesh *wz3__mm_op_bone_chain(const wz3_minmesh *in, const double p[8])
{
  wz3_minmesh *m = wz3__mm_clone(in);
  if (!m) return NULL;
  const int32_t flags = wz3__mm_to_int32(p[7]);
  int32_t count = wz3__mm_to_int32(p[6]);
  count = count < 1 ? 1 : count;
  for (uint32_t i = 1; i < m->n_clusters; i++) m->clusters[i].anim_type = 2;
  float p0[3] = {(float)p[0], (float)p[1], (float)p[2]}, p1[3] = {(float)p[3], (float)p[4], (float)p[5]};
  if (flags & 1) {
    float lo[3], hi[3];
    wz3__mm_bounds(m, lo, hi);
    p0[0] = 0; p0[1] = 0; p0[2] = lo[2];
    p1[0] = 0; p1[1] = 0; p1[2] = hi[2];
  }
  float difference[3], direction[3];
  wz3__mm_sub3(p1, p0, difference);
  double denominator = wz3__mm_dot3(difference, difference);
  if (denominator == 0.0 || denominator != denominator) denominator = 1.0;
  memcpy(direction, difference, sizeof direction);
  wz3__mm_scale3(direction, 1.0 / denominator);
  wz3__mm_bone *bones = (wz3__mm_bone *)WZ3_MALLOC((size_t)count * sizeof(wz3__mm_bone));
  if (!bones) { wz3__mm_free(m); return NULL; }
  for (int32_t index = 0; index < count; index++) {
    const double phase = count > 1 ? (double)index / (double)(count - 1) : 0.0;
    const float position[3] = {(float)((double)p0[0] + (double)difference[0] * phase), (float)((double)p0[1] + (double)difference[1] * phase),
                               (float)((double)p0[2] + (double)difference[2] * phase)};
    wz3__mm_bone *b = &bones[index];
    memset(b, 0, sizeof *b);
    wz3__mm_translation(b->no_animation, position[0], position[1], position[2]);
    wz3__mm_translation(b->base_pose, -(double)position[0], -(double)position[1], -(double)position[2]);
    b->parent = -1; b->factor = 1.0f; b->spread = 0.0f; b->offset = 0.0f;
  }
  WZ3_FREE(m->bones);
  m->bones = bones;
  m->n_bones = (uint32_t)count;
  for (uint32_t i = 0; i < m->n_verts; i++) {
    wz3__mm_vertex *v = &m->verts[i];
    /* the released source's typo is preserved: the y component starts from position.x */
    const float delta[3] = {(float)((double)v->position[0] - (double)p0[0]), (float)((double)v->position[0] - (double)p0[1]),
                            (float)((double)v->position[2] - (double)p0[2])};
    double fade = wz3__mm_dot3(direction, delta) * (double)(count - 1);
    const double truncated = trunc(fade * 1024);
    const double fixed = wz3__mm_max(0, wz3__mm_min((double)count * 1024 - 1, truncated));
    const double index = trunc(fixed / 1024);
    fade = (double)(wz3__mm_to_int32(fixed) & 1023) / 1024;
    memset(v->weights, 0, sizeof v->weights);
    memset(v->matrices, 0, sizeof v->matrices);
    if (index + 1 < (double)count) {
      const double f2 = fade * fade, f3 = f2 * fade;
      v->bone_count = 4;
      v->matrices[0] = (uint16_t)wz3__mm_to_int32(wz3__mm_max(0, index - 1));
      v->matrices[1] = (uint16_t)wz3__mm_to_int32(index);
      v->matrices[2] = (uint16_t)wz3__mm_to_int32(wz3__mm_min((double)(count - 1), index + 1));
      v->matrices[3] = (uint16_t)wz3__mm_to_int32(wz3__mm_min((double)(count - 1), index + 2));
      v->weights[0] = (float)(-0.5 * f3 + f2 - 0.5 * fade);
      v->weights[1] = (float)(1.5 * f3 - 2.5 * f2 + 1);
      v->weights[2] = (float)(-1.5 * f3 + 2 * f2 + 0.5 * fade);
      v->weights[3] = (float)(0.5 * f3 - 0.5 * f2);
    } else {
      v->bone_count = 1;
      v->matrices[0] = (uint16_t)(count - 1);
      v->weights[0] = 1.0f;
    }
  }
  return m;
}

/* MinMesh_BoneTrain: `spline_op` is the operator that produced the spline, -1 when there is none */
static wz3_minmesh *wz3__mm_op_bone_train(const wz3_minmesh *in, int32_t spline_op, double delta, int32_t mode, double offset)
{
  wz3_minmesh *m = wz3__mm_clone(in);
  if (!m) return NULL;
  if (spline_op < 0 || !m->n_bones) return m;
  const int32_t count = (int32_t)m->n_bones;
  for (int32_t index = 0; index < count; index++) {
    wz3__mm_bone *b = &m->bones[index];
    const float step = (float)((double)index * delta);
    const float divided = (float)((double)step / (double)(float)(count - 1));
    const float time = (float)((double)divided + offset);
    b->spline_kind = WZ3__MM_SPLINE_EXTERNAL;
    b->spline_op = spline_op;
    if ((mode & 3) == 0) { b->offset = time; b->factor = 1.0f; b->spread = 0.0f; }
    else if ((mode & 3) == 1) { b->offset = 0.0f; b->factor = time; b->spread = 0.0f; }
    else if ((mode & 3) == 2) { b->offset = 0.0f; b->factor = 0.0f; b->spread = 1.0f; }
  }
  return m;
}

/* ---- Pipe ---- */

static void wz3__mm_pipe_key_matrix(const wz3__rt_blob_key *key, float out[16])
{
  const double w = key->f[6], x = key->f[3], y = key->f[4], z = key->f[5];
  const double xx = (float)(2 * x * x), xy = (float)(2 * x * y), xz = (float)(2 * x * z);
  const double yy = (float)(2 * y * y), yz = (float)(2 * y * z), zz = (float)(2 * z * z);
  const double xw = (float)(2 * x * w), yw = (float)(2 * y * w), zw = (float)(2 * z * w);
  out[0] = (float)(1 - yy - zz); out[1] = (float)(xy - zw); out[2] = (float)(xz + yw); out[3] = 0;
  out[4] = (float)(xy + zw); out[5] = (float)(1 - xx - zz); out[6] = (float)(yz - xw); out[7] = 0;
  out[8] = (float)(xz - yw); out[9] = (float)(yz + xw); out[10] = (float)(1 - xx - yy); out[11] = 0;
  out[12] = key->f[0]; out[13] = key->f[1]; out[14] = key->f[2]; out[15] = 1;
}

static void wz3__mm_pipe_axis_angle(const float unit_axis[3], double angle, float out[16])
{
  const double cosine = (float)wz3__cos(angle), sine = (float)wz3__sin(angle);
  const double x = unit_axis[0], y = unit_axis[1], z = unit_axis[2];
  out[0] = (float)(x * x + cosine * (1 - x * x));
  out[1] = (float)(x * y + cosine * (-x * y) + sine * z);
  out[2] = (float)(x * z + cosine * (-x * z) - sine * y); out[3] = 0;
  out[4] = (float)(y * x + cosine * (-y * x) - sine * z);
  out[5] = (float)(y * y + cosine * (1 - y * y));
  out[6] = (float)(y * z + cosine * (-y * z) + sine * x); out[7] = 0;
  out[8] = (float)(z * x + cosine * (-z * x) + sine * y);
  out[9] = (float)(z * y + cosine * (-z * y) - sine * x);
  out[10] = (float)(z * z + cosine * (1 - z * z)); out[11] = 0;
  out[12] = out[13] = out[14] = 0; out[15] = 1;
}

static int wz3__mm_pipe_add_straight(wz3_minmesh *out, const wz3_minmesh *source, const float matrix[16], const float difference[3],
                                     float z_minimum, float z_range, int32_t copy, double copies, int32_t flags, double texture_zoom,
                                     float distance, float absolute_distance)
{
  const uint32_t first = out->n_verts;
  if (wz3__mm_add(out, source)) return -1;
  for (uint32_t i = first; i < out->n_verts; i++) {
    wz3__mm_vertex *v = &out->verts[i];
    float fraction = (float)(((double)v->position[2] - (double)z_minimum) / (double)z_range);
    fraction = (float)(((double)fraction + (double)copy) / copies);
    v->position[2] = 0.0f;
    wz3__mm_transform_point(matrix, v->position, v->position);
    wz3__mm_add3(v->position, difference, fraction);
    if (flags & 1) v->uv[0][1] = (float)((double)fraction * texture_zoom * (double)distance + (double)absolute_distance);
  }
  return 0;
}

static int wz3__mm_pipe_add_curve(wz3_minmesh *out, const wz3_minmesh *source, const float previous[16], const float center[3],
                                  const float axis[3], float gamma, float z_minimum, float z_range, int32_t flags, double texture_zoom,
                                  float distance, float absolute_distance)
{
  const uint32_t first = out->n_verts;
  if (wz3__mm_add(out, source)) return -1;
  float rotation[16], unit_axis[3];
  wz3__mm_normalize3(axis, unit_axis);
  for (uint32_t i = first; i < out->n_verts; i++) {
    wz3__mm_vertex *v = &out->verts[i];
    const float fraction = (float)(((double)v->position[2] - (double)z_minimum) / (double)z_range);
    wz3__mm_pipe_axis_angle(unit_axis, (float)(-(double)fraction * (3.141592653589793 - (double)gamma)), rotation);
    v->position[2] = 0.0f;
    wz3__mm_transform_point(previous, v->position, v->position);
    wz3__mm_sub3(v->position, center, v->position);
    wz3__mm_transform_point(rotation, v->position, v->position);
    wz3__mm_add3(v->position, center, 1.0);
    if ((flags & 17) == 17) v->uv[0][1] = (float)((double)fraction * texture_zoom * (double)distance + (double)absolute_distance);
  }
  return 0;
}

static int wz3__mm_pipe_add_transformed(wz3_minmesh *out, const wz3_minmesh *source, const float matrix[16])
{
  const uint32_t first = out->n_verts;
  if (wz3__mm_add(out, source)) return -1;
  for (uint32_t i = first; i < out->n_verts; i++) wz3__mm_transform_point(matrix, out->verts[i].position, out->verts[i].position);
  return 0;
}

/* NULL with *oom == 0: not a mode 4 spline or inconsistent keys (the JS handler returns null); with *oom != 0: out of memory */
static wz3_minmesh *wz3__mm_op_pipe(const wz3__rt_blob *spline, const wz3_minmesh *mesh0, const wz3_minmesh *mesh1, const wz3_minmesh *mesh2,
                                    int32_t flags, double texture_zoom, double ring_distance, double object_distance, int *oom)
{
  *oom = 0;
  if (!spline || spline->mode != 4 || !spline->pipe_keys || !mesh0) return NULL;
  const uint32_t segment_count = spline->n_pipe_keys;
  if (segment_count < 1 || spline->n_keys != segment_count * 2) return NULL;
  wz3_minmesh *out = wz3__mm_new();
  if (!out) { *oom = 1; return NULL; }
  float lo[3], hi[3];
  wz3__mm_bounds(mesh0, lo, hi);
  const float mesh0_z_min = lo[2], mesh0_z_range = (float)((double)hi[2] - (double)mesh0_z_min);
  const wz3_minmesh *curve_source = mesh1 ? mesh1 : mesh0;
  wz3__mm_bounds(curve_source, lo, hi);
  const float curve_z_min = lo[2], curve_z_range = (float)((double)hi[2] - (double)curve_z_min);
  float absolute_distance = 0.0f, previous_matrix[16];
  wz3_mat4_identity(previous_matrix);
  for (uint32_t segment = 0; segment < segment_count; segment++) {
    if (segment > 0) {
      const wz3__rt_pipe_key *corner = &spline->pipe_keys[segment - 1];
      const wz3__rt_blob_key *previous = &spline->keys[segment * 2 - 1], *current = &spline->keys[segment * 2];
      const float corner_position[3] = {corner->x, corner->y, corner->z};
      float a[3], b[3], direction0[3], direction1[3], axis[3], middle[3], center[3];
      a[0] = previous->f[0]; a[1] = previous->f[1]; a[2] = previous->f[2];
      wz3__mm_sub3(a, corner_position, direction0);
      wz3__mm_normalize3(direction0, direction0);
      b[0] = current->f[0]; b[1] = current->f[1]; b[2] = current->f[2];
      wz3__mm_sub3(b, corner_position, direction1);
      wz3__mm_normalize3(direction1, direction1);
      wz3__mm_cross3(direction0, direction1, axis);
      const float gamma = (float)wz3__acos(wz3__mm_dot3(direction0, direction1));
      memcpy(middle, direction0, sizeof middle);
      wz3__mm_add3(middle, direction1, 1.0);
      wz3__mm_normalize3(middle, middle);
      memcpy(center, corner_position, sizeof center);
      wz3__mm_add3(center, middle, (float)((double)corner->radius / wz3__cos((double)gamma / 2)));
      const float curve_distance = (float)((double)corner->radius * (double)gamma);
      if (wz3__mm_pipe_add_curve(out, curve_source, previous_matrix, center, axis, gamma, curve_z_min, curve_z_range, flags,
                                 texture_zoom, curve_distance, absolute_distance)) { *oom = 1; wz3__mm_free(out); return NULL; }
      if ((flags & 24) == 24) absolute_distance = (float)((double)absolute_distance + (double)curve_distance * texture_zoom);
    }
    float first_matrix[16], second_matrix[16], difference[3];
    wz3__mm_pipe_key_matrix(&spline->keys[segment * 2], first_matrix);
    wz3__mm_pipe_key_matrix(&spline->keys[segment * 2 + 1], second_matrix);
    {
      const float p1[3] = {second_matrix[12], second_matrix[13], second_matrix[14]};
      const float p0[3] = {first_matrix[12], first_matrix[13], first_matrix[14]};
      wz3__mm_sub3(p1, p0, difference);
    }
    const float distance = (float)sqrt(wz3__mm_dot3(difference, difference));
    double copies = 1.0;
    if (flags & 4) copies = wz3__mm_max(1, wz3__mm_min(64, trunc((double)distance / object_distance + 0.5)));
    for (int32_t copy = 0; (double)copy < copies; copy++) {
      if (wz3__mm_pipe_add_straight(out, mesh0, first_matrix, difference, mesh0_z_min, mesh0_z_range, copy, copies, flags, texture_zoom,
                                    distance, absolute_distance)) { *oom = 1; wz3__mm_free(out); return NULL; }
    }
    if (mesh2) {
      if (wz3__mm_pipe_add_transformed(out, mesh2, first_matrix) || wz3__mm_pipe_add_transformed(out, mesh2, second_matrix)) {
        *oom = 1; wz3__mm_free(out); return NULL;
      }
      if ((flags & 2) && ring_distance > 0.1) {
        const double rings = trunc((double)distance / ring_distance - 0.5);
        for (int32_t ring = 0; (double)ring < rings; ring++) {
          float matrix[16];
          memcpy(matrix, first_matrix, sizeof matrix);
          const float phase = (float)((double)(ring + 1) / (rings + 1));
          matrix[12] = (float)((double)matrix[12] + (double)difference[0] * (double)phase);
          matrix[13] = (float)((double)matrix[13] + (double)difference[1] * (double)phase);
          matrix[14] = (float)((double)matrix[14] + (double)difference[2] * (double)phase);
          if (wz3__mm_pipe_add_transformed(out, mesh2, matrix)) { *oom = 1; wz3__mm_free(out); return NULL; }
        }
      }
    }
    if (flags & 8) absolute_distance = (float)((double)absolute_distance + (double)distance * texture_zoom);
    memcpy(previous_matrix, second_matrix, sizeof previous_matrix);
  }
  if (wz3__mm_merge_clusters(out)) { *oom = 1; wz3__mm_free(out); return NULL; }
  return out;
}

/* ---- Explode ---- */

static void wz3__mm_explode_area(const float a[3], const float b[3], const float c[3], float out[3])
{
  float d0[3], d1[3];
  wz3__mm_sub3(a, b, d0);
  wz3__mm_sub3(b, c, d1);
  wz3__mm_cross3(d0, d1, out);
}

static void wz3__mm_vertex_for_shard(wz3__mm_vertex *v, int32_t merge_tag, int32_t shard)
{
  v->merge_tag = merge_tag;
  v->bone_count = 1;
  v->matrices[0] = (uint16_t)shard;
  v->weights[0] = 1.0f;
}

/* MinMesh_Explode; `source` gets its normals calculated like the JS does (the input is shared) */
static wz3_minmesh *wz3__mm_op_explode(wz3_minmesh *source, const double p[21], const wz3_bitmap *bitmap)
{
  wz3__mm_calc_normals(source);
  wz3__bilinear sampler;
  const int use_bitmap = bitmap && bitmap->data;
  if (use_bitmap) wz3__bilinear_init(&sampler, bitmap, 0, bitmap->width, bitmap->height);
  const float center[3] = {(float)p[0], (float)p[1], (float)p[2]};
  const double extrude = p[3], speed_normal = p[4], speed_center = p[5], speed_gravity = p[6], speed_random = p[7];
  const float rotation_speed[3] = {(float)p[8], (float)p[9], (float)p[10]};
  const double to_distance = p[11], to_power = p[12], to_random = p[13], to_constant = p[14];
  const int32_t mode = wz3__mm_to_int32(p[15]);
  float towards[3], towards_scaled[3];
  towards[0] = (float)(p[16] - (double)center[0]); towards[1] = (float)(p[17] - (double)center[1]); towards[2] = (float)(p[18] - (double)center[2]);
  const double towards_div = wz3__mm_dot3(towards, towards);
  for (int i = 0; i < 3; i++) towards_scaled[i] = (float)((double)towards[i] / towards_div);
  const double extrude_flat = 1.0 - p[19], tensor_random = p[20];
  wz3_minmesh *out = wz3__mm_new();
  if (!out) return NULL;
  uint32_t live = 0;
  for (uint32_t i = 0; i < source->n_faces; i++) if (source->faces[i].count >= 3 && source->faces[i].cluster > 0) live++;
  if (!live || live > 0xffffu) return out;
  wz3_rng random;
  wz3_rng_init(&random);
  wz3_rng_seed(&random, 1);
  wz3__mm_bone *bones = (wz3__mm_bone *)WZ3_MALLOC((size_t)live * sizeof(wz3__mm_bone));
  if (!bones) { wz3__mm_free(out); return NULL; }
  uint32_t shard = 0;
  for (uint32_t face_index = 0; face_index < source->n_faces; face_index++) {
    const wz3__mm_face *face = &source->faces[face_index];
    if (!(face->count >= 3 && face->cluster > 0)) continue;
    const int32_t *c = wz3__mm_corners(source, face);
    int32_t indices[64];
    int32_t n = face->count > 64 ? 64 : face->count;
    float face_center[3] = {0, 0, 0};
    for (int32_t k = 0; k < face->count; k++) wz3__mm_add3(face_center, source->verts[c[k]].position, 1.0);
    wz3__mm_scale3(face_center, 1.0 / (double)face->count);
    for (int32_t k = 0; k < n; k++) indices[k] = c[k];
    if (n == 4 && (mode & 2)) {
      const float *p0 = source->verts[indices[0]].position, *p1 = source->verts[indices[1]].position;
      const float *p2 = source->verts[indices[2]].position, *p3 = source->verts[indices[3]].position;
      float a0[3], a1[3], a2[3], a3[3];
      wz3__mm_explode_area(p0, p1, p2, a1);
      wz3__mm_explode_area(p1, p2, p3, a2);
      wz3__mm_explode_area(p2, p3, p0, a3);
      wz3__mm_explode_area(p3, p0, p1, a0);
      if (wz3__mm_dot3(a0, a2) > wz3__mm_dot3(a1, a3)) {
        const int32_t first = indices[0];
        indices[0] = indices[1]; indices[1] = indices[2]; indices[2] = indices[3]; indices[3] = first;
      }
    }
    const int32_t merge_tag = (int32_t)(face_index & 255u);
    int32_t top[64], bottom[64];
    for (int32_t k = 0; k < n; k++) {
      wz3__mm_vertex *v;
      top[k] = (int32_t)out->n_verts;
      if (wz3__mm_push_vertex(out, &source->verts[indices[k]], &v)) goto fail;
      wz3__mm_vertex_for_shard(v, merge_tag, (int32_t)shard);
    }
    for (int32_t k = 0; k < n; k++) {
      const wz3__mm_vertex *original = &source->verts[indices[k]];
      wz3__mm_vertex *v;
      bottom[k] = (int32_t)out->n_verts;
      if (wz3__mm_push_vertex(out, original, &v)) goto fail;
      wz3__mm_vertex_for_shard(v, merge_tag, (int32_t)shard);
      float difference[3];
      wz3__mm_sub3(v->position, face_center, difference);
      wz3__mm_scale3(difference, extrude_flat);
      memcpy(v->position, face_center, sizeof v->position);
      wz3__mm_add3(v->position, difference, 1.0);
      wz3__mm_add3(v->position, original->normal, -extrude);
    }
    for (int32_t k = 2; k < n; k++) {
      const int32_t tri[3] = {top[0], top[k - 1], top[k]};
      if (wz3__mm_push_face(out, tri, 3, 1, 0, NULL)) goto fail;
    }
    for (int32_t k = 2; k < n; k++) {
      const int32_t tri[3] = {bottom[k], bottom[k - 1], bottom[0]};
      if (wz3__mm_push_face(out, tri, 3, 1, 0, NULL)) goto fail;
    }
    for (int32_t k = 0; k < n; k++) {
      const wz3__mm_vertex *a = &source->verts[indices[k]], *b = &source->verts[indices[(k + 1) % n]];
      const int32_t base = (int32_t)out->n_verts;
      const wz3__mm_vertex *sources[4] = {a, b, a, b};
      for (int s = 0; s < 4; s++) {
        wz3__mm_vertex *v;
        if (wz3__mm_push_vertex(out, sources[s], &v)) goto fail;
        wz3__mm_vertex_for_shard(v, merge_tag, (int32_t)shard);
        if (s >= 2) {
          float difference[3];
          wz3__mm_sub3(v->position, face_center, difference);
          wz3__mm_scale3(difference, extrude_flat);
          memcpy(v->position, face_center, sizeof v->position);
          wz3__mm_add3(v->position, difference, 1.0);
          wz3__mm_add3(v->position, s == 2 ? a->normal : b->normal, -extrude);
        }
      }
      const int32_t quad[4] = {base + 2, base + 3, base + 1, base};
      if (wz3__mm_push_face(out, quad, 4, 1, 1, NULL)) goto fail;
    }
    float radial[3], speed[3] = {0, 0, 0};
    wz3__mm_sub3(face_center, center, radial);
    wz3__mm_add3(speed, radial, speed_center);
    wz3__mm_add3(speed, face->normal, speed_normal);
    for (int a = 0; a < 3; a++) speed[a] = (float)((double)speed[a] + ((double)wz3_rng_float(&random, 2.0f) - 1.0) * speed_random);
    double delay;
    if (use_bitmap) {
      /* the shard's delay comes from the red channel at its mean UV (the v coordinate scales by the width, as in the source) */
      float u = 0.0f, v = 0.0f;
      for (int32_t k = 0; k < face->count; k++) {
        u = (float)((double)u + (double)source->verts[c[k]].uv[0][0]);
        v = (float)((double)v + (double)source->verts[c[k]].uv[0][1]);
      }
      u = (float)((double)u / (double)face->count);
      v = (float)((double)v / (double)face->count);
      uint16_t texel[4];
      wz3__bilinear_sample(&sampler, wz3__rt_to_int32(trunc((double)u * 65536.0 * (double)bitmap->width)),
                           wz3__rt_to_int32(trunc((double)v * 65536.0 * (double)bitmap->width)), texel);
      const float base = (float)(1.0 - (double)(float)((double)texel[0] / 32767.0));
      delay = texel[0] ? (double)(float)(-to_distance * wz3_pow(base, to_power)) : -9999.0;
    } else if (mode & 1) {
      const double projection = wz3__mm_dot3(radial, towards_scaled);
      float scaled[3], off_axis[3];
      memcpy(scaled, towards, sizeof scaled);
      wz3__mm_scale3(scaled, projection);
      wz3__mm_sub3(radial, scaled, off_axis);
      double distance = sqrt(wz3__mm_dot3(off_axis, off_axis)) - to_power;
      if (distance < 0) {
        distance = distance < -to_power ? 0 : to_power + distance;
        delay = projection - sqrt(to_power * to_power - distance * distance) / sqrt(towards_div);
        distance = 0;
      } else delay = projection;
      delay = (float)((double)(float)(-to_distance * distance) - delay);
    } else {
      const float radius2 = (float)((double)(float)((double)(float)((double)radial[0] * radial[0]) + (double)(float)((double)radial[1] * radial[1])) +
                                    (double)(float)((double)radial[2] * radial[2]));
      delay = (float)(-to_distance * wz3_pow(radius2, to_power));
    }
    {
      const float jitter = (float)((double)wz3_rng_float(&random, 1.0f) * to_random);
      const float inner = (float)(-to_constant - (double)jitter);
      delay = (float)(delay + (double)inner);
    }
    wz3__mm_bone *b = &bones[shard];
    memset(b, 0, sizeof *b);
    wz3__mm_translation(b->no_animation, face_center[0], face_center[1], face_center[2]);
    wz3__mm_translation(b->base_pose, -(double)face_center[0], -(double)face_center[1], -(double)face_center[2]);
    b->parent = -1; b->factor = 1.0f; b->spread = 0.0f; b->offset = (float)delay;
    b->spline_kind = WZ3__MM_SPLINE_KINETIC;
    memcpy(b->kin_position, face_center, sizeof b->kin_position);
    memcpy(b->kin_speed, speed, sizeof b->kin_speed);
    b->kin_gravity[0] = 0.0f; b->kin_gravity[1] = (float)speed_gravity; b->kin_gravity[2] = 0.0f;
    if (mode & 4) {
      float axis[3];
      for (int a = 0; a < 3; a++) axis[a] = (float)((double)wz3_rng_float(&random, 2.0f) - 1.0);
      wz3__mm_normalize3(axis, axis);
      memcpy(b->kin_axis, axis, sizeof axis);
      b->kin_has_axis = 1;
      b->kin_angle = (float)(((double)wz3_rng_float(&random, 2.0f) - 1.0) * tensor_random);
    } else {
      for (int a = 0; a < 3; a++) b->kin_rotation[a] = (float)(((double)wz3_rng_float(&random, 2.0f) - 1.0) * (double)rotation_speed[a]);
    }
    shard++;
  }
  out->bones = bones;
  out->n_bones = shard;
  out->clusters[1].anim_type = 2;
  out->completely_rigid = 1;
  wz3__mm_calc_normals(out);
  return out;
fail:
  WZ3_FREE(bones);
  wz3__mm_free(out);
  return NULL;
}

/* ---- Mesh_ToMin (genmesh.cpp) ---- */

static uint32_t wz3__mm_pack_byte(double component)
{
  const double t = trunc(component * 255);
  if (t != t) return 0;
  return t < 0 ? 0u : (t > 255 ? 255u : (uint32_t)t);
}

static uint32_t wz3__mm_pack_color(double red, double green, double blue, double alpha)
{
  return (wz3__mm_pack_byte(alpha) << 24) | (wz3__mm_pack_byte(red) << 16) | (wz3__mm_pack_byte(green) << 8) | wz3__mm_pack_byte(blue);
}

static wz3_minmesh *wz3__mm_from_mesh(const wz3_mesh *src)
{
  wz3_minmesh *out = wz3__mm_new();
  if (!out) return NULL;
  /* the slot table becomes the cluster table; a non-null slot 0 would get a leading null cluster (never created) */
  out->n_clusters = 0;
  for (uint32_t i = 0; i < src->n_slots; i++)
    if (wz3__mm_push_cluster(out, wz3__mm_make_cluster(src->slots[i].material, src->slots[i].pass, (int32_t)i, 0, -1))) goto fail;
  if (!out->n_clusters || out->clusters[0].material != WZ3__MAT_NULL) {
    /* unshift(makeCluster(null)) */
    if (wz3__mm_push_cluster(out, out->clusters[0])) goto fail;
    memmove(out->clusters + 1, out->clusters, (size_t)(out->n_clusters - 1) * sizeof(wz3__mm_cluster));
    out->clusters[0] = wz3__mm_make_cluster(WZ3__MAT_NULL, 0, 0, 0, -1);
  }
  {
    const int has_color = (src->vertex_mask & WZ3__MF_COLOR0) != 0;
    const int has_uv0 = (src->vertex_mask & WZ3__MF_UV0) != 0;
    const int has_uv1 = (src->vertex_mask & WZ3__MF_UV1) != 0;
    int32_t *vertex_map = (int32_t *)WZ3_MALLOC((size_t)(src->n_verts ? src->n_verts : 1) * sizeof(int32_t));
    if (!vertex_map) goto fail;
    for (uint32_t i = 0; i < src->n_verts; i++) vertex_map[i] = -1;
    int32_t old_indices[256];
    for (uint32_t fi = 0; fi < src->n_faces; fi++) {
      const wz3__mface *face = &src->faces[fi];
      const int32_t material = face->material;
      if (!material) continue;
      const int32_t start = face->edge;
      uint32_t n = 0;
      int32_t *loop = old_indices;
      int32_t *heap = NULL;
      uint32_t cap = 256;
      if (start >= 0) {
        int32_t h = start;
        const uint32_t limit = src->n_edges * 2 + 1;
        do {
          if (n == cap) {
            int32_t *grown = (int32_t *)WZ3_MALLOC((size_t)cap * 2 * sizeof(int32_t));
            if (!grown) { WZ3_FREE(heap); WZ3_FREE(vertex_map); goto fail; }
            memcpy(grown, loop, (size_t)n * sizeof(int32_t));
            WZ3_FREE(heap);
            heap = loop = grown;
            cap *= 2;
          }
          loop[n++] = wz3__mh_vert(src, h);
          h = wz3__mh_next_face(src, h);
          if (n > limit) { WZ3_FREE(heap); WZ3_FREE(vertex_map); goto fail; }
        } while (h != start);
      }
      if (n < 3) { WZ3_FREE(heap); continue; }
      int32_t *mapped = (int32_t *)WZ3_MALLOC((size_t)n * sizeof(int32_t));
      if (!mapped) { WZ3_FREE(heap); WZ3_FREE(vertex_map); goto fail; }
      for (uint32_t k = 0; k < n; k++) {
        const int32_t old = loop[k];
        if (vertex_map[old] < 0) {
          const wz3__mvert *ov = &src->verts[old];
          const wz3__mvert *pv = (ov->first >= 0 && (uint32_t)ov->first < src->n_verts) ? &src->verts[ov->first] : ov;
          wz3__mm_vertex *v;
          vertex_map[old] = (int32_t)out->n_verts;
          if (wz3__mm_push_vertex(out, NULL, &v)) { WZ3_FREE(mapped); WZ3_FREE(heap); WZ3_FREE(vertex_map); goto fail; }
          for (int a = 0; a < 3; a++) v->position[a] = pv->v[0][a];
          if (has_uv0) { v->uv[0][0] = ov->v[4][0]; v->uv[0][1] = ov->v[4][1]; }
          if (has_uv1) { v->uv[1][0] = ov->v[5][0]; v->uv[1][1] = ov->v[5][1]; }
          v->color = has_color ? wz3__mm_pack_color(ov->v[3][0], ov->v[3][1], ov->v[3][2], ov->v[3][3]) : 0xffffffffu;
        }
        mapped[k] = vertex_map[old];
      }
      const int selected = face->select != 0;
      const uint32_t used_flag = face->used ? 0u : 1u;
      if (selected && n >= 8) {
        /* centroid vertex (addCentroid): float32 accumulation of value * (1/n) */
        float position[3] = {0, 0, 0}, color[4] = {0, 0, 0, 0}, uv[2] = {0, 0}, uv1[2] = {0, 0};
        const float inverse = (float)(1.0 / (double)n);
        for (uint32_t k = 0; k < n; k++) {
          const wz3__mvert *ov = &src->verts[loop[k]];
          const wz3__mvert *pv = (ov->first >= 0 && (uint32_t)ov->first < src->n_verts) ? &src->verts[ov->first] : ov;
          for (int a = 0; a < 3; a++) position[a] = (float)((double)position[a] + (double)pv->v[0][a] * (double)inverse);
          for (int a = 0; a < 4; a++) color[a] = (float)((double)color[a] + (double)ov->v[3][a] * (double)inverse);
          if (has_uv0) for (int a = 0; a < 2; a++) uv[a] = (float)((double)uv[a] + (double)ov->v[4][a] * (double)inverse);
          if (has_uv1) for (int a = 0; a < 2; a++) uv1[a] = (float)((double)uv1[a] + (double)ov->v[5][a] * (double)inverse);
        }
        wz3__mm_vertex *cv;
        const int32_t centroid = (int32_t)out->n_verts;
        if (wz3__mm_push_vertex(out, NULL, &cv)) { WZ3_FREE(mapped); WZ3_FREE(heap); WZ3_FREE(vertex_map); goto fail; }
        memcpy(cv->position, position, sizeof position);
        cv->color = has_color ? wz3__mm_pack_color(color[0], color[1], color[2], color[3]) : 0xffffffffu;
        if (has_uv0) memcpy(cv->uv[0], uv, sizeof uv);
        if (has_uv1) memcpy(cv->uv[1], uv1, sizeof uv1);
        for (uint32_t k = 0; k < n; k++) {
          const int32_t tri[3] = {centroid, mapped[k], mapped[(k + 1) % n]};
          if (wz3__mm_push_face(out, tri, 3, material, 0, NULL)) { WZ3_FREE(mapped); WZ3_FREE(heap); WZ3_FREE(vertex_map); goto fail; }
          out->faces[out->n_faces - 1].flags = used_flag;
        }
      } else {
        /* an unselected high-degree face keeps only its first eight corners (KMM_MAXVERT) */
        if (wz3__mm_push_face(out, mapped, n > 8 ? 8 : (int32_t)n, material, 0, NULL)) { WZ3_FREE(mapped); WZ3_FREE(heap); WZ3_FREE(vertex_map); goto fail; }
        out->faces[out->n_faces - 1].flags = used_flag;
      }
      WZ3_FREE(mapped);
      WZ3_FREE(heap);
    }
    WZ3_FREE(vertex_map);
  }
  wz3__mm_invalidate(out);
  return out;
fail:
  wz3__mm_free(out);
  return NULL;
}

/* ---- Font3D (genminmesh.cpp with the deterministic Arimo/Gelasio outlines) ---- */

#define WZ3__F3D_NATIVE_SCALE 128.0
#define WZ3__F3D_POINT_LIMIT 32768u
#define WZ3__F3D_TRIANGLE_LIMIT 32768u

typedef struct wz3__f3d_points {
  float *xy;                  /* x, y pairs */
  uint32_t n, cap;
  int oom;
} wz3__f3d_points;

static int wz3__f3d_points_push(wz3__f3d_points *p, float x, float y)
{
  if (p->n == p->cap) {
    const uint32_t cap = p->cap ? p->cap * 2 : 256u;
    float *grown = (float *)WZ3_MALLOC((size_t)cap * 2 * sizeof(float));
    if (!grown) { p->oom = 1; return -1; }
    if (p->n) memcpy(grown, p->xy, (size_t)p->n * 2 * sizeof(float));
    WZ3_FREE(p->xy);
    p->xy = grown; p->cap = cap;
  }
  p->xy[p->n * 2] = x; p->xy[p->n * 2 + 1] = y;
  p->n++;
  return 0;
}

static double wz3__f3d_round_fixed(double value) { return value < 0 ? ceil(value - 0.5) : floor(value + 0.5); }

/* GGO_UNHINTED 26.6 points expanded to POINTFX 16.16 by bit repetition, then divided by 128 */
static float wz3__f3d_point_fx(double value, double units_per_em, double ppem)
{
  if (!(units_per_em > 0) || !(ppem > 0)) return 0.0f;
  const int32_t fixed = wz3__rt_to_int32(wz3__f3d_round_fixed(value * ppem * 64 / units_per_em));
  const int32_t integer = fixed >> 6;
  int32_t fraction = (int32_t)(((uint32_t)(fixed & 0x3f)) << 10);
  fraction |= (fraction >> 6) | (fraction >> 12);
  const double full = (double)integer * 65536.0 + (double)fraction;
  const float logical = (float)((double)(float)full / (double)(float)65536.0);
  return (float)((double)logical / (double)(float)WZ3__F3D_NATIVE_SCALE);
}

static void wz3__f3d_lerp(const float a[2], const float b[2], float out[2])
{
  const float dx = (float)((double)b[0] - (double)a[0]), dy = (float)((double)b[1] - (double)a[1]);
  const float hx = (float)(0.5 * (double)dx), hy = (float)(0.5 * (double)dy);
  out[0] = (float)((double)a[0] + (double)hx);
  out[1] = (float)((double)a[1] + (double)hy);
}

static int wz3__f3d_quadratic(wz3__f3d_points *out, const float start[2], const float control[2], const float end[2], float tolerance_sq, int depth)
{
  float middle[2];
  wz3__f3d_lerp(start, end, middle);
  const float dx = (float)((double)middle[0] - (double)control[0]);
  const float dy = (float)((double)middle[1] - (double)control[1]);
  const float distance_sq = (float)((double)(float)((double)dx * dx) + (double)(float)((double)dy * dy));
  if (depth >= 12 || distance_sq <= tolerance_sq) {
    if (out->n >= WZ3__F3D_POINT_LIMIT) return -1;
    return wz3__f3d_points_push(out, end[0], end[1]);
  }
  float left[2], right[2], mid[2];
  wz3__f3d_lerp(start, control, left);
  wz3__f3d_lerp(control, end, right);
  wz3__f3d_lerp(left, right, mid);
  if (wz3__f3d_quadratic(out, start, left, mid, tolerance_sq, depth + 1)) return -1;
  return wz3__f3d_quadratic(out, mid, right, end, tolerance_sq, depth + 1);
}

/* flattenFont3DContour: `encoded` is n_points (x, y, on-curve) triples; returns the flattened ring in *out */
static int wz3__f3d_flatten(const int16_t *encoded, uint32_t n_points, double units_per_em, float height, double max_error, double ppem,
                            wz3__f3d_points *out)
{
  if (n_points < 3) return 0;
  float *pts = (float *)WZ3_MALLOC((size_t)n_points * 3 * sizeof(float) + 3 * sizeof(float));
  if (!pts) { out->oom = 1; return -1; }
  float (*points)[3] = (float (*)[3])pts;
  for (uint32_t i = 0; i < n_points; i++) {
    points[i][0] = wz3__f3d_point_fx(encoded[i * 3], units_per_em, ppem);
    points[i][1] = wz3__f3d_point_fx(encoded[i * 3 + 1], units_per_em, ppem);
    points[i][2] = encoded[i * 3 + 2] ? 1.0f : 0.0f;
  }
  float (*sequence)[3] = points;
  uint32_t n_sequence;
  float start[2];
  const float *first = points[0], *last = points[n_points - 1];
  if (first[2] != 0.0f) {
    start[0] = first[0]; start[1] = first[1];
    sequence = points + 1; n_sequence = n_points - 1;
  } else if (last[2] != 0.0f) {
    start[0] = last[0]; start[1] = last[1];
    n_sequence = n_points - 1;
  } else {
    wz3__f3d_lerp(first, last, start);
    n_sequence = n_points;
  }
  /* the pushed on-curve start point needs one more slot: copy into a roomier buffer */
  float (*seq)[3] = (float (*)[3])WZ3_MALLOC((size_t)(n_sequence + 1) * sizeof(float[3]));
  if (!seq) { WZ3_FREE(pts); out->oom = 1; return -1; }
  memcpy(seq, sequence, (size_t)n_sequence * sizeof(float[3]));
  seq[n_sequence][0] = start[0]; seq[n_sequence][1] = start[1]; seq[n_sequence][2] = 1.0f;
  n_sequence++;
  int result = 0;
  float tolerance_sq;
  {
    const double me = max_error > 0 ? max_error : 0.0;
    const float tolerance = (float)((double)(float)((double)height * (double)(float)0.1) * (double)(float)me);
    tolerance_sq = (float)((double)tolerance * tolerance);
  }
  if (wz3__f3d_points_push(out, start[0], start[1])) result = -1;
  float current[2] = {start[0], start[1]};
  for (uint32_t index = 0; index < n_sequence && !result; index++) {
    const float *point = seq[index];
    if (point[2] != 0.0f) {
      const float end[2] = {point[0], point[1]};
      if (end[0] != current[0] || end[1] != current[1]) result = wz3__f3d_points_push(out, end[0], end[1]);
      current[0] = end[0]; current[1] = end[1];
      continue;
    }
    const float *following = index + 1 < n_sequence ? seq[index + 1] : NULL;
    float end[2];
    if (!following) { result = -1; break; }
    if (following[2] != 0.0f) { end[0] = following[0]; end[1] = following[1]; }
    else wz3__f3d_lerp(point, following, end);
    if (wz3__f3d_quadratic(out, current, point, end, tolerance_sq, 0)) { result = -1; break; }
    current[0] = end[0]; current[1] = end[1];
    if (following[2] != 0.0f) index++;
  }
  if (!result && out->n > 1 && out->xy[(out->n - 1) * 2] == out->xy[0] && out->xy[(out->n - 1) * 2 + 1] == out->xy[1]) out->n--;
  WZ3_FREE(pts);
  WZ3_FREE(seq);
  return result;
}

typedef struct wz3__f3d_fit {
  uint32_t first_vertex, end_vertex;
  double offset_x;
  const wz3__f3d_glyph *glyph;
  double units, ppem;
} wz3__f3d_fit;

typedef struct wz3__f3d_sink {
  wz3_minmesh *mesh;
  int failed;
} wz3__f3d_sink;

static int32_t wz3__f3d_combine(void *user, const double c[3])
{
  wz3__f3d_sink *s = (wz3__f3d_sink *)user;
  wz3__mm_vertex *v;
  const int32_t index = (int32_t)s->mesh->n_verts;
  if (!(isfinite(c[0]) && isfinite(c[1]) && isfinite(c[2]))) s->failed = 1;
  if (wz3__mm_push_vertex(s->mesh, NULL, &v)) { s->failed = 1; return -1; }
  v->position[0] = (float)((c[0] == c[0]) ? c[0] : 0.0);
  v->position[1] = (float)((c[1] == c[1]) ? c[1] : 0.0);
  v->position[2] = (float)((c[2] == c[2]) ? c[2] : 0.0);
  return index;
}

static int wz3__f3d_emit(void *user, int32_t a, int32_t b, int32_t c)
{
  wz3__f3d_sink *s = (wz3__f3d_sink *)user;
  const int32_t n = (int32_t)s->mesh->n_verts;
  if (a < 0 || a >= n || b < 0 || b >= n || c < 0 || c >= n) { s->failed = 1; return -1; }
  if (s->mesh->n_faces >= WZ3__F3D_TRIANGLE_LIMIT) { s->failed = 1; return -1; }
  const int32_t tri[3] = {a, b, c};
  if (wz3__mm_push_face(s->mesh, tri, 3, 1, 0, NULL)) { s->failed = 1; return -1; }
  return 0;
}

/* appendFontGLUContours for one glyph: one GLU polygon, a contour per ring of at least three points */
static int wz3__f3d_append(wz3__lt_tess *tess, wz3__f3d_sink *sink, const wz3__f3d_points *rings, const uint32_t *ring_lengths, uint32_t n_rings,
                           double offset_x)
{
  if (wz3__lt_begin_polygon(tess)) return -1;
  uint32_t cursor = 0;
  for (uint32_t r = 0; r < n_rings; r++) {
    const uint32_t len = ring_lengths[r];
    const float *xy = rings->xy + (size_t)cursor * 2;
    cursor += len;
    if (len < 3) continue;
    wz3__lt_begin_contour(tess);
    for (uint32_t i = 0; i < len; i++) {
      wz3__mm_vertex *v;
      const int32_t index = (int32_t)sink->mesh->n_verts;
      const double x = offset_x + (double)xy[i * 2] * 1.0;
      const double y = 0.0 + (double)xy[i * 2 + 1] * 1.0;
      if (!(isfinite(x) && isfinite(y))) { sink->failed = 1; continue; }
      if (wz3__mm_push_vertex(sink->mesh, NULL, &v)) { sink->failed = 1; continue; }
      v->position[0] = (float)x; v->position[1] = (float)y; v->position[2] = 0.0f;
      v->normal[2] = -1.0f;
      const double coords[3] = {v->position[0], v->position[1], v->position[2]};
      wz3__lt_add_vertex(tess, coords, index);
    }
  }
  if (wz3__lt_end_polygon(tess)) sink->failed = 1;
  return sink->failed ? -1 : 0;
}

static int wz3__f3d_face_adjacency(wz3_minmesh *m, int32_t *adjacent)
{
  /* calculateFontTriangleAdjacency: weld vertices by position, pair triangle edges, at most two per edge */
  const uint32_t nv = m->n_verts, nf = m->n_faces;
  uint32_t *order = (uint32_t *)WZ3_MALLOC((size_t)(nv ? nv : 1) * sizeof(uint32_t));
  uint32_t *remap = (uint32_t *)WZ3_MALLOC((size_t)(nv ? nv : 1) * sizeof(uint32_t));
  uint32_t *keys = (uint32_t *)WZ3_MALLOC((size_t)(nf ? nf * 3 : 1) * 3 * sizeof(uint32_t));
  uint32_t *tmp = (uint32_t *)WZ3_MALLOC((size_t)(nf ? nf * 3 : 1) * 3 * sizeof(uint32_t));
  if (!order || !remap || !keys || !tmp) { WZ3_FREE(order); WZ3_FREE(remap); WZ3_FREE(keys); WZ3_FREE(tmp); return -1; }
  for (uint32_t i = 0; i < nv; i++) order[i] = i;
  /* bottom-up merge sort of the vertex indices by (x, y, z), stable in the index */
  uint32_t *work = (uint32_t *)WZ3_MALLOC((size_t)(nv ? nv : 1) * sizeof(uint32_t));
  if (!work) { WZ3_FREE(order); WZ3_FREE(remap); WZ3_FREE(keys); WZ3_FREE(tmp); return -1; }
  for (uint32_t width = 1; width < nv; width *= 2) {
    for (uint32_t lo = 0; lo < nv; lo += 2 * width) {
      const uint32_t mid = lo + width < nv ? lo + width : nv, hi = lo + 2 * width < nv ? lo + 2 * width : nv;
      uint32_t i = lo, j = mid, k = lo;
      while (i < mid && j < hi) {
        const float *a = m->verts[order[i]].position, *b = m->verts[order[j]].position;
        int less = 0;
        for (int axis = 0; axis < 3; axis++) { if (b[axis] < a[axis]) { less = 1; break; } if (a[axis] < b[axis]) break; }
        work[k++] = less ? order[j++] : order[i++];
      }
      while (i < mid) work[k++] = order[i++];
      while (j < hi) work[k++] = order[j++];
    }
    memcpy(order, work, (size_t)nv * sizeof(uint32_t));
  }
  WZ3_FREE(work);
  for (uint32_t i = 0; i < nv;) {
    uint32_t j = i + 1;
    while (j < nv) {
      const float *a = m->verts[order[i]].position, *b = m->verts[order[j]].position;
      if (!(a[0] == b[0] && a[1] == b[1] && a[2] == b[2])) break;
      j++;
    }
    for (uint32_t k = i; k < j; k++) remap[order[k]] = order[i];
    i = j;
  }
  WZ3_FREE(order);
  for (uint32_t i = 0; i < nf * 3; i++) adjacent[i] = -1;
  /* edge records: (a, b, tag), sorted by (a, b) stably; more than two tags on one edge fails */
  uint32_t n_edges = 0;
  for (uint32_t fi = 0; fi < nf; fi++) {
    const wz3__mm_face *f = &m->faces[fi];
    if (f->count != 3) { WZ3_FREE(remap); WZ3_FREE(keys); WZ3_FREE(tmp); return -2; }
    const int32_t *c = wz3__mm_corners(m, f);
    for (uint32_t e = 0; e < 3; e++) {
      uint32_t a = remap[c[e]], b = remap[c[(e + 1) % 3]];
      if (a > b) { const uint32_t t = a; a = b; b = t; }
      keys[n_edges * 3] = a; keys[n_edges * 3 + 1] = b; keys[n_edges * 3 + 2] = (fi << 3) | e;
      n_edges++;
    }
  }
  for (uint32_t width = 1; width < n_edges; width *= 2) {
    for (uint32_t lo = 0; lo < n_edges; lo += 2 * width) {
      const uint32_t mid = lo + width < n_edges ? lo + width : n_edges, hi = lo + 2 * width < n_edges ? lo + 2 * width : n_edges;
      uint32_t i = lo, j = mid, k = lo;
      while (i < mid && j < hi) {
        const uint32_t *a = &keys[i * 3], *b = &keys[j * 3];
        const int less = b[0] < a[0] || (b[0] == a[0] && b[1] < a[1]);
        memcpy(&tmp[k * 3], less ? b : a, 3 * sizeof(uint32_t));
        if (less) j++; else i++;
        k++;
      }
      while (i < mid) { memcpy(&tmp[k * 3], &keys[i * 3], 3 * sizeof(uint32_t)); i++; k++; }
      while (j < hi) { memcpy(&tmp[k * 3], &keys[j * 3], 3 * sizeof(uint32_t)); j++; k++; }
    }
    memcpy(keys, tmp, (size_t)n_edges * 3 * sizeof(uint32_t));
  }
  int result = 0;
  for (uint32_t i = 0; i < n_edges && !result;) {
    uint32_t j = i + 1;
    while (j < n_edges && keys[j * 3] == keys[i * 3] && keys[j * 3 + 1] == keys[i * 3 + 1]) j++;
    if (j - i > 2) result = -2;
    else if (j - i == 2) {
      const uint32_t first = keys[i * 3 + 2], second = keys[(i + 1) * 3 + 2];
      adjacent[(first >> 3) * 3 + (first & 7)] = (int32_t)second;
      adjacent[(second >> 3) * 3 + (second & 7)] = (int32_t)first;
    }
    i = j;
  }
  WZ3_FREE(remap); WZ3_FREE(keys); WZ3_FREE(tmp);
  return result;
}

static void wz3__f3d_redirect(int32_t *adjacent, int32_t tag, int32_t replacement)
{
  if (tag >= 0) adjacent[(tag >> 3) * 3 + (tag & 7)] = replacement;
}

static void wz3__f3d_flip_edge(wz3_minmesh *m, int32_t *adjacent, int32_t face_index, int32_t edge)
{
  const int32_t opposite = adjacent[face_index * 3 + edge];
  if (opposite < 0) return;
  const int32_t second_index = opposite >> 3, second_edge = opposite & 7;
  int32_t *first = m->corners + m->faces[face_index].first, *second = m->corners + m->faces[second_index].first;
  if (m->faces[face_index].count != 3 || m->faces[second_index].count != 3) return;
  const int32_t va = first[edge], vb = second[(second_edge + 2) % 3], vc = second[second_edge], vd = first[(edge + 2) % 3];
  const int32_t outer_ab = adjacent[second_index * 3 + (second_edge + 1) % 3];
  const int32_t outer_bc = adjacent[second_index * 3 + (second_edge + 2) % 3];
  const int32_t outer_cd = adjacent[face_index * 3 + (edge + 1) % 3];
  const int32_t outer_da = adjacent[face_index * 3 + (edge + 2) % 3];
  first[0] = vb; first[1] = vd; first[2] = va;
  second[0] = vd; second[1] = vb; second[2] = vc;
  adjacent[face_index * 3] = second_index << 3; adjacent[face_index * 3 + 1] = outer_da; adjacent[face_index * 3 + 2] = outer_ab;
  adjacent[second_index * 3] = face_index << 3; adjacent[second_index * 3 + 1] = outer_bc; adjacent[second_index * 3 + 2] = outer_cd;
  wz3__f3d_redirect(adjacent, outer_ab, (face_index << 3) | 2);
  wz3__f3d_redirect(adjacent, outer_bc, (second_index << 3) | 1);
  wz3__f3d_redirect(adjacent, outer_cd, (second_index << 3) | 2);
  wz3__f3d_redirect(adjacent, outer_da, (face_index << 3) | 1);
}

/* the single post-GLU pass: flip the first internal edge of every reversed or near-degenerate triangle */
static int wz3__f3d_clean(wz3_minmesh *m, int32_t *adjacent)
{
  const int adjacency = wz3__f3d_face_adjacency(m, adjacent);     /* -1 out of memory, -2 not a triangle mesh with at most two faces per edge */
  if (adjacency) return adjacency;
  for (uint32_t fi = 0; fi < m->n_faces; fi++) {
    const int32_t *c = wz3__mm_corners(m, &m->faces[fi]);
    const float *p0 = m->verts[c[0]].position, *p1 = m->verts[c[1]].position, *p2 = m->verts[c[2]].position;
    const float d1x = (float)((double)p1[0] - (double)p0[0]), d1y = (float)((double)p1[1] - (double)p0[1]);
    const float d2x = (float)((double)p2[0] - (double)p0[0]), d2y = (float)((double)p2[1] - (double)p0[1]);
    const float normal_z = (float)((double)(float)((double)d2x * (double)d1y) - (double)(float)((double)d2y * (double)d1x));
    if ((double)normal_z >= 1e-6) continue;
    for (int32_t edge = 0; edge < 3; edge++) if (adjacent[fi * 3 + edge] >= 0) { wz3__f3d_flip_edge(m, adjacent, (int32_t)fi, edge); break; }
  }
  return 0;
}

static int wz3__f3d_fit_range(wz3_minmesh *m, const wz3__f3d_fit *fit)
{
  if (!(fit->end_vertex > fit->first_vertex) || !fit->glyph->has_bounds || !(fit->units > 0) || !(fit->ppem > 0)) return -1;
  float min_x = INFINITY, min_y = INFINITY, max_x = -INFINITY, max_y = -INFINITY;
  for (uint32_t i = fit->first_vertex; i < fit->end_vertex; i++) {
    const float *p = m->verts[i].position;
    min_x = (float)wz3__mm_min(min_x, p[0]); min_y = (float)wz3__mm_min(min_y, p[1]);
    max_x = (float)wz3__mm_max(max_x, p[0]); max_y = (float)wz3__mm_max(max_y, p[1]);
  }
  if (!(max_x > min_x) || !(max_y > min_y)) return -1;
  const int16_t *rb = fit->glyph->bounds;
  const float target_min_x = (float)(fit->offset_x + (double)wz3__f3d_point_fx(rb[0], fit->units, fit->ppem));
  const float target_min_y = wz3__f3d_point_fx(rb[1], fit->units, fit->ppem);
  const float target_max_x = (float)(fit->offset_x + (double)wz3__f3d_point_fx(rb[2], fit->units, fit->ppem));
  const float target_max_y = wz3__f3d_point_fx(rb[3], fit->units, fit->ppem);
  for (uint32_t i = fit->first_vertex; i < fit->end_vertex; i++) {
    float *p = m->verts[i].position;
    if (p[0] == min_x) p[0] = target_min_x;
    else if (p[0] == max_x) p[0] = target_max_x;
    else p[0] = (float)((double)target_min_x + ((double)p[0] - (double)min_x) * ((double)target_max_x - (double)target_min_x) / ((double)max_x - (double)min_x));
    if (p[1] == min_y) p[1] = target_min_y;
    else if (p[1] == max_y) p[1] = target_max_y;
    else p[1] = (float)((double)target_min_y + ((double)p[1] - (double)min_y) * ((double)target_max_y - (double)target_min_y) / ((double)max_y - (double)min_y));
  }
  return 0;
}

static int wz3__f3d_extrude(wz3_minmesh *m, const int32_t *adjacent, double depth)
{
  if (depth == 0.0 || depth != depth || !m->n_faces) return 0;
  const uint32_t front_vertices = m->n_verts, front_faces = m->n_faces;
  for (uint32_t i = 0; i < front_vertices; i++) {
    wz3__mm_vertex *v;
    wz3__mm_vertex copy = m->verts[i];
    if (wz3__mm_push_vertex(m, &copy, &v)) return -1;
    v->position[2] = (float)depth;
  }
  for (uint32_t i = 0; i < front_faces; i++) {
    const int32_t *c = wz3__mm_corners(m, &m->faces[i]);
    const int32_t tri[3] = {c[2] + (int32_t)front_vertices, c[1] + (int32_t)front_vertices, c[0] + (int32_t)front_vertices};
    if (wz3__mm_push_face(m, tri, 3, 1, 0, NULL)) return -1;
  }
  for (uint32_t i = 0; i < front_vertices * 2; i++) {
    wz3__mm_vertex copy = m->verts[i];
    if (wz3__mm_push_vertex(m, &copy, NULL)) return -1;
  }
  for (uint32_t fi = 0; fi < front_faces; fi++) {
    for (int32_t edge = 0; edge < 3; edge++) {
      if (adjacent[fi * 3 + edge] != -1) continue;
      const int32_t *c = wz3__mm_corners(m, &m->faces[fi]);
      const int32_t a = c[edge], b = c[(edge + 1) % 3];
      const int32_t quad[4] = {b + (int32_t)front_vertices * 2, a + (int32_t)front_vertices * 2, a + (int32_t)front_vertices * 3, b + (int32_t)front_vertices * 3};
      if (wz3__mm_push_face(m, quad, 4, 1, 0, NULL)) return -1;
    }
  }
  return 0;
}

static const wz3__f3d_family *wz3__f3d_find_family(const char *font)
{
  /* font3DFamily: trim, drop trailing periods, default "Arial"; the table is looked up lower case */
  char name[32];
  size_t len = 0;
  const char *s = font ? font : "";
  while (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r') s++;
  size_t n = strlen(s);
  while (n && (s[n - 1] == ' ' || s[n - 1] == '\t' || s[n - 1] == '\n' || s[n - 1] == '\r')) n--;
  while (n && s[n - 1] == '.') n--;
  if (!n) { s = "Arial"; n = 5; }
  for (; len < n && len < sizeof name - 1; len++) name[len] = (char)((s[len] >= 'A' && s[len] <= 'Z') ? s[len] + 32 : s[len]);
  name[len] = 0;
  for (uint32_t i = 0; i < WZ3__F3D_N_FAMILIES; i++) if (strcmp(wz3__f3d_families[i].name, name) == 0) return &wz3__f3d_families[i];
  return NULL;
}

/* MinMesh_Font3D; *res receives WZ3_ERR_UNSUPPORTED for a family without an embedded outline set and
   WZ3_ERR_OPERATOR when the JS port would throw (missing glyph, curve limit, non-manifold tessellation) */
static wz3_minmesh *wz3__mm_op_font3d(double height_param, double extrude, double max_error, const char *text, const char *font, wz3_result *res)
{
  *res = WZ3_OK;
  const double height_d = wz3__mm_max(height_param, 8.0 / 128.0);
  const float height = (float)(height_d == height_d ? (height_d > 0 ? height_d : 0.0) : 0.0);
  const wz3__f3d_family *family = wz3__f3d_find_family(font && *font ? font : "sans-serif");
  if (!family) { *res = WZ3_ERR_UNSUPPORTED; return NULL; }
  wz3_minmesh *m = wz3__mm_new();
  if (!m) { *res = WZ3_ERR_OUT_OF_MEMORY; return NULL; }
  wz3__f3d_sink sink;
  sink.mesh = m; sink.failed = 0;
  wz3__lt_tess *tess = (wz3__lt_tess *)WZ3_MALLOC(sizeof *tess);
  wz3__f3d_fit *fits = NULL;
  uint32_t n_fits = 0, cap_fits = 0;
  int32_t *adjacent = NULL;
  if (!tess) { wz3__mm_free(m); *res = WZ3_ERR_OUT_OF_MEMORY; return NULL; }
  memset(tess, 0, sizeof *tess);
  tess->user = &sink; tess->combine = wz3__f3d_combine; tess->emit = wz3__f3d_emit;
  double x = 0.0;
  const int32_t logical_height = wz3__rt_to_int32(trunc((double)height * 128.0));
  for (const unsigned char *s = (const unsigned char *)(text ? text : ""); *s;) {
    uint32_t character = *s++;
    if (character >= 0xc0 && (*s & 0xc0) == 0x80) {                  /* UTF-8 continuation bytes */
      const int extra = character >= 0xf0 ? 3 : character >= 0xe0 ? 2 : 1;
      character &= (0x3fu >> extra);
      for (int k = 0; k < extra && (*s & 0xc0) == 0x80; k++) character = (character << 6) | (*s++ & 0x3fu);
    }
    const wz3__f3d_glyph *glyph = NULL;
    for (uint32_t g = 0; g < family->n_glyphs; g++) if (family->glyphs[g].character == character) { glyph = &family->glyphs[g]; break; }
    if (!glyph) { *res = WZ3_ERR_OPERATOR; goto fail; }
    double ppem = (double)logical_height, units = (double)family->units_per_cell;
    int gdi_sized = 0;
    for (uint32_t k = 0; k < family->n_ppem; k++) if (family->ppem_logical[k] == logical_height && family->ppem_value[k] > 0) {
      gdi_sized = 1; ppem = (double)family->ppem_value[k]; units = (double)family->units_per_em;
    }
    /* flatten every contour of the glyph into rings */
    wz3__f3d_points rings;
    uint32_t ring_lengths[64];
    uint32_t n_rings = 0;
    memset(&rings, 0, sizeof rings);
    for (uint32_t c = 0; c < glyph->n_contours; c++) {
      const uint32_t *contour = wz3__f3d_contours[glyph->first_contour + c];
      wz3__f3d_points ring;
      memset(&ring, 0, sizeof ring);
      if (wz3__f3d_flatten(wz3__f3d_points_data + (size_t)contour[0] * 3, contour[1], units, height, max_error, ppem, &ring)) {
        *res = (ring.oom || m->oom) ? WZ3_ERR_OUT_OF_MEMORY : WZ3_ERR_OPERATOR;
        WZ3_FREE(ring.xy); WZ3_FREE(rings.xy); goto fail;
      }
      if (ring.n >= 3) {
        if (n_rings >= 64) { WZ3_FREE(ring.xy); WZ3_FREE(rings.xy); *res = WZ3_ERR_LIMIT; goto fail; }
        for (uint32_t i = 0; i < ring.n; i++) {
          if (wz3__f3d_points_push(&rings, ring.xy[i * 2], ring.xy[i * 2 + 1])) { WZ3_FREE(ring.xy); WZ3_FREE(rings.xy); *res = WZ3_ERR_OUT_OF_MEMORY; goto fail; }
        }
        ring_lengths[n_rings++] = ring.n;
      }
      WZ3_FREE(ring.xy);
    }
    const uint32_t first_vertex = m->n_verts;
    if (n_rings) {
      /* the polygon is only created for glyphs with contours: resolvedGlyph.contours.length is the unfiltered count */
      if (wz3__f3d_append(tess, &sink, &rings, ring_lengths, n_rings, x)) {
        *res = (tess->oom || m->oom) ? WZ3_ERR_OUT_OF_MEMORY : WZ3_ERR_OPERATOR;
        WZ3_FREE(rings.xy); goto fail;
      }
    }
    WZ3_FREE(rings.xy);
    if (n_rings) {
      if (n_fits == cap_fits) {
        const uint32_t cap = cap_fits ? cap_fits * 2 : 16u;
        wz3__f3d_fit *grown = (wz3__f3d_fit *)WZ3_MALLOC((size_t)cap * sizeof *grown);
        if (!grown) { *res = WZ3_ERR_OUT_OF_MEMORY; goto fail; }
        if (n_fits) memcpy(grown, fits, (size_t)n_fits * sizeof *grown);
        WZ3_FREE(fits);
        fits = grown; cap_fits = cap;
      }
      fits[n_fits].first_vertex = first_vertex; fits[n_fits].end_vertex = m->n_verts; fits[n_fits].offset_x = x;
      fits[n_fits].glyph = glyph; fits[n_fits].units = units; fits[n_fits].ppem = ppem;
      n_fits++;
    }
    if (gdi_sized) {
      const double advance26_6 = wz3__f3d_round_fixed((double)glyph->advance_units * ppem * 64 / (double)family->units_per_em);
      x += ceil(advance26_6 / 64) / 128.0;
    } else {
      x += floor((double)glyph->advance_units * (double)logical_height / (double)family->units_per_cell + 0.5) / 128.0;
    }
  }
  adjacent = (int32_t *)WZ3_MALLOC((size_t)(m->n_faces ? m->n_faces : 1) * 3 * sizeof(int32_t));
  if (!adjacent) { *res = WZ3_ERR_OUT_OF_MEMORY; goto fail; }
  {
    const int cleaned = wz3__f3d_clean(m, adjacent);
    if (cleaned) { *res = cleaned == -1 ? WZ3_ERR_OUT_OF_MEMORY : WZ3_ERR_OPERATOR; goto fail; }
  }
  for (uint32_t i = 0; i < n_fits; i++) if (wz3__f3d_fit_range(m, &fits[i])) { *res = WZ3_ERR_OPERATOR; goto fail; }
  if (wz3__f3d_extrude(m, adjacent, extrude)) { *res = WZ3_ERR_OUT_OF_MEMORY; goto fail; }
  for (uint32_t i = 0; i < m->n_verts; i++) {          /* projectFontUVs */
    m->verts[i].uv[0][0] = (float)((double)m->verts[i].position[0] + (double)m->verts[i].position[2]);
    m->verts[i].uv[0][1] = m->verts[i].position[1];
  }
  wz3__mm_invalidate(m);
  WZ3_FREE(adjacent); WZ3_FREE(fits); WZ3_FREE(tess);
  return m;
fail:
  WZ3_FREE(adjacent); WZ3_FREE(fits); WZ3_FREE(tess);
  wz3__mm_free(m);
  return NULL;
}

/* ---- summary hashes (the reference fixtures in tests/data) ---- */

static uint32_t wz3__mm_hash_word(uint32_t h, uint32_t w) { return (h ^ w) * 0x01000193u; }

static uint32_t wz3__mm_float_bits(float f)
{
  uint32_t u;
  if (f != f) return 0x7fc00000u;                 /* every NaN hashes as the canonical one */
  memcpy(&u, &f, 4);
  return u;
}

static uint32_t wz3__mm_hash_floats(uint32_t h, const float *v, uint32_t n)
{
  for (uint32_t i = 0; i < n; i++) h = wz3__mm_hash_word(h, wz3__mm_float_bits(v[i]));
  return h;
}

/* every vertex attribute, polygon, cluster and bone record */
static uint32_t wz3__mm_hash(const wz3_minmesh *m)
{
  uint32_t h = 0x811c9dc5u;
  h = wz3__mm_hash_word(h, m->n_verts); h = wz3__mm_hash_word(h, m->n_faces); h = wz3__mm_hash_word(h, m->n_clusters);
  h = wz3__mm_hash_word(h, m->n_bones);
  h = wz3__mm_hash_word(h, (uint32_t)(m->completely_rigid ? 1 : 0) | (uint32_t)(m->stripped ? 2 : 0));
  for (uint32_t i = 0; i < m->n_verts; i++) {
    const wz3__mm_vertex *v = &m->verts[i];
    h = wz3__mm_hash_word(h, (uint32_t)v->select); h = wz3__mm_hash_word(h, (uint32_t)v->bone_count);
    h = wz3__mm_hash_word(h, (uint32_t)v->temp_byte); h = wz3__mm_hash_word(h, (uint32_t)v->merge_tag);
    h = wz3__mm_hash_word(h, v->color);
    h = wz3__mm_hash_floats(h, v->position, 3); h = wz3__mm_hash_floats(h, v->normal, 3); h = wz3__mm_hash_floats(h, v->tangent, 3);
    h = wz3__mm_hash_floats(h, v->uv[0], 2); h = wz3__mm_hash_floats(h, v->uv[1], 2); h = wz3__mm_hash_floats(h, v->weights, 4);
    for (int k = 0; k < 4; k++) h = wz3__mm_hash_word(h, v->matrices[k]);
  }
  for (uint32_t i = 0; i < m->n_faces; i++) {
    const wz3__mm_face *f = &m->faces[i];
    h = wz3__mm_hash_word(h, (uint32_t)f->select); h = wz3__mm_hash_word(h, (uint32_t)f->count);
    h = wz3__mm_hash_word(h, (uint32_t)f->cluster); h = wz3__mm_hash_word(h, (uint32_t)f->temp);
    h = wz3__mm_hash_word(h, f->flags);
    h = wz3__mm_hash_floats(h, f->normal, 3);
    h = wz3__mm_hash_word(h, (uint32_t)f->count);
    const int32_t *c = wz3__mm_corners(m, f);
    for (int32_t k = 0; k < f->count; k++) h = wz3__mm_hash_word(h, (uint32_t)c[k]);
  }
  for (uint32_t i = 0; i < m->n_clusters; i++) {
    const wz3__mm_cluster *c = &m->clusters[i];
    h = wz3__mm_hash_word(h, (uint32_t)c->material); h = wz3__mm_hash_word(h, (uint32_t)c->render_pass);
    h = wz3__mm_hash_word(h, (uint32_t)c->id); h = wz3__mm_hash_word(h, (uint32_t)c->anim_type);
    h = wz3__mm_hash_word(h, (uint32_t)c->anim_matrix);
  }
  for (uint32_t i = 0; i < m->n_bones; i++) {
    const wz3__mm_bone *b = &m->bones[i];
    h = wz3__mm_hash_floats(h, b->base_pose, 16);
    h = wz3__mm_hash_word(h, (uint32_t)b->parent);
    h = wz3__mm_hash_word(h, wz3__mm_float_bits(b->factor)); h = wz3__mm_hash_word(h, wz3__mm_float_bits(b->spread));
    h = wz3__mm_hash_word(h, wz3__mm_float_bits(b->offset));
    h = wz3__mm_hash_floats(h, b->no_animation, 16);
    if (b->spline_kind == WZ3__MM_SPLINE_NONE) h = wz3__mm_hash_word(h, 0);
    else if (b->spline_kind == WZ3__MM_SPLINE_KINETIC) {
      h = wz3__mm_hash_word(h, 2);
      h = wz3__mm_hash_floats(h, b->kin_position, 3); h = wz3__mm_hash_floats(h, b->kin_speed, 3);
      h = wz3__mm_hash_floats(h, b->kin_gravity, 3); h = wz3__mm_hash_floats(h, b->kin_rotation, 3);
      h = wz3__mm_hash_word(h, b->kin_has_axis ? 1u : 0u);
      if (b->kin_has_axis) h = wz3__mm_hash_floats(h, b->kin_axis, 3);
      h = wz3__mm_hash_word(h, wz3__mm_float_bits(b->kin_angle));
    } else {
      h = wz3__mm_hash_word(h, 1); h = wz3__mm_hash_word(h, (uint32_t)b->spline_op);
    }
  }
  return h;
}

/* calcNormals on a copy: normals and tangents of every vertex, then the polygon normals */
WZ3_API uint32_t wz3_minmesh_normals_hash(const wz3_minmesh *m)
{
  wz3_minmesh *copy = wz3__mm_clone(m);
  if (!copy) return 0;
  copy->normals_valid = 0;
  wz3__mm_calc_normals(copy);
  uint32_t h = 0x811c9dc5u;
  for (uint32_t i = 0; i < copy->n_verts; i++) { h = wz3__mm_hash_floats(h, copy->verts[i].normal, 3); h = wz3__mm_hash_floats(h, copy->verts[i].tangent, 3); }
  for (uint32_t i = 0; i < copy->n_faces; i++) h = wz3__mm_hash_floats(h, copy->faces[i].normal, 3);
  wz3__mm_free(copy);
  return h;
}

WZ3_API int wz3_minmesh_summary(const wz3_minmesh *m, uint32_t *vertices, uint32_t *faces, uint32_t *clusters, uint32_t *bones, uint32_t *hash)
{
  if (!m) return -1;
  if (vertices) *vertices = m->n_verts;
  if (faces) *faces = m->n_faces;
  if (clusters) *clusters = m->n_clusters;
  if (bones) *bones = m->n_bones;
  if (hash) *hash = wz3__mm_hash(m);
  return 0;
}

