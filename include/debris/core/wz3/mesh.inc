/* ======================================================================== */
/*  GenMesh (genmesh.cpp)                                                   */
/*                                                                          */
/*  The half-edge mesh of the old werkkzeug3 GenMesh: one edge record holds */
/*  two directed half-edges (halfedge = edge * 2 + side). Records keep the  */
/*  source layout so topology, selection masks and attribute wedges match   */
/*  the original record for record.                                         */
/* ======================================================================== */

#define WZ3__MESH_DEFAULT_MASK 0x2fu
enum { WZ3__MA_POS = 0, WZ3__MA_NORMAL = 1, WZ3__MA_TANGENT = 2, WZ3__MA_COLOR0 = 3, WZ3__MA_UV0 = 5, WZ3__MA_UV1 = 6 };
enum { WZ3__MF_POS = 1, WZ3__MF_NORMAL = 2, WZ3__MF_TANGENT = 4, WZ3__MF_COLOR0 = 8, WZ3__MF_UV0 = 32, WZ3__MF_UV1 = 64 };
enum { WZ3__SEL_ADD = 0, WZ3__SEL_SUB = 1, WZ3__SEL_SET = 2, WZ3__SEL_SETNOT = 3 };
enum { WZ3__ALL_EDGE = 1, WZ3__ALL_FACE = 2, WZ3__ALL_VERT = 4 };
#define WZ3__MAT_NULL (-1)      /* slot 0 */
#define WZ3__MAT_DEFAULT (-2)   /* slot 1: the built-in default material */

typedef struct wz3__mvert {
  uint8_t mask, id, select, used;
  int32_t next, first, temp, temp2, re_index;
  uint8_t weights[4], matrices[4];
  float v[6][4];                      /* position, normal, tangent, color0, uv0, uv1 */
} wz3__mvert;

typedef struct wz3__medge {
  uint8_t mask, id, select, used;
  int32_t next[2], prev[2], face[2], vert[2], temp[2], crease;
} wz3__medge;

typedef struct wz3__mface {
  uint8_t mask, id, select, used;
  int32_t material, edge, temp, temp2, temp3;
} wz3__mface;

typedef struct wz3__mslot { int32_t material; int32_t pass; } wz3__mslot;

struct wz3_mesh {
  uint32_t vertex_mask;
  wz3__mvert *verts;  uint32_t n_verts, cap_verts;
  wz3__medge *edges;  uint32_t n_edges, cap_edges;
  wz3__mface *faces;  uint32_t n_faces, cap_faces;
  wz3__mslot *slots;  uint32_t n_slots, cap_slots;
  int32_t *parts;     uint32_t n_parts, cap_parts;
  int32_t pivot;
  int got_normals;
  int oom;                            /* sticky: an allocation failed while building */
};

static void *wz3__mesh_grow(void *old, uint32_t count, uint32_t *cap, size_t elem, uint32_t need, int *oom)
{
  if (need <= *cap) return old;
  uint32_t ncap = *cap ? *cap : 16;
  while (ncap < need) ncap *= 2;
  void *p = WZ3_MALLOC((size_t)ncap * elem);
  if (!p) { *oom = 1; return old; }
  if (count) memcpy(p, old, (size_t)count * elem);
  WZ3_FREE(old);
  *cap = ncap;
  return p;
}

#define WZ3__MESH_RESERVE(m, field, count, cap, T, need)                                                    \
  do {                                                                                                      \
    if ((m)->oom) return -1;           /* sticky: never grow (and leak) after a failed allocation */         \
    T *grown_ = (T *)wz3__mesh_grow((m)->field, (m)->count, &(m)->cap, sizeof(T), (uint32_t)(need), &(m)->oom); \
    if ((m)->oom) return -1;                                                                                \
    (m)->field = grown_;                                                                                    \
  } while (0)

static wz3_mesh *wz3__mesh_new(uint32_t vertex_mask)
{
  wz3_mesh *m = (wz3_mesh *)WZ3_MALLOC(sizeof *m);
  if (!m) return NULL;
  memset(m, 0, sizeof *m);
  m->vertex_mask = vertex_mask;
  m->pivot = -1;
  m->slots = (wz3__mslot *)WZ3_MALLOC(8 * sizeof(wz3__mslot));
  if (!m->slots) { WZ3_FREE(m); return NULL; }
  m->cap_slots = 8;
  m->slots[0].material = WZ3__MAT_NULL; m->slots[0].pass = 0;
  m->slots[1].material = WZ3__MAT_DEFAULT; m->slots[1].pass = 0;
  m->n_slots = 2;
  return m;
}

static void wz3__mesh_free(wz3_mesh *m)
{
  if (!m) return;
  WZ3_FREE(m->verts); WZ3_FREE(m->edges); WZ3_FREE(m->faces); WZ3_FREE(m->slots); WZ3_FREE(m->parts);
  WZ3_FREE(m);
}

static void wz3__mvert_init(wz3__mvert *v)
{
  memset(v, 0, sizeof *v);
  v->used = 1;
  v->next = v->first = v->temp = v->temp2 = v->re_index = -1;
  v->matrices[0] = v->matrices[1] = v->matrices[2] = v->matrices[3] = 255;
}
static void wz3__medge_init(wz3__medge *e)
{
  memset(e, 0, sizeof *e);
  e->used = 1;
  e->next[0] = e->next[1] = e->prev[0] = e->prev[1] = e->face[0] = e->face[1] = e->vert[0] = e->vert[1] =
    e->temp[0] = e->temp[1] = -1;
}
static void wz3__mface_init(wz3__mface *f)
{
  memset(f, 0, sizeof *f);
  f->used = 1;
  f->material = 1;
  f->edge = -1;
  f->temp = -1;
}

static int wz3__mesh_push_vertex(wz3_mesh *m, wz3__mvert **out)
{
  WZ3__MESH_RESERVE(m, verts, n_verts, cap_verts, wz3__mvert, m->n_verts + 1);
  *out = &m->verts[m->n_verts++];
  wz3__mvert_init(*out);
  return 0;
}
static int wz3__mesh_push_edge(wz3_mesh *m, wz3__medge **out)
{
  WZ3__MESH_RESERVE(m, edges, n_edges, cap_edges, wz3__medge, m->n_edges + 1);
  *out = &m->edges[m->n_edges++];
  wz3__medge_init(*out);
  return 0;
}
static int wz3__mesh_push_face(wz3_mesh *m, wz3__mface **out)
{
  WZ3__MESH_RESERVE(m, faces, n_faces, cap_faces, wz3__mface, m->n_faces + 1);
  *out = &m->faces[m->n_faces++];
  wz3__mface_init(*out);
  return 0;
}
static int wz3__mesh_push_slot(wz3_mesh *m, int32_t material, int32_t pass)
{
  WZ3__MESH_RESERVE(m, slots, n_slots, cap_slots, wz3__mslot, m->n_slots + 1);
  m->slots[m->n_slots].material = material;
  m->slots[m->n_slots].pass = pass;
  m->n_slots++;
  return 0;
}
static int wz3__mesh_push_part(wz3_mesh *m, int32_t part)
{
  WZ3__MESH_RESERVE(m, parts, n_parts, cap_parts, int32_t, m->n_parts + 1);
  m->parts[m->n_parts++] = part;
  return 0;
}

static wz3_mesh *wz3__mesh_clone(const wz3_mesh *s)
{
  wz3_mesh *m = wz3__mesh_new(s->vertex_mask);
  if (!m) return NULL;
  WZ3_FREE(m->slots);
  m->slots = NULL; m->cap_slots = 0;
  m->verts = (wz3__mvert *)WZ3_MALLOC((size_t)(s->n_verts ? s->n_verts : 1) * sizeof(wz3__mvert));
  m->edges = (wz3__medge *)WZ3_MALLOC((size_t)(s->n_edges ? s->n_edges : 1) * sizeof(wz3__medge));
  m->faces = (wz3__mface *)WZ3_MALLOC((size_t)(s->n_faces ? s->n_faces : 1) * sizeof(wz3__mface));
  m->slots = (wz3__mslot *)WZ3_MALLOC((size_t)(s->n_slots ? s->n_slots : 1) * sizeof(wz3__mslot));
  m->parts = (int32_t *)WZ3_MALLOC((size_t)(s->n_parts ? s->n_parts : 1) * sizeof(int32_t));
  if (!m->verts || !m->edges || !m->faces || !m->slots || !m->parts) { wz3__mesh_free(m); return NULL; }
  memcpy(m->verts, s->verts, (size_t)s->n_verts * sizeof(wz3__mvert));
  memcpy(m->edges, s->edges, (size_t)s->n_edges * sizeof(wz3__medge));
  memcpy(m->faces, s->faces, (size_t)s->n_faces * sizeof(wz3__mface));
  memcpy(m->slots, s->slots, (size_t)s->n_slots * sizeof(wz3__mslot));
  memcpy(m->parts, s->parts, (size_t)s->n_parts * sizeof(int32_t));
  m->n_verts = s->n_verts; m->cap_verts = s->n_verts ? s->n_verts : 1;
  m->n_edges = s->n_edges; m->cap_edges = s->n_edges ? s->n_edges : 1;
  m->n_faces = s->n_faces; m->cap_faces = s->n_faces ? s->n_faces : 1;
  m->n_slots = s->n_slots; m->cap_slots = s->n_slots ? s->n_slots : 1;
  m->n_parts = s->n_parts; m->cap_parts = s->n_parts ? s->n_parts : 1;
  m->pivot = s->pivot;
  m->got_normals = s->got_normals;
  return m;
}

static int32_t wz3__mesh_attr_index(const wz3_mesh *m, int32_t attribute)
{
  const int32_t id = attribute & 15;
  if (!(m->vertex_mask & (1u << id))) return -1;
  switch (id) {
  case WZ3__MA_POS: return 0;
  case WZ3__MA_NORMAL: return 1;
  case WZ3__MA_TANGENT: return 2;
  case WZ3__MA_COLOR0: return 3;
  case WZ3__MA_UV0: return 4;
  case WZ3__MA_UV1: return 5;
  default: return -1;
  }
}

/* half-edge navigation: halfedge = edge * 2 + side */
static inline int32_t wz3__mh_vert(const wz3_mesh *m, int32_t h) { return m->edges[h >> 1].vert[h & 1]; }
static inline int32_t wz3__mh_face(const wz3_mesh *m, int32_t h) { return m->edges[h >> 1].face[h & 1]; }
static inline int32_t wz3__mh_next_face(const wz3_mesh *m, int32_t h) { return m->edges[h >> 1].next[h & 1]; }
static inline int32_t wz3__mh_prev_face(const wz3_mesh *m, int32_t h) { return m->edges[h >> 1].prev[h & 1]; }
static inline int32_t wz3__mh_next_vert(const wz3_mesh *m, int32_t h) { return wz3__mh_prev_face(m, h) ^ 1; }
static inline int32_t wz3__mh_prev_vert(const wz3_mesh *m, int32_t h) { return m->edges[h >> 1].next[(h ^ 1) & 1]; }

/* ---- hash map keyed by four words (corner adder, edge buckets) ---- */

typedef struct wz3__mkey {
  uint32_t k[4];
  int32_t value, value2;
  uint8_t used;
} wz3__mkey;

typedef struct wz3__kmap {
  wz3__mkey *e;
  uint32_t cap, count;
  int oom;
} wz3__kmap;

static uint32_t wz3__kmap_hash(const uint32_t k[4])
{
  uint32_t h = 0x811c9dc5u;
  for (int i = 0; i < 4; i++) {
    h ^= k[i];
    h *= 0x01000193u;
    h ^= h >> 15;
  }
  return h;
}

static void wz3__kmap_init(wz3__kmap *km, uint32_t expect)
{
  uint32_t cap = 64;
  while (cap < expect * 2u) cap *= 2;
  km->cap = cap;
  km->count = 0;
  km->oom = 0;
  km->e = (wz3__mkey *)WZ3_MALLOC((size_t)cap * sizeof(wz3__mkey));
  if (km->e) memset(km->e, 0, (size_t)cap * sizeof(wz3__mkey)); else km->oom = 1;
}
static void wz3__kmap_free(wz3__kmap *km) { WZ3_FREE(km->e); km->e = NULL; }

/* returns the entry for k, creating it (value = -1) when absent; *created tells which */
static wz3__mkey *wz3__kmap_find(wz3__kmap *km, const uint32_t k[4], int *created)
{
  if (km->oom) return NULL;
  if ((km->count + 1) * 2 > km->cap) {
    const uint32_t ncap = km->cap * 2;
    wz3__mkey *e = (wz3__mkey *)WZ3_MALLOC((size_t)ncap * sizeof(wz3__mkey));
    if (!e) { km->oom = 1; return NULL; }
    memset(e, 0, (size_t)ncap * sizeof(wz3__mkey));
    for (uint32_t i = 0; i < km->cap; i++) {
      if (!km->e[i].used) continue;
      uint32_t j = wz3__kmap_hash(km->e[i].k) & (ncap - 1);
      while (e[j].used) j = (j + 1) & (ncap - 1);
      e[j] = km->e[i];
    }
    WZ3_FREE(km->e);
    km->e = e;
    km->cap = ncap;
  }
  uint32_t j = wz3__kmap_hash(k) & (km->cap - 1);
  while (km->e[j].used) {
    if (!memcmp(km->e[j].k, k, 16)) { *created = 0; return &km->e[j]; }
    j = (j + 1) & (km->cap - 1);
  }
  memcpy(km->e[j].k, k, 16);
  km->e[j].value = km->e[j].value2 = -1;
  km->e[j].used = 1;
  km->count++;
  *created = 1;
  return &km->e[j];
}

/* ---- corner adder, polygons ---- */

typedef struct wz3__cadd { wz3_mesh *m; wz3__kmap map; } wz3__cadd;

static inline uint32_t wz3__f32_key(double v)
{
  float f = (float)v;
  if (f == 0.0f) f = 0.0f;                 /* -0 and 0 print as the same key in the source */
  uint32_t u;
  memcpy(&u, &f, 4);
  return u;
}

/* cornerAdder(...).add: position (and optional w), uv (2 or 4 values), face mask, key */
static int32_t wz3__cadd_add(wz3__cadd *c, const double pos[3], int has_w, double w, const double *uv, int uv_len,
                             uint8_t mask, const uint32_t *key)
{
  uint32_t k[4];
  if (key) memcpy(k, key, 16);
  else { k[0] = 0; k[1] = wz3__f32_key(pos[0]); k[2] = wz3__f32_key(pos[1]); k[3] = wz3__f32_key(pos[2]); }
  int created;
  wz3__mkey *entry = wz3__kmap_find(&c->map, k, &created);
  if (!entry) { c->m->oom = 1; return -1; }
  wz3__mvert *v;
  if (wz3__mesh_push_vertex(c->m, &v)) return -1;
  const int32_t index = (int32_t)c->m->n_verts - 1;
  v->next = v->re_index = index;
  v->first = created ? index : entry->value;
  if (created) entry->value = index;
  v->mask = mask;
  v->v[0][0] = (float)pos[0]; v->v[0][1] = (float)pos[1]; v->v[0][2] = (float)pos[2];
  v->v[0][3] = has_w ? (float)w : 1.0f;
  if (uv) {
    v->v[4][0] = (float)uv[0];
    v->v[4][1] = (float)uv[1];
    v->v[4][2] = uv_len > 2 ? (float)uv[2] : 0.0f;
    v->v[4][3] = uv_len > 3 ? (float)uv[3] : 1.0f;
  }
  return index;
}

typedef struct wz3__poly { uint32_t offset, count; uint8_t mask, id, select, used; int32_t material; } wz3__poly;
typedef struct wz3__pbuild {
  int32_t *verts; uint32_t n_verts, cap_verts;
  wz3__poly *polys; uint32_t n_polys, cap_polys;
  uint32_t max_n;
  int oom;
} wz3__pbuild;

static void wz3__pbuild_free(wz3__pbuild *p) { WZ3_FREE(p->verts); WZ3_FREE(p->polys); }

static int wz3__pbuild_push(wz3__pbuild *p, const int32_t *ids, uint32_t n, uint8_t mask, int32_t material)
{
  p->verts = (int32_t *)wz3__mesh_grow(p->verts, p->n_verts, &p->cap_verts, sizeof(int32_t), p->n_verts + n, &p->oom);
  p->polys = (wz3__poly *)wz3__mesh_grow(p->polys, p->n_polys, &p->cap_polys, sizeof(wz3__poly), p->n_polys + 1, &p->oom);
  if (p->oom) return -1;
  memcpy(p->verts + p->n_verts, ids, (size_t)n * sizeof(int32_t));
  p->polys[p->n_polys].offset = p->n_verts;
  p->polys[p->n_polys].count = n;
  p->polys[p->n_polys].mask = mask;
  p->polys[p->n_polys].id = 0;
  p->polys[p->n_polys].select = 0;
  p->polys[p->n_polys].used = 1;
  p->polys[p->n_polys].material = material;
  p->n_polys++;
  p->n_verts += n;
  if (n > p->max_n) p->max_n = n;
  return 0;
}

/* mesh.linkVertexCopies(): per First group, a cyclic Next list in index order */
static int wz3__mesh_link_copies(wz3_mesh *m)
{
  const uint32_t n = m->n_verts;
  if (!n) return 0;
  int32_t *head = (int32_t *)WZ3_MALLOC((size_t)n * 2 * sizeof(int32_t));
  if (!head) { m->oom = 1; return -1; }
  int32_t *last = head + n;
  for (uint32_t i = 0; i < n; i++) head[i] = last[i] = -1;
  for (uint32_t i = 0; i < n; i++) {
    wz3__mvert *v = &m->verts[i];
    int32_t first = v->first;
    if (first < 0 || (uint32_t)first >= n) first = v->first = (int32_t)i;
    if (head[first] < 0) head[first] = (int32_t)i;
    else m->verts[last[first]].next = (int32_t)i;
    last[first] = (int32_t)i;
    v->first = first;
    v->re_index = (int32_t)i;
  }
  for (uint32_t f = 0; f < n; f++)
    if (head[f] >= 0) m->verts[last[f]].next = head[f];
  WZ3_FREE(head);
  return 0;
}

/* mesh.setPolygons(): rebuild edges and faces from oriented loops, add deleted opposite faces for open sides */
static int wz3__mesh_set_polygons(wz3_mesh *m, const wz3__pbuild *pb)
{
  m->n_edges = 0;
  m->n_faces = 0;
  const uint32_t bound = pb->n_verts + 1;
  int32_t *scratch = (int32_t *)WZ3_MALLOC((size_t)(bound * 3 + pb->max_n + 1) * sizeof(int32_t));
  if (!scratch) { m->oom = 1; return -1; }
  int32_t *next_same = scratch, *end0 = scratch + bound, *end1 = scratch + bound * 2, *he = scratch + bound * 3;
  wz3__kmap buckets;
  wz3__kmap_init(&buckets, pb->n_verts + 8);
  if (buckets.oom) { WZ3_FREE(scratch); m->oom = 1; return -1; }
  int failed = 0;

  for (uint32_t pi = 0; pi < pb->n_polys && !failed; pi++) {
    const wz3__poly *poly = &pb->polys[pi];
    const int32_t *verts = pb->verts + poly->offset;
    const uint32_t n = poly->count;
    if (n < 3) continue;
    wz3__mface *face;
    if (wz3__mesh_push_face(m, &face)) { failed = 1; break; }
    const int32_t face_index = (int32_t)m->n_faces - 1;
    face->mask = poly->mask;
    face->id = poly->id;
    face->select = poly->select;
    face->used = poly->used;
    face->material = poly->material;
    uint32_t count = 0;
    for (uint32_t i = 0; i < n && !failed; i++) {
      const int32_t a = verts[i], b = verts[(i + 1) % n];
      const int32_t ca = m->verts[a].first, cb = m->verts[b].first;
      if (ca == cb) continue;
      const int32_t lo = ca < cb ? ca : cb, hi = ca < cb ? cb : ca;
      const int side = ca == lo ? 0 : 1;
      const uint32_t key[4] = {(uint32_t)lo, (uint32_t)hi, 0, 0};
      int created;
      wz3__mkey *entry = wz3__kmap_find(&buckets, key, &created);
      if (!entry) { failed = 1; break; }
      int32_t edge_index = -1;
      for (int32_t c = entry->value; c >= 0; c = next_same[c])
        if (m->edges[c].face[side] == -1) { edge_index = c; break; }
      if (edge_index < 0) {
        wz3__medge *e;
        if (wz3__mesh_push_edge(m, &e)) { failed = 1; break; }
        edge_index = (int32_t)m->n_edges - 1;
        end0[edge_index] = end1[edge_index] = -1;
        next_same[edge_index] = -1;
        if (entry->value < 0) entry->value = edge_index; else next_same[entry->value2] = edge_index;
        entry->value2 = edge_index;
      }
      wz3__medge *edge = &m->edges[edge_index];
      edge->face[side] = face_index;
      edge->vert[side] = a;
      (side ? end1 : end0)[edge_index] = b;
      he[count++] = edge_index * 2 + side;
    }
    if (failed) break;
    if (count < 3) { m->n_faces--; continue; }
    m->faces[face_index].edge = he[0];
    for (uint32_t i = 0; i < count; i++) {
      const int32_t current = he[i], side = current & 1;
      wz3__medge *edge = &m->edges[current >> 1];
      edge->prev[side] = he[(i + count - 1) % count];
      edge->next[side] = he[(i + 1) % count];
    }
  }
  if (!failed) {
    for (uint32_t ei = 0; ei < m->n_edges && !failed; ei++) {
      for (int side = 0; side < 2; side++) {
        if (m->edges[ei].face[side] != -1) continue;
        wz3__mface *face;
        if (wz3__mesh_push_face(m, &face)) { failed = 1; break; }
        face->material = 0;
        face->edge = (int32_t)ei * 2 + side;
        const int32_t face_index = (int32_t)m->n_faces - 1;
        wz3__medge *edge = &m->edges[ei];
        edge->face[side] = face_index;
        edge->vert[side] = (side ^ 1) ? end1[ei] : end0[ei];
        edge->next[side] = edge->prev[side] = face->edge;
      }
    }
  }
  wz3__kmap_free(&buckets);
  WZ3_FREE(scratch);
  return failed ? -1 : 0;
}

/* ---- selection ---- */

static void wz3__mesh_all_to_selection(wz3_mesh *m, int selected, int domains)
{
  if (domains & WZ3__ALL_VERT) for (uint32_t i = 0; i < m->n_verts; i++) m->verts[i].select = selected ? 1 : 0;
  if (domains & WZ3__ALL_FACE)
    for (uint32_t i = 0; i < m->n_faces; i++) m->faces[i].select = (selected && m->faces[i].material != 0) ? 1 : 0;
  if (domains & WZ3__ALL_EDGE) for (uint32_t i = 0; i < m->n_edges; i++) m->edges[i].select = selected ? 1 : 0;
}

static void wz3__mesh_mask_to_selection(wz3_mesh *m, uint32_t mask)
{
  if (mask & 0x00ff0000u) {
    const uint32_t bits = (mask >> 16) & 255u;
    for (uint32_t i = 0; i < m->n_verts; i++) m->verts[i].select = (m->verts[i].mask & bits) != 0;
  }
  if (mask & 0x0000ff00u) {
    const uint32_t bits = (mask >> 8) & 255u;
    for (uint32_t i = 0; i < m->n_faces; i++) m->faces[i].select = (m->faces[i].mask & bits) != 0;
  }
  if (mask & 255u) {
    const uint32_t bits = mask & 255u;
    for (uint32_t i = 0; i < m->n_edges; i++) m->edges[i].select = (m->edges[i].mask & bits) != 0;
  }
}

/* selElem(): update one record's mask byte from a selection state */
static uint8_t wz3__sel_elem(uint8_t record_mask, uint32_t mask, int state, int mode)
{
  mask &= 255u;
  uint32_t value = record_mask;
  switch (mode & 3) {
  case WZ3__SEL_ADD: if (state) value |= mask; break;
  case WZ3__SEL_SUB: if (state) value &= ~mask; break;
  case WZ3__SEL_SET: value = state ? (value | mask) : (value & ~mask); break;
  default: value = state ? (value & ~mask) : (value | mask); break;
  }
  return (uint8_t)(value & 255u);
}

static void wz3__mesh_selection_to_mask(wz3_mesh *m, uint32_t mask, int mode)
{
  if (mask & 0x00ff0000u)
    for (uint32_t i = 0; i < m->n_verts; i++) m->verts[i].mask = wz3__sel_elem(m->verts[i].mask, mask >> 16, m->verts[i].select, mode);
  if (mask & 0x0000ff00u)
    for (uint32_t i = 0; i < m->n_faces; i++) m->faces[i].mask = wz3__sel_elem(m->faces[i].mask, mask >> 8, m->faces[i].select, mode);
  if (mask & 255u)
    for (uint32_t i = 0; i < m->n_edges; i++) m->edges[i].mask = wz3__sel_elem(m->edges[i].mask, mask, m->edges[i].select, mode);
}

static void wz3__mesh_all_to_mask(wz3_mesh *m, uint32_t mask, int mode)
{
  if (mask & 0x00ff0000u)
    for (uint32_t i = 0; i < m->n_verts; i++) m->verts[i].mask = wz3__sel_elem(m->verts[i].mask, mask >> 16, 1, mode);
  if (mask & 0x0000ff00u)
    for (uint32_t i = 0; i < m->n_faces; i++)
      if (m->faces[i].material) m->faces[i].mask = wz3__sel_elem(m->faces[i].mask, mask >> 8, 1, mode);
  if (mask & 255u)
    for (uint32_t i = 0; i < m->n_edges; i++) m->edges[i].mask = wz3__sel_elem(m->edges[i].mask, mask, 1, mode);
}

/* faceToVertex(): select every vertex of a selected face */
static void wz3__mesh_face_to_vertex(wz3_mesh *m)
{
  wz3__mesh_all_to_selection(m, 0, WZ3__ALL_VERT);
  for (uint32_t f = 0; f < m->n_faces; f++) {
    if (!m->faces[f].select || m->faces[f].edge < 0) continue;
    const int32_t start = m->faces[f].edge;
    int32_t h = start;
    do {
      m->verts[wz3__mh_vert(m, h)].select = 1;
      h = wz3__mh_next_face(m, h);
    } while (h != start);
  }
}

/* vertexToFaceEdge(): edges and faces whose vertices are all selected */
static void wz3__mesh_vertex_to_face_edge(wz3_mesh *m)
{
  for (uint32_t i = 0; i < m->n_edges; i++)
    m->edges[i].select = m->verts[wz3__mh_vert(m, (int32_t)i * 2)].select && m->verts[wz3__mh_vert(m, (int32_t)i * 2 + 1)].select;
  for (uint32_t f = 0; f < m->n_faces; f++) {
    if (!m->faces[f].material) continue;
    const int32_t start = m->faces[f].edge;
    int all = 1;
    if (start >= 0) {
      int32_t h = start;
      do {
        if (!m->verts[wz3__mh_vert(m, h)].select) all = 0;
        h = wz3__mh_next_face(m, h);
      } while (h != start);
    }
    m->faces[f].select = (uint8_t)all;
  }
}

/* checkedCopy(): a private copy with the selection from `mask` (bit 31: keep; 0: select everything) */
static wz3_mesh *wz3__mesh_checked_copy(const wz3_mesh *source, uint32_t mask)
{
  wz3_mesh *m = wz3__mesh_clone(source);
  if (!m) return NULL;
  if (!(mask & 0x80000000u)) {
    if (mask) wz3__mesh_mask_to_selection(m, mask);
    else wz3__mesh_all_to_selection(m, 1, 7);
  }
  return m;
}

/* ---- transforms ---- */

static void wz3__mesh_transform_vertices(wz3_mesh *m, const float matrix[16], int32_t source_attr, int32_t dest_attr,
                                         int32_t start_in, int32_t end_in, int has_end)
{
  const int32_t si = wz3__mesh_attr_index(m, source_attr), di = wz3__mesh_attr_index(m, dest_attr & 15);
  if (si < 0 || di < 0) return;
  const int additive = (dest_attr & 0x10) != 0;
  const int32_t color_index = wz3__mesh_attr_index(m, WZ3__MA_COLOR0);
  int32_t start = start_in < 0 ? 0 : start_in;
  int32_t end = has_end ? (end_in < start ? start : end_in) : (int32_t)m->n_verts;
  if (end > (int32_t)m->n_verts) end = (int32_t)m->n_verts;
  for (int32_t index = start; index < end; index++) {
    wz3__mvert *v = &m->verts[index];
    if (!v->select) continue;
    const float *src = v->v[si];
    float *dst = v->v[di];
    const double x = src[0], y = src[1], z = src[2];
    const double w = si == color_index ? 1.0 : (double)src[3];
    float t[4];
    for (int i = 0; i < 4; i++)
      t[i] = (float)((double)matrix[i] * x + (double)matrix[4 + i] * y + (double)matrix[8 + i] * z + (double)matrix[12 + i] * w);
    if (additive) {
      dst[0] = (float)((double)dst[0] + t[0]);
      dst[1] = (float)((double)dst[1] + t[1]);
      dst[2] = (float)((double)dst[2] + t[2]);
    } else {
      const float w_keep = src[3];
      dst[0] = t[0]; dst[1] = t[1]; dst[2] = t[2];
      dst[3] = w_keep;
    }
  }
}

/* sMatrix::PivotTransform for the old GenMesh path */
static void wz3__mesh_pivot_transform(const wz3_mesh *m, const float matrix[16], int32_t attribute, float out[16])
{
  const int32_t pivot = m->pivot;
  const int32_t ai = wz3__mesh_attr_index(m, attribute);
  if (pivot < 0 || (uint32_t)pivot >= m->n_verts || ai < 0) { memcpy(out, matrix, 16 * sizeof(float)); return; }
  const float *point = m->verts[pivot].v[ai];
  float translate[16];
  wz3_mat4_identity(translate);
  translate[12] = -point[0];
  translate[13] = -point[1];
  translate[14] = -point[2];
  wz3_mat4_mul_a(translate, matrix, out);
  out[12] = (float)((double)out[12] + point[0]);
  out[13] = (float)((double)out[13] + point[1]);
  out[14] = (float)((double)out[14] + point[2]);
}

/* ---- append ---- */

static int wz3__mesh_append(wz3_mesh *m, const wz3_mesh *o)
{
  if (o->vertex_mask != m->vertex_mask) return 0;
  const uint32_t vo = m->n_verts, eo = m->n_edges, fo = m->n_faces;
  int32_t remap_small[16];
  int32_t *remap = o->n_slots <= 16 ? remap_small : (int32_t *)WZ3_MALLOC((size_t)o->n_slots * sizeof(int32_t));
  if (!remap) { m->oom = 1; return -1; }
  remap[0] = 0;
  for (uint32_t i = 1; i < o->n_slots; i++) {
    int32_t found = -1;
    for (uint32_t j = 1; j < m->n_slots; j++)
      if (m->slots[j].material == o->slots[i].material && m->slots[j].pass == o->slots[i].pass) { found = (int32_t)j; break; }
    if (found < 0) {
      found = (int32_t)m->n_slots;
      if (wz3__mesh_push_slot(m, o->slots[i].material, o->slots[i].pass)) { if (remap != remap_small) WZ3_FREE(remap); return -1; }
    }
    remap[i] = found;
  }
  if (m->n_verts + o->n_verts > m->cap_verts) { WZ3__MESH_RESERVE(m, verts, n_verts, cap_verts, wz3__mvert, m->n_verts + o->n_verts); }
  if (m->n_edges + o->n_edges > m->cap_edges) { WZ3__MESH_RESERVE(m, edges, n_edges, cap_edges, wz3__medge, m->n_edges + o->n_edges); }
  if (m->n_faces + o->n_faces > m->cap_faces) { WZ3__MESH_RESERVE(m, faces, n_faces, cap_faces, wz3__mface, m->n_faces + o->n_faces); }
  if (m->n_parts + o->n_parts > m->cap_parts) { WZ3__MESH_RESERVE(m, parts, n_parts, cap_parts, int32_t, m->n_parts + o->n_parts); }
  for (uint32_t i = 0; i < o->n_verts; i++) {
    wz3__mvert *v = &m->verts[m->n_verts++];
    *v = o->verts[i];
    if (v->next >= 0) v->next += (int32_t)vo;
    if (v->first >= 0) v->first += (int32_t)vo;
    if (v->re_index >= 0) v->re_index += (int32_t)vo;
  }
  for (uint32_t i = 0; i < o->n_edges; i++) {
    wz3__medge *e = &m->edges[m->n_edges++];
    *e = o->edges[i];
    for (int s = 0; s < 2; s++) {
      if (e->next[s] >= 0) e->next[s] += (int32_t)eo * 2;
      if (e->prev[s] >= 0) e->prev[s] += (int32_t)eo * 2;
      if (e->face[s] >= 0) e->face[s] += (int32_t)fo;
      if (e->vert[s] >= 0) e->vert[s] += (int32_t)vo;
    }
  }
  for (uint32_t i = 0; i < o->n_faces; i++) {
    wz3__mface *f = &m->faces[m->n_faces++];
    *f = o->faces[i];
    if (f->edge >= 0) f->edge += (int32_t)eo * 2;
    f->material = (f->material >= 0 && (uint32_t)f->material < o->n_slots) ? remap[f->material] : 0;
  }
  for (uint32_t i = 0; i < o->n_parts; i++) m->parts[m->n_parts++] = (int32_t)((uint32_t)o->parts[i] + fo);
  if (remap != remap_small) WZ3_FREE(remap);
  m->pivot = -1;
  m->got_normals = 0;
  return 0;
}

/* ---- normals, bounds, summary ---- */

static int wz3__mesh_calc_normals(wz3_mesh *m)
{
  const uint32_t nv = m->n_verts;
  int32_t *vertex_edges = (int32_t *)WZ3_MALLOC(((size_t)nv + 1) * sizeof(int32_t));
  if (!vertex_edges) { m->oom = 1; return -1; }
  for (uint32_t i = 0; i < nv; i++) vertex_edges[i] = -1;
  for (int32_t h = 0; h < (int32_t)m->n_edges * 2; h++) {
    const int32_t v = wz3__mh_vert(m, h);
    if (v >= 0 && (uint32_t)v < nv) vertex_edges[v] = h;
  }
  const int64_t limit = (int64_t)m->n_edges * 2 + 2;
  int bad = 0;
  for (int component = 0; component < 2 && !bad; component++) {
    const int32_t crease_mask = component ? WZ3__MF_UV0 : WZ3__MF_NORMAL;
    for (uint32_t vertex = 0; vertex < nv && !bad; vertex++) {
      int32_t edge = vertex_edges[vertex];
      if (edge < 0) continue;
      int32_t end = edge;
      int64_t guard = 0;
      do {
        if (m->edges[edge >> 1].crease & crease_mask) end = edge;
        else edge = wz3__mh_prev_vert(m, edge);
        if (++guard > limit) { bad = 1; break; }
      } while (edge != end);
      if (bad) break;
      end = edge;
      const wz3__mvert *v0 = &m->verts[wz3__mh_vert(m, edge)];
      const wz3__mvert *v2 = &m->verts[wz3__mh_vert(m, wz3__mh_next_face(m, edge))];
      double t2x = (double)v2->v[0][0] - v0->v[0][0], t2y = (double)v2->v[0][1] - v0->v[0][1], t2z = (double)v2->v[0][2] - v0->v[0][2];
      double t2l = t2x * t2x + t2y * t2y + t2z * t2z;
      double ax = 0, ay = 0, az = 0;
      int exit_ = 0, was_exit = 0;
      guard = 0;
      do {
        was_exit = exit_;
        const double t1x = t2x, t1y = t2y, t1z = t2z, t1l = t2l;
        const wz3__mvert *v1 = v2;
        v2 = &m->verts[wz3__mh_vert(m, was_exit ? wz3__mh_prev_face(m, wz3__mh_prev_vert(m, edge)) : wz3__mh_next_face(m, edge))];
        t2x = (double)v2->v[0][0] - v0->v[0][0];
        t2y = (double)v2->v[0][1] - v0->v[0][1];
        t2z = (double)v2->v[0][2] - v0->v[0][2];
        t2l = t2x * t2x + t2y * t2y + t2z * t2z;
        if (component == 0) {
          if (t1l * t2l > 1e-20) {
            const double scale = 1.0 / (t1l * t2l);
            ax += (t1y * t2z - t1z * t2y) * scale;
            ay += (t1z * t2x - t1x * t2z) * scale;
            az += (t1x * t2y - t1y * t2x) * scale;
          }
        } else {
          const double c1 = (double)v0->v[4][1] - v1->v[4][1];
          const double c2 = (double)v2->v[4][1] - v0->v[4][1];
          double determinant = ((double)v1->v[4][0] - v0->v[4][0]) * c2 + ((double)v2->v[4][0] - v0->v[4][0]) * c1;
          if ((determinant < 0 ? -determinant : determinant) > 1e-20) {
            determinant = 1.0 / determinant;
            double tx = t1x * c2 * determinant + t2x * c1 * determinant;
            double ty = t1y * c2 * determinant + t2y * c1 * determinant;
            double tz = t1z * c2 * determinant + t2z * c1 * determinant;
            const double length2 = tx * tx + ty * ty + tz * tz;
            if (length2 > 1e-20) {
              const double inverse = 1.0 / sqrt(length2);
              tx *= inverse; ty *= inverse; tz *= inverse;
              ax += tx; ay += ty; az += tz;
            }
          }
        }
        edge = wz3__mh_next_vert(m, edge);
        exit_ = edge == end || (m->edges[edge >> 1].crease & crease_mask) != 0;
        if (++guard > limit) { bad = 1; break; }
      } while (!was_exit);
      if (bad) break;
      float *output = component ? m->verts[vertex].v[2] : m->verts[vertex].v[1];
      if (component) {
        const float *normal = m->verts[vertex].v[1];
        const double parallel = ax * normal[0] + ay * normal[1] + az * normal[2];
        ax -= (double)normal[0] * parallel;
        ay -= (double)normal[1] * parallel;
        az -= (double)normal[2] * parallel;
      }
      const double length2 = ax * ax + ay * ay + az * az;
      if (length2 > 1e-20) {
        const double inverse = 1.0 / sqrt(length2);
        output[0] = (float)(ax * inverse);
        output[1] = (float)(ay * inverse);
        output[2] = (float)(az * inverse);
      } else {
        output[0] = 1.0f; output[1] = output[2] = 0.0f;
      }
      output[3] = 0.0f;
    }
  }
  WZ3_FREE(vertex_edges);
  if (bad) return -1;
  m->got_normals = 1;
  return 0;
}

static int wz3__mesh_need_normals(wz3_mesh *m) { return m->got_normals ? 0 : wz3__mesh_calc_normals(m); }

/* bounds(true): over the vertices of live faces, using each vertex's First position */
static int wz3__mesh_bounds(const wz3_mesh *m, float min[3], float max[3])
{
  const uint32_t nv = m->n_verts;
  uint8_t *referenced = (uint8_t *)WZ3_MALLOC((size_t)nv + 1);
  if (!referenced) return -1;
  memset(referenced, 0, (size_t)nv + 1);
  for (uint32_t f = 0; f < m->n_faces; f++) {
    if (!m->faces[f].material || m->faces[f].edge < 0) continue;
    const int32_t start = m->faces[f].edge;
    int32_t h = start;
    do {
      const int32_t v = wz3__mh_vert(m, h);
      if (v >= 0 && (uint32_t)v < nv) referenced[v] = 1;
      h = wz3__mh_next_face(m, h);
    } while (h != start);
  }
  min[0] = min[1] = min[2] = INFINITY;
  max[0] = max[1] = max[2] = -INFINITY;
  for (uint32_t i = 0; i < nv; i++) {
    if (!referenced[i]) continue;
    const int32_t first = m->verts[i].first;
    const float *p = (first >= 0 && (uint32_t)first < nv) ? m->verts[first].v[0] : m->verts[i].v[0];
    for (int a = 0; a < 3; a++) {
      if (p[a] < min[a]) min[a] = p[a];
      if (p[a] > max[a]) max[a] = p[a];
    }
  }
  WZ3_FREE(referenced);
  if (!(min[0] - min[0] == 0.0f)) { min[0] = min[1] = min[2] = max[0] = max[1] = max[2] = 0.0f; }
  return 0;
}

static uint32_t wz3__mesh_hash_word(uint32_t h, uint32_t v)
{
  h = (h ^ (v & 255u)) * 0x01000193u;
  h = (h ^ ((v >> 8) & 255u)) * 0x01000193u;
  h = (h ^ ((v >> 16) & 255u)) * 0x01000193u;
  return (h ^ (v >> 24)) * 0x01000193u;
}

static uint32_t wz3__mesh_float_bits(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }

/* the topology and vertex hashes of Mesh.summary(), plus the number of live faces */
static void wz3__mesh_hashes(const wz3_mesh *m, uint32_t *topology_out, uint32_t *vertex_out, uint32_t *active_out)
{
  uint32_t topology = 0x811c9dc5u, vertex_hash = 0x811c9dc5u;
  const int attribute_count = (m->vertex_mask & WZ3__MF_UV1) ? 6 : 5;
  for (uint32_t i = 0; i < m->n_verts; i++) {
    const wz3__mvert *v = &m->verts[i];
    topology = wz3__mesh_hash_word(topology, (uint32_t)v->mask | ((uint32_t)v->id << 8) | ((uint32_t)(v->select ? 1 : 0) << 16) |
                                                 ((uint32_t)(v->used ? 1 : 0) << 17));
    topology = wz3__mesh_hash_word(topology, (uint32_t)v->first);
    topology = wz3__mesh_hash_word(topology, (uint32_t)v->next);
    for (int a = 0; a < attribute_count; a++)
      for (int c = 0; c < 4; c++) vertex_hash = wz3__mesh_hash_word(vertex_hash, wz3__mesh_float_bits(v->v[a][c]));
  }
  for (uint32_t i = 0; i < m->n_edges; i++) {
    const wz3__medge *e = &m->edges[i];
    topology = wz3__mesh_hash_word(topology, (uint32_t)e->mask | ((uint32_t)e->crease << 8));
    for (int s = 0; s < 2; s++) {
      topology = wz3__mesh_hash_word(topology, (uint32_t)e->next[s]);
      topology = wz3__mesh_hash_word(topology, (uint32_t)e->prev[s]);
      topology = wz3__mesh_hash_word(topology, (uint32_t)e->face[s]);
      topology = wz3__mesh_hash_word(topology, (uint32_t)e->vert[s]);
    }
  }
  uint32_t active = 0;
  for (uint32_t i = 0; i < m->n_faces; i++) {
    const wz3__mface *f = &m->faces[i];
    if (f->material) active++;
    topology = wz3__mesh_hash_word(topology, (uint32_t)f->mask | ((uint32_t)f->material << 8) | ((uint32_t)(f->used ? 1 : 0) << 24));
    topology = wz3__mesh_hash_word(topology, (uint32_t)f->edge);
  }
  *topology_out = topology;
  *vertex_out = vertex_hash;
  *active_out = active;
}

/* Mesh.summary() folded into one word, the way the reference fixtures (tests/data) do */
static int wz3__mesh_summary(const wz3_mesh *m, uint32_t *combined)
{
  uint32_t topology, vertex_hash, active;
  wz3__mesh_hashes(m, &topology, &vertex_hash, &active);
  float min[3], max[3];
  if (wz3__mesh_bounds(m, min, max)) return -1;
  uint32_t h = 0x811c9dc5u;
  const uint32_t words[11] = {active, m->n_slots, m->n_parts, topology, vertex_hash,
                              wz3__mesh_float_bits(min[0]), wz3__mesh_float_bits(min[1]), wz3__mesh_float_bits(min[2]),
                              wz3__mesh_float_bits(max[0]), wz3__mesh_float_bits(max[1]), wz3__mesh_float_bits(max[2])};
  for (int i = 0; i < 11; i++) h = (h ^ words[i]) * 0x01000193u;
  *combined = h;
  return 0;
}

/* ---- primitives ---- */

#define WZ3__TAU (6.283185307179586)
#define WZ3__PI_D (3.141592653589793)

/* orient(): flip the loop (keeping vertex 0) when its normal points against `target` */
static int wz3__pbuild_push_oriented(wz3__pbuild *pb, const wz3_mesh *m, const int32_t *ids, uint32_t n, const double target[3],
                                     uint8_t mask, int32_t material)
{
  int32_t tmp[512];
  if (n > 512 || n < 3) return -1;
  for (uint32_t i = 0; i < n; i++)
    if (ids[i] < 0 || (uint32_t)ids[i] >= m->n_verts) return -1;
  const float *a = m->verts[ids[0]].v[0], *b = m->verts[ids[1]].v[0], *c = m->verts[ids[2]].v[0];
  const double ux = (double)b[0] - a[0], uy = (double)b[1] - a[1], uz = (double)b[2] - a[2];
  const double vx = (double)c[0] - a[0], vy = (double)c[1] - a[1], vz = (double)c[2] - a[2];
  const double nx = uy * vz - uz * vy, ny = uz * vx - ux * vz, nz = ux * vy - uy * vx;
  if (nx * target[0] + ny * target[1] + nz * target[2] < 0) {
    tmp[0] = ids[0];
    for (uint32_t i = 1; i < n; i++) tmp[i] = ids[n - i];
  } else {
    memcpy(tmp, ids, (size_t)n * sizeof(int32_t));
  }
  return wz3__pbuild_push(pb, tmp, n, mask, material);
}

/* markUVDiscontinuities(): UV0 crease on edges whose two wedges disagree at both ends */
static void wz3__mesh_mark_uv_discontinuities(wz3_mesh *m)
{
  for (uint32_t ei = 0; ei < m->n_edges; ei++) {
    const int32_t h0 = (int32_t)ei * 2, h1 = h0 + 1;
    const float *start0 = m->verts[wz3__mh_vert(m, h0)].v[4];
    const float *end0 = m->verts[wz3__mh_vert(m, wz3__mh_next_face(m, h0))].v[4];
    const float *start1 = m->verts[wz3__mh_vert(m, h1)].v[4];
    const float *end1 = m->verts[wz3__mh_vert(m, wz3__mh_next_face(m, h1))].v[4];
    const int first = start0[0] != end1[0] || start0[1] != end1[1];
    const int second = end0[0] != start1[0] || end0[1] != start1[1];
    if (first && second) m->edges[ei].crease |= WZ3__MF_UV0;
  }
}

typedef struct wz3__build {
  wz3_mesh *m;
  wz3__cadd add;
  wz3__pbuild pb;
} wz3__build;

static int wz3__build_begin(wz3__build *b, uint32_t expect)
{
  memset(b, 0, sizeof *b);
  b->m = wz3__mesh_new(WZ3__MESH_DEFAULT_MASK);
  if (!b->m) return -1;
  b->add.m = b->m;
  wz3__kmap_init(&b->add.map, expect);
  if (b->add.map.oom) { wz3__mesh_free(b->m); b->m = NULL; return -1; }
  return 0;
}

static void wz3__build_abort(wz3__build *b)
{
  wz3__kmap_free(&b->add.map);
  wz3__pbuild_free(&b->pb);
  wz3__mesh_free(b->m);
  b->m = NULL;
}

/* link First cycles, build the half-edges, and release the builder state */
static int wz3__build_finish_topology(wz3__build *b)
{
  wz3__kmap_free(&b->add.map);
  int bad = b->m->oom || b->pb.oom;
  if (!bad && wz3__mesh_link_copies(b->m)) bad = 1;
  if (!bad && wz3__mesh_set_polygons(b->m, &b->pb)) bad = 1;
  wz3__pbuild_free(&b->pb);
  if (bad) { wz3__mesh_free(b->m); b->m = NULL; return -1; }
  return 0;
}

static wz3_mesh *wz3__mesh_cube(int32_t tx, int32_t ty, int32_t tz, int32_t flags, const double srt_in[9])
{
  tx = tx < 1 ? 1 : tx; ty = ty < 1 ? 1 : ty; tz = tz < 1 ? 1 : tz;
  float srt[9];
  for (int i = 0; i < 9; i++) srt[i] = (float)srt_in[i];
  const double uv_scale[3] = {(flags & 8) ? srt[0] : 1.0, (flags & 8) ? srt[1] : 1.0, (flags & 8) ? srt[2] : 1.0};
  wz3__build b;
  if (wz3__build_begin(&b, (uint32_t)(2 * ((tx + 1) * (ty + 1) + (tx + 1) * (tz + 1) + (ty + 1) * (tz + 1))))) return NULL;
  for (int32_t face = 0; face < 6 && !b.m->oom; face++) {
    const int axis = face >> 1;
    const double sign = (face & 1) ? 1.0 : -1.0;
    const int32_t us = axis == 0 ? ty : tx, vs = axis == 2 ? ty : tz;
    int32_t *grid = (int32_t *)WZ3_MALLOC((size_t)(us + 1) * (size_t)(vs + 1) * sizeof(int32_t));
    if (!grid) { b.m->oom = 1; break; }
    for (int32_t u = 0; u <= us; u++)
      for (int32_t v = 0; v <= vs; v++) {
        double p[3], uv[2];
        const double fu = (double)u / (double)us - 0.5, fv = (double)v / (double)vs - 0.5;
        if (axis == 0) { p[0] = sign * 0.5; p[1] = fu; p[2] = fv; }
        else if (axis == 1) { p[0] = fu; p[1] = sign * 0.5; p[2] = fv; }
        else { p[0] = fu; p[1] = fv; p[2] = sign * 0.5; }
        if (flags & 4) p[1] += 0.5;
        if (axis == 0) {
          uv[0] = sign * p[2] * uv_scale[2] + 0.5 * uv_scale[2];
          uv[1] = -p[1] * uv_scale[1] + ((flags & 4) ? uv_scale[1] : 0.5 * uv_scale[1]);
        } else if (axis == 1) {
          uv[0] = p[0] * uv_scale[0] + 0.5 * uv_scale[0];
          uv[1] = -sign * p[2] * uv_scale[2] + 0.5 * uv_scale[2];
        } else {
          uv[0] = -sign * p[0] * uv_scale[0] + 0.5 * uv_scale[0];
          uv[1] = -p[1] * uv_scale[1] + ((flags & 4) ? uv_scale[1] : 0.5 * uv_scale[1]);
        }
        double u_offset = 0.0;
        if (flags & 2) {
          if (face == 4) u_offset = uv_scale[2];
          else if (face == 1) u_offset = uv_scale[2] + uv_scale[0];
          else if (face == 5) u_offset = uv_scale[2] * 2 + uv_scale[0];
        }
        uv[0] += u_offset;
        grid[u * (vs + 1) + v] = wz3__cadd_add(&b.add, p, 0, 1.0, uv, 2, (uint8_t)(1 << face), NULL);
      }
    double target[3] = {0, 0, 0};
    target[axis] = sign;
    for (int32_t u = 0; u < us; u++)
      for (int32_t v = 0; v < vs; v++) {
        const int32_t ids[4] = {grid[u * (vs + 1) + v], grid[(u + 1) * (vs + 1) + v], grid[(u + 1) * (vs + 1) + v + 1],
                                grid[u * (vs + 1) + v + 1]};
        if (wz3__pbuild_push_oriented(&b.pb, b.m, ids, 4, target, (uint8_t)(1 << face), 1)) b.m->oom = 1;
      }
    WZ3_FREE(grid);
  }
  if (wz3__build_finish_topology(&b)) return NULL;
  wz3_mesh *m = b.m;
  for (uint32_t i = 0; i < m->n_edges; i++) {
    wz3__medge *edge = &m->edges[i];
    const wz3__mface *f0 = &m->faces[edge->face[0]], *f1 = &m->faces[edge->face[1]];
    edge->mask = (uint8_t)(((f0->material ? f0->mask : 0) | (f1->material ? f1->mask : 0)) & 255);
    if (f0->mask != f1->mask) edge->crease = (flags & 1) ? 0x1fe : WZ3__MF_UV0;
  }
  if (wz3__mesh_push_part(m, 0)) { wz3__mesh_free(m); return NULL; }
  /* applySRT */
  float matrix[16];
  wz3_mat4_srt(srt, matrix);
  wz3__mesh_all_to_selection(m, 1, 7);
  wz3__mesh_transform_vertices(m, matrix, WZ3__MA_POS, WZ3__MA_POS, 0, 0, 0);
  m->got_normals = 0;
  return m;
}

static wz3_mesh *wz3__mesh_grid(int32_t mode, int32_t tess_u, int32_t tess_v)
{
  tess_u = tess_u < 1 ? 1 : tess_u;
  tess_v = tess_v < 1 ? 1 : tess_v;
  wz3__build b;
  if (wz3__build_begin(&b, (uint32_t)(2 * (tess_u + 1) * (tess_v + 1)))) return NULL;
  for (int side = 0; side < 2 && !b.m->oom; side++) {
    int32_t *grid = (int32_t *)WZ3_MALLOC((size_t)(tess_u + 1) * (size_t)(tess_v + 1) * sizeof(int32_t));
    if (!grid) { b.m->oom = 1; break; }
    for (int32_t u = 0; u <= tess_u; u++)
      for (int32_t v = 0; v <= tess_v; v++) {
        const double p[3] = {(double)u / (double)tess_u - 0.5, 0.0, 0.5 - (double)v / (double)tess_v};
        uint32_t mask = side == 0 ? 1u : 2u;
        if (v == 0 || v == tess_v) mask |= 4; else mask |= 8;
        if (u == 0 || u == tess_u) mask |= 16; else mask |= 32;
        if (mask & (4 | 16)) mask |= 64; else mask |= 128;
        const double uv[2] = {side == 0 ? 1.0 - (double)u / (double)tess_u : (double)u / (double)tess_u, (double)v / (double)tess_v};
        const int perimeter = u == 0 || u == tess_u || v == 0 || v == tess_v;
        const uint32_t key[4] = {perimeter ? 1u : 2u, perimeter ? (uint32_t)u : (uint32_t)side, perimeter ? (uint32_t)v : (uint32_t)u,
                                 perimeter ? 0u : (uint32_t)v};
        grid[u * (tess_v + 1) + v] = wz3__cadd_add(&b.add, p, 0, 1.0, uv, 2, (uint8_t)mask, key);
      }
    const double target[3] = {0, side == 0 ? -1.0 : 1.0, 0};
    for (int32_t u = 0; u < tess_u; u++)
      for (int32_t v = 0; v < tess_v; v++) {
        const int32_t g00 = grid[u * (tess_v + 1) + v], g10 = grid[(u + 1) * (tess_v + 1) + v],
                      g11 = grid[(u + 1) * (tess_v + 1) + v + 1], g01 = grid[u * (tess_v + 1) + v + 1];
        const int32_t ids_front[4] = {g00, g10, g11, g01}, ids_back[4] = {g10, g00, g01, g11};
        uint32_t mask = side == 0 ? 1u : 2u;
        if (v == 0 || v + 1 == tess_v) mask |= 4; else mask |= 8;
        if (u == 0 || u + 1 == tess_u) mask |= 16; else mask |= 32;
        if (mask & (4 | 16)) mask |= 64; else mask |= 128;
        if (wz3__pbuild_push_oriented(&b.pb, b.m, side == 0 ? ids_front : ids_back, 4, target, (uint8_t)mask,
                                      (side == 1 && !(mode & 1)) ? 0 : 1))
          b.m->oom = 1;
      }
    WZ3_FREE(grid);
  }
  if (wz3__build_finish_topology(&b)) return NULL;
  wz3_mesh *m = b.m;
  for (uint32_t i = 0; i < m->n_edges; i++) {
    wz3__medge *edge = &m->edges[i];
    if (((m->faces[edge->face[0]].mask ^ m->faces[edge->face[1]].mask) & 3) == 3) edge->crease = 0xfe;
  }
  if (wz3__mesh_push_part(m, 0)) { wz3__mesh_free(m); return NULL; }
  m->got_normals = 0;
  return m;
}

static wz3_mesh *wz3__mesh_cylinder(int32_t tx, int32_t ty, int32_t mode, int32_t tz, int32_t arc)
{
  tx = tx < 3 ? 3 : tx; ty = ty < 1 ? 1 : ty; tz = tz < 1 ? 1 : tz;
  arc = arc < 0 ? 0 : (arc > tx - 1 ? tx - 1 : arc);
  const double y_offset = (mode & 2) ? 0.5 : 0.0;
  wz3__build b;
  if (wz3__build_begin(&b, (uint32_t)((tx + 1) * (ty + 1) + 2 * (tx + 1) * (tz + 1) + 8))) return NULL;
  if (arc == 0) {
    int32_t *side = (int32_t *)WZ3_MALLOC((size_t)(ty + 1) * (size_t)(tx + 1) * sizeof(int32_t));
    int32_t *prev = (int32_t *)WZ3_MALLOC((size_t)(tx + 1) * 2 * sizeof(int32_t));
    if (!side || !prev) b.m->oom = 1;
    else {
      int32_t *cur = prev + (tx + 1);
      for (int32_t level = 0; level <= ty; level++)
        for (int32_t j = 0; j <= tx; j++) {
          const double angle = WZ3__TAU * (double)(j % tx) / (double)tx;
          const double p[3] = {0.5 * wz3__sin(angle), 0.5 - (double)level / (double)ty + y_offset, -0.5 * wz3__cos(angle)};
          const double uv[2] = {(double)j / (double)tx, (double)level / (double)ty};
          const uint32_t key[4] = {3u, (uint32_t)level, (uint32_t)(j % tx), 0u};
          side[level * (tx + 1) + j] = wz3__cadd_add(&b.add, p, 0, 1.0, uv, 2, 0, key);
        }
      for (int32_t level = 0; level < ty; level++)
        for (int32_t j = 0; j < tx; j++) {
          const int32_t ids[4] = {side[level * (tx + 1) + j], side[(level + 1) * (tx + 1) + j],
                                  side[(level + 1) * (tx + 1) + j + 1], side[level * (tx + 1) + j + 1]};
          const double angle = WZ3__TAU * ((double)j + 0.5) / (double)tx;
          const double target[3] = {wz3__sin(angle), 0.0, -wz3__cos(angle)};
          if (wz3__pbuild_push_oriented(&b.pb, b.m, ids, 4, target, 0, 1)) b.m->oom = 1;
        }
      for (int cap = 0; cap < 2; cap++) {
        const double sign = cap == 0 ? 1.0 : -1.0;
        const double y = sign * 0.5 + y_offset;
        const double cp[3] = {0, y, 0}, cuv[2] = {0.5, 0.5};
        const uint32_t ckey[4] = {5u, (uint32_t)cap, 0u, 0u};
        const int32_t center = wz3__cadd_add(&b.add, cp, 0, 1.0, cuv, 2, 1, ckey);
        for (int32_t ring = 1; ring <= tz; ring++) {
          const double radius = 0.5 * (double)ring / (double)tz;
          for (int32_t j = 0; j <= tx; j++) {
            const double angle = WZ3__TAU * (double)(j % tx) / (double)tx;
            const double p[3] = {radius * wz3__sin(angle), y, -radius * wz3__cos(angle)};
            const double uv[2] = {p[0] + 0.5, -sign * p[2] + 0.5};
            const uint32_t key[4] = {ring == tz ? 3u : 6u, ring == tz ? (uint32_t)(cap == 0 ? 0 : ty) : (uint32_t)cap,
                                     ring == tz ? (uint32_t)(j % tx) : (uint32_t)ring, ring == tz ? 0u : (uint32_t)(j % tx)};
            cur[j] = wz3__cadd_add(&b.add, p, 0, 1.0, uv, 2, 1, key);
          }
          const double target[3] = {0, sign, 0};
          for (int32_t j = 0; j < tx; j++) {
            if (ring == 1) {
              const int32_t ids[3] = {center, cur[j], cur[j + 1]};
              if (wz3__pbuild_push_oriented(&b.pb, b.m, ids, 3, target, 1, (mode & 1) ? 0 : 1)) b.m->oom = 1;
            } else {
              const int32_t ids[4] = {prev[j], cur[j], cur[j + 1], prev[j + 1]};
              if (wz3__pbuild_push_oriented(&b.pb, b.m, ids, 4, target, 1, (mode & 1) ? 0 : 1)) b.m->oom = 1;
            }
          }
          memcpy(prev, cur, (size_t)(tx + 1) * sizeof(int32_t));
        }
      }
    }
    WZ3_FREE(side);
    WZ3_FREE(prev);
  } else {
    const int32_t outer_count = tx - arc + 1;
    const int32_t base_count = outer_count + 1;
    double (*base)[2] = (double (*)[2])WZ3_MALLOC((size_t)base_count * sizeof *base);
    int32_t *rings = (int32_t *)WZ3_MALLOC((size_t)(ty + 1) * (size_t)base_count * sizeof(int32_t));
    if (!base || !rings) b.m->oom = 1;
    else {
      for (int32_t j = 0; j < outer_count; j++) {
        const double angle = WZ3__TAU * (double)j / (double)tx;
        base[j][0] = 0.5 * wz3__sin(angle);
        base[j][1] = -0.5 * wz3__cos(angle);
      }
      base[outer_count][0] = 0.0; base[outer_count][1] = 0.0;
      for (int32_t level = 0; level <= ty; level++)
        for (int32_t j = 0; j < base_count; j++) {
          const double p[3] = {base[j][0], 0.5 - (double)level / (double)ty + y_offset, base[j][1]};
          const double uv[2] = {(double)j / (double)base_count, (double)level / (double)ty};
          const uint32_t key[4] = {7u, (uint32_t)level, (uint32_t)j, 0u};
          rings[level * base_count + j] = wz3__cadd_add(&b.add, p, 0, 1.0, uv, 2, 0, key);
        }
      for (int32_t level = 0; level < ty; level++)
        for (int32_t j = 0; j < base_count; j++) {
          const int32_t next = (j + 1) % base_count;
          const int32_t ids[4] = {rings[level * base_count + j], rings[(level + 1) * base_count + j],
                                  rings[(level + 1) * base_count + next], rings[level * base_count + next]};
          const double target[3] = {base[j][0] + base[next][0], 0.0, base[j][1] + base[next][1]};
          if (wz3__pbuild_push_oriented(&b.pb, b.m, ids, 4, target, 0, 1)) b.m->oom = 1;
        }
      for (int cap = 0; cap < 2; cap++) {
        const double sign = cap == 0 ? 1.0 : -1.0;
        const int32_t *loop = rings + (cap == 0 ? 0 : ty) * base_count;
        const int32_t center_index = base_count - 1;
        const double target[3] = {0, sign, 0};
        for (int32_t j = 0; j + 1 < center_index; j++) {
          const int32_t ids[3] = {loop[center_index], loop[j], loop[j + 1]};
          if (wz3__pbuild_push_oriented(&b.pb, b.m, ids, 3, target, 1, (mode & 1) ? 0 : 1)) b.m->oom = 1;
        }
      }
    }
    WZ3_FREE(base);
    WZ3_FREE(rings);
  }
  if (wz3__build_finish_topology(&b)) return NULL;
  wz3_mesh *m = b.m;
  for (uint32_t i = 0; i < m->n_edges; i++) {
    wz3__medge *edge = &m->edges[i];
    if ((m->faces[edge->face[0]].mask ^ m->faces[edge->face[1]].mask) & 1) edge->crease |= 0xfe;
    const float *a = m->verts[edge->vert[0]].v[0], *c = m->verts[edge->vert[1]].v[0];
    if ((a[0] < 0 ? -a[0] : a[0]) < 1e-6 && (c[0] < 0 ? -c[0] : c[0]) < 1e-6 && a[2] < -0.49 && c[2] < -0.49)
      edge->crease |= WZ3__MF_UV0;
  }
  if (wz3__mesh_push_part(m, 0)) { wz3__mesh_free(m); return NULL; }
  m->got_normals = 0;
  return m;
}

static wz3_mesh *wz3__mesh_sphere(int32_t tx, int32_t ty)
{
  tx = tx < 3 ? 3 : tx; ty = ty < 1 ? 1 : ty;
  wz3__build b;
  if (wz3__build_begin(&b, (uint32_t)((tx + 1) * (ty + 1) + 8))) return NULL;
  int32_t *rings = (int32_t *)WZ3_MALLOC((size_t)(ty + 1) * (size_t)(tx + 1) * sizeof(int32_t));
  if (!rings) { wz3__build_abort(&b); return NULL; }
  for (int32_t lat = 0; lat <= ty; lat++) {
    const double phi = ((double)lat + 0.5) * WZ3__PI_D / (double)(ty + 1);
    const double radius = 0.5 * wz3__sin(phi), y = -0.5 * wz3__cos(phi);
    for (int32_t lon = 0; lon <= tx; lon++) {
      const double theta = WZ3__TAU * (double)(lon % tx) / (double)tx;
      const double p[3] = {radius * wz3__sin(theta), y, -radius * wz3__cos(theta)};
      const double uv[2] = {(double)lon / (double)tx, 1.0 - (double)lat / (double)ty};
      const uint32_t key[4] = {9u, (uint32_t)lat, (uint32_t)(lon % tx), 0u};
      rings[lat * (tx + 1) + lon] = wz3__cadd_add(&b.add, p, 0, 1.0, uv, 2, 0, key);
    }
  }
  for (int32_t lat = 0; lat < ty; lat++)
    for (int32_t lon = 0; lon < tx; lon++) {
      const int32_t ids[4] = {rings[lat * (tx + 1) + lon], rings[(lat + 1) * (tx + 1) + lon],
                              rings[(lat + 1) * (tx + 1) + lon + 1], rings[lat * (tx + 1) + lon + 1]};
      const float *pf = b.m->verts[ids[0]].v[0];
      const double target[3] = {pf[0], pf[1], pf[2]};
      if (wz3__pbuild_push_oriented(&b.pb, b.m, ids, 4, target, 0, 1)) b.m->oom = 1;
    }
  const double sp[3] = {0, -0.5, 0}, np[3] = {0, 0.5, 0};
  const double suv[2] = {0, 1}, nuv[2] = {0, 0}, ssuv[2] = {1, 1}, nsuv[2] = {1, 0};
  const uint32_t skey[4] = {10u, 0u, 0u, 0u}, nkey[4] = {11u, 0u, 0u, 0u};
  const int32_t south = wz3__cadd_add(&b.add, sp, 0, 1.0, suv, 2, 0, skey);
  const int32_t north = wz3__cadd_add(&b.add, np, 0, 1.0, nuv, 2, 0, nkey);
  const int32_t south_seam = wz3__cadd_add(&b.add, sp, 0, 1.0, ssuv, 2, 0, skey);
  const int32_t north_seam = wz3__cadd_add(&b.add, np, 0, 1.0, nsuv, 2, 0, nkey);
  for (int32_t lon = 0; lon < tx; lon++) {
    const int32_t south_pole = lon + 1 == tx ? south_seam : south, north_pole = lon + 1 == tx ? north_seam : north;
    const int32_t ids_s[3] = {south_pole, rings[lon + 1], rings[lon]};
    const double ts[3] = {0, -1, 0}, tn[3] = {0, 1, 0};
    if (wz3__pbuild_push_oriented(&b.pb, b.m, ids_s, 3, ts, 1, 1)) b.m->oom = 1;
    const int32_t ids_n[3] = {north_pole, rings[ty * (tx + 1) + lon], rings[ty * (tx + 1) + lon + 1]};
    if (wz3__pbuild_push_oriented(&b.pb, b.m, ids_n, 3, tn, 1, 1)) b.m->oom = 1;
  }
  WZ3_FREE(rings);
  if (wz3__build_finish_topology(&b)) return NULL;
  wz3_mesh *m = b.m;
  wz3__mesh_mark_uv_discontinuities(m);
  if (wz3__mesh_push_part(m, 0)) { wz3__mesh_free(m); return NULL; }
  wz3__mesh_all_to_mask(m, 0x00fffeffu, WZ3__SEL_SUB);
  m->got_normals = 0;
  return m;
}

static wz3_mesh *wz3__mesh_torus(int32_t tx, int32_t ty, double ro_in, double ri_in, double phase_in, double arc_in, int32_t flags)
{
  tx = tx < 3 ? 3 : tx; ty = ty < 3 ? 3 : ty;
  double ro = (float)ro_in, ri = (float)ri_in;
  const double phase = (float)phase_in, arc_length = (float)arc_in;
  if (flags & 1) {
    ri = (float)((ro - ri) * 0.5);
    ro = (float)(ro - ri);
  }
  const int closed = arc_length == 1.0;
  wz3__build b;
  if (wz3__build_begin(&b, (uint32_t)((tx + 1) * (ty + 1) + 2 * ty + 8))) return NULL;
  int32_t *grid = (int32_t *)WZ3_MALLOC((size_t)(tx + 1) * (size_t)(ty + 1) * sizeof(int32_t));
  if (!grid) { wz3__build_abort(&b); return NULL; }
  for (int32_t major = 0; major <= tx; major++)
    for (int32_t minor = 0; minor <= ty; minor++) {
      const int32_t major_step = closed ? major % tx : major;
      const double a = arc_length * WZ3__TAU * (double)major_step / (double)tx;
      const double bb = phase * WZ3__TAU / (double)ty + WZ3__TAU * (double)(minor % ty) / (double)ty;
      const double radial = (float)(wz3__sin(bb) * ri - ro);
      const double p[3] = {radial * wz3__cos(a), -wz3__cos(bb) * ri + ((flags & 2) ? ri : 0.0), -radial * wz3__sin(a)};
      const double uv[2] = {(double)minor / (double)ty, (double)major / (double)tx};
      const uint32_t key[4] = {8u, (uint32_t)(closed ? major % tx : major), (uint32_t)(minor % ty), 0u};
      grid[major * (ty + 1) + minor] = wz3__cadd_add(&b.add, p, 0, 1.0, uv, 2, 0, key);
    }
  for (int32_t major = 0; major < tx; major++)
    for (int32_t minor = 0; minor < ty; minor++) {
      const int32_t ids[4] = {grid[major * (ty + 1) + minor], grid[major * (ty + 1) + minor + 1],
                              grid[(major + 1) * (ty + 1) + minor + 1], grid[(major + 1) * (ty + 1) + minor]};
      const double a = arc_length * WZ3__TAU * ((double)major + 0.5) / (double)tx;
      const double bb = phase * WZ3__TAU / (double)ty + WZ3__TAU * ((double)minor + 0.5) / (double)ty;
      const double target[3] = {wz3__sin(bb) * wz3__cos(a), -wz3__cos(bb), -wz3__sin(bb) * wz3__sin(a)};
      if (wz3__pbuild_push_oriented(&b.pb, b.m, ids, 4, target, 0, 1)) b.m->oom = 1;
    }
  if (!closed) {
    int32_t cap_ids[512];
    for (int cap_end = 0; cap_end < 2; cap_end++) {
      const int32_t end = cap_end ? tx : 0;
      if (ty > 512) { b.m->oom = 1; break; }
      for (int32_t minor = 0; minor < ty; minor++) {
        const wz3__mvert *source = &b.m->verts[grid[end * (ty + 1) + minor]];
        const double p[3] = {source->v[0][0], source->v[0][1], source->v[0][2]};
        const double uv[4] = {source->v[4][0], source->v[4][1], source->v[4][2], source->v[4][3]};
        const uint32_t key[4] = {8u, (uint32_t)end, (uint32_t)minor, 0u};
        cap_ids[minor] = wz3__cadd_add(&b.add, p, 1, source->v[0][3], uv, 4, 1, key);
      }
      const double a = arc_length * WZ3__TAU * (double)end / (double)tx;
      const double sign = end == 0 ? 1.0 : -1.0;
      const double target[3] = {-wz3__sin(a) * sign, 0.0 * sign, -wz3__cos(a) * sign};
      if (wz3__pbuild_push_oriented(&b.pb, b.m, cap_ids, (uint32_t)ty, target, 1, 1)) b.m->oom = 1;
    }
  }
  WZ3_FREE(grid);
  if (wz3__build_finish_topology(&b)) return NULL;
  wz3_mesh *m = b.m;
  for (uint32_t i = 0; i < m->n_edges; i++) {
    wz3__medge *edge = &m->edges[i];
    if (m->faces[edge->face[0]].mask != m->faces[edge->face[1]].mask) edge->crease |= WZ3__MF_UV0;
  }
  wz3__mesh_mark_uv_discontinuities(m);
  if (wz3__mesh_push_part(m, 0)) { wz3__mesh_free(m); return NULL; }
  wz3__mesh_all_to_mask(m, 0xffffffffu, WZ3__SEL_SETNOT);
  m->got_normals = 0;
  return m;
}

