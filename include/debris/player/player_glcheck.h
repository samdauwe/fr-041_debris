/* player_glcheck.h: GL object accounting and error bookkeeping of the player's soak, SDL-free so that it can be tested headless.
 *
 *   #define PLAYER_GLCHECK_IMPLEMENTATION in exactly one source file.
 *
 * The player (--gl-check) puts counting wrappers between glr and the driver for glGen* / glDelete* / glCreate* / glDelete{Program,Shader}
 * and polls glGetError after every frame; this file is the bookkeeping behind them: live objects per kind (created minus deleted, id 0 is
 * ignored like GL does), the GL errors seen, and the comparison of two snapshots that the soak uses to prove the objects stop growing.
 * The count is independent of glr's own bookkeeping: it is what the driver was asked.
 */
#ifndef PLAYER_GLCHECK_H
#define PLAYER_GLCHECK_H

#include <stddef.h>
#include <stdint.h>

#ifndef PLAYER_GLCHECK_API
#ifdef PLAYER_GLCHECK_STATIC
#define PLAYER_GLCHECK_API static
#else
#define PLAYER_GLCHECK_API extern
#endif
#endif

typedef enum pgl_kind { PGL_TEXTURE = 0, PGL_BUFFER, PGL_VERTEX_ARRAY, PGL_FRAMEBUFFER, PGL_RENDERBUFFER, PGL_PROGRAM, PGL_SHADER, PGL_KINDS } pgl_kind;

typedef struct pglcheck {
  int64_t live[PGL_KINDS];
  uint64_t created[PGL_KINDS], deleted[PGL_KINDS];
  uint64_t over_deleted;            /* deletions beyond what was created: a double delete or a foreign object */
  uint64_t polls, errors;           /* glGetError polls, and the errors they returned */
  uint32_t first_error, last_error; /* GL error codes (0: none yet) */
  uint64_t first_error_poll;        /* the poll that saw the first one */
} pglcheck;

PLAYER_GLCHECK_API void pglcheck_init(pglcheck *c);
/* n objects were generated (ids may be NULL: counted as they are, zero ids in the array are skipped). */
PLAYER_GLCHECK_API void pglcheck_created(pglcheck *c, pgl_kind kind, int32_t n, const uint32_t *ids);
PLAYER_GLCHECK_API void pglcheck_deleted(pglcheck *c, pgl_kind kind, int32_t n, const uint32_t *ids);
/* One poll's worth of errors: codes[0..n) as glGetError returned them (0 entries are ignored). */
PLAYER_GLCHECK_API void pglcheck_polled(pglcheck *c, const uint32_t *codes, int32_t n);
PLAYER_GLCHECK_API int64_t pglcheck_total(const pglcheck *c);
/* 1 when every live count of `a` equals that of `b`. */
PLAYER_GLCHECK_API int pglcheck_same_objects(const pglcheck *a, const pglcheck *b);
PLAYER_GLCHECK_API const char *pglcheck_kind_name(pgl_kind kind);
PLAYER_GLCHECK_API const char *pglcheck_error_name(uint32_t code);
/* "textures 256 buffers 2310 ... errors 0" (always terminated; truncated to the buffer). */
PLAYER_GLCHECK_API void pglcheck_format(const pglcheck *c, char *out, size_t size);

#endif /* PLAYER_GLCHECK_H */

#ifdef PLAYER_GLCHECK_IMPLEMENTATION
#ifndef PLAYER_GLCHECK_IMPLEMENTATION_DONE
#define PLAYER_GLCHECK_IMPLEMENTATION_DONE

#include <stdio.h>
#include <string.h>

PLAYER_GLCHECK_API void pglcheck_init(pglcheck *c)
{
  if (c) memset(c, 0, sizeof *c);
}

static int32_t pglcheck__count(int32_t n, const uint32_t *ids)
{
  if (n <= 0) return 0;
  if (!ids) return n;
  int32_t k = 0;
  for (int32_t i = 0; i < n; i++) k += ids[i] != 0;
  return k;
}

PLAYER_GLCHECK_API void pglcheck_created(pglcheck *c, pgl_kind kind, int32_t n, const uint32_t *ids)
{
  if (!c || (unsigned)kind >= PGL_KINDS) return;
  const int32_t k = pglcheck__count(n, ids);
  c->created[kind] += (uint64_t)k;
  c->live[kind] += k;
}

PLAYER_GLCHECK_API void pglcheck_deleted(pglcheck *c, pgl_kind kind, int32_t n, const uint32_t *ids)
{
  if (!c || (unsigned)kind >= PGL_KINDS) return;
  const int32_t k = pglcheck__count(n, ids);
  c->deleted[kind] += (uint64_t)k;
  c->live[kind] -= k;
  if (c->live[kind] < 0) {
    c->over_deleted += (uint64_t)(-c->live[kind]);
    c->live[kind] = 0;
  }
}

PLAYER_GLCHECK_API void pglcheck_polled(pglcheck *c, const uint32_t *codes, int32_t n)
{
  if (!c) return;
  c->polls++;
  for (int32_t i = 0; codes && i < n; i++) {
    if (!codes[i]) continue;
    if (!c->errors) {
      c->first_error = codes[i];
      c->first_error_poll = c->polls;
    }
    c->errors++;
    c->last_error = codes[i];
  }
}

PLAYER_GLCHECK_API int64_t pglcheck_total(const pglcheck *c)
{
  int64_t total = 0;
  if (c)
    for (int k = 0; k < PGL_KINDS; k++) total += c->live[k];
  return total;
}

PLAYER_GLCHECK_API int pglcheck_same_objects(const pglcheck *a, const pglcheck *b)
{
  if (!a || !b) return 0;
  for (int k = 0; k < PGL_KINDS; k++)
    if (a->live[k] != b->live[k]) return 0;
  return 1;
}

PLAYER_GLCHECK_API const char *pglcheck_kind_name(pgl_kind kind)
{
  static const char *const names[PGL_KINDS] = { "textures", "buffers", "vertex arrays", "framebuffers", "renderbuffers", "programs", "shaders" };
  return (unsigned)kind < PGL_KINDS ? names[kind] : "?";
}

PLAYER_GLCHECK_API const char *pglcheck_error_name(uint32_t code)
{
  switch (code) {
  case 0x0000: return "GL_NO_ERROR";
  case 0x0500: return "GL_INVALID_ENUM";
  case 0x0501: return "GL_INVALID_VALUE";
  case 0x0502: return "GL_INVALID_OPERATION";
  case 0x0503: return "GL_STACK_OVERFLOW";
  case 0x0504: return "GL_STACK_UNDERFLOW";
  case 0x0505: return "GL_OUT_OF_MEMORY";
  case 0x0506: return "GL_INVALID_FRAMEBUFFER_OPERATION";
  case 0x0507: return "GL_CONTEXT_LOST";
  default: return "unknown GL error";
  }
}

PLAYER_GLCHECK_API void pglcheck_format(const pglcheck *c, char *out, size_t size)
{
  if (!out || !size) return;
  out[0] = 0;
  if (!c) return;
  size_t used = 0;
  for (int k = 0; k < PGL_KINDS && used + 1 < size; k++) {
    const int n = snprintf(out + used, size - used, "%s%s %lld", k ? ", " : "", pglcheck_kind_name((pgl_kind)k), (long long)c->live[k]);
    if (n < 0) break;
    used += (size_t)n < size - used ? (size_t)n : size - used - 1;
  }
  if (used + 1 < size)
    snprintf(out + used, size - used, "; %llu GL errors in %llu polls, %llu over-deleted", (unsigned long long)c->errors, (unsigned long long)c->polls,
             (unsigned long long)c->over_deleted);
}

#endif /* PLAYER_GLCHECK_IMPLEMENTATION_DONE */
#endif /* PLAYER_GLCHECK_IMPLEMENTATION */
