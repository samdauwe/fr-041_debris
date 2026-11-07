/* Checks for player_replay.h: the snapshot list and the replay behind a seek.
 *
 *   test_player_replay <party.kx>
 *
 * psnapshots keeps the first snapshot and the most recent ones, finds the latest one at or before a sample, and a failed
 * snapshot leaves the list as it was. preplay_seek_check restores a mid-song snapshot and replays to a target on the real
 * document and must end in exactly the state of linear playback; it reports allocation failures without leaking.
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
static void *t_realloc(void *q, size_t n)
{
  if (!q) return t_malloc(n);
  if (g_count++ == g_fail_at) return NULL;
  void *p = realloc((char *)q - 16, n + 16);
  return p ? (char *)p + 16 : NULL;
}
#if defined(__GNUC__)
__attribute__((noinline))
#endif
static void t_free(void *q)
{
  if (!q) return;
  free((char *)q - 16);
  g_live--;
}
#define PLAYER_REPLAY_MALLOC(n)     t_malloc(n)
#define PLAYER_REPLAY_REALLOC(p, n) t_realloc(p, n)
#define PLAYER_REPLAY_FREE(p)       t_free(p)

#define V2_IMPLEMENTATION
#include <debris/core/v2.h>
#define WZ3_IMPLEMENTATION
#include <debris/core/wz3.h>
#define PLAYER_CORE_IMPLEMENTATION
#include <debris/player/player_core.h>
#define PLAYER_REPLAY_IMPLEMENTATION
#include <debris/player/player_replay.h>

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

#define RATE 44100
#define FPS  30

static int read_file(const char *path, unsigned char **data, size_t *size)
{
  FILE *f = fopen(path, "rb");
  if (!f) return 0;
  fseek(f, 0, SEEK_END);
  const long n = ftell(f);
  fseek(f, 0, SEEK_SET);
  unsigned char *d = (unsigned char *)malloc((size_t)n + 1);
  if (!d || fread(d, 1, (size_t)n, f) != (size_t)n) {
    fclose(f);
    return 0;
  }
  fclose(f);
  *data = d;
  *size = (size_t)n;
  return 1;
}

static void test_list(wz3_rt *rt)
{
  psnapshots s;
  memset(&s, 0, sizeof s);
  CHECK(psnapshots_before(&s, 0) == NULL && psnapshots_bytes(&s) == 0, "an empty list");
  /* one more than the limit plus several: the first stays, the oldest after it goes */
  for (int i = 0; i < PSNAPSHOT_LIMIT + 5; i++) {
    const int64_t sample = (int64_t)i * 1000;
    CHECK(psnapshots_take(&s, rt, sample) == WZ3_OK, "take %d", i);
    CHECK(s.last_sample == sample, "last sample after take %d", i);
    CHECK(s.count == (i + 1 < PSNAPSHOT_LIMIT ? i + 1 : PSNAPSHOT_LIMIT), "count after take %d is %d", i, s.count);
  }
  CHECK(s.items[0].sample == 0, "the first snapshot stays");
  for (int i = 1; i < s.count; i++) CHECK(s.items[i].sample == (int64_t)(i + 5) * 1000, "item %d is sample %lld", i, (long long)s.items[i].sample);
  const psnapshot *b = psnapshots_before(&s, 3500);
  CHECK(b && b->sample == 0, "before 3.5k is the first (the 1k..5k ones were dropped): %lld", b ? (long long)b->sample : -1);
  b = psnapshots_before(&s, 6000);
  CHECK(b && b->sample == 6000, "at or before: exactly 6000");
  b = psnapshots_before(&s, 6999);
  CHECK(b && b->sample == 6000, "just before the next");
  b = psnapshots_before(&s, 1000000);
  CHECK(b && b->sample == 20000, "after the last: the last");
  CHECK(psnapshots_before(&s, -1) == NULL, "before the first is nothing");
  CHECK(psnapshots_bytes(&s) == (size_t)s.count * wz3_rt_snapshot_size(rt), "bytes %zu", psnapshots_bytes(&s));

  /* a failed snapshot leaves the list alone, whether the buffer or the list is what fails */
  const int count = s.count;
  const int64_t last = s.last_sample;
  const long live = g_live;
  g_count = 0;
  g_fail_at = 0;
  CHECK(psnapshots_take(&s, rt, 99999) == WZ3_ERR_OUT_OF_MEMORY, "snapshot out of memory");
  g_fail_at = -1;
  CHECK(s.count == count && s.last_sample == last && s.items[count - 1].sample == 20000 && g_live == live, "list unchanged by the failure");
  psnapshots_clear(&s);
  CHECK(s.count == 0 && g_live == live - count, "clear frees every snapshot (%ld left)", g_live - (live - count));
  CHECK(psnapshots_take(&s, rt, 5) == WZ3_OK && s.count == 1, "usable after clear");
  psnapshots_clear(&s);
}

static void test_same_state(void)
{
  const unsigned char a[4] = { 1, 2, 3, 4 }, b[4] = { 1, 2, 3, 5 }, c[3] = { 1, 2, 3 };
  CHECK(preplay_same_state(a, 4, a, 4) && preplay_same_state(a, 4, a, 4), "identical");
  CHECK(!preplay_same_state(a, 4, b, 4), "one byte differs");
  CHECK(!preplay_same_state(a, 4, c, 3) && !preplay_same_state(c, 3, a, 4), "sizes differ with a common prefix");
  CHECK(preplay_same_state(NULL, 0, NULL, 0) && !preplay_same_state(a, 4, NULL, 0), "empty");
}

static void test_replay(const wz3_kx *kx)
{
  wz3_rt_options o;
  memset(&o, 0, sizeof o);
  o.release_consumed = 1;
  o.frame_jobs = 1;
  o.geometry = WZ3_GEOMETRY_PLACEHOLDER;
  wz3_result res;
  wz3_rt *rt = wz3_rt_create(kx, &o, &res);
  CHECK(rt != NULL, "create");
  if (!rt) return;
  wz3_rt_set_aspect(rt, 2.0);
  CHECK(wz3_rt_precalc(rt) == WZ3_OK, "precalc");

  test_list(rt);

  preplay_check c;
  res = preplay_seek_check(rt, (int64_t)30 * RATE, RATE, FPS, &c);
  CHECK(res == WZ3_OK && c.equal && c.effects_equal && c.snapshot_size > 0, "30 s: %s, equal %d, effects equal %d", wz3_result_string(res), c.equal,
        c.effects_equal);
  CHECK(c.middle >= (int64_t)14 * RATE && c.middle <= (int64_t)15 * RATE + RATE / FPS, "restored at %lld", (long long)c.middle);
  res = preplay_seek_check(rt, 0, RATE, FPS, &c);
  CHECK(res == WZ3_OK && c.equal && c.effects_equal, "target 0");
  /* where the demo has deferred effects (chain lines, water) the check hashes them: 27 or 28 of them at 110 s */
  res = preplay_seek_check(rt, (int64_t)110 * RATE, RATE, FPS, &c);
  CHECK(res == WZ3_OK && c.equal && c.effects_equal && c.effects > 10 && c.effects_hash != 0, "110 s: %s, %u effects, equal %d", wz3_result_string(res),
        c.effects, c.effects_equal);
  uint32_t hash = 0, count = 0;
  CHECK(preplay_effects_hash(NULL, &hash, &count) == WZ3_ERR_ARGUMENT && preplay_effects_hash(rt, NULL, &count) == WZ3_ERR_ARGUMENT, "effects hash arguments");
  CHECK(preplay_effects_hash(rt, &hash, NULL) == WZ3_OK, "effects hash without a counter");

  /* advance: ends exactly at the target, never beyond, and is a no-op at or past it */
  uint32_t ran = 0;
  int64_t sample = 0;
  CHECK(preplay_advance(rt, &sample, 100000, RATE, FPS, &ran) == WZ3_OK && sample == 100000, "advance to 100000: %lld", (long long)sample);
  const int64_t before = sample;
  CHECK(preplay_advance(rt, &sample, 50000, RATE, FPS, &ran) == WZ3_OK && sample == before, "no backward advance");
  CHECK(preplay_step(rt, 120000, RATE, NULL) == WZ3_OK, "step without an effects counter");
  CHECK(preplay_step(rt, 0, 0, &ran) == WZ3_ERR_ARGUMENT, "step with a zero sample rate");

  /* every allocation of the check (the snapshot buffers) fails in turn */
  const long live = g_live;
  long points = 0;
  for (long fail = 0; fail < 40; fail++) {
    g_count = 0;
    g_fail_at = fail;
    res = preplay_seek_check(rt, (int64_t)4 * RATE, RATE, FPS, &c);
    g_fail_at = -1;
    points++;
    CHECK(g_live == live, "allocation %ld leaks %ld blocks", fail, g_live - live);
    if (res == WZ3_OK) {
      CHECK(c.equal && c.effects_equal, "unfailed run differs");
      break;
    }
    CHECK(res == WZ3_ERR_OUT_OF_MEMORY, "allocation %ld reported as %s", fail, wz3_result_string(res));
  }
  CHECK(points >= 3 && points < 40, "%ld allocation points", points);
  printf("player_replay: seek check equal at 30 s (restored at %.1f s), %ld allocation points clean\n", (double)c.middle / RATE, points);
  wz3_rt_destroy(rt);
}

int suite_player_replay(int argc, char **argv)
{
  if (argc < 2) {
    printf("usage: test_player_replay <party.kx>\n");
    return 2;
  }
  unsigned char *bytes;
  size_t size;
  if (!read_file(argv[1], &bytes, &size)) {
    printf("cannot read %s\n", argv[1]);
    return 2;
  }
  wz3_kx *kx = NULL;
  CHECK(wz3_kx_parse(&kx, bytes, size, NULL) == WZ3_OK, "parse");
  CHECK(kx != NULL, "document");
  test_same_state();
  if (kx) test_replay(kx);
  wz3_kx_destroy(kx);
  free(bytes);
  printf("%d checks, %d failures\n", g_checks, g_failures);
  return g_failures ? 1 : 0;
}
