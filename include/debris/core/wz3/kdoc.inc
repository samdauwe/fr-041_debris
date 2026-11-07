/* ======================================================================== */
/*  kdoc parser (KDoc::Init compact format)                                 */
/* ======================================================================== */

#define WZ3__MAX_CLASSES 128
#define WZ3__MAX_PACKING_LENGTH 256
#define WZ3__MAX_PARAMETER_SLOTS (4u * 1024u * 1024u)

typedef struct wz3__reader {
  const uint8_t *p;
  size_t pos, len;
  wz3_result err;
} wz3__reader;

static int wz3__rd_have(wz3__reader *r, uint64_t n)
{
  if (r->err != WZ3_OK) return 0;
  if (n > (uint64_t)(r->len - r->pos)) {
    r->err = WZ3_ERR_TRUNCATED;
    return 0;
  }
  return 1;
}

/* returns the start offset, or 0 with r->err set */
static size_t wz3__rd_skip(wz3__reader *r, uint64_t n)
{
  if (!wz3__rd_have(r, n)) return 0;
  const size_t start = r->pos;
  r->pos += (size_t)n;
  return start;
}

static uint32_t wz3__rd_u8(wz3__reader *r)
{
  if (!wz3__rd_have(r, 1)) return 0;
  return r->p[r->pos++];
}

static uint32_t wz3__rd_u16(wz3__reader *r)
{
  if (!wz3__rd_have(r, 2)) return 0;
  const uint32_t v = (uint32_t)r->p[r->pos] | ((uint32_t)r->p[r->pos + 1] << 8);
  r->pos += 2;
  return v;
}

static uint32_t wz3__rd_u24(wz3__reader *r)
{
  const uint32_t a = wz3__rd_u8(r), b = wz3__rd_u8(r), c = wz3__rd_u8(r);
  return a | (b << 8) | (c << 16);
}

static uint32_t wz3__rd_u32(wz3__reader *r)
{
  if (!wz3__rd_have(r, 4)) return 0;
  const uint8_t *q = r->p + r->pos;
  const uint32_t v = (uint32_t)q[0] | ((uint32_t)q[1] << 8) | ((uint32_t)q[2] << 16) | ((uint32_t)q[3] << 24);
  r->pos += 4;
  return v;
}

static uint32_t wz3__rd_compact(wz3__reader *r)
{
  uint32_t v = wz3__rd_u8(r);
  if (v & 0x80u) v = (v & 0x7fu) | (wz3__rd_u8(r) << 7);
  return v;
}

/* NUL-terminated string inside the buffer; the pointer aliases it. */
static const char *wz3__rd_string(wz3__reader *r, size_t max_length)
{
  if (r->err != WZ3_OK) return "";
  const size_t start = r->pos;
  size_t pos = start;
  while (pos < r->len && r->p[pos] != 0) {
    if (pos - start >= max_length) {
      r->err = WZ3_ERR_LIMIT;
      return "";
    }
    pos++;
  }
  if (pos == r->len) {
    r->err = WZ3_ERR_TRUNCATED;
    return "";
  }
  r->pos = pos + 1;
  return (const char *)r->p + start;
}

static float wz3__rd_f16(wz3__reader *r)
{
  const uint32_t first = wz3__rd_u8(r);
  if (first == 0x00) return 0.0f;
  if (first == 0x80) return 1.0f;
  if (first == 0x01) return 0.5f;
  if (first == 0x81) return 0.25f; /* exact released-reader behavior */
  const uint32_t value = (first << 8) | wz3__rd_u8(r);
  const uint32_t bits = ((value & 0x8000u) << 16) | (((((value >> 10) & 31u) + 128u - 16u) & 0xffu) << 23) |
                        ((value & 1023u) << 13);
  return wz3__bits2float(bits);
}

static float wz3__rd_f24(wz3__reader *r)
{
  const uint32_t first = wz3__rd_u8(r);
  if (first == 0x00) return 0.0f;
  if (first == 0x01) return 1.0f;
  if (first == 0xff) return -1.0f;
  const uint32_t second = wz3__rd_u16(r);
  return wz3__bits2float((first << 23) | ((second & 0x8000u) << 16) | ((second & 0x7fffu) << 8));
}

static wz3_word wz3__rd_param(wz3__reader *r, char kind)
{
  wz3_word w;
  w.u = 0;
  if (kind >= 'A' && kind <= 'Z') kind = (char)(kind + 32);
  switch (kind) {
  case 'g': w.f = wz3__rd_f16(r); break;
  case 'f': w.f = wz3__rd_f24(r); break;
  case 'e': {
    const int32_t v = (int16_t)(uint16_t)wz3__rd_u16(r);
    w.f = (float)((double)v / 4096.0);
  } break;
  case 'i': w.i = (int32_t)wz3__rd_u32(r); break;
  case 's': w.i = (int16_t)(uint16_t)wz3__rd_u16(r); break;
  case 'b': w.u = wz3__rd_u8(r); break;
  case 'c': w.u = wz3__rd_u32(r); break;
  case 'm': w.u = wz3__rd_u24(r); break;
  default: break;
  }
  return w;
}

static const uint8_t wz3__anim_payload[34] = {
  0, 0, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 16, 4, 4, 2, 0, 0, 0, 8, 2, 0,
  0, 0,
};

static void *wz3__zalloc(size_t count, size_t size)
{
  if (size && count > ((size_t)-1) / size) return NULL;
  size_t bytes = count * size;
  if (!bytes) bytes = 1;
  void *p = WZ3_MALLOC(bytes);
  if (p) memset(p, 0, bytes);
  return p;
}

WZ3_API void wz3_kx_destroy(wz3_kx *kx)
{
  if (!kx) return;
  if (kx->splines) {
    for (uint32_t i = 0; i < kx->n_splines; i++)
      for (int c = 0; c < 4; c++) {
        WZ3_FREE((void *)kx->splines[i].channels[c].key_time);
        WZ3_FREE((void *)kx->splines[i].channels[c].key_value);
      }
  }
  WZ3_FREE(kx->splines);
  WZ3_FREE(kx->events);
  WZ3_FREE(kx->ops);
  WZ3_FREE(kx->classes);
  for (int i = 0; i < 6; i++) WZ3_FREE(kx->internal_[i]);
  WZ3_FREE(kx);
}

static wz3_result wz3__kx_parse_body(wz3_kx *kx, wz3__reader *r)
{
#define WZ3__FAIL(code) do { if (r->err == WZ3_OK) r->err = (code); return r->err; } while (0)
#define WZ3__CHECK() do { if (r->err != WZ3_OK) return r->err; } while (0)
  const uint8_t *p = r->p;

  const uint32_t flags = wz3__rd_u32(r);
  WZ3__CHECK();
  if (flags & ~7u) WZ3__FAIL(WZ3_ERR_FORMAT);
  kx->flags = flags;
  if (flags & 1u) {
    const size_t at = wz3__rd_skip(r, 32);
    WZ3__CHECK();
    size_t n = 0;
    while (n < 32 && p[at + n]) n++;
    memcpy(kx->name, p + at, n);
    kx->name[n] = 0;
  }

  const uint32_t song_size = wz3__rd_u32(r);
  const size_t song_offset = wz3__rd_skip(r, ((uint64_t)song_size + 3u) & ~(uint64_t)3u);
  WZ3__CHECK();
  kx->song = p + song_offset;
  kx->song_size = song_size;
  kx->sections.song = (uint32_t)song_offset;

  if (flags & 2u) {
    const uint32_t sample_size = wz3__rd_u32(r);
    const size_t sample_offset = wz3__rd_skip(r, ((uint64_t)sample_size + 3u) & ~(uint64_t)3u);
    WZ3__CHECK();
    kx->samples = p + sample_offset;
    kx->sample_size = sample_size;
    kx->sections.samples = (uint32_t)sample_offset;
  }

  kx->sections.document = (uint32_t)r->pos;
  kx->song_bpm_fixed = wz3__rd_u32(r);
  kx->song_length = wz3__rd_u32(r);
  const uint32_t n_ops = wz3__rd_compact(r);
  const uint32_t n_doc_splines = wz3__rd_compact(r);
  for (int i = 0; i < WZ3_MAX_OP_ROOT; i++) kx->roots[i] = wz3__rd_compact(r);
  WZ3__CHECK();

  /* class table */
  kx->classes = (wz3_kx_class *)wz3__zalloc(WZ3__MAX_CLASSES, sizeof(wz3_kx_class));
  if (!kx->classes) WZ3__FAIL(WZ3_ERR_OUT_OF_MEMORY);
  for (;;) {
    const uint32_t convention = wz3__rd_u32(r);
    WZ3__CHECK();
    if (convention == 0) break;
    if (kx->n_classes == WZ3__MAX_CLASSES) WZ3__FAIL(WZ3_ERR_LIMIT);
    wz3_kx_class *c = &kx->classes[kx->n_classes];
    c->convention = convention;
    c->id = (uint16_t)wz3__rd_u16(r);
    c->packing = wz3__rd_string(r, WZ3__MAX_PACKING_LENGTH);
    WZ3__CHECK();
    c->packing_length = (uint32_t)strlen(c->packing);
    kx->n_classes++;
  }
  const uint32_t n_classes = kx->n_classes;

  /* graph: pass 0 validates and counts the input pool, pass 1 fills it */
  kx->sections.graph = (uint32_t)r->pos;
  kx->ops = (wz3_kx_op *)wz3__zalloc(n_ops, sizeof(wz3_kx_op));
  if (!kx->ops) WZ3__FAIL(WZ3_ERR_OUT_OF_MEMORY);
  kx->n_ops = n_ops;
  const size_t graph_pos = r->pos;
  size_t total_inputs = 0, input_cursor = 0;
  uint32_t *input_pool = NULL;
  for (int pass = 0; pass < 2; pass++) {
    if (pass == 1) {
      r->pos = graph_pos;
      input_pool = (uint32_t *)wz3__zalloc(total_inputs, sizeof(uint32_t));
      if (!input_pool) WZ3__FAIL(WZ3_ERR_OUT_OF_MEMORY);
      kx->internal_[1] = input_pool;
    }
    for (uint32_t op_index = 0; op_index < n_ops; op_index++) {
      const uint32_t type = wz3__rd_u8(r);
      WZ3__CHECK();
      const uint32_t class_index = type & 0x7fu;
      if (class_index >= n_classes) WZ3__FAIL(WZ3_ERR_FORMAT);
      const uint32_t convention = kx->classes[class_index].convention;
      uint32_t count;
      if (type & 0x80u) count = 1;
      else if (convention & WZ3_OPC_FLEXINPUT) count = wz3__rd_u8(r);
      else count = (convention >> 8) & 0xfu;
      wz3_kx_op *op = &kx->ops[op_index];
      if (pass == 1) {
        op->class_index = (uint16_t)class_index;
        op->class_id = kx->classes[class_index].id;
        op->n_inputs = count;
        op->inputs = input_pool + input_cursor;
      }
      for (uint32_t i = 0; i < count; i++) {
        const uint32_t delta = (type & 0x80u) ? 0u : wz3__rd_compact(r);
        WZ3__CHECK();
        if (delta + 1u > op_index) WZ3__FAIL(WZ3_ERR_FORMAT);   /* target = op_index - 1 - delta must be >= 0 */
        if (pass == 1) input_pool[input_cursor + i] = op_index - 1u - delta;
      }
      if (pass == 0) total_inputs += count;
      else input_cursor += count;
    }
  }

  /* pools sized from the class table so a tiny document cannot amplify */
  uint32_t *class_ops = (uint32_t *)wz3__zalloc(n_classes, sizeof(uint32_t));
  if (!class_ops) WZ3__FAIL(WZ3_ERR_OUT_OF_MEMORY);
  for (uint32_t i = 0; i < n_ops; i++) class_ops[kx->ops[i].class_index]++;
  uint64_t slots = 0, n_links = 0, n_strings = 0, n_spline_refs = 0;
  for (uint32_t c = 0; c < n_classes; c++) {
    const uint32_t conv = kx->classes[c].convention;
    slots += (uint64_t)class_ops[c] * kx->classes[c].packing_length;
    n_links += (uint64_t)class_ops[c] * ((conv >> 12) & 0xfu);
    n_strings += (uint64_t)class_ops[c] * ((conv >> 16) & 7u);
    n_spline_refs += (uint64_t)class_ops[c] * ((conv >> 20) & 7u);
  }
  WZ3_FREE(class_ops);
  uint64_t budget = (uint64_t)r->len * 4u;
  if (budget < 4096u) budget = 4096u;
  if (budget > WZ3__MAX_PARAMETER_SLOTS) budget = WZ3__MAX_PARAMETER_SLOTS;
  if (slots > budget) WZ3__FAIL(WZ3_ERR_LIMIT);

  int32_t *link_pool = (int32_t *)wz3__zalloc((size_t)n_links, sizeof(int32_t));
  wz3_word *param_pool = (wz3_word *)wz3__zalloc((size_t)slots, sizeof(wz3_word));
  const char **string_pool = (const char **)wz3__zalloc((size_t)n_strings, sizeof(const char *));
  int32_t *spline_pool = (int32_t *)wz3__zalloc((size_t)n_spline_refs, sizeof(int32_t));
  kx->internal_[2] = link_pool;
  kx->internal_[3] = param_pool;
  kx->internal_[4] = string_pool;
  kx->internal_[5] = spline_pool;
  if (!link_pool || !param_pool || !string_pool || !spline_pool) WZ3__FAIL(WZ3_ERR_OUT_OF_MEMORY);

  {
    size_t lc = 0, pc = 0, sc = 0, rc = 0;
    for (uint32_t i = 0; i < n_ops; i++) {
      wz3_kx_op *op = &kx->ops[i];
      const wz3_kx_class *cls = &kx->classes[op->class_index];
      op->n_links = (cls->convention >> 12) & 0xfu;
      op->links = link_pool + lc;
      lc += op->n_links;
      op->params = param_pool + pc;
      pc += cls->packing_length;
      op->n_strings = (cls->convention >> 16) & 7u;
      op->strings = (const char *const *)(string_pool + sc);
      sc += op->n_strings;
      op->n_splines = (cls->convention >> 20) & 7u;
      op->splines = spline_pool + rc;
      rc += op->n_splines;
    }
  }

  for (uint32_t i = 0; i < n_ops; i++) {
    wz3_kx_op *op = &kx->ops[i];
    for (uint32_t k = 0; k < op->n_links; k++) {
      const uint32_t encoded = wz3__rd_compact(r);
      WZ3__CHECK();
      if (encoded && encoded - 1u >= n_ops) WZ3__FAIL(WZ3_ERR_FORMAT);
      ((int32_t *)op->links)[k] = encoded ? (int32_t)(encoded - 1u) : -1;
    }
  }

  kx->sections.parameters = (uint32_t)r->pos;
  for (uint32_t ci = 0; ci < n_classes; ci++) {
    const wz3_kx_class *cls = &kx->classes[ci];
    for (uint32_t i = 0; i < n_ops; i++) {
      wz3_kx_op *op = &kx->ops[i];
      if (op->class_index != ci) continue;
      wz3_word *words = (wz3_word *)op->params;
      for (uint32_t k = 0; k < cls->packing_length; k++) words[k] = wz3__rd_param(r, cls->packing[k]);
      WZ3__CHECK();
      const char **strings = (const char **)op->strings;
      for (uint32_t k = 0; k < op->n_strings; k++) strings[k] = wz3__rd_string(r, (size_t)-1);
      int32_t *refs = (int32_t *)op->splines;
      for (uint32_t k = 0; k < op->n_splines; k++) {
        const uint32_t ref = wz3__rd_u16(r);
        WZ3__CHECK();
        if (ref && ref - 1u >= n_doc_splines) WZ3__FAIL(WZ3_ERR_FORMAT);
        refs[k] = ref ? (int32_t)(ref - 1u) : -1;
      }
      if (cls->convention & WZ3_OPC_BLOB) op->blob_size = wz3__rd_u32(r);
      WZ3__CHECK();
    }
  }

  kx->sections.animations = (uint32_t)r->pos;
  for (uint32_t i = 0; i < n_ops; i++) {
    const size_t start = r->pos;
    for (;;) {
      const uint32_t command = wz3__rd_u8(r);
      WZ3__CHECK();
      if (command >= 0x80u) {
        wz3__rd_skip(r, 1);
      } else {
        if (command >= sizeof wz3__anim_payload) WZ3__FAIL(WZ3_ERR_FORMAT);
        wz3__rd_skip(r, wz3__anim_payload[command]);
      }
      WZ3__CHECK();
      if (command == 1) break;
    }
    kx->ops[i].anim = p + start;
    kx->ops[i].anim_size = (uint32_t)(r->pos - start);
  }

  kx->sections.events = (uint32_t)r->pos;
  const uint32_t n_events = wz3__rd_compact(r);
  WZ3__CHECK();
  if ((uint64_t)n_events * 29u > (uint64_t)(r->len - r->pos)) WZ3__FAIL(WZ3_ERR_TRUNCATED);
  kx->events = (wz3_kx_event *)wz3__zalloc(n_events, sizeof(wz3_kx_event));
  if (!kx->events) WZ3__FAIL(WZ3_ERR_OUT_OF_MEMORY);
  for (uint32_t i = 0; i < n_events; i++) {
    wz3_kx_event *e = &kx->events[i];
    e->operation = wz3__rd_compact(r);
    WZ3__CHECK();
    if (e->operation >= n_ops) WZ3__FAIL(WZ3_ERR_FORMAT);
    e->start = (int32_t)wz3__rd_u32(r);
    e->end = (int32_t)wz3__rd_u32(r);
    e->velocity = wz3__rd_f24(r);
    e->modulation = wz3__rd_f24(r);
    e->select = (uint8_t)wz3__rd_u8(r);
    for (int k = 0; k < 3; k++) e->scale[k] = wz3__rd_f24(r);
    for (int k = 0; k < 3; k++) e->rotate[k] = wz3__rd_f24(r);
    for (int k = 0; k < 3; k++) e->translate[k] = wz3__rd_f24(r);
    e->color = wz3__rd_u32(r);
    const uint32_t ref = wz3__rd_compact(r);
    e->spline = ref ? (int32_t)(ref - 1u) : -1;
    e->start_interval = wz3__rd_f16(r);
    e->end_interval = wz3__rd_f16(r);
    e->flags = (uint8_t)wz3__rd_u8(r);
    WZ3__CHECK();
    if (e->spline >= 0 && (uint32_t)e->spline >= n_doc_splines) WZ3__FAIL(WZ3_ERR_FORMAT);
    kx->n_events = i + 1;
  }
  kx->n_events = n_events;

  kx->sections.splines = (uint32_t)r->pos;
  if ((uint64_t)n_doc_splines > (uint64_t)(r->len - r->pos)) WZ3__FAIL(WZ3_ERR_TRUNCATED);
  kx->splines = (wz3_kx_spline *)wz3__zalloc(n_doc_splines, sizeof(wz3_kx_spline));
  if (!kx->splines) WZ3__FAIL(WZ3_ERR_OUT_OF_MEMORY);
  kx->n_splines = n_doc_splines;
  for (uint32_t i = 0; i < n_doc_splines; i++) {
    wz3_kx_spline *s = &kx->splines[i];
    const uint32_t descriptor = wz3__rd_u8(r);
    WZ3__CHECK();
    const uint32_t channels = descriptor & 7u;
    if (channels > 4) WZ3__FAIL(WZ3_ERR_FORMAT);
    s->interpolation = descriptor >> 3;
    s->n_channels = channels;
    for (uint32_t c = 0; c < channels; c++) {
      const uint32_t key_count = wz3__rd_compact(r);
      WZ3__CHECK();
      if ((uint64_t)key_count * 2u > (uint64_t)(r->len - r->pos)) WZ3__FAIL(WZ3_ERR_TRUNCATED);
      uint8_t *times = (uint8_t *)wz3__zalloc(key_count, 1);
      float *values = (float *)wz3__zalloc(key_count, sizeof(float));
      s->channels[c].key_time = times;
      s->channels[c].key_value = values;
      if (!times || !values) WZ3__FAIL(WZ3_ERR_OUT_OF_MEMORY);
      s->channels[c].n_keys = key_count;
      for (uint32_t k = 0; k < key_count; k++) {
        times[k] = (uint8_t)wz3__rd_u8(r);
        values[k] = wz3__rd_f16(r);
      }
      WZ3__CHECK();
    }
  }

  kx->sections.blobs = (uint32_t)r->pos;
  for (uint32_t i = 0; i < n_ops; i++) {
    wz3_kx_op *op = &kx->ops[i];
    if (!op->blob_size) continue;
    const size_t offset = wz3__rd_skip(r, op->blob_size);
    WZ3__CHECK();
    op->blob = p + offset;
  }
  kx->sections.end = (uint32_t)r->pos;
  return WZ3_OK;
#undef WZ3__FAIL
#undef WZ3__CHECK
}

WZ3_API wz3_result wz3_kx_parse(wz3_kx **out, const void *data, size_t size, size_t *error_offset)
{
  if (error_offset) *error_offset = 0;
  if (out) *out = NULL;
  if (!out || (!data && size)) return WZ3_ERR_ARGUMENT;
  if (size > 0xffffffffu) return WZ3_ERR_LIMIT; /* section offsets are 32-bit */
  wz3_kx *kx = (wz3_kx *)wz3__zalloc(1, sizeof(wz3_kx));
  uint8_t *copy = (uint8_t *)WZ3_MALLOC(size ? size : 1);
  if (!kx || !copy) {
    WZ3_FREE(kx);
    WZ3_FREE(copy);
    return WZ3_ERR_OUT_OF_MEMORY;
  }
  if (size) memcpy(copy, data, size);
  kx->internal_[0] = copy;
  wz3__reader r;
  r.p = copy;
  r.pos = 0;
  r.len = size;
  r.err = WZ3_OK;
  const wz3_result res = wz3__kx_parse_body(kx, &r);
  if (res != WZ3_OK) {
    if (error_offset) *error_offset = r.pos;
    wz3_kx_destroy(kx);
    return res;
  }
  *out = kx;
  return WZ3_OK;
}

