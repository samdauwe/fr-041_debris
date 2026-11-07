/* Compares wz3.h against the C++ source oracles in tools/oracles.
 *
 *   test_wz3_oracle <blur|cell|perlin|misc|lookup> <file with the oracle's stdout>
 *
 * The oracles are standalone transcriptions of the released werkkzeug3 code
 * (blur, Bitmap_Cell, Perlin, the misc MMX/x87 paths, the legacy lookups). Each
 * case below is recomputed through the library with the oracle's inputs.
 * The implementation is included so internal helpers can be checked too.
 */
#define WZ3_IMPLEMENTATION
#include <debris/core/wz3.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_failures, g_checks;

#define CHECK_EQ_U(label, got, want)                                                                  \
  do {                                                                                                \
    g_checks++;                                                                                       \
    if ((unsigned long)(got) != (unsigned long)(want)) {                                              \
      g_failures++;                                                                                   \
      printf("FAIL %s: got %lu, oracle %lu\n", (label), (unsigned long)(got), (unsigned long)(want)); \
    }                                                                                                 \
  } while (0)

static char *read_file(const char *path)
{
  FILE *f = fopen(path, "rb");
  if (!f) return NULL;
  fseek(f, 0, SEEK_END);
  long n = ftell(f);
  fseek(f, 0, SEEK_SET);
  char *text = (char *)malloc((size_t)n + 1);
  if (!text) {
    fclose(f);
    return NULL;
  }
  if (fread(text, 1, (size_t)n, f) != (size_t)n) {
    fclose(f);
    free(text);
    return NULL;
  }
  text[n] = 0;
  fclose(f);
  return text;
}

/* finds "<key><sep>" at the start of a line; returns the number after it */
static int find_value(const char *text, const char *key, char sep, int base, unsigned long *value)
{
  const size_t klen = strlen(key);
  const char *p = text;
  while (*p) {
    if (!strncmp(p, key, klen) && p[klen] == sep) {
      *value = strtoul(p + klen + 1, NULL, base);
      return 1;
    }
    const char *nl = strchr(p, '\n');
    if (!nl) break;
    p = nl + 1;
  }
  return 0;
}

static void compare_named(const char *text, const char *key, char sep, int base, uint32_t got)
{
  unsigned long want;
  g_checks++;
  if (!find_value(text, key, sep, base, &want)) {
    g_failures++;
    printf("FAIL %s: missing from oracle output\n", key);
    return;
  }
  if ((uint32_t)want != got) {
    g_failures++;
    printf("FAIL %s: got %lu, oracle %lu\n", key, (unsigned long)got, want);
  }
}

static uint32_t fnv_bytes(const uint8_t *p, size_t n)
{
  uint32_t hash = 0x811c9dc5u;
  for (size_t i = 0; i < n; i++) {
    hash ^= p[i];
    hash *= 0x01000193u;
  }
  return hash;
}

/* ---- blur (JSON) ----------------------------------------------------------------------------- */

static void blur_input(wz3_bitmap *b, int variant)
{
  static const uint16_t pattern[12] = { 0x0000, 0x0001, 0x7fff, 0x8000, 0xffff, 0x4000, 0xc000, 0x1234, 0xfedc, 0x7ffe, 0x8001, 0x5555 };
  const size_t n = (size_t)b->width * (size_t)b->height * 4;
  for (size_t i = 0; i < n; i++) {
    const uint32_t mixed = ((uint32_t)(i + 17) * 7919u) ^ ((uint32_t)i * 313u);
    if (variant == 0)
      b->data[i] = (uint16_t)(mixed & 0x7fffu);
    else if (variant == 1)
      b->data[i] = (uint16_t)mixed;
    else
      b->data[i] = pattern[(i * 5 + i / 4) % 12];
  }
}

static int json_words(const char *start, const char *key, uint16_t out[16])
{
  const char *p = strstr(start, key);
  if (!p) return 0;
  p += strlen(key);
  for (int i = 0; i < 16; i++) {
    char *end;
    out[i] = (uint16_t)strtoul(p, &end, 10);
    if (end == p) return 0;
    p = end + 1;
  }
  return 1;
}

static void check_blur(const char *text)
{
  static const struct {
    const char *name;
    int w, h, flags;
    float sx, sy, amp;
    int variant;
  } cases[] = {
    { "production_normal_order2", 16, 16, 0x02, 0.06298828125f, 0.06298828125f, 1.0f, 0 },
    { "production_sharpen_order2", 16, 16, 0x12, 0.0999755859375f, 0.0999755859375f, 1.0f, 0 },
    { "production_sharpen_odd_one_axis", 16, 8, 0x11, 0.024993896484375f, 0.0f, 1.0f, 0 },
    { "multiwrap_order3", 8, 8, 0x03, 1.75f, 0.125f, 0.875f, 0 },
    { "signed_saturating_sharpen", 8, 8, 0x12, 0.029998779296875f, 0.029998779296875f, 1.25f, 2 },
    { "round_even_and_fractional_amplitude", 8, 8, 0x01, 0.00244140625f, 0.00341796875f, 0.10000000149011612f, 1 },
  };
  for (size_t c = 0; c < sizeof cases / sizeof cases[0]; c++) {
    char key[128];
    snprintf(key, sizeof key, "\"name\":\"%s\"", cases[c].name);
    const char *rec = strstr(text, key);
    g_checks++;
    if (!rec) {
      g_failures++;
      printf("FAIL blur %s: missing from oracle output\n", cases[c].name);
      continue;
    }
    wz3_bitmap *in = wz3_bitmap_create(cases[c].w, cases[c].h);
    blur_input(in, cases[c].variant);
    wz3_bitmap *out = wz3_bitmap_blur(in, cases[c].flags, cases[c].sx, cases[c].sy, cases[c].amp);
    CHECK_EQ_U(cases[c].name, out ? 1 : 0, 1);
    if (out) {
      const char *hp = strstr(rec, "\"hash\":");
      CHECK_EQ_U(cases[c].name, wz3_bitmap_hash(out), hp ? strtoul(hp + 7, NULL, 10) : 0xdeadbeefu);
      uint16_t head[16], tail[16];
      const size_t n = (size_t)out->width * (size_t)out->height * 4;
      g_checks++;
      if (!json_words(rec, "\"head\":[", head) || !json_words(rec, "\"tail\":[", tail) || memcmp(head, out->data, sizeof head) ||
          memcmp(tail, out->data + n - 16, sizeof tail)) {
        g_failures++;
        printf("FAIL blur %s: head/tail words differ\n", cases[c].name);
      }
      /* the input must be untouched */
      wz3_bitmap *again = wz3_bitmap_create(cases[c].w, cases[c].h);
      blur_input(again, cases[c].variant);
      CHECK_EQ_U("blur input unchanged", memcmp(in->data, again->data, n * 2), 0);
      wz3_bitmap_destroy(again);
    }
    wz3_bitmap_destroy(out);
    wz3_bitmap_destroy(in);
  }
}

/* ---- cell ------------------------------------------------------------------------------------- */

static void check_cell(const char *text)
{
  static const struct {
    const char *name;
    uint32_t xs;
    uint32_t col0, col1, col2;
    int max, seed;
    float amp, gamma;
    int mode;
    float mindist;
    int percent;
    float aspect;
  } fx[] = {
    { "op452", 7, 4294967295u, 4278190080u, 4278190080u, 128, 1, 1.499755859375f, 0.5f, 1, 0.0f, 0, -2.49993896484375f },
    { "op475", 8, 4280624421u, 4278190080u, 4278190080u, 90, 5, 0.0f, 0.0f, 7, 0.125f, 0, 0.0f },
    { "op481", 9, 4280624421u, 4278453252u, 4278190080u, 255, 11, 0.0f, 0.0f, 7, 0.0625f, 0, 0.0f },
  };
  wz3_bitmap_ctx ctx;
  wz3_bitmap_ctx_init(&ctx);
  for (size_t i = 0; i < sizeof fx / sizeof fx[0]; i++) {
    wz3_bitmap *b = wz3_bitmap_cell(&ctx, fx[i].xs, fx[i].xs, fx[i].col0, fx[i].col1, fx[i].col2, fx[i].max, fx[i].seed, fx[i].amp, fx[i].gamma, fx[i].mode,
                                    fx[i].mindist, fx[i].percent, fx[i].aspect);
    CHECK_EQ_U(fx[i].name, b ? 1 : 0, 1);
    if (b) compare_named(text, fx[i].name, '=', 10, wz3_bitmap_hash(b));
    wz3_bitmap_destroy(b);
  }
}

/* ---- perlin ----------------------------------------------------------------------------------- */

static void check_perlin(const char *text)
{
  static const struct {
    const char *name;
    uint32_t xs, ys;
    int frequency, octaves;
    float fadeoff;
    int seed, mode;
    float amplitude, gamma;
    uint32_t c0, c1;
  } cases[] = {
    { "small0", 3, 3, 1, 3, 0.5f, 17, 0, 1.0f, 1.0f, 0xff000000u, 0xffffffffu },
    { "small1", 4, 3, 1, 4, 0.625f, 93, 1, 0.75f, 1.25f, 0xff102030u, 0xffe0c080u },
    { "small2", 4, 3, 2, 3, 0.75f, 7, 2, 1.125f, 0.8f, 0x80402010u, 0xffd0e0f0u },
    { "small3", 4, 3, 2, 3, 0.75f, 7, 3, 1.125f, 0.8f, 0x80402010u, 0xffd0e0f0u },
    { "production11", 9, 9, 2, 7, 0.999755859375f, 1, 0, 0.849609375f, 0.7998046875f, 0xff000000u, 0xffffffffu },
    { "production49", 9, 9, 4, 5, 0.639892578125f, 0, 0, 1.599609375f, 1.0f, 0xff202020u, 0xffffffffu },
  };
  wz3_bitmap_ctx ctx;
  wz3_bitmap_ctx_init(&ctx);
  for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
    wz3_bitmap *b = wz3_bitmap_perlin(&ctx, cases[i].xs, cases[i].ys, cases[i].frequency, cases[i].octaves, cases[i].fadeoff, cases[i].seed, cases[i].mode,
                                      cases[i].amplitude, cases[i].gamma, cases[i].c0, cases[i].c1);
    CHECK_EQ_U(cases[i].name, b ? 1 : 0, 1);
    if (b) compare_named(text, cases[i].name, ' ', 16, wz3_bitmap_hash(b));
    wz3_bitmap_destroy(b);
  }
}

/* ---- misc ------------------------------------------------------------------------------------- */

static wz3_bitmap *pattern_bitmap(int w, int h)
{
  wz3_bitmap *b = wz3_bitmap_create(w, h);
  const size_t n = (size_t)w * (size_t)h * 4;
  for (size_t i = 0; i < n; i++) b->data[i] = (uint16_t)((((uint32_t)i + 17u) * 7919u ^ ((uint32_t)i * 313u)) & 0x7fffu);
  return b;
}

static void check_misc(const char *text)
{
  wz3_bitmap_ctx ctx;
  wz3_bitmap_ctx_init(&ctx);
  unsigned long gray, alpha;
  g_checks++;
  {
    /* "gray=%u alpha=%u" on one line */
    const char *p = strstr(text, "gray=");
    if (!p || sscanf(p, "gray=%lu alpha=%lu", &gray, &alpha) != 2) {
      g_failures++;
      printf("FAIL gray: missing from oracle output\n");
    } else {
      CHECK_EQ_U("gray", wz3__gray_mmx(3, 0, 1), gray);
      CHECK_EQ_U("alpha", wz3__gray_mmx(7, 2, 5), alpha);
    }
  }

  wz3_bitmap *source = pattern_bitmap(16, 8);
  wz3_bitmap *r;

  r = wz3_bitmap_gradient(&ctx, 4, 3, 0xff102030u, 0xffe0c080u, 0.3125f, 0.173828125f, 0.73193359375f, 0);
  compare_named(text, "gradient", '=', 10, r ? wz3_bitmap_hash(r) : 0);
  wz3_bitmap_destroy(r);

  {
    const float p[9] = { -0.25f, 0.0f, 0.5f, 0.5498046875f, -0.125f, 0.25f, 0.875f, 0.125f, -0.0478515625f };
    r = wz3_bitmap_color_balance(source, p);
    compare_named(text, "color_balance", '=', 10, r ? wz3_bitmap_hash(r) : 0);
    wz3_bitmap_destroy(r);
  }

  {
    wz3_bitmap *h = wz3_bitmap_copy(source);
    h->data[0] = h->data[1] = h->data[2] = 234 * 32;
    r = wz3_bitmap_hscb(h, 0.0f, 1.0f, 0.8447265625f, 0.82275390625f);
    compare_named(text, "hscb", '=', 10, r ? wz3_bitmap_hash(r) : 0);
    wz3_bitmap_destroy(r);
    wz3_bitmap_destroy(h);
  }

  {
    wz3_bitmap *black = wz3_bitmap_flat(&ctx, 10, 0, 0xff000000u);
    r = wz3_bitmap_glowrect(black, 0, 0, 1, 1, 0, 0, 0xffffffffu, 1, 0.625f, 0, 2);
    compare_named(text, "glow", '=', 10, r ? wz3_bitmap_hash(r) : 0);
    wz3_bitmap_destroy(r);
    wz3_bitmap_destroy(black);
  }

  r = wz3_bitmap_bricks(&ctx, 3, 3, 0xff000000u, 0xffffffffu, 0xff000000u, 0, 0, 1, 1, 138, 0, 0, 0, 0.25f);
  compare_named(text, "bricks", '=', 10, r ? wz3_bitmap_hash(r) : 0);
  wz3_bitmap_destroy(r);

  r = wz3_bitmap_bulge(source, 1.0f);
  compare_named(text, "bulge", '=', 10, r ? wz3_bitmap_hash(r) : 0);
  wz3_bitmap_destroy(r);

  {
    wz3_bitmap *normals = wz3_bitmap_copy(source);
    const size_t n = (size_t)normals->width * (size_t)normals->height * 4;
    for (size_t i = 3; i < n; i += 4) normals->data[i] = 0xffff;
    r = wz3_bitmap_bump(source, normals, 2, 0.5f, 0.5f, 0.5f, 0.125f, 1.0f, 0xffffffffu, 0xffffffffu, 0.75f, 1.0f, 1.0f, 0xffffffffu, 16.0f, 1.0f);
    compare_named(text, "directional_bump", '=', 10, r ? wz3_bitmap_hash(r) : 0);
    wz3_bitmap_destroy(r);
    r = wz3_bitmap_bump(source, normals, 2, 0.5f, 0.5f, 0.5f, 0.25f, -0.125f, 0xffffffffu, 0xffffffffu, 0.75f, 1.0f, 1.0f, 0xffffffffu, 16.0f, 1.0f);
    compare_named(text, "directional_bump_trig", '=', 10, r ? wz3_bitmap_hash(r) : 0);
    wz3_bitmap_destroy(r);
    wz3_bitmap_destroy(normals);
  }
  wz3_bitmap_destroy(source);
}

/* ---- legacy lookups --------------------------------------------------------------------------- */

static void check_lookup(const char *text)
{
  static uint8_t cube[WZ3_CUBE_NORMALIZER_BYTES];
  static uint8_t volume[WZ3_ATTENUATION_VOLUME_BYTES];
  wz3_make_cube_normalizer(cube);
  wz3_make_attenuation_volume(volume);
  compare_named(text, "cube", ' ', 16, fnv_bytes(cube, sizeof cube));
  for (int face = 0; face < 6; face++) {
    char key[16];
    snprintf(key, sizeof key, "face%d", face);
    compare_named(text, key, ' ', 16, fnv_bytes(cube + (size_t)face * 64 * 64 * 4, 64 * 64 * 4));
  }
  compare_named(text, "volume", ' ', 16, fnv_bytes(volume, sizeof volume));
}

int main(int argc, char **argv)
{
  if (argc != 3) {
    fprintf(stderr, "usage: %s <blur|cell|perlin|misc|lookup> <oracle-output>\n", argv[0]);
    return 2;
  }
  char *text = read_file(argv[2]);
  if (!text || !*text) {
    fprintf(stderr, "cannot read oracle output %s\n", argv[2]);
    return 2;
  }
  if (!strcmp(argv[1], "blur"))
    check_blur(text);
  else if (!strcmp(argv[1], "cell"))
    check_cell(text);
  else if (!strcmp(argv[1], "perlin"))
    check_perlin(text);
  else if (!strcmp(argv[1], "misc"))
    check_misc(text);
  else if (!strcmp(argv[1], "lookup"))
    check_lookup(text);
  else {
    fprintf(stderr, "unknown oracle %s\n", argv[1]);
    return 2;
  }
  free(text);
  printf("%s: %d checks, %d failures\n", argv[1], g_checks, g_failures);
  return g_failures ? 1 : 0;
}
