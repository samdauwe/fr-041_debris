/* A recording OpenGL table for the glr.h tests: every call becomes one trace line in the format of tools/lib/gl_trace.mjs
 * (handles as #n in creation order, floats as f<hex32>, buffers and uniform arrays as d:<bytes>:<fnv1a>, sized bufferData as
 * z:<bytes>, uniform locations as @name), so that what glr.h asks of the driver can be compared with what the JavaScript
 * Renderer asked of the recording WebGL. Queries are answered but not traced.
 *
 * Include after glr.h in a single translation unit. The recorder keeps a call count and an FNV-1a hash over every trace line
 * (each followed by a newline), can keep the lines, stream them to a file and call a per-line hook; it counts the GL objects
 * created minus deleted and supports failure injection (compile, link, framebuffer, missing loader function).
 */
#ifndef GLR_TEST_GL_MOCK_H
#define GLR_TEST_GL_MOCK_H

#include <stdarg.h>
#include <stdio.h>

typedef struct glm_state {
  uint64_t calls;
  uint32_t hash; /* FNV-1a over every line plus '\n' */
  char *kept;    /* all lines back to back (keep != 0) */
  size_t kept_size, kept_cap;
  uint64_t kept_lines;
  int keep;
  FILE *sink; /* every line is written here when set */
  void (*hook)(void *user, const char *line, uint64_t index);
  void *hook_user;
  unsigned next_id;
  long live_objects;
  const char *names[512];
  int name_count;
  int fail_compile, fail_link, fail_fb, compiles, links, fb_checks;
  const char *missing;
  int fill_pixels; /* glReadPixels returns a pattern (byte i = i * 37 + 11) instead of zeros */
} glm_state;

static glm_state glm;

static uint32_t glm_fnv(uint32_t h, const void *data, size_t n)
{
  const uint8_t *p = (const uint8_t *)data;
  for (size_t i = 0; i < n; i++) h = (h ^ p[i]) * 0x01000193u;
  return h;
}
static uint32_t glm_f2u(float f)
{
  uint32_t u;
  memcpy(&u, &f, 4);
  return u;
}

static void glm_reset(void)
{
  free(glm.kept);
  memset(&glm, 0, sizeof glm);
  glm.hash = 0x811c9dc5u;
  glm.fail_compile = glm.fail_link = glm.fail_fb = -1;
}

static __attribute__((unused)) void glm_restart_hash(void)
{
  glm.calls = 0;
  glm.hash = 0x811c9dc5u;
}

static void glm_rec(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void glm_rec(const char *fmt, ...)
{
  char line[512];
  va_list ap;
  va_start(ap, fmt);
  const int n = vsnprintf(line, sizeof line, fmt, ap);
  va_end(ap);
  if (n < 0 || n >= (int)sizeof line) {
    fprintf(stderr, "glm: trace line too long: %.80s\n", line);
    abort();
  }
  line[n] = '\n';
  glm.hash = glm_fnv(glm.hash, line, (size_t)n + 1);
  line[n] = 0;
  if (glm.keep) {
    if (glm.kept_size + (size_t)n + 1 > glm.kept_cap) {
      glm.kept_cap = glm.kept_cap ? glm.kept_cap * 2 : 1u << 16;
      while (glm.kept_size + (size_t)n + 1 > glm.kept_cap) glm.kept_cap *= 2;
      glm.kept = (char *)realloc(glm.kept, glm.kept_cap);
    }
    memcpy(glm.kept + glm.kept_size, line, (size_t)n + 1);
    glm.kept_size += (size_t)n + 1;
    glm.kept_lines++;
  }
  if (glm.sink) {
    fputs(line, glm.sink);
    fputc('\n', glm.sink);
  }
  if (glm.hook) glm.hook(glm.hook_user, line, glm.calls);
  glm.calls++;
}

/* the i-th kept line (keep mode); NULL past the end */
static __attribute__((unused)) const char *glm_line(uint64_t index)
{
  const char *p = glm.kept;
  const char *end = glm.kept + glm.kept_size;
  for (uint64_t i = 0; p && p < end; i++) {
    if (i == index) return p;
    p += strlen(p) + 1;
  }
  return NULL;
}

#define GLM_GEN(fn, call)                        \
  static void glm_##fn(GLRsizei n, GLRuint *ids) \
  {                                              \
    for (GLRsizei i = 0; i < n; i++) {           \
      ids[i] = ++glm.next_id;                    \
      glm.live_objects++;                        \
      glm_rec(call);                             \
    }                                            \
  }
#define GLM_DEL(fn, call)                              \
  static void glm_##fn(GLRsizei n, const GLRuint *ids) \
  {                                                    \
    for (GLRsizei i = 0; i < n; i++)                   \
      if (ids[i]) {                                    \
        glm.live_objects--;                            \
        glm_rec(call " #%u", ids[i]);                  \
      }                                                \
  }
GLM_GEN(glGenVertexArrays, "createVertexArray")
GLM_GEN(glGenBuffers, "createBuffer")
GLM_GEN(glGenFramebuffers, "createFramebuffer")
GLM_GEN(glGenRenderbuffers, "createRenderbuffer")
GLM_GEN(glGenTextures, "createTexture")
GLM_DEL(glDeleteVertexArrays, "deleteVertexArray")
GLM_DEL(glDeleteBuffers, "deleteBuffer")
GLM_DEL(glDeleteFramebuffers, "deleteFramebuffer")
GLM_DEL(glDeleteRenderbuffers, "deleteRenderbuffer")
GLM_DEL(glDeleteTextures, "deleteTexture")

static GLRuint glm_glCreateShader(GLRenum type)
{
  glm_rec("createShader %u", type);
  glm.live_objects++;
  return ++glm.next_id;
}
static void glm_glShaderSource(GLRuint s, GLRsizei count, const char *const *strings, const GLRint *lengths)
{
  size_t total = 0;
  uint32_t h = 0x811c9dc5u;
  (void)lengths;
  for (GLRsizei i = 0; i < count; i++) {
    total += strlen(strings[i]);
    h = glm_fnv(h, strings[i], strlen(strings[i]));
  }
  glm_rec("shaderSource #%u s:%zu:%08x", s, total, h);
}
static void glm_glCompileShader(GLRuint s) { glm_rec("compileShader #%u", s); }
static void glm_glGetShaderiv(GLRuint s, GLRenum pname, GLRint *out)
{
  (void)s;
  (void)pname;
  *out = glm.compiles++ != glm.fail_compile;
}
static void glm_glGetShaderInfoLog(GLRuint s, GLRsizei max, GLRsizei *length, char *log)
{
  (void)s;
  (void)max;
  strcpy(log, "mock compile error");
  *length = (GLRsizei)strlen(log);
}
static void glm_glDeleteShader(GLRuint s)
{
  glm_rec("deleteShader #%u", s);
  glm.live_objects--;
}
static GLRuint glm_glCreateProgram(void)
{
  glm_rec("createProgram");
  glm.live_objects++;
  return ++glm.next_id;
}
static void glm_glAttachShader(GLRuint p, GLRuint s) { glm_rec("attachShader #%u #%u", p, s); }
static void glm_glLinkProgram(GLRuint p) { glm_rec("linkProgram #%u", p); }
static void glm_glGetProgramiv(GLRuint p, GLRenum pname, GLRint *out)
{
  (void)p;
  (void)pname;
  *out = glm.links++ != glm.fail_link;
}
static void glm_glGetProgramInfoLog(GLRuint p, GLRsizei max, GLRsizei *length, char *log)
{
  (void)p;
  (void)max;
  strcpy(log, "mock link error");
  *length = (GLRsizei)strlen(log);
}
static void glm_glDeleteProgram(GLRuint p)
{
  glm_rec("deleteProgram #%u", p);
  glm.live_objects--;
}
static GLRint glm_glGetUniformLocation(GLRuint p, const char *name)
{
  glm_rec("getUniformLocation #%u \"%s\"", p, name);
  glm.names[glm.name_count] = name;
  return glm.name_count++;
}
static const char *glm_name(GLRint l) { return l >= 0 && l < glm.name_count ? glm.names[l] : "?"; }
static void glm_glUseProgram(GLRuint p)
{
  if (p)
    glm_rec("useProgram #%u", p);
  else
    glm_rec("useProgram null");
}
static void glm_glBindVertexArray(GLRuint v)
{
  if (v)
    glm_rec("bindVertexArray #%u", v);
  else
    glm_rec("bindVertexArray null");
}
static void glm_glBindFramebuffer(GLRenum t, GLRuint f)
{
  if (f)
    glm_rec("bindFramebuffer %u #%u", t, f);
  else
    glm_rec("bindFramebuffer %u null", t);
}
static void glm_glFramebufferTexture2D(GLRenum t, GLRenum a, GLRenum tt, GLRuint tex, GLRint level)
{
  if (tex)
    glm_rec("framebufferTexture2D %u %u %u #%u %d", t, a, tt, tex, level);
  else
    glm_rec("framebufferTexture2D %u %u %u null %d", t, a, tt, level);
}
static void glm_glFramebufferRenderbuffer(GLRenum t, GLRenum a, GLRenum rt, GLRuint rb)
{
  if (rb)
    glm_rec("framebufferRenderbuffer %u %u %u #%u", t, a, rt, rb);
  else
    glm_rec("framebufferRenderbuffer %u %u %u null", t, a, rt);
}
static GLRenum glm_glCheckFramebufferStatus(GLRenum t)
{
  (void)t;
  return glm.fb_checks++ == glm.fail_fb ? 0x8CD6u : GLR_FRAMEBUFFER_COMPLETE;
}
static void glm_glBindRenderbuffer(GLRenum t, GLRuint rb)
{
  if (rb)
    glm_rec("bindRenderbuffer %u #%u", t, rb);
  else
    glm_rec("bindRenderbuffer %u null", t);
}
static void glm_glRenderbufferStorage(GLRenum t, GLRenum f, GLRsizei w, GLRsizei h) { glm_rec("renderbufferStorage %u %u %d %d", t, f, w, h); }
static void glm_glBindTexture(GLRenum t, GLRuint tex)
{
  if (tex)
    glm_rec("bindTexture %u #%u", t, tex);
  else
    glm_rec("bindTexture %u null", t);
}
static void glm_glActiveTexture(GLRenum u) { glm_rec("activeTexture %u", u); }
static void glm_glTexImage2D(GLRenum t, GLRint l, GLRint f, GLRsizei w, GLRsizei h, GLRint b, GLRenum fm, GLRenum ty, const void *data)
{
  char d[48] = "null";
  if (data) snprintf(d, sizeof d, "d:%d:%08x", w * h * 4, glm_fnv(0x811c9dc5u, data, (size_t)w * (size_t)h * 4));
  glm_rec("texImage2D %u %d %d %d %d %d %u %u %s", t, l, f, w, h, b, fm, ty, d);
}
static void glm_glTexImage3D(GLRenum t, GLRint l, GLRint f, GLRsizei w, GLRsizei h, GLRsizei dp, GLRint b, GLRenum fm, GLRenum ty, const void *data)
{
  char d[48] = "null";
  const size_t n = (size_t)w * (size_t)h * (size_t)dp * 4;
  if (data) snprintf(d, sizeof d, "d:%zu:%08x", n, glm_fnv(0x811c9dc5u, data, n));
  glm_rec("texImage3D %u %d %d %d %d %d %d %u %u %s", t, l, f, w, h, dp, b, fm, ty, d);
}
static void glm_glCompressedTexImage2D(GLRenum t, GLRint l, GLRenum f, GLRsizei w, GLRsizei h, GLRint b, GLRsizei size, const void *data)
{
  glm_rec("compressedTexImage2D %u %d %u %d %d %d d:%d:%08x", t, l, f, w, h, b, size, glm_fnv(0x811c9dc5u, data, (size_t)size));
}
static void glm_glTexParameteri(GLRenum t, GLRenum p, GLRint v) { glm_rec("texParameteri %u %u %d", t, p, v); }
static void glm_glTexParameterf(GLRenum t, GLRenum p, GLRfloat v) { glm_rec("texParameterf %u %u f%08x", t, p, glm_f2u(v)); }
static void glm_glGenerateMipmap(GLRenum t) { glm_rec("generateMipmap %u", t); }
static void glm_glPixelStorei(GLRenum p, GLRint v) { glm_rec("pixelStorei %u %d", p, v); }
static void glm_glViewport(GLRint x, GLRint y, GLRsizei w, GLRsizei h) { glm_rec("viewport %d %d %d %d", x, y, w, h); }
static void glm_glScissor(GLRint x, GLRint y, GLRsizei w, GLRsizei h) { glm_rec("scissor %d %d %d %d", x, y, w, h); }
static void glm_glEnable(GLRenum c) { glm_rec("enable %u", c); }
static void glm_glDisable(GLRenum c) { glm_rec("disable %u", c); }
static void glm_glColorMask(GLRboolean r, GLRboolean g, GLRboolean b, GLRboolean a) { glm_rec("colorMask %d %d %d %d", r != 0, g != 0, b != 0, a != 0); }
static void glm_glDepthMask(GLRboolean d) { glm_rec("depthMask %d", d != 0); }
static void glm_glStencilMask(GLRuint m) { glm_rec("stencilMask %u", m); }
static void glm_glClearColor(GLRfloat r, GLRfloat g, GLRfloat b, GLRfloat a)
{
  glm_rec("clearColor f%08x f%08x f%08x f%08x", glm_f2u(r), glm_f2u(g), glm_f2u(b), glm_f2u(a));
}
static void glm_glClearDepthf(GLRfloat d) { glm_rec("clearDepth f%08x", glm_f2u(d)); }
static void glm_glClearStencil(GLRint s) { glm_rec("clearStencil %d", s); }
static void glm_glClear(GLRuint m) { glm_rec("clear %u", m); }
static void glm_glUniform1i(GLRint l, GLRint v) { glm_rec("uniform1i @%s %d", glm_name(l), v); }
static void glm_glUniform1f(GLRint l, GLRfloat v) { glm_rec("uniform1f @%s f%08x", glm_name(l), glm_f2u(v)); }
static void glm_glUniform2f(GLRint l, GLRfloat a, GLRfloat b) { glm_rec("uniform2f @%s f%08x f%08x", glm_name(l), glm_f2u(a), glm_f2u(b)); }
static void glm_glUniform3f(GLRint l, GLRfloat a, GLRfloat b, GLRfloat c)
{
  glm_rec("uniform3f @%s f%08x f%08x f%08x", glm_name(l), glm_f2u(a), glm_f2u(b), glm_f2u(c));
}
static void glm_glUniform4f(GLRint l, GLRfloat a, GLRfloat b, GLRfloat c, GLRfloat d)
{
  glm_rec("uniform4f @%s f%08x f%08x f%08x f%08x", glm_name(l), glm_f2u(a), glm_f2u(b), glm_f2u(c), glm_f2u(d));
}
#define GLM_UNIFORM_V(fn, name, ctype, per)                                                                     \
  static void glm_##fn(GLRint l, GLRsizei n, const ctype *v)                                                    \
  {                                                                                                             \
    glm_rec(name " @%s d:%d:%08x", glm_name(l), n * (per), glm_fnv(0x811c9dc5u, v, (size_t)n * (size_t)(per))); \
  }
GLM_UNIFORM_V(glUniform1fv, "uniform1fv", GLRfloat, 4)
GLM_UNIFORM_V(glUniform3fv, "uniform3fv", GLRfloat, 12)
GLM_UNIFORM_V(glUniform4fv, "uniform4fv", GLRfloat, 16)
GLM_UNIFORM_V(glUniform1iv, "uniform1iv", GLRint, 4)
GLM_UNIFORM_V(glUniform2iv, "uniform2iv", GLRint, 8)
GLM_UNIFORM_V(glUniform4iv, "uniform4iv", GLRint, 16)
static void glm_glUniformMatrix4fv(GLRint l, GLRsizei n, GLRboolean transpose, const GLRfloat *v)
{
  glm_rec("uniformMatrix4fv @%s %d d:%d:%08x", glm_name(l), transpose != 0, n * 64, glm_fnv(0x811c9dc5u, v, (size_t)n * 64u));
}
static void glm_glDrawArrays(GLRenum m, GLRint f, GLRsizei c) { glm_rec("drawArrays %u %d %d", m, f, c); }
static void glm_glDrawElements(GLRenum m, GLRsizei c, GLRenum t, const void *o) { glm_rec("drawElements %u %d %u %d", m, c, t, (int)(uintptr_t)o); }
static void glm_glDrawElementsInstanced(GLRenum m, GLRsizei c, GLRenum t, const void *o, GLRsizei n)
{
  glm_rec("drawElementsInstanced %u %d %u %d %d", m, c, t, (int)(uintptr_t)o, n);
}
static void glm_glReadPixels(GLRint x, GLRint y, GLRsizei w, GLRsizei h, GLRenum f, GLRenum t, void *p)
{
  glm_rec("readPixels %d %d %d %d %u %u", x, y, w, h, f, t);
  if (!p) return;
  memset(p, 0, (size_t)w * (size_t)h * 4u); /* the recording WebGL leaves the destination array zero, like the JS recorder */
  if (glm.fill_pixels)
    for (size_t i = 0; i < (size_t)w * (size_t)h * 4u; i++) ((uint8_t *)p)[i] = (uint8_t)(i * 37u + 11u);
}
static void glm_glBindBuffer(GLRenum t, GLRuint b)
{
  if (b)
    glm_rec("bindBuffer %u #%u", t, b);
  else
    glm_rec("bindBuffer %u null", t);
}
static void glm_glBufferData(GLRenum t, GLRsizeiptr size, const void *data, GLRenum usage)
{
  if (data)
    glm_rec("bufferData %u d:%td:%08x %u", t, size, glm_fnv(0x811c9dc5u, data, (size_t)size), usage);
  else
    glm_rec("bufferData %u z:%td %u", t, size, usage);
}
static void glm_glBufferSubData(GLRenum t, GLRintptr offset, GLRsizeiptr size, const void *data)
{
  glm_rec("bufferSubData %u %td d:%td:%08x", t, offset, size, glm_fnv(0x811c9dc5u, data, (size_t)size));
}
static void glm_glEnableVertexAttribArray(GLRuint i) { glm_rec("enableVertexAttribArray %u", i); }
static void glm_glDisableVertexAttribArray(GLRuint i) { glm_rec("disableVertexAttribArray %u", i); }
static void glm_glVertexAttribPointer(GLRuint i, GLRint size, GLRenum type, GLRboolean norm, GLRsizei stride, const void *offset)
{
  glm_rec("vertexAttribPointer %u %d %u %d %d %d", i, size, type, norm != 0, stride, (int)(uintptr_t)offset);
}
static void glm_glVertexAttribDivisor(GLRuint i, GLRuint d) { glm_rec("vertexAttribDivisor %u %u", i, d); }
static void glm_glVertexAttrib3f(GLRuint i, GLRfloat a, GLRfloat b, GLRfloat c)
{
  glm_rec("vertexAttrib3f %u f%08x f%08x f%08x", i, glm_f2u(a), glm_f2u(b), glm_f2u(c));
}
static void glm_glVertexAttrib4f(GLRuint i, GLRfloat a, GLRfloat b, GLRfloat c, GLRfloat d)
{
  glm_rec("vertexAttrib4f %u f%08x f%08x f%08x f%08x", i, glm_f2u(a), glm_f2u(b), glm_f2u(c), glm_f2u(d));
}
static void glm_glVertexAttrib4fv(GLRuint i, const GLRfloat *v) { glm_rec("vertexAttrib4fv %u d:16:%08x", i, glm_fnv(0x811c9dc5u, v, 16)); }
static void glm_glCullFace(GLRenum m) { glm_rec("cullFace %u", m); }
static void glm_glFrontFace(GLRenum m) { glm_rec("frontFace %u", m); }
static void glm_glDepthFunc(GLRenum f) { glm_rec("depthFunc %u", f); }
static void glm_glBlendEquation(GLRenum m) { glm_rec("blendEquation %u", m); }
static void glm_glBlendFunc(GLRenum s, GLRenum d) { glm_rec("blendFunc %u %u", s, d); }
static void glm_glPolygonOffset(GLRfloat f, GLRfloat u) { glm_rec("polygonOffset f%08x f%08x", glm_f2u(f), glm_f2u(u)); }
static void glm_glStencilFunc(GLRenum f, GLRint ref, GLRuint mask) { glm_rec("stencilFunc %u %d %u", f, ref, mask); }
static void glm_glStencilOp(GLRenum a, GLRenum b, GLRenum c) { glm_rec("stencilOp %u %u %u", a, b, c); }
static void glm_glStencilOpSeparate(GLRenum f, GLRenum a, GLRenum b, GLRenum c) { glm_rec("stencilOpSeparate %u %u %u %u", f, a, b, c); }
static void glm_glBlitFramebuffer(GLRint a, GLRint b, GLRint c, GLRint d, GLRint e, GLRint f, GLRint g, GLRint h, GLRuint mask, GLRenum filter)
{
  glm_rec("blitFramebuffer %d %d %d %d %d %d %d %d %u %u", a, b, c, d, e, f, g, h, mask, filter);
}

static void *glm_loader(const char *name)
{
  if (glm.missing && !strcmp(name, glm.missing)) return NULL;
#define GLR_X(ret, fn, args) \
  if (!strcmp(name, #fn)) return (void *)glm_##fn;
  GLR_GL_FUNCTIONS(GLR_X)
#undef GLR_X
  return NULL;
}

#endif
