/* player_args.h: the command line of the debris player, SDL-free so that it can be tested headless.
 *
 *   #define PLAYER_ARGS_IMPLEMENTATION in exactly one source file.
 *
 * poptions_parse() fills a poptions or explains in `error` what is wrong with the arguments (unknown option, missing or
 * malformed value, a bad key script); nothing is printed and nothing is exited here.
 */
#ifndef PLAYER_ARGS_H
#define PLAYER_ARGS_H

#include <stddef.h>

#ifndef PLAYER_ARGS_API
#ifdef PLAYER_ARGS_STATIC
#define PLAYER_ARGS_API static
#else
#define PLAYER_ARGS_API extern
#endif
#endif

typedef enum pkey { PKEY_SPACE = 1, PKEY_LEFT, PKEY_RIGHT, PKEY_F11, PKEY_RESIZE, PKEY_FULLSCREEN, PKEY_CREDITS, PKEY_STATS } pkey;

typedef struct pscript_event {
  double time; /* seconds after the party started */
  pkey key;
} pscript_event;

#define PSCRIPT_MAX 32
#define PTIMES_MAX  64

typedef struct poptions {
  const char *kx_path, *loader_path, *screenshot_path;
  int width, height;
  int fullscreen, vsync, audio, self_check;
  double fixed_time; /* < 0: live */
  double exit_after; /* < 0: never; seconds after the process started (loading included) */
  double play_for;   /* < 0: never; seconds after the party started */
  pscript_event script[PSCRIPT_MAX];
  int n_script;
  double expect_position;             /* < 0: unchecked */
  int seek_check;
  const char *texture_quality;        /* --texture-quality as given, "high" by default */
  int texture_offset;                 /* the size exponent offset it means: 0 high, -1 normal, -2 low, down to -3 */
  int stats;                          /* --stats: one statistics line per second on stdout */
  int single_thread;                  /* --single-thread: load in time slices on the main thread instead of a worker (always so on the web) */
  int gl_check;                       /* --gl-check: count GL objects and poll glGetError (reported and checked at exit) */
  int soak_loops;                     /* --soak N: play the whole demo N times in deterministic steps (0: off) */
  double soak_limit;                  /* --soak-limit S: stop after S wall seconds even in the middle of a loop (< 0: none) */
  int licences;                       /* --licenses: print the credits and licences and exit */
  int overlay_credits, overlay_stats; /* --overlay credits,stats: draw those screens over the --time picture */
  double times[PTIMES_MAX];           /* --times T1,T2,...: strictly increasing song positions drawn in one run (the golden set) ... */
  int n_times;
  const char *screenshot_dir;         /* ... each written to DIR/frame_<milliseconds>.ppm */
} poptions;

/* Returns 1 on success; 0 with a message in `error` (error_size > 0). argv[0] is skipped; the strings are not copied. */
PLAYER_ARGS_API int poptions_parse(int argc, char **argv, poptions *o, char *error, size_t error_size);
/* --texture-quality: high|ultra|full (0), normal|reduced|medium (-1), low (-2) in any letter case, or a number from -3 to 0 (the
   offset added to every bitmap size exponent). Returns 1 and sets *offset, or 0. the reference player takes anything and means high for what it
   does not know; a command line rather says so. */
PLAYER_ARGS_API int ptexture_quality(const char *text, int *offset);
/* Key script "T:KEY,T:KEY" (T seconds after the party started; KEY one of space, left, right, f11, resize, fullscreen, credits, stats). Returns the event count or -1. */
PLAYER_ARGS_API int pscript_parse(const char *text, pscript_event *out, int max);

#endif /* PLAYER_ARGS_H */

#ifdef PLAYER_ARGS_IMPLEMENTATION
#ifndef PLAYER_ARGS_IMPLEMENTATION_DONE
#define PLAYER_ARGS_IMPLEMENTATION_DONE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

PLAYER_ARGS_API int pscript_parse(const char *text, pscript_event *out, int max)
{
  int n = 0;
  while (*text) {
    char *end;
    const double t = strtod(text, &end);
    if (end == text || *end != ':' || !(t >= 0.0)) return -1;
    const char *key = end + 1;
    const size_t len = strcspn(key, ",");
    pkey code;
    if (len == 5 && !strncmp(key, "space", 5))
      code = PKEY_SPACE;
    else if (len == 4 && !strncmp(key, "left", 4))
      code = PKEY_LEFT;
    else if (len == 5 && !strncmp(key, "right", 5))
      code = PKEY_RIGHT;
    else if (len == 3 && !strncmp(key, "f11", 3))
      code = PKEY_F11;
    else if (len == 6 && !strncmp(key, "resize", 6))
      code = PKEY_RESIZE;
    else if (len == 10 && !strncmp(key, "fullscreen", 10))
      code = PKEY_FULLSCREEN;
    else if (len == 7 && !strncmp(key, "credits", 7))
      code = PKEY_CREDITS;
    else if (len == 5 && !strncmp(key, "stats", 5))
      code = PKEY_STATS;
    else
      return -1;
    if (n == max) return -1;
    out[n].time = t;
    out[n].key = code;
    n++;
    text = key + len;
    if (*text == ',') {
      text++;
      if (!*text) return -1; /* a trailing comma */
    }
  }
  return n;
}

PLAYER_ARGS_API int ptexture_quality(const char *text, int *offset)
{
  static const struct {
    const char *name;
    int offset;
  } names[] = { { "high", 0 }, { "ultra", 0 }, { "full", 0 }, { "normal", -1 }, { "reduced", -1 }, { "medium", -1 }, { "low", -2 } };
  for (size_t k = 0; k < sizeof names / sizeof names[0]; k++) {
    const char *a = text, *b = names[k].name;
    while (*a && *b && ((*a >= 'A' && *a <= 'Z') ? *a + 32 : *a) == *b) {
      a++;
      b++;
    }
    if (!*a && !*b) {
      *offset = names[k].offset;
      return 1;
    }
  }
  char *end;
  const long v = strtol(text, &end, 10);
  if (end == text || *end || v < -3 || v > 0) return 0;
  *offset = (int)v;
  return 1;
}

static int poptions_number(const char *text, double *value)
{
  char *end;
  *value = strtod(text, &end);
  return *text && !*end && *value == *value;
}

PLAYER_ARGS_API int poptions_parse(int argc, char **argv, poptions *o, char *error, size_t error_size)
{
#define PARSE_FAIL(...)                                       \
  do {                                                        \
    if (error_size) snprintf(error, error_size, __VA_ARGS__); \
    return 0;                                                 \
  } while (0)
  memset(o, 0, sizeof *o);
  o->width = 1280;
  o->height = 720;
  o->vsync = 1;
  o->audio = 1;
  o->fixed_time = -1;
  o->exit_after = -1;
  o->play_for = -1;
  o->expect_position = -1;
  o->texture_quality = "high";
  o->soak_limit = -1;
  for (int i = 1; i < argc; i++) {
    const char *arg = argv[i];
    static const char *const with_value[] = { "--kx",
                                              "--loader",
                                              "--screenshot",
                                              "--size",
                                              "--time",
                                              "--exit-after",
                                              "--play-for",
                                              "--script",
                                              "--expect-position",
                                              "--texture-quality",
                                              "--soak",
                                              "--soak-limit",
                                              "--overlay",
                                              "--times",
                                              "--screenshot-dir" };
    int takes_value = 0;
    for (size_t k = 0; k < sizeof with_value / sizeof with_value[0]; k++) takes_value |= !strcmp(arg, with_value[k]);
    if (takes_value && i + 1 >= argc) PARSE_FAIL("%s needs a value", arg);
    const char *value = takes_value ? argv[++i] : NULL;
    double d;
    if (!strcmp(arg, "--kx"))
      o->kx_path = value;
    else if (!strcmp(arg, "--loader"))
      o->loader_path = value;
    else if (!strcmp(arg, "--screenshot"))
      o->screenshot_path = value;
    else if (!strcmp(arg, "--size")) {
      char tail;
      if (sscanf(value, "%dx%d%c", &o->width, &o->height, &tail) != 2 || o->width < 64 || o->height < 64 || o->width > 16384 || o->height > 16384)
        PARSE_FAIL("--size wants WIDTHxHEIGHT between 64x64 and 16384x16384, got '%s'", value);
    } else if (!strcmp(arg, "--time")) {
      if (!poptions_number(value, &d) || d < 0 || d > 3600) PARSE_FAIL("--time wants seconds from 0 to 3600, got '%s'", value);
      o->fixed_time = d;
    } else if (!strcmp(arg, "--exit-after")) {
      if (!poptions_number(value, &d) || d < 0) PARSE_FAIL("--exit-after wants seconds, got '%s'", value);
      o->exit_after = d;
    } else if (!strcmp(arg, "--play-for")) {
      if (!poptions_number(value, &d) || d < 0) PARSE_FAIL("--play-for wants seconds, got '%s'", value);
      o->play_for = d;
    } else if (!strcmp(arg, "--expect-position")) {
      if (!poptions_number(value, &d) || d < 0) PARSE_FAIL("--expect-position wants seconds, got '%s'", value);
      o->expect_position = d;
    } else if (!strcmp(arg, "--script")) {
      o->n_script = pscript_parse(value, o->script, PSCRIPT_MAX);
      if (o->n_script < 0) {
        o->n_script = 0;
        PARSE_FAIL("--script wants T:KEY,... with KEY one of space, left, right, f11, resize, fullscreen, credits, stats (at most %d events), got '%s'",
                   PSCRIPT_MAX, value);
      }
    } else if (!strcmp(arg, "--soak")) {
      if (!poptions_number(value, &d) || d < 1 || d > 1000 || d != (double)(int)d) PARSE_FAIL("--soak wants a loop count from 1 to 1000, got '%s'", value);
      o->soak_loops = (int)d;
    } else if (!strcmp(arg, "--overlay")) {
      o->overlay_credits = o->overlay_stats = 0;
      for (const char *p = value; *p;) {
        const size_t len = strcspn(p, ",");
        if (len == 7 && !strncmp(p, "credits", 7))
          o->overlay_credits = 1;
        else if (len == 5 && !strncmp(p, "stats", 5))
          o->overlay_stats = 1;
        else
          PARSE_FAIL("--overlay wants credits, stats or credits,stats, got '%s'", value);
        p += len;
        if (*p == ',' && !p[1]) PARSE_FAIL("--overlay wants credits, stats or credits,stats, got '%s'", value);
        if (*p == ',') p++;
      }
      if (!*value) PARSE_FAIL("--overlay wants credits, stats or credits,stats, got ''");
    } else if (!strcmp(arg, "--times")) {
      o->n_times = 0;
      for (const char *p = value; *p;) {
        char *end;
        const double t = strtod(p, &end);
        if (end == p || (*end && *end != ',') || !(t >= 0.0 && t <= 3600.0) || o->n_times == PTIMES_MAX || (o->n_times && t <= o->times[o->n_times - 1]) ||
            (*end == ',' && !end[1]))
          PARSE_FAIL("--times wants up to %d strictly increasing song positions from 0 to 3600 seconds separated by commas, got '%s'", PTIMES_MAX, value);
        o->times[o->n_times++] = t;
        p = *end ? end + 1 : end;
      }
      if (!o->n_times) PARSE_FAIL("--times wants up to %d strictly increasing song positions from 0 to 3600 seconds separated by commas, got ''", PTIMES_MAX);
    } else if (!strcmp(arg, "--screenshot-dir"))
      o->screenshot_dir = value;
    else if (!strcmp(arg, "--soak-limit")) {
      if (!poptions_number(value, &d) || d <= 0) PARSE_FAIL("--soak-limit wants seconds above 0, got '%s'", value);
      o->soak_limit = d;
    } else if (!strcmp(arg, "--texture-quality")) {
      if (!ptexture_quality(value, &o->texture_offset)) PARSE_FAIL("--texture-quality wants high, normal, low or a number from -3 to 0, got '%s'", value);
      o->texture_quality = value;
    } else if (!strcmp(arg, "--seek-check"))
      o->seek_check = 1;
    else if (!strcmp(arg, "--fullscreen"))
      o->fullscreen = 1;
    else if (!strcmp(arg, "--no-vsync"))
      o->vsync = 0;
    else if (!strcmp(arg, "--no-audio"))
      o->audio = 0;
    else if (!strcmp(arg, "--self-check"))
      o->self_check = 1;
    else if (!strcmp(arg, "--stats"))
      o->stats = 1;
    else if (!strcmp(arg, "--single-thread"))
      o->single_thread = 1;
    else if (!strcmp(arg, "--gl-check"))
      o->gl_check = 1;
    else if (!strcmp(arg, "--licenses") || !strcmp(arg, "--licences"))
      o->licences = 1;
    else
      PARSE_FAIL("unknown option '%s'", arg);
  }
  if (o->n_times && o->fixed_time >= 0) PARSE_FAIL("--times replaces --time: use one of them");
  if (o->n_times && (o->screenshot_path || o->seek_check))
    PARSE_FAIL("--times writes its pictures to --screenshot-dir and cannot be combined with --screenshot or --seek-check");
  if (o->n_times && !o->screenshot_dir) PARSE_FAIL("--times needs --screenshot-dir");
  if (o->screenshot_dir && !o->n_times) PARSE_FAIL("--screenshot-dir needs --times");
  if (o->n_times) o->fixed_time = o->times[0];
  if (o->fixed_time >= 0) o->audio = 0;
  if (o->soak_loops) {
    o->audio = 0;
    o->vsync = 0;
  }
  if (o->soak_loops && o->fixed_time >= 0) PARSE_FAIL("--soak plays the whole demo: it cannot be combined with --time");
  if (o->soak_limit > 0 && !o->soak_loops) PARSE_FAIL("--soak-limit needs --soak");
  if ((o->overlay_credits || o->overlay_stats) && o->fixed_time < 0) PARSE_FAIL("--overlay needs --time (the live screens have keys: C and F10)");
  if (o->seek_check && o->fixed_time <= 0) PARSE_FAIL("--seek-check needs --time with a position above 0");
  if (o->screenshot_path && o->fixed_time < 0) PARSE_FAIL("--screenshot needs --time");
  return 1;
#undef PARSE_FAIL
}

#endif /* PLAYER_ARGS_IMPLEMENTATION_DONE */
#endif /* PLAYER_ARGS_IMPLEMENTATION */
