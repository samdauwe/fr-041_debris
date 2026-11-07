/* ======================================================================== */
/*  graph: bitmap operator evaluation                                       */
/* ======================================================================== */

#define WZ3__CLASS_BITMAP_RENDER 0x3fu

struct wz3_graph {
  const wz3_kx *kx;
  wz3_bitmap_ctx ctx;
  int release;
  wz3_bitmap_render_fn render;
  void *render_user;
  int render_placeholder;
  uint8_t *render_dep; /* the operator is a Bitmap_Render or consumes one */
  wz3_bitmap **bm;
  uint8_t *state;      /* 0 pending, 1 computed, 2 failed */
  uint8_t *pinned;
  uint8_t *mark;
  wz3_result *err;
  uint32_t *total;     /* bitmap consumers of each operator (per input slot) */
  uint32_t *remaining;
  uint32_t *stack;
};

/* inputs/data words required by each evaluable class */
typedef struct wz3__bitmap_spec { uint16_t id; uint8_t inputs, words, strings; } wz3__bitmap_spec;
static const wz3__bitmap_spec wz3__bitmap_specs[] = {
  { 0x21, 0, 3, 0 }, { 0x22, 0, 11, 0 }, { 0x23, 1, 2, 0 }, { 0x24, 1, 1, 0 }, { 0x25, 1, 3, 0 },
  { 0x27, 1, 11, 0 }, { 0x29, 1, 4, 0 }, { 0x2a, 3, 1, 0 }, { 0x2b, 1, 4, 0 }, { 0x2c, 1, 8, 0 },
  { 0x2d, 2, 2, 0 }, { 0x2e, 1, 2, 0 }, { 0x30, 2, 14, 0 }, { 0x31, 1, 9, 2 }, { 0x32, 0, 13, 0 },
  { 0x34, 0, 8, 0 }, { 0x35, 1, 3, 0 }, { 0x36, 1, 10, 0 }, { 0x38, 1, 4, 0 }, { 0x39, 1, 11, 0 },
  { 0x3a, 0, 0, 1 }, { 0x3b, 1, 9, 0 }, { 0x3d, 0, 14, 0 }, { 0x3e, 1, 1, 0 },
};

static const wz3__bitmap_spec *wz3__find_spec(uint16_t id)
{
  for (size_t i = 0; i < sizeof wz3__bitmap_specs / sizeof wz3__bitmap_specs[0]; i++)
    if (wz3__bitmap_specs[i].id == id) return &wz3__bitmap_specs[i];
  return NULL;
}

/* A data word read the way the JS port decodes it from the class's packing
   character: float kinds as floats, i/s as signed, everything else unsigned. */
static float wz3__word_f(const wz3_kx_class *cls, const wz3_kx_op *op, uint32_t i)
{
  const char k = cls->packing[i];
  if (k == 'g' || k == 'f' || k == 'e' || k == 'G' || k == 'F' || k == 'E') return op->params[i].f;
  if (k == 'i' || k == 's' || k == 'I' || k == 'S') return (float)op->params[i].i;
  return (float)op->params[i].u;
}

static int32_t wz3__word_i(const wz3_kx_class *cls, const wz3_kx_op *op, uint32_t i)
{
  const char k = cls->packing[i];
  if (k == 'g' || k == 'f' || k == 'e' || k == 'G' || k == 'F' || k == 'E')
    return wz3__ftoi((double)op->params[i].f);
  return op->params[i].i;
}

#pragma push_macro("WZ3__PI")
#undef WZ3__PI
#define WZ3__PF(i) wz3__word_f(cls, op, (i))
#define WZ3__PI(i) wz3__word_i(cls, op, (i))
#define WZ3__PU(i) ((uint32_t)wz3__word_i(cls, op, (i)))

static wz3_bitmap *wz3__run_bitmap_op(const wz3_graph *g, const wz3_kx_op *op, const wz3_bitmap *const *in)
{
  const wz3_kx *kx = g->kx;
  const wz3_kx_class *cls = &kx->classes[op->class_index];
  const wz3_bitmap_ctx *ctx = &g->ctx;
  switch (op->class_id) {
  case 0x21: return wz3_bitmap_flat(ctx, WZ3__PU(0), WZ3__PU(1), WZ3__PU(2));
  case 0x22:
    return wz3_bitmap_perlin(ctx, WZ3__PU(0), WZ3__PU(1), WZ3__PI(2), WZ3__PI(3), WZ3__PF(4), WZ3__PI(5),
                             WZ3__PI(6), WZ3__PF(7), WZ3__PF(8), WZ3__PU(9), WZ3__PU(10));
  case 0x23: return wz3_bitmap_color(in[0], WZ3__PI(0), WZ3__PU(1));
  case 0x24: return wz3_bitmap_merge(WZ3__PI(0), in, (int32_t)op->n_inputs);
  case 0x25: return wz3_bitmap_format(in[0], WZ3__PI(0), WZ3__PI(1), WZ3__PI(2));
  case 0x27:
  case 0x39:
    return wz3_bitmap_glowrect(in[0], WZ3__PF(0), WZ3__PF(1), WZ3__PF(2), WZ3__PF(3), WZ3__PF(4), WZ3__PF(5),
                               WZ3__PU(6), WZ3__PF(7), WZ3__PF(8), WZ3__PI(9), WZ3__PI(10));
  case 0x29: return wz3_bitmap_blur(in[0], WZ3__PI(0), WZ3__PF(1), WZ3__PF(2), WZ3__PF(3));
  case 0x2a: return wz3_bitmap_mask(in[0], in[1], in[2], WZ3__PI(0));
  case 0x2b: return wz3_bitmap_hscb(in[0], WZ3__PF(0), WZ3__PF(1), WZ3__PF(2), WZ3__PF(3));
  case 0x2c:
    return wz3_bitmap_rotate(ctx, in[0], WZ3__PF(0), WZ3__PF(1), WZ3__PF(2), WZ3__PF(3), WZ3__PF(4),
                             WZ3__PI(5), WZ3__PI(6), WZ3__PI(7));
  case 0x2d: return wz3_bitmap_distort(in[0], in[1], WZ3__PF(0), WZ3__PI(1));
  case 0x2e: return wz3_bitmap_normals(in[0], WZ3__PF(0), WZ3__PI(1));
  case 0x30:
    return wz3_bitmap_bump(in[0], in[1], WZ3__PI(0), WZ3__PF(1), WZ3__PF(2), WZ3__PF(3), WZ3__PF(4),
                           WZ3__PF(5), WZ3__PU(6), WZ3__PU(7), WZ3__PF(8), WZ3__PF(9), WZ3__PF(10),
                           WZ3__PU(11), WZ3__PF(12), WZ3__PF(13));
  case 0x31:
    return wz3_bitmap_text(ctx, op->blob, op->blob_size, in[0], WZ3__PF(0), WZ3__PF(1), WZ3__PF(2), WZ3__PF(3),
                           WZ3__PU(4), WZ3__PI(5), WZ3__PF(6), WZ3__PF(7), WZ3__PF(8), op->strings[0],
                           op->strings[1], NULL);
  case 0x32:
    return wz3_bitmap_cell(ctx, WZ3__PU(0), WZ3__PU(1), WZ3__PU(2), WZ3__PU(3), WZ3__PU(4), WZ3__PI(5),
                           WZ3__PI(6), WZ3__PF(7), WZ3__PF(8), WZ3__PI(9), WZ3__PF(10), WZ3__PI(11),
                           WZ3__PF(12));
  case 0x34:
    return wz3_bitmap_gradient(ctx, WZ3__PU(0), WZ3__PU(1), WZ3__PU(2), WZ3__PU(3), WZ3__PF(4), WZ3__PF(5),
                               WZ3__PF(6), WZ3__PI(7));
  case 0x35: return wz3_bitmap_range(in[0], WZ3__PI(0), WZ3__PU(1), WZ3__PU(2));
  case 0x36:
    return wz3_bitmap_rotate_mul(ctx, in[0], WZ3__PF(0), WZ3__PF(1), WZ3__PF(2), WZ3__PF(3), WZ3__PF(4),
                                 WZ3__PI(5), WZ3__PU(6), WZ3__PI(7), WZ3__PI(8), WZ3__PU(9));
  case 0x38: return wz3_bitmap_sharpen(in[0], WZ3__PI(0), WZ3__PF(1), WZ3__PF(2), WZ3__PF(3));
  case 0x3a: return wz3_bitmap_import(ctx, op->blob, op->blob_size, op->strings[0]);
  case 0x3b: {
    float v[9];
    for (uint32_t i = 0; i < 9; i++) v[i] = WZ3__PF(i);
    return wz3_bitmap_color_balance(in[0], v);
  }
  case 0x3d:
    return wz3_bitmap_bricks(ctx, WZ3__PU(0), WZ3__PU(1), WZ3__PU(2), WZ3__PU(3), WZ3__PU(4), WZ3__PF(5),
                             WZ3__PF(6), WZ3__PI(7), WZ3__PI(8), WZ3__PI(9), WZ3__PI(10), WZ3__PI(11),
                             WZ3__PF(12), WZ3__PF(13));
  case 0x3e: return wz3_bitmap_bulge(in[0], WZ3__PF(0));
  default: return NULL;
  }
}

#undef WZ3__PF
#undef WZ3__PI
#undef WZ3__PU
#pragma pop_macro("WZ3__PI")

/* Bitmap_Render: the host renders the scene of the operator's input (Exec_Bitmap_Render itself is empty in the player). */
static void wz3__graph_run_render(wz3_graph *g, uint32_t o)
{
  const wz3_kx *kx = g->kx;
  const wz3_kx_op *op = &kx->ops[o];
  const wz3_kx_class *cls = &kx->classes[op->class_index];
  if (cls->packing_length < 2) {
    g->state[o] = 2;
    g->err[o] = WZ3_ERR_FORMAT;
    return;
  }
  wz3_bitmap_render_request req;
  memset(&req, 0, sizeof req);
  req.op = o;
  req.has_input = op->n_inputs > 0;
  req.input = req.has_input ? op->inputs[0] : 0;
  req.xs = (uint32_t)wz3__word_i(cls, op, 0);
  req.ys = (uint32_t)wz3__word_i(cls, op, 1);
  wz3__bitmap_dims(&g->ctx, req.xs, req.ys, &req.width, &req.height);
  wz3_bitmap *result = NULL;
  wz3_result failure = WZ3_ERR_UNSUPPORTED;
  if (g->render) {
    failure = g->render(g->render_user, &req, &result);
    if (failure == WZ3_OK && !result) failure = WZ3_ERR_OPERATOR;
    if (failure != WZ3_OK) {
      wz3_bitmap_destroy(result);
      result = NULL;
    }
  }
  if (failure == WZ3_ERR_UNSUPPORTED && g->render_placeholder) {
    result = wz3__new_bitmap(&g->ctx, req.xs, req.ys);
    failure = result ? WZ3_OK : WZ3_ERR_OUT_OF_MEMORY;
  }
  if (failure != WZ3_OK) {
    g->state[o] = 2;
    g->err[o] = failure;
    return;
  }
  g->bm[o] = result;
  g->state[o] = 1;
}

static void wz3__graph_free_slot(wz3_graph *g, uint32_t i)
{
  wz3_bitmap_destroy(g->bm[i]);
  g->bm[i] = NULL;
  g->state[i] = 0;
  g->remaining[i] = g->total[i];
}

WZ3_API void wz3_graph_destroy(wz3_graph *g)
{
  if (!g) return;
  if (g->bm && g->kx)
    for (uint32_t i = 0; i < g->kx->n_ops; i++) wz3_bitmap_destroy(g->bm[i]);
  WZ3_FREE(g->bm);
  WZ3_FREE(g->render_dep);
  WZ3_FREE(g->state);
  WZ3_FREE(g->pinned);
  WZ3_FREE(g->mark);
  WZ3_FREE(g->err);
  WZ3_FREE(g->total);
  WZ3_FREE(g->remaining);
  WZ3_FREE(g->stack);
  WZ3_FREE(g);
}

WZ3_API wz3_graph *wz3_graph_create(const wz3_kx *kx, const wz3_graph_options *options, wz3_result *result)
{
  if (result) *result = WZ3_OK;
  if (!kx) {
    if (result) *result = WZ3_ERR_ARGUMENT;
    return NULL;
  }
  wz3_graph *g = (wz3_graph *)wz3__zalloc(1, sizeof(wz3_graph));
  if (g) {
    const uint32_t n = kx->n_ops;
    g->kx = kx;
    if (options) {
      g->ctx = options->ctx;
      g->release = options->release_consumed;
      g->render = options->render;
      g->render_user = options->render_user;
      g->render_placeholder = options->render_placeholder;
    } else {
      wz3_bitmap_ctx_init(&g->ctx);
    }
    g->bm = (wz3_bitmap **)wz3__zalloc(n, sizeof(wz3_bitmap *));
    g->render_dep = (uint8_t *)wz3__zalloc(n, 1);
    g->state = (uint8_t *)wz3__zalloc(n, 1);
    g->pinned = (uint8_t *)wz3__zalloc(n, 1);
    g->mark = (uint8_t *)wz3__zalloc(n, 1);
    g->err = (wz3_result *)wz3__zalloc(n, sizeof(wz3_result));
    g->total = (uint32_t *)wz3__zalloc(n, sizeof(uint32_t));
    g->remaining = (uint32_t *)wz3__zalloc(n, sizeof(uint32_t));
    g->stack = (uint32_t *)wz3__zalloc(n, sizeof(uint32_t));
  }
  if (!g || !g->bm || !g->render_dep || !g->state || !g->pinned || !g->mark || !g->err || !g->total || !g->remaining || !g->stack) {
    wz3_graph_destroy(g);
    if (result) *result = WZ3_ERR_OUT_OF_MEMORY;
    return NULL;
  }
  for (uint32_t i = 0; i < kx->n_ops; i++) {
    const wz3_kx_op *op = &kx->ops[i];
    if (op->class_id == WZ3__CLASS_BITMAP_RENDER) {
      g->render_dep[i] = 1;
      continue;
    }
    if (!wz3__find_spec(op->class_id)) continue;
    for (uint32_t k = 0; k < op->n_inputs; k++) {
      g->total[op->inputs[k]]++;
      if (op->inputs[k] < i && g->render_dep[op->inputs[k]]) g->render_dep[i] = 1;
    }
  }
  memcpy(g->remaining, g->total, (size_t)kx->n_ops * sizeof(uint32_t));
  return g;
}

static void wz3__graph_run(wz3_graph *g, uint32_t o)
{
  const wz3_kx *kx = g->kx;
  const wz3_kx_op *op = &kx->ops[o];
  const wz3__bitmap_spec *spec = wz3__find_spec(op->class_id);
  const wz3_kx_class *cls = &kx->classes[op->class_index];
  if (op->class_id == WZ3__CLASS_BITMAP_RENDER) {
    wz3__graph_run_render(g, o);
    return;
  }
  if (!spec) {
    g->state[o] = 2;
    g->err[o] = WZ3_ERR_UNSUPPORTED;
    return;
  }
  if (op->n_inputs < spec->inputs || cls->packing_length < spec->words || op->n_strings < spec->strings) {
    g->state[o] = 2;
    g->err[o] = WZ3_ERR_FORMAT;
    return;
  }
  const wz3_bitmap *stackbuf[16];
  const wz3_bitmap **in = stackbuf;
  if (op->n_inputs > 16) {
    in = (const wz3_bitmap **)wz3__zalloc(op->n_inputs, sizeof(*in));
    if (!in) {
      g->state[o] = 2;
      g->err[o] = WZ3_ERR_OUT_OF_MEMORY;
      return;
    }
  }
  wz3_result failure = WZ3_OK;
  for (uint32_t k = 0; k < op->n_inputs && failure == WZ3_OK; k++) {
    const uint32_t src = op->inputs[k];
    if (g->state[src] == 1) in[k] = g->bm[src];
    else failure = g->state[src] == 2 ? g->err[src] : WZ3_ERR_OPERATOR;
  }
  wz3_bitmap *result = NULL;
  if (failure == WZ3_OK) {
    result = wz3__run_bitmap_op(g, op, in);
    if (!result) failure = WZ3_ERR_OPERATOR;
  }
  if (failure != WZ3_OK) {
    g->state[o] = 2;
    g->err[o] = failure;
  } else {
    g->bm[o] = result;
    g->state[o] = 1;
    if (g->release) {
      for (uint32_t k = 0; k < op->n_inputs; k++) {
        const uint32_t src = op->inputs[k];
        if (g->remaining[src] > 0 && --g->remaining[src] == 0 && !g->pinned[src]) wz3__graph_free_slot(g, src);
      }
    }
  }
  if (in != stackbuf) WZ3_FREE((void *)in);
}

WZ3_API wz3_result wz3_graph_eval_bitmap(wz3_graph *g, uint32_t op, const wz3_bitmap **out)
{
  if (out) *out = NULL;
  if (!g || !out || op >= g->kx->n_ops) return WZ3_ERR_ARGUMENT;
  if (g->state[op] == 1) {
    g->pinned[op] = 1;
    *out = g->bm[op];
    return WZ3_OK;
  }
  if (g->state[op] == 2) return g->err[op];

  /* collect every uncomputed bitmap ancestor, then run them in index order
     (inputs always precede their consumers) */
  const wz3_kx *kx = g->kx;
  size_t sp = 0;
  uint32_t lowest = op;
  g->mark[op] = 1;
  g->stack[sp++] = op;
  while (sp) {
    const uint32_t o = g->stack[--sp];
    if (!wz3__find_spec(kx->ops[o].class_id)) continue;   /* fails at its own run */
    for (uint32_t k = 0; k < kx->ops[o].n_inputs; k++) {
      const uint32_t src = kx->ops[o].inputs[k];
      if (g->mark[src] || g->state[src] != 0) continue;
      g->mark[src] = 1;
      g->stack[sp++] = src;
      if (src < lowest) lowest = src;
    }
  }
  for (uint32_t o = lowest; o <= op; o++) {
    if (!g->mark[o]) continue;
    g->mark[o] = 0;
    wz3__graph_run(g, o);
  }
  if (g->state[op] != 1) return g->err[op];
  g->pinned[op] = 1;
  *out = g->bm[op];
  return WZ3_OK;
}

WZ3_API void wz3_graph_release(wz3_graph *g, uint32_t op)
{
  if (!g || op >= g->kx->n_ops) return;
  g->pinned[op] = 0;
  if (g->state[op] == 1) wz3__graph_free_slot(g, op);
}

WZ3_API int wz3_graph_render_dependent(const wz3_graph *g, uint32_t op)
{
  return g && op < g->kx->n_ops && g->render_dep[op];
}

