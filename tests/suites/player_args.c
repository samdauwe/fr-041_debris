/* Checks for player_args.h: the debris command line.
 *
 *   test_player_args
 *
 * Every option sets exactly its field, every malformed argument is refused with a message that names it, and nothing can
 * overrun a short error buffer.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PLAYER_ARGS_IMPLEMENTATION
#include <debris/player/player_args.h>

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

/* splits `line` at spaces into an argv (after argv[0]) and parses it */
static int parse(const char *line, poptions *o, char *error, size_t error_size)
{
  static char copy[1024];
  char *argv[64];
  int argc = 0;
  argv[argc++] = (char *)"debris";
  snprintf(copy, sizeof copy, "%s", line);
  for (char *p = strtok(copy, " "); p && argc < 64; p = strtok(NULL, " ")) argv[argc++] = p;
  return poptions_parse(argc, argv, o, error, error_size);
}

static void accepted(const char *line, poptions *o)
{
  char error[256] = "";
  CHECK(parse(line, o, error, sizeof error), "'%s' refused: %s", line, error);
}

static void refused(const char *line, const char *mentions)
{
  poptions o;
  char error[256] = "";
  const int ok = parse(line, &o, error, sizeof error);
  CHECK(!ok, "'%s' accepted", line);
  CHECK(ok || strstr(error, mentions), "'%s': message '%s' does not mention '%s'", line, error, mentions);
}

int suite_player_args(int argc, char **argv)
{
  (void)argc;
  (void)argv;
  poptions o;
  accepted("", &o);
  CHECK(o.width == 1280 && o.height == 720 && o.vsync && o.audio && !o.fullscreen && !o.self_check && !o.seek_check, "flags");
  CHECK(o.fixed_time < 0 && o.exit_after < 0 && o.play_for < 0 && o.expect_position < 0 && o.n_script == 0, "defaults");
  CHECK(!o.kx_path && !o.loader_path && !o.screenshot_path, "paths");

  accepted("--kx a.kx --loader b.v2m --size 640x360 --fullscreen --no-vsync --no-audio --self-check", &o);
  CHECK(!strcmp(o.kx_path, "a.kx") && !strcmp(o.loader_path, "b.v2m") && o.width == 640 && o.height == 360, "values");
  CHECK(o.fullscreen && !o.vsync && !o.audio && o.self_check, "switches");
  accepted("--exit-after 1.5 --play-for 7 --expect-position 5.6", &o);
  CHECK(o.exit_after == 1.5 && o.play_for == 7.0 && o.expect_position == 5.6, "seconds");
  accepted("--time 30 --screenshot x.ppm --size 320x180", &o);
  CHECK(o.fixed_time == 30.0 && !o.audio && !strcmp(o.screenshot_path, "x.ppm"), "fixed time silences the audio");
  accepted("--time 0", &o);
  CHECK(o.fixed_time == 0.0, "time zero is a time");
  /* --texture-quality: names in any letter case and numbers from -3 to 0 */
  accepted("", &o);
  CHECK(o.texture_offset == 0 && !strcmp(o.texture_quality, "high"), "texture quality defaults to high");
  {
    static const struct {
      const char *text;
      int offset;
    } good[] = { { "high", 0 }, { "HIGH", 0 }, { "ultra", 0 }, { "Full", 0 }, { "normal", -1 }, { "Reduced", -1 }, { "MEDIUM", -1 }, { "low", -2 },
                 { "LoW", -2 }, { "0", 0 },    { "-0", 0 },    { "-1", -1 },  { "-2", -2 },     { "-3", -3 },      { "+0", 0 } };
    for (size_t i = 0; i < sizeof good / sizeof good[0]; i++) {
      char line[64];
      snprintf(line, sizeof line, "--texture-quality %s", good[i].text);
      accepted(line, &o);
      CHECK(o.texture_offset == good[i].offset && !strcmp(o.texture_quality, good[i].text), "'%s' is offset %d, got %d", good[i].text, good[i].offset,
            o.texture_offset);
    }
  }
  refused("--texture-quality", "needs a value");
  refused("--texture-quality best", "--texture-quality");
  refused("--texture-quality 1", "--texture-quality");
  refused("--texture-quality -4", "--texture-quality");
  refused("--texture-quality 0.5", "--texture-quality");
  refused("--texture-quality low2", "--texture-quality");
  refused("--texture-quality ''", "--texture-quality");
  accepted("--time 120 --seek-check", &o);
  CHECK(o.seek_check, "seek check");
  accepted("--size 64x64", &o);
  accepted("--size 16384x16384", &o);
  accepted("--time 3600", &o);
  accepted("--script 1.5:space,2.5:left,3:right,4:f11 --self-check", &o);
  CHECK(o.n_script == 4 && o.script[0].time == 1.5 && o.script[0].key == PKEY_SPACE && o.script[1].key == PKEY_LEFT && o.script[2].key == PKEY_RIGHT &&
          o.script[3].key == PKEY_F11 && o.script[3].time == 4.0,
        "script events");
  accepted("--kx first --kx second", &o);
  CHECK(!strcmp(o.kx_path, "second"), "the last one wins");
  /* window keys, statistics, GL accounting, soak, licences */
  accepted("--script 1:resize,2:fullscreen,3:credits,4:stats --self-check", &o);
  CHECK(o.n_script == 4 && o.script[0].key == PKEY_RESIZE && o.script[1].key == PKEY_FULLSCREEN && o.script[2].key == PKEY_CREDITS &&
          o.script[3].key == PKEY_STATS,
        "window script keys");
  accepted("", &o);
  CHECK(!o.stats && !o.gl_check && o.soak_loops == 0 && o.soak_limit < 0 && !o.licences && !o.single_thread,
        "defaults of the window, statistics and soak options");
  accepted("--single-thread", &o);
  CHECK(o.single_thread, "single-thread");
  accepted("--stats --gl-check --licenses", &o);
  CHECK(o.stats && o.gl_check && o.licences, "stats, gl-check, licences");
  accepted("--licences", &o);
  CHECK(o.licences, "the British spelling");
  accepted("--soak 3 --soak-limit 90.5 --gl-check", &o);
  CHECK(o.soak_loops == 3 && o.soak_limit == 90.5 && !o.audio && !o.vsync && o.gl_check, "soak silences the audio and drops vsync");
  accepted("--soak 1000", &o);
  refused("--soak 0", "--soak");
  refused("--soak 1001", "--soak");
  refused("--soak 1.5", "--soak");
  refused("--soak many", "--soak");
  refused("--soak", "--soak");
  refused("--soak 2 --soak-limit 0", "--soak-limit");
  refused("--soak 2 --soak-limit -5", "--soak-limit");
  refused("--soak-limit 10", "--soak-limit");
  refused("--soak 2 --time 5", "--soak");
  accepted("--time 5 --overlay credits", &o);
  CHECK(o.overlay_credits && !o.overlay_stats, "overlay credits");
  accepted("--time 5 --overlay stats", &o);
  CHECK(!o.overlay_credits && o.overlay_stats, "overlay stats");
  accepted("--time 5 --overlay stats,credits", &o);
  CHECK(o.overlay_credits && o.overlay_stats, "both overlays");
  accepted("--time 5 --overlay credits --overlay stats", &o);
  CHECK(!o.overlay_credits && o.overlay_stats, "the last --overlay wins");
  refused("--time 5 --overlay", "--overlay");
  refused("--time 5 --overlay credit", "--overlay");
  refused("--time 5 --overlay credits,", "--overlay");
  refused("--time 5 --overlay ,credits", "--overlay");
  refused("--time 5 --overlay credits,,stats", "--overlay");
  refused("--overlay credits", "--overlay");
  /* --times / --screenshot-dir: the golden set in one run */
  accepted("--times 0,5.5,30 --screenshot-dir out", &o);
  CHECK(o.n_times == 3 && o.times[0] == 0.0 && o.times[1] == 5.5 && o.times[2] == 30.0 && !strcmp(o.screenshot_dir, "out") && o.fixed_time == 0.0 && !o.audio,
        "times");
  accepted("--times 3600 --screenshot-dir out --overlay stats", &o);
  CHECK(o.n_times == 1 && o.times[0] == 3600.0 && o.overlay_stats, "one time, with an overlay");
  accepted("", &o);
  CHECK(o.n_times == 0 && !o.screenshot_dir, "no times by default");
  refused("--times 5,5 --screenshot-dir out", "--times");
  refused("--times 5,3 --screenshot-dir out", "--times");
  refused("--times 3601 --screenshot-dir out", "--times");
  refused("--times -1 --screenshot-dir out", "--times");
  refused("--times x --screenshot-dir out", "--times");
  refused("--times 5, --screenshot-dir out", "--times");
  refused("--times ,5 --screenshot-dir out", "--times");
  refused("--times 5,,6 --screenshot-dir out", "--times");
  refused("--times 5x --screenshot-dir out", "--times");
  refused("--times 5", "--times");
  refused("--screenshot-dir out", "--screenshot-dir");
  refused("--times 5 --screenshot-dir out --time 5", "--times");
  refused("--times 5 --screenshot-dir out --screenshot x.ppm", "--times");
  refused("--times 5 --screenshot-dir out --seek-check", "--times");
  refused("--times 5 --soak 2 --screenshot-dir out", "--soak");
  {
    char many[1024] = "--screenshot-dir out --times ";
    for (int i = 0; i < PTIMES_MAX; i++) {
      char t[8];
      snprintf(t, sizeof t, i ? ",%d" : "%d", i + 1);
      strcat(many, t);
    }
    accepted(many, &o);
    CHECK(o.n_times == PTIMES_MAX, "%d times", o.n_times);
    strcat(many, ",100");
    refused(many, "--times");
  }

  /* every option that takes a value says so when it is the last argument */
  static const char *const needs[] = { "--kx",     "--loader",          "--screenshot", "--size",       "--time",    "--exit-after", "--play-for",
                                       "--script", "--expect-position", "--soak",       "--soak-limit", "--overlay", "--times",      "--screenshot-dir" };
  for (size_t i = 0; i < sizeof needs / sizeof needs[0]; i++) {
    char line[64];
    snprintf(line, sizeof line, "--no-audio %s", needs[i]);
    refused(line, needs[i]);
  }
  refused("--nonsense", "--nonsense");
  refused("stray", "stray");
  refused("--time nonsense", "--time");
  refused("--time -1", "--time");
  refused("--time 3601", "--time");
  refused("--time 1x", "--time");
  refused("--time nan", "--time");
  refused("--exit-after -1", "--exit-after");
  refused("--exit-after abc", "--exit-after");
  refused("--play-for -0.5", "--play-for");
  refused("--expect-position x", "--expect-position");
  refused("--expect-position -1", "--expect-position");
  refused("--size 1280", "--size");
  refused("--size 1280x", "--size");
  refused("--size 1280x720x3", "--size");
  refused("--size 63x100", "--size");
  refused("--size 100x63", "--size");
  refused("--size 16385x64", "--size");
  refused("--size axb", "--size");
  refused("--seek-check", "--seek-check");
  refused("--seek-check --time 0", "--seek-check");
  refused("--screenshot x.ppm", "--screenshot");
  refused("--script 1:foo", "--script");
  refused("--script :space", "--script");
  refused("--script 1:space,", "--script");
  refused("--script -1:space", "--script");
  refused("--script space", "--script");
  {
    char many[1024] = "--script ";
    for (int i = 0; i <= PSCRIPT_MAX; i++) strcat(many, i ? ",1:space" : "1:space");
    refused(many, "--script");
    many[strlen(many) - (int)strlen(",1:space")] = 0; /* exactly PSCRIPT_MAX events */
    accepted(many, &o);
    CHECK(o.n_script == PSCRIPT_MAX, "%d events", o.n_script);
  }

  /* a short or absent error buffer is respected */
  {
    char tiny[8];
    memset(tiny, 'x', sizeof tiny);
    CHECK(!parse("--size nonsense", &o, tiny, sizeof tiny) && memchr(tiny, 0, sizeof tiny), "message is terminated in a tiny buffer");
    CHECK(!parse("--size nonsense", &o, NULL, 0), "no buffer");
  }
  CHECK(pscript_parse("", o.script, PSCRIPT_MAX) == 0, "an empty script has no events");
  CHECK(pscript_parse("0:space", o.script, 0) == -1, "no room for one event");

  printf("player_args: %d checks, %d failures\n", g_checks, g_failures);
  return g_failures ? 1 : 0;
}
