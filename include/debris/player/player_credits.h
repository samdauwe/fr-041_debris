/* player_credits.h: the credits of the production and the notices that come with the player, SDL-free.
 *
 *   #define PLAYER_CREDITS_IMPLEMENTATION in exactly one source file.
 *
 * The credits are those of the launcher of the browser port (index.html) and of README.md. The licence texts are embedded in the player
 * by the build (embed_licenses.cmake): the notices of the vendored sources the player is built from, the Creative Commons Attribution 3.0
 * notice of the production data, and the SDL3 notice. `debris --licenses` prints all of it.
 */
#ifndef PLAYER_CREDITS_H
#define PLAYER_CREDITS_H

#include <stddef.h>
#include <stdio.h>

#ifndef PLAYER_CREDITS_API
#ifdef PLAYER_CREDITS_STATIC
#define PLAYER_CREDITS_API static
#else
#define PLAYER_CREDITS_API extern
#endif
#endif

typedef struct plicense {
  const char *title; /* what the notice covers */
  const char *text;  /* the notice itself */
} plicense;

PLAYER_CREDITS_API const char *pcredits_title(void);    /* "fr-041  debris." */
PLAYER_CREDITS_API const char *pcredits_subtitle(void); /* the group, size and party */
PLAYER_CREDITS_API const char *pcredits_roles(void);    /* the credits, one line per three roles */
/* The complete text of `debris --licenses`: title, credits, then every licence under a ruled heading. Returns 0 if a write failed. */
PLAYER_CREDITS_API int pcredits_print(FILE *out, const plicense *licenses, size_t n_licenses);

#endif /* PLAYER_CREDITS_H */

#ifdef PLAYER_CREDITS_IMPLEMENTATION
#ifndef PLAYER_CREDITS_IMPLEMENTATION_DONE
#define PLAYER_CREDITS_IMPLEMENTATION_DONE

#include <string.h>

PLAYER_CREDITS_API const char *pcredits_title(void) { return "fr-041  debris."; }
PLAYER_CREDITS_API const char *pcredits_subtitle(void) { return "farbrausch - 177kb demo - breakpoint 2007"; }
PLAYER_CREDITS_API const char *pcredits_roles(void)
{
  return "fiver2 visuals & direction - chaos / ryg code - ronny soundtrack\n"
         "kb synthesizer - wayfinder sound effects - giZMo / tron / fried";
}

PLAYER_CREDITS_API int pcredits_print(FILE *out, const plicense *licenses, size_t n_licenses)
{
  int ok = fprintf(out, "%s\n%s\n\n%s\n\n", pcredits_title(), pcredits_subtitle(), pcredits_roles()) > 0;
  ok = ok && fprintf(out, "Source-level C99 / SDL3 / OpenGL port of fr-041: debris. by Farbrausch (Breakpoint 2007).\n"
                          "The production data is released by Farbrausch under CC BY 3.0. Notices follow.\n") > 0;
  for (size_t i = 0; ok && i < n_licenses; i++) {
    const size_t n = strlen(licenses[i].text);
    ok = fprintf(out,
                 "\n================================================================================\n%s\n"
                 "================================================================================\n%s%s",
                 licenses[i].title, licenses[i].text, n && licenses[i].text[n - 1] != '\n' ? "\n" : "") > 0;
  }
  return ok && fflush(out) == 0;
}

#endif /* PLAYER_CREDITS_IMPLEMENTATION_DONE */
#endif /* PLAYER_CREDITS_IMPLEMENTATION */
