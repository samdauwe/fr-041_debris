/* debris_tests: runs every test suite of the project. No arguments are needed:
 *
 *     ./debris_tests
 *
 * Each suite is a function `int suite_<name>(int argc, char **argv)` in tests/suites/ that returns 0 on success,
 * DEBRIS_TEST_SKIP when it cannot run on this machine (no OpenGL driver, no player executable) and anything else on failure.
 * The suites to run, their arguments (the data directories) and limits are listed in suites.def.
 *
 * On POSIX systems every suite runs in a child process of its own, a few at a time: a crash, an exit() or a hang in one suite
 * cannot affect another, and the output of a suite is only shown when it fails. Suites marked SUITE_SERIAL measure wall-clock
 * behaviour and run alone after the others. Elsewhere the suites run in this process, one after the other.
 *
 * Optional arguments: --list (print the suites), --jobs N (parallel suites; default: the number of processors, at most 8),
 * --verbose (show the output of passing suites too), and any other argument selects the suites whose name contains it.
 * DEBRIS_TEST_TIMEOUT_SCALE (environment) multiplies the time limits, for slow machines and sanitizer builds. */

#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L
#endif

#include "support/suite.h"

#if defined(__SANITIZE_ADDRESS__)
#define DEBRIS_TEST_ASAN 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer)
#define DEBRIS_TEST_ASAN 1
#endif
#endif

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#if defined(_WIN32)
#define DEBRIS_TEST_FORK 0
#else
#define DEBRIS_TEST_FORK 1
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#define SUITE_SERIAL      1u /* runs alone: it measures wall-clock behaviour */
#define SUITE_SOFTWARE_GL 2u /* needs a deterministic OpenGL driver: Mesa llvmpipe */

typedef struct suite {
  const char *name;
  int (*run)(int argc, char **argv);
  unsigned timeout_s;
  unsigned flags;
  const char *args[4]; /* NULL-terminated */
} suite;

#ifdef DEBRIS_TEST_ASAN
/* One-time allocations inside the Mesa/EGL driver that the OpenGL suites load; they are not freed by design. */
const char *__lsan_default_suppressions(void);
const char *__lsan_default_suppressions(void) { return "leak:libEGL\nleak:libgallium\nleak:libGLX_mesa\nleak:swrast_dri\n"; }
#endif

#define ASSETS DEBRIS_ASSETS_DIR
#define DATA   DEBRIS_TEST_DATA_DIR

#define SUITE(name, fn, timeout, flags, ...) int fn(int argc, char **argv);
#include "suites.def"
#undef SUITE

#define SUITE(name, fn, timeout, flags, ...) { name, fn, timeout, flags, { __VA_ARGS__ } },
static const suite g_suites[] = {
#include "suites.def"
};
#undef SUITE

#define N_SUITES (sizeof g_suites / sizeof g_suites[0])

typedef enum outcome { OUTCOME_PASS, OUTCOME_SKIP, OUTCOME_FAIL, OUTCOME_TIMEOUT, OUTCOME_CRASH } outcome;

typedef struct result {
  outcome outcome;
  int code;     /* exit code, or the signal number for OUTCOME_CRASH */
  double seconds;
  FILE *output; /* what the suite printed; closed after reporting */
} result;

static double now_seconds(void)
{
#if defined(_WIN32)
  return (double)clock() / CLOCKS_PER_SEC;
#else
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
#endif
}

static unsigned timeout_scale(void)
{
  const char *text = getenv("DEBRIS_TEST_TIMEOUT_SCALE");
  const long v = text ? strtol(text, NULL, 10) : 1;
  return v > 0 ? (unsigned)v : 1u;
}

static int run_suite(const suite *s)
{
  char *argv[6];
  int argc = 0;
  argv[argc++] = (char *)s->name;
  for (size_t i = 0; i < 4 && s->args[i]; i++) argv[argc++] = (char *)s->args[i];
  argv[argc] = NULL;
  const int rc = s->run(argc, argv);
  fflush(stdout);
  fflush(stderr);
  return rc;
}

static void classify(result *r, int rc)
{
  r->code = rc;
  r->outcome = rc == 0 ? OUTCOME_PASS : rc == DEBRIS_TEST_SKIP ? OUTCOME_SKIP : OUTCOME_FAIL;
}

static void print_output(FILE *f)
{
  char buf[4096];
  size_t n;
  if (!f) return;
  rewind(f);
  while ((n = fread(buf, 1, sizeof buf, f)) > 0) fwrite(buf, 1, n, stdout);
}

static void report(const suite *s, const result *r, int verbose)
{
  static const char *const label[] = { " ok ", "SKIP", "FAIL", "TIME", "CRSH" };
  printf("[%s] %-28s %7.2f s", label[r->outcome], s->name, r->seconds);
  if (r->outcome == OUTCOME_FAIL) printf("  (exit %d)", r->code);
  if (r->outcome == OUTCOME_CRASH) printf("  (signal %d)", r->code);
  if (r->outcome == OUTCOME_TIMEOUT) printf("  (limit %u s)", s->timeout_s * timeout_scale());
  printf("\n");
  if (r->output && (verbose || (r->outcome != OUTCOME_PASS && r->outcome != OUTCOME_SKIP))) {
    print_output(r->output);
    printf("\n");
  }
  fflush(stdout);
}

#if DEBRIS_TEST_FORK

typedef struct running {
  pid_t pid;
  size_t index;
  double started;
  result res;
} running;

/* Child side: the suite's output goes to the temporary file; the limit is enforced by SIGALRM. */
static void child(const suite *s, FILE *out)
{
  fflush(NULL);
  if (dup2(fileno(out), STDOUT_FILENO) < 0 || dup2(fileno(out), STDERR_FILENO) < 0) _exit(127);
  alarm(s->timeout_s * timeout_scale());
  if (s->flags & SUITE_SOFTWARE_GL) {
    setenv("LIBGL_ALWAYS_SOFTWARE", "1", 1);
    setenv("GALLIUM_DRIVER", "llvmpipe", 1);
  }
  const int rc = run_suite(s);
  _exit(rc & 0xff);
}

static int start(const suite *s, size_t index, running *slot)
{
  FILE *out = tmpfile();
  if (!out) {
    perror("tmpfile");
    return -1;
  }
  fflush(NULL);
  const pid_t pid = fork();
  if (pid < 0) {
    perror("fork");
    fclose(out);
    return -1;
  }
  if (pid == 0) child(s, out);
  slot->pid = pid;
  slot->index = index;
  slot->started = now_seconds();
  slot->res.output = out;
  return 0;
}

static void finish(running *slot, int status)
{
  result *r = &slot->res;
  r->seconds = now_seconds() - slot->started;
  if (WIFEXITED(status))
    classify(r, WEXITSTATUS(status));
  else if (WIFSIGNALED(status)) {
    r->code = WTERMSIG(status);
    r->outcome = r->code == SIGALRM ? OUTCOME_TIMEOUT : OUTCOME_CRASH;
  }
}

static int run_all(const size_t *selected, size_t n, unsigned jobs, int verbose)
{
  int failures = 0, skipped = 0;
  size_t passed = 0;
  for (int phase = 0; phase < 2; phase++) {
    const unsigned limit = phase == 0 ? jobs : 1u;
    running slots[64];
    unsigned active = 0;
    size_t next = 0;
    for (;;) {
      while (active < limit && next < n) {
        const size_t index = selected[next++];
        if (((g_suites[index].flags & SUITE_SERIAL) != 0) != (phase == 1)) continue;
        if (start(&g_suites[index], index, &slots[active]) != 0) {
          failures++;
          continue;
        }
        active++;
      }
      if (!active) break;
      int status = 0;
      const pid_t pid = waitpid(-1, &status, 0);
      if (pid < 0) {
        if (errno == EINTR) continue;
        perror("waitpid");
        return 1;
      }
      for (unsigned k = 0; k < active; k++) {
        if (slots[k].pid != pid) continue;
        finish(&slots[k], status);
        report(&g_suites[slots[k].index], &slots[k].res, verbose);
        if (slots[k].res.outcome == OUTCOME_PASS)
          passed++;
        else if (slots[k].res.outcome == OUTCOME_SKIP)
          skipped++;
        else
          failures++;
        fclose(slots[k].res.output);
        slots[k] = slots[--active];
        break;
      }
    }
  }
  printf("\n%zu passed, %d skipped, %d failed\n", passed, skipped, failures);
  return failures ? 1 : 0;
}

static unsigned default_jobs(void)
{
  const long n = sysconf(_SC_NPROCESSORS_ONLN);
  return n < 1 ? 1u : n > 8 ? 8u : (unsigned)n;
}

#else /* in-process fallback */

static int run_all(const size_t *selected, size_t n, unsigned jobs, int verbose)
{
  int failures = 0, skipped = 0;
  size_t passed = 0;
  (void)jobs;
  for (size_t k = 0; k < n; k++) {
    result r = { OUTCOME_PASS, 0, 0.0, NULL };
    const double t0 = now_seconds();
    classify(&r, run_suite(&g_suites[selected[k]]));
    r.seconds = now_seconds() - t0;
    report(&g_suites[selected[k]], &r, verbose);
    if (r.outcome == OUTCOME_PASS)
      passed++;
    else if (r.outcome == OUTCOME_SKIP)
      skipped++;
    else
      failures++;
  }
  printf("\n%zu passed, %d skipped, %d failed\n", passed, skipped, failures);
  return failures ? 1 : 0;
}

static unsigned default_jobs(void) { return 1u; }

#endif

int main(int argc, char **argv)
{
  unsigned jobs = default_jobs();
  int verbose = 0, list = 0;
  const char *filters[32];
  size_t n_filters = 0;
  for (int i = 1; i < argc; i++) {
    if (!strcmp(argv[i], "--list"))
      list = 1;
    else if (!strcmp(argv[i], "--verbose"))
      verbose = 1;
    else if (!strcmp(argv[i], "--jobs") && i + 1 < argc) {
      const long v = strtol(argv[++i], NULL, 10);
      jobs = v < 1 ? 1u : v > 32 ? 32u : (unsigned)v;
    } else if (argv[i][0] != '-' && n_filters < 32)
      filters[n_filters++] = argv[i];
    else {
      fprintf(stderr, "usage: %s [--list] [--verbose] [--jobs N] [suite-name-part...]\n", argv[0]);
      return 2;
    }
  }

  size_t selected[N_SUITES], n = 0;
  for (size_t i = 0; i < N_SUITES; i++) {
    int match = n_filters == 0;
    for (size_t f = 0; f < n_filters; f++) {
      int exact = 0; /* a name that is a suite's full name selects only that suite */
      for (size_t k = 0; k < N_SUITES; k++)
        if (!strcmp(g_suites[k].name, filters[f])) exact = 1;
      if (exact ? !strcmp(g_suites[i].name, filters[f]) : strstr(g_suites[i].name, filters[f]) != NULL) match = 1;
    }
    if (match) selected[n++] = i;
  }
  if (list) {
    for (size_t k = 0; k < n; k++) printf("%s\n", g_suites[selected[k]].name);
    return 0;
  }
  if (!n) {
    fprintf(stderr, "no suite matches\n");
    return 2;
  }
  setvbuf(stdout, NULL, _IOLBF, 0);
  return run_all(selected, n, jobs, verbose);
}
