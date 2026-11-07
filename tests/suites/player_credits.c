/* Checks for player_credits.h and the licences the build embeds in the player.
 *
 *   test_player_credits
 *
 * The credits name everyone of the production, `--licenses` prints the notice of every source the player is built from (werkkzeug3,
 * V2, the GLU tessellator, the Arimo outlines, the CC BY 3.0 data, SDL3), each licence text is the real one (hand-checked markers),
 * and a failing write is reported.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PLAYER_CREDITS_IMPLEMENTATION
#include <debris/player/player_credits.h>
#include "licenses_data.h"

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

int suite_player_credits(int argc, char **argv)
{
  (void)argc;
  (void)argv;
  static const char *const people[] = { "fiver2", "chaos / ryg", "ronny", "kb", "wayfinder", "giZMo", "tron", "fried", "farbrausch", "breakpoint 2007" };
  char all[512];
  snprintf(all, sizeof all, "%s\n%s\n%s", pcredits_title(), pcredits_subtitle(), pcredits_roles());
  for (size_t i = 0; i < sizeof people / sizeof people[0]; i++) CHECK(strstr(all, people[i]), "the credits do not name '%s'", people[i]);
  CHECK(strstr(pcredits_title(), "debris") && strstr(pcredits_title(), "041"), "title");
  for (const char *p = all; *p; p++)
    CHECK((unsigned char)*p == '\n' || ((unsigned char)*p >= 32 && (unsigned char)*p < 127), "credits stay in printable ASCII (the text rasterizer's range)");

  /* the embedded notices, in order, each the real text */
  static const struct {
    const char *title_has, *marker;
    size_t min_bytes;
  } expected[] = {
    { "werkkzeug3", "Dierk Ohlerich", 1000 },
    { "V2", "public", 1000 },
    { "tessellator", "SGI FREE SOFTWARE LICENSE B", 1000 },
    { "Arimo", "SIL OPEN FONT LICENSE", 3000 },
    { "Creative Commons", "Attribution 3.0", 500 },
    { "SDL3", "provided 'as-is'", 500 },
  };
  CHECK(player_n_licenses == sizeof expected / sizeof expected[0], "%zu embedded notices", player_n_licenses);
  for (size_t i = 0; i < player_n_licenses && i < sizeof expected / sizeof expected[0]; i++) {
    const size_t n = strlen(player_licenses[i].text);
    CHECK(strstr(player_licenses[i].title, expected[i].title_has), "notice %zu is titled '%s'", i, player_licenses[i].title);
    CHECK(n >= expected[i].min_bytes && strstr(player_licenses[i].text, expected[i].marker), "notice %zu ('%s') has %zu bytes and no '%s'", i,
          player_licenses[i].title, n, expected[i].marker);
  }
  CHECK(strstr(player_licenses[3].text, "Arimo") && strstr(player_licenses[3].text, "Gelasio"), "the font notice names both fonts");
  CHECK(strstr(player_licenses[0].text, "THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS"), "the BSD text is complete");
  CHECK(strstr(player_licenses[4].text, "creativecommons.org/licenses/by/3.0"), "the CC BY notice links its licence");

  /* what --licenses prints */
  FILE *f = tmpfile();
  CHECK(f != NULL, "tmpfile");
  if (f) {
    CHECK(pcredits_print(f, player_licenses, player_n_licenses) == 1, "print");
    const long size = ftell(f);
    rewind(f);
    char *text = (char *)calloc((size_t)size + 1, 1);
    CHECK(text && fread(text, 1, (size_t)size, f) == (size_t)size, "read back");
    if (text) {
      CHECK(strstr(text, pcredits_title()) == text, "starts with the title");
      for (size_t i = 0; i < sizeof people / sizeof people[0]; i++) CHECK(strstr(text, people[i]), "the printout lacks '%s'", people[i]);
      size_t total = 0;
      for (size_t i = 0; i < player_n_licenses; i++) {
        total += strlen(player_licenses[i].text);
        CHECK(strstr(text, player_licenses[i].title) && strstr(text, player_licenses[i].text), "the printout lacks notice %zu", i);
      }
      CHECK((size_t)size > total && text[size - 1] == '\n', "the printout ends with a newline after %zu bytes of notices", total);
      free(text);
    }
    fclose(f);
  }
  /* a failing write is reported; a notice without a final newline gets one */
  {
    static const plicense odd[] = { { "x", "no newline" }, { "y", "" } };
    FILE *g = tmpfile();
    CHECK(g && pcredits_print(g, odd, 2) == 1, "odd notices");
    if (g) {
      rewind(g);
      char buf[2048] = { 0 };
      const size_t got = fread(buf, 1, sizeof buf - 1, g);
      CHECK(got > 0 && strstr(buf, "no newline\n") != NULL, "a final newline is added");
      fclose(g);
    }
    FILE *ro = fopen("/dev/full", "w");
    if (ro) {
      CHECK(pcredits_print(ro, player_licenses, player_n_licenses) == 0, "a full device is reported");
      fclose(ro);
    }
  }
  {
    FILE *none = tmpfile();
    CHECK(none && pcredits_print(none, NULL, 0) == 1, "no notices");
    if (none) fclose(none);
  }
  printf("player_credits: %d checks, %d failures\n", g_checks, g_failures);
  return g_failures ? 1 : 0;
}
