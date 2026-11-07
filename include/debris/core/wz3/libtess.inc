/* ======================================================================== */
/*  GLU tessellator (GLU tessellator: libtess 1.2.2, the SGI sample         */
/*  implementation). A direct port of the sweep-line tessellator with the   */
/*  fixed configuration Font3D uses: odd winding, normal (0,0,-1), triangle */
/*  output with edge flags ignored. All geometry is binary64 like in JS.    */
/*  Everything a polygon allocates lives in one arena released at the end.  */
/*                                                                          */
/*  Derived from libtess.js 1.2.2 (https://github.com/brendankenny/         */
/*  libtess.js), itself derived from the SGI GLU sample implementation.     */
/*  Copyright 2000, Silicon Graphics, Inc. All Rights Reserved.             */
/*  Copyright 2012 and 2015, Google Inc. All Rights Reserved.               */
/*  Licensed under the SGI Free Software License B, Version 2.0; the        */
/*  permission notice is in vendor/libtess/LICENSE.txt and at               */
/*  http://oss.sgi.com/projects/FreeB/                                      */
/* ======================================================================== */

#define WZ3__LT_MAX_COORD 1e150
#define WZ3__LT_SENTINEL (4.0 * WZ3__LT_MAX_COORD)
#define WZ3__LT_NODATA (-1)

typedef struct wz3__lt_vertex wz3__lt_vertex;
typedef struct wz3__lt_halfedge wz3__lt_halfedge;
typedef struct wz3__lt_face wz3__lt_face;
typedef struct wz3__lt_region wz3__lt_region;
typedef struct wz3__lt_dnode wz3__lt_dnode;

struct wz3__lt_vertex {
  wz3__lt_vertex *next, *prev;
  wz3__lt_halfedge *an_edge;
  int32_t data;
  double coords[3];
  double s, t;
  int32_t pq_handle;
};

struct wz3__lt_face {
  wz3__lt_face *next, *prev;
  wz3__lt_halfedge *an_edge;
  int inside;
};

struct wz3__lt_halfedge {
  wz3__lt_halfedge *next, *sym, *o_next, *l_next;
  wz3__lt_vertex *org;
  wz3__lt_face *l_face;
  wz3__lt_region *active_region;
  int32_t winding;
};

struct wz3__lt_dnode {
  void *key;
  wz3__lt_dnode *next, *prev;
};

struct wz3__lt_region {
  wz3__lt_halfedge *e_up;
  wz3__lt_dnode *node_up;
  int32_t winding_number;
  int inside, sentinel, dirty, fix_upper_edge;
};

typedef struct wz3__lt_chunk {
  struct wz3__lt_chunk *next;
  size_t used, cap;
} wz3__lt_chunk;

typedef struct wz3__lt_tess {
  wz3__lt_chunk *chunks;
  int oom, fatal_error, failed;
  jmp_buf jump;                      /* armed while the sweep runs: an allocation failure unwinds to end_polygon */
  int armed;
  wz3__lt_vertex *v_head;
  wz3__lt_face *f_head;
  wz3__lt_halfedge *e_head, *e_head_sym;
  wz3__lt_halfedge *last_edge;
  wz3__lt_vertex *event;
  wz3__lt_dnode d_head;
  /* priority queue: the sorted array (verts/order) and the heap */
  wz3__lt_vertex **q_verts; uint32_t q_verts_cap;
  int32_t *q_order;
  int32_t q_size;
  int q_initialized;
  int32_t *h_heap; uint32_t h_heap_cap;
  wz3__lt_vertex **h_verts; uint32_t h_verts_cap;
  int32_t *h_handles; uint32_t h_handles_cap;
  int32_t h_size, h_max, h_free_list;
  int h_initialized;
  /* sinks */
  void *user;
  int32_t (*combine)(void *user, const double coords[3]);          /* new vertex index, or -1 */
  int (*emit)(void *user, int32_t a, int32_t b, int32_t c);        /* 0 on success */
} wz3__lt_tess;

/* out of memory: inside the sweep nothing can continue, so unwind; elsewhere the flag makes end_polygon give up */
static void wz3__lt_fail(wz3__lt_tess *t)
{
  t->oom = 1;
  if (t->armed) longjmp(t->jump, 1);
}

static void *wz3__lt_alloc(wz3__lt_tess *t, size_t size)
{
  size = (size + 15u) & ~(size_t)15u;
  wz3__lt_chunk *c = t->chunks;
  if (!c || c->cap - c->used < size) {
    size_t cap = size > 65536u ? size : 65536u;
    wz3__lt_chunk *n = (wz3__lt_chunk *)WZ3_MALLOC(sizeof(wz3__lt_chunk) + 16u + cap);
    if (!n) { wz3__lt_fail(t); return NULL; }
    n->next = t->chunks; n->used = 0; n->cap = cap;
    t->chunks = n;
    c = n;
  }
  void *p = (char *)c + ((sizeof(wz3__lt_chunk) + 15u) & ~(size_t)15u) + c->used;
  c->used += size;
  memset(p, 0, size);
  return p;
}

static void wz3__lt_release(wz3__lt_tess *t)
{
  while (t->chunks) {
    wz3__lt_chunk *next = t->chunks->next;
    WZ3_FREE(t->chunks);
    t->chunks = next;
  }
  WZ3_FREE(t->q_verts); WZ3_FREE(t->q_order); WZ3_FREE(t->h_heap); WZ3_FREE(t->h_verts); WZ3_FREE(t->h_handles);
  t->q_verts = NULL; t->q_order = NULL; t->h_heap = NULL; t->h_verts = NULL; t->h_handles = NULL;
  t->q_verts_cap = t->h_heap_cap = t->h_verts_cap = t->h_handles_cap = 0;
}

/* ---- half-edge accessors ---- */
#define WZ3__LT_RFACE(e) ((e)->sym->l_face)
#define WZ3__LT_DST(e) ((e)->sym->org)
#define WZ3__LT_OPREV(e) ((e)->sym->l_next)
#define WZ3__LT_LPREV(e) ((e)->o_next->sym)
#define WZ3__LT_DPREV(e) ((e)->l_next->sym)
#define WZ3__LT_RPREV(e) ((e)->sym->o_next)
#define WZ3__LT_DNEXT(e) (WZ3__LT_RPREV(e)->sym)

/* ---- mesh (mesh.c) ---- */

static wz3__lt_vertex *wz3__lt_new_vertex(wz3__lt_tess *t, wz3__lt_vertex *next, wz3__lt_vertex *prev)
{
  wz3__lt_vertex *v = (wz3__lt_vertex *)wz3__lt_alloc(t, sizeof *v);
  if (!v) return NULL;
  v->next = next ? next : v;
  v->prev = prev ? prev : v;
  v->data = WZ3__LT_NODATA;
  return v;
}

static wz3__lt_face *wz3__lt_new_face(wz3__lt_tess *t, wz3__lt_face *next, wz3__lt_face *prev)
{
  wz3__lt_face *f = (wz3__lt_face *)wz3__lt_alloc(t, sizeof *f);
  if (!f) return NULL;
  f->next = next ? next : f;
  f->prev = prev ? prev : f;
  return f;
}

static wz3__lt_halfedge *wz3__lt_new_halfedge(wz3__lt_tess *t)
{
  wz3__lt_halfedge *e = (wz3__lt_halfedge *)wz3__lt_alloc(t, sizeof *e);
  if (e) e->next = e;
  return e;
}

static void wz3__lt_splice_raw(wz3__lt_halfedge *a, wz3__lt_halfedge *b)
{
  wz3__lt_halfedge *a_o_next = a->o_next, *b_o_next = b->o_next;
  a_o_next->sym->l_next = b;
  b_o_next->sym->l_next = a;
  a->o_next = b_o_next;
  b->o_next = a_o_next;
}

static wz3__lt_halfedge *wz3__lt_make_edge_pair(wz3__lt_tess *t, wz3__lt_halfedge *e_next)
{
  wz3__lt_halfedge *e = wz3__lt_new_halfedge(t), *e_sym = wz3__lt_new_halfedge(t);
  if (!e || !e_sym) return NULL;
  wz3__lt_halfedge *e_prev = e_next->sym->next;
  e_sym->next = e_prev;
  e_prev->sym->next = e;
  e->next = e_next;
  e_next->sym->next = e_sym;
  e->sym = e_sym;
  e->o_next = e;
  e->l_next = e_sym;
  e_sym->sym = e;
  e_sym->o_next = e_sym;
  e_sym->l_next = e;
  return e;
}

static void wz3__lt_make_vertex(wz3__lt_tess *t, wz3__lt_halfedge *e_orig, wz3__lt_vertex *v_next)
{
  wz3__lt_vertex *v_prev = v_next->prev;
  wz3__lt_vertex *v_new = wz3__lt_new_vertex(t, v_next, v_prev);
  if (!v_new) return;
  v_prev->next = v_new;
  v_next->prev = v_new;
  v_new->an_edge = e_orig;
  wz3__lt_halfedge *e = e_orig;
  do { e->org = v_new; e = e->o_next; } while (e != e_orig);
}

static void wz3__lt_make_face(wz3__lt_tess *t, wz3__lt_halfedge *e_orig, wz3__lt_face *f_next)
{
  wz3__lt_face *f_prev = f_next->prev;
  wz3__lt_face *f_new = wz3__lt_new_face(t, f_next, f_prev);
  if (!f_new) return;
  f_prev->next = f_new;
  f_next->prev = f_new;
  f_new->an_edge = e_orig;
  f_new->inside = f_next->inside;
  wz3__lt_halfedge *e = e_orig;
  do { e->l_face = f_new; e = e->l_next; } while (e != e_orig);
}

static void wz3__lt_kill_edge(wz3__lt_halfedge *e_del)
{
  wz3__lt_halfedge *e_next = e_del->next, *e_prev = e_del->sym->next;
  e_next->sym->next = e_prev;
  e_prev->sym->next = e_next;
}

static void wz3__lt_kill_vertex(wz3__lt_vertex *v_del, wz3__lt_vertex *new_org)
{
  wz3__lt_halfedge *e_start = v_del->an_edge, *e = e_start;
  do { e->org = new_org; e = e->o_next; } while (e != e_start);
  wz3__lt_vertex *v_prev = v_del->prev, *v_next = v_del->next;
  v_next->prev = v_prev;
  v_prev->next = v_next;
}

static void wz3__lt_kill_face(wz3__lt_face *f_del, wz3__lt_face *new_l_face)
{
  wz3__lt_halfedge *e_start = f_del->an_edge, *e = e_start;
  do { e->l_face = new_l_face; e = e->l_next; } while (e != e_start);
  wz3__lt_face *f_prev = f_del->prev, *f_next = f_del->next;
  f_next->prev = f_prev;
  f_prev->next = f_next;
}

static wz3__lt_halfedge *wz3__lt_make_edge(wz3__lt_tess *t)
{
  wz3__lt_halfedge *e = wz3__lt_make_edge_pair(t, t->e_head);
  if (!e) return NULL;
  wz3__lt_make_vertex(t, e, t->v_head);
  wz3__lt_make_vertex(t, e->sym, t->v_head);
  wz3__lt_make_face(t, e, t->f_head);
  return t->oom ? NULL : e;
}

static void wz3__lt_mesh_splice(wz3__lt_tess *t, wz3__lt_halfedge *e_org, wz3__lt_halfedge *e_dst)
{
  int joining_loops = 0, joining_vertices = 0;
  if (e_org == e_dst) return;
  if (e_dst->org != e_org->org) { joining_vertices = 1; wz3__lt_kill_vertex(e_dst->org, e_org->org); }
  if (e_dst->l_face != e_org->l_face) { joining_loops = 1; wz3__lt_kill_face(e_dst->l_face, e_org->l_face); }
  wz3__lt_splice_raw(e_dst, e_org);
  if (!joining_vertices) {
    wz3__lt_make_vertex(t, e_dst, e_org->org);
    e_org->org->an_edge = e_org;
  }
  if (!joining_loops) {
    wz3__lt_make_face(t, e_dst, e_org->l_face);
    e_org->l_face->an_edge = e_org;
  }
}

static void wz3__lt_delete_edge(wz3__lt_tess *t, wz3__lt_halfedge *e_del)
{
  wz3__lt_halfedge *e_del_sym = e_del->sym;
  int joining_loops = 0;
  if (e_del->l_face != WZ3__LT_RFACE(e_del)) { joining_loops = 1; wz3__lt_kill_face(e_del->l_face, WZ3__LT_RFACE(e_del)); }
  if (e_del->o_next == e_del) {
    wz3__lt_kill_vertex(e_del->org, NULL);
  } else {
    WZ3__LT_RFACE(e_del)->an_edge = WZ3__LT_OPREV(e_del);
    e_del->org->an_edge = e_del->o_next;
    wz3__lt_splice_raw(e_del, WZ3__LT_OPREV(e_del));
    if (!joining_loops) wz3__lt_make_face(t, e_del, e_del->l_face);
  }
  if (e_del_sym->o_next == e_del_sym) {
    wz3__lt_kill_vertex(e_del_sym->org, NULL);
    wz3__lt_kill_face(e_del_sym->l_face, NULL);
  } else {
    e_del->l_face->an_edge = WZ3__LT_OPREV(e_del_sym);
    e_del_sym->org->an_edge = e_del_sym->o_next;
    wz3__lt_splice_raw(e_del_sym, WZ3__LT_OPREV(e_del_sym));
  }
  wz3__lt_kill_edge(e_del);
}

static wz3__lt_halfedge *wz3__lt_add_edge_vertex(wz3__lt_tess *t, wz3__lt_halfedge *e_org)
{
  wz3__lt_halfedge *e_new = wz3__lt_make_edge_pair(t, e_org);
  if (!e_new) return NULL;
  wz3__lt_halfedge *e_new_sym = e_new->sym;
  wz3__lt_splice_raw(e_new, e_org->l_next);
  e_new->org = WZ3__LT_DST(e_org);
  wz3__lt_make_vertex(t, e_new_sym, e_new->org);
  e_new->l_face = e_new_sym->l_face = e_org->l_face;
  return e_new;
}

static wz3__lt_halfedge *wz3__lt_split_edge(wz3__lt_tess *t, wz3__lt_halfedge *e_org)
{
  wz3__lt_halfedge *temp = wz3__lt_add_edge_vertex(t, e_org);
  if (!temp) return e_org;
  wz3__lt_halfedge *e_new = temp->sym;
  wz3__lt_splice_raw(e_org->sym, WZ3__LT_OPREV(e_org->sym));
  wz3__lt_splice_raw(e_org->sym, e_new);
  e_org->sym->org = e_new->org;
  WZ3__LT_DST(e_new)->an_edge = e_new->sym;
  e_new->sym->l_face = WZ3__LT_RFACE(e_org);
  e_new->winding = e_org->winding;
  e_new->sym->winding = e_org->sym->winding;
  return e_new;
}

static wz3__lt_halfedge *wz3__lt_connect(wz3__lt_tess *t, wz3__lt_halfedge *e_org, wz3__lt_halfedge *e_dst)
{
  int joining_loops = 0;
  wz3__lt_halfedge *e_new = wz3__lt_make_edge_pair(t, e_org);
  if (!e_new) return e_org;
  wz3__lt_halfedge *e_new_sym = e_new->sym;
  if (e_dst->l_face != e_org->l_face) { joining_loops = 1; wz3__lt_kill_face(e_dst->l_face, e_org->l_face); }
  wz3__lt_splice_raw(e_new, e_org->l_next);
  wz3__lt_splice_raw(e_new_sym, e_dst);
  e_new->org = WZ3__LT_DST(e_org);
  e_new_sym->org = e_dst->org;
  e_new->l_face = e_new_sym->l_face = e_org->l_face;
  e_org->l_face->an_edge = e_new_sym;
  if (!joining_loops) wz3__lt_make_face(t, e_new, e_org->l_face);
  return e_new;
}

/* ---- geometry (geom.c) ---- */

static int wz3__lt_vert_eq(const wz3__lt_vertex *u, const wz3__lt_vertex *v) { return u->s == v->s && u->t == v->t; }
static int wz3__lt_vert_leq(const wz3__lt_vertex *u, const wz3__lt_vertex *v) { return (u->s < v->s) || (u->s == v->s && u->t <= v->t); }
static int wz3__lt_trans_leq(const wz3__lt_vertex *u, const wz3__lt_vertex *v) { return (u->t < v->t) || (u->t == v->t && u->s <= v->s); }

static double wz3__lt_edge_eval(const wz3__lt_vertex *u, const wz3__lt_vertex *v, const wz3__lt_vertex *w)
{
  const double gap_l = v->s - u->s, gap_r = w->s - v->s;
  if (gap_l + gap_r > 0) {
    if (gap_l < gap_r) return (v->t - u->t) + (u->t - w->t) * (gap_l / (gap_l + gap_r));
    return (v->t - w->t) + (w->t - u->t) * (gap_r / (gap_l + gap_r));
  }
  return 0;
}

static double wz3__lt_edge_sign(const wz3__lt_vertex *u, const wz3__lt_vertex *v, const wz3__lt_vertex *w)
{
  const double gap_l = v->s - u->s, gap_r = w->s - v->s;
  if (gap_l + gap_r > 0) return (v->t - w->t) * gap_l + (v->t - u->t) * gap_r;
  return 0;
}

static double wz3__lt_trans_eval(const wz3__lt_vertex *u, const wz3__lt_vertex *v, const wz3__lt_vertex *w)
{
  const double gap_l = v->t - u->t, gap_r = w->t - v->t;
  if (gap_l + gap_r > 0) {
    if (gap_l < gap_r) return (v->s - u->s) + (u->s - w->s) * (gap_l / (gap_l + gap_r));
    return (v->s - w->s) + (w->s - u->s) * (gap_r / (gap_l + gap_r));
  }
  return 0;
}

static double wz3__lt_trans_sign(const wz3__lt_vertex *u, const wz3__lt_vertex *v, const wz3__lt_vertex *w)
{
  const double gap_l = v->t - u->t, gap_r = w->t - v->t;
  if (gap_l + gap_r > 0) return (v->s - w->s) * gap_l + (v->s - u->s) * gap_r;
  return 0;
}

static int wz3__lt_edge_goes_left(const wz3__lt_halfedge *e) { return wz3__lt_vert_leq(WZ3__LT_DST(e), e->org); }
static int wz3__lt_edge_goes_right(const wz3__lt_halfedge *e) { return wz3__lt_vert_leq(e->org, WZ3__LT_DST(e)); }
static double wz3__lt_l1_dist(const wz3__lt_vertex *u, const wz3__lt_vertex *v) { return fabs(u->s - v->s) + fabs(u->t - v->t); }

static double wz3__lt_interpolate(double a, double x, double b, double y)
{
  a = (a < 0) ? 0 : a;
  b = (b < 0) ? 0 : b;
  if (a <= b) {
    if (b == 0) return (x + y) / 2;
    return x + (y - x) * (a / (a + b));
  }
  return y + (x - y) * (b / (a + b));
}

static void wz3__lt_edge_intersect(const wz3__lt_vertex *o1, const wz3__lt_vertex *d1, const wz3__lt_vertex *o2,
                                   const wz3__lt_vertex *d2, wz3__lt_vertex *v)
{
  double z1, z2;
  const wz3__lt_vertex *tmp;
  if (!wz3__lt_vert_leq(o1, d1)) { tmp = o1; o1 = d1; d1 = tmp; }
  if (!wz3__lt_vert_leq(o2, d2)) { tmp = o2; o2 = d2; d2 = tmp; }
  if (!wz3__lt_vert_leq(o1, o2)) { tmp = o1; o1 = o2; o2 = tmp; tmp = d1; d1 = d2; d2 = tmp; }
  if (!wz3__lt_vert_leq(o2, d1)) {
    v->s = (o2->s + d1->s) / 2;
  } else if (wz3__lt_vert_leq(d1, d2)) {
    z1 = wz3__lt_edge_eval(o1, o2, d1);
    z2 = wz3__lt_edge_eval(o2, d1, d2);
    if (z1 + z2 < 0) { z1 = -z1; z2 = -z2; }
    v->s = wz3__lt_interpolate(z1, o2->s, z2, d1->s);
  } else {
    z1 = wz3__lt_edge_sign(o1, o2, d1);
    z2 = -wz3__lt_edge_sign(o1, d2, d1);
    if (z1 + z2 < 0) { z1 = -z1; z2 = -z2; }
    v->s = wz3__lt_interpolate(z1, o2->s, z2, d2->s);
  }
  if (!wz3__lt_trans_leq(o1, d1)) { tmp = o1; o1 = d1; d1 = tmp; }
  if (!wz3__lt_trans_leq(o2, d2)) { tmp = o2; o2 = d2; d2 = tmp; }
  if (!wz3__lt_trans_leq(o1, o2)) { tmp = o1; o1 = o2; o2 = tmp; tmp = d1; d1 = d2; d2 = tmp; }
  if (!wz3__lt_trans_leq(o2, d1)) {
    v->t = (o2->t + d1->t) / 2;
  } else if (wz3__lt_trans_leq(d1, d2)) {
    z1 = wz3__lt_trans_eval(o1, o2, d1);
    z2 = wz3__lt_trans_eval(o2, d1, d2);
    if (z1 + z2 < 0) { z1 = -z1; z2 = -z2; }
    v->t = wz3__lt_interpolate(z1, o2->t, z2, d1->t);
  } else {
    z1 = wz3__lt_trans_sign(o1, o2, d1);
    z2 = -wz3__lt_trans_sign(o1, d2, d1);
    if (z1 + z2 < 0) { z1 = -z1; z2 = -z2; }
    v->t = wz3__lt_interpolate(z1, o2->t, z2, d2->t);
  }
}

/* ---- priority queue (priorityq sources) ---- */

static int wz3__lt_grow_i32(int32_t **a, uint32_t *cap, uint32_t need)
{
  if (need < *cap) return 0;
  uint32_t ncap = *cap ? *cap : 64u;
  while (ncap <= need) ncap *= 2u;
  int32_t *n = (int32_t *)WZ3_MALLOC((size_t)ncap * sizeof(int32_t));
  if (!n) return -1;
  memset(n, 0, (size_t)ncap * sizeof(int32_t));
  if (*cap) memcpy(n, *a, (size_t)*cap * sizeof(int32_t));
  WZ3_FREE(*a);
  *a = n; *cap = ncap;
  return 0;
}

static int wz3__lt_grow_vp(wz3__lt_vertex ***a, uint32_t *cap, uint32_t need)
{
  if (need < *cap) return 0;
  uint32_t ncap = *cap ? *cap : 64u;
  while (ncap <= need) ncap *= 2u;
  wz3__lt_vertex **n = (wz3__lt_vertex **)WZ3_MALLOC((size_t)ncap * sizeof(wz3__lt_vertex *));
  if (!n) return -1;
  memset(n, 0, (size_t)ncap * sizeof(wz3__lt_vertex *));
  if (*cap) memcpy(n, *a, (size_t)*cap * sizeof(wz3__lt_vertex *));
  WZ3_FREE(*a);
  *a = n; *cap = ncap;
  return 0;
}

static void wz3__lt_heap_float_down(wz3__lt_tess *t, int32_t index)
{
  int32_t *heap = t->h_heap, *handles = t->h_handles;
  wz3__lt_vertex **verts = t->h_verts;
  int32_t curr_index = index, curr_handle = heap[curr_index];
  for (;;) {
    int32_t child_index = curr_index << 1;
    if (child_index < t->h_size) {
      if (wz3__lt_vert_leq(verts[heap[child_index + 1]], verts[heap[child_index]])) child_index = child_index + 1;
    }
    const int32_t child_handle = heap[child_index];
    if (child_index > t->h_size || wz3__lt_vert_leq(verts[curr_handle], verts[child_handle])) {
      heap[curr_index] = curr_handle;
      handles[curr_handle] = curr_index;
      return;
    }
    heap[curr_index] = child_handle;
    handles[child_handle] = curr_index;
    curr_index = child_index;
  }
}

static void wz3__lt_heap_float_up(wz3__lt_tess *t, int32_t index)
{
  int32_t *heap = t->h_heap, *handles = t->h_handles;
  wz3__lt_vertex **verts = t->h_verts;
  int32_t curr_index = index, curr_handle = heap[curr_index];
  for (;;) {
    const int32_t parent_index = curr_index >> 1;
    const int32_t parent_handle = heap[parent_index];
    if (parent_index == 0 || wz3__lt_vert_leq(verts[parent_handle], verts[curr_handle])) {
      heap[curr_index] = curr_handle;
      handles[curr_handle] = curr_index;
      return;
    }
    heap[curr_index] = parent_handle;
    handles[parent_handle] = curr_index;
    curr_index = parent_index;
  }
}

static int wz3__lt_heap_init_storage(wz3__lt_tess *t)
{
  if (wz3__lt_grow_i32(&t->h_heap, &t->h_heap_cap, 33u) || wz3__lt_grow_vp(&t->h_verts, &t->h_verts_cap, 33u) ||
      wz3__lt_grow_i32(&t->h_handles, &t->h_handles_cap, 33u)) return -1;
  t->h_size = 0; t->h_max = 32; t->h_free_list = 0; t->h_initialized = 0;
  t->h_heap[1] = 1;
  return 0;
}

static int32_t wz3__lt_heap_insert(wz3__lt_tess *t, wz3__lt_vertex *vert)
{
  const int32_t end_index = ++t->h_size;
  if ((end_index * 2) > t->h_max) t->h_max *= 2;
  if (wz3__lt_grow_i32(&t->h_heap, &t->h_heap_cap, (uint32_t)end_index + 1u) ||
      wz3__lt_grow_vp(&t->h_verts, &t->h_verts_cap, (uint32_t)end_index + 1u) ||
      wz3__lt_grow_i32(&t->h_handles, &t->h_handles_cap, (uint32_t)(t->h_max + 1))) { wz3__lt_fail(t); return 0; }
  int32_t new_slot;
  if (t->h_free_list == 0) {
    new_slot = end_index;
  } else {
    new_slot = t->h_free_list;
    t->h_free_list = t->h_handles[t->h_free_list];
  }
  t->h_verts[new_slot] = vert;
  t->h_handles[new_slot] = end_index;
  t->h_heap[end_index] = new_slot;
  if (t->h_initialized) wz3__lt_heap_float_up(t, end_index);
  return new_slot;
}

static wz3__lt_vertex *wz3__lt_heap_minimum(wz3__lt_tess *t) { return t->h_verts[t->h_heap[1]]; }

static wz3__lt_vertex *wz3__lt_heap_extract_min(wz3__lt_tess *t)
{
  int32_t *heap = t->h_heap, *handles = t->h_handles;
  wz3__lt_vertex **verts = t->h_verts;
  const int32_t min_handle = heap[1];
  wz3__lt_vertex *min_vertex = verts[min_handle];
  if (t->h_size > 0) {
    heap[1] = heap[t->h_size];
    handles[heap[1]] = 1;
    verts[min_handle] = NULL;
    handles[min_handle] = t->h_free_list;
    t->h_free_list = min_handle;
    if (--t->h_size > 0) wz3__lt_heap_float_down(t, 1);
  }
  return min_vertex;
}

static void wz3__lt_heap_remove(wz3__lt_tess *t, int32_t remove_handle)
{
  int32_t *heap = t->h_heap, *handles = t->h_handles;
  wz3__lt_vertex **verts = t->h_verts;
  const int32_t heap_index = handles[remove_handle];
  heap[heap_index] = heap[t->h_size];
  handles[heap[heap_index]] = heap_index;
  if (heap_index <= --t->h_size) {
    if (heap_index <= 1) {
      wz3__lt_heap_float_down(t, heap_index);
    } else {
      wz3__lt_vertex *vert = verts[heap[heap_index]];
      wz3__lt_vertex *parent_vert = verts[heap[heap_index >> 1]];
      if (wz3__lt_vert_leq(parent_vert, vert)) wz3__lt_heap_float_down(t, heap_index);
      else wz3__lt_heap_float_up(t, heap_index);
    }
  }
  verts[remove_handle] = NULL;
  handles[remove_handle] = t->h_free_list;
  t->h_free_list = remove_handle;
}

/* The JS sorts an index array with `leq ? 1 : -1`, which orders equal vertices unpredictably (V8's TimSort);
   a stable merge sort gives the same order whenever no two vertices coincide (checked on the production text). */
static void wz3__lt_sort_order(const wz3__lt_vertex *const *verts, int32_t *order, int32_t *tmp, int32_t n)
{
  for (int32_t width = 1; width < n; width *= 2) {
    for (int32_t lo = 0; lo < n; lo += 2 * width) {
      const int32_t mid = lo + width < n ? lo + width : n, hi = lo + 2 * width < n ? lo + 2 * width : n;
      int32_t i = lo, j = mid, k = lo;
      while (i < mid && j < hi) {
        /* descending order by vertLeq: `a` precedes `b` when b <= a does not hold */
        if (wz3__lt_vert_leq(verts[order[i]], verts[order[j]]) && !wz3__lt_vert_leq(verts[order[j]], verts[order[i]])) tmp[k++] = order[j++];
        else tmp[k++] = order[i++];
      }
      while (i < mid) tmp[k++] = order[i++];
      while (j < hi) tmp[k++] = order[j++];
    }
    memcpy(order, tmp, (size_t)n * sizeof(int32_t));
  }
}

static int32_t wz3__lt_pq_insert(wz3__lt_tess *t, wz3__lt_vertex *vert)
{
  if (t->q_initialized) return wz3__lt_heap_insert(t, vert);
  const int32_t curr = t->q_size++;
  if (wz3__lt_grow_vp(&t->q_verts, &t->q_verts_cap, (uint32_t)curr + 1u)) { wz3__lt_fail(t); return 0; }
  t->q_verts[curr] = vert;
  return -(curr + 1);
}

static void wz3__lt_pq_init(wz3__lt_tess *t)
{
  int32_t *tmp = (int32_t *)WZ3_MALLOC((size_t)(t->q_size ? t->q_size : 1) * sizeof(int32_t));
  t->q_order = (int32_t *)WZ3_MALLOC((size_t)(t->q_size ? t->q_size : 1) * sizeof(int32_t));
  if (!tmp || !t->q_order) { WZ3_FREE(tmp); wz3__lt_fail(t); return; }
  for (int32_t i = 0; i < t->q_size; i++) t->q_order[i] = i;
  wz3__lt_sort_order((const wz3__lt_vertex *const *)t->q_verts, t->q_order, tmp, t->q_size);
  WZ3_FREE(tmp);
  t->q_initialized = 1;
  for (int32_t i = t->h_size; i >= 1; --i) wz3__lt_heap_float_down(t, i);
  t->h_initialized = 1;
}

static wz3__lt_vertex *wz3__lt_pq_extract_min(wz3__lt_tess *t)
{
  if (t->q_size == 0) return wz3__lt_heap_extract_min(t);
  wz3__lt_vertex *sort_min = t->q_verts[t->q_order[t->q_size - 1]];
  if (t->h_size != 0) {
    wz3__lt_vertex *heap_min = wz3__lt_heap_minimum(t);
    if (wz3__lt_vert_leq(heap_min, sort_min)) return wz3__lt_heap_extract_min(t);
  }
  do { --t->q_size; } while (t->q_size > 0 && t->q_verts[t->q_order[t->q_size - 1]] == NULL);
  return sort_min;
}

static wz3__lt_vertex *wz3__lt_pq_minimum(wz3__lt_tess *t)
{
  if (t->q_size == 0) return wz3__lt_heap_minimum(t);
  wz3__lt_vertex *sort_min = t->q_verts[t->q_order[t->q_size - 1]];
  if (t->h_size != 0) {
    wz3__lt_vertex *heap_min = wz3__lt_heap_minimum(t);
    if (wz3__lt_vert_leq(heap_min, sort_min)) return heap_min;
  }
  return sort_min;
}

static void wz3__lt_pq_remove(wz3__lt_tess *t, int32_t remove_handle)
{
  if (remove_handle >= 0) { wz3__lt_heap_remove(t, remove_handle); return; }
  remove_handle = -(remove_handle + 1);
  t->q_verts[remove_handle] = NULL;
  while (t->q_size > 0 && t->q_verts[t->q_order[t->q_size - 1]] == NULL) --t->q_size;
}

/* ---- edge dictionary (dict sources) ---- */

static int wz3__lt_edge_leq(wz3__lt_tess *t, const wz3__lt_region *reg1, const wz3__lt_region *reg2)
{
  const wz3__lt_vertex *event = t->event;
  const wz3__lt_halfedge *e1 = reg1->e_up, *e2 = reg2->e_up;
  if (WZ3__LT_DST(e1) == event) {
    if (WZ3__LT_DST(e2) == event) {
      if (wz3__lt_vert_leq(e1->org, e2->org)) return wz3__lt_edge_sign(WZ3__LT_DST(e2), e1->org, e2->org) <= 0;
      return wz3__lt_edge_sign(WZ3__LT_DST(e1), e2->org, e1->org) >= 0;
    }
    return wz3__lt_edge_sign(WZ3__LT_DST(e2), event, e2->org) <= 0;
  }
  if (WZ3__LT_DST(e2) == event) return wz3__lt_edge_sign(WZ3__LT_DST(e1), event, e1->org) >= 0;
  const double t1 = wz3__lt_edge_eval(WZ3__LT_DST(e1), event, e1->org);
  const double t2 = wz3__lt_edge_eval(WZ3__LT_DST(e2), event, e2->org);
  return t1 >= t2;
}

static wz3__lt_dnode *wz3__lt_dict_insert_before(wz3__lt_tess *t, wz3__lt_dnode *node, void *key)
{
  do { node = node->prev; } while (node->key != NULL && !wz3__lt_edge_leq(t, (const wz3__lt_region *)node->key, (const wz3__lt_region *)key));
  wz3__lt_dnode *n = (wz3__lt_dnode *)wz3__lt_alloc(t, sizeof *n);
  if (!n) return node;
  n->key = key; n->next = node->next; n->prev = node;
  node->next->prev = n;
  node->next = n;
  return n;
}

static void wz3__lt_dict_delete(wz3__lt_dnode *node)
{
  node->next->prev = node->prev;
  node->prev->next = node->next;
}

static wz3__lt_dnode *wz3__lt_dict_search(wz3__lt_tess *t, void *key)
{
  wz3__lt_dnode *node = &t->d_head;
  do { node = node->next; } while (node->key != NULL && !wz3__lt_edge_leq(t, (const wz3__lt_region *)key, (const wz3__lt_region *)node->key));
  return node;
}

/* ---- sweep (sweep.c) ---- */

#define WZ3__LT_REGION_ABOVE(r) ((wz3__lt_region *)(r)->node_up->next->key)
#define WZ3__LT_REGION_BELOW(r) ((wz3__lt_region *)(r)->node_up->prev->key)

static void wz3__lt_sweep_event(wz3__lt_tess *t, wz3__lt_vertex *v_event);
static void wz3__lt_walk_dirty_regions(wz3__lt_tess *t, wz3__lt_region *reg_up);
static void wz3__lt_add_right_edges(wz3__lt_tess *t, wz3__lt_region *reg_up, wz3__lt_halfedge *e_first, wz3__lt_halfedge *e_last,
                                    wz3__lt_halfedge *e_top_left, int clean_up);

static void wz3__lt_add_winding(wz3__lt_halfedge *e_dst, const wz3__lt_halfedge *e_src)
{
  e_dst->winding += e_src->winding;
  e_dst->sym->winding += e_src->sym->winding;
}

static void wz3__lt_delete_region(wz3__lt_tess *t, wz3__lt_region *reg)
{
  (void)t;
  reg->e_up->active_region = NULL;
  wz3__lt_dict_delete(reg->node_up);
  reg->node_up = NULL;
}

static void wz3__lt_fix_upper_edge(wz3__lt_tess *t, wz3__lt_region *reg, wz3__lt_halfedge *new_edge)
{
  wz3__lt_delete_edge(t, reg->e_up);
  reg->fix_upper_edge = 0;
  reg->e_up = new_edge;
  new_edge->active_region = reg;
}

static wz3__lt_region *wz3__lt_top_left_region(wz3__lt_tess *t, wz3__lt_region *reg)
{
  wz3__lt_vertex *org = reg->e_up->org;
  do { reg = WZ3__LT_REGION_ABOVE(reg); } while (reg->e_up->org == org);
  if (reg->fix_upper_edge) {
    wz3__lt_halfedge *e = wz3__lt_connect(t, WZ3__LT_REGION_BELOW(reg)->e_up->sym, reg->e_up->l_next);
    wz3__lt_fix_upper_edge(t, reg, e);
    reg = WZ3__LT_REGION_ABOVE(reg);
  }
  return reg;
}

static wz3__lt_region *wz3__lt_top_right_region(wz3__lt_region *reg)
{
  wz3__lt_vertex *dst = WZ3__LT_DST(reg->e_up);
  do { reg = WZ3__LT_REGION_ABOVE(reg); } while (WZ3__LT_DST(reg->e_up) == dst);
  return reg;
}

static wz3__lt_region *wz3__lt_add_region_below(wz3__lt_tess *t, wz3__lt_region *reg_above, wz3__lt_halfedge *e_new_up)
{
  wz3__lt_region *reg_new = (wz3__lt_region *)wz3__lt_alloc(t, sizeof *reg_new);
  if (!reg_new) return reg_above;
  reg_new->e_up = e_new_up;
  reg_new->node_up = wz3__lt_dict_insert_before(t, reg_above->node_up, reg_new);
  e_new_up->active_region = reg_new;
  return reg_new;
}

static void wz3__lt_compute_winding(wz3__lt_region *reg)
{
  reg->winding_number = WZ3__LT_REGION_ABOVE(reg)->winding_number + reg->e_up->winding;
  reg->inside = (reg->winding_number & 1) != 0;
}

static void wz3__lt_finish_region(wz3__lt_tess *t, wz3__lt_region *reg)
{
  wz3__lt_halfedge *e = reg->e_up;
  wz3__lt_face *f = e->l_face;
  f->inside = reg->inside;
  f->an_edge = e;
  wz3__lt_delete_region(t, reg);
}

static wz3__lt_halfedge *wz3__lt_finish_left_regions(wz3__lt_tess *t, wz3__lt_region *reg_first, wz3__lt_region *reg_last)
{
  wz3__lt_region *reg_prev = reg_first;
  wz3__lt_halfedge *e_prev = reg_first->e_up;
  while (reg_prev != reg_last) {
    reg_prev->fix_upper_edge = 0;
    wz3__lt_region *reg = WZ3__LT_REGION_BELOW(reg_prev);
    wz3__lt_halfedge *e = reg->e_up;
    if (e->org != e_prev->org) {
      if (!reg->fix_upper_edge) {
        wz3__lt_finish_region(t, reg_prev);
        break;
      }
      e = wz3__lt_connect(t, WZ3__LT_LPREV(e_prev), e->sym);
      wz3__lt_fix_upper_edge(t, reg, e);
    }
    if (e_prev->o_next != e) {
      wz3__lt_mesh_splice(t, WZ3__LT_OPREV(e), e);
      wz3__lt_mesh_splice(t, e_prev, e);
    }
    wz3__lt_finish_region(t, reg_prev);
    e_prev = reg->e_up;
    reg_prev = reg;
  }
  return e_prev;
}

static void wz3__lt_call_combine(wz3__lt_tess *t, wz3__lt_vertex *isect, const int32_t data[4], int needed)
{
  const double coords[3] = {isect->coords[0], isect->coords[1], isect->coords[2]};
  isect->data = WZ3__LT_NODATA;
  const int32_t index = t->combine ? t->combine(t->user, coords) : -1;
  isect->data = index > 0 ? index : WZ3__LT_NODATA;
  if (isect->data == WZ3__LT_NODATA) {
    if (!needed) {
      isect->data = data[0];
    } else if (!t->fatal_error) {
      t->failed = 1;
      t->fatal_error = 1;
    }
  }
}

static void wz3__lt_splice_merge_vertices(wz3__lt_tess *t, wz3__lt_halfedge *e1, wz3__lt_halfedge *e2)
{
  const int32_t data[4] = {e1->org->data, e2->org->data, WZ3__LT_NODATA, WZ3__LT_NODATA};
  wz3__lt_call_combine(t, e1->org, data, 0);
  wz3__lt_mesh_splice(t, e1, e2);
}

static void wz3__lt_vertex_weights(wz3__lt_vertex *isect, const wz3__lt_vertex *org, const wz3__lt_vertex *dst, double *weights)
{
  const double t1 = wz3__lt_l1_dist(org, isect), t2 = wz3__lt_l1_dist(dst, isect);
  weights[0] = 0.5 * t2 / (t1 + t2);
  weights[1] = 0.5 * t1 / (t1 + t2);
  isect->coords[0] += weights[0] * org->coords[0] + weights[1] * dst->coords[0];
  isect->coords[1] += weights[0] * org->coords[1] + weights[1] * dst->coords[1];
  isect->coords[2] += weights[0] * org->coords[2] + weights[1] * dst->coords[2];
}

static void wz3__lt_get_intersect_data(wz3__lt_tess *t, wz3__lt_vertex *isect, const wz3__lt_vertex *org_up,
                                       const wz3__lt_vertex *dst_up, const wz3__lt_vertex *org_lo, const wz3__lt_vertex *dst_lo)
{
  double weights[4];
  const int32_t data[4] = {org_up->data, dst_up->data, org_lo->data, dst_lo->data};
  isect->coords[0] = isect->coords[1] = isect->coords[2] = 0;
  wz3__lt_vertex_weights(isect, org_up, dst_up, weights);
  wz3__lt_vertex_weights(isect, org_lo, dst_lo, weights + 2);
  wz3__lt_call_combine(t, isect, data, 1);
}

static int wz3__lt_check_for_right_splice(wz3__lt_tess *t, wz3__lt_region *reg_up)
{
  wz3__lt_region *reg_lo = WZ3__LT_REGION_BELOW(reg_up);
  wz3__lt_halfedge *e_up = reg_up->e_up, *e_lo = reg_lo->e_up;
  if (wz3__lt_vert_leq(e_up->org, e_lo->org)) {
    if (wz3__lt_edge_sign(WZ3__LT_DST(e_lo), e_up->org, e_lo->org) > 0) return 0;
    if (!wz3__lt_vert_eq(e_up->org, e_lo->org)) {
      wz3__lt_split_edge(t, e_lo->sym);
      wz3__lt_mesh_splice(t, e_up, WZ3__LT_OPREV(e_lo));
      reg_up->dirty = reg_lo->dirty = 1;
    } else if (e_up->org != e_lo->org) {
      wz3__lt_pq_remove(t, e_up->org->pq_handle);
      wz3__lt_splice_merge_vertices(t, WZ3__LT_OPREV(e_lo), e_up);
    }
  } else {
    if (wz3__lt_edge_sign(WZ3__LT_DST(e_up), e_lo->org, e_up->org) < 0) return 0;
    WZ3__LT_REGION_ABOVE(reg_up)->dirty = reg_up->dirty = 1;
    wz3__lt_split_edge(t, e_up->sym);
    wz3__lt_mesh_splice(t, WZ3__LT_OPREV(e_lo), e_up);
  }
  return 1;
}

static int wz3__lt_check_for_left_splice(wz3__lt_tess *t, wz3__lt_region *reg_up)
{
  wz3__lt_region *reg_lo = WZ3__LT_REGION_BELOW(reg_up);
  wz3__lt_halfedge *e_up = reg_up->e_up, *e_lo = reg_lo->e_up, *e;
  if (wz3__lt_vert_leq(WZ3__LT_DST(e_up), WZ3__LT_DST(e_lo))) {
    if (wz3__lt_edge_sign(WZ3__LT_DST(e_up), WZ3__LT_DST(e_lo), e_up->org) < 0) return 0;
    WZ3__LT_REGION_ABOVE(reg_up)->dirty = reg_up->dirty = 1;
    e = wz3__lt_split_edge(t, e_up);
    wz3__lt_mesh_splice(t, e_lo->sym, e);
    e->l_face->inside = reg_up->inside;
  } else {
    if (wz3__lt_edge_sign(WZ3__LT_DST(e_lo), WZ3__LT_DST(e_up), e_lo->org) > 0) return 0;
    reg_up->dirty = reg_lo->dirty = 1;
    e = wz3__lt_split_edge(t, e_lo);
    wz3__lt_mesh_splice(t, e_up->l_next, e_lo->sym);
    WZ3__LT_RFACE(e)->inside = reg_up->inside;
  }
  return 1;
}

static int wz3__lt_check_for_intersect(wz3__lt_tess *t, wz3__lt_region *reg_up)
{
  wz3__lt_region *reg_lo = WZ3__LT_REGION_BELOW(reg_up);
  wz3__lt_halfedge *e_up = reg_up->e_up, *e_lo = reg_lo->e_up;
  wz3__lt_vertex *org_up = e_up->org, *org_lo = e_lo->org;
  wz3__lt_vertex *dst_up = WZ3__LT_DST(e_up), *dst_lo = WZ3__LT_DST(e_lo);
  wz3__lt_vertex isect;
  memset(&isect, 0, sizeof isect);
  isect.data = WZ3__LT_NODATA;
  if (org_up == org_lo) return 0;
  const double t_min_up = org_up->t < dst_up->t ? org_up->t : dst_up->t;
  const double t_max_lo = org_lo->t > dst_lo->t ? org_lo->t : dst_lo->t;
  if (t_min_up > t_max_lo) return 0;
  if (wz3__lt_vert_leq(org_up, org_lo)) {
    if (wz3__lt_edge_sign(dst_lo, org_up, org_lo) > 0) return 0;
  } else {
    if (wz3__lt_edge_sign(dst_up, org_lo, org_up) < 0) return 0;
  }
  wz3__lt_edge_intersect(dst_up, org_up, dst_lo, org_lo, &isect);
  if (wz3__lt_vert_leq(&isect, t->event)) { isect.s = t->event->s; isect.t = t->event->t; }
  const wz3__lt_vertex *org_min = wz3__lt_vert_leq(org_up, org_lo) ? org_up : org_lo;
  if (wz3__lt_vert_leq(org_min, &isect)) { isect.s = org_min->s; isect.t = org_min->t; }
  if (wz3__lt_vert_eq(&isect, org_up) || wz3__lt_vert_eq(&isect, org_lo)) {
    wz3__lt_check_for_right_splice(t, reg_up);
    return 0;
  }
  if ((!wz3__lt_vert_eq(dst_up, t->event) && wz3__lt_edge_sign(dst_up, t->event, &isect) >= 0) ||
      (!wz3__lt_vert_eq(dst_lo, t->event) && wz3__lt_edge_sign(dst_lo, t->event, &isect) <= 0)) {
    if (dst_lo == t->event) {
      wz3__lt_split_edge(t, e_up->sym);
      wz3__lt_mesh_splice(t, e_lo->sym, e_up);
      reg_up = wz3__lt_top_left_region(t, reg_up);
      e_up = WZ3__LT_REGION_BELOW(reg_up)->e_up;
      wz3__lt_finish_left_regions(t, WZ3__LT_REGION_BELOW(reg_up), reg_lo);
      wz3__lt_add_right_edges(t, reg_up, WZ3__LT_OPREV(e_up), e_up, e_up, 1);
      return 1;
    }
    if (dst_up == t->event) {
      wz3__lt_split_edge(t, e_lo->sym);
      wz3__lt_mesh_splice(t, e_up->l_next, WZ3__LT_OPREV(e_lo));
      reg_lo = reg_up;
      reg_up = wz3__lt_top_right_region(reg_up);
      wz3__lt_halfedge *e = WZ3__LT_RPREV(WZ3__LT_REGION_BELOW(reg_up)->e_up);
      reg_lo->e_up = WZ3__LT_OPREV(e_lo);
      e_lo = wz3__lt_finish_left_regions(t, reg_lo, NULL);
      wz3__lt_add_right_edges(t, reg_up, e_lo->o_next, WZ3__LT_RPREV(e_up), e, 1);
      return 1;
    }
    if (wz3__lt_edge_sign(dst_up, t->event, &isect) >= 0) {
      WZ3__LT_REGION_ABOVE(reg_up)->dirty = reg_up->dirty = 1;
      wz3__lt_split_edge(t, e_up->sym);
      e_up->org->s = t->event->s;
      e_up->org->t = t->event->t;
    }
    if (wz3__lt_edge_sign(dst_lo, t->event, &isect) <= 0) {
      reg_up->dirty = reg_lo->dirty = 1;
      wz3__lt_split_edge(t, e_lo->sym);
      e_lo->org->s = t->event->s;
      e_lo->org->t = t->event->t;
    }
    return 0;
  }
  wz3__lt_split_edge(t, e_up->sym);
  wz3__lt_split_edge(t, e_lo->sym);
  wz3__lt_mesh_splice(t, WZ3__LT_OPREV(e_lo), e_up);
  e_up->org->s = isect.s;
  e_up->org->t = isect.t;
  e_up->org->pq_handle = wz3__lt_pq_insert(t, e_up->org);
  wz3__lt_get_intersect_data(t, e_up->org, org_up, dst_up, org_lo, dst_lo);
  WZ3__LT_REGION_ABOVE(reg_up)->dirty = reg_up->dirty = reg_lo->dirty = 1;
  return 0;
}

static void wz3__lt_add_right_edges(wz3__lt_tess *t, wz3__lt_region *reg_up, wz3__lt_halfedge *e_first, wz3__lt_halfedge *e_last,
                                    wz3__lt_halfedge *e_top_left, int clean_up)
{
  int first_time = 1;
  wz3__lt_halfedge *e = e_first;
  do {
    wz3__lt_add_region_below(t, reg_up, e->sym);
    e = e->o_next;
  } while (e != e_last);
  if (e_top_left == NULL) e_top_left = WZ3__LT_RPREV(WZ3__LT_REGION_BELOW(reg_up)->e_up);
  wz3__lt_region *reg_prev = reg_up, *reg;
  wz3__lt_halfedge *e_prev = e_top_left;
  for (;;) {
    reg = WZ3__LT_REGION_BELOW(reg_prev);
    e = reg->e_up->sym;
    if (e->org != e_prev->org) break;
    if (e->o_next != e_prev) {
      wz3__lt_mesh_splice(t, WZ3__LT_OPREV(e), e);
      wz3__lt_mesh_splice(t, WZ3__LT_OPREV(e_prev), e);
    }
    reg->winding_number = reg_prev->winding_number - e->winding;
    reg->inside = (reg->winding_number & 1) != 0;
    reg_prev->dirty = 1;
    if (!first_time && wz3__lt_check_for_right_splice(t, reg_prev)) {
      wz3__lt_add_winding(e, e_prev);
      wz3__lt_delete_region(t, reg_prev);
      wz3__lt_delete_edge(t, e_prev);
    }
    first_time = 0;
    reg_prev = reg;
    e_prev = e;
  }
  reg_prev->dirty = 1;
  if (clean_up) wz3__lt_walk_dirty_regions(t, reg_prev);
}

static void wz3__lt_walk_dirty_regions(wz3__lt_tess *t, wz3__lt_region *reg_up)
{
  wz3__lt_region *reg_lo = WZ3__LT_REGION_BELOW(reg_up);
  for (;;) {
    while (reg_lo->dirty) {
      reg_up = reg_lo;
      reg_lo = WZ3__LT_REGION_BELOW(reg_lo);
    }
    if (!reg_up->dirty) {
      reg_lo = reg_up;
      reg_up = WZ3__LT_REGION_ABOVE(reg_up);
      if (reg_up == NULL || !reg_up->dirty) return;
    }
    reg_up->dirty = 0;
    wz3__lt_halfedge *e_up = reg_up->e_up, *e_lo = reg_lo->e_up;
    if (WZ3__LT_DST(e_up) != WZ3__LT_DST(e_lo)) {
      if (wz3__lt_check_for_left_splice(t, reg_up)) {
        if (reg_lo->fix_upper_edge) {
          wz3__lt_delete_region(t, reg_lo);
          wz3__lt_delete_edge(t, e_lo);
          reg_lo = WZ3__LT_REGION_BELOW(reg_up);
          e_lo = reg_lo->e_up;
        } else if (reg_up->fix_upper_edge) {
          wz3__lt_delete_region(t, reg_up);
          wz3__lt_delete_edge(t, e_up);
          reg_up = WZ3__LT_REGION_ABOVE(reg_lo);
          e_up = reg_up->e_up;
        }
      }
    }
    if (e_up->org != e_lo->org) {
      if (WZ3__LT_DST(e_up) != WZ3__LT_DST(e_lo) && !reg_up->fix_upper_edge && !reg_lo->fix_upper_edge &&
          (WZ3__LT_DST(e_up) == t->event || WZ3__LT_DST(e_lo) == t->event)) {
        if (wz3__lt_check_for_intersect(t, reg_up)) return;
      } else {
        wz3__lt_check_for_right_splice(t, reg_up);
      }
    }
    if (e_up->org == e_lo->org && WZ3__LT_DST(e_up) == WZ3__LT_DST(e_lo)) {
      wz3__lt_add_winding(e_lo, e_up);
      wz3__lt_delete_region(t, reg_up);
      wz3__lt_delete_edge(t, e_up);
      reg_up = WZ3__LT_REGION_ABOVE(reg_lo);
    }
  }
}

static void wz3__lt_connect_right_vertex(wz3__lt_tess *t, wz3__lt_region *reg_up, wz3__lt_halfedge *e_bottom_left)
{
  wz3__lt_halfedge *e_top_left = e_bottom_left->o_next;
  wz3__lt_region *reg_lo = WZ3__LT_REGION_BELOW(reg_up);
  wz3__lt_halfedge *e_up = reg_up->e_up, *e_lo = reg_lo->e_up;
  int degenerate = 0;
  if (WZ3__LT_DST(e_up) != WZ3__LT_DST(e_lo)) wz3__lt_check_for_intersect(t, reg_up);
  if (wz3__lt_vert_eq(e_up->org, t->event)) {
    wz3__lt_mesh_splice(t, WZ3__LT_OPREV(e_top_left), e_up);
    reg_up = wz3__lt_top_left_region(t, reg_up);
    e_top_left = WZ3__LT_REGION_BELOW(reg_up)->e_up;
    wz3__lt_finish_left_regions(t, WZ3__LT_REGION_BELOW(reg_up), reg_lo);
    degenerate = 1;
  }
  if (wz3__lt_vert_eq(e_lo->org, t->event)) {
    wz3__lt_mesh_splice(t, e_bottom_left, WZ3__LT_OPREV(e_lo));
    e_bottom_left = wz3__lt_finish_left_regions(t, reg_lo, NULL);
    degenerate = 1;
  }
  if (degenerate) {
    wz3__lt_add_right_edges(t, reg_up, e_bottom_left->o_next, e_top_left, e_top_left, 1);
    return;
  }
  wz3__lt_halfedge *e_new = wz3__lt_vert_leq(e_lo->org, e_up->org) ? WZ3__LT_OPREV(e_lo) : e_up;
  e_new = wz3__lt_connect(t, WZ3__LT_LPREV(e_bottom_left), e_new);
  wz3__lt_add_right_edges(t, reg_up, e_new, e_new->o_next, e_new->o_next, 0);
  e_new->sym->active_region->fix_upper_edge = 1;
  wz3__lt_walk_dirty_regions(t, reg_up);
}

static void wz3__lt_connect_left_degenerate(wz3__lt_tess *t, wz3__lt_region *reg_up, wz3__lt_vertex *v_event)
{
  wz3__lt_halfedge *e = reg_up->e_up;
  if (wz3__lt_vert_eq(e->org, v_event)) return;            /* TOLERANCE_NONZERO_ is false */
  if (!wz3__lt_vert_eq(WZ3__LT_DST(e), v_event)) {
    wz3__lt_split_edge(t, e->sym);
    if (reg_up->fix_upper_edge) {
      wz3__lt_delete_edge(t, e->o_next);
      reg_up->fix_upper_edge = 0;
    }
    wz3__lt_mesh_splice(t, v_event->an_edge, e);
    wz3__lt_sweep_event(t, v_event);
    return;
  }
}

static void wz3__lt_connect_left_vertex(wz3__lt_tess *t, wz3__lt_vertex *v_event)
{
  wz3__lt_region tmp;
  memset(&tmp, 0, sizeof tmp);
  tmp.e_up = v_event->an_edge->sym;
  wz3__lt_region *reg_up = (wz3__lt_region *)wz3__lt_dict_search(t, &tmp)->key;
  wz3__lt_region *reg_lo = WZ3__LT_REGION_BELOW(reg_up);
  wz3__lt_halfedge *e_up = reg_up->e_up, *e_lo = reg_lo->e_up;
  if (wz3__lt_edge_sign(WZ3__LT_DST(e_up), v_event, e_up->org) == 0) {
    wz3__lt_connect_left_degenerate(t, reg_up, v_event);
    return;
  }
  wz3__lt_region *reg = wz3__lt_vert_leq(WZ3__LT_DST(e_lo), WZ3__LT_DST(e_up)) ? reg_up : reg_lo;
  if (reg_up->inside || reg->fix_upper_edge) {
    wz3__lt_halfedge *e_new;
    if (reg == reg_up) {
      e_new = wz3__lt_connect(t, v_event->an_edge->sym, e_up->l_next);
    } else {
      wz3__lt_halfedge *temp = wz3__lt_connect(t, WZ3__LT_DNEXT(e_lo), v_event->an_edge);
      e_new = temp->sym;
    }
    if (reg->fix_upper_edge) wz3__lt_fix_upper_edge(t, reg, e_new);
    else wz3__lt_compute_winding(wz3__lt_add_region_below(t, reg_up, e_new));
    wz3__lt_sweep_event(t, v_event);
  } else {
    wz3__lt_add_right_edges(t, reg_up, v_event->an_edge, v_event->an_edge, NULL, 1);
  }
}

static void wz3__lt_sweep_event(wz3__lt_tess *t, wz3__lt_vertex *v_event)
{
  t->event = v_event;
  wz3__lt_halfedge *e = v_event->an_edge;
  while (e->active_region == NULL) {
    e = e->o_next;
    if (e == v_event->an_edge) {
      wz3__lt_connect_left_vertex(t, v_event);
      return;
    }
  }
  wz3__lt_region *reg_up = wz3__lt_top_left_region(t, e->active_region);
  wz3__lt_region *reg = WZ3__LT_REGION_BELOW(reg_up);
  wz3__lt_halfedge *e_top_left = reg->e_up;
  wz3__lt_halfedge *e_bottom_left = wz3__lt_finish_left_regions(t, reg, NULL);
  if (e_bottom_left->o_next == e_top_left) wz3__lt_connect_right_vertex(t, reg_up, e_bottom_left);
  else wz3__lt_add_right_edges(t, reg_up, e_bottom_left->o_next, e_top_left, e_top_left, 1);
}

static void wz3__lt_add_sentinel(wz3__lt_tess *t, double tt)
{
  wz3__lt_region *reg = (wz3__lt_region *)wz3__lt_alloc(t, sizeof *reg);
  wz3__lt_halfedge *e = wz3__lt_make_edge(t);
  if (!reg || !e) return;
  e->org->s = WZ3__LT_SENTINEL;
  e->org->t = tt;
  WZ3__LT_DST(e)->s = -WZ3__LT_SENTINEL;
  WZ3__LT_DST(e)->t = tt;
  t->event = WZ3__LT_DST(e);
  reg->e_up = e;
  reg->node_up = wz3__lt_dict_insert_before(t, &t->d_head, reg);
  reg->sentinel = 1;
}

static void wz3__lt_remove_degenerate_edges(wz3__lt_tess *t)
{
  wz3__lt_halfedge *e_head = t->e_head, *e_next;
  for (wz3__lt_halfedge *e = e_head->next; e != e_head; e = e_next) {
    e_next = e->next;
    wz3__lt_halfedge *e_l_next = e->l_next;
    if (wz3__lt_vert_eq(e->org, WZ3__LT_DST(e)) && e->l_next->l_next != e) {
      wz3__lt_splice_merge_vertices(t, e_l_next, e);
      wz3__lt_delete_edge(t, e);
      e = e_l_next;
      e_l_next = e->l_next;
    }
    if (e_l_next->l_next == e) {
      if (e_l_next != e) {
        if (e_l_next == e_next || e_l_next == e_next->sym) e_next = e_next->next;
        wz3__lt_delete_edge(t, e_l_next);
      }
      if (e == e_next || e == e_next->sym) e_next = e_next->next;
      wz3__lt_delete_edge(t, e);
    }
  }
}

static void wz3__lt_remove_degenerate_faces(wz3__lt_tess *t)
{
  wz3__lt_face *f_next;
  for (wz3__lt_face *f = t->f_head->next; f != t->f_head; f = f_next) {
    f_next = f->next;
    wz3__lt_halfedge *e = f->an_edge;
    if (e->l_next->l_next == e) {
      wz3__lt_add_winding(e->o_next, e);
      wz3__lt_delete_edge(t, e);
    }
  }
}

static void wz3__lt_compute_interior(wz3__lt_tess *t)
{
  t->fatal_error = 0;
  wz3__lt_remove_degenerate_edges(t);
  /* initPriorityQ_ */
  t->q_size = 0; t->q_initialized = 0;
  if (wz3__lt_heap_init_storage(t)) { wz3__lt_fail(t); return; }
  for (wz3__lt_vertex *v = t->v_head->next; v != t->v_head; v = v->next) v->pq_handle = wz3__lt_pq_insert(t, v);
  wz3__lt_pq_init(t);
  if (t->oom) return;
  /* initEdgeDict_ */
  t->d_head.key = NULL; t->d_head.next = t->d_head.prev = &t->d_head;
  wz3__lt_add_sentinel(t, -WZ3__LT_SENTINEL);
  wz3__lt_add_sentinel(t, WZ3__LT_SENTINEL);
  if (t->oom) return;
  wz3__lt_vertex *v;
  while ((v = wz3__lt_pq_extract_min(t)) != NULL) {
    for (;;) {
      wz3__lt_vertex *v_next = wz3__lt_pq_minimum(t);
      if (v_next == NULL || !wz3__lt_vert_eq(v_next, v)) break;
      v_next = wz3__lt_pq_extract_min(t);
      wz3__lt_splice_merge_vertices(t, v->an_edge, v_next->an_edge);
    }
    wz3__lt_sweep_event(t, v);
    if (t->oom) return;
  }
  wz3__lt_region *min_region = (wz3__lt_region *)t->d_head.next->key;
  t->event = min_region->e_up->org;
  /* doneEdgeDict_ */
  wz3__lt_region *reg;
  while ((reg = (wz3__lt_region *)t->d_head.next->key) != NULL) wz3__lt_delete_region(t, reg);
  wz3__lt_remove_degenerate_faces(t);
}

/* ---- monotone tessellation and output (tessmono.c, render.c) ---- */

static void wz3__lt_tessellate_mono_region(wz3__lt_tess *t, wz3__lt_face *face)
{
  wz3__lt_halfedge *up = face->an_edge, *lo;
  for (; wz3__lt_vert_leq(WZ3__LT_DST(up), up->org); up = WZ3__LT_LPREV(up)) { }
  for (; wz3__lt_vert_leq(up->org, WZ3__LT_DST(up)); up = up->l_next) { }
  lo = WZ3__LT_LPREV(up);
  while (up->l_next != lo) {
    if (wz3__lt_vert_leq(WZ3__LT_DST(up), lo->org)) {
      while (lo->l_next != up && (wz3__lt_edge_goes_left(lo->l_next) ||
             wz3__lt_edge_sign(lo->org, WZ3__LT_DST(lo), WZ3__LT_DST(lo->l_next)) <= 0)) {
        lo = wz3__lt_connect(t, lo->l_next, lo)->sym;
      }
      lo = WZ3__LT_LPREV(lo);
    } else {
      while (lo->l_next != up && (wz3__lt_edge_goes_right(WZ3__LT_LPREV(up)) ||
             wz3__lt_edge_sign(WZ3__LT_DST(up), up->org, WZ3__LT_LPREV(up)->org) >= 0)) {
        up = wz3__lt_connect(t, up, WZ3__LT_LPREV(up))->sym;
      }
      up = up->l_next;
    }
  }
  while (lo->l_next->l_next != up) lo = wz3__lt_connect(t, lo->l_next, lo)->sym;
}

static void wz3__lt_tessellate_interior(wz3__lt_tess *t)
{
  wz3__lt_face *next;
  for (wz3__lt_face *f = t->f_head->next; f != t->f_head; f = next) {
    next = f->next;
    if (f->inside) wz3__lt_tessellate_mono_region(t, f);
    if (t->oom) return;
  }
}

/* renderMesh: every inside face is a triangle after tessellateInterior */
static int wz3__lt_render_mesh(wz3__lt_tess *t)
{
  for (wz3__lt_face *f = t->f_head->prev; f != t->f_head; f = f->prev) {
    if (!f->inside) continue;
    wz3__lt_halfedge *e = f->an_edge;
    if (e->l_next->l_next->l_next != e) return -1;
    if (t->emit(t->user, e->org->data, e->l_next->org->data, e->l_next->l_next->org->data)) return -1;
  }
  return 0;
}

/* ---- polygon API (tess.c) ---- */

static int wz3__lt_begin_polygon(wz3__lt_tess *t)
{
  t->failed = 0;
  t->fatal_error = 0;
  t->last_edge = NULL;
  t->v_head = wz3__lt_new_vertex(t, NULL, NULL);
  t->f_head = wz3__lt_new_face(t, NULL, NULL);
  t->e_head = wz3__lt_new_halfedge(t);
  t->e_head_sym = wz3__lt_new_halfedge(t);
  if (!t->v_head || !t->f_head || !t->e_head || !t->e_head_sym) { wz3__lt_release(t); return -1; }
  t->e_head->sym = t->e_head_sym;
  t->e_head_sym->sym = t->e_head;
  return 0;
}

static void wz3__lt_begin_contour(wz3__lt_tess *t) { t->last_edge = NULL; }

static void wz3__lt_add_vertex(wz3__lt_tess *t, const double coords[3], int32_t data)
{
  wz3__lt_halfedge *e = t->last_edge;
  if (e == NULL) {
    e = wz3__lt_make_edge(t);
    if (!e) return;
    wz3__lt_mesh_splice(t, e, e->sym);
  } else {
    wz3__lt_split_edge(t, e);
    e = e->l_next;
  }
  e->org->data = data;
  e->org->coords[0] = coords[0];
  e->org->coords[1] = coords[1];
  e->org->coords[2] = coords[2];
  e->winding = 1;
  e->sym->winding = -1;
  t->last_edge = e;
}

/* gluTessEndPolygon with the fixed normal (0,0,-1): longAxis is z, so s = x and t = -y */
static int wz3__lt_end_polygon(wz3__lt_tess *t)
{
  volatile int result = 0;      /* assigned after setjmp */
  t->armed = 0;
  if (t->oom || !t->v_head) {
    result = -1;
  } else if (setjmp(t->jump)) {
    t->armed = 0;
    result = -1;                                       /* an allocation failed inside the sweep */
  } else {
    t->armed = 1;
    for (wz3__lt_vertex *v = t->v_head->next; v != t->v_head; v = v->next) {
      v->s = v->coords[0];
      v->t = -1 * v->coords[1];
    }
    wz3__lt_compute_interior(t);
    if (t->oom) result = -1;
    else if (!t->fatal_error) {
      wz3__lt_tessellate_interior(t);
      if (t->oom || wz3__lt_render_mesh(t)) result = -1;
    } else result = -1;
    t->armed = 0;
  }
  wz3__lt_release(t);
  t->v_head = NULL; t->f_head = NULL; t->e_head = NULL; t->e_head_sym = NULL;
  return result || t->failed ? -1 : 0;
}


