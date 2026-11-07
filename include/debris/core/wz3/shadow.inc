/* ======================================================================== */
/*  Shadow volumes (engine.cpp; shadow recipes of genmesh.cpp               */
/*  and genminmesh.cpp)                                                     */
/*                                                                          */
/*  A shadow topology is the welded polygon/edge structure of a caster in   */
/*  one block; a shadow volume is the index list that extrudes its          */
/*  silhouette from a light (plus caps for z-fail). Float32 stores of the   */
/*  JavaScript port are (float) casts here.                                 */
/* ======================================================================== */

/* ---- growable vectors ---- */

typedef struct wz3__sv {
  uint8_t *p;
  size_t n, cap, es;
  int failed;
} wz3__sv;

static void wz3__sv_init(wz3__sv *v, size_t element_size) { memset(v, 0, sizeof *v); v->es = element_size; }
static void wz3__sv_free(wz3__sv *v) { WZ3_FREE(v->p); v->p = NULL; v->n = v->cap = 0; }
static void *wz3__sv_push(wz3__sv *v, const void *item)
{
  if (v->failed) return NULL;
  if (v->n == v->cap) {
    const size_t capacity = v->cap ? v->cap * 2 : 64;
    uint8_t *p = (uint8_t *)WZ3_MALLOC(capacity * v->es);
    if (!p) { v->failed = 1; return NULL; }
    if (v->n) memcpy(p, v->p, v->n * v->es);
    WZ3_FREE(v->p);
    v->p = p; v->cap = capacity;
  }
  uint8_t *slot = v->p + v->n * v->es;
  if (item) memcpy(slot, item, v->es); else memset(slot, 0, v->es);
  v->n++;
  return slot;
}
static void wz3__sv_u32(wz3__sv *v, uint32_t x) { wz3__sv_push(v, &x); }
static void wz3__sv_f32(wz3__sv *v, float x) { wz3__sv_push(v, &x); }

/* open-addressing map from a 64-bit non-zero key to an int32 (Map of the JavaScript port) */
typedef struct wz3__hm { uint64_t *keys; int32_t *vals; uint32_t cap; int failed; } wz3__hm;

static void wz3__hm_init(wz3__hm *m, uint32_t expected)
{
  uint32_t cap = 16;
  while (cap < expected * 2u + 2u) cap <<= 1;
  m->keys = (uint64_t *)WZ3_MALLOC((size_t)cap * sizeof *m->keys);
  m->vals = (int32_t *)WZ3_MALLOC((size_t)cap * sizeof *m->vals);
  m->cap = cap;
  m->failed = !m->keys || !m->vals;
  if (!m->failed) memset(m->keys, 0, (size_t)cap * sizeof *m->keys);
}
static void wz3__hm_free(wz3__hm *m) { WZ3_FREE(m->keys); WZ3_FREE(m->vals); m->keys = NULL; m->vals = NULL; }
static uint32_t wz3__hm_hash(uint64_t k) { k ^= k >> 33; k *= 0xff51afd7ed558ccdull; k ^= k >> 33; k *= 0xc4ceb9fe1a85ec53ull; k ^= k >> 33; return (uint32_t)k; }
/* slot of `key`: either holding it or the empty slot where it belongs */
static uint32_t wz3__hm_find(const wz3__hm *m, uint64_t key)
{
  uint32_t at = wz3__hm_hash(key) & (m->cap - 1u);
  while (m->keys[at] && m->keys[at] != key) at = (at + 1u) & (m->cap - 1u);
  return at;
}

/* ---- the topology block ---- */

static size_t wz3__sh_align(size_t v) { return (v + 7u) & ~(size_t)7u; }

typedef struct wz3__sh_input {
  uint32_t n_vertices, n_triangles, n_polygons, n_edges;
  const float *positions;            /* canonical, 3 per vertex */
  const uint32_t *source_indices, *faces, *triangle_polygon;
  const float *polygon_planes;       /* 4 per polygon */
  const wz3_shadow_edge *edges;
} wz3__sh_input;

/* copies the vectors into one allocation and derives the volume vertices (each canonical vertex twice: base, extruded) */
static wz3_result wz3__sh_finish(wz3_shadow_topology *t, const wz3__sh_input *in)
{
  const size_t nv = in->n_vertices, nt = in->n_triangles, np = in->n_polygons, ne = in->n_edges;
  size_t o = 0;
  const size_t o_pos = o;      o = wz3__sh_align(o + nv * 12);
  const size_t o_vol = o;      o = wz3__sh_align(o + nv * 24);
  const size_t o_planes = o;   o = wz3__sh_align(o + np * 16);
  const size_t o_ext = o;      o = wz3__sh_align(o + nv * 2);
  const size_t o_src = o;      o = wz3__sh_align(o + nv * 4);
  const size_t o_faces = o;    o = wz3__sh_align(o + nt * 12);
  const size_t o_tp = o;       o = wz3__sh_align(o + nt * 4);
  const size_t o_edges = o;    o = wz3__sh_align(o + ne * sizeof(wz3_shadow_edge));
  uint8_t *block = (uint8_t *)WZ3_MALLOC(o ? o : 8);
  if (!block) return WZ3_ERR_OUT_OF_MEMORY;
  memset(block, 0, o ? o : 8);
  t->block_ = block; t->block_bytes_ = o ? o : 8;
  t->n_vertices = (uint32_t)nv; t->n_triangles = (uint32_t)nt; t->n_polygons = (uint32_t)np; t->n_edges = (uint32_t)ne;
  t->positions = (float *)(block + o_pos); t->volume_positions = (float *)(block + o_vol);
  t->polygon_planes = (float *)(block + o_planes); t->extrusions = block + o_ext;
  t->source_indices = (uint32_t *)(block + o_src); t->faces = (uint32_t *)(block + o_faces);
  t->triangle_polygon = (uint32_t *)(block + o_tp); t->edges = (wz3_shadow_edge *)(block + o_edges);
  if (nv) { memcpy(t->positions, in->positions, nv * 12); memcpy(t->source_indices, in->source_indices, nv * 4); }
  if (np) memcpy(t->polygon_planes, in->polygon_planes, np * 16);
  if (nt) { memcpy(t->faces, in->faces, nt * 12); memcpy(t->triangle_polygon, in->triangle_polygon, nt * 4); }
  if (ne) memcpy(t->edges, in->edges, ne * sizeof(wz3_shadow_edge));
  for (size_t i = 0; i < nv; i++) {
    const float *p = t->positions + i * 3;
    float *v = t->volume_positions + i * 6;
    v[0] = v[3] = p[0]; v[1] = v[4] = p[1]; v[2] = v[5] = p[2];
    t->extrusions[i * 2 + 1] = 1;
  }
  return WZ3_OK;
}

WZ3_API void wz3_shadow_topology_free(wz3_shadow_topology *t)
{
  if (!t) return;
  WZ3_FREE(t->block_);
  memset(t, 0, sizeof *t);
}

/* refreshAnimatedShadowPlanes: one plane per polygon from its first current triangle; 0 when the face data is inconsistent */
static int wz3__sh_refresh_planes(wz3_shadow_topology *t)
{
  if (!t->faces || !t->triangle_polygon || !t->polygon_planes) return 0;
  memset(t->polygon_planes, 0, (size_t)t->n_polygons * 16);
  int64_t previous = -1;
  const float *c = t->positions;
  for (uint32_t tri = 0; tri < t->n_triangles; tri++) {
    const uint32_t polygon = t->triangle_polygon[tri];
    if ((int64_t)polygon == previous) continue;
    if (polygon >= t->n_polygons) return 0;
    previous = polygon;
    const uint32_t ia = t->faces[tri * 3] * 3u, ib = t->faces[tri * 3 + 1] * 3u, ic = t->faces[tri * 3 + 2] * 3u;
    const float d1x = (float)((double)c[ib] - c[ia]), d1y = (float)((double)c[ib + 1] - c[ia + 1]), d1z = (float)((double)c[ib + 2] - c[ia + 2]);
    const float d2x = (float)((double)c[ic] - c[ia]), d2y = (float)((double)c[ic + 1] - c[ia + 1]), d2z = (float)((double)c[ic + 2] - c[ia + 2]);
    const float nx = (float)((double)(float)((double)d1y * d2z) - (double)(float)((double)d1z * d2y));
    const float ny = (float)((double)(float)((double)d1z * d2x) - (double)(float)((double)d1x * d2z));
    const float nz = (float)((double)(float)((double)d1x * d2y) - (double)(float)((double)d1y * d2x));
    float *plane = t->polygon_planes + (size_t)polygon * 4;
    plane[0] = nx; plane[1] = ny; plane[2] = nz;
    const float a = (float)((double)c[ia] * nx), b = (float)((double)c[ia + 1] * ny), cc = (float)((double)c[ia + 2] * nz);
    plane[3] = (float)-(double)(float)((double)(float)((double)a + b) + cc);
  }
  return 1;
}

WZ3_API int wz3_shadow_topology_refresh_planes(wz3_shadow_topology *t) { return wz3__sh_refresh_planes(t); }

/* refreshShadowTopologyPositions */
WZ3_API int wz3_shadow_topology_refresh_positions(wz3_shadow_topology *t, const float *positions, uint32_t n_vertices)
{
  if (!t || !t->block_) return 0;
  for (uint32_t i = 0; i < t->n_vertices; i++) {
    const uint32_t source = t->source_indices[i];
    if (source >= n_vertices) return 0;
    const float *p = positions + (size_t)source * 3;
    float *c = t->positions + (size_t)i * 3, *v = t->volume_positions + (size_t)i * 6;
    c[0] = p[0]; c[1] = p[1]; c[2] = p[2];
    v[0] = v[3] = p[0]; v[1] = v[4] = p[1]; v[2] = v[5] = p[2];
  }
  return wz3__sh_refresh_planes(t);
}

WZ3_API int wz3_shadow_group_structure_matches(const wz3_geometry_group *a, uint32_t na, const wz3_geometry_group *b, uint32_t nb)
{
  if (na != nb) return 0;
  for (uint32_t i = 0; i < na; i++)
    if (a[i].material != b[i].material || a[i].slot != b[i].slot || a[i].pass != b[i].pass || a[i].start != b[i].start || a[i].count != b[i].count) return 0;
  return 1;
}

WZ3_API int wz3_shadow_topology_reuse(wz3_shadow_topology *t, int streams_identical, int positions_changed, const float *positions, uint32_t n_vertices)
{
  if (!streams_identical) return WZ3_SHADOW_DROP;
  if (!positions_changed) return WZ3_SHADOW_KEEP;
  /* position-derived welding can split or merge as vertices move: only an authored stable map proves the structure holds */
  if (!t->uses_shadow_vertex_map || !wz3_shadow_topology_refresh_positions(t, positions, n_vertices)) return WZ3_SHADOW_DROP;
  return WZ3_SHADOW_REFRESHED;
}

/* ---- native topology (prepareNativeShadowTopology) ---- */

static wz3_result wz3__sh_native(const wz3_geometry *g, const wz3_shadow_recipe *r, int positions_dynamic, wz3_shadow_topology *t)
{
  const size_t nv = r->n_source;
  float *canonical = (float *)WZ3_MALLOC((nv ? nv : 1) * 12);
  wz3_shadow_edge *edges = (wz3_shadow_edge *)WZ3_MALLOC((r->n_edges ? r->n_edges : 1) * sizeof *edges);
  if (!canonical || !edges) { WZ3_FREE(canonical); WZ3_FREE(edges); return WZ3_ERR_OUT_OF_MEMORY; }
  for (size_t i = 0; i < nv; i++) {
    if (r->source_indices[i] >= g->n_vertices) { WZ3_FREE(canonical); WZ3_FREE(edges); return WZ3_ERR_ARGUMENT; }
    memcpy(canonical + i * 3, g->positions + (size_t)r->source_indices[i] * 3, 12);
  }
  for (size_t e = 0; e < r->n_edges; e++) {
    wz3_shadow_edge *edge = &edges[e];
    edge->a0 = r->edge_vertices[e * 2]; edge->b0 = r->edge_vertices[e * 2 + 1]; edge->f0 = r->edge_planes[e * 2];
    edge->a1 = edge->b0; edge->b1 = edge->a0; edge->f1 = r->edge_planes[e * 2 + 1];
    edge->count = 2;
  }
  wz3__sh_input in;
  in.n_vertices = r->n_source; in.n_triangles = r->n_triangles; in.n_polygons = r->n_planes; in.n_edges = r->n_edges;
  in.positions = canonical; in.source_indices = r->source_indices; in.faces = r->faces; in.triangle_polygon = r->triangle_planes;
  in.polygon_planes = r->planes; in.edges = edges;
  wz3_result res = wz3__sh_finish(t, &in);
  WZ3_FREE(canonical); WZ3_FREE(edges);
  if (res != WZ3_OK) return res;
  t->native = 1;
  t->uses_shadow_vertex_map = 1;
  t->max_edge_incidence = r->n_edges ? 2u : 0u;
  t->source_non_manifold_edges = r->source_non_manifold_edges;
  t->trailing_plane_slots = r->trailing_plane_slots;
  if (positions_dynamic && !wz3__sh_refresh_planes(t)) { wz3_shadow_topology_free(t); return WZ3_ERR_ARGUMENT; }
  return WZ3_OK;
}

/* ---- generic topology (prepareShadowTopology without a recipe) ---- */

static uint32_t wz3__sh_keybits(float f)
{
  uint32_t u;
  memcpy(&u, &f, 4);
  if (f != f) return 0x7fc00000u;
  return u == 0x80000000u ? 0u : u;       /* weldKey writes -0 as 0 and every NaN alike */
}

static wz3_result wz3__sh_generic(const wz3_geometry *g, const wz3_shadow_data *d, const uint32_t *ranges, uint32_t n_ranges,
                                  int positions_dynamic, wz3_shadow_topology *t)
{
  const uint32_t nv = g->n_vertices;
  const uint32_t *vmap = d ? d->vertex_map : NULL;
  int uses_map = vmap != NULL;
  wz3__sv canonical, sources, faces, tri_polygon, ring, ring_store, polygon_off, polygon_len, polygon_source, edges_v;
  wz3__sv_init(&canonical, 4); wz3__sv_init(&sources, 4); wz3__sv_init(&faces, 4); wz3__sv_init(&tri_polygon, 4);
  wz3__sv_init(&ring, 4); wz3__sv_init(&ring_store, 4); wz3__sv_init(&polygon_off, 4); wz3__sv_init(&polygon_len, 4);
  wz3__sv_init(&polygon_source, 4); wz3__sv_init(&edges_v, sizeof(wz3_shadow_edge));
  int32_t *source_to_canonical = (int32_t *)WZ3_MALLOC(((size_t)nv + 1) * 4);
  wz3__hm by_index, by_position, by_edge;
  memset(&by_edge, 0, sizeof by_edge);
  wz3__hm_init(&by_index, nv); wz3__hm_init(&by_position, nv);
  wz3_result res = WZ3_OK;
  float *polygon_planes = NULL;
  uint32_t n_polygons = 0;
  if (!source_to_canonical || by_index.failed || by_position.failed) { res = WZ3_ERR_OUT_OF_MEMORY; goto done_early; }
  for (uint32_t i = 0; i < nv; i++) source_to_canonical[i] = -1;
  const uint32_t n_indices = g->n_indices;

  /* canonicalIndex() and the polygon rings, in the order the JavaScript port visits them */
  int64_t ring_source = -1;
  int have_ring = 0;
#define WZ3__SH_CANON(var, sourceIndex)                                                                   \
  do {                                                                                                    \
    const uint32_t s_ = (sourceIndex);                                                                    \
    if (s_ >= nv) { res = WZ3_ERR_ARGUMENT; goto done; }                                                  \
    int32_t c_ = source_to_canonical[s_];                                                                 \
    if (c_ < 0) {                                                                                         \
      int have_topology_ = vmap && s_ < nv;                                                               \
      if (!have_topology_) uses_map = 0;                                                                  \
      const float *p_ = g->positions + (size_t)s_ * 3;                                                    \
      uint32_t slot_;                                                                                     \
      int32_t found_ = -1;                                                                                \
      if (have_topology_) {                                                                               \
        slot_ = wz3__hm_find(&by_index, (uint64_t)vmap[s_] + 1u);                                         \
        if (by_index.keys[slot_]) found_ = by_index.vals[slot_];                                          \
      } else {                                                                                            \
        const uint64_t h_ = ((uint64_t)wz3__sh_keybits(p_[0]) * 0x9e3779b97f4a7c15ull) ^ ((uint64_t)wz3__sh_keybits(p_[1]) << 21) ^ ((uint64_t)wz3__sh_keybits(p_[2]) << 42) ^ 1ull; \
        uint32_t at_ = wz3__hm_hash(h_) & (by_position.cap - 1u);                                         \
        slot_ = at_;                                                                                      \
        while (by_position.keys[slot_]) {                                                                 \
          const int32_t cand_ = by_position.vals[slot_];                                                  \
          const float *q_ = (const float *)canonical.p + (size_t)cand_ * 3;                               \
          if (wz3__sh_keybits(q_[0]) == wz3__sh_keybits(p_[0]) && wz3__sh_keybits(q_[1]) == wz3__sh_keybits(p_[1]) && wz3__sh_keybits(q_[2]) == wz3__sh_keybits(p_[2])) { found_ = cand_; break; } \
          slot_ = (slot_ + 1u) & (by_position.cap - 1u);                                                  \
        }                                                                                                 \
        if (found_ < 0) by_position.keys[slot_] = h_ | 1ull;                                              \
      }                                                                                                   \
      if (found_ < 0) {                                                                                   \
        found_ = (int32_t)(canonical.n / 3);                                                              \
        wz3__sv_f32(&canonical, p_[0]); wz3__sv_f32(&canonical, p_[1]); wz3__sv_f32(&canonical, p_[2]);   \
        wz3__sv_u32(&sources, s_);                                                                        \
        if (have_topology_) { by_index.keys[slot_] = (uint64_t)vmap[s_] + 1u; by_index.vals[slot_] = found_; } \
        else by_position.vals[slot_] = found_;                                                            \
        if (canonical.failed || sources.failed) { res = WZ3_ERR_OUT_OF_MEMORY; goto done; }               \
      }                                                                                                   \
      source_to_canonical[s_] = found_;                                                                   \
      c_ = found_;                                                                                        \
    }                                                                                                     \
    (var) = (uint32_t)c_;                                                                                 \
  } while (0)
#define WZ3__SH_FLUSH()                                                                                   \
  do {                                                                                                    \
    if (have_ring) {                                                                                      \
      /* collapse vertices the weld made identical; a polygon that degenerates below a triangle keeps its slot */ \
      const uint32_t *r_ = (const uint32_t *)ring.p;                                                      \
      const uint32_t start_ = (uint32_t)(ring_store.n);                                                   \
      for (size_t k_ = 0; k_ < ring.n; k_++) {                                                            \
        if (ring_store.n == start_ || ((const uint32_t *)ring_store.p)[ring_store.n - 1] != r_[k_]) wz3__sv_u32(&ring_store, r_[k_]); \
      }                                                                                                   \
      while (ring_store.n - start_ > 1 && ((const uint32_t *)ring_store.p)[start_] == ((const uint32_t *)ring_store.p)[ring_store.n - 1]) ring_store.n--; \
      uint32_t len_ = (uint32_t)(ring_store.n - start_);                                                  \
      if (len_ < 3) { ring_store.n = start_; len_ = 0; }                                                  \
      wz3__sv_u32(&polygon_off, start_); wz3__sv_u32(&polygon_len, len_); wz3__sv_u32(&polygon_source, (uint32_t)ring_source); \
      ring.n = 0; have_ring = 0;                                                                          \
      if (ring_store.failed || polygon_off.failed || polygon_len.failed || polygon_source.failed) { res = WZ3_ERR_OUT_OF_MEMORY; goto done; } \
    }                                                                                                     \
  } while (0)

  for (uint32_t gi = 0; gi < n_ranges; gi++) {
    const uint64_t end64 = (uint64_t)ranges[gi * 2] + ranges[gi * 2 + 1];
    const uint32_t end = end64 < n_indices ? (uint32_t)end64 : n_indices;
    for (uint32_t cursor = ranges[gi * 2]; (uint64_t)cursor + 2 < end; cursor += 3) {
      const uint32_t triangle = cursor / 3u;
      if (d && d->triangle_mask && (triangle >= d->n_triangles || !d->triangle_mask[triangle])) continue;
      uint32_t a, b, c;
      WZ3__SH_CANON(a, g->indices[cursor]);
      WZ3__SH_CANON(b, g->indices[cursor + 1]);
      WZ3__SH_CANON(c, g->indices[cursor + 2]);
      const int64_t source = (d && d->face_map && triangle < d->n_triangles) ? (int64_t)d->face_map[triangle] : (int64_t)triangle;
      if (!have_ring || source != ring_source) {
        WZ3__SH_FLUSH();
        ring_source = source;
        ring.n = 0;
        wz3__sv_u32(&ring, a); wz3__sv_u32(&ring, b); wz3__sv_u32(&ring, c);
        have_ring = 1;
      } else wz3__sv_u32(&ring, c);
      if (ring.failed) { res = WZ3_ERR_OUT_OF_MEMORY; goto done; }
      if (a != b && b != c && c != a) {
        wz3__sv_u32(&faces, a); wz3__sv_u32(&faces, b); wz3__sv_u32(&faces, c);
        wz3__sv_u32(&tri_polygon, (uint32_t)polygon_off.n);
        if (faces.failed || tri_polygon.failed) { res = WZ3_ERR_OUT_OF_MEMORY; goto done; }
      }
    }
  }
  WZ3__SH_FLUSH();
#undef WZ3__SH_CANON
#undef WZ3__SH_FLUSH
  n_polygons = (uint32_t)polygon_off.n;
  const uint32_t vertex_count = (uint32_t)(canonical.n / 3);
  const float *pos = (const float *)canonical.p;
  const uint32_t *store = (const uint32_t *)ring_store.p;
  const uint32_t *poff = (const uint32_t *)polygon_off.p, *plen = (const uint32_t *)polygon_len.p, *psrc = (const uint32_t *)polygon_source.p;

  /* one edge per polygon perimeter segment; `face` names a polygon */
  wz3__hm_init(&by_edge, (uint32_t)ring_store.n + 4u);   /* one record per perimeter segment at most */
  if (by_edge.failed) { res = WZ3_ERR_OUT_OF_MEMORY; goto done; }
  for (uint32_t polygon = 0; polygon < n_polygons; polygon++) {
    const uint32_t len = plen[polygon];
    if (!len) continue;
    for (uint32_t k = 0; k < len; k++) {
      const uint32_t a = store[poff[polygon] + k], b = store[poff[polygon] + (k + 1) % len];
      const uint32_t lo = a < b ? a : b, hi_ = a < b ? b : a;
      const uint64_t key = (((uint64_t)lo << 32) | hi_) + 1u;
      const uint32_t slot = wz3__hm_find(&by_edge, key);
      if (!by_edge.keys[slot]) {
        wz3_shadow_edge fresh;
        memset(&fresh, 0, sizeof fresh);
        fresh.a0 = a; fresh.b0 = b; fresh.f0 = polygon; fresh.count = 1;
        if (!wz3__sv_push(&edges_v, &fresh)) { res = WZ3_ERR_OUT_OF_MEMORY; goto done; }
        by_edge.keys[slot] = key; by_edge.vals[slot] = (int32_t)(edges_v.n - 1);
      } else {
        wz3_shadow_edge *edge = &((wz3_shadow_edge *)edges_v.p)[by_edge.vals[slot]];
        if (edge->count == 1) { edge->a1 = a; edge->b1 = b; edge->f1 = polygon; }
        edge->count++;
      }
    }
  }

  /* one plane per polygon: the stored face normal when there is one, else CalcFaceNormalAccurate's last corner */
  polygon_planes = (float *)WZ3_MALLOC(((size_t)n_polygons + 1) * 16);
  if (!polygon_planes) { res = WZ3_ERR_OUT_OF_MEMORY; goto done; }
  memset(polygon_planes, 0, ((size_t)n_polygons + 1) * 16);
  const float *source_normals = d ? d->face_normals : NULL;
  for (uint32_t polygon = 0; polygon < n_polygons; polygon++) {
    const uint32_t len = plen[polygon];
    if (!len) continue;
    const uint32_t *loop = store + poff[polygon];
    double nx = 0, ny = 0, nz = 0;
    const uint64_t source = psrc[polygon];
    if (source_normals && source * 3u + 2u < (uint64_t)d->n_face_normals * 3u) {
      nx = source_normals[source * 3u]; ny = source_normals[source * 3u + 1u]; nz = source_normals[source * 3u + 2u];
    }
    if (!source_normals || (nx == 0 && ny == 0 && nz == 0)) {
      double length = 0;
      for (uint32_t corner = 0; corner < len; corner++) {
        const float *previous = pos + (size_t)loop[(corner + len - 1) % len] * 3, *current = pos + (size_t)loop[corner] * 3, *next = pos + (size_t)loop[(corner + 1) % len] * 3;
        const double t1x = (double)current[0] - previous[0], t1y = (double)current[1] - previous[1], t1z = (double)current[2] - previous[2];
        const double t2x = (double)next[0] - previous[0], t2y = (double)next[1] - previous[1], t2z = (double)next[2] - previous[2];
        nx = t1y * t2z - t1z * t2y; ny = t1z * t2x - t1x * t2z; nz = t1x * t2y - t1y * t2x;
        length = nx * nx + ny * ny + nz * nz;
        if (!(length > 1e-40)) break;
      }
      if (!(length > 1e-40)) { nx = 1; ny = 0; nz = 0; }
    }
    const float *origin = pos + (size_t)loop[0] * 3;
    float *plane = polygon_planes + (size_t)polygon * 4;
    plane[0] = (float)nx; plane[1] = (float)ny; plane[2] = (float)nz;
    plane[3] = (float)-(nx * origin[0] + ny * origin[1] + nz * origin[2]);
  }

  /* edge statistics */
  {
    const wz3_shadow_edge *e = (const wz3_shadow_edge *)edges_v.p;
    for (size_t i = 0; i < edges_v.n; i++) {
      if (e[i].count > t->max_edge_incidence) t->max_edge_incidence = e[i].count;
      if (e[i].count == 1) t->boundary_edges++;
      else if (e[i].count > 2) t->non_manifold_edges++;
      else if (e[i].a0 == e[i].a1 && e[i].b0 == e[i].b1) t->winding_conflict_edges++;
    }
  }
  {
    wz3__sh_input in;
    in.n_vertices = vertex_count; in.n_triangles = (uint32_t)tri_polygon.n; in.n_polygons = n_polygons; in.n_edges = (uint32_t)edges_v.n;
    in.positions = pos; in.source_indices = (const uint32_t *)sources.p; in.faces = (const uint32_t *)faces.p;
    in.triangle_polygon = (const uint32_t *)tri_polygon.p; in.polygon_planes = polygon_planes; in.edges = (const wz3_shadow_edge *)edges_v.p;
    res = wz3__sh_finish(t, &in);
    if (res != WZ3_OK) goto done;
  }
  t->native = 0;
  t->uses_shadow_vertex_map = (uint8_t)(uses_map != 0);
  if (positions_dynamic && !wz3__sh_refresh_planes(t)) { wz3_shadow_topology_free(t); res = WZ3_ERR_ARGUMENT; }
done:
  wz3__hm_free(&by_edge);
done_early:
  WZ3_FREE(polygon_planes);
  WZ3_FREE(source_to_canonical);
  wz3__hm_free(&by_index); wz3__hm_free(&by_position);
  wz3__sv_free(&canonical); wz3__sv_free(&sources); wz3__sv_free(&faces); wz3__sv_free(&tri_polygon); wz3__sv_free(&ring);
  wz3__sv_free(&ring_store); wz3__sv_free(&polygon_off); wz3__sv_free(&polygon_len); wz3__sv_free(&polygon_source); wz3__sv_free(&edges_v);
  return res;
}

WZ3_API wz3_result wz3_shadow_topology_prepare(const wz3_geometry *g, const wz3_shadow_data *d, const uint32_t *ranges, uint32_t n_ranges,
                                               int positions_dynamic, wz3_shadow_topology *out)
{
  if (!g || !out) return WZ3_ERR_ARGUMENT;
  memset(out, 0, sizeof *out);
  const wz3_shadow_recipe *recipe = d ? d->recipe : NULL;
  if (recipe && recipe->n_key == n_ranges && (!n_ranges || !memcmp(recipe->key, ranges, (size_t)n_ranges * 2 * sizeof(uint32_t))))
    return wz3__sh_native(g, recipe, positions_dynamic, out);
  return wz3__sh_generic(g, d, ranges, n_ranges, positions_dynamic, out);
}

/* ---- shadow volume (buildShadowVolume) ---- */

/* faceFront[polygon]: an out-of-range polygon reads `undefined`, which is false */
static int wz3__sh_front(const wz3_shadow_volume *v, uint32_t polygon, uint32_t n_polygons) { return polygon < n_polygons ? v->face_front[polygon] : 0; }

WZ3_API void wz3_shadow_volume_free(wz3_shadow_volume *v)
{
  if (!v) return;
  WZ3_FREE(v->indices);
  WZ3_FREE(v->face_front);
  memset(v, 0, sizeof *v);
}

WZ3_API wz3_result wz3_shadow_volume_build(const wz3_shadow_topology *t, const float light_world[3], const float *model, int include_caps, wz3_shadow_volume *out)
{
  memset(out, 0, sizeof *out);
  float identity[16];
  if (!model) { wz3_mat4_identity(identity); model = identity; }
  const double world[3] = {light_world[0], light_world[1], light_world[2]};
  wz3_legacy_transr_vector(model, world, 1, out->light_position);
  const uint32_t np = t->n_polygons;
  out->face_front = (uint8_t *)WZ3_MALLOC(np ? np : 1);
  const size_t capacity = (size_t)t->n_edges * 6 + (include_caps ? (size_t)t->n_triangles * 3 : 0);
  out->indices = (uint32_t *)WZ3_MALLOC((capacity ? capacity : 1) * 4);
  if (!out->face_front || !out->indices) { wz3_shadow_volume_free(out); return WZ3_ERR_OUT_OF_MEMORY; }
  const double lx = out->light_position[0], ly = out->light_position[1], lz = out->light_position[2];
  for (uint32_t polygon = 0; polygon < np; polygon++) {
    const float *p = t->polygon_planes + (size_t)polygon * 4;
    out->face_front[polygon] = (uint8_t)((double)p[0] * lx + (double)p[1] * ly + (double)p[2] * lz + (double)p[3] >= 0 ? 1 : 0);
  }
  out->wide = (size_t)t->n_vertices * 2 > 0xffffu;
  out->positions = t->volume_positions;
  out->extrusions = t->extrusions;
  uint32_t n = 0;
  for (uint32_t e = 0; e < t->n_edges; e++) {
    const wz3_shadow_edge *edge = &t->edges[e];
    const int first_front = wz3__sh_front(out, edge->f0, np);
    const int second = edge->count >= 2;
    const int second_front = second ? wz3__sh_front(out, edge->f1, np) : 1;
    if (first_front == second_front) continue;
    uint32_t a, b;
    if (first_front) { a = edge->a0; b = edge->b0; }
    else if (second) { a = edge->a1; b = edge->b1; }
    else { a = edge->b0; b = edge->a0; }
    a *= 2; b *= 2;
    out->indices[n++] = a; out->indices[n++] = b; out->indices[n++] = b + 1;
    out->indices[n++] = a; out->indices[n++] = b + 1; out->indices[n++] = a + 1;
  }
  out->silhouette_index_count = n;
  if (include_caps) {
    for (uint32_t tri = 0; tri < t->n_triangles; tri++) {
      const uint32_t polygon = t->triangle_polygon[tri];
      /* a polygon that does not exist makes the JavaScript side `undefined`: the sum is NaN and a typed array stores 0 */
      const uint32_t side = polygon < np ? out->face_front[polygon] : 0u;
      const int valid = polygon < np;
      out->indices[n++] = valid ? t->faces[tri * 3] * 2 + side : 0u;
      out->indices[n++] = valid ? t->faces[tri * 3 + 1] * 2 + side : 0u;
      out->indices[n++] = valid ? t->faces[tri * 3 + 2] * 2 + side : 0u;
    }
  }
  out->n_indices = n;
  out->cap_index_count = n - out->silhouette_index_count;
  return WZ3_OK;
}

WZ3_API void wz3_shadow_data_free(wz3_shadow_data *d)
{
  if (!d) return;
  WZ3_FREE(d->vertex_map); WZ3_FREE(d->triangle_mask); WZ3_FREE(d->face_map); WZ3_FREE(d->face_normals);
  if (d->recipe) {
    WZ3_FREE(d->recipe->source_indices); WZ3_FREE(d->recipe->faces); WZ3_FREE(d->recipe->triangle_planes); WZ3_FREE(d->recipe->planes);
    WZ3_FREE(d->recipe->edge_vertices); WZ3_FREE(d->recipe->edge_planes); WZ3_FREE(d->recipe->key);
    WZ3_FREE(d->recipe);
  }
  memset(d, 0, sizeof *d);
}

/* ---- native shadow jobs of a Mesh / MinMesh (prepareLegacyShadowRecipe, prepareMinMeshShadowRecipe) ---- */

static int wz3__sd_casts(wz3_shadow_casts_fn casts, void *user, int32_t material) { return material >= 0 && casts && casts(user, material); }

static int wz3__sd_alloc(wz3_shadow_data *d, uint32_t nv, uint32_t nt)
{
  d->n_vertices = nv; d->n_triangles = nt;
  d->vertex_map = (uint32_t *)WZ3_MALLOC(((size_t)nv + 1) * 4);
  d->triangle_mask = (uint8_t *)WZ3_MALLOC((size_t)nt + 1);
  d->face_map = (uint32_t *)WZ3_MALLOC(((size_t)nt + 1) * 4);
  return d->vertex_map && d->triangle_mask && d->face_map;
}

/* what both recipes build: the caster vertices (canonical vertices some caster polygon uses, ascending), planes, fan triangles, edges */
typedef struct wz3__sr {
  wz3__sv source, faces, tri_planes, edge_vertices, edge_planes, planes, key;
  uint32_t plane_count, trailing, non_manifold;
} wz3__sr;

static void wz3__sr_init(wz3__sr *r)
{
  memset(r, 0, sizeof *r);
  wz3__sv_init(&r->source, 4); wz3__sv_init(&r->faces, 4); wz3__sv_init(&r->tri_planes, 4); wz3__sv_init(&r->edge_vertices, 4);
  wz3__sv_init(&r->edge_planes, 4); wz3__sv_init(&r->planes, 4); wz3__sv_init(&r->key, 4);
}
static void wz3__sr_free(wz3__sr *r)
{
  wz3__sv_free(&r->source); wz3__sv_free(&r->faces); wz3__sv_free(&r->tri_planes); wz3__sv_free(&r->edge_vertices);
  wz3__sv_free(&r->edge_planes); wz3__sv_free(&r->planes); wz3__sv_free(&r->key);
}
static int wz3__sr_failed(const wz3__sr *r)
{
  return r->source.failed || r->faces.failed || r->tri_planes.failed || r->edge_vertices.failed || r->edge_planes.failed || r->planes.failed || r->key.failed;
}

/* moves the vectors into a recipe (which then owns them) */
static wz3_result wz3__sr_take(wz3__sr *r, wz3_shadow_data *d)
{
  wz3_shadow_recipe *rc = (wz3_shadow_recipe *)WZ3_MALLOC(sizeof *rc);
  if (!rc) return WZ3_ERR_OUT_OF_MEMORY;
  memset(rc, 0, sizeof *rc);
  /* the vectors may be empty (no storage allocated): give the recipe real blocks so every pointer is valid */
  #define WZ3__SR_COPY(field, vec, type) do { rc->field = (type *)WZ3_MALLOC((vec.n ? vec.n : 1) * sizeof(type)); if (!rc->field) goto fail; if (vec.n) memcpy(rc->field, vec.p, vec.n * sizeof(type)); } while (0)
  WZ3__SR_COPY(source_indices, r->source, uint32_t);
  WZ3__SR_COPY(faces, r->faces, uint32_t);
  WZ3__SR_COPY(triangle_planes, r->tri_planes, uint32_t);
  WZ3__SR_COPY(planes, r->planes, float);
  WZ3__SR_COPY(edge_vertices, r->edge_vertices, uint32_t);
  WZ3__SR_COPY(edge_planes, r->edge_planes, uint32_t);
  WZ3__SR_COPY(key, r->key, uint32_t);
  #undef WZ3__SR_COPY
  rc->n_source = (uint32_t)r->source.n; rc->n_triangles = (uint32_t)(r->faces.n / 3); rc->n_planes = r->plane_count;
  rc->n_edges = (uint32_t)(r->edge_vertices.n / 2); rc->n_key = (uint32_t)(r->key.n / 2);
  rc->trailing_plane_slots = r->trailing; rc->source_non_manifold_edges = r->non_manifold;
  d->recipe = rc;
  return WZ3_OK;
fail:
  WZ3_FREE(rc->source_indices); WZ3_FREE(rc->faces); WZ3_FREE(rc->triangle_planes); WZ3_FREE(rc->planes);
  WZ3_FREE(rc->edge_vertices); WZ3_FREE(rc->edge_planes); WZ3_FREE(rc->key); WZ3_FREE(rc);
  return WZ3_ERR_OUT_OF_MEMORY;
}

static float wz3__sd_f32(double v) { return (float)v; }

/* GenMesh: calculatePlane of prepareLegacyShadowRecipe, every product rounded to float */
static void wz3__sd_mesh_plane(const wz3_mesh *m, const float *positions, const int32_t *loop, uint32_t n, float *plane)
{
  float nx = 0, ny = 0, nz = 0, length = 0;
  for (uint32_t corner = 0; corner < n; corner++) {
    const float *previous = positions + (size_t)wz3__mh_vert(m, loop[(corner + n - 1) % n]) * 3;
    const float *current = positions + (size_t)wz3__mh_vert(m, loop[corner]) * 3;
    const float *next = positions + (size_t)wz3__mh_vert(m, loop[(corner + 1) % n]) * 3;
    const float t1x = wz3__sd_f32((double)current[0] - previous[0]), t1y = wz3__sd_f32((double)current[1] - previous[1]), t1z = wz3__sd_f32((double)current[2] - previous[2]);
    const float t2x = wz3__sd_f32((double)next[0] - previous[0]), t2y = wz3__sd_f32((double)next[1] - previous[1]), t2z = wz3__sd_f32((double)next[2] - previous[2]);
    nx = wz3__sd_f32((double)wz3__sd_f32((double)t1y * t2z) - wz3__sd_f32((double)t1z * t2y));
    ny = wz3__sd_f32((double)wz3__sd_f32((double)t1z * t2x) - wz3__sd_f32((double)t1x * t2z));
    nz = wz3__sd_f32((double)wz3__sd_f32((double)t1x * t2y) - wz3__sd_f32((double)t1y * t2x));
    length = wz3__sd_f32((double)wz3__sd_f32((double)wz3__sd_f32((double)nx * nx) + wz3__sd_f32((double)ny * ny)) + wz3__sd_f32((double)nz * nz));
    if (!(length > 1e-40)) break;
  }
  if (length >= 1e-40) {
    const float inverse = wz3__sd_f32(1.0 / sqrt((double)length));
    nx = wz3__sd_f32((double)nx * inverse); ny = wz3__sd_f32((double)ny * inverse); nz = wz3__sd_f32((double)nz * inverse);
  } else { nx = 1; ny = 0; nz = 0; }
  const float *origin = positions + (size_t)wz3__mh_vert(m, loop[0]) * 3;
  plane[0] = nx; plane[1] = ny; plane[2] = nz;
  plane[3] = wz3__sd_f32(-(double)wz3__sd_f32((double)wz3__sd_f32((double)wz3__sd_f32((double)nx * origin[0]) + wz3__sd_f32((double)ny * origin[1])) + wz3__sd_f32((double)nz * origin[2])));
}

static wz3_result wz3__sd_mesh_recipe(const wz3_mesh *m, const wz3_geometry *g, wz3_shadow_casts_fn casts, void *user, wz3_shadow_data *d)
{
  const uint32_t nf = m->n_faces, nvm = m->n_verts;
  wz3__sr r;
  wz3__sr_init(&r);
  /* groupKey: the ranges of the groups whose slot casts */
  for (uint32_t i = 0; i < g->n_groups; i++) {
    const int32_t slot = g->groups[i].slot;
    if (slot >= 0 && (uint32_t)slot < m->n_slots && wz3__sd_casts(casts, user, m->slots[slot].material)) { wz3__sv_u32(&r.key, g->groups[i].start); wz3__sv_u32(&r.key, g->groups[i].count); }
  }
  if (r.key.failed) { wz3__sr_free(&r); return WZ3_ERR_OUT_OF_MEMORY; }
  if (!r.key.n) { wz3__sr_free(&r); return WZ3_OK; }
  uint32_t *face_plane = (uint32_t *)WZ3_MALLOC(((size_t)nf + 1) * 4);
  uint32_t *loop_off = (uint32_t *)WZ3_MALLOC(((size_t)nf + 1) * 4);
  uint32_t *loop_len = (uint32_t *)WZ3_MALLOC(((size_t)nf + 1) * 4);
  uint8_t *used = (uint8_t *)WZ3_MALLOC((size_t)nvm + 1);
  int32_t *to_shadow = (int32_t *)WZ3_MALLOC(((size_t)nvm + 1) * 4);
  uint8_t *edge_seen = (uint8_t *)WZ3_MALLOC((size_t)m->n_edges + 1);
  wz3__sv loops;
  wz3__sv_init(&loops, 4);
  wz3_result res = WZ3_OK;
  if (!face_plane || !loop_off || !loop_len || !used || !to_shadow || !edge_seen) { res = WZ3_ERR_OUT_OF_MEMORY; goto done; }
  uint32_t casters = 0;
  const uint32_t limit = m->n_edges * 2u + 1u;
  for (uint32_t f = 0; f < nf; f++) {
    const wz3__mface *face = &m->faces[f];
    face_plane[f] = casters + 1;       /* a non-caster aliases the next caster's plane (Temp+1) */
    loop_len[f] = 0;
    if (!face->used || !((face->material >= 0 && (uint32_t)face->material < m->n_slots) && wz3__sd_casts(casts, user, m->slots[face->material].material))) continue;
    if (face->edge < 0) continue;
    const size_t start_n = loops.n;
    int32_t h = face->edge;
    uint32_t len = 0;
    do {
      wz3__sv_u32(&loops, (uint32_t)h);
      len++;
      h = wz3__mh_next_face(m, h);
      if (len > limit) { res = WZ3_ERR_ARGUMENT; goto done; }       /* "broken GenMesh shadow face loop" */
    } while (h != face->edge);
    if (loops.failed) { res = WZ3_ERR_OUT_OF_MEMORY; goto done; }
    if (len < 3) { loops.n = start_n; continue; }
    loop_off[f] = (uint32_t)start_n; loop_len[f] = len;
    casters++;
  }
  if (!casters) goto done;
  const int32_t *loop_store = (const int32_t *)loops.p;
  const uint32_t *vmap = d->vertex_map;
  memset(used, 0, (size_t)nvm + 1);
  for (uint32_t f = 0; f < nf; f++) {
    for (uint32_t k = 0; k < loop_len[f]; k++) {
      const int32_t v = wz3__mh_vert(m, loop_store[loop_off[f] + k]);
      const uint32_t canonical = (v >= 0 && (uint32_t)v < g->n_vertices) ? vmap[v] : 0xffffffffu;
      if (canonical < nvm) used[canonical] = 1;
    }
  }
  for (uint32_t i = 0; i < nvm; i++) {
    to_shadow[i] = -1;
    if (used[i]) { to_shadow[i] = (int32_t)r.source.n; wz3__sv_u32(&r.source, i); }
  }
  r.plane_count = casters + 1;
  for (uint32_t i = 0; i < r.plane_count * 4; i++) wz3__sv_f32(&r.planes, 0.0f);
  if (r.planes.failed || r.source.failed) { res = WZ3_ERR_OUT_OF_MEMORY; goto done; }
  float *planes = (float *)r.planes.p;
#define WZ3__SD_SHADOW(vertex) ((uint32_t)to_shadow[vmap[wz3__mh_vert(m, (vertex))]])
  for (uint32_t f = 0; f < nf; f++) {
    const uint32_t len = loop_len[f];
    if (!len) continue;
    const int32_t *loop = loop_store + loop_off[f];
    const uint32_t plane = face_plane[f];
    wz3__sd_mesh_plane(m, g->positions, loop, len, planes + (size_t)plane * 4);
    const uint32_t first = WZ3__SD_SHADOW(loop[0]);
    for (uint32_t corner = 2; corner < len; corner++) {
      wz3__sv_u32(&r.faces, first); wz3__sv_u32(&r.faces, WZ3__SD_SHADOW(loop[corner - 1])); wz3__sv_u32(&r.faces, WZ3__SD_SHADOW(loop[corner]));
      wz3__sv_u32(&r.tri_planes, plane);
    }
  }
  memset(edge_seen, 0, (size_t)m->n_edges + 1);
  for (uint32_t f = 0; f < nf; f++) {
    const uint32_t len = loop_len[f];
    if (!len) continue;
    const int32_t *loop = loop_store + loop_off[f];
    for (uint32_t k = 0; k < len; k++) {
      const int32_t h = loop[k];
      const uint32_t edge = (uint32_t)h >> 1;
      if (edge_seen[edge]) continue;
      edge_seen[edge] = 1;
      wz3__sv_u32(&r.edge_vertices, WZ3__SD_SHADOW(h));
      wz3__sv_u32(&r.edge_vertices, WZ3__SD_SHADOW(wz3__mh_next_face(m, h)));
      const int32_t f0 = wz3__mh_face(m, h), f1 = wz3__mh_face(m, h ^ 1);
      uint32_t plane0 = (f0 >= 0 && (uint32_t)f0 < nf) ? face_plane[f0] : 0u, plane1 = (f1 >= 0 && (uint32_t)f1 < nf) ? face_plane[f1] : 0u;
      /* Temp+1 after the final caster addresses native scratch beyond this job's planes: the reserved front plane */
      if (plane0 >= r.plane_count) { plane0 = 0; r.trailing++; }
      if (plane1 >= r.plane_count) { plane1 = 0; r.trailing++; }
      wz3__sv_u32(&r.edge_planes, plane0); wz3__sv_u32(&r.edge_planes, plane1);
    }
  }
#undef WZ3__SD_SHADOW
  if (wz3__sr_failed(&r)) { res = WZ3_ERR_OUT_OF_MEMORY; goto done; }
  res = wz3__sr_take(&r, d);
done:
  WZ3_FREE(face_plane); WZ3_FREE(loop_off); WZ3_FREE(loop_len); WZ3_FREE(used); WZ3_FREE(to_shadow); WZ3_FREE(edge_seen);
  wz3__sv_free(&loops);
  wz3__sr_free(&r);
  return res;
}

WZ3_API wz3_result wz3_mesh_shadow_data(const wz3_mesh *mesh, const wz3_geometry *g, wz3_shadow_casts_fn casts, void *user, wz3_shadow_data *out)
{
  if (!mesh || !g || !out) return WZ3_ERR_ARGUMENT;
  memset(out, 0, sizeof *out);
  if (g->n_vertices != mesh->n_verts || g->colors == NULL) return WZ3_ERR_ARGUMENT;
  const uint32_t nv = g->n_vertices, nt = g->n_indices / 3;
  if (!wz3__sd_alloc(out, nv, nt)) { wz3_shadow_data_free(out); return WZ3_ERR_OUT_OF_MEMORY; }
  for (uint32_t i = 0; i < nv; i++) {
    const int32_t first = mesh->verts[i].first;
    out->vertex_map[i] = (first >= 0 && (uint32_t)first < nv) ? (uint32_t)first : i;
  }
  /* per triangle, in the order wz3_mesh_prepare lays the groups and their faces out */
  const uint32_t ring_guard = mesh->n_edges * 2u + 2u;
  uint32_t t = 0;
  for (uint32_t gi = 0; gi < g->n_groups; gi++) {
    const int32_t slot = g->groups[gi].slot;
    for (uint32_t f = 0; f < mesh->n_faces && t <= nt; f++) {
      const wz3__mface *face = &mesh->faces[f];
      if (!face->material || face->edge < 0 || face->material < 0 || (uint32_t)face->material >= mesh->n_slots || face->material != slot) continue;
      const uint32_t corners = wz3__geom_face_corners(mesh, face->edge, ring_guard);
      if (corners < 3) continue;
      for (uint32_t k = 0; k + 2 < corners; k++) {
        if (t < nt) { out->triangle_mask[t] = face->used ? 1 : 0; out->face_map[t] = f; }
        t++;
      }
    }
  }
  if (t != nt) { wz3_shadow_data_free(out); return WZ3_ERR_ARGUMENT; }
  const wz3_result r = wz3__sd_mesh_recipe(mesh, g, casts, user, out);
  if (r != WZ3_OK) wz3_shadow_data_free(out);
  return r;
}

/* ---- GenMinMesh ---- */

static int wz3__mm_edge_less(const int32_t *e, uint32_t left, uint32_t right)
{
  const uint32_t a = left * 3, b = right * 3;
  return e[a + 1] < e[b + 1] || (e[a + 1] == e[b + 1] && e[a + 2] < e[b + 2]);
}

/* heapSortMinMeshEdges: GenMinMesh::CalcAdjacency's comparison-only heap sort (not stable; ties keep the order it produces) */
static void wz3__mm_sift_down(int32_t *e, uint32_t count, uint32_t root)
{
  const int32_t face_vert = e[root * 3], v0 = e[root * 3 + 1], v1 = e[root * 3 + 2];
  while (root < (count >> 1)) {
    uint32_t child = root * 2 + 1;
    if (child + 1 < count && wz3__mm_edge_less(e, child, child + 1)) child++;
    const uint32_t offset = child * 3;
    if (!(v0 < e[offset + 1] || (v0 == e[offset + 1] && v1 < e[offset + 2]))) break;
    e[root * 3] = e[offset]; e[root * 3 + 1] = e[offset + 1]; e[root * 3 + 2] = e[offset + 2];
    root = child;
  }
  e[root * 3] = face_vert; e[root * 3 + 1] = v0; e[root * 3 + 2] = v1;
}

static void wz3__mm_heap_sort_edges(int32_t *e, uint32_t count)
{
  for (int64_t root = (int64_t)(count >> 1) - 1; root >= 0; root--) wz3__mm_sift_down(e, count, (uint32_t)root);
  for (uint32_t n = count; n-- > 1;) {
    const uint32_t tail = n * 3;
    const int32_t face_vert = e[tail], v0 = e[tail + 1], v1 = e[tail + 2];
    e[tail] = e[0]; e[tail + 1] = e[1]; e[tail + 2] = e[2];
    e[0] = face_vert; e[1] = v0; e[2] = v1;
    wz3__mm_sift_down(e, n, 0);
  }
}

static wz3_result wz3__sd_minmesh_recipe(const wz3_minmesh *m, const wz3_geometry *g, wz3_shadow_casts_fn casts, void *user, wz3_shadow_data *d)
{
  const uint32_t nf = m->n_faces, nvm = m->n_verts;
  wz3__sr r;
  wz3__sr_init(&r);
  for (uint32_t i = 0; i < g->n_groups; i++) {
    const int32_t cluster = g->groups[i].slot;
    if (cluster >= 0 && (uint32_t)cluster < m->n_clusters && wz3__sd_casts(casts, user, m->clusters[cluster].material)) { wz3__sv_u32(&r.key, g->groups[i].start); wz3__sv_u32(&r.key, g->groups[i].count); }
  }
  if (r.key.failed) { wz3__sr_free(&r); return WZ3_ERR_OUT_OF_MEMORY; }
  if (!r.key.n) { wz3__sr_free(&r); return WZ3_OK; }
  const uint32_t *vmap = d->vertex_map;
  uint8_t *selected = (uint8_t *)WZ3_MALLOC((size_t)nf + 1);
  uint32_t *face_plane = (uint32_t *)WZ3_MALLOC(((size_t)nf + 1) * 4);
  int32_t *adjacency = (int32_t *)WZ3_MALLOC(((size_t)m->n_corners + 1) * 4);
  uint8_t *used = (uint8_t *)WZ3_MALLOC((size_t)nvm + 1);
  int32_t *to_shadow = (int32_t *)WZ3_MALLOC(((size_t)nvm + 1) * 4);
  int32_t *sorted = NULL;
  wz3_result res = WZ3_OK;
  if (!selected || !face_plane || !adjacency || !used || !to_shadow) { res = WZ3_ERR_OUT_OF_MEMORY; goto done; }
  uint32_t casters = 0, incidences = 0;
  for (uint32_t f = 0; f < nf; f++) {
    const wz3__mm_face *face = &m->faces[f];
    selected[f] = 0; face_plane[f] = 0;
    if (face->count >= 3) incidences += (uint32_t)face->count;
    if (face->count < 3 || (face->flags & 1u) || face->cluster < 0 || (uint32_t)face->cluster >= m->n_clusters ||
        !wz3__sd_casts(casts, user, m->clusters[face->cluster].material)) continue;
    selected[f] = 1;
    face_plane[f] = ++casters;
  }
  if (!casters) goto done;
  sorted = (int32_t *)WZ3_MALLOC(((size_t)incidences + 1) * 3 * 4);
  if (!sorted) { res = WZ3_ERR_OUT_OF_MEMORY; goto done; }
  uint32_t at = 0;
  for (uint32_t f = 0; f < nf; f++) {
    const wz3__mm_face *face = &m->faces[f];
    if (face->count < 3) continue;
    if (face->count > 8) { res = WZ3_ERR_ARGUMENT; goto done; }      /* "exceeds native adjacency tag width" */
    for (int32_t corner = 0; corner < face->count; corner++) {
      uint32_t v0 = vmap[m->corners[face->first + corner]], v1 = vmap[m->corners[face->first + (corner + 1) % face->count]];
      if (v0 > v1) { const uint32_t swap = v0; v0 = v1; v1 = swap; }
      sorted[at * 3] = (int32_t)(f * 8u + (uint32_t)corner); sorted[at * 3 + 1] = (int32_t)v0; sorted[at * 3 + 2] = (int32_t)v1;
      at++;
    }
  }
  wz3__mm_heap_sort_edges(sorted, incidences);
  for (uint32_t i = 0; i < m->n_corners; i++) adjacency[i] = -1;
  {
    int32_t last0 = -1, last1 = -1, pending = -1;
    uint32_t run = 0;
    for (uint32_t i = 0; i < incidences; i++) {
      const int32_t face_vert = sorted[i * 3], v0 = sorted[i * 3 + 1], v1 = sorted[i * 3 + 2];
      if (v0 == last0 && v1 == last1) {
        run++;
        if (pending < 0) pending = face_vert;
        else {
          const uint32_t f0 = (uint32_t)pending >> 3, c0 = (uint32_t)pending & 7u, f1 = (uint32_t)face_vert >> 3, c1 = (uint32_t)face_vert & 7u;
          adjacency[m->faces[f0].first + c0] = face_vert;
          adjacency[m->faces[f1].first + c1] = pending;
          pending = -1;
        }
      } else {
        if (run > 2) r.non_manifold++;
        last0 = v0; last1 = v1; run = 1; pending = face_vert;
      }
    }
    if (run > 2) r.non_manifold++;
  }
  memset(used, 0, (size_t)nvm + 1);
  for (uint32_t f = 0; f < nf; f++) {
    if (!selected[f]) continue;
    for (int32_t k = 0; k < m->faces[f].count; k++) used[vmap[m->corners[m->faces[f].first + k]]] = 1;
  }
  for (uint32_t i = 0; i < nvm; i++) {
    to_shadow[i] = -1;
    if (used[i]) { to_shadow[i] = (int32_t)r.source.n; wz3__sv_u32(&r.source, i); }
  }
  r.plane_count = casters + 1;
  for (uint32_t i = 0; i < r.plane_count * 4; i++) wz3__sv_f32(&r.planes, 0.0f);
  if (r.planes.failed || r.source.failed) { res = WZ3_ERR_OUT_OF_MEMORY; goto done; }
  float *planes = (float *)r.planes.p;
#define WZ3__SD_SHADOW(corner_index) ((uint32_t)to_shadow[vmap[m->corners[(corner_index)]]])
  for (uint32_t f = 0; f < nf; f++) {
    if (!selected[f]) continue;
    const wz3__mm_face *face = &m->faces[f];
    const uint32_t plane = face_plane[f];
    const float *point = m->verts[m->corners[face->first]].position;
    float *p = planes + (size_t)plane * 4;
    const float nx = face->normal[0], ny = face->normal[1], nz = face->normal[2];
    p[0] = nx; p[1] = ny; p[2] = nz;
    p[3] = wz3__sd_f32(-(double)wz3__sd_f32((double)wz3__sd_f32((double)wz3__sd_f32((double)nx * point[0]) + wz3__sd_f32((double)ny * point[1])) + wz3__sd_f32((double)nz * point[2])));
    const uint32_t first = WZ3__SD_SHADOW(face->first);
    for (int32_t corner = 2; corner < face->count; corner++) {
      wz3__sv_u32(&r.faces, first); wz3__sv_u32(&r.faces, WZ3__SD_SHADOW(face->first + (uint32_t)corner - 1)); wz3__sv_u32(&r.faces, WZ3__SD_SHADOW(face->first + (uint32_t)corner));
      wz3__sv_u32(&r.tri_planes, plane);
    }
    for (int32_t corner = 0; corner < face->count; corner++) {
      const int32_t tag = adjacency[face->first + (uint32_t)corner];
      const int32_t adjacent = tag >> 3;
      if ((int32_t)f >= adjacent) continue;
      wz3__sv_u32(&r.edge_vertices, WZ3__SD_SHADOW(face->first + (uint32_t)corner));
      wz3__sv_u32(&r.edge_vertices, WZ3__SD_SHADOW(face->first + (uint32_t)((corner + 1) % face->count)));
      wz3__sv_u32(&r.edge_planes, plane); wz3__sv_u32(&r.edge_planes, adjacent >= 0 ? face_plane[adjacent] : 0u);
    }
  }
#undef WZ3__SD_SHADOW
  if (wz3__sr_failed(&r)) { res = WZ3_ERR_OUT_OF_MEMORY; goto done; }
  res = wz3__sr_take(&r, d);
done:
  WZ3_FREE(selected); WZ3_FREE(face_plane); WZ3_FREE(adjacency); WZ3_FREE(used); WZ3_FREE(to_shadow); WZ3_FREE(sorted);
  wz3__sr_free(&r);
  return res;
}

WZ3_API wz3_result wz3_minmesh_shadow_data(const wz3_minmesh *mesh, const wz3_geometry *g, wz3_shadow_casts_fn casts, void *user, wz3_shadow_data *out)
{
  if (!mesh || !g || !out) return WZ3_ERR_ARGUMENT;
  memset(out, 0, sizeof *out);
  if (g->n_vertices != mesh->n_verts || g->colors != NULL) return WZ3_ERR_ARGUMENT;
  wz3_minmesh *owned = NULL;
  const wz3_minmesh *m = mesh;
  if (!m->normals_valid) {                          /* MinMesh.prepare calls calcNormals(): the face normals are the derived ones */
    owned = wz3__mm_clone(mesh);
    if (!owned) return WZ3_ERR_OUT_OF_MEMORY;
    wz3__mm_calc_normals(owned);
    m = owned;
  }
  const uint32_t nv = g->n_vertices, nt = g->n_indices / 3;
  wz3_result res = WZ3_OK;
  if (!wz3__sd_alloc(out, nv, nt)) { res = WZ3_ERR_OUT_OF_MEMORY; goto fail; }
  /* vertices weld when position, bone matrices and weights are bit-identical (compactPreparedTemplate); the first of a group stands for it */
  {
    wz3__hm table;
    wz3__hm_init(&table, nv);
    if (table.failed) { wz3__hm_free(&table); res = WZ3_ERR_OUT_OF_MEMORY; goto fail; }
    for (uint32_t i = 0; i < nv; i++) {
      const wz3__mm_vertex *v = &m->verts[i];
      uint32_t words[11];
      memcpy(words, v->position, 12); memcpy(words + 3, v->weights, 16);
      words[7] = (uint32_t)v->matrices[0] | ((uint32_t)v->matrices[1] << 16);
      words[8] = (uint32_t)v->matrices[2] | ((uint32_t)v->matrices[3] << 16);
      uint64_t h = 0x811c9dc5u;
      for (int k = 0; k < 9; k++) h = (h ^ words[k]) * 0x100000001b3ull;
      h |= 1ull;
      uint32_t slot = wz3__hm_hash(h) & (table.cap - 1u);
      int32_t found = -1;
      while (table.keys[slot]) {
        const wz3__mm_vertex *o = &m->verts[table.vals[slot]];
        if (!memcmp(o->position, v->position, 12) && !memcmp(o->weights, v->weights, 16) && !memcmp(o->matrices, v->matrices, 8)) { found = table.vals[slot]; break; }
        slot = (slot + 1u) & (table.cap - 1u);
      }
      if (found < 0) { table.keys[slot] = h; table.vals[slot] = (int32_t)i; found = (int32_t)i; }
      out->vertex_map[i] = (uint32_t)found;
    }
    wz3__hm_free(&table);
  }
  {
    uint32_t t = 0;
    for (uint32_t gi = 0; gi < g->n_groups; gi++) {
      const int32_t cluster = g->groups[gi].slot;
      for (uint32_t f = 0; f < m->n_faces; f++) {
        const wz3__mm_face *face = &m->faces[f];
        if (face->cluster != cluster || face->cluster <= 0 || (uint32_t)face->cluster >= m->n_clusters || face->count < 3) continue;
        for (int32_t k = 2; k < face->count; k++) {
          if (t < nt) { out->triangle_mask[t] = (face->flags & 1u) ? 0 : 1; out->face_map[t] = f; }
          t++;
        }
      }
    }
    if (t != nt) { res = WZ3_ERR_ARGUMENT; goto fail; }
  }
  out->n_face_normals = m->n_faces;
  out->face_normals = (float *)WZ3_MALLOC(((size_t)m->n_faces * 3 + 1) * 4);
  if (!out->face_normals) { res = WZ3_ERR_OUT_OF_MEMORY; goto fail; }
  for (uint32_t f = 0; f < m->n_faces; f++) memcpy(out->face_normals + (size_t)f * 3, m->faces[f].normal, 12);
  res = wz3__sd_minmesh_recipe(m, g, casts, user, out);
  if (res != WZ3_OK) goto fail;
  wz3__mm_free(owned);
  return WZ3_OK;
fail:
  wz3_shadow_data_free(out);
  wz3__mm_free(owned);
  return res;
}
