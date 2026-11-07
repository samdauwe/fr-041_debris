/* Checks for player_core.h.
 *
 *   test_player_core <loader.v2m> <party.kx> <replay_sequence.txt>
 *
 * pclock: a simulated audio device (reports in whole periods, polled by a jittery frame loop) must never see the clock
 * run backwards, never lead the audio, and lag it by no more than one period plus one poll interval; pauses, seeks and
 * an underrun are modelled. paudio: the loader -> party hand-over, pause, seek, error paths and every allocation failure
 * leave nothing behind, and chunked rendering equals one call.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static long g_live, g_count, g_fail_at = -1;
static void *t_malloc(size_t n)
{
  if (g_count++ == g_fail_at) return NULL;
  size_t *p = (size_t *)malloc(n + 16);
  if (!p) return NULL;
  g_live++;
  return (char *)p + 16;
}
#if defined(__GNUC__)
__attribute__((noinline)) /* inlined into a NULL-checked caller, gcc misreports the header arithmetic */
#endif
static void t_free(void *q)
{
  if (!q) return;
  free((void *)((uintptr_t)q - 16));
  g_live--;
}
#define V2_MALLOC(n) t_malloc(n)
#define V2_FREE(p)   t_free(p)

#define V2_IMPLEMENTATION
#include <debris/core/v2.h>
#define WZ3_IMPLEMENTATION
#include <debris/core/wz3.h>
#define PLAYER_CORE_IMPLEMENTATION
#include <debris/player/player_core.h>

static int g_checks, g_failures;
#define CHECK(cond, ...)                            \
  do {                                              \
    g_checks++;                                     \
    if (!(cond)) {                                  \
      g_failures++;                                 \
      if (g_failures <= 30) {                       \
        printf("FAIL %s:%d: ", __FILE__, __LINE__); \
        printf(__VA_ARGS__);                        \
        printf("\n");                               \
      }                                             \
    }                                               \
  } while (0)

static uint64_t g_rng = 0x9e3779b97f4a7c15ull;
static uint32_t rnd(void)
{
  g_rng ^= g_rng << 13;
  g_rng ^= g_rng >> 7;
  g_rng ^= g_rng << 17;
  return (uint32_t)(g_rng >> 32);
}
static double frand(double lo, double hi) { return lo + (hi - lo) * ((double)rnd() / 4294967296.0); }

/* ---- the simulated device ---- */

typedef struct sim {
  double rate, period; /* Hz, frames */
  double play_time;    /* seconds of audio the device has actually played */
  int device_running;
} sim;

/* what the device reports: whole periods of the true position */
static int64_t sim_reported(const sim *s) { return (int64_t)(s->period * (double)(int64_t)(s->rate * s->play_time / s->period)); }
static double sim_truth(const sim *s) { return s->rate * s->play_time; }

/* the first polls after a pause, a seek or an underrun legitimately re-anchor */
static int was_paused_now(int was_paused, double t, double paused_until) { return was_paused && t < paused_until + 0.2; }
static int sought_now(int sought, double t) { return sought && t > 15.0 && t < 15.2; }

static void run_clock(uint32_t rate, uint32_t period, double poll_lo, double poll_hi, int use_pause, int use_underrun, int use_seek)
{
  pclock c;
  sim s = { (double)rate, (double)period, 0, 1 };
  const uint32_t cap_ms = 100;
  pclock_init(&c, rate, cap_ms);
  uint64_t now = 1000000000ull; /* an arbitrary monotonic epoch */
  pclock_reset(&c, 0, now);
  pclock_set_running(&c, 1, now);
  int64_t prev = 0;
  double worst_lead = -1e9, worst_lag = 0, base = 0; /* base: the seek position */
  const double max_poll = poll_hi;
  int paused = 0, starved = 0, sought = 0, was_paused = 0, was_starved = 0;
  double paused_until = 0, starved_until = 0, t = 0, last_change_t = 0, prev_t = 0;
  int64_t prev_report = 0;
  while (t < 20.0) {
    double dt = frand(poll_lo, poll_hi);
    if ((rnd() & 255) == 0) dt = 0.09; /* a hiccup */
    t += dt;
    now += (uint64_t)(dt * 1e9);
    if (use_pause && !paused && !was_paused && t > 5.0) {
      was_paused = 1;
      paused = 1;
      paused_until = t + 1.0;
      pclock_set_running(&c, 0, now);
      s.device_running = 0;
    }
    if (paused && t >= paused_until) {
      paused = 0;
      pclock_set_running(&c, 1, now);
      s.device_running = 1;
    }
    if (use_underrun && !starved && !was_starved && t > 10.0) {
      was_starved = 1;
      starved = 1;
      starved_until = t + 1.0;
      s.device_running = 0;
    }
    if (starved && t >= starved_until) {
      starved = 0;
      s.device_running = 1;
    }
    const int recovering = was_starved && t < starved_until + (double)cap_ms / 1000.0 + 0.05;
    if (use_seek && !sought && t > 15.0) {
      sought = 1;
      s.play_time = 0;
      base = 90.0 * rate;
      pclock_reset(&c, (int64_t)base, now);
    }
    if (s.device_running) s.play_time += dt;
    const int64_t report = (int64_t)base + sim_reported(&s);
    if (report != prev_report) {
      last_change_t = t;
      prev_report = report;
    }
    pclock_report(&c, report, now);
    const int64_t v = pclock_now(&c, now);
    /* between changes of the reported value the clock must keep running with wall time, not step with the device */
    if (!paused && !starved && !was_paused_now(was_paused, t, paused_until) && t - last_change_t < (double)cap_ms / 1000.0 && t - prev_t > 0 &&
        prev_t > last_change_t && !sought_now(sought, t))
      CHECK(v - prev >= (int64_t)((t - prev_t) * rate) - 2, "clock stalled between reports: advanced %lld of %.0f frames (t=%.3f)", (long long)(v - prev),
            (t - prev_t) * rate, t);
    prev_t = t;
    const double truth = base + sim_truth(&s);
    CHECK(v >= prev, "clock went backwards: %lld -> %lld (t=%.3f)", (long long)prev, (long long)v, t);
    prev = v;
    const double lead = (double)v - truth;
    if (lead > worst_lead) worst_lead = lead;
    if (!starved) {
      const double lag = truth - (double)v;
      if (lag > worst_lag && !paused) worst_lag = lag;
    }
    /* an underrun lets the clock run on for at most the extrapolation cap */
    const double allowed_lead = (starved || recovering) ? (double)cap_ms * rate / 1000.0 + 2 : (double)rate / 1000.0;
    CHECK(lead <= allowed_lead, "clock leads the audio by %.1f frames (t=%.3f, paused %d, starved %d)", lead, t, paused, starved);
  }
  const double lag_bound = (double)period + max_poll * rate + 2;
  CHECK(worst_lag <= lag_bound, "clock lags by %.1f frames (bound %.1f) rate %u period %u", worst_lag, lag_bound, rate, period);
}

static void test_clock(void)
{
  static const uint32_t rates[] = { 44100, 48000 };
  static const uint32_t periods[] = { 256, 512, 1024, 2048 };
  for (int r = 0; r < 2; r++)
    for (int p = 0; p < 4; p++) {
      run_clock(rates[r], periods[p], 0.004, 0.025, 0, 0, 0);
      run_clock(rates[r], periods[p], 0.010, 0.040, 1, 1, 1);
    }
  /* while stopped, the clock holds */
  pclock c;
  pclock_init(&c, 44100, 250);
  pclock_reset(&c, 1000, 0);
  CHECK(pclock_now(&c, 5000000000ull) == 1000, "a stopped clock moved");
  pclock_set_running(&c, 1, 5000000000ull);
  CHECK(pclock_now(&c, 5000000000ull + 100000000ull) == 1000 + 4410, "resume extrapolation");
  printf("player clock: 8 device models x2 scenarios, %d checks so far\n", g_checks);
}

/* ---- paudio ---- */

static unsigned char *read_file(const char *path, size_t *size)
{
  FILE *f = fopen(path, "rb");
  if (!f) {
    printf("cannot open %s\n", path);
    exit(2);
  }
  fseek(f, 0, SEEK_END);
  const long n = ftell(f);
  fseek(f, 0, SEEK_SET);
  unsigned char *d = (unsigned char *)malloc((size_t)n);
  if (!d || fread(d, 1, (size_t)n, f) != (size_t)n) exit(2);
  fclose(f);
  *size = (size_t)n;
  return d;
}

static void render_chunked(paudio *a, float *out, uint32_t frames)
{
  uint32_t done = 0;
  while (done < frames) {
    uint32_t n = 1 + rnd() % 3000;
    if (n > frames - done) n = frames - done;
    paudio_render(a, out + (size_t)done * 2, n);
    done += n;
  }
}

static void test_paudio(const unsigned char *loader, size_t loader_size, const unsigned char *party, size_t party_size)
{
  enum { FRAMES = 60000 };
  float *a_out = (float *)malloc(FRAMES * 2 * sizeof(float));
  float *b_out = (float *)malloc(FRAMES * 2 * sizeof(float));
  const long base = g_live;

  paudio a;
  paudio_init(&a, 44100);
  /* nothing plays: silence */
  memset(a_out, 0x55, 64);
  paudio_render(&a, a_out, 8);
  int silent = 1;
  for (int i = 0; i < 16; i++) silent &= a_out[i] == 0.0f;
  CHECK(silent, "silence before anything plays");

  CHECK(paudio_play_loader(&a, loader, loader_size) == V2_OK && a.stage == PAUDIO_LOADER, "play loader");
  render_chunked(&a, a_out, 20000);
  CHECK(a.party_frames == 0, "the loader does not count as party frames");
  CHECK(paudio_play_party(&a, party, party_size, 0) == V2_OK && a.stage == PAUDIO_PARTY, "hand over to the party song");
  CHECK(g_live - base <= 1 + 8, "one synth alive after the hand-over"); /* synth + its song buffers */
  render_chunked(&a, a_out, FRAMES);
  CHECK(a.party_frames == FRAMES, "party frame counter %llu", (unsigned long long)a.party_frames);

  /* chunked rendering after a hand-over equals one call on a fresh synth */
  paudio b;
  paudio_init(&b, 44100);
  CHECK(paudio_play_party(&b, party, party_size, 0) == V2_OK, "fresh party");
  paudio_render(&b, b_out, FRAMES);
  CHECK(!memcmp(a_out, b_out, FRAMES * 2 * sizeof(float)), "hand-over + chunking changes the PCM");

  /* pause: silence, counter holds; resume continues where it stopped */
  paudio_set_paused(&a, 1);
  const uint64_t held = a.party_frames;
  paudio_render(&a, b_out, 4096);
  silent = 1;
  for (int i = 0; i < 4096 * 2; i++) silent &= b_out[i] == 0.0f;
  CHECK(silent && a.party_frames == held, "pause renders silence and holds the counter");
  paudio_set_paused(&a, 0);

  /* seek = restart at the offset */
  CHECK(paudio_seek(&a, 1500) == V2_OK && a.party_base == (uint64_t)(1500.0 * 44100 / 1000.0) && a.party_frames == 0, "seek bookkeeping");
  paudio_render(&a, a_out, 20000);
  CHECK(paudio_play_party(&b, party, party_size, 1500) == V2_OK, "party from 1500 ms");
  paudio_render(&b, b_out, 20000);
  CHECK(!memcmp(a_out, b_out, 20000 * 2 * sizeof(float)), "seek differs from a fresh start at the same offset");
  paudio_stop(&a);
  CHECK(paudio_seek(&a, 0) == V2_ERR_BAD_ARGUMENT, "seek without a song");

  /* bad data: an error, no synth left, the previous one is gone */
  static const unsigned char garbage[64] = { 1, 2, 3 };
  CHECK(paudio_play_party(&b, garbage, sizeof garbage, 0) != V2_OK && b.synth == NULL && b.stage == PAUDIO_SILENT, "garbage song");

  paudio_destroy(&a);
  paudio_destroy(&b);
  CHECK(g_live == base, "%ld blocks leaked", g_live - base);

  /* every allocation of a hand-over may fail */
  long points = 0;
  for (long fail = 0;; fail++) {
    paudio c;
    paudio_init(&c, 44100);
    g_count = 0;
    g_fail_at = fail;
    v2_result res = paudio_play_loader(&c, loader, loader_size);
    if (res == V2_OK) res = paudio_play_party(&c, party, party_size, 0);
    g_fail_at = -1;
    points++;
    if (res == V2_OK) {
      paudio_destroy(&c);
      break;
    }
    CHECK(c.synth == NULL && c.stage == PAUDIO_SILENT, "allocation %ld failed but a synth stayed", fail);
    paudio_destroy(&c);
    CHECK(g_live == base, "allocation %ld leaked %ld blocks", fail, g_live - base);
    if (fail > 400) {
      CHECK(0, "hand-over never succeeds");
      break;
    }
  }
  printf("player audio: hand-over, pause, seek, chunking, %ld allocation points\n", points);
  free(a_out);
  free(b_out);
}

/* pseek_next_sample against every sequence src/app.js yields (fixtures/replay_sequence.txt) */
static void test_replay_cadence(const char *path)
{
  size_t size;
  char *text = (char *)read_file(path, &size);
  text = (char *)realloc(text, size + 1);
  text[size] = 0;
  long cases = 0, samples = 0;
  for (char *line = strtok(text, "\n"); line; line = strtok(NULL, "\n")) {
    long long start, target, rate, fps;
    int n, used;
    if (sscanf(line, "R %lld %lld %lld %lld %d%n", &start, &target, &rate, &fps, &n, &used) < 5) continue;
    const char *p = line + used;
    int64_t sample = start;
    if (target < sample) target = sample;
    int i = 0;
    int ok = 1;
    while (sample < target) {
      sample = pseek_next_sample(sample, target, rate, fps);
      long long want = -1;
      int u = 0;
      if (i >= n || sscanf(p, " %lld%n", &want, &u) < 1 || want != sample) {
        ok = 0;
        break;
      }
      p += u;
      i++;
      samples++;
    }
    CHECK(ok && i == n, "replay cadence %lld..%lld @ %lld/%lld: sample %d differs", start, target, rate, fps, i);
    cases++;
  }
  CHECK(cases >= 400, "only %ld replay cases", cases);
  printf("player seek: %ld replay sequences, %ld samples identical to src/app.js\n", cases, samples);
  free(text);
}

int suite_player_core(int argc, char **argv)
{
  if (argc < 4) {
    printf("usage: test_player_core <loader.v2m> <party.kx> <replay_sequence.txt>\n");
    return 2;
  }
  test_clock();
  test_replay_cadence(argv[3]);
  size_t ls, ks;
  unsigned char *loader = read_file(argv[1], &ls), *kx_bytes = read_file(argv[2], &ks);
  wz3_kx *kx = NULL;
  if (wz3_kx_parse(&kx, kx_bytes, ks, NULL) != WZ3_OK || !kx->song) {
    printf("cannot parse %s\n", argv[2]);
    return 2;
  }
  test_paudio(loader, ls, kx->song, kx->song_size); /* the song the player starts: the KX's embedded V2M */
  wz3_kx_destroy(kx);
  free(loader);
  free(kx_bytes);
  printf("player core: %d checks, %d failures\n", g_checks, g_failures);
  return g_failures ? 1 : 0;
}
