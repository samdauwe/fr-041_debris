static void wz3__rt_cache_release(wz3__rt_cache *cache)
{
  if (cache->owned) {
    if (cache->kind == WZ3__RC_BLOB) wz3__rt_blob_free((wz3__rt_blob *)cache->obj);
    else if (cache->kind == WZ3__RC_SHAKER) WZ3_FREE(cache->obj);
    else if (cache->kind == WZ3__RC_MESH) wz3__mesh_free((wz3_mesh *)cache->obj);
    else if (cache->kind == WZ3__RC_MINMESH) wz3__mm_free((wz3_minmesh *)cache->obj);
    else wz3__fj_cache_free(cache->kind, cache->obj);
  }
  memset(cache, 0, sizeof *cache);
}

/* ---- animation: bone matrices at a time, and skinning (MinMesh.prepare with a time) ---- */

/* evaluateCompactKinetic: the Explode shard spline (axis/angle or Euler rotation, then a ballistic translation) */
static void wz3__mm_kinetic_eval(const wz3__mm_bone *b, double time_in, float out[16])
{
  const float time = (float)wz3__mm_max(0, time_in);
  const double tau = (double)(float)(3.141592653589793 * 2.0);
  if (b->kin_has_axis) {
    float axis[3];
    wz3__mm_normalize3(b->kin_axis, axis);
    const float radians = (float)((double)(float)((double)b->kin_angle * (double)time) * tau);
    const double c = (float)wz3__cos(radians), s = (float)wz3__sin(radians), t = (float)(1 - c);
    const double x = axis[0], y = axis[1], z = axis[2];
#define WZ3__MM_R(v) ((float)(v))
    out[0] = (float)((double)WZ3__MM_R(WZ3__MM_R(t * x) * x) + c);
    out[1] = (float)((double)WZ3__MM_R(WZ3__MM_R(t * x) * y) + (double)WZ3__MM_R(s * z));
    out[2] = (float)((double)WZ3__MM_R(WZ3__MM_R(t * x) * z) - (double)WZ3__MM_R(s * y)); out[3] = 0;
    out[4] = (float)((double)WZ3__MM_R(WZ3__MM_R(t * x) * y) - (double)WZ3__MM_R(s * z));
    out[5] = (float)((double)WZ3__MM_R(WZ3__MM_R(t * y) * y) + c);
    out[6] = (float)((double)WZ3__MM_R(WZ3__MM_R(t * y) * z) + (double)WZ3__MM_R(s * x)); out[7] = 0;
    out[8] = (float)((double)WZ3__MM_R(WZ3__MM_R(t * x) * z) + (double)WZ3__MM_R(s * y));
    out[9] = (float)((double)WZ3__MM_R(WZ3__MM_R(t * y) * z) - (double)WZ3__MM_R(s * x));
    out[10] = (float)((double)WZ3__MM_R(WZ3__MM_R(t * z) * z) + c); out[11] = 0;
    out[12] = 0; out[13] = 0; out[14] = 0; out[15] = 1;
#undef WZ3__MM_R
  } else {
    const float a = (float)((double)(float)((double)b->kin_rotation[0] * tau) * (double)time);
    const float b2 = (float)((double)(float)((double)b->kin_rotation[1] * tau) * (double)time);
    const float c2 = (float)((double)(float)((double)b->kin_rotation[2] * tau) * (double)time);
    wz3_mat4_euler(a, b2, c2, out);
  }
  const float time2 = (float)((double)time * (double)time);
  for (int a = 0; a < 3; a++)
    out[12 + a] = (float)((double)(float)((double)b->kin_position[a] + (double)(float)((double)b->kin_speed[a] * (double)time)) +
                          (double)(float)((double)b->kin_gravity[a] * (double)time2));
}

/* evaluateCompactAnimation: `matrices` receives 16 floats per bone (parent chain, then the base pose) */
WZ3_API int wz3_rt_minmesh_pose(const wz3_rt *rt, const wz3_minmesh *m, double time, float *matrices)
{
  if (!rt || !m || !m->n_bones || !matrices) return -1;
  float *temporary = (float *)WZ3_MALLOC((size_t)m->n_bones * 16 * sizeof(float));
  if (!temporary) return -1;
  time = wz3__mm_max(0, time);
  const double scale = m->n_bones > 1 ? 1.0 / (double)(m->n_bones - 1) : 0.0;
  for (uint32_t index = 0; index < m->n_bones; index++) {
    const wz3__mm_bone *b = &m->bones[index];
    float local_matrix[16], value[16];
    const float *local = b->no_animation;
    const double spline_time = time * (double)b->factor + (double)b->offset + (double)index * (double)b->spread * scale;
    if (b->spline_kind == WZ3__MM_SPLINE_EXTERNAL) {
      const wz3__rt_cache *cache = (b->spline_op >= 0 && (uint32_t)b->spline_op < rt->n_ops) ? &rt->ops[b->spline_op].cache : NULL;
      if (wz3__rt_is_spline_cache(cache)) {
        float zoom;
        wz3__rt_spline_eval(rt, cache, spline_time, value, &zoom);
        local = value;
      }
    } else if (b->spline_kind == WZ3__MM_SPLINE_KINETIC) {
      wz3__mm_kinetic_eval(b, spline_time, local_matrix);
      local = local_matrix;
    }
    float *t = temporary + (size_t)index * 16;
    if (b->parent >= 0 && (uint32_t)b->parent < m->n_bones) wz3__mm_matrix_mul(temporary + (size_t)b->parent * 16, local, t);
    else memcpy(t, local, 16 * sizeof(float));
    wz3__mm_matrix_mul(t, b->base_pose, matrices + (size_t)index * 16);
  }
  WZ3_FREE(temporary);
  return 0;
}

/* skinCompactGeometry: normals and tangents must have been calculated; tangents get four floats per vertex */
WZ3_API int wz3_minmesh_skin(const wz3_minmesh *m, const float *matrices, float *positions, float *normals, float *tangents)
{
  if (!m || !positions || !normals || !tangents) return -1;
  for (uint32_t i = 0; i < m->n_verts; i++) {
    const wz3__mm_vertex *v = &m->verts[i];
    const double sx = v->position[0], sy = v->position[1], sz = v->position[2];
    const double snx = v->normal[0], sny = v->normal[1], snz = v->normal[2];
    const double stx = v->tangent[0], sty = v->tangent[1], stz = v->tangent[2];
    float *p = positions + (size_t)i * 3, *n = normals + (size_t)i * 3, *t = tangents + (size_t)i * 4;
    if (matrices && v->bone_count) {
      float px = 0, py = 0, pz = 0, nx = 0, ny = 0, nz = 0, tx = 0, ty = 0, tz = 0;
      for (int32_t bone = 0; bone < v->bone_count; bone++) {
        if (bone >= 4) continue;
        if (v->matrices[bone] >= m->n_bones) continue;
        const float *mt = matrices + (size_t)v->matrices[bone] * 16;
        const double weight = v->weights[bone];
        const float p0 = (float)((double)mt[0] * sx + (double)mt[4] * sy + (double)mt[8] * sz + (double)mt[12]);
        const float p1 = (float)((double)mt[1] * sx + (double)mt[5] * sy + (double)mt[9] * sz + (double)mt[13]);
        const float p2 = (float)((double)mt[2] * sx + (double)mt[6] * sy + (double)mt[10] * sz + (double)mt[14]);
        const float n0 = (float)((double)mt[0] * snx + (double)mt[4] * sny + (double)mt[8] * snz);
        const float n1 = (float)((double)mt[1] * snx + (double)mt[5] * sny + (double)mt[9] * snz);
        const float n2 = (float)((double)mt[2] * snx + (double)mt[6] * sny + (double)mt[10] * snz);
        const float t0 = (float)((double)mt[0] * stx + (double)mt[4] * sty + (double)mt[8] * stz);
        const float t1 = (float)((double)mt[1] * stx + (double)mt[5] * sty + (double)mt[9] * stz);
        const float t2 = (float)((double)mt[2] * stx + (double)mt[6] * sty + (double)mt[10] * stz);
        px = (float)((double)px + (double)p0 * weight); py = (float)((double)py + (double)p1 * weight); pz = (float)((double)pz + (double)p2 * weight);
        nx = (float)((double)nx + (double)n0 * weight); ny = (float)((double)ny + (double)n1 * weight); nz = (float)((double)nz + (double)n2 * weight);
        tx = (float)((double)tx + (double)t0 * weight); ty = (float)((double)ty + (double)t1 * weight); tz = (float)((double)tz + (double)t2 * weight);
      }
      p[0] = px; p[1] = py; p[2] = pz; n[0] = nx; n[1] = ny; n[2] = nz; t[0] = tx; t[1] = ty; t[2] = tz;
    } else {
      p[0] = v->position[0]; p[1] = v->position[1]; p[2] = v->position[2];
      n[0] = v->normal[0]; n[1] = v->normal[1]; n[2] = v->normal[2];
      t[0] = v->tangent[0]; t[1] = v->tangent[1]; t[2] = v->tangent[2];
    }
    t[3] = 1.0f;
  }
  return 0;
}

/* The per-frame geometry of an animated mesh at `time`, as MinMesh.prepare({time}) produces it: normals derived
   (on a copy), bones posed, vertices skinned. Hash of positions, normals and tangents. */
WZ3_API uint32_t wz3_rt_minmesh_frame_hash(const wz3_rt *rt, const wz3_minmesh *m, double time)
{
  wz3_minmesh *copy = wz3__mm_clone(m);
  float *matrices = (float *)WZ3_MALLOC((size_t)(m->n_bones ? m->n_bones : 1) * 16 * sizeof(float));
  float *geometry = (float *)WZ3_MALLOC((size_t)(m->n_verts ? m->n_verts : 1) * 10 * sizeof(float));
  uint32_t h = 0;
  if (copy && matrices && geometry && m->n_bones) {
    wz3__mm_calc_normals(copy);
    if (!wz3_rt_minmesh_pose(rt, copy, time, matrices) &&
        !wz3_minmesh_skin(copy, matrices, geometry, geometry + (size_t)m->n_verts * 3, geometry + (size_t)m->n_verts * 6)) {
      h = 0x811c9dc5u;
      h = wz3__mm_hash_floats(h, geometry, m->n_verts * 3);
      h = wz3__mm_hash_floats(h, geometry + (size_t)m->n_verts * 3, m->n_verts * 3);
      h = wz3__mm_hash_floats(h, geometry + (size_t)m->n_verts * 6, m->n_verts * 4);
    }
  }
  WZ3_FREE(geometry); WZ3_FREE(matrices); wz3__mm_free(copy);
  return h;
}

WZ3_API wz3_result wz3_rt_minmesh_pose_geometry(const wz3_rt *rt, const wz3_minmesh *mesh, const wz3_geometry *base, double time, wz3_geometry *out)
{
  if (!rt || !mesh || !base || !out || out == base || !mesh->n_bones || base->n_vertices != mesh->n_verts || base->animated != 1) return WZ3_ERR_ARGUMENT;
  wz3_minmesh *owned = NULL;
  const wz3_minmesh *source = mesh;
  if (!mesh->normals_valid) {
    owned = wz3__mm_clone(mesh);
    if (!owned) return WZ3_ERR_OUT_OF_MEMORY;
    wz3__mm_calc_normals(owned);
    source = owned;
  }
  const size_t nv = mesh->n_verts;
  const size_t streams = nv * 10 * sizeof(float);
  const size_t groups_offset = (streams + 15u) & ~(size_t)15u;
  const size_t total = groups_offset + (size_t)base->n_groups * sizeof(wz3_geometry_group);
  uint8_t *block = (uint8_t *)WZ3_MALLOC(total ? total : 16);
  float *matrices = (float *)WZ3_MALLOC((size_t)mesh->n_bones * 16 * sizeof(float));
  if (!block || !matrices) { WZ3_FREE(block); WZ3_FREE(matrices); wz3__mm_free(owned); return WZ3_ERR_OUT_OF_MEMORY; }
  float *positions = (float *)block, *normals = positions + nv * 3, *tangents = positions + nv * 6;
  const int bad = wz3_rt_minmesh_pose(rt, source, time, matrices) || wz3_minmesh_skin(source, matrices, positions, normals, tangents);
  WZ3_FREE(matrices);
  wz3__mm_free(owned);
  if (bad) { WZ3_FREE(block); return WZ3_ERR_OUT_OF_MEMORY; }   /* the arguments are valid: the pose's scratch matrices failed */
  *out = *base;
  out->positions = positions; out->normals = normals; out->tangents = tangents;
  out->block_ = block; out->block_bytes_ = total ? total : 16;
  out->groups = (wz3_geometry_group *)(block + groups_offset);
  if (base->n_groups) memcpy(out->groups, base->groups, (size_t)base->n_groups * sizeof(wz3_geometry_group));
  /* normalizeGeometryBounds(null, positions): the box of the finite positions, zero when there are none */
  float mn[3] = {INFINITY, INFINITY, INFINITY}, mx[3] = {-INFINITY, -INFINITY, -INFINITY};
  for (size_t i = 0; i < nv; i++) {
    const float *p = positions + i * 3;
    if (!wz3__finite((double)p[0]) || !wz3__finite((double)p[1]) || !wz3__finite((double)p[2])) continue;
    for (int axis = 0; axis < 3; axis++) {
      if (p[axis] < mn[axis]) mn[axis] = p[axis];
      if (p[axis] > mx[axis]) mx[axis] = p[axis];
    }
  }
  if (!wz3__finite((double)mn[0])) { memset(mn, 0, sizeof mn); memset(mx, 0, sizeof mx); }
  memcpy(out->bounds_min, mn, sizeof mn); memcpy(out->bounds_max, mx, sizeof mx);
  out->animated = 1;
  for (uint32_t g = 0; g < out->n_groups; g++) {
    out->groups[g].has_bounds = out->n_groups == 1;
    memset(out->groups[g].bounds_min, 0, sizeof out->groups[g].bounds_min);
    memset(out->groups[g].bounds_max, 0, sizeof out->groups[g].bounds_max);
    if (out->n_groups == 1) { memcpy(out->groups[g].bounds_min, mn, sizeof mn); memcpy(out->groups[g].bounds_max, mx, sizeof mx); }
  }
  return WZ3_OK;
}

/* initMiscShaker: params are the operator's animated parameter values 0..16 */
static wz3__rt_shaker *wz3__rt_shaker_make(uint32_t op_id, const wz3__rt_cache *parent, const wz3__rt_cache *amplitude,
                                           const double params[17])
{
  wz3__rt_shaker *sh = (wz3__rt_shaker *)WZ3_MALLOC(sizeof *sh);
  if (!sh) return NULL;
  memset(sh, 0, sizeof *sh);
  sh->op = op_id;
  if (wz3__rt_is_spline_cache(parent)) { sh->parent = *parent; sh->parent.owned = 0; }
  if (wz3__rt_is_spline_cache(amplitude)) { sh->amplitude = *amplitude; sh->amplitude.owned = 0; }
  sh->mode = (uint32_t)wz3__rt_to_int32(params[0]);
  for (uint32_t i = 0; i < 3; i++) {
    sh->translate_amp[i] = (float)params[1 + i];
    sh->rotate_amp[i] = (float)params[4 + i];
    sh->translate_freq[i] = (float)params[7 + i];
    sh->rotate_freq[i] = (float)params[10 + i];
    sh->center[i] = (float)params[14 + i];
  }
  return sh;
}

/* initMiscSplineScale: a scaled copy of a blob spline */
static wz3__rt_blob *wz3__rt_blob_scaled(const wz3__rt_blob *from, double px, double py, double pz, int32_t bits)
{
  wz3__rt_blob *blob = (wz3__rt_blob *)WZ3_MALLOC(sizeof *blob);
  if (!blob) return NULL;
  *blob = *from;
  blob->keys = (wz3__rt_blob_key *)WZ3_MALLOC((size_t)from->n_keys * sizeof(wz3__rt_blob_key));
  blob->pipe_keys = from->n_pipe_keys ? (wz3__rt_pipe_key *)WZ3_MALLOC((size_t)from->n_pipe_keys * sizeof(wz3__rt_pipe_key)) : NULL;
  if (!blob->keys || (from->n_pipe_keys && !blob->pipe_keys)) { WZ3_FREE(blob->keys); WZ3_FREE(blob->pipe_keys); WZ3_FREE(blob); return NULL; }
  memcpy(blob->keys, from->keys, (size_t)from->n_keys * sizeof(wz3__rt_blob_key));
  if (from->n_pipe_keys) memcpy(blob->pipe_keys, from->pipe_keys, (size_t)from->n_pipe_keys * sizeof(wz3__rt_pipe_key));
  for (uint32_t i = 0; i < blob->n_keys; i++) {
    wz3__rt_blob_key *key = &blob->keys[i];
    key->f[0] = (float)((double)key->f[0] * px);
    key->f[1] = (float)((double)key->f[1] * py);
    key->f[2] = (float)((double)key->f[2] * pz);
    if (bits & 1) key->f[3] = 0.0f;
    if (bits & 2) key->f[4] = 0.0f;
    if (bits & 4) key->f[5] = 0.0f;
  }
  return blob;
}

/* ---- mesh operators (the Mesh_* handlers of genmesh.cpp) ---- */

WZ3_API int wz3_mesh_summary(const wz3_mesh *m, uint32_t *vertices, uint32_t *edges, uint32_t *faces, uint32_t *hash)
{
  if (!m) return -1;
  uint32_t h;
  if (wz3__mesh_summary(m, &h)) return -1;
  if (vertices) *vertices = m->n_verts;
  if (edges) *edges = m->n_edges;
  if (faces) *faces = m->n_faces;
  if (hash) *hash = h;
  return 0;
}

static void wz3__mesh_random_srt(wz3_rng *r, const double src[9], float out[16])
{
  float v[9];
  for (int i = 0; i < 3; i++) v[i] = (float)((src[i] - 1.0) * (double)wz3_rng_float(r, 1.0f) + 1.0);
  for (int i = 3; i < 9; i++) v[i] = (float)(src[i] * ((double)wz3_rng_float(r, 1.0f) - 0.5) * 2.0);
  wz3_mat4_srt(v, out);
}

static void wz3__srt_matrix(const double srt[9], float out[16])
{
  float f[9];
  for (int i = 0; i < 9; i++) f[i] = (float)srt[i];
  wz3_mat4_srt(f, out);
}

/* transformXYZ(matrix, value[0..3]) -> float[4] */
static void wz3__txyz(const float m[16], const float *v, float out[4])
{
  const double x = v[0], y = v[1], z = v[2], w = v[3];
  float t[4];
  for (int i = 0; i < 4; i++) t[i] = (float)((double)m[i] * x + (double)m[4 + i] * y + (double)m[8 + i] * z + (double)m[12 + i] * w);
  memcpy(out, t, sizeof t);
}

static double wz3__clamp01(double v) { return v < 0 ? 0 : v > 1 ? 1 : v; }

/* GenMesh::Crease(0,...) for the faces that were just selected: split wedges where the new boundary meets an old crease */
static int wz3__mesh_crease_selected_boundaries(wz3_mesh *m, int32_t feature)
{
  const int32_t halfedge_count = (int32_t)m->n_edges * 2;
  for (int32_t h = 0; h < halfedge_count; h++) {
    wz3__medge *edge = &m->edges[h >> 1];
    const wz3__mface *face = &m->faces[edge->face[h & 1]], *opposite = &m->faces[edge->face[(h & 1) ^ 1]];
    if (!face->select || opposite->select) continue;
    if (!edge->crease) {
      for (int endpoint = 0; endpoint < 2; endpoint++) {
        int32_t wedge = h ^ endpoint;
        const int32_t source_id = wz3__mh_vert(m, wedge);
        const int32_t first = m->verts[source_id].first;
        int32_t existing = 0, around = wedge, guard = 0;
        do {
          existing |= m->edges[around >> 1].crease;
          around = wz3__mh_next_vert(m, around);
          if (++guard > halfedge_count + 1) return -1;
        } while (around != wedge);
        if (existing) {
          wz3__mvert *copy;
          const wz3__mvert source = m->verts[source_id];
          if (wz3__mesh_push_vertex(m, &copy)) return -1;
          *copy = source;
          const int32_t copy_id = (int32_t)m->n_verts - 1;
          copy->re_index = copy_id;
          copy->first = first;
          m->verts[source_id].next = copy_id;
          guard = 0;
          do {
            m->edges[wedge >> 1].vert[wedge & 1] = copy_id;
            wedge = wz3__mh_next_vert(m, wedge);
            if (++guard > halfedge_count + 1) return -1;
          } while (!m->edges[wedge >> 1].crease);
        }
      }
    }
    edge->crease |= feature;
  }
  return 0;
}

/* faceNormal(): the first corner's cross product (unnormalised, float32) */
static void wz3__mesh_face_normal(const wz3_mesh *m, const wz3__mface *face, float out[4])
{
  int32_t ids[3];
  uint32_t n = 0;
  int32_t last = -1;
  if (face->edge >= 0) {
    int32_t h = face->edge;
    do {
      const int32_t v = wz3__mh_vert(m, h);
      if (n < 2) ids[n] = v;
      last = v;
      n++;
      h = wz3__mh_next_face(m, h);
    } while (h != face->edge && n <= m->n_edges * 2u + 1u);
  }
  if (n < 3) { out[0] = 1; out[1] = out[2] = out[3] = 0; return; }
  const float *a = m->verts[last].v[0], *b = m->verts[ids[0]].v[0], *c = m->verts[ids[1]].v[0];
  const double ux = (double)b[0] - a[0], uy = (double)b[1] - a[1], uz = (double)b[2] - a[2];
  const double vx = (double)c[0] - a[0], vy = (double)c[1] - a[1], vz = (double)c[2] - a[2];
  out[0] = (float)(uy * vz - uz * vy);
  out[1] = (float)(uz * vx - ux * vz);
  out[2] = (float)(ux * vy - uy * vx);
  out[3] = 0.0f;
}

/* Mesh_UVProjection for the box (0) and shifted box (3) projections; the cylinder/sphere types need atan2 */
static wz3_mesh *wz3__mesh_op_uv_projection(const wz3_mesh *in, int32_t mask, const double srt[9], int32_t type)
{
  wz3_mesh *r = wz3__mesh_checked_copy(in, 0);
  if (!r) return NULL;
  float matrix[16];
  wz3__srt_matrix(srt, matrix);
  const int real = type == 0;
  for (int axis_code = 0; axis_code < 6; axis_code++) {
    const double sign = real ? ((axis_code & 1) ? 1.0 : -1.0) : 1.0;
    wz3__mesh_all_to_selection(r, 0, WZ3__ALL_FACE | WZ3__ALL_VERT);
    for (uint32_t f = 0; f < r->n_faces; f++) {
      if (mask && !(r->faces[f].mask & (uint32_t)mask)) continue;
      float normal[4];
      wz3__mesh_face_normal(r, &r->faces[f], normal);
      double maximum = normal[0];
      int axis = 0;
      if (fabs((double)normal[1]) > fabs(maximum)) { maximum = normal[1]; axis = 2; }
      if (fabs((double)normal[2]) > fabs(maximum)) { maximum = normal[2]; axis = 4; }
      if (maximum > 0) axis++;
      r->faces[f].select = axis == axis_code;
    }
    if (wz3__mesh_crease_selected_boundaries(r, WZ3__MF_UV0)) { wz3__mesh_free(r); return NULL; }
    wz3__mesh_face_to_vertex(r);
    for (uint32_t i = 0; i < r->n_verts; i++) {
      wz3__mvert *v = &r->verts[i];
      if (!v->select) continue;
      float point[4];
      wz3__txyz(matrix, v->v[0], point);
      const int plane = axis_code >> 1;
      if (plane == 0) { v->v[4][0] = (float)((double)point[2] * sign); v->v[4][1] = (float)(-(double)point[1]); }
      else if (plane == 1) { v->v[4][0] = point[0]; v->v[4][1] = (float)(-(double)point[2] * sign); }
      else { v->v[4][0] = (float)(-(double)point[0] * sign); v->v[4][1] = (float)(-(double)point[1]); }
      v->v[4][2] = 0.0f;
      v->v[4][3] = v->v[0][3];
    }
  }
  return r;
}

static wz3_mesh *wz3__mesh_op_bend(const wz3_mesh *in, int32_t mask, const double srt1[9], const double srt2[9],
                                   const double direction[2], const double y_range[2], int32_t mode)
{
  wz3_mesh *r = wz3__mesh_checked_copy(in, (uint32_t)mask << 16);
  if (!r) return NULL;
  float base[16], matrix1[16], matrix2[16], measuring[16];
  wz3__srt_matrix(srt1, base);
  wz3__mesh_pivot_transform(r, base, WZ3__MA_POS, matrix1);
  wz3__srt_matrix(srt2, base);
  wz3__mesh_pivot_transform(r, base, WZ3__MA_POS, matrix2);
  wz3_mat4_euler((float)(direction[0] * WZ3__TAU), 0.0f, (float)(direction[1] * WZ3__TAU), measuring);
  double y_min, y_max;
  if (mode & 2) {
    y_min = 1e30; y_max = -1e30;
    for (uint32_t i = 0; i < r->n_verts; i++) {
      if (!r->verts[i].select) continue;
      const float *p = r->verts[i].v[0];
      const double value = (double)p[0] * measuring[1] + (double)p[1] * measuring[5] + (double)p[2] * measuring[9];
      if (value < y_min) y_min = value;
      if (value > y_max) y_max = value;
    }
  } else {
    y_min = y_range[0];
    y_max = y_range[1];
  }
  const double y_scale = 1.0 / (y_max - y_min);
  for (uint32_t i = 0; i < r->n_verts; i++) {
    wz3__mvert *v = &r->verts[i];
    if (!v->select) continue;
    float *p = v->v[0];
    double t = ((double)p[0] * measuring[1] + (double)p[1] * measuring[5] + (double)p[2] * measuring[9] - y_min) * y_scale;
    t = wz3__clamp01(t);
    switch (mode & 5) {
    case 1: t = t * t * (3 - 2 * t); break;
    case 4: t = 1 - fabs(t - 0.5) * 2; break;
    case 5: t = t * t * (16 + t * (16 * t - 32)); break;
    default: break;
    }
    float p1[4], p2[4];
    wz3__txyz(matrix1, p, p1);
    wz3__txyz(matrix2, p, p2);
    p[0] = (float)((double)p1[0] + ((double)p2[0] - p1[0]) * t);
    p[1] = (float)((double)p1[1] + ((double)p2[1] - p1[1]) * t);
    p[2] = (float)((double)p1[2] + ((double)p2[2] - p1[2]) * t);
  }
  return r;
}

static wz3_mesh *wz3__mesh_op_bend2(const wz3_mesh *in, const double center[3], const double rotate[3], double length, double angle_in)
{
  wz3_mesh *r = wz3__mesh_checked_copy(in, 0);
  if (!r) return NULL;
  float top[16], bottom[16];
  const float rot[3] = {(float)rotate[0], (float)rotate[1], (float)rotate[2]};
  wz3_mat4_euler_turns(rot, top);
  top[12] = (float)(-center[0]); top[13] = (float)(-center[1]); top[14] = (float)(-center[2]);
  wz3_mat4_identity(bottom);                                  /* rigidInverse(top) */
  bottom[0] = top[0]; bottom[1] = top[4]; bottom[2] = top[8];
  bottom[4] = top[1]; bottom[5] = top[5]; bottom[6] = top[9];
  bottom[8] = top[2]; bottom[9] = top[6]; bottom[10] = top[10];
  const double tx = top[12], ty = top[13], tz = top[14];
  bottom[12] = (float)(-((double)bottom[0] * tx + (double)bottom[4] * ty + (double)bottom[8] * tz));
  bottom[13] = (float)(-((double)bottom[1] * tx + (double)bottom[5] * ty + (double)bottom[9] * tz));
  bottom[14] = (float)(-((double)bottom[2] * tx + (double)bottom[6] * ty + (double)bottom[10] * tz));
  const double angle = (float)(angle_in * WZ3__TAU);
  for (uint32_t i = 0; i < r->n_verts; i++) {
    float local[4];
    wz3__txyz(top, r->verts[i].v[0], local);
    double t = local[1];
    if (t >= 0) local[1] = (float)((double)local[1] - (t < length ? t : length));
    t = (float)(wz3__clamp01((float)(t / length)) * angle);
    const double sine = (float)wz3__sin(t), cosine = (float)wz3__cos(t);
    const double x = local[0], y = local[1];
    local[0] = (float)(cosine * x - sine * y);
    local[1] = (float)(sine * x + cosine * y);
    wz3__txyz(bottom, local, r->verts[i].v[0]);
  }
  r->got_normals = 0;
  return r;
}

static uint8_t wz3__sel_logic(uint8_t record_mask, uint32_t source1, uint32_t source2, uint32_t destination, int32_t mode)
{
  int s1 = (record_mask & source1) != 0, s2 = (record_mask & source2) != 0;
  if (mode & 4) s1 = !s1;
  if (mode & 8) s2 = !s2;
  switch (mode & 3) {
  case 0: s1 = s1 || s2; break;
  case 1: s1 = s1 && s2; break;
  case 2: s1 = s1 != s2; break;
  default: break;
  }
  if (mode & 16) s1 = !s1;
  return (uint8_t)((s1 ? (record_mask | destination) : (record_mask & ~destination)) & 255u);
}

static wz3_mesh *wz3__mesh_op_select_logic(const wz3_mesh *in, uint32_t s1, uint32_t s2, uint32_t dst, int32_t mode)
{
  wz3_mesh *r = wz3__mesh_checked_copy(in, 0);
  if (!r) return NULL;
  const uint32_t sources = s1 | s2;
  mode &= 31 | 32;
  if ((sources & 255u) && (dst & 255u))
    for (uint32_t i = 0; i < r->n_edges; i++) r->edges[i].mask = wz3__sel_logic(r->edges[i].mask, s1 & 255u, s2 & 255u, dst & 255u, mode & 31);
  if ((sources & 0xff00u) && (dst & 0xff00u))
    for (uint32_t i = 0; i < r->n_faces; i++)
      r->faces[i].mask = wz3__sel_logic(r->faces[i].mask, (s1 >> 8) & 255u, (s2 >> 8) & 255u, (dst >> 8) & 255u, mode & 31);
  if ((sources & 0xff0000u) && (dst & 0xff0000u))
    for (uint32_t i = 0; i < r->n_verts; i++)
      r->verts[i].mask = wz3__sel_logic(r->verts[i].mask, (s1 >> 16) & 255u, (s2 >> 16) & 255u, (dst >> 16) & 255u, mode & 31);
  if ((dst & 0xff0000u) && (dst & 0xff00u) && (mode & 32)) {
    wz3__mesh_mask_to_selection(r, dst & 0xff00u);
    wz3__mesh_face_to_vertex(r);
    wz3__mesh_selection_to_mask(r, dst & 0xff0000u, WZ3__SEL_SET);
  }
  return r;
}

/* ---- Subdivide (Catmull-Clark style, genmesh.cpp) ---- */

typedef struct wz3__ivec { int32_t *p; uint32_t n, cap; } wz3__ivec;

static int wz3__ivec_push(wz3__ivec *v, int32_t x, int unique)
{
  if (unique) for (uint32_t i = 0; i < v->n; i++) if (v->p[i] == x) return 0;
  if (v->n == v->cap) {
    const uint32_t cap = v->cap ? v->cap * 2 : 4;
    int32_t *p = (int32_t *)WZ3_MALLOC((size_t)cap * sizeof(int32_t));
    if (!p) return -1;
    if (v->n) memcpy(p, v->p, (size_t)v->n * sizeof(int32_t));
    WZ3_FREE(v->p);
    v->p = p;
    v->cap = cap;
  }
  v->p[v->n++] = x;
  return 0;
}

typedef struct wz3__subterm { const float (*values)[4]; double weight; } wz3__subterm;

/* weightedValues(): every attribute vector of `dst` as the weighted sum of the terms (accumulated in double) */
static void wz3__sub_weighted(wz3__mvert *dst, const wz3__subterm *terms, uint32_t n)
{
  for (int a = 0; a < 6; a++)
    for (int c = 0; c < 4; c++) {
      double value = 0.0;
      for (uint32_t t = 0; t < n; t++) value += (double)terms[t].values[a][c] * terms[t].weight;
      dst->v[a][c] = (float)value;
    }
}

static const int32_t wz3__attribute_features[6] = {WZ3__MF_POS, WZ3__MF_NORMAL, WZ3__MF_TANGENT, WZ3__MF_COLOR0, WZ3__MF_UV0, WZ3__MF_UV1};

/* stableSubdivisionFanOrigin(): rotate an unselected polygon's loop so its fan from vertex 0 keeps the winding */
static int wz3__sub_stable_origin(const wz3_mesh *m, const int32_t *ids, uint32_t n, int32_t *out)
{
  memcpy(out, ids, (size_t)n * sizeof(int32_t));
  if (n <= 3) return 0;
  const float *pos_of[256];
  if (n > 256) return 0;
  for (uint32_t i = 0; i < n; i++) {
    const wz3__mvert *v = &m->verts[ids[i]];
    pos_of[i] = (v->first >= 0 && (uint32_t)v->first < m->n_verts) ? m->verts[v->first].v[0] : v->v[0];
  }
  double normal[3] = {0, 0, 0};
  for (uint32_t i = 0; i < n; i++) {
    const float *cur = pos_of[i], *nxt = pos_of[(i + 1) % n];
    normal[0] += ((double)cur[1] - nxt[1]) * ((double)cur[2] + nxt[2]);
    normal[1] += ((double)cur[2] - nxt[2]) * ((double)cur[0] + nxt[0]);
    normal[2] += ((double)cur[0] - nxt[0]) * ((double)cur[1] + nxt[1]);
  }
  if (normal[0] * normal[0] + normal[1] * normal[1] + normal[2] * normal[2] <= 1e-20) return 0;
  for (uint32_t offset = 0; offset < n; offset++) {
    const float *first = pos_of[offset];
    int valid = 1;
    for (uint32_t index = 2; index < n && valid; index++) {
      const float *second = pos_of[(offset + index - 1) % n], *third = pos_of[(offset + index) % n];
      const double ax = (double)second[0] - first[0], ay = (double)second[1] - first[1], az = (double)second[2] - first[2];
      const double bx = (double)third[0] - first[0], by = (double)third[1] - first[1], bz = (double)third[2] - first[2];
      const double cx = ay * bz - az * by, cy = az * bx - ax * bz, cz = ax * by - ay * bx;
      if (cx * cx + cy * cy + cz * cz <= 1e-20 || cx * normal[0] + cy * normal[1] + cz * normal[2] <= 0) valid = 0;
    }
    if (valid) {
      for (uint32_t i = 0; i < n; i++) out[i] = ids[(offset + i) % n];
      return 0;
    }
  }
  return 0;
}

static wz3_mesh *wz3__mesh_subdivide_once(const wz3_mesh *s, double alpha)
{
  const uint32_t nv = s->n_verts, ne = s->n_edges, nf = s->n_faces;
  int ok = 1;
  wz3_mesh *out = wz3__mesh_clone(s);
  if (!out) return NULL;
  /* the output starts from the source vertices and materials only */
  out->n_edges = 0;
  out->n_faces = 0;
  out->got_normals = 0;

  wz3__ivec *inc_faces = (wz3__ivec *)WZ3_MALLOC(((size_t)nv + 1) * sizeof(wz3__ivec) * 4);
  int32_t *vert_temp = (int32_t *)WZ3_MALLOC(((size_t)nv + 1) * sizeof(int32_t) * 4);
  int32_t *face_center = (int32_t *)WZ3_MALLOC(((size_t)nf + 1) * sizeof(int32_t));
  int32_t *edge_pts = (int32_t *)WZ3_MALLOC(((size_t)ne + 1) * 2 * sizeof(int32_t));
  if (!inc_faces || !vert_temp || !face_center || !edge_pts) {
    WZ3_FREE(inc_faces); WZ3_FREE(vert_temp); WZ3_FREE(face_center); WZ3_FREE(edge_pts);
    wz3__mesh_free(out);
    return NULL;
  }
  wz3__ivec *neighbors = inc_faces + nv, *phys_faces = inc_faces + 2 * (size_t)nv, *phys_neighbors = inc_faces + 3 * (size_t)nv,
            *groups = NULL;
  memset(inc_faces, 0, (size_t)nv * sizeof(wz3__ivec) * 4);
  int32_t *boundary = vert_temp + nv, *incident_creases = vert_temp + 2 * (size_t)nv;       /* boundary: 2 per vertex */
  int32_t *boundary_pair = (int32_t *)WZ3_MALLOC(((size_t)nv + 1) * 2 * sizeof(int32_t));
  groups = (wz3__ivec *)WZ3_MALLOC(((size_t)nv + 1) * sizeof(wz3__ivec));
  if (!boundary_pair || !groups) { ok = 0; }
  if (ok) {
    memset(groups, 0, ((size_t)nv + 1) * sizeof(wz3__ivec));
    for (uint32_t i = 0; i < nv; i++) { vert_temp[i] = -1; boundary[i] = 0; incident_creases[i] = 0; boundary_pair[2 * i] = boundary_pair[2 * i + 1] = -1; }
    for (uint32_t i = 0; i < nf; i++) face_center[i] = -1;
    for (uint32_t i = 0; i < ne * 2; i++) edge_pts[i] = -1;
  }
  /* face loops are walked on demand; incident faces first */
  for (uint32_t f = 0; ok && f < nf; f++) {
    if (!s->faces[f].select || s->faces[f].edge < 0) continue;
    const int32_t start = s->faces[f].edge;
    int32_t h = start;
    do {
      const int32_t v = wz3__mh_vert(s, h);
      if (wz3__ivec_push(&inc_faces[v], (int32_t)f, 0)) ok = 0;
      if (vert_temp[v] < 0) vert_temp[v] = h;
      const int32_t first = s->verts[v].first;
      if (first >= 0 && (uint32_t)first < nv && wz3__ivec_push(&phys_faces[first], (int32_t)f, 1)) ok = 0;
      h = wz3__mh_next_face(s, h);
    } while (ok && h != start);
  }
  for (uint32_t e = 0; ok && e < ne; e++) {
    const wz3__medge *edge = &s->edges[e];
    const int32_t a = edge->vert[0], b = edge->vert[1];
    if (a < 0 || b < 0) continue;
    incident_creases[a] |= edge->crease;
    incident_creases[b] |= edge->crease;
    const int sel0 = s->faces[edge->face[0]].select, sel1 = s->faces[edge->face[1]].select;
    if (!(sel0 || sel1)) continue;
    if (wz3__ivec_push(&neighbors[a], b, 1) || wz3__ivec_push(&neighbors[b], a, 1)) ok = 0;
    const int32_t fa = s->verts[a].first, fb = s->verts[b].first;
    if (fa != fb && fa >= 0 && fb >= 0 && (uint32_t)fa < nv && (uint32_t)fb < nv) {
      if (wz3__ivec_push(&phys_neighbors[fa], fb, 1) || wz3__ivec_push(&phys_neighbors[fb], fa, 1)) ok = 0;
    }
  }
  for (uint32_t i = 0; ok && i < nv; i++) {
    const int32_t first = s->verts[i].first;
    if (first >= 0 && (uint32_t)first < nv && wz3__ivec_push(&groups[first], (int32_t)i, 0)) ok = 0;
  }
  /* boundary pairs: walk the one-ring from the first selected-face half-edge */
  const int64_t ring_limit = (int64_t)ne * 2 + 1;
  for (uint32_t v = 0; ok && v < nv; v++) {
    const int32_t start = vert_temp[v];
    if (start < 0) continue;
    int32_t edge = start, last = -1;
    int64_t guard = 0;
    do {
      if (!s->faces[wz3__mh_face(s, edge)].select) last = edge;
      edge = wz3__mh_next_vert(s, edge);
      if (++guard > ring_limit) { last = -1; break; }
    } while (edge != start);
    if (last < 0) continue;
    boundary_pair[2 * v] = wz3__mh_vert(s, last ^ 1);
    boundary_pair[2 * v + 1] = wz3__mh_vert(s, wz3__mh_next_vert(s, last) ^ 1);
  }

  /* face centers */
  for (uint32_t f = 0; ok && f < nf; f++) {
    const wz3__mface *face = &s->faces[f];
    if (!face->select || face->edge < 0) continue;
    wz3__subterm terms[64];
    uint32_t n = 0;
    int32_t h = face->edge;
    do {
      if (n >= 64) { ok = 0; break; }
      terms[n].values = (const float (*)[4])s->verts[wz3__mh_vert(s, h)].v;
      n++;
      h = wz3__mh_next_face(s, h);
    } while (h != face->edge);
    if (!ok || !n) continue;
    const double weight = 1.0 / (double)n;
    for (uint32_t t = 0; t < n; t++) terms[t].weight = weight;
    wz3__mvert *center;
    if (wz3__mesh_push_vertex(out, &center)) { ok = 0; break; }
    center->mask = face->mask;
    center->select = face->select;
    wz3__sub_weighted(center, terms, n);
    const int32_t index = (int32_t)out->n_verts - 1;
    center->first = center->next = center->re_index = index;
    face_center[f] = index;
  }

  /* even vertices over the physical (First) position */
  float (*even_pos)[4] = (float (*)[4])WZ3_MALLOC(((size_t)nv + 1) * sizeof(float[4]));
  uint8_t *has_even = (uint8_t *)WZ3_MALLOC((size_t)nv + 1);
  if (!even_pos || !has_even) ok = 0;
  if (ok) memset(has_even, 0, (size_t)nv + 1);
  for (uint32_t first = 0; ok && first < nv; first++) {
    if (!groups[first].n || !phys_faces[first].n) continue;
    const float *original = s->verts[first].v[0];
    int32_t pair0 = boundary_pair[2 * first], pair1 = boundary_pair[2 * first + 1];
    if (pair0 < 0 && pair1 < 0)
      for (uint32_t g = 0; g < groups[first].n; g++) {
        const int32_t member = groups[first].p[g];
        if (boundary_pair[2 * member] >= 0 || boundary_pair[2 * member + 1] >= 0) { pair0 = boundary_pair[2 * member]; pair1 = boundary_pair[2 * member + 1]; break; }
      }
    const int has_pair = pair0 >= 0 || pair1 >= 0;
    if (has_pair) {
      const double w1 = alpha * 0.125;
      const float *a = s->verts[pair0].v[0], *b = s->verts[pair1].v[0];
      for (int c = 0; c < 4; c++) even_pos[first][c] = (float)((double)original[c] * (1 - 2 * w1) + ((double)a[c] + b[c]) * w1);
    } else {
      const double n = (double)phys_faces[first].n;
      const double ring_weight = alpha / (n * n);
      for (int c = 0; c < 4; c++) {
        double sum = (double)original[c] * (1 - alpha * 2 / n);
        for (uint32_t k = 0; k < phys_neighbors[first].n; k++) sum += (double)s->verts[phys_neighbors[first].p[k]].v[0][c] * ring_weight;
        for (uint32_t k = 0; k < phys_faces[first].n; k++) sum += (double)out->verts[face_center[phys_faces[first].p[k]]].v[0][c] * ring_weight;
        even_pos[first][c] = (float)sum;
      }
    }
    has_even[first] = 1;
  }
  for (uint32_t v = 0; ok && v < nv; v++) {
    if (!inc_faces[v].n) continue;
    const wz3__mvert *original = &s->verts[v];
    wz3__mvert *dst = &out->verts[v];
    wz3__subterm terms[256];
    uint32_t n = 0;
    if (boundary_pair[2 * v] >= 0 || boundary_pair[2 * v + 1] >= 0) {
      const double w1 = alpha * 0.125;
      terms[0].values = (const float (*)[4])original->v; terms[0].weight = 1 - 2 * w1;
      terms[1].values = (const float (*)[4])s->verts[boundary_pair[2 * v]].v; terms[1].weight = w1;
      terms[2].values = (const float (*)[4])s->verts[boundary_pair[2 * v + 1]].v; terms[2].weight = w1;
      wz3__sub_weighted(dst, terms, 3);
    } else {
      const double fn = (double)inc_faces[v].n;
      const double ring_weight = alpha / (fn * fn);
      terms[n].values = (const float (*)[4])original->v; terms[n].weight = 1 - alpha * 2 / fn; n++;
      for (uint32_t k = 0; k < neighbors[v].n && n < 256; k++) { terms[n].values = (const float (*)[4])s->verts[neighbors[v].p[k]].v; terms[n].weight = ring_weight; n++; }
      for (uint32_t k = 0; k < inc_faces[v].n && n < 256; k++) { terms[n].values = (const float (*)[4])out->verts[face_center[inc_faces[v].p[k]]].v; terms[n].weight = ring_weight; n++; }
      wz3__sub_weighted(dst, terms, n);
      for (int a = 0; a < 6; a++)
        if (incident_creases[v] & wz3__attribute_features[a]) memcpy(dst->v[a], original->v[a], sizeof dst->v[a]);
    }
  }
  for (uint32_t first = 0; ok && first < nv; first++) {
    if (!has_even[first]) continue;
    for (uint32_t g = 0; g < groups[first].n; g++) memcpy(out->verts[groups[first].p[g]].v[0], even_pos[first], sizeof(float[4]));
  }

  /* edge points */
  for (uint32_t e = 0; ok && e < ne; e++) {
    const wz3__medge *edge = &s->edges[e];
    const wz3__mface *f0 = &s->faces[edge->face[0]], *f1 = &s->faces[edge->face[1]];
    const int sel0 = f0->select, sel1 = f1->select;
    if (!(sel0 || sel1) || edge->vert[0] < 0 || edge->vert[1] < 0) continue;
    const int split = edge->crease != 0;
    int32_t points[2] = {-1, -1};
    for (int side = 0; side < (split ? 2 : 1) && ok; side++) {
      const int32_t h = (int32_t)e * 2 + side;
      const wz3__mvert *a = &s->verts[wz3__mh_vert(s, h)], *b = &s->verts[wz3__mh_vert(s, wz3__mh_next_face(s, h))];
      wz3__mvert *point;
      if (wz3__mesh_push_vertex(out, &point)) { ok = 0; break; }
      point->mask = (uint8_t)((a->mask | b->mask) & 255);
      point->select = 1;
      const int smooth = sel0 && sel1;
      const double w1 = alpha * 0.25, w2 = 0.5 - w1;
      for (int at = 0; at < 6; at++)
        for (int c = 0; c < 4; c++) {
          if (smooth && !(edge->crease & wz3__attribute_features[at])) {
            const float *c0 = out->verts[face_center[edge->face[0]]].v[at], *c1 = out->verts[face_center[edge->face[1]]].v[at];
            point->v[at][c] = (float)(((double)a->v[at][c] + b->v[at][c]) * w2 + ((double)c0[c] + c1[c]) * w1);
          } else {
            point->v[at][c] = (float)(((double)a->v[at][c] + b->v[at][c]) * 0.5);
          }
        }
      const int32_t index = (int32_t)out->n_verts - 1;
      point->first = side ? points[0] : index;
      point->next = point->re_index = index;
      points[side] = index;
    }
    if (!split) points[1] = points[0];
    edge_pts[2 * e] = points[0];
    edge_pts[2 * e + 1] = points[1];
  }

  /* polygons */
  wz3__pbuild pb;
  memset(&pb, 0, sizeof pb);
  int32_t ids[512], stable[512];
  const uint32_t source_vertices = nv;
  for (uint32_t f = 0; ok && f < nf; f++) {
    const wz3__mface *face = &s->faces[f];
    if (face->edge < 0) continue;
    int32_t loop[256];
    uint32_t n = 0;
    int32_t h = face->edge;
    do {
      if (n >= 256) { ok = 0; break; }
      loop[n++] = h;
      h = wz3__mh_next_face(s, h);
    } while (h != face->edge);
    if (!ok || n < 3) continue;
    uint32_t before = pb.n_polys;
    if (face->select) {
      const int32_t center = face_center[f];
      for (uint32_t i = 0; i < n; i++) {
        const int32_t current = wz3__mh_vert(s, loop[i]);
        const int32_t outgoing = edge_pts[2 * (loop[i] >> 1) + (loop[i] & 1)];
        const int32_t incoming_h = loop[(i + n - 1) % n];
        const int32_t incoming = edge_pts[2 * (incoming_h >> 1) + (incoming_h & 1)];
        const int32_t quad[4] = {current, outgoing, center, incoming};
        if (wz3__pbuild_push(&pb, quad, 4, face->mask, face->material)) { ok = 0; break; }
        wz3__poly *pl = &pb.polys[pb.n_polys - 1];
        pl->id = face->id; pl->select = 1; pl->used = face->used;
      }
    } else {
      uint32_t m = 0;
      int has_split = 0;
      for (uint32_t i = 0; i < n && m + 2 <= 512; i++) {
        ids[m++] = wz3__mh_vert(s, loop[i]);
        const int32_t point = edge_pts[2 * (loop[i] >> 1) + (loop[i] & 1)];
        if (point >= 0) { ids[m++] = point; has_split = 1; }
      }
      const int32_t *use = ids;
      if (has_split) {
        /* the stable origin needs the output vertices as created so far */
        wz3__sub_stable_origin(out, ids, m, stable);
        use = stable;
      }
      if (wz3__pbuild_push(&pb, use, m, face->mask, face->material)) ok = 0;
      else {
        wz3__poly *pl = &pb.polys[pb.n_polys - 1];
        pl->id = face->id; pl->select = 0; pl->used = face->used;
      }
    }
    (void)before;
  }
  if (ok) {
    if (wz3__mesh_link_copies(out) || wz3__mesh_set_polygons(out, &pb)) ok = 0;
  }
  wz3__pbuild_free(&pb);
  if (ok) {
    /* SplitBridge keeps the source edge's flags on both child halves */
    int32_t *point_source = (int32_t *)WZ3_MALLOC(((size_t)out->n_verts + 1) * sizeof(int32_t));
    if (!point_source) ok = 0;
    else {
      for (uint32_t i = 0; i < out->n_verts; i++) point_source[i] = -1;
      for (uint32_t e = 0; e < ne; e++)
        for (int k = 0; k < 2; k++) {
          const int32_t pv = edge_pts[2 * e + k];
          if (pv >= 0) point_source[pv] = (int32_t)e;
        }
      for (uint32_t i = 0; i < out->n_edges; i++) {
        wz3__medge *edge = &out->edges[i];
        int32_t source_edge = -1;
        for (int side = 0; side < 2; side++)
          if (edge->vert[side] >= 0 && point_source[edge->vert[side]] >= 0) source_edge = point_source[edge->vert[side]];
        if (source_edge < 0) continue;
        int other_is_old = 0;
        for (int side = 0; side < 2; side++)
          if (edge->vert[side] >= 0 && (uint32_t)edge->vert[side] < source_vertices && point_source[edge->vert[side]] < 0) other_is_old = 1;
        if (!other_is_old) continue;
        const wz3__medge *old = &s->edges[source_edge];
        edge->mask = old->mask; edge->id = old->id; edge->select = old->select; edge->used = old->used; edge->crease = old->crease;
      }
      WZ3_FREE(point_source);
    }
  }
  for (uint32_t i = 0; i < nv; i++) {
    WZ3_FREE(inc_faces[i].p); WZ3_FREE(inc_faces[nv + i].p); WZ3_FREE(inc_faces[2 * (size_t)nv + i].p);
    WZ3_FREE(inc_faces[3 * (size_t)nv + i].p);
    if (groups) WZ3_FREE(groups[i].p);
  }
  WZ3_FREE(inc_faces); WZ3_FREE(vert_temp); WZ3_FREE(face_center); WZ3_FREE(edge_pts);
  WZ3_FREE(boundary_pair); WZ3_FREE(groups); WZ3_FREE(even_pos); WZ3_FREE(has_even);
  if (!ok || out->oom) { wz3__mesh_free(out); return NULL; }
  out->got_normals = 0;
  return out;
}

static wz3_mesh *wz3__mesh_op_subdivide(const wz3_mesh *in, int32_t mask, double alpha_in, int32_t count)
{
  wz3_mesh *r = wz3__mesh_checked_copy(in, (uint32_t)mask << 8);
  if (!r) return NULL;
  const double alpha = (float)alpha_in;
  while (count-- > 0) {
    wz3_mesh *next = wz3__mesh_subdivide_once(r, alpha);
    wz3__mesh_free(r);
    r = next;
    if (!r) return NULL;
  }
  r->got_normals = 0;
  return r;
}

static wz3_mesh *wz3__mesh_op_transform(const wz3_mesh *in, int32_t mask, const double srt[9], int32_t src_attr,
                                        int32_t dst_attr, int ex)
{
  wz3_mesh *r = wz3__mesh_checked_copy(in, (uint32_t)mask << 16);
  if (!r) return NULL;
  float base[16], matrix[16];
  if (ex && (src_attr == WZ3__MA_NORMAL || src_attr == WZ3__MA_TANGENT) && wz3__mesh_need_normals(r)) { wz3__mesh_free(r); return NULL; }
  wz3__srt_matrix(srt, base);
  wz3__mesh_pivot_transform(r, base, ex ? src_attr : WZ3__MA_POS, matrix);
  wz3__mesh_transform_vertices(r, matrix, ex ? src_attr : WZ3__MA_POS, ex ? dst_attr : WZ3__MA_POS, 0, 0, 0);
  if (!ex || dst_attr == WZ3__MA_POS || dst_attr == WZ3__MA_NORMAL || dst_attr == WZ3__MA_TANGENT) r->got_normals = 0;
  return r;
}

static wz3_mesh *wz3__mesh_op_multiply(const wz3_mesh *input, const double srt[9], int32_t count, int32_t mode,
                                       double translate_u, double translate_v, const double local_rotate[3], double extrude)
{
  wz3_mesh *source = wz3__mesh_checked_copy(input, 0);
  if (!source) return NULL;
  float step[16], local_step[16], transform[16], local_transform[16], tmp[16], matrix[16];
  wz3__srt_matrix(srt, step);
  const float lr[3] = {(float)local_rotate[0], (float)local_rotate[1], (float)local_rotate[2]};
  wz3_mat4_euler_turns(lr, local_step);
  wz3_mat4_identity(transform);
  wz3_mat4_identity(local_transform);
  wz3_mesh *out = wz3__mesh_new(source->vertex_mask);
  wz3_rng random;
  wz3_rng_init(&random);
  wz3_rng_seed(&random, count);
  if (!out || (extrude != 0.0 && wz3__mesh_need_normals(source))) { wz3__mesh_free(out); wz3__mesh_free(source); return NULL; }
  uint32_t prev_start = 0, prev_end = 0;
  for (int32_t iteration = 0; iteration < count; iteration++) {
    const uint32_t start = out->n_verts;
    if (wz3__mesh_append(out, source)) { wz3__mesh_free(out); wz3__mesh_free(source); return NULL; }
    const uint32_t end = out->n_verts;
    for (uint32_t i = prev_start; i < prev_end; i++) out->verts[i].select = 0;
    for (uint32_t i = start; i < end; i++) out->verts[i].select = 1;
    if (extrude != 0.0) {
      wz3_mat4_identity(matrix);
      matrix[0] = matrix[5] = matrix[10] = (float)((double)iteration * extrude);
      wz3__mesh_transform_vertices(out, matrix, WZ3__MA_NORMAL, WZ3__MA_POS | 0x10, (int32_t)start, (int32_t)end, 1);
    }
    wz3_mat4_mul_a(local_transform, transform, tmp);
    wz3__mesh_transform_vertices(out, tmp, WZ3__MA_POS, WZ3__MA_POS, (int32_t)start, (int32_t)end, 1);
    if (mode & 2) wz3__mesh_random_srt(&random, srt, transform);
    else { wz3_mat4_mul_a(transform, step, tmp); memcpy(transform, tmp, sizeof tmp); }
    wz3_mat4_mul_a(local_transform, local_step, tmp);
    memcpy(local_transform, tmp, sizeof tmp);
    if (mode & 1) {
      wz3_mat4_identity(matrix);
      matrix[12] = (float)((double)iteration * translate_u);
      matrix[13] = (float)((double)iteration * translate_v);
      wz3__mesh_transform_vertices(out, matrix, WZ3__MA_UV0, WZ3__MA_UV0, (int32_t)start, (int32_t)end, 1);
    }
    prev_start = start;
    prev_end = end;
  }
  out->got_normals = 0;
  wz3__mesh_free(source);
  return out;
}

static wz3_mesh *wz3__mesh_op_multiply2(wz3_mesh *const *meshes, uint32_t n_valid, int32_t seed, const int32_t count1_in[3],
                                        const double t1[3], const int32_t count2_in[3], const double t2[3],
                                        int32_t random_count, const int32_t count3_in[3], const double t3[3])
{
  int32_t c1[3] = {count1_in[0], count1_in[1], count1_in[2]}, c2[3] = {count2_in[0], count2_in[1], count2_in[2]},
          c3[3] = {count3_in[0], count3_in[1], count3_in[2]};
  int32_t product = 1;
  for (int i = 0; i < 3; i++) product = (int32_t)((uint32_t)product * (uint32_t)c3[i]);
  for (int i = 0; i < 3; i++) product = (int32_t)((uint32_t)product * (uint32_t)c2[i]);
  for (int i = 0; i < 3; i++) product = (int32_t)((uint32_t)product * (uint32_t)c1[i]);
  if (product > 1024) { for (int i = 0; i < 3; i++) c1[i] = c2[i] = c3[i] = 1; }
  wz3_mesh *result = wz3__mesh_new(WZ3__MESH_DEFAULT_MASK);
  if (!result) return NULL;
  wz3_rng random;
  wz3_rng_init(&random);
  wz3_rng_seed(&random, seed);
  uint32_t last_start = 0, start = 0;
  float matrix[16];
  wz3_mat4_identity(matrix);
  for (int32_t z3 = 0; z3 < c3[2]; z3++) for (int32_t y3 = 0; y3 < c3[1]; y3++) for (int32_t x3 = 0; x3 < c3[0]; x3++)
  for (int32_t z2 = 0; z2 < c2[2]; z2++) for (int32_t y2 = 0; y2 < c2[1]; y2++) for (int32_t x2 = 0; x2 < c2[0]; x2++)
  for (int32_t z1 = 0; z1 < c1[2]; z1++) for (int32_t y1 = 0; y1 < c1[1]; y1++) for (int32_t x1 = 0; x1 < c1[0]; x1++) {
    last_start = start;
    start = result->n_verts;
    const uint32_t range = n_valid + (uint32_t)random_count;
    uint32_t choice = range ? wz3_rng_u32(&random) % range : 0u;
    if (choice > n_valid - 1) choice = n_valid - 1;
    if (wz3__mesh_append(result, meshes[choice])) { wz3__mesh_free(result); return NULL; }
    for (uint32_t i = last_start; i < start; i++) result->verts[i].select = 0;
    for (uint32_t i = start; i < result->n_verts; i++) result->verts[i].select = 1;
    matrix[12] = (float)((double)x1 * t1[0] + (double)x2 * t2[0] + (double)x3 * t3[0]);
    matrix[13] = (float)((double)y1 * t1[1] + (double)y2 * t2[1] + (double)y3 * t3[1]);
    matrix[14] = (float)((double)z1 * t1[2] + (double)z2 * t2[2] + (double)z3 * t3[2]);
    wz3__mesh_transform_vertices(result, matrix, WZ3__MA_POS, WZ3__MA_POS, (int32_t)start, (int32_t)result->n_verts, 1);
  }
  result->got_normals = 0;
  return result;
}

static wz3_mesh *wz3__mesh_of(const wz3__rt_cache *c)
{
  return c && c->kind == WZ3__RC_MESH ? (wz3_mesh *)c->obj : NULL;
}

/* Runs the mesh operator of `op`; *out is the new mesh or NULL (JS: handler returned null). */
static wz3_result wz3__rt_init_mesh_op(wz3_rt *rt, wz3__rt_op *op, wz3_mesh **out)
{
  double p[24];
  for (uint32_t i = 0; i < 24; i++) p[i] = i < op->data_words ? wz3__rt_anim_param(op, i) : 0.0;
  const wz3_mesh *in0 = wz3__mesh_of(wz3__rt_input_cache(rt, op, 0));
  wz3_mesh *r = NULL;
  *out = NULL;
  switch (op->class_id) {
  case 0x81: r = wz3__mesh_cube(wz3__rt_to_int32(p[0]), wz3__rt_to_int32(p[1]), wz3__rt_to_int32(p[2]), wz3__rt_to_int32(p[3]), p + 4); break;
  case 0x82: r = wz3__mesh_cylinder(wz3__rt_to_int32(p[0]), wz3__rt_to_int32(p[1]), wz3__rt_to_int32(p[2]), wz3__rt_to_int32(p[3]), wz3__rt_to_int32(p[4])); break;
  case 0x83: r = wz3__mesh_torus(wz3__rt_to_int32(p[0]), wz3__rt_to_int32(p[1]), p[2], p[3], p[4], p[5], wz3__rt_to_int32(p[6])); break;
  case 0x84: r = wz3__mesh_sphere(wz3__rt_to_int32(p[0]), wz3__rt_to_int32(p[1])); break;
  case 0x9d: r = wz3__mesh_grid(wz3__rt_to_int32(p[0]), wz3__rt_to_int32(p[1]), wz3__rt_to_int32(p[2])); break;
  case 0x88: if (in0) r = wz3__mesh_op_transform(in0, wz3__rt_to_int32(p[0]), p + 1, 0, 0, 0); else return WZ3_OK; break;
  case 0x89:
    if (in0) r = wz3__mesh_op_transform(in0, wz3__rt_to_int32(p[0]), p + 1, wz3__rt_to_int32(p[10]), wz3__rt_to_int32(p[11]), 1);
    else return WZ3_OK;
    break;
  case 0x92: {
    uint32_t first = 0;
    const wz3_mesh *first_mesh = NULL;
    for (uint32_t i = 0; i < op->src->n_inputs && !first_mesh; i++) {
      first_mesh = wz3__mesh_of(wz3__rt_input_cache(rt, op, i));
      first = i;
    }
    if (!first_mesh) return WZ3_OK;
    r = wz3__mesh_checked_copy(first_mesh, 0);
    for (uint32_t i = 0; r && i < op->src->n_inputs; i++) {
      const wz3_mesh *other = wz3__mesh_of(wz3__rt_input_cache(rt, op, i));
      if (!other || i == first) continue;
      if (wz3__mesh_append(r, other)) { wz3__mesh_free(r); r = NULL; }
    }
    if (r) r->got_normals = 0;
    break;
  }
  case 0x93:
    if (!in0) return WZ3_OK;
    r = wz3__mesh_checked_copy(in0, (uint32_t)wz3__rt_to_int32(p[0]) << 8);
    if (r) {
      for (uint32_t i = 0; i < r->n_faces; i++)
        if (r->faces[i].select) { r->faces[i].material = 0; r->faces[i].select = 0; r->faces[i].mask = 0; }
    }
    break;
  case 0x96: {
    const int32_t link = op->src->n_links ? op->src->links[0] : -1;
    if (link < 0 || rt->ops[link].cache.kind == WZ3__RC_NONE || !in0) return WZ3_OK;
    r = wz3__mesh_checked_copy(in0, (uint32_t)wz3__rt_to_int32(p[0]) << 8);
    if (!r) break;
    const int32_t slot = (int32_t)r->n_slots;
    if (wz3__mesh_push_slot(r, link, wz3__rt_to_int32(p[1]))) { wz3__mesh_free(r); r = NULL; break; }
    for (uint32_t i = 0; i < r->n_faces; i++)
      if (r->faces[i].select) r->faces[i].material = slot;
    break;
  }
  case 0xa3:
    if (!in0) return WZ3_OK;
    r = wz3__mesh_checked_copy(in0, 0);
    if (r) {
      for (uint32_t i = 0; i < r->n_edges; i++) {
        r->edges[i].temp[0] = wz3__mh_vert(r, r->edges[i].next[0]);
        r->edges[i].temp[1] = wz3__mh_vert(r, r->edges[i].next[1]);
      }
      for (uint32_t i = 0; i < r->n_edges; i++) {
        wz3__medge *e = &r->edges[i];
        int32_t swap = e->next[0]; e->next[0] = e->prev[0]; e->prev[0] = swap;
        swap = e->next[1]; e->next[1] = e->prev[1]; e->prev[1] = swap;
        e->vert[0] = e->temp[0];
        e->vert[1] = e->temp[1];
      }
      r->got_normals = 0;
    }
    break;
  case 0xa6: {
    if (!in0) return WZ3_OK;
    r = wz3__mesh_checked_copy(in0, (uint32_t)wz3__rt_to_int32(p[0]) << 16);
    if (!r) break;
    const int32_t which = wz3__rt_to_int32(p[1]);
    wz3__mesh_all_to_selection(r, 1, WZ3__ALL_FACE);
    wz3__mesh_face_to_vertex(r);
    float min[3], max[3];
    if (wz3__mesh_bounds(r, min, max)) { wz3__mesh_free(r); r = NULL; break; }
    wz3__mesh_all_to_selection(r, 1, WZ3__ALL_VERT);
    float matrix[16];
    wz3_mat4_identity(matrix);
    if (which & 1) matrix[12] = (float)(-0.5 * ((double)min[0] + (double)max[0]));
    if (which & 2) matrix[13] = (float)((which & 128) ? -(double)min[1] : -0.5 * ((double)min[1] + (double)max[1]));
    if (which & 4) matrix[14] = (float)(-0.5 * ((double)min[2] + (double)max[2]));
    wz3__mesh_transform_vertices(r, matrix, WZ3__MA_POS, WZ3__MA_POS, 0, 0, 0);
    break;
  }
  case 0x87:
    if (!in0) return WZ3_OK;
    r = wz3__mesh_op_subdivide(in0, wz3__rt_to_int32(p[0]), p[1], wz3__rt_to_int32(p[2]));
    break;
  case 0xa0:
    if (!in0) return WZ3_OK;
    r = wz3__mesh_op_bend(in0, wz3__rt_to_int32(p[0]), p + 1, p + 10, p + 19, p + 21, wz3__rt_to_int32(p[23]));
    break;
  case 0xab:
    if (!in0) return WZ3_OK;
    r = wz3__mesh_op_bend2(in0, p, p + 3, p[6], p[7]);
    break;
  case 0xa1:
    if (!in0) return WZ3_OK;
    r = wz3__mesh_op_select_logic(in0, (uint32_t)wz3__rt_to_int32(p[0]), (uint32_t)wz3__rt_to_int32(p[1]),
                                  (uint32_t)wz3__rt_to_int32(p[2]), wz3__rt_to_int32(p[3]));
    break;
  case 0xa5: {
    const int32_t type = wz3__rt_to_int32(p[10]);
    if (!in0) return WZ3_OK;
    if (type != 0 && type != 3) return WZ3_ERR_UNSUPPORTED;   /* cylinder/sphere projections: not in the document */
    r = wz3__mesh_op_uv_projection(in0, wz3__rt_to_int32(p[0]), p + 1, type);
    break;
  }
  case 0xb0:
    if (!in0) return WZ3_OK;
    r = wz3__mesh_checked_copy(in0, 0);
    if (r) for (uint32_t i = 0; i < r->n_faces; i++) r->faces[i].used = p[0] != 0.0 ? 1 : 0;
    break;
  case 0x86: {
    if (!in0) return WZ3_OK;
    r = wz3__mesh_checked_copy(in0, 0);
    if (!r) break;
    const int32_t mode = wz3__rt_to_int32(p[1]);
    const double half[3] = {(float)(p[5] * 0.5), (float)(p[6] * 0.5), (float)(p[7] * 0.5)};
    for (uint32_t i = 0; i < r->n_verts; i++) {
      wz3__mvert *v = &r->verts[i];
      const float *pos = (v->first >= 0 && (uint32_t)v->first < r->n_verts) ? r->verts[v->first].v[0] : v->v[0];
      const double dx = (double)pos[0] - p[2], dy = (double)pos[1] - p[3], dz = (double)pos[2] - p[4];
      v->select = (dx < 0 ? -dx : dx) <= half[0] && (dy < 0 ? -dy : dy) <= half[1] && (dz < 0 ? -dz : dz) <= half[2];
    }
    wz3__mesh_vertex_to_face_edge(r);
    if (mode & 4) wz3__mesh_face_to_vertex(r);
    wz3__mesh_selection_to_mask(r, (uint32_t)wz3__rt_to_int32(p[0]), mode & 3);
    break;
  }
  case 0x95:
    if (!in0) return WZ3_OK;
    r = wz3__mesh_op_multiply(in0, p, wz3__rt_to_int32(p[9]), wz3__rt_to_int32(p[10]), p[11], p[12], p + 13, p[16]);
    break;
  case 0xb4: {
    wz3_mesh *valid[32];
    uint32_t n_valid = 0;
    for (uint32_t i = 0; i < op->src->n_inputs && n_valid < 32; i++) {
      wz3_mesh *candidate = (wz3_mesh *)wz3__mesh_of(wz3__rt_input_cache(rt, op, i));
      if (candidate) valid[n_valid++] = candidate;
    }
    if (!n_valid) return WZ3_OK;
    const int32_t c1[3] = {wz3__rt_to_int32(p[1]), wz3__rt_to_int32(p[2]), wz3__rt_to_int32(p[3])};
    const int32_t c2[3] = {wz3__rt_to_int32(p[7]), wz3__rt_to_int32(p[8]), wz3__rt_to_int32(p[9])};
    const int32_t c3[3] = {wz3__rt_to_int32(p[14]), wz3__rt_to_int32(p[15]), wz3__rt_to_int32(p[16])};
    r = wz3__mesh_op_multiply2(valid, n_valid, wz3__rt_to_int32(p[0]), c1, p + 4, c2, p + 10, wz3__rt_to_int32(p[13]), c3, p + 17);
    break;
  }
  default: return WZ3_OK;                                      /* not ported yet */
  }
  if (!r) return WZ3_ERR_OUT_OF_MEMORY;
  if (r->oom) { wz3__mesh_free(r); return WZ3_ERR_OUT_OF_MEMORY; }
  *out = r;
  return WZ3_OK;
}

static int wz3__rt_is_mesh_class(uint16_t id)
{
  switch (id) {
  case 0x81: case 0x82: case 0x83: case 0x84: case 0x86: case 0x87: case 0x88: case 0x89: case 0x92: case 0x93: case 0x95:
  case 0x96: case 0x9d: case 0xa0: case 0xa1: case 0xa3: case 0xa5: case 0xa6: case 0xab: case 0xb0: case 0xb4:
    return 1;
  default: return 0;
  }
}

static wz3_minmesh *wz3__minmesh_of(const wz3__rt_cache *c)
{
  return c && c->kind == WZ3__RC_MINMESH ? (wz3_minmesh *)c->obj : NULL;
}

/* A valid material is the output of a KC_MATERIAL operator; MinMesh clusters refer to it by operator id. */
static int wz3__rt_material_ok(const wz3_rt *rt, int32_t op_index)
{
  if (op_index < 0 || (uint32_t)op_index >= rt->n_ops) return 0;
  const wz3__rt_cache *c = &rt->ops[op_index].cache;
  return c->kind != WZ3__RC_NONE && c->class_id == WZ3_KC_MATERIAL;
}

/* Runs the MinMesh operator of `op` (and Mesh_ToMin); *out is the new mesh or NULL (JS: handler returned null).
   Operators the JS port would throw for (a missing mesh input) report WZ3_ERR_OPERATOR. */
static wz3_result wz3__rt_init_minmesh_op(wz3_rt *rt, wz3__rt_op *op, wz3_minmesh **out)
{
  double p[24];
  for (uint32_t i = 0; i < 24; i++) p[i] = i < op->data_words ? wz3__rt_anim_param(op, i) : 0.0;
  wz3_minmesh *in0 = wz3__minmesh_of(wz3__rt_input_cache(rt, op, 0));
  wz3_minmesh *r = NULL;
  wz3_result res = WZ3_OK;
  int unsupported = 0;
  *out = NULL;
  switch (op->class_id) {
  case 0x101: r = wz3__mm_op_grid(wz3__rt_to_int32(p[0]), wz3__rt_to_int32(p[1]), wz3__rt_to_int32(p[2])); break;
  case 0x104: r = wz3__mm_op_sphere(wz3__rt_to_int32(p[0]), wz3__rt_to_int32(p[1])); break;
  case 0x110: case 0x117: {
    if (!in0) return WZ3_ERR_OPERATOR;
    const int32_t link = op->class_id == 0x110 ? (op->src->n_links ? op->src->links[0] : -1)
                                               : (op->src->n_inputs > 1 ? (int32_t)op->src->inputs[1] : -1);
    if (!wz3__rt_material_ok(rt, link)) return WZ3_OK;
    r = wz3__mm_op_mat_link(in0, link, wz3__rt_to_int32(p[0]), wz3__rt_to_int32(p[1]));
    break;
  }
  case 0x111: {
    const wz3_minmesh *valid[64];
    uint32_t n_valid = 0;
    for (uint32_t i = 0; i < op->src->n_inputs; i++) {
      wz3_minmesh *candidate = wz3__minmesh_of(wz3__rt_input_cache(rt, op, i));
      if (!candidate) continue;
      if (n_valid == 64) return WZ3_ERR_LIMIT;
      valid[n_valid++] = candidate;
    }
    if (!n_valid) return WZ3_OK;
    r = wz3__mm_op_add(valid, n_valid);
    break;
  }
  case 0x113: if (!in0) return WZ3_ERR_OPERATOR; r = wz3__mm_op_select_cube(in0, wz3__rt_to_int32(p[0]), p + 1); break;
  case 0x114: if (!in0) return WZ3_ERR_OPERATOR; r = wz3__mm_op_delete_faces(in0); break;
  case 0x120: case 0x121:
    if (!in0) return WZ3_ERR_OPERATOR;
    r = wz3__mm_op_transform_ex(in0, wz3__rt_to_int32(p[0]), p + 1, &unsupported);
    if (unsupported) return WZ3_ERR_UNSUPPORTED;
    break;
  case 0x124: if (!in0) return WZ3_ERR_OPERATOR; r = wz3__mm_op_perlin(in0, wz3__rt_to_int32(p[0]), p + 1); break;
  case 0x125: if (!in0) return WZ3_ERR_OPERATOR; r = wz3__mm_op_bend2(in0, p); break;
  case 0x128: if (!in0) return WZ3_ERR_OPERATOR; r = wz3__mm_op_bone_chain(in0, p); break;
  case 0x129: {
    if (!in0) return WZ3_ERR_OPERATOR;
    const int32_t spline = wz3__rt_input_cache(rt, op, 1) ? (int32_t)op->src->inputs[1] : -1;
    r = wz3__mm_op_bone_train(in0, spline, p[0], wz3__rt_to_int32(p[1]), p[2]);
    break;
  }
  case 0x12a: if (!in0) return WZ3_ERR_OPERATOR; r = wz3__mm_op_triangulate(in0); break;
  case 0x12c: {
    const wz3__rt_cache *spline = wz3__rt_input_cache(rt, op, 0);
    const wz3_minmesh *mesh0 = wz3__minmesh_of(wz3__rt_input_cache(rt, op, 1));
    if (!spline || spline->kind != WZ3__RC_BLOB || !mesh0) return WZ3_OK;
    int pipe_oom;
    r = wz3__mm_op_pipe((const wz3__rt_blob *)spline->obj, mesh0, wz3__minmesh_of(wz3__rt_input_cache(rt, op, 2)),
                        wz3__minmesh_of(wz3__rt_input_cache(rt, op, 3)), wz3__rt_to_int32(p[0]), p[1], p[2], p[3], &pipe_oom);
    if (!r) return pipe_oom ? WZ3_ERR_OUT_OF_MEMORY : WZ3_OK;      /* else: not a mode 4 spline, or inconsistent keys */
    break;
  }
  case 0x12f: if (!in0) return WZ3_ERR_OPERATOR; r = wz3__mm_op_multiply(in0, p); break;
  case 0x131: if (!in0) return WZ3_ERR_OPERATOR; r = wz3__mm_op_center(in0, wz3__rt_to_int32(p[0])); break;
  case 0x133:
    r = wz3__mm_op_font3d(p[0], p[1], p[2], op->src->n_strings > 0 ? op->src->strings[0] : "", op->src->n_strings > 1 ? op->src->strings[1] : "", &res);
    if (!r) return res;
    break;
  case 0x134: {
    if (!in0) return WZ3_ERR_OPERATOR;
    const wz3_bitmap *bitmap = (rt->options.host.bitmap && op->src->n_inputs > 1) ? rt->options.host.bitmap(rt->options.host.user, op->src->inputs[1]) : NULL;
    r = wz3__mm_op_explode(in0, p, bitmap);
    break;
  }
  case 0xb5: {
    const wz3_mesh *mesh = wz3__mesh_of(wz3__rt_input_cache(rt, op, 0));
    if (!mesh) return WZ3_OK;
    r = wz3__mm_from_mesh(mesh);
    break;
  }
  default: return WZ3_OK;
  }
  if (!r) return WZ3_ERR_OUT_OF_MEMORY;
  if (r->oom) { wz3__mm_free(r); return WZ3_ERR_OUT_OF_MEMORY; }
  *out = r;
  return WZ3_OK;
}

static int wz3__rt_is_minmesh_class(uint16_t id)
{
  switch (id) {
  case 0x101: case 0x104: case 0x110: case 0x111: case 0x113: case 0x114: case 0x117: case 0x120: case 0x121: case 0x124:
  case 0x125: case 0x128: case 0x129: case 0x12a: case 0x12c: case 0x12f: case 0x131: case 0x133: case 0x134: case 0xb5:
    return 1;
  default: return 0;
  }
}

static wz3_result wz3__rt_init_op(wz3_rt *rt, wz3__rt_op *op)
{
  wz3__rt_cache fresh;
  memset(&fresh, 0, sizeof fresh);
  if (rt->options.geometry == WZ3_GEOMETRY_PLACEHOLDER && (wz3__rt_is_minmesh_class(op->class_id) || wz3__rt_is_mesh_class(op->class_id))) {
    const int is_min = wz3__rt_is_minmesh_class(op->class_id);   /* a placeholder: the operator has an output, the host owns the geometry */
    fresh.kind = is_min ? WZ3__RC_MINMESH : WZ3__RC_MESH;
    fresh.class_id = is_min ? WZ3_KC_MINMESH : WZ3_KC_MESH;
    wz3__rt_cache_release(&op->cache);
    op->cache = fresh;
    return WZ3_OK;
  }
  if (rt->options.frame_jobs) {
    int handled;
    const wz3_result fr = wz3__fj_init_op(rt, op, &fresh, &handled);
    if (handled) {
      if (fr != WZ3_OK) return fr;
      wz3__rt_cache_release(&op->cache);
      op->cache = fresh;
      return WZ3_OK;
    }
  }
  if (wz3__rt_is_minmesh_class(op->class_id)) {
    wz3_minmesh *mesh;
    const wz3_result mr = wz3__rt_init_minmesh_op(rt, op, &mesh);
    if (mr != WZ3_OK) return mr;
    if (mesh) {
      fresh.kind = WZ3__RC_MINMESH; fresh.owned = 1; fresh.class_id = WZ3_KC_MINMESH; fresh.obj = mesh;
    }
    if (rt->options.host.minmesh) {
      const wz3_result sr = rt->options.host.minmesh(rt->options.host.user, op->id, mesh);
      if (sr != WZ3_OK) { if (mesh) wz3__mm_free(mesh); return sr; }
    }
    wz3__rt_cache_release(&op->cache);
    op->cache = fresh;
    return WZ3_OK;
  }
  if (wz3__rt_is_mesh_class(op->class_id)) {
    wz3_mesh *mesh;
    const wz3_result mr = wz3__rt_init_mesh_op(rt, op, &mesh);
    if (mr != WZ3_OK) return mr;
    if (mesh) {
      fresh.kind = WZ3__RC_MESH; fresh.owned = 1; fresh.class_id = WZ3_KC_MESH; fresh.obj = mesh;
      if (rt->options.host.mesh) {
        const wz3_result sr = rt->options.host.mesh(rt->options.host.user, op->id, mesh);
        if (sr != WZ3_OK) { wz3__mesh_free(mesh); return sr; }
      }
    }
    wz3__rt_cache_release(&op->cache);
    op->cache = fresh;
    return WZ3_OK;
  }
  wz3_result r = WZ3_OK;
  switch (op->class_id) {
  case 0x06: case 0x16: {                                      /* Event, EventTime: the input's object */
    const wz3__rt_cache *input = wz3__rt_input_cache(rt, op, 0);
    if (input) { fresh = *input; fresh.owned = 0; }
    break;
  }
  case 0x0d: {                                                 /* Demo */
    const int variable = (op->convention & WZ3_OPC_VARIABLEINPUT) != 0;
    const uint32_t count = variable ? op->src->n_inputs : (op->input_slots > op->src->n_inputs ? op->input_slots : op->src->n_inputs);
    int ok = 1;
    for (uint32_t i = 0; i < count && ok; i++) {
      const wz3__rt_cache *c = wz3__rt_input_cache(rt, op, i);
      if (!c || (c->class_id != WZ3_KC_DEMO && c->class_id != WZ3_KC_IPP)) ok = 0;
    }
    if (ok) { fresh.kind = WZ3__RC_DEMO; fresh.class_id = WZ3_KC_DEMO; }
    break;
  }
  case 0x18: case 0x1a: {                                      /* Spline, PipeSpline */
    wz3__rt_blob *blob = op->class_id == 0x18 ? wz3__rt_blob_from_bytes(op->src->blob, op->src->blob_size, &r)
                                              : wz3__rt_blob_from_pipe(op->src->blob, op->src->blob_size, &r);
    if (!blob) return r;
    fresh.kind = WZ3__RC_BLOB; fresh.owned = 1; fresh.class_id = WZ3_KC_SPLINE; fresh.obj = blob;
    break;
  }
  case 0x19: {                                                 /* Shaker */
    double params[17];
    for (uint32_t i = 0; i < 17; i++) params[i] = i < op->data_words ? wz3__rt_anim_param(op, i) : 0.0;
    wz3__rt_shaker *sh = wz3__rt_shaker_make(op->id, wz3__rt_input_cache(rt, op, 0), wz3__rt_input_cache(rt, op, 1), params);
    if (!sh) return WZ3_ERR_OUT_OF_MEMORY;
    fresh.kind = WZ3__RC_SHAKER; fresh.owned = 1; fresh.class_id = WZ3_KC_SPLINE; fresh.obj = sh;
    break;
  }
  case 0x1c: {                                                 /* SplineScale */
    const wz3__rt_cache *input = wz3__rt_input_cache(rt, op, 0);
    if (!input || input->kind != WZ3__RC_BLOB) break;
    wz3__rt_blob *blob = wz3__rt_blob_scaled((const wz3__rt_blob *)input->obj, wz3__rt_anim_param(op, 0),
                                             wz3__rt_anim_param(op, 1), wz3__rt_anim_param(op, 2),
                                             wz3__rt_to_int32(wz3__rt_anim_param(op, 3)));
    if (!blob) return WZ3_ERR_OUT_OF_MEMORY;
    fresh.kind = WZ3__RC_BLOB; fresh.owned = 1; fresh.class_id = WZ3_KC_SPLINE; fresh.obj = blob;
    break;
  }
  default:                                                     /* MissingCache */
    fresh.kind = WZ3__RC_MISSING;
    fresh.class_id = op->out_class;
    break;
  }
  wz3__rt_cache_release(&op->cache);
  op->cache = fresh;
  return WZ3_OK;
}

static wz3_result wz3__rt_precalc_op(wz3_rt *rt, uint32_t index)
{
  wz3__rt_op *op = &rt->ops[index];
  if (op->calc_state == 2 && !op->changed) return WZ3_OK;
  if (op->calc_state == 1) return WZ3_ERR_OPERATOR;          /* operator calculation cycle */
  if (rt->options.host.cancelled && rt->options.host.cancelled(rt->options.host.user)) return WZ3_ERR_CANCELLED;
  op->calc_state = 1;
  if (op->changed || op->cache.kind == WZ3__RC_NONE) memcpy(op->anim, op->edit, (size_t)op->data_words * sizeof(wz3_word));
  const uint32_t saved = rt->env.n_saves;
  uint32_t pops;
  wz3_result r = wz3__rt_exec_animation(rt, op, &pops);
  for (uint32_t i = 0; r == WZ3_OK && i < op->src->n_inputs; i++) r = wz3__rt_precalc_op(rt, op->src->inputs[i]);
  if (!(op->convention & WZ3_OPC_DONTCALLLINK))
    for (uint32_t i = 0; r == WZ3_OK && i < op->src->n_links; i++)
      if (op->src->links[i] >= 0) r = wz3__rt_precalc_op(rt, (uint32_t)op->src->links[i]);
  if (r == WZ3_OK && rt->options.host.cancelled && rt->options.host.cancelled(rt->options.host.user)) r = WZ3_ERR_CANCELLED;   /* before the costly part */
  if (r == WZ3_OK) r = wz3__rt_init_op(rt, op);
  if (r == WZ3_OK) {
    op->calc_error = op->cache.kind == WZ3__RC_NONE;
    op->changed = 0;
    op->calc_state = 2;
    if (rt->progress_done < rt->progress_total) rt->progress_done++;
    if (rt->options.host.progress) rt->options.host.progress(rt->options.host.user, index, rt->progress_done, rt->progress_total);
    if (rt->options.release_consumed && rt->remaining) {
      /* the consumers of an operator count its inputs and links; the last one frees a mesh */
      for (uint32_t i = 0; i < op->src->n_inputs + op->src->n_links; i++) {
        const int32_t source = i < op->src->n_inputs ? (int32_t)op->src->inputs[i] : op->src->links[i - op->src->n_inputs];
        if (source < 0 || rt->remaining[source] == 0) continue;
        if (--rt->remaining[source] == 0 && rt->ops[source].cache.owned &&
            (rt->ops[source].cache.kind == WZ3__RC_MESH || rt->ops[source].cache.kind == WZ3__RC_MINMESH)) {
          if (rt->ops[source].cache.kind == WZ3__RC_MESH) wz3__mesh_free((wz3_mesh *)rt->ops[source].cache.obj);
          else wz3__mm_free((wz3_minmesh *)rt->ops[source].cache.obj);
          rt->ops[source].cache.obj = NULL;
          rt->ops[source].cache.owned = 0;
        }
      }
    }
  } else {
    op->calc_state = 0;
  }
  const wz3_result popped = wz3__rt_env_pop(&rt->env, rt->env.n_saves - saved);
  return r != WZ3_OK ? r : popped;
}

/* ---- timing ---- */

static int32_t wz3__rt_wrap32(int64_t v) { return (int32_t)(uint32_t)(uint64_t)v; }

WZ3_API int32_t wz3_sample_to_beat(int32_t sample, uint32_t song_bpm_fixed, int buzz_timing, int32_t sample_rate)
{
  const int32_t bpm_fixed = (int32_t)song_bpm_fixed;
  if (!buzz_timing) {
    const int32_t denominator = (int32_t)((uint32_t)sample_rate * 60u);
    if (!denominator) return 0;
    return wz3__rt_wrap32((int64_t)sample * (int64_t)bpm_fixed / (int64_t)denominator);
  }
  const int32_t bpm = bpm_fixed >> 16;
  const int32_t divisor = (int32_t)((uint32_t)bpm * 8u);
  if (!divisor) return 0;
  const int32_t numerator = (int32_t)((uint32_t)sample_rate * 60u);
  const int32_t denominator = (int32_t)((double)numerator / (double)divisor);   /* Math.trunc(a / b) | 0 */
  if (!denominator) return 0;
  const int32_t shifted = wz3__rt_wrap32((int64_t)((uint64_t)(int64_t)sample << 16) / (int64_t)denominator);
  return shifted / 8;
}

/* ---- runtime object ---- */

/* The operators wz3__rt_precalc_op visits from `root` and how many of them are already calculated. */
static wz3_result wz3__rt_count_reachable(const wz3_rt *rt, uint32_t root, uint32_t *total, uint32_t *done)
{
  uint8_t *seen = (uint8_t *)WZ3_MALLOC(rt->n_ops);
  uint32_t *stack = (uint32_t *)WZ3_MALLOC((size_t)rt->n_ops * sizeof *stack);
  if (!seen || !stack) { WZ3_FREE(seen); WZ3_FREE(stack); return WZ3_ERR_OUT_OF_MEMORY; }
  memset(seen, 0, rt->n_ops);
  uint32_t depth = 0, count = 0, finished = 0;
  stack[depth++] = root;
  seen[root] = 1;
  while (depth) {
    const wz3__rt_op *op = &rt->ops[stack[--depth]];
    count++;
    if (op->calc_state == 2 && !op->changed) finished++;
    for (uint32_t i = 0; i < op->src->n_inputs; i++) {
      const uint32_t next = op->src->inputs[i];
      if (next < rt->n_ops && !seen[next]) { seen[next] = 1; stack[depth++] = next; }
    }
    if (!(op->convention & WZ3_OPC_DONTCALLLINK))
      for (uint32_t i = 0; i < op->src->n_links; i++) {
        const int32_t next = op->src->links[i];
        if (next >= 0 && (uint32_t)next < rt->n_ops && !seen[next]) { seen[next] = 1; stack[depth++] = (uint32_t)next; }
      }
  }
  WZ3_FREE(seen);
  WZ3_FREE(stack);
  *total = count;
  *done = finished;
  return WZ3_OK;
}

WZ3_API wz3_result wz3_rt_precalc(wz3_rt *rt)
{
  if (!rt) return WZ3_ERR_ARGUMENT;
  const uint32_t root = rt->kx->roots[rt->current_root];
  if (root >= rt->n_ops) return WZ3_ERR_ARGUMENT;
  rt->progress_done = rt->progress_total = 0;
  wz3_result r = WZ3_OK;
  if (rt->options.host.progress) r = wz3__rt_count_reachable(rt, root, &rt->progress_total, &rt->progress_done);
  if (r != WZ3_OK) return r;
  r = wz3__rt_init_frame(rt, 0, 0);
  if (r == WZ3_OK) {
    r = wz3__rt_precalc_op(rt, root);
    if (r == WZ3_OK) rt->precalculated = 1;
  }
  wz3__rt_exit_frame(rt);
  return r;
}

WZ3_API wz3_result wz3_rt_frame(wz3_rt *rt, int32_t beat, int32_t milliseconds)
{
  if (!rt) return WZ3_ERR_ARGUMENT;
  wz3_result r = WZ3_OK;
  if (!rt->precalculated) {
    r = wz3_rt_precalc(rt);
    if (r != WZ3_OK) return r;
  }
  r = wz3__rt_init_frame(rt, beat, milliseconds);
  if (r == WZ3_OK) {
    for (uint32_t i = 0; i < rt->n_events && r == WZ3_OK; i++) {
      int added;
      r = wz3__rt_add_static_event(rt, i, &added);
    }
    const uint32_t root = rt->kx->roots[rt->current_root];
    if (r == WZ3_OK && rt->trace) {
      /* tests: which events are active, per operator in chain order (the reference fixtures in tests/data) */
      uint32_t h = 0x811c9dc5u, n = 0;
      for (uint32_t i = 0; i < rt->n_ops; i++) {
        if (rt->ops[i].first_event < 0) continue;
        h = (h ^ i) * 0x01000193u;
        for (int32_t ev = rt->ops[i].first_event; ev >= 0; ev = rt->events[ev].next_op) {
          h = (h ^ (uint32_t)ev) * 0x01000193u;
          n++;
        }
      }
      rt->trace_active = (h ^ n) * 0x01000193u;
    }
    if (r == WZ3_OK && beat >= 0 && root < rt->n_ops && rt->ops[root].cache.kind != WZ3__RC_NONE &&
        rt->ops[root].cache.class_id == WZ3_KC_DEMO)
      r = wz3__rt_exec_op(rt, root);
  }
  wz3__rt_exit_frame(rt);
  wz3__fj_refresh_view(rt);
  return r;
}

WZ3_API wz3_result wz3_rt_frame_at_sample(wz3_rt *rt, int32_t sample, int32_t sample_rate)
{
  if (!rt || sample_rate <= 0) return WZ3_ERR_ARGUMENT;
  const int32_t beat = wz3_sample_to_beat(sample, rt->kx->song_bpm_fixed, (rt->kx->flags & 4u) != 0, sample_rate);
  const int32_t milliseconds = wz3__rt_wrap32((int64_t)sample * 1000 / (int64_t)sample_rate);
  return wz3_rt_frame(rt, beat, milliseconds);
}

/* ---- snapshot / restore ---- */

typedef struct wz3__rt_wr {
  uint8_t *p;
  size_t pos, cap;
  int overflow;
} wz3__rt_wr;

static void wz3__rt_put(wz3__rt_wr *w, const void *data, size_t n)
{
  if (w->p) {
    if (w->pos + n > w->cap) { w->overflow = 1; return; }
    memcpy(w->p + w->pos, data, n);
  }
  w->pos += n;
}
static void wz3__rt_put_u32(wz3__rt_wr *w, uint32_t v)
{
  const uint8_t b[4] = {(uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24)};
  wz3__rt_put(w, b, 4);
}
static void wz3__rt_put_chain(wz3__rt_wr *w, const wz3__rt_chain *c)
{
  wz3__rt_put_u32(w, c->count);
  for (uint32_t i = 0; i < c->count; i++) {
    wz3__rt_put_u32(w, c->items[i].op_id);
    wz3__rt_put_u32(w, (uint32_t)c->items[i].reset);
    for (int k = 0; k < 4; k++) wz3__rt_put_u32(w, (uint32_t)c->items[i].words[k]);
    wz3__rt_put_u32(w, c->items[i].blob_size);
    if (c->items[i].blob_size) {
      static const uint8_t zero[4] = {0, 0, 0, 0};
      wz3__rt_put(w, c->items[i].blob, c->items[i].blob_size);
      wz3__rt_put(w, zero, (4u - (c->items[i].blob_size & 3u)) & 3u);
    }
  }
}

#define WZ3__RT_SNAPSHOT_MAGIC 0x32535a57u /* WZS2 */

static void wz3__rt_write_snapshot(const wz3_rt *rt, wz3__rt_wr *w)
{
  const wz3__rt_env *env = &rt->env;
  wz3__rt_put_u32(w, WZ3__RT_SNAPSHOT_MAGIC);
  wz3__rt_put_u32(w, rt->n_ops);
  wz3__rt_put_u32(w, rt->n_events);
  wz3__rt_put_u32(w, rt->current_root);
  wz3__rt_put_u32(w, (uint32_t)rt->precalculated);
  for (uint32_t i = 0; i < rt->n_ops; i++) {
    wz3__rt_put_u32(w, rt->ops[i].changed);
    for (uint32_t k = 0; k < rt->ops[i].data_words; k++) wz3__rt_put_u32(w, rt->ops[i].anim[k].u);
  }
  for (uint32_t i = 0; i < rt->n_events; i++) {
    wz3__rt_put_u32(w, (uint32_t)rt->events[i].start);
    wz3__rt_put_u32(w, (uint32_t)rt->events[i].end);
    wz3__rt_put_chain(w, &rt->events[i].instances);
  }
  wz3__rt_put_u32(w, (uint32_t)env->beat_time);
  wz3__rt_put_u32(w, (uint32_t)env->current_time);
  wz3__rt_put_u32(w, (uint32_t)env->last_time);
  wz3__rt_put_u32(w, (uint32_t)env->time_delta);
  wz3__rt_put_u32(w, (uint32_t)env->time_slices);
  wz3__rt_put_u32(w, (uint32_t)env->time_jitter);
  wz3__rt_put_u32(w, (uint32_t)env->time_reset);
  wz3__rt_put_u32(w, env->n_dynamic);
  for (uint32_t i = 0; i < env->n_dynamic; i++) wz3__rt_put_u32(w, env->dynamic[i]);
  wz3__rt_put_chain(w, &env->default_instances);
  {
    uint32_t n_effects = 0;
    for (uint32_t i = 0; i < rt->n_ops; i++) if (rt->ops[i].cache.kind == WZ3__RC_EFFECT && rt->ops[i].cache.obj) n_effects++;
    wz3__rt_put_u32(w, n_effects);
    for (uint32_t i = 0; i < rt->n_ops; i++) {
      if (rt->ops[i].cache.kind != WZ3__RC_EFFECT || !rt->ops[i].cache.obj) continue;
      wz3__rt_put_u32(w, i);
      wz3__rt_put_chain(w, &((const wz3__fj_effect *)rt->ops[i].cache.obj)->scene_instances);
    }
  }
  for (int i = 0; i < 32; i++)                      /* the scene markers outlive a frame */
    for (int k = 0; k < 16; k++) {
      uint32_t bits;
      memcpy(&bits, &rt->fj.markers[i][k], 4);
      wz3__rt_put_u32(w, bits);
    }
}

WZ3_API size_t wz3_rt_snapshot_size(const wz3_rt *rt)
{
  if (!rt) return 0;
  wz3__rt_wr w = {NULL, 0, 0, 0};
  wz3__rt_write_snapshot(rt, &w);
  return w.pos;
}

WZ3_API wz3_result wz3_rt_snapshot(const wz3_rt *rt, void *buffer, size_t size)
{
  if (!rt || !buffer) return WZ3_ERR_ARGUMENT;
  wz3__rt_wr w = {(uint8_t *)buffer, 0, size, 0};
  wz3__rt_write_snapshot(rt, &w);
  return w.overflow ? WZ3_ERR_LIMIT : WZ3_OK;
}

typedef struct wz3__rt_rd {
  const uint8_t *p;
  size_t pos, len;
  int bad;
} wz3__rt_rd;

static uint32_t wz3__rt_get_u32(wz3__rt_rd *r)
{
  if (r->pos + 4 > r->len) { r->bad = 1; return 0; }
  const uint8_t *q = r->p + r->pos;
  r->pos += 4;
  return (uint32_t)q[0] | ((uint32_t)q[1] << 8) | ((uint32_t)q[2] << 16) | ((uint32_t)q[3] << 24);
}

/* walks one serialized chain without keeping it; 0 when it is malformed */
static int wz3__rt_skip_chain(wz3__rt_rd *r)
{
  const uint32_t count = wz3__rt_get_u32(r);
  if (r->bad) return 0;
  for (uint32_t i = 0; i < count; i++) {
    if (r->pos + 28 > r->len) { r->bad = 1; return 0; }
    r->pos += 24;
    const uint32_t size = wz3__rt_get_u32(r);
    const size_t padded = (size_t)size + ((4u - (size & 3u)) & 3u);
    if (padded > r->len - r->pos) { r->bad = 1; return 0; }
    r->pos += padded;
  }
  return 1;
}

static wz3_result wz3__rt_get_chain(wz3__rt_rd *r, wz3__rt_chain *c)
{
  const uint32_t count = wz3__rt_get_u32(r);
  if (r->bad || (size_t)count > (r->len - r->pos) / 28) return WZ3_ERR_FORMAT;
  wz3__rt_chain_clear(c);
  if (count > c->cap) {
    wz3__rt_instance *items = (wz3__rt_instance *)WZ3_MALLOC((size_t)count * sizeof *items);
    if (!items) return WZ3_ERR_OUT_OF_MEMORY;
    WZ3_FREE(c->items);
    c->items = items;
    c->cap = count;
  }
  for (uint32_t i = 0; i < count; i++) {
    wz3__rt_instance *item = &c->items[i];
    memset(item, 0, sizeof *item);
    item->op_id = wz3__rt_get_u32(r);
    item->reset = (int32_t)wz3__rt_get_u32(r);
    for (int k = 0; k < 4; k++) item->words[k] = (int32_t)wz3__rt_get_u32(r);
    const uint32_t size = wz3__rt_get_u32(r);
    const size_t padded = (size_t)size + ((4u - (size & 3u)) & 3u);
    if (r->bad || padded > r->len - r->pos) return WZ3_ERR_FORMAT;
    if (size) {
      item->blob = WZ3_MALLOC(size);
      if (!item->blob) return WZ3_ERR_OUT_OF_MEMORY;
      memcpy(item->blob, r->p + r->pos, size);
      item->blob_size = size;
    }
    r->pos += padded;
    c->count = i + 1;
  }
  return r->bad ? WZ3_ERR_FORMAT : WZ3_OK;
}

WZ3_API wz3_result wz3_rt_restore(wz3_rt *rt, const void *buffer, size_t size)
{
  if (!rt || !buffer) return WZ3_ERR_ARGUMENT;
  {
    /* walk the whole buffer first so a damaged snapshot cannot leave the runtime half restored */
    wz3__rt_rd v = {(const uint8_t *)buffer, 0, size, 0};
    if (wz3__rt_get_u32(&v) != WZ3__RT_SNAPSHOT_MAGIC || wz3__rt_get_u32(&v) != rt->n_ops ||
        wz3__rt_get_u32(&v) != rt->n_events)
      return WZ3_ERR_FORMAT;
    if (wz3__rt_get_u32(&v) >= WZ3_MAX_OP_ROOT) return WZ3_ERR_FORMAT;
    wz3__rt_get_u32(&v);
    for (uint32_t i = 0; i < rt->n_ops && !v.bad; i++) v.pos += 4 + (size_t)rt->ops[i].data_words * 4;
    for (uint32_t i = 0; i < rt->n_events && !v.bad; i++) {
      if (v.pos + 8 > v.len) { v.bad = 1; break; }
      v.pos += 8;
      wz3__rt_skip_chain(&v);
    }
    if (!v.bad && v.pos + 32 > v.len) v.bad = 1;
    if (!v.bad) {
      v.pos += 28;
      const uint32_t n_dynamic = wz3__rt_get_u32(&v);
      if (v.bad || (size_t)n_dynamic > (v.len - v.pos) / 4) v.bad = 1;
      else {
        for (uint32_t i = 0; i < n_dynamic; i++)
          if (wz3__rt_get_u32(&v) >= rt->n_events) v.bad = 1;
        wz3__rt_skip_chain(&v);
        const uint32_t n_effects = wz3__rt_get_u32(&v);
        for (uint32_t i = 0; !v.bad && i < n_effects; i++) {
          if (wz3__rt_get_u32(&v) >= rt->n_ops) v.bad = 1;
          wz3__rt_skip_chain(&v);
        }
        if (!v.bad && v.pos + 32 * 16 * 4 != v.len) v.bad = 1;
      }
    }
    if (v.bad) return WZ3_ERR_FORMAT;
  }
  wz3__rt_rd r = {(const uint8_t *)buffer, 0, size, 0};
  r.pos = 12;
  const uint32_t root = wz3__rt_get_u32(&r);
  const uint32_t precalculated = wz3__rt_get_u32(&r);
  if (r.bad || root >= WZ3_MAX_OP_ROOT) return WZ3_ERR_FORMAT;
  wz3_result res = WZ3_OK;
  for (uint32_t i = 0; i < rt->n_ops; i++) {
    rt->ops[i].changed = (uint8_t)wz3__rt_get_u32(&r);
    for (uint32_t k = 0; k < rt->ops[i].data_words; k++) rt->ops[i].anim[k].u = wz3__rt_get_u32(&r);
    rt->ops[i].first_event = -1;
  }
  for (uint32_t i = 0; i < rt->n_events && res == WZ3_OK; i++) {
    rt->events[i].start = (int32_t)wz3__rt_get_u32(&r);
    rt->events[i].end = (int32_t)wz3__rt_get_u32(&r);
    rt->events[i].next_op = -1;
    res = wz3__rt_get_chain(&r, &rt->events[i].instances);
  }
  wz3__rt_env *env = &rt->env;
  env->beat_time = (int32_t)wz3__rt_get_u32(&r);
  env->current_time = (int32_t)wz3__rt_get_u32(&r);
  env->last_time = (int32_t)wz3__rt_get_u32(&r);
  env->time_delta = (int32_t)wz3__rt_get_u32(&r);
  env->time_slices = (int32_t)wz3__rt_get_u32(&r);
  env->time_jitter = (int32_t)wz3__rt_get_u32(&r);
  env->time_reset = (int)wz3__rt_get_u32(&r);
  const uint32_t n_dynamic = wz3__rt_get_u32(&r);
  if (res == WZ3_OK && (r.bad || (size_t)n_dynamic > (r.len - r.pos) / 4)) res = WZ3_ERR_FORMAT;
  if (res == WZ3_OK) {
    env->n_dynamic = 0;
    for (uint32_t i = 0; i < n_dynamic && res == WZ3_OK; i++) {
      const uint32_t ev = wz3__rt_get_u32(&r);
      if (ev >= rt->n_events) res = WZ3_ERR_FORMAT;
      else res = wz3__rt_push_index(&env->dynamic, &env->n_dynamic, &env->cap_dynamic, ev);
    }
  }
  if (res == WZ3_OK) res = wz3__rt_get_chain(&r, &env->default_instances);
  if (res == WZ3_OK) {
    for (uint32_t i = 0; i < rt->n_ops; i++)
      if (rt->ops[i].cache.kind == WZ3__RC_EFFECT && rt->ops[i].cache.obj)
        wz3__rt_chain_clear(&((wz3__fj_effect *)rt->ops[i].cache.obj)->scene_instances);
    const uint32_t n_effects = wz3__rt_get_u32(&r);
    for (uint32_t i = 0; res == WZ3_OK && i < n_effects; i++) {
      const uint32_t op = wz3__rt_get_u32(&r);
      if (op < rt->n_ops && rt->ops[op].cache.kind == WZ3__RC_EFFECT && rt->ops[op].cache.obj) {
        res = wz3__rt_get_chain(&r, &((wz3__fj_effect *)rt->ops[op].cache.obj)->scene_instances);
      } else {
        wz3__rt_chain scratch;                       /* the effect does not exist in this runtime (not precalculated): drop its tape */
        memset(&scratch, 0, sizeof scratch);
        res = wz3__rt_get_chain(&r, &scratch);
        wz3__rt_chain_free(&scratch);
      }
    }
  }
  if (res == WZ3_OK) {
    for (int i = 0; i < 32; i++)
      for (int k = 0; k < 16; k++) {
        const uint32_t bits = wz3__rt_get_u32(&r);
        memcpy(&rt->fj.markers[i][k], &bits, 4);
      }
  }
  if (res == WZ3_OK && r.bad) res = WZ3_ERR_FORMAT;
  if (res != WZ3_OK) return res;
  rt->current_root = root;
  rt->precalculated = (int)precalculated;
  env->n_cleanup = 0;
  env->n_saves = 0;
  env->event_spline = -1;
  env->current_event = -1;
  env->clear_instance_memory = 0;
  env->chain = &env->default_instances;
  env->cursor = 0;
  wz3__rt_stack_pop_all(env);
  return WZ3_OK;
}

WZ3_API wz3_result wz3_rt_render_end(wz3_rt *rt)
{
  if (!rt) return WZ3_ERR_ARGUMENT;
  if (!rt->render_saved) return WZ3_OK;
  wz3__rt_exit_frame(rt);
  const wz3_result r = wz3_rt_restore(rt, rt->render_saved, rt->render_saved_size);
  if (r != WZ3_OK) return r;                         /* out of memory: the snapshot stays for another attempt */
  rt->fj.aspect = rt->render_saved_aspect;
  WZ3_FREE(rt->render_saved);
  rt->render_saved = NULL;
  rt->render_saved_size = 0;
  wz3__fj_refresh_view(rt);
  return WZ3_OK;
}

WZ3_API wz3_result wz3_rt_render_begin(wz3_rt *rt, uint32_t input, double aspect, int32_t *root)
{
  if (!rt || !root) return WZ3_ERR_ARGUMENT;
  *root = -1;
  if (!rt->precalculated || input >= rt->n_ops || rt->render_saved) return WZ3_ERR_ARGUMENT;
  const size_t size = wz3_rt_snapshot_size(rt);
  uint8_t *saved = (uint8_t *)WZ3_MALLOC(size ? size : 1);
  if (!saved) return WZ3_ERR_OUT_OF_MEMORY;
  wz3_result r = wz3_rt_snapshot(rt, saved, size);
  if (r != WZ3_OK) { WZ3_FREE(saved); return r; }
  rt->render_saved = saved;
  rt->render_saved_size = size;
  rt->render_saved_aspect = rt->fj.aspect;
  rt->fj.aspect = aspect;
  r = wz3__rt_init_frame(rt, 0, 0);
  if (r == WZ3_OK) r = wz3__rt_exec_op(rt, input);
  if (r == WZ3_OK) *root = rt->fj.last_output;
  wz3__fj_refresh_view(rt);
  if (r != WZ3_OK) (void)wz3_rt_render_end(rt);
  return r;
}

WZ3_API void wz3_rt_destroy(wz3_rt *rt)
{
  if (!rt) return;
  WZ3_FREE(rt->render_saved);
  for (uint32_t i = 0; i < rt->n_ops; i++) wz3__rt_cache_release(&rt->ops[i].cache);
  for (uint32_t i = 0; i < rt->n_events; i++) wz3__rt_chain_free(&rt->events[i].instances);
  wz3__rt_chain_free(&rt->env.default_instances);
  WZ3_FREE(rt->env.dynamic);
  WZ3_FREE(rt->env.cleanup);
  WZ3_FREE(rt->env.stack);
  {
    wz3__fj *fj = &rt->fj;
    WZ3_FREE(fj->mesh.p); WZ3_FREE(fj->effect.p); WZ3_FREE(fj->light.p); WZ3_FREE(fj->instance.p);
    WZ3_FREE(fj->variables.p); WZ3_FREE(fj->marker_tables.p); WZ3_FREE(fj->nodes.p); WZ3_FREE(fj->outputs.p);
    WZ3_FREE(fj->order.p);
    WZ3_FREE(fj->ipp_node);
  }
  WZ3_FREE(rt->ops);
  WZ3_FREE(rt->words);
  WZ3_FREE(rt->remaining);
  WZ3_FREE(rt->events);
  WZ3_FREE(rt->splines);
  WZ3_FREE(rt->spline_times);
  WZ3_FREE(rt);
}

WZ3_API wz3_rt *wz3_rt_create(const wz3_kx *kx, const wz3_rt_options *options, wz3_result *result)
{
  wz3_result dummy;
  if (!result) result = &dummy;
  if (!kx) { *result = WZ3_ERR_ARGUMENT; return NULL; }
  wz3_rt *rt = (wz3_rt *)WZ3_MALLOC(sizeof *rt);
  if (!rt) { *result = WZ3_ERR_OUT_OF_MEMORY; return NULL; }
  memset(rt, 0, sizeof *rt);
  rt->kx = kx;
  rt->options.generic_exec = 1;
  if (options) rt->options = *options;
  wz3__rt_make_permutation(rt->perm);

  size_t total_words = 0, total_keys = 0;
  for (uint32_t i = 0; i < kx->n_ops; i++) total_words += (size_t)(kx->classes[kx->ops[i].class_index].convention & 0xffu);
  for (uint32_t i = 0; i < kx->n_splines; i++)
    for (uint32_t c = 0; c < kx->splines[i].n_channels && c < 4; c++) total_keys += kx->splines[i].channels[c].n_keys;
  rt->ops = (wz3__rt_op *)WZ3_MALLOC(((size_t)kx->n_ops + 1) * sizeof(wz3__rt_op));
  rt->words = (wz3_word *)WZ3_MALLOC((total_words * 2 + 1) * sizeof(wz3_word));
  rt->events = (wz3__rt_event *)WZ3_MALLOC(((size_t)kx->n_events + 1) * sizeof(wz3__rt_event));
  rt->splines = (wz3__rt_dspline *)WZ3_MALLOC(((size_t)kx->n_splines + 1) * sizeof(wz3__rt_dspline));
  rt->spline_times = (float *)WZ3_MALLOC((total_keys + 1) * sizeof(float));
  rt->env.stack = (float *)WZ3_MALLOC(16 * 16 * sizeof(float));
  rt->remaining = (uint32_t *)WZ3_MALLOC(((size_t)kx->n_ops + 1) * sizeof(uint32_t));
  rt->fj.ipp_node = (int32_t *)WZ3_MALLOC(((size_t)kx->n_ops + 1) * sizeof(int32_t));
  if (!rt->ops || !rt->words || !rt->events || !rt->splines || !rt->spline_times || !rt->env.stack || !rt->remaining ||
      !rt->fj.ipp_node) {
    *result = WZ3_ERR_OUT_OF_MEMORY;
    wz3_rt_destroy(rt);
    return NULL;
  }
  memset(rt->ops, 0, ((size_t)kx->n_ops + 1) * sizeof(wz3__rt_op));
  memset(rt->words, 0, (total_words * 2 + 1) * sizeof(wz3_word));
  memset(rt->events, 0, ((size_t)kx->n_events + 1) * sizeof(wz3__rt_event));
  rt->n_ops = kx->n_ops;
  rt->n_events = kx->n_events;
  rt->n_splines = kx->n_splines;
  rt->env.cap_stack = 16;

  wz3_word *pool = rt->words;
  for (uint32_t i = 0; i < kx->n_ops; i++) {
    const wz3_kx_op *src = &kx->ops[i];
    const wz3_kx_class *cls = &kx->classes[src->class_index];
    wz3__rt_op *op = &rt->ops[i];
    op->id = i;
    op->convention = cls->convention;
    op->class_id = src->class_id;
    op->input_slots = (uint8_t)((cls->convention & 0x00000f00u) >> 8);
    op->link_slots = (uint8_t)((cls->convention & 0x0000f000u) >> 12);
    op->data_words = (uint8_t)(cls->convention & 0xffu);
    const wz3_class_info *info = wz3_class_lookup(src->class_id);
    op->out_class = info ? info->output_class : (uint8_t)WZ3_KC_ANY;
    op->src = src;
    op->cls = cls;
    op->edit = pool; pool += op->data_words;
    op->anim = pool; pool += op->data_words;
    const uint32_t copy = cls->packing_length < op->data_words ? cls->packing_length : op->data_words;
    for (uint32_t w = 0; w < copy; w++) op->edit[w] = src->params[w];
    memcpy(op->anim, op->edit, (size_t)op->data_words * sizeof(wz3_word));
    op->changed = 1;
    op->first_event = -1;
  }
  for (uint32_t i = 0; i < kx->n_events; i++) {
    const wz3_kx_event *src = &kx->events[i];
    wz3__rt_event *e = &rt->events[i];
    e->id = i;
    e->op = src->operation;
    e->next_op = -1;
    e->start = src->start;
    e->end = src->end;
    e->velocity = src->velocity;
    e->modulation = src->modulation;
    e->select = src->select;
    for (int k = 0; k < 3; k++) { e->scale[k] = src->scale[k]; e->rotate[k] = src->rotate[k]; e->translate[k] = src->translate[k]; }
    e->color = src->color;
    e->spline = src->spline;
    e->start_interval = src->start_interval;
    e->end_interval = src->end_interval;
    e->flags = src->flags;
    wz3_mat4_identity(e->matrix);
    if (e->op >= kx->n_ops) { *result = WZ3_ERR_FORMAT; wz3_rt_destroy(rt); return NULL; }
  }
  float *times = rt->spline_times;
  for (uint32_t i = 0; i < kx->n_splines; i++) {
    const wz3_kx_spline *src = &kx->splines[i];
    wz3__rt_dspline *s = &rt->splines[i];
    memset(s, 0, sizeof *s);
    s->interpolation = (int32_t)src->interpolation;
    s->n_channels = src->n_channels < 4 ? src->n_channels : 4;
    for (uint32_t c = 0; c < s->n_channels; c++) {
      s->channels[c].n = src->channels[c].n_keys;
      s->channels[c].time = times;
      s->channels[c].value = src->channels[c].key_value;
      for (uint32_t k = 0; k < src->channels[c].n_keys; k++) *times++ = (float)((double)src->channels[c].key_time[k] / 255.0);
    }
  }
  /* consumer counts of kdoc.cpp: inputs, links, roots and event targets */
  memset(rt->remaining, 0, ((size_t)kx->n_ops + 1) * sizeof(uint32_t));
  for (uint32_t i = 0; i < kx->n_ops; i++) {
    for (uint32_t k = 0; k < kx->ops[i].n_inputs; k++) if (kx->ops[i].inputs[k] < kx->n_ops) rt->remaining[kx->ops[i].inputs[k]]++;
    for (uint32_t k = 0; k < kx->ops[i].n_links; k++)
      if (kx->ops[i].links[k] >= 0 && (uint32_t)kx->ops[i].links[k] < kx->n_ops) rt->remaining[kx->ops[i].links[k]]++;
  }
  for (uint32_t i = 0; i < WZ3_MAX_OP_ROOT; i++) if (kx->roots[i] < kx->n_ops) rt->remaining[kx->roots[i]]++;
  for (uint32_t i = 0; i < kx->n_events; i++) rt->remaining[kx->events[i].operation]++;
  rt->env.depth = 1;
  wz3_mat4_identity(rt->env.stack);
  rt->fj.aspect = 1.0;
  for (int i = 0; i < 32; i++) wz3_mat4_identity(rt->fj.markers[i]);
  rt->env.chain = &rt->env.default_instances;
  rt->env.time_reset = 1;
  rt->env.event_spline = -1;
  rt->env.current_event = -1;
  *result = wz3__rt_init_frame(rt, 0, 0);
  if (*result != WZ3_OK) { wz3_rt_destroy(rt); return NULL; }
  return rt;
}

