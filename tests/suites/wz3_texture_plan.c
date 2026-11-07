/* Texture upload plan, sampler state and texture quality check for wz3.h against src/gl.js and src/app.js.
 *
 *   test_wz3_texture_plan <assets dir> <fixtures dir> [--print]
 *
 * Replays tools/dump_texture_plan.mjs. The JavaScript side ran the real TextureCache on a recording WebGL mock; this test runs
 * the C plan (wz3_texture_get, wz3_sampler_bind, ...) over the same scenarios, turns every command into the same GL call
 * (the pixel bytes come from wz3_texture_prepare) and compares the hash of the whole call stream: texture creation and
 * deletion, uploads, parameters, binds, cache statistics. It also replays the texture of every production material and
 * the quality strings of textureSizeOffset.
 */
#define WZ3_IMPLEMENTATION
#include <debris/core/wz3.h>
#include "support/gate_common.h"

static uint32_t fnv_bytes(const uint8_t *p, size_t n)
{
  uint32_t h = 0x811c9dc5u;
  for (size_t i = 0; i < n; i++) h = (h ^ p[i]) * 0x01000193u;
  return h;
}

/* what the generated scenarios exercised */
static struct {
  unsigned hits, fallbacks, uploads, throws, generate, compressed, snorm, images, replaced;
  unsigned min_filters[4], aniso_calls, empty_binds, binds, estimates, scenarios;
  unsigned storage[3];
} g_seen;

typedef struct sim_bitmap {
  int32_t width, height, format, mip_count, mip_threshold;
  uint16_t *owned;       /* scenario pixels */
  const uint16_t *array; /* the array the bitmap currently has (owned, or the graph's) */
  size_t words;
  int has_array;
  uint64_t data_id;
  wz3_texture_entry entry;
  wz3_sampler_state sampler; /* of the texture the entry names */
  uint32_t texture;
} sim_bitmap;

typedef struct sim {
  hash h;
  wz3_texture_policy policy;
  wz3_sampler_caps caps;
  wz3_texture_stats stats;
  uint32_t next_texture, fallback_texture;
  wz3_sampler_state fallback_sampler;
  sim_bitmap bm[3];
  uint64_t next_data_id;
} sim;

static void sim_init(sim *s, int mode, int hw, int aniso, double max_aniso)
{
  memset(s, 0, sizeof *s);
  hash_init(&s->h);
  const wz3_result r = wz3_texture_policy_make(&s->policy, mode, hw);
  CHECK(r == WZ3_OK, "policy make %d", (int)r);
  wz3_sampler_caps_make(&s->caps, aniso, max_aniso);
  s->next_texture = 1;
  s->next_data_id = 1;
}

static void sim_free(sim *s)
{
  for (int i = 0; i < 3; i++) free(s->bm[i].owned);
}

static uint32_t ev_create(sim *s)
{
  hu(&s->h, 1);
  return s->next_texture++;
}
static void ev_bind(sim *s, uint32_t tex)
{
  hu(&s->h, 2);
  hu(&s->h, WZ3_GL_TEXTURE_2D);
  hu(&s->h, tex);
}

/* one command of a plan as the GL call it stands for */
static void ev_command(sim *s, const wz3_texture_command *c, const wz3_texture_image *img, const uint8_t *fixed_bytes)
{
  hash *h = &s->h;
  switch (c->kind) {
  case WZ3_TEXCMD_PIXEL_STORE:
    hu(h, 3);
    hu(h, c->pname);
    hi(h, c->value);
    break;
  case WZ3_TEXCMD_IMAGE:
  case WZ3_TEXCMD_COMPRESSED: {
    const uint8_t *bytes = fixed_bytes;
    if (!bytes) {
      CHECK(img && c->level >= 0 && (uint32_t)c->level < img->n_levels, "command level %d outside the prepared image", c->level);
      if (!img || c->level < 0 || (uint32_t)c->level >= img->n_levels) return;
      const wz3_texture_level *l = &img->levels[c->level];
      CHECK(l->size == c->size && l->width == c->width && l->height == c->height, "level %d: plan %dx%d %zu bytes, image %dx%d %zu bytes", c->level, c->width,
            c->height, c->size, l->width, l->height, l->size);
      CHECK((c->kind == WZ3_TEXCMD_COMPRESSED) == (img->kind == WZ3_TEXTURE_BC3), "command kind against image kind");
      bytes = img->data + l->offset;
    }
    if (c->kind == WZ3_TEXCMD_IMAGE) {
      hu(h, 4);
      hu(h, WZ3_GL_TEXTURE_2D);
      hi(h, c->level);
      hu(h, c->internal_format);
      hi(h, c->width);
      hi(h, c->height);
      hu(h, 0);
      hu(h, c->format);
      hu(h, c->type);
      hu(h, (uint32_t)c->size);
      hu(h, fnv_bytes(bytes, c->size));
      g_seen.images++;
      if (c->internal_format == WZ3_GL_RGBA8_SNORM) g_seen.snorm++;
    } else {
      hu(h, 5);
      hu(h, WZ3_GL_TEXTURE_2D);
      hi(h, c->level);
      hu(h, c->internal_format);
      hi(h, c->width);
      hi(h, c->height);
      hu(h, 0);
      hu(h, (uint32_t)c->size);
      hu(h, fnv_bytes(bytes, c->size));
      g_seen.compressed++;
    }
    break;
  }
  case WZ3_TEXCMD_GENERATE_MIPMAP:
    hu(h, 6);
    hu(h, WZ3_GL_TEXTURE_2D);
    g_seen.generate++;
    break;
  case WZ3_TEXCMD_PARAMETER:
    hu(h, 7);
    hu(h, WZ3_GL_TEXTURE_2D);
    hu(h, c->pname);
    hi(h, c->value);
    break;
  default: CHECK(0, "unknown command kind %u", c->kind);
  }
}

static void sim_stats(sim *s)
{
  wz3_texture_report rep;
  wz3_texture_stats_report(&s->stats, &rep);
  hu(&s->h, rep.textures);
  hu(&s->h, rep.bitmap_textures);
  hu(&s->h, rep.dxt5_compressed);
  hu(&s->h, rep.dxt5_fallback);
  hu(&s->h, (uint32_t)rep.estimated_bytes);
}

/* TextureCache.fallbackTexture() */
static uint32_t sim_fallback(sim *s)
{
  wz3_texture_plan plan;
  if (!wz3_texture_fallback(&s->stats, &plan)) return s->fallback_texture;
  s->fallback_texture = ev_create(s);
  ev_bind(s, s->fallback_texture);
  uint8_t texel[4];
  wz3_texture_fallback_texel(texel);
  for (uint32_t i = 0; i < plan.n_commands; i++) ev_command(s, &plan.commands[i], NULL, texel);
  wz3_sampler_state_fallback(&s->fallback_sampler);
  return s->fallback_texture;
}

static wz3_texture_desc sim_desc(const sim_bitmap *b)
{
  wz3_texture_desc d;
  memset(&d, 0, sizeof d);
  d.width = b->width;
  d.height = b->height;
  d.format = b->format;
  d.mip_count = b->mip_count;
  d.mip_threshold = b->mip_threshold;
  d.has_data = b->has_array && b->width >= 1 && b->height >= 1 && b->words == (size_t)b->width * (size_t)b->height * 4u;
  d.data_id = b->data_id;
  return d;
}

/* TextureCache.get(bitmap): returns the texture, or 0 when it threw (sampler/mipmapped describe the bound texture) */
static uint32_t sim_get(sim *s, sim_bitmap *b, wz3_sampler_state **sampler, int *mipmapped)
{
  const wz3_texture_desc d = sim_desc(b);
  wz3_texture_plan plan;
  wz3_texture_action action;
  int32_t delete_old;
  const wz3_texture_entry before = b->entry;
  const wz3_result r = wz3_texture_get(&action, &delete_old, &plan, &b->entry, &s->stats, &s->policy, &d);
  if (r != WZ3_OK) {
    CHECK(r == WZ3_ERR_UNSUPPORTED, "get failed with %s", wz3_result_string(r));
    CHECK(memcmp(&before, &b->entry, sizeof before) == 0, "an error changed the entry");
    hu(&s->h, 0xbad);
    g_seen.throws++;
    return 0;
  }
  if (action == WZ3_TEXTURE_HIT) {
    g_seen.hits++;
    *sampler = &b->sampler;
    *mipmapped = b->entry.mipmapped;
    return b->texture;
  }
  if (action == WZ3_TEXTURE_FALLBACK) {
    g_seen.fallbacks++;
    if (delete_old) {
      hu(&s->h, 9);
      hu(&s->h, b->texture);
      b->texture = 0;
      memset(&b->sampler, 0, sizeof b->sampler);
    }
    *sampler = &s->fallback_sampler;
    *mipmapped = 0;
    const uint32_t t = sim_fallback(s);
    *sampler = &s->fallback_sampler;
    return t;
  }
  g_seen.uploads++;
  const uint32_t texture = ev_create(s);
  ev_bind(s, texture);
  wz3_bitmap view;
  memset(&view, 0, sizeof view);
  view.width = b->width;
  view.height = b->height;
  view.format = b->format;
  view.mip_count = b->mip_count;
  view.mip_threshold = b->mip_threshold;
  view.data = (uint16_t *)(uintptr_t)b->array;
  wz3_texture_image img;
  const wz3_result pr = wz3_texture_prepare(&img, &view, plan.allow_bc3);
  CHECK(pr == WZ3_OK, "prepare: %s", wz3_result_string(pr));
  if (pr != WZ3_OK) return 0;
  CHECK((img.kind == WZ3_TEXTURE_BC3) == (plan.entry.storage == WZ3_STORAGE_DXT5), "prepared kind against the storage");
  CHECK(img.n_levels >= 1, "no levels");
  for (uint32_t i = 0; i < plan.n_commands; i++) ev_command(s, &plan.commands[i], &img, NULL);
  wz3_texture_release(&img);
  g_seen.storage[plan.entry.storage]++;
  if (delete_old) {
    hu(&s->h, 9);
    hu(&s->h, b->texture);
    g_seen.replaced++;
  }
  b->texture = texture;
  wz3_sampler_state_after_plan(&b->sampler, &plan);
  *sampler = &b->sampler;
  *mipmapped = b->entry.mipmapped;
  return texture;
}

static void ev_sampler_calls(sim *s, const wz3_sampler_call *calls, uint32_t n)
{
  for (uint32_t i = 0; i < n; i++) {
    if (calls[i].is_float) {
      hu(&s->h, 8);
      hu(&s->h, WZ3_GL_TEXTURE_2D);
      hu(&s->h, calls[i].pname);
      hd(&s->h, calls[i].value);
      g_seen.aniso_calls++;
    } else {
      hu(&s->h, 7);
      hu(&s->h, WZ3_GL_TEXTURE_2D);
      hu(&s->h, calls[i].pname);
      hu(&s->h, (uint32_t)calls[i].value);
      if (calls[i].pname == WZ3_GL_TEXTURE_MIN_FILTER) {
        const uint32_t v = (uint32_t)calls[i].value;
        if (v == WZ3_GL_NEAREST_MIPMAP_NEAREST)
          g_seen.min_filters[0]++;
        else if (v == WZ3_GL_LINEAR_MIPMAP_NEAREST)
          g_seen.min_filters[1]++;
        else if (v == WZ3_GL_LINEAR_MIPMAP_LINEAR)
          g_seen.min_filters[2]++;
        else
          g_seen.min_filters[3]++;
      }
    }
  }
}

static void sim_set_bitmap(sim *s, int index, uint32_t seed, int32_t w, int32_t h, int32_t format, int32_t count, int32_t threshold, uint32_t mode)
{
  sim_bitmap *b = &s->bm[index];
  b->width = w;
  b->height = h;
  b->format = format;
  b->mip_count = count;
  b->mip_threshold = threshold;
  if (mode == 1) return; /* keep the old array (and its identity) */
  free(b->owned);
  b->owned = NULL;
  b->has_array = mode != 4;
  b->words = 0;
  b->data_id = s->next_data_id++;
  if (mode == 0 || mode == 3) {
    const size_t n = (size_t)w * (size_t)h * 4u - (mode == 3 ? 1u : 0u);
    b->owned = (uint16_t *)calloc(n ? n : 1, sizeof(uint16_t));
    b->words = n;
    uint32_t st = seed;
    if (mode == 0)
      for (size_t i = 0; i < n; i++) {
        st = st * 1664525u + 1013904223u;
        const uint32_t v = st >> 17;
        b->owned[i] = (uint16_t)((st & 0xe0u) == 0xe0u ? ((v | 0x8000u) & 0xffffu) : (v & 0x7fffu));
      }
  } else if (mode == 2) {
    b->owned = (uint16_t *)calloc(1, sizeof(uint16_t));
  }
  b->array = b->owned;
}

static uint32_t run_scenario(reader *r)
{
  const int mode = ri32(r), hw = ri32(r), aniso = ri32(r);
  const double max_aniso = rf64(r);
  const int nb = ri32(r);
  const uint32_t n_steps = ru32(r);
  static sim s;
  sim_init(&s, mode, hw, aniso, max_aniso);
  CHECK(nb >= 1 && nb <= 3, "bitmap count %d", nb);
  for (uint32_t k = 0; k < n_steps; k++) {
    const uint32_t op = ru32(r);
    hu(&s.h, 0x500u + op);
    switch (op) {
    case 0: {
      const int index = ri32(r);
      const uint32_t seed = ru32(r);
      const int32_t w = ri32(r), h = ri32(r), format = ri32(r), count = ri32(r), threshold = ri32(r);
      const uint32_t dmode = ru32(r);
      if (index >= 0 && index < 3) sim_set_bitmap(&s, index, seed, w, h, format, count, threshold, dmode);
      break;
    }
    case 1: {
      const int index = ri32(r);
      wz3_sampler_state *sampler;
      int mipmapped;
      if (sim_get(&s, &s.bm[index], &sampler, &mipmapped)) sim_stats(&s);
      break;
    }
    case 2: {
      const int index = ri32(r);
      const wz3_texture_desc d = sim_desc(&s.bm[index]);
      hu(&s.h, (uint32_t)wz3_texture_estimate_bytes(&s.policy, &d, &s.bm[index].entry, &s.stats));
      g_seen.estimates++;
      break;
    }
    case 3:
      sim_fallback(&s);
      sim_stats(&s);
      break;
    case 4: sim_stats(&s); break;
    case 5: {
      const int index = ri32(r);
      const uint32_t unit = ru32(r);
      wz3_sampler_request req;
      memset(&req, 0, sizeof req);
      req.has_clamp = ri32(r);
      req.clamp = ri32(r);
      req.has_filter_mode = ri32(r);
      req.filter_mode = ri32(r);
      req.material11 = ri32(r);
      req.has_nearest = ri32(r);
      req.nearest = ri32(r);
      hu(&s.h, 10);
      hu(&s.h, unit);
      wz3_sampler_state *sampler = NULL;
      int mipmapped = 0;
      uint32_t texture;
      if (index < 0) {
        texture = sim_fallback(&s);
        sampler = &s.fallback_sampler;
      } else {
        texture = sim_get(&s, &s.bm[index], &sampler, &mipmapped);
        if (!texture) break;
      }
      ev_bind(&s, texture);
      wz3_sampler_call calls[WZ3_SAMPLER_MAX_CALLS];
      const uint32_t n = wz3_sampler_bind(sampler, &s.caps, mipmapped, &req, calls);
      CHECK(n <= WZ3_SAMPLER_MAX_CALLS, "too many sampler calls");
      ev_sampler_calls(&s, calls, n);
      sim_stats(&s);
      g_seen.binds++;
      if (n == 0) g_seen.empty_binds++;
      break;
    }
    default: CHECK(0, "unknown opcode %u", op);
    }
  }
  const uint32_t result = s.h.h;
  sim_free(&s);
  g_seen.scenarios++;
  return result;
}

/* the texture of one production material, on a fresh cache like the dumper's */
static void production_texture(const wz3_bitmap *bitmap, int hw, uint32_t *storage, uint32_t *estimate, uint32_t *levels, uint32_t *hash_out)
{
  static sim s;
  sim_init(&s, WZ3_DXT5_AUTO, hw, 0, 1.0);
  sim_bitmap *b = &s.bm[0];
  b->width = bitmap->width;
  b->height = bitmap->height;
  b->format = bitmap->format;
  b->mip_count = bitmap->mip_count;
  b->mip_threshold = bitmap->mip_threshold;
  b->array = bitmap->data;
  b->has_array = 1;
  b->words = (size_t)bitmap->width * (size_t)bitmap->height * 4u;
  b->data_id = 1;
  const wz3_texture_desc d = sim_desc(b);
  *estimate = (uint32_t)wz3_texture_estimate_bytes(&s.policy, &d, &b->entry, &s.stats);
  wz3_sampler_state *sampler;
  int mipmapped;
  sim_get(&s, b, &sampler, &mipmapped);
  sim_stats(&s);
  *storage = (uint32_t)b->entry.storage;
  *levels = (uint32_t)b->entry.mip_levels;
  *hash_out = s.h.h;
}

static unsigned replay_production(const wz3_kx *kx, const char *fixture_dir, const char *file, int32_t offset)
{
  char *text = (char *)gate_read_all(fixture_dir, file, NULL);
  wz3_graph_options options;
  memset(&options, 0, sizeof options);
  wz3_bitmap_ctx_init(&options.ctx);
  options.ctx.texture_offset = offset;
  options.release_consumed = 1;
  wz3_result res;
  wz3_graph *g = wz3_graph_create(kx, &options, &res);
  CHECK(g && res == WZ3_OK, "graph create");
  unsigned count = 0, bc3 = 0, snorm = 0, rgba = 0, fallback_differs = 0;
  uint32_t *consumers = (uint32_t *)calloc(kx->n_ops, sizeof(uint32_t));
  for (uint32_t o = 0; g && o < kx->n_ops; o++) {
    const wz3_class_info *info = wz3_class_lookup(kx->ops[o].class_id);
    if (!info || info->output_class != WZ3_KC_BITMAP || kx->ops[o].class_id == 0x3f) continue;
    for (uint32_t k = 0; k < kx->ops[o].n_inputs; k++) consumers[kx->ops[o].inputs[k]]++;
  }
  for (char *line = text; g && *line;) {
    char *nl = strchr(line, '\n');
    if (nl) *nl = 0;
    if (line[0] == 'P' && line[1] == ' ') {
      reader r;
      r.p = line + 2;
      char *end;
      const uint32_t op = (uint32_t)strtoul(r.p, &end, 10);
      const int fmt = (int)strtol(end, &end, 10), w = (int)strtol(end, &end, 10), h = (int)strtol(end, &end, 10);
      const uint32_t levels = (uint32_t)strtoul(end, &end, 10);
      r.p = end;
      uint32_t want[2][3];
      for (int k = 0; k < 2; k++) {
        want[k][0] = ru32(&r);
        want[k][1] = ru32(&r);
        want[k][2] = ru32(&r);
      }
      const wz3_bitmap *bm = NULL;
      const wz3_result er = wz3_graph_eval_bitmap(g, op, &bm);
      CHECK(er == WZ3_OK && bm, "op %u: %s", op, wz3_result_string(er));
      if (er == WZ3_OK && bm) {
        CHECK(bm->format == fmt && bm->width == w && bm->height == h, "op %u: %dx%d format %d, JavaScript %dx%d format %d", op, bm->width, bm->height,
              bm->format, w, h, fmt);
        for (int k = 0; k < 2; k++) {
          uint32_t storage, estimate, lv, hash_value;
          production_texture(bm, k == 0, &storage, &estimate, &lv, &hash_value);
          CHECK(storage == want[k][0] && estimate == want[k][1] && lv == levels, "op %u s3tc %d: storage %u estimate %u levels %u, JavaScript %u %u %u", op,
                k == 0, storage, estimate, lv, want[k][0], want[k][1], levels);
          CHECK(hash_value == want[k][2], "op %u s3tc %d: GL call stream hash %x, JavaScript %x", op, k == 0, hash_value, want[k][2]);
          if (k == 0) {
            if (storage == WZ3_STORAGE_DXT5)
              bc3++;
            else if (fmt == 5)
              snorm++;
            else
              rgba++;
          }
        }
        if (want[0][2] != want[1][2]) fallback_differs++;
        count++;
      }
      if (!consumers[op]) wz3_graph_release(g, op);
      for (uint32_t k = 0; k < kx->ops[op].n_inputs; k++) {
        const uint32_t in = kx->ops[op].inputs[k];
        if (in < kx->n_ops && consumers[in] && --consumers[in] == 0) wz3_graph_release(g, in);
      }
    }
    if (!nl) break;
    line = nl + 1;
  }
  /* the JavaScript port cannot evaluate the 20 textures that depend on a Text operator (no host font): 233 of the 253 */
  CHECK(count == 233 && bc3 == 2 && snorm == 50 && rgba == 181, "%s: %u production textures (BC3 %u, signed %u, RGBA8 %u), expected 233 (2, 50, 181)", file,
        count, bc3, snorm, rgba);
  CHECK(fallback_differs == bc3, "%s: only BC3 textures may differ between the S3TC and the RGBA8 path (%u differ, %u BC3)", file, fallback_differs, bc3);
  printf("texture plan: %s: %u production textures (%u BC3, %u signed, %u RGBA8)\n", file, count, bc3, snorm, rgba);
  free(consumers);
  wz3_graph_destroy(g);
  free(text);
  return count;
}

static int hexval(char c) { return c >= '0' && c <= '9' ? c - '0' : c - 'a' + 10; }

int suite_wz3_texture_plan(int argc, char **argv)
{
  if (argc < 3) {
    printf("usage: test_wz3_texture_plan <assets dir> <fixtures dir> [--print]\n");
    return 2;
  }
  const int print = argc > 3 && !strcmp(argv[3], "--print");
  char *fixture = (char *)gate_read_all(argv[2], "texture_plan.txt", NULL);
  unsigned compared = 0, policies = 0, qualities = 0, mismatches = 0;
  size_t line_number = 0;
  for (char *line = fixture; *line;) {
    char *nl = strchr(line, '\n');
    if (nl) *nl = 0;
    line_number++;
    char kind[8];
    reader r;
    const char *eq = strstr(line, " = ");
    if (sscanf(line, "%7s", kind) != 1 || !eq) {
      CHECK(0, "line %zu is malformed", line_number);
      break;
    }
    r.p = line + strlen(kind);
    uint32_t got = 0, expected;
    char *expected_end;
    expected = (uint32_t)strtoul(eq + 3, &expected_end, 16);
    unsigned long second = strtoul(expected_end, NULL, 16);
    if (!strcmp(kind, "T")) {
      got = run_scenario(&r);
    } else if (!strcmp(kind, "pol")) {
      wz3_texture_policy p;
      const int mode = ri32(&r), hw = ri32(&r);
      const wz3_result res = wz3_texture_policy_make(&p, mode == 3 ? 99 : mode, hw);
      got = res == WZ3_ERR_ARGUMENT ? 3u : res == WZ3_ERR_UNSUPPORTED ? 4u : res == WZ3_OK ? (p.s3tc ? 1u : 0u) : 9u;
      policies++;
    } else if (!strcmp(kind, "qual")) {
      char text[512];
      size_t n = 0;
      const char *p = r.p;
      while (p < eq && *p == ' ') p++;
      while (p + 1 < eq && p[0] != ' ' && n + 1 < sizeof text) {
        text[n++] = (char)(hexval(p[0]) * 16 + hexval(p[1]));
        p += 2;
      }
      text[n] = 0;
      int recognised;
      const int32_t offset = wz3_texture_quality_offset(text, &recognised);
      got = (uint32_t)offset;
      if (recognised != (int)second && !print) {
        mismatches++;
        CHECK(0, "line %zu: quality '%s' recognised %d, JavaScript %lu", line_number, text, recognised, second);
      }
      qualities++;
    } else {
      CHECK(0, "line %zu: unknown kind '%s'", line_number, kind);
      break;
    }
    compared++;
    if (print)
      printf("%s = %x\n", kind, got);
    else if (got != expected) {
      mismatches++;
      CHECK(0, "line %zu (%s) result %x, JavaScript %x", line_number, kind, got, expected);
    }
    if (!nl) break;
    line = nl + 1;
  }
  free(fixture);

  CHECK(compared >= 1400 && policies == 8 && qualities >= 400, "%u lines, %u policies, %u qualities", compared, policies, qualities);
  /* the scenarios must reach the branches they are meant to compare */
  CHECK(g_seen.scenarios >= 1000 && g_seen.hits >= 300 && g_seen.fallbacks >= 150 && g_seen.uploads >= 1400 && g_seen.throws >= 20 && g_seen.replaced >= 250,
        "scenarios %u hits %u fallbacks %u uploads %u throws %u replaced %u", g_seen.scenarios, g_seen.hits, g_seen.fallbacks, g_seen.uploads, g_seen.throws,
        g_seen.replaced);
  CHECK(g_seen.generate >= 25 && g_seen.compressed >= 200 && g_seen.snorm >= 200 && g_seen.images >= 2500, "generate %u compressed %u signed %u images %u",
        g_seen.generate, g_seen.compressed, g_seen.snorm, g_seen.images);
  CHECK(g_seen.storage[0] > 100 && g_seen.storage[1] > 50 && g_seen.storage[2] > 50, "storage %u %u %u", g_seen.storage[0], g_seen.storage[1],
        g_seen.storage[2]);
  CHECK(g_seen.min_filters[0] >= 20 && g_seen.min_filters[1] >= 20 && g_seen.min_filters[2] >= 10 && g_seen.min_filters[3] >= 100, "min filters %u %u %u %u",
        g_seen.min_filters[0], g_seen.min_filters[1], g_seen.min_filters[2], g_seen.min_filters[3]);
  CHECK(g_seen.aniso_calls >= 40 && g_seen.empty_binds >= 100 && g_seen.binds >= 1000 && g_seen.estimates >= 300,
        "aniso %u empty binds %u binds %u estimates %u", g_seen.aniso_calls, g_seen.empty_binds, g_seen.binds, g_seen.estimates);

  /* property: the public level count agrees with the one wz3_texture_prepare uses */
  for (int32_t w = 1; w <= 64; w++)
    for (int32_t h = 1; h <= 64; h += (w & 1) ? 7 : 1)
      for (int32_t c = -2; c <= 8; c++) {
        wz3_bitmap b;
        memset(&b, 0, sizeof b);
        b.width = w;
        b.height = h;
        b.mip_count = c;
        CHECK(wz3_texture_mip_levels(w, h, c) == wz3__tex_level_count(&b), "mip levels %dx%d count %d", w, h, c);
      }
  CHECK(wz3_texture_mip_levels(0, 4, 0) == 0 && wz3_texture_mip_levels(4, -1, 0) == 0, "empty bitmaps have no levels");

  /* the production textures */
  size_t kx_size;
  uint8_t *kx_bytes = gate_read_all(argv[1], "debris_party.kx", &kx_size);
  wz3_kx *kx = NULL;
  CHECK(wz3_kx_parse(&kx, kx_bytes, kx_size, NULL) == WZ3_OK, "parse");
  if (kx) {
    replay_production(kx, argv[2], "texture_plan_offset_m2.txt", -2);
#ifndef DEBRIS_TEST_SKIP_FULL_SIZE
    replay_production(kx, argv[2], "texture_plan_offset_0.txt", 0);
#endif
    wz3_kx_destroy(kx);
  }
  free(kx_bytes);
  printf("texture plan: %u lines compared (%u scenarios, %u uploads, %u binds), %u mismatches, %d checks, %d failures\n", compared, g_seen.scenarios,
         g_seen.uploads, g_seen.binds, mismatches, g_checks, g_failures);
  return g_failures ? 1 : 0;
}
