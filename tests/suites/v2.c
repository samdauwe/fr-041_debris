/* Unit tests for v2.h: the public API, robustness fuzzing and the shipped songs (DEBRIS_ASSETS_DIR). */

#define V2_IMPLEMENTATION
#include <debris/core/v2.h>

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef DEBRIS_ASSETS_DIR
#error "DEBRIS_ASSETS_DIR must name the directory holding debris_loader.v2m and debris_party.kx"
#endif

static int g_checks, g_failures;

#define CHECK(cond)                                                            \
  do {                                                                         \
    g_checks++;                                                                \
    if (!(cond)) {                                                             \
      g_failures++;                                                            \
      fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
    }                                                                          \
  } while (0)

/* ---- helpers ------------------------------------------------------------ */

typedef struct blob {
  uint8_t *data;
  size_t size;
} blob;

static blob read_blob(const char *name)
{
  char path[1024];
  blob b = { NULL, 0 };
  snprintf(path, sizeof path, "%s/%s", DEBRIS_ASSETS_DIR, name);
  FILE *f = fopen(path, "rb");
  if (!f) {
    fprintf(stderr, "cannot open %s\n", path);
    exit(2);
  }
  fseek(f, 0, SEEK_END);
  long len = ftell(f);
  fseek(f, 0, SEEK_SET);
  b.data = (uint8_t *)malloc((size_t)len);
  if (!b.data || fread(b.data, 1, (size_t)len, f) != (size_t)len) {
    fprintf(stderr, "read error %s\n", path);
    exit(2);
  }
  b.size = (size_t)len;
  fclose(f);
  return b;
}

/* the V2M of a debris .kx payload follows its 40-byte header; the byte length
   is the header's last word */
static blob party_song(void)
{
  blob kx = read_blob("debris_party.kx");
  size_t len = (size_t)kx.data[36] | ((size_t)kx.data[37] << 8) | ((size_t)kx.data[38] << 16) | ((size_t)kx.data[39] << 24);
  blob b;
  b.size = len;
  b.data = (uint8_t *)malloc(len);
  memcpy(b.data, kx.data + 40, len);
  free(kx.data);
  return b;
}

static v2_synth *open_song(uint32_t rate, const blob *song)
{
  v2_synth *s = v2_create(rate);
  if (!s || v2_open(s, song->data, song->size) != V2_OK) {
    fprintf(stderr, "failed to open song at %u Hz\n", (unsigned)rate);
    exit(2);
  }
  return s;
}

static float *render(v2_synth *s, uint32_t frames)
{
  float *buf = (float *)malloc(((size_t)frames * 2 + 1) * sizeof(float));
  v2_render(s, buf, frames);
  return buf;
}

static int all_finite(const float *p, size_t n)
{
  for (size_t i = 0; i < n; i++)
    if (!isfinite(p[i])) return 0;
  return 1;
}

static float peak_of(const float *p, size_t n)
{
  float m = 0.0f;
  for (size_t i = 0; i < n; i++) m = fabsf(p[i]) > m ? fabsf(p[i]) : m;
  return m;
}

static int all_zero(const float *p, size_t n)
{
  for (size_t i = 0; i < n; i++)
    if (p[i] != 0.0f) return 0;
  return 1;
}

/* ---- synthetic v2m builder --------------------------------------------- */

static void put32(uint8_t **p, uint32_t v)
{
  *(*p)++ = (uint8_t)v;
  *(*p)++ = (uint8_t)(v >> 8);
  *(*p)++ = (uint8_t)(v >> 16);
  *(*p)++ = (uint8_t)(v >> 24);
}

/* canonical v6 defaults with an audible patch: amp EG amplify and chan volume up */
static const uint8_t k_patch_defaults[V2__NUM_PATCH_PARMS] = {
  64, 64, 1, 0,  64,  64, 0,  127, 0, 0,  64,  64, 32, 127, 0,  0,  64, 64, 32, 127, 1, 127, 0, 0,  64, 0,  0,  64,  0, 32,
  0,  64, 0, 64, 127, 64, 80, 127, 0, 64, 127, 64, 80, 64,  1,  1,  0,  64, 2,  0,   0, 1,   1, 0,  64, 2,  0,  127, 0, 127,
  0,  0,  0, 0,  0,   0,  0,  0,   0, 32, 100, 64, 64, 64,  32, 32, 0,  0,  64, 0,   0, 1,   2, 90, 32, 20, 64, 64,  4,
};
static const uint8_t k_global_defaults[V2__NUM_GLOBAL_PARMS] = {
  64, 64, 32, 127, 100, 80, 64, 64, 0, 0, 64, 0, 127, 0, 0, 1, 2, 90, 32, 20, 64, 64, 0,
};

/* One channel, one program change at tick 0, a note on at tick 1 and off at
   tick 129; 500000 us per quarter at 128 ticks per quarter. */
static size_t build_song(uint8_t *buf, int version)
{
  uint8_t *p = buf;
  int i, nparm = 0, nglob = 0;
  for (i = 0; i < V2__NUM_PATCH_PARMS; i++) nparm += v2__patch_parm_ver[i] <= version;
  for (i = 0; i < V2__NUM_GLOBAL_PARMS; i++) nglob += v2__global_parm_ver[i] <= version;

  put32(&p, 128);
  put32(&p, 1000);
  put32(&p, 1);
  *p++ = 0;
  *p++ = 0;
  *p++ = 0;
  put32(&p, 500000);
  *p++ = 4;
  *p++ = 4;
  *p++ = 8;

  put32(&p, 2);
  *p++ = 1;
  *p++ = 128;
  *p++ = 0;
  *p++ = 0;
  *p++ = 0;
  *p++ = 0;
  *p++ = 48;
  *p++ = 0;
  *p++ = 100;
  *p++ = 156;
  put32(&p, 1);
  *p++ = 0;
  *p++ = 0;
  *p++ = 0;
  *p++ = 0;
  put32(&p, 0);
  for (i = 0; i < 7; i++) put32(&p, 0);
  for (i = 1; i < 16; i++) put32(&p, 0);

  put32(&p, (uint32_t)nglob);
  for (i = 0; i < V2__NUM_GLOBAL_PARMS; i++)
    if (v2__global_parm_ver[i] <= version) *p++ = k_global_defaults[i];

  put32(&p, (uint32_t)(4 + nparm + 1));
  put32(&p, 4);
  for (i = 0; i < V2__NUM_PATCH_PARMS; i++)
    if (v2__patch_parm_ver[i] <= version) *p++ = k_patch_defaults[i];
  *p++ = 0;     /* modnum */

  put32(&p, 0); /* no speech */
  return (size_t)(p - buf);
}

/* ---- tests -------------------------------------------------------------- */

static void test_api_basics(void)
{
  CHECK(v2_create(0) == NULL);
  CHECK(v2_create(V2_MIN_SAMPLE_RATE - 1) == NULL);
  CHECK(v2_create(22050) == NULL); /* unstable filters below 44.1 kHz */
  CHECK(v2_create(V2_MAX_SAMPLE_RATE + 1) == NULL);
  v2_synth *s = v2_create(44100);
  CHECK(s != NULL);
  CHECK(v2_sample_rate(s) == 44100);
  CHECK(v2_file_version(s) == -1);
  CHECK(!v2_is_playing(s));
  CHECK(v2_fp_env_ok());
  CHECK(v2_length_frames(s) == 0);

  float buf[64];
  memset(buf, 0x55, sizeof buf);
  v2_render(s, buf, 32);
  CHECK(all_zero(buf, 64));
  v2_play(s, 0); /* no song: no-op */
  CHECK(!v2_is_playing(s));
  v2_stop(s, 0);

  int poly[16];
  uint32_t ons[16];
  float levels[16];
  CHECK(v2_get_channel_poly(s, poly) == 0);
  v2_get_channel_note_ons(s, ons);
  v2_get_channel_levels(s, levels);
  v2_midi(s, (const uint8_t *)"\x90\x30\x40", 3);
  v2_render_direct(s, buf, 16);
  CHECK(all_zero(buf, 32));

  CHECK(v2_open(NULL, buf, sizeof buf) == V2_ERR_BAD_ARGUMENT);
  CHECK(v2_open(s, NULL, 100) == V2_ERR_BAD_FILE);
  v2_render(NULL, buf, 1);
  v2_render(s, NULL, 1);
  CHECK(v2_file_version(NULL) == -1);
  CHECK(v2_sample_rate(NULL) == 0);
  CHECK(!v2_is_playing(NULL));
  v2_destroy(s);
  v2_destroy(NULL);

  for (int r = V2_OK; r <= V2_ERR_BAD_ARGUMENT; r++) {
    const char *str = v2_result_string((v2_result)r);
    CHECK(str && *str);
  }
  CHECK(strcmp(v2_result_string((v2_result)99), "unknown error") == 0);
}

static void test_inplace(void)
{
  uint8_t song[512];
  size_t n = build_song(song, 6);
  size_t size = v2_synth_size();
  CHECK(size > 1000000);

  uint8_t *raw = (uint8_t *)malloc(size + 16);
  CHECK(v2_synth_init(raw + 1, size, 44100) == NULL); /* misaligned */
  CHECK(v2_synth_init(raw, size - 1, 44100) == NULL); /* too small */
  CHECK(v2_synth_init(raw, size, 100) == NULL);       /* bad rate */
  CHECK(v2_synth_init(NULL, size, 44100) == NULL);
  v2_synth *a = v2_synth_init(raw, size, 44100);
  CHECK(a != NULL);
  CHECK(v2_open(a, song, n) == V2_OK);
  v2_play(a, 0);
  float *x = render(a, 20000);

  v2_synth *b = v2_create(44100);
  CHECK(v2_open(b, song, n) == V2_OK);
  v2_play(b, 0);
  float *y = render(b, 20000);
  CHECK(memcmp(x, y, 20000 * 2 * sizeof(float)) == 0);
  CHECK(peak_of(x, 40000) > 0.01f);

  v2_close(a); /* in-place instances are released by the owner of the storage */
  free(raw);
  v2_destroy(b);
  free(x);
  free(y);
}

static void test_open_errors(void)
{
  uint8_t song[512];
  size_t n = build_song(song, 6);
  v2_synth *s = v2_create(44100);

  CHECK(v2_open(s, song, n) == V2_OK);
  CHECK(v2_file_version(s) == 6);

  /* other format versions are recognized and rejected */
  for (int ver = 0; ver <= 5; ver++) {
    uint8_t old[512];
    size_t m = build_song(old, ver);
    v2_result r = v2_open(s, old, m);
    CHECK(r == V2_ERR_UNSUPPORTED_VERSION);
    if (ver == 0) CHECK(v2_file_version(s) == 0);
    if (ver == 5) CHECK(v2_file_version(s) == 5);
    CHECK(!v2_is_playing(s));
  }

  /* every truncation fails cleanly, except those that only drop the optional speech block */
  int accepted = 0;
  for (size_t cut = 0; cut < n; cut++) accepted += v2_open(s, song, cut) == V2_OK;
  CHECK(accepted <= 8);
  CHECK(v2_open(s, song, 8) == V2_ERR_BAD_FILE);

  uint8_t zeros[256] = { 0 };
  CHECK(v2_open(s, zeros, sizeof zeros) == V2_ERR_BAD_FILE);

  /* an open failure leaves the instance silent and reusable */
  CHECK(v2_open(s, zeros, sizeof zeros) == V2_ERR_BAD_FILE);
  float buf[64];
  v2_render(s, buf, 32);
  CHECK(all_zero(buf, 64));
  CHECK(v2_open(s, song, n) == V2_OK);
  v2_destroy(s);
}

static void test_synthetic_song(void)
{
  uint8_t song[512];
  size_t n = build_song(song, 6);

  static const uint32_t rates[] = { 44100, 48000, 64000, 88200, 96000 };
  for (size_t r = 0; r < sizeof rates / sizeof rates[0]; r++) {
    v2_synth *s = v2_create(rates[r]);
    CHECK(v2_open(s, song, n) == V2_OK);

    /* 129 ticks of 500000/128 us; the tempo uses the integer rate/100 */
    uint64_t len = v2_length_frames(s);
    double expect = 129.0 * 500000.0 / 128.0 / 1e6 * (rates[r] / 100 * 100);
    CHECK(fabs((double)len - expect) < 3.0);

    v2_play(s, 0);
    CHECK(v2_is_playing(s));
    float *a = render(s, rates[r] / 4);
    CHECK(all_finite(a, rates[r] / 2));
    CHECK(peak_of(a, rates[r] / 2) > 0.005f);

    /* the song ends after its last event; the voice tail keeps ringing */
    float *b = render(s, rates[r] * 2);
    CHECK(!v2_is_playing(s));
    CHECK(all_finite(b, (size_t)rates[r] * 4));
    float *c = render(s, rates[r] / 2);
    CHECK(all_finite(c, rates[r]));
    free(a);
    free(b);
    free(c);
    v2_destroy(s);
  }
}

static void test_chunk_invariance(void)
{
  blob loader = read_blob("debris_loader.v2m");
  const uint32_t total = 220000;
  v2_synth *a = open_song(44100, &loader);
  v2_play(a, 0);
  float *whole = render(a, total);
  CHECK(peak_of(whole, (size_t)total * 2) > 0.0f);

  static const uint32_t sizes[] = { 1, 2, 3, 127, 128, 129, 255, 256, 257, 1000, 4097, 33 };
  v2_synth *b = open_song(44100, &loader);
  v2_play(b, 0);
  float *parts = (float *)malloc((size_t)total * 2 * sizeof(float));
  uint32_t done = 0, k = 0;
  while (done < total) {
    uint32_t n = sizes[k++ % (sizeof sizes / sizeof sizes[0])];
    if (n > total - done) n = total - done;
    v2_render(b, parts + (size_t)done * 2, n);
    done += n;
  }
  CHECK(memcmp(whole, parts, (size_t)total * 2 * sizeof(float)) == 0);

  /* across the song end as well */
  uint8_t song[512];
  size_t sn = build_song(song, 6);
  v2_synth *c = v2_create(44100), *d = v2_create(44100);
  v2_open(c, song, sn);
  v2_open(d, song, sn);
  v2_play(c, 0);
  v2_play(d, 0);
  float *x = render(c, 90000);
  float *y = (float *)malloc(90000 * 2 * sizeof(float));
  for (uint32_t i = 0; i < 90000; i += 300) v2_render(d, y + (size_t)i * 2, 300);
  CHECK(memcmp(x, y, 90000 * 2 * sizeof(float)) == 0);

  free(whole);
  free(parts);
  free(x);
  free(y);
  free(loader.data);
  v2_destroy(a);
  v2_destroy(b);
  v2_destroy(c);
  v2_destroy(d);
}

static void test_play_seek_length(void)
{
  blob loader = read_blob("debris_loader.v2m");
  v2_synth *s = open_song(44100, &loader);

  v2_play(s, 0);
  float *a = render(s, 50000);
  v2_play(s, 0);
  float *b = render(s, 50000);
  CHECK(memcmp(a, b, 50000 * 2 * sizeof(float)) == 0);

  /* v2_length_frames resets the engine; a following play restarts identically */
  CHECK(v2_length_frames(s) == 2559552); /* documented length of the loader sequence */
  CHECK(!v2_is_playing(s));
  v2_play(s, 0);
  float *c = render(s, 50000);
  CHECK(memcmp(a, c, 50000 * 2 * sizeof(float)) == 0);

  v2_play(s, 20000);
  float *d = render(s, 44100);
  CHECK(all_finite(d, 88200));
  CHECK(v2_is_playing(s));

  v2_play(s, 0xffffffffu); /* absurd seek: clamps to the end, no crash */
  float *e = render(s, 1000);
  CHECK(all_finite(e, 2000));
  v2_play(s, 3600u * 1000u); /* beyond the song end */
  CHECK(!v2_is_playing(s));
  float *f = render(s, 1000);
  CHECK(all_finite(f, 2000));

  free(a);
  free(b);
  free(c);
  free(d);
  free(e);
  free(f);
  v2_destroy(s);
  free(loader.data);

  blob party = party_song();
  v2_synth *p = open_song(44100, &party);
  CHECK(v2_length_frames(p) == 18934066); /* documented length of the party sequence */
  v2_destroy(p);
  free(party.data);
}

static void test_mask_and_monitors(void)
{
  blob loader = read_blob("debris_loader.v2m");
  v2_synth *s = open_song(44100, &loader);

  v2_play(s, 0);
  float *full = render(s, 200000);

  v2_set_channel_mask(s, 0); /* persists across play */
  v2_play(s, 0);
  float *none = render(s, 200000);
  CHECK(peak_of(none, 400000) < 1e-3f * peak_of(full, 400000));

  v2_set_channel_mask(s, 0xffff);
  v2_play(s, 0);
  float *again = render(s, 200000);
  CHECK(memcmp(full, again, 200000 * 2 * sizeof(float)) == 0);

  uint32_t ons[16];
  int poly[16];
  float lv[16];
  v2_get_channel_note_ons(s, ons);
  uint32_t total_ons = 0;
  for (int i = 0; i < 16; i++) total_ons += ons[i];
  CHECK(total_ons > 0);

  int total = v2_get_channel_poly(s, poly);
  int sum = 0;
  for (int i = 0; i < 16; i++) sum += poly[i];
  CHECK(total == sum && total >= 0 && total <= 64);

  v2_get_channel_levels(s, lv);
  float lsum = 0;
  for (int i = 0; i < 16; i++) lsum += lv[i];
  CHECK(lsum > 0.0f);
  v2_get_channel_levels(s, lv); /* read-and-clear */
  lsum = 0;
  for (int i = 0; i < 16; i++) lsum += lv[i];
  CHECK(lsum == 0.0f);

  free(full);
  free(none);
  free(again);
  free(loader.data);
  v2_destroy(s);
}

static void test_stop_and_fade(void)
{
  blob loader = read_blob("debris_loader.v2m");
  v2_synth *s = open_song(44100, &loader);

  v2_play(s, 0);
  float *a = render(s, 100000);
  v2_stop(s, 0);
  CHECK(!v2_is_playing(s));
  float *b = render(s, 2000);
  CHECK(all_zero(b, 4000));

  v2_play(s, 0);
  free(a);
  a = render(s, 100000);
  v2_stop(s, 100); /* 100 ms fade */
  CHECK(v2_is_playing(s));
  float *c = render(s, 8820);
  CHECK(all_finite(c, 17640));
  CHECK(!v2_is_playing(s)); /* 100 ms = 4410 frames */
  CHECK(all_zero(c + 2 * 4500, 2 * (8820 - 4500)));
  float *d = render(s, 1000);
  CHECK(all_zero(d, 2000));

  free(a);
  free(b);
  free(c);
  free(d);
  free(loader.data);
  v2_destroy(s);
}

static void test_direct_midi(void)
{
  uint8_t song[512];
  size_t n = build_song(song, 6);
  v2_synth *s = v2_create(44100);
  CHECK(v2_open(s, song, n) == V2_OK);

  int poly[16];
  float buf[2 * 4096];
  static const uint8_t on[] = { 0x90, 48, 100 };
  v2_midi(s, on, sizeof on);
  CHECK(v2_get_channel_poly(s, poly) == 1 && poly[0] == 1);
  v2_render_direct(s, buf, 4096);
  CHECK(all_finite(buf, sizeof buf / sizeof buf[0]));
  CHECK(peak_of(buf, sizeof buf / sizeof buf[0]) > 0.001f);

  static const uint8_t off[] = { 0x80, 48, 0 };
  v2_midi(s, off, sizeof off);
  for (int i = 0; i < 400 && v2_get_channel_poly(s, poly) > 0; i++) v2_render_direct(s, buf, 4096);
  CHECK(v2_get_channel_poly(s, poly) == 0); /* released voices are freed */

  /* velocity 0 is a note off; a truncated message and a missing terminator are ignored */
  static const uint8_t onoff[] = { 0x90, 50, 90, 0x90, 50, 0 };
  v2_midi(s, onoff, sizeof onoff);
  v2_render_direct(s, buf, 4096);
  static const uint8_t trunc[] = { 0x90, 52 };
  v2_midi(s, trunc, sizeof trunc);
  static const uint8_t term[] = { 0x93, 60, 100, 0xfd, 0x93, 61, 100 };
  v2_midi(s, term, sizeof term);
  v2_get_channel_poly(s, poly);
  CHECK(poly[3] == 1);

  static const uint8_t alloff[] = { 0xb3, 123, 0 };
  v2_midi(s, alloff, sizeof alloff);
  static const uint8_t soundoff[] = { 0xb3, 120, 0 };
  v2_midi(s, soundoff, sizeof soundoff);
  v2_get_channel_poly(s, poly);
  CHECK(poly[3] == 0);

  /* system reset byte followed by stray data must not hang */
  static const uint8_t sys[] = { 0xff, 1, 2, 3 };
  v2_midi(s, sys, sizeof sys);
  v2_midi(s, NULL, 4);
  v2_midi(s, on, 0);
  v2_destroy(s);
}

static void test_two_instances(void)
{
  blob loader = read_blob("debris_loader.v2m"), party = party_song();
  const uint32_t n = 80000, step = 997;

  v2_synth *l1 = open_song(44100, &loader), *p1 = open_song(44100, &party);
  v2_play(l1, 0);
  v2_play(p1, 0);
  float *lref = render(l1, n), *pref = render(p1, n);

  v2_synth *l2 = open_song(44100, &loader), *p2 = open_song(48000, &party);
  v2_synth *p3 = open_song(44100, &party);
  v2_play(l2, 0);
  v2_play(p2, 0);
  v2_play(p3, 0);
  float *lout = (float *)malloc((size_t)n * 2 * sizeof(float));
  float *pout = (float *)malloc((size_t)n * 2 * sizeof(float));
  float *junk = (float *)malloc(step * 2 * sizeof(float));
  for (uint32_t i = 0; i < n; i += step) {
    uint32_t c = n - i < step ? n - i : step;
    v2_render(l2, lout + (size_t)i * 2, c);
    v2_render(p2, junk, c); /* a third, independent rate interleaved */
    v2_render(p3, pout + (size_t)i * 2, c);
  }
  CHECK(memcmp(lref, lout, (size_t)n * 2 * sizeof(float)) == 0);
  CHECK(memcmp(pref, pout, (size_t)n * 2 * sizeof(float)) == 0);

  free(lref);
  free(pref);
  free(lout);
  free(pout);
  free(junk);
  v2_destroy(l1);
  v2_destroy(l2);
  v2_destroy(p1);
  v2_destroy(p2);
  v2_destroy(p3);
  free(loader.data);
  free(party.data);
}

static void test_sample_rates(void)
{
  blob loader = read_blob("debris_loader.v2m");
  static const uint32_t rates[] = { 44100, 48000, 64000, 88200, 96000 };
  for (size_t i = 0; i < sizeof rates / sizeof rates[0]; i++) {
    v2_synth *s = open_song(rates[i], &loader);
    v2_play(s, 0);
    uint32_t frames = rates[i] * 4;
    float *a = render(s, frames);
    CHECK(all_finite(a, (size_t)frames * 2));
    float pk = peak_of(a, (size_t)frames * 2);
    CHECK(pk > 0.001f && pk < 4.0f);
    /* the sequence length scales with the integer tempo unit rate/100 (the
       original's fractional carry is only approximate, hence the tolerance) */
    double len = (double)v2_length_frames(s);
    double expect = 2559552.0 / 441.0 * (rates[i] / 100);
    CHECK(fabs(len - expect) / expect < 1e-4);
    free(a);
    v2_destroy(s);
  }
  free(loader.data);

  /* a dense production song stays finite at the highest rates too */
  blob party = party_song();
  for (size_t i = 3; i < sizeof rates / sizeof rates[0]; i++) {
    v2_synth *s = open_song(rates[i], &party);
    v2_play(s, 0);
    float *a = render(s, rates[i] * 6);
    CHECK(all_finite(a, (size_t)rates[i] * 12));
    CHECK(peak_of(a, (size_t)rates[i] * 12) < 4.0f);
    free(a);
    v2_destroy(s);
  }
  free(party.data);
}

static void test_seed(void)
{
  blob loader = read_blob("debris_loader.v2m");
  v2_synth *a = open_song(44100, &loader), *b = open_song(44100, &loader);
  v2_set_seed(a, 0x1234567812345678ull);
  v2_set_seed(b, 0x1234567812345678ull);
  v2_play(a, 0);
  v2_play(b, 0);
  float *x = render(a, 100000), *y = render(b, 100000);
  CHECK(memcmp(x, y, 100000 * 2 * sizeof(float)) == 0);
  CHECK(all_finite(x, 200000));
  free(x);
  free(y);
  free(loader.data);
  v2_destroy(a);
  v2_destroy(b);
}

/* ---- robustness: mutated songs must never crash or hang ------------------ */

static uint64_t rng_state = 0x9e3779b97f4a7c15ull;
static uint32_t rnd(void)
{
  rng_state ^= rng_state << 13;
  rng_state ^= rng_state >> 7;
  rng_state ^= rng_state << 17;
  return (uint32_t)(rng_state >> 16);
}

static void fuzz_one(v2_synth *s, uint8_t *buf, size_t size, int extra_calls)
{
  v2_result r = v2_open(s, buf, size);
  if (r != V2_OK) return;
  v2_set_channel_mask(s, rnd());
  v2_play(s, rnd() % 4 == 0 ? rnd() % 200000 : 0);
  float out[2 * 1500];
  v2_render(s, out, 1500);
  int poly[16];
  uint32_t ons[16];
  float lv[16];
  v2_get_channel_poly(s, poly);
  v2_get_channel_note_ons(s, ons);
  v2_get_channel_levels(s, lv);
  if (extra_calls) {
    (void)v2_length_frames(s);
    v2_play(s, 0);
    v2_render(s, out, 700);
  }
  v2_stop(s, rnd() % 50);
  v2_render(s, out, 100);
}

static void test_fuzz(void)
{
  blob loader = read_blob("debris_loader.v2m"), party = party_song();
  uint8_t synth_song[512];
  size_t synth_n = build_song(synth_song, 6);
  v2_synth *s = v2_create(44100);
  const char *scale_env = getenv("V2_FUZZ_SCALE"); /* e.g. 20 for a long soak run */
  const int scale = scale_env && atoi(scale_env) > 0 ? atoi(scale_env) : 1;
  const struct {
    const uint8_t *data;
    size_t size;
    int iters;
  } sources[] = {
    { loader.data, loader.size, 1200 * scale },
    { synth_song, synth_n, 600 * scale },
    { party.data, party.size, 150 * scale },
  };

  for (size_t k = 0; k < sizeof sources / sizeof sources[0]; k++) {
    uint8_t *buf = (uint8_t *)malloc(sources[k].size);
    for (int it = 0; it < sources[k].iters; it++) {
      memcpy(buf, sources[k].data, sources[k].size);
      size_t size = sources[k].size;
      int muts = 1 + (int)(rnd() % 6);
      for (int m = 0; m < muts; m++) {
        /* bias mutations toward the structured start of the file */
        size_t pos = (rnd() & 1) ? rnd() % (size < 400 ? size : 400) : rnd() % size;
        switch (rnd() % 4) {
        case 0: buf[pos] ^= (uint8_t)(1u << (rnd() % 8)); break;
        case 1: buf[pos] = (uint8_t)rnd(); break;
        case 2: buf[pos] = (rnd() & 1) ? 0xff : 0x00; break;
        case 3:
          if (size > 16 && (rnd() % 8) == 0) size = 16 + rnd() % (size - 16);
          break;
        }
      }
      fuzz_one(s, buf, size, it % 5 == 0);
    }
    free(buf);
  }
  v2_destroy(s);
  free(loader.data);
  free(party.data);
}

int suite_v2(int argc, char **argv)
{
  (void)argc;
  (void)argv;
  test_api_basics();
  test_inplace();
  test_open_errors();
  test_synthetic_song();
  test_chunk_invariance();
  test_play_seek_length();
  test_mask_and_monitors();
  test_stop_and_fade();
  test_direct_midi();
  test_two_instances();
  test_sample_rates();
  test_seed();
  test_fuzz();

  printf("%d checks, %d failures\n", g_checks, g_failures);
  return g_failures ? 1 : 0;
}
