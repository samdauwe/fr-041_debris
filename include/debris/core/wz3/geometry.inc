/* ======================================================================== */
/*  Render-ready geometry (engine.cpp, fed by the                           */
/*  geometry of genmesh.cpp and genminmesh.cpp)                             */
/*                                                                          */
/*  One allocation per geometry: vertex streams, a 32-bit index stream and  */
/*  the material groups. Producer-specific defaults of the reference port   */
/*  are applied here (a second UV channel an old Mesh does not have is the  */
/*  first one; colours are float for Mesh, bytes for MinMesh).              */
/* ======================================================================== */

typedef struct wz3__geom_layout {
  size_t positions, normals, tangents, uv0, uv1, colors, colors_u8, bone_weights, bone_indices, indices, groups, total;
} wz3__geom_layout;

static size_t wz3__geom_align(size_t v) { return (v + 15u) & ~(size_t)15u; }

static void wz3__geom_plan(wz3__geom_layout *l, size_t nv, size_t ni, size_t ng, int mesh_kind, int has_uv1)
{
  size_t o = 0;
  l->positions = o; o = wz3__geom_align(o + nv * 12);
  l->normals = o; o = wz3__geom_align(o + nv * 12);
  l->tangents = o; o = wz3__geom_align(o + nv * 16);
  l->uv0 = o; o = wz3__geom_align(o + nv * 8);
  l->uv1 = has_uv1 ? o : l->uv0;
  if (has_uv1) o = wz3__geom_align(o + nv * 8);
  l->colors = mesh_kind ? o : 0;
  if (mesh_kind) o = wz3__geom_align(o + nv * 16);
  l->colors_u8 = mesh_kind ? 0 : o;
  if (!mesh_kind) o = wz3__geom_align(o + nv * 4);
  l->bone_weights = mesh_kind ? 0 : o;
  if (!mesh_kind) o = wz3__geom_align(o + nv * 16);
  l->bone_indices = mesh_kind ? 0 : o;
  if (!mesh_kind) o = wz3__geom_align(o + nv * 8);
  l->indices = o; o = wz3__geom_align(o + ni * 4);
  l->groups = o; o = wz3__geom_align(o + ng * sizeof(wz3_geometry_group));
  l->total = o;
}

static wz3_result wz3__geom_init(wz3_geometry *g, size_t nv, size_t ni, size_t ng, int mesh_kind, int has_uv1)
{
  wz3__geom_layout l;
  memset(g, 0, sizeof *g);
  wz3__geom_plan(&l, nv, ni, ng, mesh_kind, has_uv1);
  uint8_t *block = (uint8_t *)WZ3_MALLOC(l.total ? l.total : 16);
  if (!block) return WZ3_ERR_OUT_OF_MEMORY;
  memset(block, 0, l.total ? l.total : 16);
  g->block_ = block;
  g->block_bytes_ = l.total ? l.total : 16;
  g->n_vertices = (uint32_t)nv; g->n_indices = (uint32_t)ni; g->n_groups = (uint32_t)ng;
  g->positions = (float *)(block + l.positions);
  g->normals = (float *)(block + l.normals);
  g->tangents = (float *)(block + l.tangents);
  g->uv0 = (float *)(block + l.uv0);
  g->uv1 = (float *)(block + l.uv1);
  g->colors = mesh_kind ? (float *)(block + l.colors) : NULL;
  g->colors_u8 = mesh_kind ? NULL : block + l.colors_u8;
  g->bone_weights = mesh_kind ? NULL : (float *)(block + l.bone_weights);
  g->bone_indices = mesh_kind ? NULL : (uint16_t *)(block + l.bone_indices);
  g->indices = (uint32_t *)(block + l.indices);
  g->groups = (wz3_geometry_group *)(block + l.groups);
  return WZ3_OK;
}

WZ3_API void wz3_geometry_free(wz3_geometry *g)
{
  if (!g) return;
  WZ3_FREE(g->block_);
  memset(g, 0, sizeof *g);
}

/* number of corners of a face, or 0 for a broken ring */
static uint32_t wz3__geom_face_corners(const wz3_mesh *m, int32_t start, uint32_t guard)
{
  uint32_t n = 0;
  int32_t h = start;
  do {
    n++;
    h = wz3__mh_next_face(m, h);
  } while (h != start && n <= guard);
  return h == start ? n : 0;
}

/* indexedGeometryBounds per group; a single group takes the whole-mesh box (normalizePreparedGeometry) */
WZ3_API void wz3_geometry_group_bounds(wz3_geometry *g)
{
  for (uint32_t gi = 0; gi < g->n_groups; gi++) {
    wz3_geometry_group *group = &g->groups[gi];
    if (g->n_groups == 1) {
      group->has_bounds = 1;
      memcpy(group->bounds_min, g->bounds_min, sizeof group->bounds_min);
      memcpy(group->bounds_max, g->bounds_max, sizeof group->bounds_max);
      continue;
    }
    float mn[3] = {INFINITY, INFINITY, INFINITY}, mx[3] = {-INFINITY, -INFINITY, -INFINITY};
    const uint64_t end64 = (uint64_t)group->start + group->count;
    const uint32_t end = end64 < g->n_indices ? (uint32_t)end64 : g->n_indices;
    for (uint32_t offset = group->start; offset < end; offset++) {
      const uint32_t vertex = g->indices[offset];
      if (vertex >= g->n_vertices) continue;
      const float *p = g->positions + (size_t)vertex * 3;
      if (!wz3__finite((double)p[0]) || !wz3__finite((double)p[1]) || !wz3__finite((double)p[2])) continue;
      for (int axis = 0; axis < 3; axis++) {
        if (p[axis] < mn[axis]) mn[axis] = p[axis];
        if (p[axis] > mx[axis]) mx[axis] = p[axis];
      }
    }
    group->has_bounds = wz3__finite((double)mn[0]) ? 1 : 0;
    memcpy(group->bounds_min, mn, sizeof mn);
    memcpy(group->bounds_max, mx, sizeof mx);
    if (!group->has_bounds) { memset(group->bounds_min, 0, sizeof group->bounds_min); memset(group->bounds_max, 0, sizeof group->bounds_max); }
  }
}

WZ3_API wz3_result wz3_mesh_prepare(const wz3_mesh *source, wz3_geometry *out)
{
  if (!source || !out) return WZ3_ERR_ARGUMENT;
  memset(out, 0, sizeof *out);
  wz3_mesh *owned = NULL;
  const wz3_mesh *m = source;
  if (!m->got_normals) {                            /* Mesh.prepare calls needNormals(): do it on a copy */
    owned = wz3__mesh_clone(source);
    if (!owned) return WZ3_ERR_OUT_OF_MEMORY;
    if (wz3__mesh_calc_normals(owned)) { wz3__mesh_free(owned); return WZ3_ERR_OUT_OF_MEMORY; }
    m = owned;
  }
  const uint32_t nv = m->n_verts;
  const uint32_t ring_guard = m->n_edges * 2u + 2u;
  /* first pass: triangles per material slot, in order of first appearance */
  uint32_t *tri_count = (uint32_t *)WZ3_MALLOC(((size_t)m->n_slots + 1) * sizeof(uint32_t));
  int32_t *order = (int32_t *)WZ3_MALLOC(((size_t)m->n_slots + 1) * sizeof(int32_t));
  if (!tri_count || !order) { WZ3_FREE(tri_count); WZ3_FREE(order); wz3__mesh_free(owned); return WZ3_ERR_OUT_OF_MEMORY; }
  memset(tri_count, 0, ((size_t)m->n_slots + 1) * sizeof(uint32_t));
  uint32_t n_order = 0;
  size_t total_tris = 0;
  for (uint32_t f = 0; f < m->n_faces; f++) {
    const wz3__mface *face = &m->faces[f];
    if (!face->material || face->edge < 0 || face->material < 0 || (uint32_t)face->material >= m->n_slots) continue;
    const uint32_t corners = wz3__geom_face_corners(m, face->edge, ring_guard);
    if (corners < 3) continue;
    if (!tri_count[face->material]) order[n_order++] = face->material;
    tri_count[face->material] += corners - 2;
    total_tris += corners - 2;
  }
  wz3_result res = wz3__geom_init(out, nv, total_tris * 3, n_order, 1, 0);
  if (res != WZ3_OK) { WZ3_FREE(tri_count); WZ3_FREE(order); wz3__mesh_free(owned); return res; }
  for (uint32_t i = 0; i < nv; i++) {
    const wz3__mvert *v = &m->verts[i];
    const int32_t first = v->first;
    const wz3__mvert *p = (first >= 0 && (uint32_t)first < nv) ? &m->verts[first] : v;
    memcpy(out->positions + i * 3, p->v[0], 3 * sizeof(float));
    memcpy(out->normals + i * 3, v->v[1], 3 * sizeof(float));
    memcpy(out->tangents + i * 4, v->v[2], 4 * sizeof(float));
    memcpy(out->colors + i * 4, v->v[3], 4 * sizeof(float));
    memcpy(out->uv0 + i * 2, v->v[4], 2 * sizeof(float));
  }
  /* uv1 aliases uv0 (see header); a second pass lays the groups out and fills the indices */
  uint32_t *cursor = (uint32_t *)WZ3_MALLOC(((size_t)m->n_slots + 1) * sizeof(uint32_t));
  if (!cursor) { wz3_geometry_free(out); WZ3_FREE(tri_count); WZ3_FREE(order); wz3__mesh_free(owned); return WZ3_ERR_OUT_OF_MEMORY; }
  uint32_t start = 0;
  for (uint32_t g = 0; g < n_order; g++) {
    const int32_t slot = order[g];
    cursor[slot] = start;
    const wz3__mslot *s = (uint32_t)slot < m->n_slots ? &m->slots[slot] : &m->slots[0];
    out->groups[g].slot = slot;
    out->groups[g].pass = s->pass;
    out->groups[g].material = s->material;
    out->groups[g].start = start;
    out->groups[g].count = tri_count[slot] * 3;
    start += tri_count[slot] * 3;
  }
  for (uint32_t f = 0; f < m->n_faces; f++) {
    const wz3__mface *face = &m->faces[f];
    if (!face->material || face->edge < 0 || face->material < 0 || (uint32_t)face->material >= m->n_slots) continue;
    int32_t ids_first = -1, previous = -1;
    uint32_t corners = 0;
    int32_t h = face->edge;
    do {
      const int32_t id = wz3__mh_vert(m, h);
      if (corners == 0) ids_first = id;
      else if (corners >= 2) {
        uint32_t *dst = out->indices + cursor[face->material];
        dst[0] = (uint32_t)ids_first; dst[1] = (uint32_t)previous; dst[2] = (uint32_t)id;
        cursor[face->material] += 3;
      }
      previous = id;
      corners++;
      h = wz3__mh_next_face(m, h);
    } while (h != face->edge && corners <= ring_guard);
  }
  /* bounds(true) of the producing mesh */
  res = wz3__mesh_bounds(m, out->bounds_min, out->bounds_max) ? WZ3_ERR_OUT_OF_MEMORY : WZ3_OK;
  out->uv1_is_uv0 = 1;
  if (res == WZ3_OK) wz3_geometry_group_bounds(out);
  WZ3_FREE(cursor);
  WZ3_FREE(tri_count);
  WZ3_FREE(order);
  wz3__mesh_free(owned);
  if (res != WZ3_OK) wz3_geometry_free(out);
  return res;
}

WZ3_API wz3_minmesh *wz3_minmesh_retain(const wz3_minmesh *mesh)
{
  if (!mesh) return NULL;
  wz3_minmesh *copy = wz3__mm_clone(mesh);
  if (copy && !copy->normals_valid) wz3__mm_calc_normals(copy);
  return copy;
}

WZ3_API void wz3_minmesh_release(wz3_minmesh *mesh) { wz3__mm_free(mesh); }

WZ3_API wz3_result wz3_minmesh_prepare(const wz3_minmesh *source, wz3_geometry *out)
{
  if (!source || !out) return WZ3_ERR_ARGUMENT;
  memset(out, 0, sizeof *out);
  wz3_minmesh *owned = NULL;
  const wz3_minmesh *m = source;
  if (!m->normals_valid) {                          /* MinMesh.prepare calls calcNormals() */
    owned = wz3__mm_clone(source);
    if (!owned) return WZ3_ERR_OUT_OF_MEMORY;
    wz3__mm_calc_normals(owned);
    m = owned;
  }
  const uint32_t nv = m->n_verts, nc = m->n_clusters;
  uint32_t *tri_count = (uint32_t *)WZ3_MALLOC(((size_t)nc + 1) * 2 * sizeof(uint32_t));
  if (!tri_count) { wz3__mm_free(owned); return WZ3_ERR_OUT_OF_MEMORY; }
  uint32_t *cursor = tri_count + nc + 1;
  memset(tri_count, 0, ((size_t)nc + 1) * 2 * sizeof(uint32_t));
  size_t total_tris = 0;
  uint32_t n_groups = 0;
  for (uint32_t f = 0; f < m->n_faces; f++) {
    const wz3__mm_face *face = &m->faces[f];
    if (face->cluster <= 0 || (uint32_t)face->cluster >= nc || face->count < 3) continue;
    if (!tri_count[face->cluster]) n_groups++;
    tri_count[face->cluster] += (uint32_t)face->count - 2;
    total_tris += (uint32_t)face->count - 2;
  }
  wz3_result res = wz3__geom_init(out, nv, total_tris * 3, n_groups, 0, 1);
  if (res != WZ3_OK) { WZ3_FREE(tri_count); wz3__mm_free(owned); return res; }
  for (uint32_t i = 0; i < nv; i++) {
    const wz3__mm_vertex *v = &m->verts[i];
    memcpy(out->positions + i * 3, v->position, 3 * sizeof(float));
    memcpy(out->normals + i * 3, v->normal, 3 * sizeof(float));
    memcpy(out->tangents + i * 4, v->tangent, 3 * sizeof(float));
    out->tangents[i * 4 + 3] = 1.0f;
    memcpy(out->uv0 + i * 2, v->uv[0], 2 * sizeof(float));
    memcpy(out->uv1 + i * 2, v->uv[1], 2 * sizeof(float));
    out->colors_u8[i * 4] = (uint8_t)((v->color >> 16) & 255u);
    out->colors_u8[i * 4 + 1] = (uint8_t)((v->color >> 8) & 255u);
    out->colors_u8[i * 4 + 2] = (uint8_t)(v->color & 255u);
    out->colors_u8[i * 4 + 3] = (uint8_t)(v->color >> 24);
    memcpy(out->bone_weights + i * 4, v->weights, 4 * sizeof(float));
    memcpy(out->bone_indices + i * 4, v->matrices, 4 * sizeof(uint16_t));
  }
  uint32_t start = 0, g = 0;
  for (uint32_t c = 1; c < nc; c++) {
    if (!tri_count[c]) continue;
    cursor[c] = start;
    out->groups[g].slot = (int32_t)c;
    out->groups[g].pass = m->clusters[c].render_pass;
    out->groups[g].material = m->clusters[c].material;
    out->groups[g].start = start;
    out->groups[g].count = tri_count[c] * 3;
    start += tri_count[c] * 3;
    g++;
  }
  for (uint32_t f = 0; f < m->n_faces; f++) {
    const wz3__mm_face *face = &m->faces[f];
    if (face->cluster <= 0 || (uint32_t)face->cluster >= nc || face->count < 3) continue;
    const int32_t *ring = m->corners + face->first;
    for (int32_t corner = 2; corner < face->count; corner++) {
      uint32_t *dst = out->indices + cursor[face->cluster];
      dst[0] = (uint32_t)ring[0]; dst[1] = (uint32_t)ring[corner - 1]; dst[2] = (uint32_t)ring[corner];
      cursor[face->cluster] += 3;
    }
  }
  wz3__mm_bounds(m, out->bounds_min, out->bounds_max);
  out->animated = m->n_bones > 0;
  wz3_geometry_group_bounds(out);
  WZ3_FREE(tri_count);
  wz3__mm_free(owned);
  return WZ3_OK;
}

/* One FNV-1a word stream over what the renderer consumes; the reference fixtures fold the same words. */
WZ3_API uint32_t wz3_geometry_hash(const wz3_geometry *g)
{
  uint32_t h = 0x811c9dc5u;
#define WZ3__GH(w) (h = (h ^ (uint32_t)(w)) * 0x01000193u)
#define WZ3__GHF(f) do { uint32_t b_; memcpy(&b_, &(f), 4); if (b_ << 1 > 0xff000000u) b_ = 0x7fc00000u; WZ3__GH(b_); } while (0)
  const uint32_t nv = g->n_vertices;
  WZ3__GH(nv); WZ3__GH(g->n_indices); WZ3__GH(g->n_groups);
  for (uint32_t i = 0; i < nv * 3; i++) WZ3__GHF(g->positions[i]);
  for (uint32_t i = 0; i < nv * 3; i++) WZ3__GHF(g->normals[i]);
  for (uint32_t i = 0; i < nv * 4; i++) WZ3__GHF(g->tangents[i]);
  for (uint32_t i = 0; i < nv * 2; i++) WZ3__GHF(g->uv0[i]);
  for (uint32_t i = 0; i < nv * 2; i++) WZ3__GHF(g->uv1[i]);
  if (g->colors) { WZ3__GH(0); for (uint32_t i = 0; i < nv * 4; i++) WZ3__GHF(g->colors[i]); }
  else { WZ3__GH(1); for (uint32_t i = 0; i < nv * 4; i++) WZ3__GH(g->colors_u8[i]); }
  if (g->bone_weights) {
    WZ3__GH(1);
    for (uint32_t i = 0; i < nv * 4; i++) WZ3__GHF(g->bone_weights[i]);
    for (uint32_t i = 0; i < nv * 4; i++) WZ3__GH(g->bone_indices[i]);
  } else WZ3__GH(0);
  for (uint32_t i = 0; i < g->n_indices; i++) WZ3__GH(g->indices[i]);
  for (uint32_t i = 0; i < g->n_groups; i++) {
    WZ3__GH(g->groups[i].slot); WZ3__GH(g->groups[i].pass); WZ3__GH(g->groups[i].start); WZ3__GH(g->groups[i].count);
  }
  for (int i = 0; i < 3; i++) WZ3__GHF(g->bounds_min[i]);
  for (int i = 0; i < 3; i++) WZ3__GHF(g->bounds_max[i]);
#undef WZ3__GH
#undef WZ3__GHF
  return h;
}

WZ3_API size_t wz3_geometry_bytes(const wz3_geometry *g)
{
  return (g && g->block_) ? g->block_bytes_ : 0;
}

static int wz3__is_geometry_class(uint16_t id)
{
  const wz3_class_info *info = wz3_class_lookup(id);
  return info && (info->output_class == WZ3_KC_MESH || info->output_class == WZ3_KC_MINMESH);
}

WZ3_API uint32_t wz3_kx_scene_geometry(const wz3_kx *kx, uint8_t *marks)
{
  if (!kx || !marks) return 0;
  memset(marks, 0, kx->n_ops);
  uint32_t count = 0;
  for (uint32_t i = 0; i < kx->n_ops; i++) {
    const wz3_kx_op *op = &kx->ops[i];
    if (wz3__is_geometry_class(op->class_id)) continue;
    for (uint32_t k = 0; k < op->n_inputs; k++) {
      const uint32_t in = op->inputs[k];
      if (in < kx->n_ops && !marks[in] && wz3__is_geometry_class(kx->ops[in].class_id)) { marks[in] = 1; count++; }
    }
  }
  return count;
}
