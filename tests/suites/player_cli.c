/* The player executable, started headless (SDL offscreen video driver, dummy audio) and judged by its exit status and report.
 * Which case to run is argv[1]; the cases are the table below. A case is skipped when the machine has no OpenGL ES context. */

#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "support/suite.h"

#if !defined(_WIN32) && defined(DEBRIS_PLAYER_EXE)

#include <sys/stat.h>
#include <sys/wait.h>

#define WORK DEBRIS_TEST_WORK_DIR

typedef struct cli_case {
  const char *name;
  const char *args;            /* the command line after the executable */
  int must_fail;               /* the player must exit with a non-zero status */
  int needs_gl;                /* an OpenGL ES context is created: skip when that is impossible */
  const char *const *contains; /* fragments of the output, in this order; NULL-terminated */
  int (*verify)(const char *output);
  int timing;                  /* judged against wall-clock time: skipped in a sanitizer build, where the player is several times slower */
} cli_case;

/* the fragment after `marker`, or NULL */
static const char *after(const char *text, const char *marker)
{
  const char *p = strstr(text, marker);
  return p ? p + strlen(marker) : NULL;
}

/* "screenshot: <size>, <N> lit pixels, <M> colours": the picture is not black (lit pixels and several hundred colours) */
static int verify_picture(const char *output, const char *size)
{
  char marker[64];
  snprintf(marker, sizeof marker, "screenshot: %s, ", size);
  const char *p = after(output, marker);
  unsigned lit = 0, colours = 0;
  if (!p || sscanf(p, "%u lit pixels, %u colours", &lit, &colours) != 2) {
    printf("no '%s<N> lit pixels, <M> colours' line\n", marker);
    return 0;
  }
  if (lit < 10000 || colours < 100) {
    printf("the picture looks empty: %u lit pixels, %u colours\n", lit, colours);
    return 0;
  }
  return 1;
}

static int verify_fixed_time(const char *output) { return verify_picture(output, "320x180"); }
static int verify_overlay(const char *output) { return verify_picture(output, "640x360"); }

/* the window was resized at least four times, and the GL accounting balances */
static int verify_window(const char *output)
{
  const char *p = after(output, "self-check: 0 violations, position ");
  unsigned frames = 0, resizes = 0;
  if (!p || sscanf(p, "%*f s after %u frames, %u resizes", &frames, &resizes) != 2) {
    printf("no 'self-check: 0 violations, position <s> after <N> frames, <K> resizes' line\n");
    return 0;
  }
  if (resizes < 4) {
    printf("only %u resizes were seen\n", resizes);
    return 0;
  }
  return 1;
}

static const char *const k_gl_balanced[] = { "gl-check: 0 GL errors in ", " polls; after glr_destroy: 0 live objects", NULL };
static const char *const k_licenses[] = { "fiver2 visuals",    "werkkzeug3 operator library",      "V2 synthesizer",     "GLU tessellator",
                                          "Arimo and Gelasio", "Creative Commons Attribution 3.0", "SDL3, zlib license", NULL };
static const char *const k_bad_time[] = { "debris: --time wants seconds from 0 to 3600, got 'nonsense'", NULL };
static const char *const k_missing_kx[] = { "cannot read the KX document", NULL };
static const char *const k_cancelled[] = { "load: cancelled at ", " operators", NULL };
static const char *const k_soak[] = {
  "soak: ", " of 1 loops in ", "0 GL errors, 0 violations", "gl-check: 0 GL errors in ", " polls; after glr_destroy: 0 live objects", NULL
};
#ifdef DEBRIS_TEST_SOAK
static const char *const k_soak_long[] = { "soak: ",
                                           " of 1000 loops in ",
                                           "(stopped by --soak-limit)",
                                           "0 GL errors, 0 violations",
                                           "gl-check: 0 GL errors in ",
                                           " polls; after glr_destroy: 0 live objects",
                                           NULL };
#endif

static const cli_case k_cases[] = {
  /* credits and licences: nothing to load, no window */
  { "licenses", "--licenses", 0, 0, k_licenses, NULL },
  /* the command line reports errors; it does not crash */
  { "bad_args", "--time nonsense", 1, 0, k_bad_time, NULL },
  { "missing_kx", "--kx \"" WORK "/does-not-exist.kx\" --no-audio --time 1", 1, 0, k_missing_kx, NULL },
  /* restoring a snapshot and replaying reproduces linear playback on the real document */
  { "seek_replay", "--time 120 --seek-check --size 320x180", 0, 1, NULL, NULL },
  /* the golden-capture path draws the real scene */
  { "fixed_time", "--time 30 --size 320x180 --screenshot \"" WORK "/debris_fixed_time.ppm\"", 0, 1, NULL, verify_fixed_time },
  /* the GL accounting on the golden path: nothing is left once the renderer is destroyed */
  { "gl_check", "--gl-check --time 60 --size 320x180 --texture-quality -2", 0, 1, k_gl_balanced, NULL },
  /* the credits and the statistics screens change the pixels a plain frame has */
  { "overlay", "--time 100 --size 640x360 --texture-quality -2 --overlay credits,stats --screenshot \"" WORK "/debris_overlay.ppm\"", 0, 1, NULL,
    verify_overlay },
  /* deterministic stepping through the demo with seeks, resizes and screens */
  { "soak_short", "--soak 1 --soak-limit 25 --size 320x180 --texture-quality -2", 0, 1, k_soak, NULL },
  /* quitting while the runtime is still loading cancels the worker at its next operator */
  { "cancel_loading", "--self-check --no-audio --no-vsync --exit-after 0.05", 0, 1, k_cancelled, NULL },
  /* audio clock, pause (rewinds the audio), forward and backward seek through the real SDL audio pipeline; the script times
     count from the party start, so the load time does not matter */
  { "live_session", "--self-check --no-vsync --play-for 7 --script 1.5:space,2.5:space,3:right,4:left --expect-position 5.6", 0, 1, NULL, NULL, 1 },
  { "live_no_audio", "--self-check --no-audio --no-vsync --play-for 4 --script 1:space,2:space --expect-position 3", 0, 1, NULL, NULL, 1 },
  /* the window: other shapes (the 2:1 picture stays centred), statistics and credits, fullscreen where the driver allows it;
     the GL objects balance and glGetError stays clear after every frame */
  { "window",
    "--self-check --gl-check --no-audio --no-vsync --size 320x180 --texture-quality -2 --play-for 7 "
    "--script 1:resize,1.5:resize,2:resize,2.5:resize,3:stats,3.5:credits,4:fullscreen,5:fullscreen,6:f11,6.5:stats",
    0, 1, k_gl_balanced, verify_window, 1 },
#ifdef DEBRIS_TEST_SOAK
  { "soak_long", "--soak 1000 --soak-limit 600 --size 320x180 --texture-quality -2", 0, 1, k_soak_long, NULL },
#endif
};

static char *run(const char *command, int *exit_status)
{
  FILE *p = popen(command, "r");
  if (!p) return NULL;
  size_t size = 0, cap = 1 << 16;
  char *buf = (char *)malloc(cap);
  size_t n;
  while (buf && (n = fread(buf + size, 1, cap - size - 1, p)) > 0) {
    size += n;
    if (cap - size < 4096) {
      char *bigger = (char *)realloc(buf, cap * 2);
      if (!bigger) {
        free(buf);
        buf = NULL;
        break;
      }
      buf = bigger;
      cap *= 2;
    }
  }
  const int status = pclose(p);
  if (buf) buf[size] = 0;
  *exit_status = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
  return buf;
}

int suite_player_cli(int argc, char **argv)
{
  if (argc < 2) {
    printf("usage: player_cli <case>\n");
    return 2;
  }
  const cli_case *c = NULL;
  for (size_t i = 0; i < sizeof k_cases / sizeof k_cases[0]; i++)
    if (!strcmp(k_cases[i].name, argv[1])) c = &k_cases[i];
  if (!c) {
    printf("unknown case %s\n", argv[1]);
    return 2;
  }

#ifdef DEBRIS_TEST_SLOW_BUILD
  if (c->timing) {
    printf("judged against wall-clock time: skipped in a sanitizer build\n");
    return DEBRIS_TEST_SKIP;
  }
#endif

  mkdir(WORK, 0777);
  setenv("SDL_VIDEODRIVER", "offscreen", 1);
  setenv("SDL_AUDIODRIVER", "dummy", 1);
#ifdef DEBRIS_TEST_LSAN_SUPP
  setenv("LSAN_OPTIONS", "suppressions=" DEBRIS_TEST_LSAN_SUPP, 1);
#endif

  char command[2048];
  snprintf(command, sizeof command, "\"%s\" %s 2>&1", DEBRIS_PLAYER_EXE, c->args);
  int status = 0;
  char *output = run(command, &status);
  if (!output) {
    printf("cannot run %s\n", command);
    return 1;
  }

  int rc = 0;
  if (c->needs_gl && status != 0 && (strstr(output, "OpenGL context") || strstr(output, "window:"))) {
    printf("no OpenGL ES context: %s\n", output);
    rc = DEBRIS_TEST_SKIP;
  } else {
    if (c->must_fail ? status == 0 : status != 0) {
      printf("exit status %d\n", status);
      rc = 1;
    }
    const char *p = output;
    for (size_t i = 0; rc == 0 && c->contains && c->contains[i]; i++) {
      const char *hit = strstr(p, c->contains[i]);
      if (!hit) {
        printf("the output lacks '%s'\n", c->contains[i]);
        rc = 1;
      } else
        p = hit + strlen(c->contains[i]);
    }
    if (rc == 0 && c->verify && !c->verify(output)) rc = 1;
  }
  if (rc == 1) printf("--- %s\n%s\n", command, output);
  free(output);
  return rc;
}

#else

int suite_player_cli(int argc, char **argv)
{
  (void)argc;
  (void)argv;
  printf("the player executable is not available\n");
  return DEBRIS_TEST_SKIP;
}

#endif
