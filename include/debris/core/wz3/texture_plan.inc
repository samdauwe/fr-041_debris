/* ======================================================================== */
/*  texture upload plan, sampler state and texture quality                  */
/*  (the texture cache and the sampler slots of a material pass in          */
/*  engine.cpp, and the texture size offset of a quality setting)           */
/* ======================================================================== */

#define WZ3__TEX_MAX_SIZE 16384

WZ3_API wz3_result wz3_texture_policy_make(wz3_texture_policy *out, int mode, int hardware_s3tc)
{
  if (!out) return WZ3_ERR_ARGUMENT;
  if (mode != WZ3_DXT5_AUTO && mode != WZ3_DXT5_S3TC && mode != WZ3_DXT5_RGBA8) return WZ3_ERR_ARGUMENT;
  if (mode == WZ3_DXT5_S3TC && !hardware_s3tc) return WZ3_ERR_UNSUPPORTED;
  out->s3tc = (mode != WZ3_DXT5_RGBA8 && hardware_s3tc) ? 1 : 0;
  return WZ3_OK;
}

WZ3_API void wz3_texture_desc_from_bitmap(wz3_texture_desc *out, const wz3_bitmap *bitmap)
{
  if (!out) return;
  memset(out, 0, sizeof *out);
  if (!bitmap) return;
  out->width = bitmap->width;
  out->height = bitmap->height;
  out->format = bitmap->format;
  out->mip_count = bitmap->mip_count;
  out->mip_threshold = bitmap->mip_threshold;
  out->has_data = bitmap->data != NULL && bitmap->width >= 1 && bitmap->height >= 1;
  out->data_id = (uint64_t)(uintptr_t)bitmap->data;
}

WZ3_API int32_t wz3_texture_mip_levels(int32_t width, int32_t height, int32_t mip_count)
{
  if (width < 1 || height < 1) return 0;
  if ((width & (width - 1)) || (height & (height - 1))) return 1;
  const int32_t wp = wz3__tex_power(width), hp = wz3__tex_power(height);
  const int32_t shorter = wp < hp ? wp : hp;
  if (mip_count == 0) return shorter + 1;
  int32_t n = mip_count < shorter ? mip_count : shorter;
  return n < 1 ? 1 : n;
}

/* rgba8MipChainByteLength / dxt5MipChainByteLength */
static uint64_t wz3__tex_chain_bytes(int32_t width, int32_t height, int32_t levels, int dxt5)
{
  uint64_t total = 0;
  for (int32_t level = 0; level < levels && width > 0 && height > 0; level++) {
    total += dxt5 ? (uint64_t)((width + 3) >> 2) * (uint64_t)((height + 3) >> 2) * 16u : (uint64_t)width * (uint64_t)height * 4u;
    if (width <= 1 || height <= 1) break;
    width >>= 1;
    height >>= 1;
  }
  return total;
}

static int wz3__tex_desc_valid(const wz3_texture_desc *d) { return d->has_data && d->width >= 1 && d->height >= 1; }

/* The identity TextureCache keeps for a bitmap. With preflight, the DXT5 quality check of get() (forEachDxt5Mip's RangeError). */
static wz3_result wz3__tex_describe(wz3_texture_entry *e, const wz3_texture_policy *policy, const wz3_texture_desc *d, int preflight)
{
  if (preflight && d->format == 9 && (d->mip_threshold < 0 || d->mip_threshold >= 64)) return WZ3_ERR_UNSUPPORTED;
  memset(e, 0, sizeof *e);
  e->present = 1;
  e->data_id = d->data_id;
  e->width = d->width;
  e->height = d->height;
  e->mip_levels = wz3_texture_mip_levels(d->width, d->height, d->mip_count);
  e->mipmapped = e->mip_levels > 1;
  e->mip_threshold = d->mip_threshold;
  e->format = d->format;
  e->storage = d->format == 9 ? (policy->s3tc ? WZ3_STORAGE_DXT5 : WZ3_STORAGE_DXT5_RGBA8) : WZ3_STORAGE_RGBA8;
  e->byte_length = wz3__tex_chain_bytes(d->width, d->height, e->mip_levels, e->storage == WZ3_STORAGE_DXT5);
  return WZ3_OK;
}

static int wz3__tex_entry_matches(const wz3_texture_entry *a, const wz3_texture_entry *b)
{
  return a->present && a->data_id == b->data_id && a->width == b->width && a->height == b->height && a->mipmapped == b->mipmapped &&
         a->mip_levels == b->mip_levels && a->mip_threshold == b->mip_threshold && a->format == b->format && a->storage == b->storage;
}

static wz3_texture_command *wz3__tex_command(wz3_texture_plan *p, int kind)
{
  wz3_texture_command *c = &p->commands[p->n_commands++];
  memset(c, 0, sizeof *c);
  c->kind = (uint8_t)kind;
  return c;
}

static void wz3__tex_parameter(wz3_texture_plan *p, uint32_t pname, int32_t value)
{
  wz3_texture_command *c = wz3__tex_command(p, WZ3_TEXCMD_PARAMETER);
  c->pname = pname;
  c->value = value;
}

static void wz3__tex_image(wz3_texture_plan *p, int kind, int32_t level, int32_t width, int32_t height, uint32_t internal_format,
                           uint32_t type, size_t size)
{
  wz3_texture_command *c = wz3__tex_command(p, kind);
  c->level = level;
  c->width = width;
  c->height = height;
  c->internal_format = internal_format;
  if (kind == WZ3_TEXCMD_IMAGE) {
    c->format = WZ3_GL_RGBA;
    c->type = type;
  }
  c->size = size;
}

WZ3_API wz3_result wz3_texture_plan_make(wz3_texture_plan *out, const wz3_texture_policy *policy, const wz3_texture_desc *desc)
{
  if (!out || !policy || !desc) return WZ3_ERR_ARGUMENT;
  if (!wz3__tex_desc_valid(desc)) return WZ3_ERR_ARGUMENT;
  if (desc->width > WZ3__TEX_MAX_SIZE || desc->height > WZ3__TEX_MAX_SIZE) return WZ3_ERR_UNSUPPORTED;
  wz3_texture_entry entry;
  const wz3_result r = wz3__tex_describe(&entry, policy, desc, 1);
  if (r != WZ3_OK) return r;
  memset(out, 0, sizeof *out);
  out->entry = entry;
  out->allow_bc3 = entry.storage == WZ3_STORAGE_DXT5;
  const int format = desc->format;
  const int snorm = format == 5;
  wz3__tex_command(out, WZ3_TEXCMD_PIXEL_STORE)->pname = WZ3_GL_UNPACK_ALIGNMENT;
  out->commands[0].value = 1;
  if (format == 9 || (entry.mipmapped && format != 8)) {
    /* one upload per level: forEachDxt5Mip / forEachSourceWordMip */
    int32_t w = desc->width, h = desc->height;
    for (int32_t level = 0; level < entry.mip_levels; level++) {
      if (format == 9 && entry.storage == WZ3_STORAGE_DXT5)
        wz3__tex_image(out, WZ3_TEXCMD_COMPRESSED, level, w, h, WZ3_GL_COMPRESSED_RGBA_S3TC_DXT5, 0,
                       (size_t)((w + 3) >> 2) * (size_t)((h + 3) >> 2) * 16u);
      else
        wz3__tex_image(out, WZ3_TEXCMD_IMAGE, level, w, h, snorm ? WZ3_GL_RGBA8_SNORM : WZ3_GL_RGBA8,
                       snorm ? WZ3_GL_BYTE : WZ3_GL_UNSIGNED_BYTE, (size_t)w * (size_t)h * 4u);
      if (w <= 1 || h <= 1) break;
      w >>= 1;
      h >>= 1;
    }
  } else {
    /* the base level, then the GPU derives the chain (format 8) */
    wz3__tex_image(out, WZ3_TEXCMD_IMAGE, 0, desc->width, desc->height, snorm ? WZ3_GL_RGBA8_SNORM : WZ3_GL_RGBA8,
                   snorm ? WZ3_GL_BYTE : WZ3_GL_UNSIGNED_BYTE, (size_t)desc->width * (size_t)desc->height * 4u);
    if (entry.mipmapped) wz3__tex_command(out, WZ3_TEXCMD_GENERATE_MIPMAP);
  }
  wz3__tex_parameter(out, WZ3_GL_TEXTURE_MAX_LEVEL, entry.mip_levels - 1 > 0 ? entry.mip_levels - 1 : 0);
  wz3__tex_parameter(out, WZ3_GL_TEXTURE_MIN_FILTER, entry.mipmapped ? WZ3_GL_LINEAR_MIPMAP_LINEAR : WZ3_GL_LINEAR);
  wz3__tex_parameter(out, WZ3_GL_TEXTURE_MAG_FILTER, WZ3_GL_LINEAR);
  wz3__tex_parameter(out, WZ3_GL_TEXTURE_WRAP_S, WZ3_GL_REPEAT);
  wz3__tex_parameter(out, WZ3_GL_TEXTURE_WRAP_T, WZ3_GL_REPEAT);
  return WZ3_OK;
}

static void wz3__tex_stats_add(wz3_texture_stats *s, const wz3_texture_entry *e)
{
  s->estimated_bytes += e->byte_length;
  if (e->storage == WZ3_STORAGE_DXT5) s->dxt5_compressed++;
  else if (e->storage == WZ3_STORAGE_DXT5_RGBA8) s->dxt5_fallback++;
}

static void wz3__tex_stats_sub(wz3_texture_stats *s, const wz3_texture_entry *e)
{
  s->estimated_bytes -= e->byte_length;
  if (e->storage == WZ3_STORAGE_DXT5) s->dxt5_compressed--;
  else if (e->storage == WZ3_STORAGE_DXT5_RGBA8) s->dxt5_fallback--;
}

WZ3_API void wz3_texture_stats_report(const wz3_texture_stats *stats, wz3_texture_report *out)
{
  if (!stats || !out) return;
  out->textures = stats->entries + (stats->fallback ? 1u : 0u);
  out->bitmap_textures = stats->entries;
  out->dxt5_compressed = stats->dxt5_compressed;
  out->dxt5_fallback = stats->dxt5_fallback;
  out->estimated_bytes = stats->estimated_bytes + (stats->fallback ? 4u : 0u);
}

WZ3_API wz3_result wz3_texture_get(wz3_texture_action *action, int32_t *delete_old, wz3_texture_plan *plan, wz3_texture_entry *entry,
                                   wz3_texture_stats *stats, const wz3_texture_policy *policy, const wz3_texture_desc *desc)
{
  if (!action || !delete_old || !plan || !entry || !stats || !policy || !desc) return WZ3_ERR_ARGUMENT;
  *delete_old = 0;
  if (!wz3__tex_desc_valid(desc)) {
    if (entry->present) {
      wz3__tex_stats_sub(stats, entry);
      stats->entries--;
      memset(entry, 0, sizeof *entry);
      *delete_old = 1;
    }
    *action = WZ3_TEXTURE_FALLBACK;
    return WZ3_OK;
  }
  wz3_texture_entry candidate;
  wz3_result r = wz3__tex_describe(&candidate, policy, desc, 1);
  if (r != WZ3_OK) return r;
  if (wz3__tex_entry_matches(entry, &candidate)) {
    *action = WZ3_TEXTURE_HIT;
    return WZ3_OK;
  }
  r = wz3_texture_plan_make(plan, policy, desc);
  if (r != WZ3_OK) return r;
  if (entry->present) {
    wz3__tex_stats_sub(stats, entry);
    *delete_old = 1;
  } else {
    stats->entries++;
  }
  *entry = plan->entry;
  wz3__tex_stats_add(stats, entry);
  *action = WZ3_TEXTURE_UPLOAD;
  return WZ3_OK;
}

WZ3_API uint64_t wz3_texture_estimate_bytes(const wz3_texture_policy *policy, const wz3_texture_desc *desc, const wz3_texture_entry *entry,
                                            const wz3_texture_stats *stats)
{
  if (!policy || !desc || !stats) return 0;
  if (!wz3__tex_desc_valid(desc)) return stats->fallback ? 0 : 4;
  wz3_texture_entry candidate;
  wz3__tex_describe(&candidate, policy, desc, 0);
  if (entry && wz3__tex_entry_matches(entry, &candidate)) return 0;
  return candidate.byte_length;
}

WZ3_API void wz3_texture_fallback_texel(uint8_t out[4])
{
  out[0] = out[1] = out[2] = out[3] = 255;
}

WZ3_API int wz3_texture_fallback(wz3_texture_stats *stats, wz3_texture_plan *plan)
{
  if (!stats || stats->fallback) return 0;
  stats->fallback = 1;
  if (plan) {
    memset(plan, 0, sizeof *plan);
    wz3__tex_image(plan, WZ3_TEXCMD_IMAGE, 0, 1, 1, WZ3_GL_RGBA8, WZ3_GL_UNSIGNED_BYTE, 4);
    wz3__tex_parameter(plan, WZ3_GL_TEXTURE_MIN_FILTER, WZ3_GL_LINEAR);
    wz3__tex_parameter(plan, WZ3_GL_TEXTURE_MAG_FILTER, WZ3_GL_LINEAR);
  }
  return 1;
}

/* ---- sampler state ---- */

static const uint32_t wz3__sampler_pnames[WZ3_SAMPLER_PARAMS] = {
  WZ3_GL_TEXTURE_MAX_LEVEL, WZ3_GL_TEXTURE_MIN_FILTER, WZ3_GL_TEXTURE_MAG_FILTER, WZ3_GL_TEXTURE_WRAP_S, WZ3_GL_TEXTURE_WRAP_T,
  WZ3_GL_TEXTURE_MAX_ANISOTROPY
};

/* setSamplerParameter: a call only when the texture does not already have the value. */
static void wz3__sampler_set(wz3_sampler_state *s, int index, double value, wz3_sampler_call *out, uint32_t *n)
{
  if (s->known[index] && s->value[index] == value) return;
  if (out) {
    out[*n].pname = wz3__sampler_pnames[index];
    out[*n].value = value;
    out[*n].is_float = index == WZ3_SAMPLER_ANISOTROPY;
    (*n)++;
  }
  s->known[index] = 1;
  s->value[index] = value;
}

WZ3_API void wz3_sampler_caps_make(wz3_sampler_caps *out, int available, double reported_max)
{
  if (!out) return;
  out->anisotropy = available ? 1 : 0;
  double v = reported_max;
  if (v != v || v == 0.0) v = 1.0;   /* Number(x) || 1 */
  out->max_anisotropy = available ? (v < 1.0 ? 1.0 : v) : 1.0;
}

WZ3_API void wz3_sampler_state_after_plan(wz3_sampler_state *out, const wz3_texture_plan *plan)
{
  if (!out) return;
  memset(out, 0, sizeof *out);
  if (!plan) return;
  for (uint32_t i = 0; i < plan->n_commands; i++) {
    const wz3_texture_command *c = &plan->commands[i];
    if (c->kind != WZ3_TEXCMD_PARAMETER) continue;
    for (int k = 0; k < WZ3_SAMPLER_PARAMS; k++)
      if (wz3__sampler_pnames[k] == c->pname) wz3__sampler_set(out, k, (double)c->value, NULL, NULL);
  }
}

WZ3_API void wz3_sampler_state_fallback(wz3_sampler_state *out)
{
  if (!out) return;
  memset(out, 0, sizeof *out);
  wz3__sampler_set(out, WZ3_SAMPLER_MIN_FILTER, (double)WZ3_GL_LINEAR, NULL, NULL);
  wz3__sampler_set(out, WZ3_SAMPLER_MAG_FILTER, (double)WZ3_GL_LINEAR, NULL, NULL);
}

WZ3_API uint32_t wz3_sampler_bind(wz3_sampler_state *state, const wz3_sampler_caps *caps, int mipmapped, const wz3_sampler_request *request,
                                  wz3_sampler_call out[WZ3_SAMPLER_MAX_CALLS])
{
  uint32_t n = 0;
  if (!state || !caps || !request || !out) return 0;
  if (request->has_clamp) {
    const double wrap = request->clamp ? (double)WZ3_GL_CLAMP_TO_EDGE : (double)WZ3_GL_REPEAT;
    wz3__sampler_set(state, WZ3_SAMPLER_WRAP_S, wrap, out, &n);
    wz3__sampler_set(state, WZ3_SAMPLER_WRAP_T, wrap, out, &n);
  }
  if (request->has_filter_mode) {
    const int32_t mode = request->filter_mode & 7;
    const int m11 = request->material11 != 0;
    /* Material11 and Material20 use different native D3D filter tables: mode 2 is trilinear in M11 but point-mip in M20 */
    const int point = m11 ? (mode == 0 || mode == 4 || mode >= 6) : (mode == 0 || mode >= 5);
    uint32_t min_filter;
    if (point) min_filter = (m11 && mode == 4 && mipmapped) ? WZ3_GL_NEAREST_MIPMAP_NEAREST : WZ3_GL_NEAREST;
    else if (!mipmapped || mode == 1) min_filter = WZ3_GL_LINEAR;
    else if ((m11 && mode == 5) || (!m11 && mode == 2)) min_filter = WZ3_GL_LINEAR_MIPMAP_NEAREST;
    else min_filter = WZ3_GL_LINEAR_MIPMAP_LINEAR;
    wz3__sampler_set(state, WZ3_SAMPLER_MAG_FILTER, point ? (double)WZ3_GL_NEAREST : (double)WZ3_GL_LINEAR, out, &n);
    wz3__sampler_set(state, WZ3_SAMPLER_MIN_FILTER, (double)min_filter, out, &n);
    if (caps->anisotropy) {
      const int anisotropic = m11 ? mode == 3 : mode == 4;
      double level = 1.0;
      if (anisotropic) level = caps->max_anisotropy < 4.0 ? caps->max_anisotropy : 4.0;
      wz3__sampler_set(state, WZ3_SAMPLER_ANISOTROPY, level, out, &n);
    }
  } else if (request->has_nearest) {
    wz3__sampler_set(state, WZ3_SAMPLER_MAG_FILTER, request->nearest ? (double)WZ3_GL_NEAREST : (double)WZ3_GL_LINEAR, out, &n);
  }
  return n;
}

/* ---- the sampler slots of a material pass (engine.cpp) ---- */

static uint32_t wz3__ss_flag(const uint32_t *flags, int32_t index) { return index >= 0 && index < 4 ? flags[index] : 0u; }
/* an entry the view does not have reads as NaN, which the setup turns into scale 1 */
static double wz3__ss_scale(const double *scales, uint32_t present, int32_t index)
{
  return index >= 0 && index < 4 && ((present >> index) & 1u) ? scales[index] : (double)NAN;
}

WZ3_API void wz3_material_sampler_setup(const wz3_material_view *view, wz3_sampler_setup *out)
{
  if (!view || !out) return;
  memset(out, 0, sizeof *out);
  const int material20 = view->system == 20, material11 = view->system == 11;
  for (int i = 0; i < 4; i++) {
    const int32_t source = view->texture_map[i];
    const int none = source == -1;
    out->texture_op[i] = (source >= 0 && source < 7 && view->textures) ? view->textures[source] : -1;
    if (!material20 || none) {
      out->m20_flags[i] = 0;
      out->m20_scales[i] = 1.0f;
    } else {
      const int lighting = source >= 4;
      const int32_t slot = lighting ? source - 4 : source;
      const uint32_t flags = lighting ? wz3__ss_flag(view->light_flags4, slot) : wz3__ss_flag(view->texture_flags, slot);
      const double scale = lighting ? wz3__ss_scale(view->light_scales, view->light_scale_present, slot)
                                    : wz3__ss_scale(view->texture_scales, view->texture_scale_present, slot);
      out->m20_flags[i] = (int32_t)flags;
      out->m20_scales[i] = isfinite(scale) ? (float)scale : 1.0f;
    }
    out->m11_flags[i] = material11 ? (int32_t)view->slot_texture_flags[i] : 0;
    out->m11_scales[i] = (material11 && ((view->slot_scale_present >> i) & 1u) && isfinite(view->slot_texture_scales[i])) ? (float)view->slot_texture_scales[i] : 1.0f;
    out->bind_flags[i] = none ? 1u : source < 4 ? wz3__ss_flag(view->texture_flags, source) : wz3__ss_flag(view->extra_texture_flags, source - 4);
    if (out->texture_op[i] >= 0) out->texture_mask |= 1 << i;
  }
}

WZ3_API void wz3_sampler_setup_request(const wz3_material_view *view, const wz3_sampler_setup *setup, int slot, wz3_sampler_request *out)
{
  if (!view || !setup || !out) return;
  memset(out, 0, sizeof *out);
  if (slot < 0 || slot > 3) return;
  out->has_clamp = 1;
  out->clamp = (setup->bind_flags[slot] & 0x100u) != 0;
  out->has_filter_mode = 1;
  out->filter_mode = (int32_t)(setup->bind_flags[slot] & 7u);
  out->material11 = view->system == 11;
}

/* ---- texture quality ---- */

/* Length of the JavaScript WhiteSpace / LineTerminator character at `p` (UTF-8), 0 if it is none. */
static size_t wz3__js_space(const unsigned char *p)
{
  if (p[0] == 9 || p[0] == 10 || p[0] == 11 || p[0] == 12 || p[0] == 13 || p[0] == 32) return 1;
  if (p[0] == 0xc2 && p[1] == 0xa0) return 2;                                           /* U+00A0 */
  if (p[0] == 0xe1 && p[1] == 0x9a && p[2] == 0x80) return 3;                           /* U+1680 */
  if (p[0] == 0xe2 && p[1] == 0x80 && ((p[2] >= 0x80 && p[2] <= 0x8a) || p[2] == 0xa8 || p[2] == 0xa9 || p[2] == 0xaf)) return 3;
  if (p[0] == 0xe2 && p[1] == 0x81 && p[2] == 0x9f) return 3;                           /* U+205F */
  if (p[0] == 0xe3 && p[1] == 0x80 && p[2] == 0x80) return 3;                           /* U+3000 */
  if (p[0] == 0xef && p[1] == 0xbb && p[2] == 0xbf) return 3;                           /* U+FEFF */
  return 0;
}

static int wz3__js_digit(int c, int radix)
{
  int v = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'z' ? c - 'a' + 10 : c >= 'A' && c <= 'Z' ? c - 'A' + 10 : 99;
  return v < radix ? v : -1;
}

/* Number(text) when it is a finite number (1, value set); NaN and +-Infinity (0) are not told apart. */
static int wz3__js_finite_number(const char *text, double *value)
{
  const unsigned char *p = (const unsigned char *)text;
  while (wz3__js_space(p)) p += wz3__js_space(p);
  const unsigned char *start = p, *last_end = p;
  while (*p) {
    const size_t space = wz3__js_space(p);
    if (space) { p += space; continue; }
    p++;                       /* a byte of a non-space character; continuation bytes follow and are consumed one by one */
    last_end = p;
  }
  if (last_end == start) { *value = 0.0; return 1; }
  const size_t length = (size_t)(last_end - start);
  if (length > 2 && start[0] == '0' && ((start[1] | 0x20) == 'x' || (start[1] | 0x20) == 'o' || (start[1] | 0x20) == 'b')) {
    const int radix = (start[1] | 0x20) == 'x' ? 16 : (start[1] | 0x20) == 'o' ? 8 : 2;
    double v = 0.0;
    for (size_t i = 2; i < length; i++) {
      const int d = wz3__js_digit(start[i], radix);
      if (d < 0) return 0;
      v = v * (double)radix + (double)d;
    }
    if (!isfinite(v)) return 0;
    *value = v;
    return 1;
  }
  size_t i = 0;
  if (start[i] == '+' || start[i] == '-') i++;
  size_t digits = 0, fraction = 0;
  while (i < length && start[i] >= '0' && start[i] <= '9') { i++; digits++; }
  if (i < length && start[i] == '.') {
    i++;
    while (i < length && start[i] >= '0' && start[i] <= '9') { i++; fraction++; }
  }
  if (digits + fraction == 0) return 0;      /* also "Infinity", "NaN", "inf" */
  if (i < length && (start[i] == 'e' || start[i] == 'E')) {
    i++;
    if (i < length && (start[i] == '+' || start[i] == '-')) i++;
    size_t exponent = 0;
    while (i < length && start[i] >= '0' && start[i] <= '9') { i++; exponent++; }
    if (!exponent) return 0;
  }
  if (i != length) return 0;
  char *end;
  const double v = strtod((const char *)start, &end);
  if ((size_t)((const unsigned char *)end - start) != length || !isfinite(v)) return 0;
  *value = v;
  return 1;
}

WZ3_API int32_t wz3_texture_quality_offset(const char *quality, int *recognised)
{
  if (recognised) *recognised = 0;
  if (!quality) return 0;
  double number;
  if (wz3__js_finite_number(quality, &number)) {
    if (recognised) *recognised = 1;
    const int32_t v = wz3__rt_to_int32(number);
    return v > 0 ? 0 : v < -3 ? -3 : v;
  }
  char lower[16];
  size_t n = 0;
  for (; quality[n]; n++) {
    if (n + 1 >= sizeof lower) return 0;     /* longer than any name */
    const char c = quality[n];
    lower[n] = (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
  }
  lower[n] = 0;
  static const struct { const char *name; int32_t offset; } names[] = {
    {"high", 0}, {"ultra", 0}, {"full", 0}, {"normal", -1}, {"reduced", -1}, {"medium", -1}, {"low", -2}
  };
  for (size_t k = 0; k < sizeof names / sizeof names[0]; k++)
    if (!strcmp(lower, names[k].name)) {
      if (recognised) *recognised = 1;
      return names[k].offset;
    }
  return 0;
}
