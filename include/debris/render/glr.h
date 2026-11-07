/* glr.h - OpenGL executor of the werkkzeug3 frame jobs - single-header C99 library (stb style)

   Draws what wz3.h computes. The renderer takes the semantic frame jobs of wz3.h (wz3_frame) and executes them: materials,
   lights and shadow volumes, the intermediate render targets and post-processing passes, and the letterboxed presentation of
   the master target. It talks to OpenGL 3.3 core or OpenGL ES 3.0 only through a function table that the host fills with its
   loader (glr_loader_fn), and it owns no window or context. The GLSL is the text of the JavaScript reference renderer (see
   doc/REFERENCE.md), so a driver compiles the same shaders as in the browser.

   USAGE
     In exactly one translation unit:

         #define GLR_IMPLEMENTATION
         #include <debris/render/glr.h>

   CONFIGURATION (before including the implementation)
     GLR_STATIC    make every API function static
*/
#ifndef GLR_H_INCLUDED
#define GLR_H_INCLUDED

#ifndef WZ3_H_INCLUDED
#include <debris/core/wz3.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

#ifndef GLR_API
#ifdef GLR_STATIC
#define GLR_API static
#else
#define GLR_API extern
#endif
#endif

typedef unsigned int GLRenum;
typedef unsigned int GLRuint;
typedef int GLRint;
typedef int GLRsizei;
typedef unsigned char GLRboolean;
typedef float GLRfloat;
typedef ptrdiff_t GLRsizeiptr;
typedef ptrdiff_t GLRintptr;

typedef enum glr_result {
  GLR_OK = 0,
  GLR_ERR_ARGUMENT,
  GLR_ERR_OUT_OF_MEMORY,
  GLR_ERR_LOADER, /* a required GL function is missing; glr_last_error names it */
  GLR_ERR_SHADER, /* compile failure; glr_last_error has the driver's log */
  GLR_ERR_LINK,
  GLR_ERR_FRAMEBUFFER
} glr_result;

typedef void *(*glr_loader_fn)(const char *name);

typedef struct glr_options {
  int no_shadows;        /* skip the shadow volumes (the JS option shadows: false); zero keeps them, like the JS default */
  int s3tc;              /* the device has WEBGL_compressed_texture_s3tc: BC3 textures are uploaded compressed */
  double max_anisotropy; /* EXT_texture_filter_anisotropic's MAX_TEXTURE_MAX_ANISOTROPY; 0 when the extension is missing */
  char *error;           /* optional: receives the failure text of glr_create (compiler log, missing function, ...) */
  size_t error_size;
} glr_options;

typedef struct glr_stats {
  uint32_t draw_calls, triangles, outputs, unsupported_nodes;
  wz3_draw_stats culling;
  int32_t width, height, render_width, render_height;
  wz3_region presentation;
} glr_stats;

typedef struct glr glr;

/* What a frame draws besides the frame jobs: the runtime (frame jobs, materials, deferred effects) and the data the jobs name by
   operator. Everything a callback returns must stay valid until the next glr_render_frame. */
typedef struct glr_host {
  void *user;
  wz3_rt *rt;
  uint32_t n_ops;
  /* The prepared geometry of Mesh / MinMesh operator `op`. For an animated MinMesh (wz3_geometry.animated of its static
     geometry, asked with time < 0) the renderer then asks again with the pose time and a scratch slot number: the pose
     of `time`, one allocation per slot. NULL draws nothing. */
  const wz3_geometry *(*geometry)(void *user, uint32_t op, double time, int32_t slot);
  /* The shadow data (vertex map, masks, recipe) of a static geometry, NULL when it has none. */
  const wz3_shadow_data *(*shadow_data)(void *user, uint32_t op);
  /* The pixels of a bitmap operator, NULL when it has none (the fallback texture is bound instead). */
  const wz3_bitmap *(*bitmap)(void *user, uint32_t op);
  /* Optional: called once the pixels of bitmap operator `op` are in a GL texture. From then on the renderer keeps that texture and never
     asks for the operator's bitmap again, so the host may free its copy (the JavaScript port keeps it, which is why the frame tests
     leave this NULL). */
  void (*bitmap_uploaded)(void *user, uint32_t op);
} glr_host;

/* The context must be current. *res (optional) receives the result; on failure NULL is returned. */
GLR_API glr *glr_create(glr_loader_fn load, const glr_options *options, glr_result *res);
GLR_API void glr_destroy(glr *r);
GLR_API const char *glr_last_error(const glr *r);
/* Size of the drawable in pixels; presentation_aspect > 0 letterboxes the master target to that aspect (2 for debris). */
GLR_API glr_result glr_resize(glr *r, int32_t width, int32_t height, double presentation_aspect);
GLR_API glr_result glr_render(glr *r, const wz3_frame *frame);
/* Renderer.render for the last frame of host->rt: draws every IPP root into the master target and presents it. */
GLR_API glr_result glr_render_frame(glr *r, const glr_host *host);
/* The renderer side of Bitmap_Render: the frame jobs of operator `input` (the Render operator's first input,
   wz3_bitmap_render_request.input) in a frame of their own (wz3_rt_render_begin), drawn into a width x height drawable and read back as a
   bitmap the caller owns (wz3_bitmap_destroy); a Render without output yields a blank bitmap. The drawable is left at that size: the next
   glr_resize / glr_render_frame restores the presentation. Not during a glr_render_frame. */
GLR_API glr_result glr_render_bitmap(glr *r, const glr_host *host, uint32_t input, int32_t width, int32_t height, wz3_bitmap **out);
/* Warm-up (no JavaScript counterpart; the JS port uploads on first use): creates the GL texture of bitmap operator `op`, or the buffers of the
   static geometry of Mesh / MinMesh operator `op`, ahead of the first frame that uses it, through the same cache a frame uses, so the frame that
   does use it finds a hit. Animated MinMeshes are posed per frame and need no warm-up (GLR_OK, nothing done). GLR_ERR_ARGUMENT for an operator
   outside host->n_ops or a host without the callback; GLR_ERR_OUT_OF_MEMORY when an allocation failed (nothing was cached, try again).
   Not during a glr_render_frame. */
GLR_API glr_result glr_warm_texture(glr *r, const glr_host *host, uint32_t op);
GLR_API glr_result glr_warm_geometry(glr *r, const glr_host *host, uint32_t op);
/* Draws `texture` (a 2D RGBA texture) letterboxed into the default framebuffer. */
GLR_API void glr_present(glr *r, GLRuint texture);
/* Blends width x height pixels (premultiplied RGBA, top row first) over the default framebuffer with their top-left corner at (x, y) pixels
   from the top-left of the drawable; what falls outside the drawable is clipped. For credits and statistics on top of a presented frame or the
   loader screen; no JavaScript counterpart. GLR_ERR_ARGUMENT for no pixels or a side above 8192, GLR_ERR_OUT_OF_MEMORY when the copy failed. */
GLR_API glr_result glr_present_overlay(glr *r, const uint8_t *rgba, int32_t width, int32_t height, int32_t x, int32_t y);
/* Loader screen: a dark track with a white marquee segment at phase (0..1) of its travel; no JS counterpart. */
GLR_API void glr_present_loading(glr *r, double phase);
/* Loader screen with a determinate bar: the track filled to `fraction` (0..1; NaN counts as 0). */
GLR_API void glr_present_progress(glr *r, double fraction);
GLR_API GLRuint glr_master_texture(const glr *r);
/* Reads the default framebuffer, top row first, 4 bytes per pixel. Returns 0 on failure. */
GLR_API int glr_read_pixels(glr *r, uint8_t *rgba, int32_t width, int32_t height);
GLR_API void glr_get_stats(const glr *r, glr_stats *out);

#ifdef __cplusplus
}
#endif

#endif /* GLR_H_INCLUDED */

#ifdef GLR_IMPLEMENTATION
#ifndef GLR_IMPLEMENTATION_DONE
#define GLR_IMPLEMENTATION_DONE

/* BEGIN GENERATED by tools/sync_glr_shaders.mjs from the reference renderer and tools/lib/gl_constants.mjs (see doc/REFERENCE.md) */
#define GLR_FALSE                       0x0000
#define GLR_TRUE                        0x0001
#define GLR_NO_ERROR                    0x0000
#define GLR_ZERO                        0x0000
#define GLR_ONE                         0x0001
#define GLR_TRIANGLES                   0x0004
#define GLR_NEVER                       0x0200
#define GLR_LESS                        0x0201
#define GLR_EQUAL                       0x0202
#define GLR_LEQUAL                      0x0203
#define GLR_ALWAYS                      0x0207
#define GLR_DST_ALPHA                   0x0304
#define GLR_ONE_MINUS_DST_COLOR         0x0307
#define GLR_FRONT                       0x0404
#define GLR_BACK                        0x0405
#define GLR_CW                          0x0900
#define GLR_BYTE                        0x1400
#define GLR_UNSIGNED_SHORT              0x1403
#define GLR_UNSIGNED_INT                0x1405
#define GLR_FLOAT                       0x1406
#define GLR_KEEP                        0x1E00
#define GLR_INCR_WRAP                   0x8507
#define GLR_DECR_WRAP                   0x8508
#define GLR_NEAREST_MIPMAP_NEAREST      0x2700
#define GLR_LINEAR_MIPMAP_NEAREST       0x2701
#define GLR_LINEAR_MIPMAP_LINEAR        0x2703
#define GLR_FUNC_REVERSE_SUBTRACT       0x800B
#define GLR_ARRAY_BUFFER                0x8892
#define GLR_ELEMENT_ARRAY_BUFFER        0x8893
#define GLR_STATIC_DRAW                 0x88E4
#define GLR_DYNAMIC_DRAW                0x88E8
#define GLR_RGBA8_SNORM                 0x8F97
#define GLR_TEXTURE1                    0x84C1
#define GLR_TEXTURE4                    0x84C4
#define GLR_DEPTH_BUFFER_BIT            0x0100
#define GLR_STENCIL_BUFFER_BIT          0x0400
#define GLR_COLOR_BUFFER_BIT            0x4000
#define GLR_SRC_COLOR                   0x0300
#define GLR_ONE_MINUS_SRC_COLOR         0x0301
#define GLR_SRC_ALPHA                   0x0302
#define GLR_ONE_MINUS_SRC_ALPHA         0x0303
#define GLR_DST_COLOR                   0x0306
#define GLR_FUNC_ADD                    0x8006
#define GLR_CULL_FACE                   0x0B44
#define GLR_DEPTH_TEST                  0x0B71
#define GLR_STENCIL_TEST                0x0B90
#define GLR_BLEND                       0x0BE2
#define GLR_SCISSOR_TEST                0x0C11
#define GLR_POLYGON_OFFSET_FILL         0x8037
#define GLR_UNPACK_ALIGNMENT            0x0CF5
#define GLR_UNSIGNED_BYTE               0x1401
#define GLR_RGBA                        0x1908
#define GLR_TEXTURE_2D                  0x0DE1
#define GLR_TEXTURE_3D                  0x806F
#define GLR_TEXTURE_CUBE_MAP            0x8513
#define GLR_TEXTURE_CUBE_MAP_POSITIVE_X 0x8515
#define GLR_TEXTURE0                    0x84C0
#define GLR_NEAREST                     0x2600
#define GLR_LINEAR                      0x2601
#define GLR_TEXTURE_MAG_FILTER          0x2800
#define GLR_TEXTURE_MIN_FILTER          0x2801
#define GLR_TEXTURE_WRAP_S              0x2802
#define GLR_TEXTURE_WRAP_T              0x2803
#define GLR_TEXTURE_WRAP_R              0x8072
#define GLR_TEXTURE_BASE_LEVEL          0x813C
#define GLR_TEXTURE_MAX_LEVEL           0x813D
#define GLR_REPEAT                      0x2901
#define GLR_CLAMP_TO_EDGE               0x812F
#define GLR_RGBA8                       0x8058
#define GLR_DEPTH_COMPONENT24           0x81A6
#define GLR_DEPTH24_STENCIL8            0x88F0
#define GLR_FRAGMENT_SHADER             0x8B30
#define GLR_VERTEX_SHADER               0x8B31
#define GLR_COMPILE_STATUS              0x8B81
#define GLR_LINK_STATUS                 0x8B82
#define GLR_FRAMEBUFFER                 0x8D40
#define GLR_RENDERBUFFER                0x8D41
#define GLR_READ_FRAMEBUFFER            0x8CA8
#define GLR_DRAW_FRAMEBUFFER            0x8CA9
#define GLR_COLOR_ATTACHMENT0           0x8CE0
#define GLR_DEPTH_ATTACHMENT            0x8D00
#define GLR_DEPTH_STENCIL_ATTACHMENT    0x821A
#define GLR_FRAMEBUFFER_COMPLETE        0x8CD5

static const char *const glr__material_vertex_parts[] = {
  "#version 300 es\n"
  "precision highp float;\n"
  "precision highp int;\n"
  "layout(location=0) in vec3 aPosition;\n"
  "layout(location=1) in vec3 aNormal;\n"
  "layout(location=2) in vec2 aUV;\n"
  "layout(location=3) in vec4 aColor;\n"
  "layout(location=4) in vec4 aTangent;\n"
  "layout(location=5) in vec4 aInstance0;\n"
  "layout(location=6) in vec4 aInstance1;\n"
  "layout(location=7) in vec4 aInstance2;\n"
  "layout(location=8) in vec4 aInstance3;\n"
  "layout(location=9) in vec2 aUV1;\n"
  "uniform mat4 uViewProjection;\n"
  "uniform vec4 uUVTransform;\n"
  "uniform int uMaterial20;\n"
  "uniform ivec4 uM20SamplerFlags;\n"
  "uniform vec4 uM20SamplerScales;\n"
  "uniform vec4 uM20UVTransform1[2];\n"
  "uniform vec4 uM20UVTransform2[2];\n"
  "uniform int uMaterial11;\n"
  "uniform mat4 uM11View;\n"
  "uniform mat4 uM11WorldToModel;\n"
  "uniform mat4 uM11ModelToWorld;\n"
  "uniform int uM11SpecialFlags;\n"
  "uniform int uLegacyLightingMode;\n"
  "uniform int uConditionLegacyBasis;\n"
  "uniform vec3 uMaterialCameraPosition;\n"
  "uniform float uSpecularStrength;\n"
  "uniform int uLightCount;\n"
  "uniform vec4 uLightPosition[16];\n"
  "uniform vec4 uLightAttenuation[16];\n"
  "uniform ivec4 uM11SamplerFlags;\n"
  "uniform vec4 uM11SamplerScales;\n"
  "uniform vec4 uM11UVTransform1[2];\n"
  "uniform vec4 uM11UVTransform2[2];\n"
  "out vec3 vWorld;\n"
  "out vec3 vNormal;\n"
  "out vec3 vTangent;\n"
  "out vec3 vLegacyLight;\n"
  "out vec3 vLegacyHalfway;\n"
  "out vec3 vLegacyAttenuation;\n"
  "out vec2 vUV;\n"
  "out vec2 vM20UV0;\n"
  "out vec2 vM20UV1;\n"
  "out vec2 vM20UV2;\n"
  "out vec2 vM20UV3;\n"
  "out vec2 vM11UV0;\n"
  "out vec2 vM11UV1;\n"
  "out vec2 vM11UV2;\n"
  "out vec2 vM11UV3;\n"
  "out vec4 vColor;\n"
  "out vec4 vClipPosition;\n"
  "\n"
  "vec2 material20UV(vec2 uv, int samplerIndex) {\n"
  "  int mode = (uM20SamplerFlags[samplerIndex] >> 12) & 15;\n"
  "  if (mode == 1) return uv * uM20SamplerScales[samplerIndex];\n"
  "  vec4 source = vec4(uv, 0.0, 1.0);\n"
  "  if (mode == 2) return vec2(dot(source, uM20UVTransform1[0]),\n"
  "    dot(source, uM20UVTransform1[1]));\n"
  "  if (mode == 3) return vec2(dot(source, uM20UVTransform2[0]),\n"
  "    dot(source, uM20UVTransform2[1]));\n"
  "  return uv;\n"
  "}\n"
  "\n"
  "vec2 material11UV(int samplerIndex, vec4 world, vec3 worldNormal,\n"
  "    vec4 modelPosition, vec3 modelNormal) {\n"
  "  int flags = uM11SamplerFlags[samplerIndex];\n"
  "  vec2 selectedUV = ((flags >> 4) & 3) == 1 ? aUV1 : aUV;\n"
  "  vec4 source = vec4(selectedUV, 0.0, 1.0);\n"
  "\n"
  "  if (samplerIndex == 2) {\n"
  "    int environmentMode = (uM11SpecialFlags >> 4) & 3;\n"
  "    vec3 eyeNormal = mat3(uM11View) * worldNormal;\n"
  "    if (environmentMode == 1) {\n"
  "      source.xy = eyeNormal.xy * vec2(0.5, -0.5) + 0.5;\n"
  "    } else if (environmentMode == 2) {\n"
  "      vec3 eyePosition = (uM11View * world).xyz;\n"
  "      float eyeSquared = dot(eyePosition, eyePosition);\n"
  "      vec3 eyeDirection = eyeSquared > 1e-20\n"
  "        ? eyePosition * inversesqrt(eyeSquared) : vec3(0.0);\n"
  "      float normalSquared = dot(eyeNormal, eyeNormal);\n"
  "      float factor = normalSquared > 1e-20\n"
  "        ? 2.0 * dot(eyeDirection, eyeNormal) / normalSquared : 0.0;\n"
  "      source.xyz = eyeNormal * factor - eyeDirection;\n"
  "      source.xy = source.xy * vec2(0.5, -0.5) + 0.5;\n"
  "    } else if (environmentMode == 3) {\n",
  "      // Undocumented source mode used by Debris water lighting.\n"
  "      source.xyz = modelNormal;\n"
  "    }\n"
  "  } else if (samplerIndex == 3) {\n"
  "    int projectionMode = (uM11SpecialFlags >> 8) & 7;\n"
  "    if (projectionMode == 1) source = modelPosition;\n"
  "    else if (projectionMode == 2) source = world;\n"
  "    else if (projectionMode == 3) source = uM11View * world;\n"
  "  }\n"
  "\n"
  "  int transformMode = (flags >> 12) & 3;\n"
  "  if (transformMode == 1) return source.xy * uM11SamplerScales[samplerIndex];\n"
  "  if (transformMode == 2) return vec2(dot(source, uM11UVTransform1[0]),\n"
  "    dot(source, uM11UVTransform1[1]));\n"
  "  if (transformMode == 3) return vec2(dot(source, uM11UVTransform2[0]),\n"
  "    dot(source, uM11UVTransform2[1]));\n"
  "  return source.xy;\n"
  "}\n"
  "\n"
  "void main() {\n"
  "  mat4 model = mat4(aInstance0, aInstance1, aInstance2, aInstance3);\n"
  "  vec4 world = model * vec4(aPosition, 1.0);\n"
  "  vec3 worldNormal = mat3(model) * aNormal;\n"
  "  vec4 modelPosition = uM11WorldToModel * world;\n"
  "  vec3 modelNormal = mat3(uM11WorldToModel) * worldNormal;\n"
  "  vec3 worldTangent = mat3(model) * aTangent.xyz;\n"
  "  // material11.vsh CodegenFlags variant 1 repairs the skinned input basis\n"
  "  // before any model/view transform. This applies to texture/environment\n"
  "  // phases as well as light phases; static and shader-instance variants keep\n"
  "  // the authored vectors raw.\n"
  "  vec3 conditionedInputNormal = aNormal;\n"
  "  vec3 conditionedInputTangent = aTangent.xyz;\n"
  "  if (uConditionLegacyBasis != 0) {\n"
  "    conditionedInputNormal = normalize(conditionedInputNormal);\n"
  "    conditionedInputTangent = normalize(conditionedInputTangent -\n"
  "      conditionedInputNormal * dot(conditionedInputNormal,\n"
  "        conditionedInputTangent));\n"
  "  }\n"
  "  vec3 conditionedWorldNormal = mat3(model) * conditionedInputNormal;\n"
  "  vec3 conditionedWorldTangent = mat3(model) * conditionedInputTangent;\n"
  "  vec3 material11WorldNormal = uConditionLegacyBasis != 0\n"
  "    ? conditionedWorldNormal : worldNormal;\n"
  "  vec3 material11ModelNormal = mat3(uM11WorldToModel) * material11WorldNormal;\n"
  "  vWorld = world.xyz;\n"
  "  vNormal = normalize(worldNormal);\n"
  "  vTangent = normalize(worldTangent);\n"
  "  // material11.vsh and material20_light.vsh calculate lighting coordinates at\n"
  "  // vertices. Undo only the paint job's base matrix; shader-instance\n"
  "  // transforms deliberately remain in position/N/T, matching transformedPos.\n"
  "  vec3 materialTangent = mat3(uM11WorldToModel) * worldTangent;\n"
  "  vec3 legacyNormal = modelNormal;\n"
  "  vec3 legacyTangent = materialTangent;\n"
  "  // Native's animated-mesh shader variant repairs skinned N/T. Static meshes,\n"
  "  // CPU instances and shader instances use the authored/transformed vectors\n"
  "  // directly, including their lengths and lack of orthogonality.\n"
  "  if (uLegacyLightingMode == 2) {\n"
  "    if (uConditionLegacyBasis != 0) {\n"
  "      // Material11 variant 1 performed the repair on its input attributes;\n"
  "      // transform that repaired basis into the same model space as L/H.\n"
  "      legacyNormal = material11ModelNormal;\n",
  "      legacyTangent = mat3(uM11WorldToModel) * conditionedWorldTangent;\n"
  "    } else {\n"
  "      legacyNormal = normalize(legacyNormal);\n"
  "      legacyTangent = normalize(legacyTangent - legacyNormal *\n"
  "        dot(legacyNormal, legacyTangent));\n"
  "    }\n"
  "  }\n"
  "  vec3 legacyBitangent = cross(legacyNormal, legacyTangent);\n"
  "  vLegacyLight = vec3(0.0);\n"
  "  vLegacyHalfway = vec3(0.0);\n"
  "  vLegacyAttenuation = vec3(0.0);\n"
  "  bool legacyLightPass = uLegacyLightingMode == 1 || uLegacyLightingMode == 2;\n"
  "  if (uLegacyLightingMode == 3) {\n"
  "    // material20_envi.vsh forms B in input space, then transforms raw N/B/T\n"
  "    // separately. In particular, cross(worldN,worldT) is wrong for authored\n"
  "    // non-uniform transforms. The otherwise-unused light varyings keep this\n"
  "    // source order without increasing the varying budget.\n"
  "    vLegacyLight = worldNormal;\n"
  "    vLegacyHalfway = mat3(model) * cross(aNormal, aTangent.xyz);\n"
  "    vLegacyAttenuation = worldTangent;\n"
  "  } else if (legacyLightPass && uLightCount > 0) {\n"
  "    vec3 lightVector = uLightPosition[0].xyz -\n"
  "      modelPosition.xyz * uLightPosition[0].w;\n"
  "    // The native shaders intentionally leave L unnormalised until after\n"
  "    // raster interpolation, and project in their unusual N,B,T ordering.\n"
  "    vLegacyLight = vec3(dot(lightVector, legacyNormal),\n"
  "      dot(lightVector, legacyBitangent), dot(lightVector, legacyTangent));\n"
  "    if (uSpecularStrength > 0.0) {\n"
  "      vec3 eyeVector = normalize(uMaterialCameraPosition - modelPosition.xyz);\n"
  "      vec3 halfwayVector = normalize(lightVector) + eyeVector;\n"
  "      vLegacyHalfway = vec3(dot(halfwayVector, legacyNormal),\n"
  "        dot(halfwayVector, legacyBitangent), dot(halfwayVector, legacyTangent));\n"
  "    }\n"
  "    // M20 emits its centred vector directly. material11.vsh instead performs\n"
  "    // the c5 MAD at each vertex so the 3D sampler receives [0,1] coordinates\n"
  "    // after raster interpolation, including the source's float ordering.\n"
  "    vec3 legacyAttenuationVector =\n"
  "      (modelPosition.xyz - uLightAttenuation[0].xyz) *\n"
  "      uLightAttenuation[0].w;\n"
  "    vLegacyAttenuation = uMaterial11 != 0\n"
  "      ? legacyAttenuationVector * 0.5 + vec3(0.5)\n"
  "      : legacyAttenuationVector;\n"
  "  }\n"
  "  vUV = aUV * uUVTransform.xy + uUVTransform.zw;\n"
  "  vM20UV0 = uMaterial20 != 0 ? material20UV(aUV, 0) : vUV;\n"
  "  vM20UV1 = uMaterial20 != 0 ? material20UV(aUV, 1) : vUV;\n"
  "  vM20UV2 = uMaterial20 != 0 ? material20UV(aUV, 2) : vUV;\n"
  "  vM20UV3 = uMaterial20 != 0 ? material20UV(aUV, 3) : vUV;\n"
  "  vM11UV0 = uMaterial11 != 0\n"
  "    ? material11UV(0, world, material11WorldNormal, modelPosition,\n"
  "      material11ModelNormal) : vUV;\n"
  "  vM11UV1 = uMaterial11 != 0\n"
  "    ? material11UV(1, world, material11WorldNormal, modelPosition,\n"
  "      material11ModelNormal) : vUV;\n"
  "  vM11UV2 = uMaterial11 != 0\n"
  "    ? material11UV(2, world, material11WorldNormal, modelPosition,\n"
  "      material11ModelNormal) : vUV;\n"
  "  vM11UV3 = uMaterial11 != 0\n"
  "    ? material11UV(3, world, material11WorldNormal, modelPosition,\n"
  "      material11ModelNormal) : vUV;\n"
  "  vColor = aColor;\n",
  "  vClipPosition = uViewProjection * world;\n"
  "  gl_Position = vClipPosition;\n"
  "}",
};
static const char *const glr__material_fragment_parts[] = {
  "#version 300 es\n"
  "precision highp float;\n"
  "precision highp int;\n"
  "// GLSL ES defines defaults for sampler2D/samplerCube, but not sampler3D.\n"
  "// ANGLE's Metal compiler therefore requires this explicit precision.\n"
  "precision highp sampler3D;\n"
  "in vec3 vWorld;\n"
  "in vec3 vNormal;\n"
  "in vec3 vTangent;\n"
  "in vec3 vLegacyLight;\n"
  "in vec3 vLegacyHalfway;\n"
  "in vec3 vLegacyAttenuation;\n"
  "in vec2 vUV;\n"
  "in vec2 vM20UV0;\n"
  "in vec2 vM20UV1;\n"
  "in vec2 vM20UV2;\n"
  "in vec2 vM20UV3;\n"
  "in vec2 vM11UV0;\n"
  "in vec2 vM11UV1;\n"
  "in vec2 vM11UV2;\n"
  "in vec2 vM11UV3;\n"
  "in vec4 vColor;\n"
  "in vec4 vClipPosition;\n"
  "uniform sampler2D uTexture0;\n"
  "uniform sampler2D uTexture1;\n"
  "uniform sampler2D uTexture2;\n"
  "uniform sampler2D uTexture3;\n"
  "uniform sampler2D uPrelightTexture;\n"
  "uniform samplerCube uM11NormalizerCube;\n"
  "uniform sampler3D uM11AttenuationVolume;\n"
  "uniform int uTextureMask;\n"
  "uniform int uMode;\n"
  "uniform int uVertexColorMode;\n"
  "uniform int uUsePrelight;\n"
  "uniform int uMaterial20;\n"
  "uniform int uMaterial11;\n"
  "uniform int uM11MultipassLight;\n"
  "uniform int uM11SpecialFlags;\n"
  "uniform int uLegacyLightingMode;\n"
  "uniform int uM20Flags;\n"
  "uniform int uM20EnvironmentFlags;\n"
  "uniform int uM20RuntimeEnvironmentFlags;\n"
  "uniform ivec2 uDetailOps;\n"
  "uniform int uCombiners[13];\n"
  "uniform int uAlphaCombiner;\n"
  "uniform vec4 uMaterialColors[4];\n"
  "uniform vec4 uBaseColor;\n"
  "uniform vec3 uAmbient;\n"
  "uniform vec3 uCameraPosition;\n"
  "uniform vec3 uMaterialCameraPosition;\n"
  "uniform mat4 uM11View;\n"
  "uniform mat4 uM11WorldToModel;\n"
  "uniform mat4 uM11ModelToWorld;\n"
  "uniform vec3 uFogColor;\n"
  "uniform vec2 uFogRange;\n"
  "uniform int uFogEnabled;\n"
  "uniform float uAlphaCutoff;\n"
  "uniform float uSpecularPower;\n"
  "uniform vec3 uSpecularColor;\n"
  "uniform float uSpecularStrength;\n"
  "uniform int uLightCount;\n"
  "uniform vec4 uLightPosition[16];\n"
  "uniform vec4 uLightAttenuation[16];\n"
  "uniform vec4 uLightColor[16];\n"
  "uniform float uLightSpecular[16];\n"
  "uniform vec4 uM11LightConstant;\n"
  "out vec4 outColor;\n"
  "\n"
  "vec4 texOrWhite(sampler2D image, int bit, vec2 uv) {\n"
  "  return (uTextureMask & bit) != 0 ? texture(image, uv) : vec4(1.0);\n"
  "}\n"
  "\n"
  "vec3 bumpVector(vec4 sampleValue) {\n"
  "  // Q8W8V8U8 arrives directly from an RGBA8_SNORM sampler. Native unsigned\n"
  "  // bump formats remain their raw 0..1 samples; the released shaders do not\n"
  "  // apply a conventional *2-1 remap to either representation.\n"
  "  return sampleValue.xyz;\n"
  "}\n"
  "\n"
  "vec3 detailCombine(vec3 current, vec3 detail, int operation) {\n"
  "  if (operation == 0) return current * detail;\n"
  "  if (operation == 1) return current * detail * 2.0;\n"
  "  if (operation == 2) return current * detail * 4.0;\n"
  "  if (operation == 3) return current + detail;\n"
  "  if (operation == 4) return current + detail - current * detail;\n"
  "  return current;\n"
  "}\n"
  "\n"
  "// material11.psh implements its PS1.1 specular power with four paired\n"
  "// multiply instructions rather than a hardware pow. material11.cpp derives\n"
  "// these two constants from the authored exponent (clamped to 8..60).\n"
  "float material11Specular(float cosine, float power) {\n"
  "  float usePower = clamp(power, 8.0, 60.0);\n"
  "  float inverseScale = 1.0 / 7.336032;\n",
  "  float scale = inverseScale + (usePower - 8.0) * 0.01565302;\n"
  "  float bias = inverseScale - scale;\n"
  "  float value = clamp((cosine * scale + bias) * 4.0, 0.0, 1.0);\n"
  "  value = value * value * 2.0;\n"
  "  value = value * value * 2.0;\n"
  "  return clamp(value * value * 2.0, 0.0, 1.0);\n"
  "}\n"
  "\n"
  "vec4 material11Source(int index, vec4 t0, vec4 t1, vec4 t2, vec4 t3) {\n"
  "  if (index == 1) return uMaterialColors[0];\n"
  "  if (index == 2) return uMaterialColors[1];\n"
  "  if (index == 3) return t0;\n"
  "  if (index == 4) return t1;\n"
  "  if (index == 7) return uMaterialColors[2];\n"
  "  if (index == 8) return uMaterialColors[3];\n"
  "  if (index == 9) return t2;\n"
  "  if (index == 10) return t3;\n"
  "  if (index == 11) return vColor;\n"
  "  return vec4(0.0);\n"
  "}\n"
  "\n"
  "float material11Alpha(int source, vec4 t0, vec4 t1, vec4 t2, vec4 t3) {\n"
  "  if (source == 4) return t0.a;\n"
  "  if (source == 5) return t1.a;\n"
  "  if (source == 6) return t2.a;\n"
  "  if (source == 7) return t3.a;\n"
  "  if (source == 8) return uMaterialColors[0].a;\n"
  "  if (source == 9) return uMaterialColors[1].a;\n"
  "  if (source == 10) return uMaterialColors[2].a;\n"
  "  if (source == 11) return uMaterialColors[3].a;\n"
  "  if (source == 12) return 0.5;\n"
  "  if (source == 13) return 0.0;\n"
  "  return 1.0;\n"
  "}\n"
  "\n"
  "vec4 material11Combine(vec4 t0, vec4 t1, vec4 t2, vec4 t3) {\n"
  "  vec3 current = vec3(0.0);\n"
  "  for (int index = 0; index < 13; ++index) {\n"
  "    int word = uCombiners[index];\n"
  "    int operation = (word >> 4) & 15;\n"
  "    if (operation == 0) continue;\n"
  "    vec3 source = material11Source(index, t0, t1, t2, t3).rgb;\n"
  "    int alphaCode = word & 15;\n"
  "    float sourceAlpha = material11Alpha(alphaCode, t0, t1, t2, t3);\n"
  "    // sMCA_INVERTA/B belong to AlphaCombiner. material11.cpp deliberately\n"
  "    // does not propagate either bit into a color-combiner stage's AlphaSrc.\n"
  "    if (alphaCode != 0 && operation != 9) source *= sourceAlpha;\n"
  "    if (operation == 1) current = source;\n"
  "    else if (operation == 2) current += source;\n"
  "    else if (operation == 3) current -= source;\n"
  "    else if (operation == 4) current *= source;\n"
  "    else if (operation == 5) current *= source * 2.0;\n"
  "    else if (operation == 6) current *= source * 4.0;\n"
  "    else if (operation == 7) current *= source * 8.0;\n"
  "    else if (operation == 8) current = current * (vec3(1.0) - source) + source;\n"
  "    else if (operation == 9) current = mix(source, current, sourceAlpha);\n"
  "    else if (operation == 10) current = source - current;\n"
  "    else if (operation == 11) current = vec3(dot(current, source));\n"
  "    else if (operation == 12) current = clamp(current * source, 0.0, 1.0);\n"
  "  }\n"
  "  int alphaA = uAlphaCombiner & 15;\n"
  "  int alphaB = (uAlphaCombiner >> 4) & 15;\n"
  "  float alpha;\n"
  "  if (alphaB == 14) {\n"
  "    // The source's special MIX01 path bypasses AlphaOps (and therefore both\n"
  "    // inversion bits): +mul r0.w,t0,c0; mad r0.w,t1,c0,r0.\n"
  "    alpha = (t0.a + t1.a) * uMaterialColors[0].a;\n"
  "  } else {\n"
  "    alpha = material11Alpha(alphaA, t0, t1, t2, t3);\n"
  "    if ((uAlphaCombiner & 256) != 0) alpha = 1.0 - alpha;\n"
  "    if (alphaB != 0) {\n",
  "      float second = material11Alpha(alphaB, t0, t1, t2, t3);\n"
  "      if ((uAlphaCombiner & 512) != 0) second = 1.0 - second;\n"
  "      alpha *= second;\n"
  "    }\n"
  "  }\n"
  "  return vec4(current, alpha);\n"
  "}\n"
  "\n"
  "void main() {\n"
  "  vec2 textureUV0 = uMaterial11 != 0 ? vM11UV0 : vM20UV0;\n"
  "  vec2 textureUV1 = uMaterial11 != 0 ? vM11UV1 : vM20UV1;\n"
  "  vec2 textureUV2 = uMaterial11 != 0 ? vM11UV2 : vM20UV2;\n"
  "  vec2 textureUV3 = uMaterial11 != 0 ? vM11UV3 : vM20UV3;\n"
  "  vec4 t0 = texOrWhite(uTexture0, 1, textureUV0);\n"
  "  vec4 t1 = texOrWhite(uTexture1, 2, textureUV1);\n"
  "  vec4 t2 = texOrWhite(uTexture2, 4, textureUV2);\n"
  "  vec4 t3 = texOrWhite(uTexture3, 8, textureUV3);\n"
  "  vec4 albedo = uBaseColor * t0;\n"
  "  if (uVertexColorMode == 1) albedo *= vColor;\n"
  "  else if (uVertexColorMode == 2) albedo.rgb += vColor.rgb;\n"
  "  else if (uVertexColorMode == 3) albedo = vColor * t0;\n"
  "  else if (uVertexColorMode == 4) {\n"
  "    // Generated Material11 BASE: Color0 SET + vertex ADD, with Tex0 used by\n"
  "    // AlphaCombiner only. Its RGB must not be modulated by the alpha texture.\n"
  "    albedo = vec4(uBaseColor.rgb + vColor.rgb, uBaseColor.a * t0.a);\n"
  "  }\n"
  "  // Legacy light passes receive N,B,T-space L/H from the vertex shader. Their\n"
  "  // no-bump normal is +N, represented by X in that unusual tangent ordering.\n"
  "  bool legacyVertexLighting =\n"
  "    uLegacyLightingMode == 1 || uLegacyLightingMode == 2;\n"
  "  bool legacyEnvironmentBasis = uLegacyLightingMode == 3;\n"
  "  vec3 normal = legacyVertexLighting ? vec3(1.0, 0.0, 0.0) : normalize(vNormal);\n"
  "  if (uMode == 2 && ((uTextureMask & 2) != 0 ||\n"
  "      (uMaterial20 != 0 && (uTextureMask & 4) != 0))) {\n"
  "    vec3 mapped = vec3(1.0, 0.0, 0.0);\n"
  "    int bumpCount = 0;\n"
  "    if ((uTextureMask & 2) != 0) {\n"
  "      mapped = bumpVector(t1);\n"
  "      bumpCount = 1;\n"
  "    }\n"
  "    if (uMaterial20 != 0 && (uTextureMask & 4) != 0) {\n"
  "      vec3 secondMap = bumpVector(t2);\n"
  "      mapped = (uTextureMask & 2) != 0 ? mapped + secondMap : secondMap;\n"
  "      bumpCount += 1;\n"
  "    }\n"
  "    // material20_light.psh normalizes only a sum of two maps or when Flags[5]\n"
  "    // requests it. material11.psh uses its filtered bump sample directly.\n"
  "    // Retain normalization only for non-legacy embedder materials. The\n"
  "    // released player's DefaultMat is represented as Material11 above.\n"
  "    bool renormalizeBump = uMaterial20 != 0\n"
  "      ? bumpCount > 1 || (uM20Flags & 32) != 0\n"
  "      : uMaterial11 == 0;\n"
  "    if (renormalizeBump) mapped = normalize(mapped);\n"
  "    if (legacyVertexLighting) {\n"
  "      // Native light pixel shaders dot the sampled vector directly against\n"
  "      // interpolated, cube/NRM-normalized tangent-space L and H.\n"
  "      normal = mapped;\n"
  "    } else {\n"
  "      vec3 tangent = normalize(vTangent - normal * dot(normal, vTangent));\n"
  "      vec3 bitangent = normalize(cross(normal, tangent));\n"
  "      normal = normal * mapped.x + bitangent * mapped.y + tangent * mapped.z;\n"
  "      if (renormalizeBump) normal = normalize(normal);\n"
  "    }\n"
  "  }\n"
  "\n"
  "  vec4 result = albedo;\n"
  "  if (uMode == 1) {\n"
  "    result = vec4(0.0, 0.0, 0.0, albedo.a);\n",
  "  } else if (uMode == 2) {\n"
  "    vec3 diffuseLighting = vec3(0.0);\n"
  "    vec3 specularLighting = vec3(0.0);\n"
  "    float material11SpecularLighting = 0.0;\n"
  "    float material20AttenuationAlpha = 0.0;\n"
  "    if (legacyVertexLighting && uLightCount > 0) {\n"
  "      // Material20 uses NRM after interpolation. Material11 instead samples\n"
  "      // the native 64x64 cube lookup, including its 8-bit quantization and\n"
  "      // bilinear face filtering, before PS1.1's _bx2 modifier.\n"
  "      vec3 lightDirection = uMaterial11 != 0\n"
  "        ? texture(uM11NormalizerCube, vLegacyLight).rgb * 2.0 - 1.0\n"
  "        : normalize(vLegacyLight);\n"
  "      float radiusSquared = clamp(dot(vLegacyAttenuation,\n"
  "        vLegacyAttenuation), 0.0, 1.0);\n"
  "      // M11's 32^3 lookup is intentionally asymmetric around texture-space\n"
  "      // 0.5 by half a texel and quantized to RGBA8. M20 keeps its analytic\n"
  "      // saturated MAD and exports radiusSquared in alpha.\n"
  "      float attenuation = uMaterial11 != 0\n"
  "        ? texture(uM11AttenuationVolume, vLegacyAttenuation).r\n"
  "        : 1.0 - radiusSquared;\n"
  "      float diffuse = clamp(dot(normal, lightDirection), 0.0, 1.0);\n"
  "      float specular = 0.0;\n"
  "      if (uSpecularStrength > 0.0) {\n"
  "        vec3 halfwayDirection = uMaterial11 != 0\n"
  "          ? texture(uM11NormalizerCube, vLegacyHalfway).rgb * 2.0 - 1.0\n"
  "          : normalize(vLegacyHalfway);\n"
  "        float cosine = clamp(dot(normal, halfwayDirection), 0.0, 1.0);\n"
  "        specular = pow(cosine, max(1.0, uSpecularPower));\n"
  "        if (uM11MultipassLight != 0) {\n"
  "          specular = material11Specular(cosine, uSpecularPower);\n"
  "        }\n"
  "        if (uMaterial20 != 0 && (uM20Flags & 128) != 0) {\n"
  "          specular *= (uTextureMask & 2) != 0 ? t1.a : t2.a;\n"
  "        }\n"
  "        if (uM11MultipassLight != 0 && (uM11SpecialFlags & 128) != 0) specular *= t1.a;\n"
  "      }\n"
  "      // sMaterial11 combines Color[0], the selected light color and its\n"
  "      // amplification on the CPU, then uploads that product to PS1.1 c0.\n"
  "      // PS1.x constant registers are restricted to [-1,+1].  Keeping the\n"
  "      // factors separate in GLSL would let amplified values above one survive\n"
  "      // until the framebuffer blend and substantially over-warm bright lights.\n"
  "      diffuseLighting = (uMaterial11 != 0\n"
  "        ? uM11LightConstant.rgb : uLightColor[0].rgb) * attenuation * diffuse;\n"
  "      specularLighting = uLightColor[0].rgb * attenuation * specular;\n"
  "      material11SpecularLighting = (uMaterial11 != 0\n"
  "        ? uM11LightConstant.a : uLightSpecular[0]) * attenuation * specular;\n"
  "      if (uMaterial20 != 0) material20AttenuationAlpha = radiusSquared;\n"
  "    } else if (!legacyVertexLighting) {\n"
  "      // Retain the port's generic world-space material path, including its\n"
  "      // ability to accumulate several selected lights in one draw.\n"
  "      vec3 viewDirection = normalize(uCameraPosition - vWorld);\n"
  "      for (int index = 0; index < 16; ++index) {\n"
  "        if (index >= uLightCount) break;\n"
  "        vec3 vectorToLight = uLightPosition[index].xyz -\n",
  "          vWorld * uLightPosition[index].w;\n"
  "        float distanceToLight = length(vectorToLight);\n"
  "        vec3 lightDirection = distanceToLight > 1e-5\n"
  "          ? vectorToLight / distanceToLight : vec3(0.0, 1.0, 0.0);\n"
  "        float radius = distanceToLight * uLightAttenuation[index].w;\n"
  "        float attenuation = uLightPosition[index].w == 0.0 ? 1.0 :\n"
  "          max(0.0, 1.0 - radius * radius);\n"
  "        float diffuse = max(dot(normal, lightDirection), 0.0);\n"
  "        vec3 halfwayDirection = normalize(lightDirection + viewDirection);\n"
  "        float specular = pow(max(dot(normal, halfwayDirection), 0.0),\n"
  "          max(1.0, uSpecularPower));\n"
  "        diffuseLighting += uLightColor[index].rgb * attenuation * diffuse;\n"
  "        specularLighting += uLightColor[index].rgb * attenuation * specular;\n"
  "      }\n"
  "    }\n"
  "    vec4 surface = albedo;\n"
  "    if (uUsePrelight != 0) {\n"
  "      vec2 screenUV = vClipPosition.xy / max(1e-6, vClipPosition.w) * 0.5 + 0.5;\n"
  "      surface = texture(uPrelightTexture, screenUV) * uBaseColor;\n"
  "    }\n"
  "    if (uM11MultipassLight != 0) {\n"
  "      // Material11's light phase stores diffuse in RGB and specular in alpha.\n"
  "      // Material11Insert preserves alpha through POSTLIGHT, then adds it back\n"
  "      // to RGB. Keeping the channels separate is essential: otherwise the\n"
  "      // multiplicative texture phase incorrectly textures the highlight.\n"
  "      // The material diffuse and alpha are already part of the native c0\n"
  "      // constant above; multiplying by surface here would apply them twice.\n"
  "      result = vec4(diffuseLighting,\n"
  "        material11SpecularLighting * uSpecularStrength);\n"
  "    } else {\n"
  "      vec3 litColor = (uMaterial11 != 0 ? diffuseLighting\n"
  "        : surface.rgb * diffuseLighting) +\n"
  "        uSpecularColor * specularLighting * uSpecularStrength;\n"
  "      // material20_light.psh's attenuation MAD is saturated before the\n"
  "      // optional final COLOR0 multiplication.\n"
  "      if (uMaterial20 != 0) litColor = clamp(litColor, 0.0, 1.0);\n"
  "      result = vec4(litColor,\n"
  "        uMaterial20 != 0 ? material20AttenuationAlpha : albedo.a);\n"
  "      // material20_light.psh applies COLOR0 after material, specular and\n"
  "      // attenuation, so it modulates highlights and radial alpha as well as\n"
  "      // the diffuse surface. No other pass shares this ordering.\n"
  "      if (uMaterial20 != 0 && (uM20Flags & 8) != 0) result *= vColor;\n"
  "    }\n"
  "  } else if (uMode == 3) {\n"
  "    result = vec4(albedo.rgb * t2.rgb * 2.0, albedo.a * t2.a);\n"
  "  } else if (uMode == 4) {\n"
  "    if (uMaterial11 != 0) {\n"
  "      // material11.vsh has already produced sphere/reflection/projection UVs\n"
  "      // for logical Tex2, including its per-slot texture transform.\n"
  "      vec3 environment = t2.rgb;\n"
  "      if ((uTextureMask & 1) != 0) environment *= t0.a;\n"
  "      result = vec4(environment, 1.0);\n"
  "    } else {\n"
  "    vec4 environmentMap = (uTextureMask & 2) != 0\n"
  "      ? texture(uTexture1, vUV) : vec4(1.0);\n"
  "    vec3 environmentNormal = legacyEnvironmentBasis ? vLegacyLight : normal;\n",
  "    if (uMaterial20 != 0 && (uM20EnvironmentFlags & 131072) != 0 &&\n"
  "        (uTextureMask & 2) != 0) {\n"
  "      vec3 mapped = bumpVector(environmentMap);\n"
  "      if (legacyEnvironmentBasis) {\n"
  "        // material20_envi.psh combines the three raw, independently\n"
  "        // interpolated basis vectors and normalizes only the result.\n"
  "        environmentNormal = vLegacyLight * mapped.x +\n"
  "          vLegacyHalfway * mapped.y + vLegacyAttenuation * mapped.z;\n"
  "      } else {\n"
  "        vec3 tangent = normalize(vTangent - normal * dot(normal, vTangent));\n"
  "        vec3 bitangent = normalize(cross(normal, tangent));\n"
  "        environmentNormal = normalize(normal * mapped.x +\n"
  "          bitangent * mapped.y + tangent * mapped.z);\n"
  "      }\n"
  "    }\n"
  "    if (legacyEnvironmentBasis && (uM20RuntimeEnvironmentFlags & 65536) == 0) {\n"
  "      // The non-reflection environment variant uploads ModelSpace*Camera, so\n"
  "      // its basis is in eye space before the sphere-map projection.\n"
  "      environmentNormal = mat3(uM11View) * environmentNormal;\n"
  "    }\n"
  "    if (legacyEnvironmentBasis) environmentNormal = normalize(environmentNormal);\n"
  "    vec3 environmentDirection = environmentNormal;\n"
  "    if (uMaterial20 == 0 || (uM20EnvironmentFlags & 65536) != 0) {\n"
  "      vec3 eyeVector = legacyEnvironmentBasis\n"
  "        ? mat3(uM11ModelToWorld) * (uMaterialCameraPosition -\n"
  "          (uM11WorldToModel * vec4(vWorld, 1.0)).xyz)\n"
  "        : uCameraPosition - vWorld;\n"
  "      vec3 eyeDirection = normalize(eyeVector);\n"
  "      environmentDirection = eyeDirection - 2.0 *\n"
  "        dot(eyeDirection, environmentNormal) * environmentNormal;\n"
  "    }\n"
  "    vec3 normalizedDirection = normalize(environmentDirection);\n"
  "    vec2 environmentUV = normalizedDirection.xy * vec2(0.5, -0.5) + 0.5;\n"
  "    vec4 environmentColor = (uTextureMask & 4) != 0\n"
  "      ? texture(uTexture2, environmentUV) : t2;\n"
  "    vec3 environment = environmentColor.rgb * uBaseColor.rgb;\n"
  "    if ((uTextureMask & 1) != 0) environment *= t0.rgb;\n"
  "    if (uMaterial20 != 0 && (uM20Flags & 128) != 0) environment *= environmentMap.a;\n"
  "    result = vec4(environment, environmentColor.a * uBaseColor.a);\n"
  "    }\n"
  "  } else if (uMode == 5) {\n"
  "    // Material20's ambient shader writes RGB only; Engine strips ambient\n"
  "    // alpha before uploading the constant.\n"
  "    result = vec4(albedo.rgb * uAmbient, 0.0);\n"
  "  } else if (uMode == 6) {\n"
  "    // material20_tex.psh's no-main-texture constant is c8=(1,1,1,0), not\n"
  "    // the ordinary white fallback texture (whose alpha is one).\n"
  "    result = (uTextureMask & 1) != 0 ? t0 : vec4(1.0, 1.0, 1.0, 0.0);\n"
  "    if ((uTextureMask & 2) != 0) result.rgb = detailCombine(result.rgb, t1.rgb, uDetailOps.x);\n"
  "    if ((uTextureMask & 4) != 0) result.rgb = detailCombine(result.rgb, t2.rgb, uDetailOps.y);\n"
  "  } else if (uMode == 7) {\n"
  "    result = material11Combine(t0, t1, t2, t3);\n"
  "  } else {\n"
  "    result.rgb *= max(uAmbient, vec3(1.0));\n"
  "    if ((uTextureMask & 4) != 0) result.rgb += t2.rgb * uBaseColor.rgb;\n"
  "    if ((uTextureMask & 8) != 0) result *= t3 * 2.0;\n"
  "  }\n",
  "  if (result.a <= uAlphaCutoff) discard;\n"
  "  if (uFogEnabled != 0) {\n"
  "    // material11.vsh emits dot(modelPos,(Model*View).k): camera-space Z.\n"
  "    // Radial distance produces visibly different fog off the optical axis.\n"
  "    float eyeZ = (uM11View * vec4(vWorld, 1.0)).z;\n"
  "    float fog = clamp((eyeZ - uFogRange.x) /\n"
  "      max(1e-5, uFogRange.y - uFogRange.x), 0.0, 1.0);\n"
  "    result.rgb = mix(result.rgb, uFogColor, fog);\n"
  "  }\n"
  "  outColor = result;\n"
  "}",
};
static const char *const glr__shadow_vertex_parts[] = {
  "#version 300 es\n"
  "precision highp float;\n"
  "layout(location=0) in vec3 aPosition;\n"
  "layout(location=1) in float aExtrude;\n"
  "uniform mat4 uViewProjection;\n"
  "uniform mat4 uModel;\n"
  "uniform vec3 uLightPosition;\n"
  "void main() {\n"
  "  vec4 modelPosition = vec4(aPosition, 1.0) -\n"
  "    vec4(uLightPosition, 1.0) * aExtrude;\n"
  "  // q=1 projects a homogeneous direction (w=0) exactly onto z=w. Nudging it\n"
  "  // inward would turn the infinite z-fail volume into an arbitrary finite one.\n"
  "  gl_Position = uViewProjection * (uModel * modelPosition);\n"
  "}",
};
static const char *const glr__shadow_fragment_parts[] = {
  "#version 300 es\n"
  "precision highp float;\n"
  "out vec4 outColor;\n"
  "void main() { outColor = vec4(0.0); }",
};
static const char *const glr__fullscreen_vertex_parts[] = {
  "#version 300 es\n"
  "precision highp float;\n"
  "out vec2 vUV;\n"
  "void main() {\n"
  "  vec2 position = vec2((gl_VertexID << 1) & 2, gl_VertexID & 2);\n"
  "  vUV = position;\n"
  "  gl_Position = vec4(position * 2.0 - 1.0, 0.0, 1.0);\n"
  "}",
};
static const char *const glr__fullscreen_fragment_parts[] = {
  "#version 300 es\n"
  "precision highp float;\n"
  "in vec2 vUV;\n"
  "uniform sampler2D uImage;\n"
  "uniform sampler2D uMaterialTexture;\n"
  "uniform int uMode;\n"
  "uniform vec4 uColor0;\n"
  "uniform vec4 uColor1;\n"
  "uniform vec4 uParameters;\n"
  "uniform vec4 uColorCorrect[7];\n"
  "uniform vec2 uTexel;\n"
  "uniform vec4 uUVRect;\n"
  "out vec4 outColor;\n"
  "\n"
  "void main() {\n"
  "  vec2 uv = mix(uUVRect.xy, uUVRect.zw, vUV);\n"
  "  vec4 source = texture(uImage, uv);\n"
  "  if (uMode == 1) {\n"
  "    // effect_glare.psh's bilinear 3x3 approximation of the native\n"
  "    // (1,4,6,4,1)/16 downsample kernel.\n"
  "    const float weights[3] = float[3](5.0 / 16.0, 6.0 / 16.0, 5.0 / 16.0);\n"
  "    source = vec4(0.0);\n"
  "    for (int y = 0; y < 3; y++) {\n"
  "      for (int x = 0; x < 3; x++) {\n"
  "        vec2 offset = vec2(float(x - 1), float(y - 1)) * 1.2 * uTexel;\n"
  "        source += texture(uImage, uv + offset) * weights[x] * weights[y];\n"
  "      }\n"
  "    }\n"
  "    // effect_glare.psh seeds accu with c0 as part of its first MAD. c0.rgb\n"
  "    // is zero, while c0.a is the packed first-tap weight.\n"
  "    source.a += weights[0] * weights[0];\n"
  "    source = clamp(source, 0.0, 1.0);\n"
  "  } else if (uMode == 2) {\n"
  "    float gray = dot(source.rgb, vec3(1.0 / 3.0));\n"
  "    vec3 dark = gray * uColorCorrect[5].rgb + source.rgb * uColorCorrect[0].rgb;\n"
  "    float range = clamp(gray * uColorCorrect[4].y + uColorCorrect[4].x, 0.0, 1.0);\n"
  "    vec3 light = gray * uColorCorrect[6].rgb + source.rgb * uColorCorrect[1].rgb;\n"
  "    source.rgb = dark * range + light * (1.0 - range) + uColorCorrect[4].z * range;\n"
  "    source.rgb = clamp(source.rgb - uColorCorrect[3].rgb, 0.0, 1.0);\n"
  "    source.rgb *= uColorCorrect[2].rgb;\n"
  "  } else if (uMode == 3) {\n"
  "    vec4 material = texture(uMaterialTexture, vUV);\n"
  "    source *= material * uColor0;\n"
  "  } else if (uMode == 4) {\n"
  "    // effect_glare2.psh: subtract the authored threshold, derive its custom\n"
  "    // gray scalar, then mix the chromatic and grayscale results.\n"
  "    vec3 color = source.rgb - uColor0.rgb;\n"
  "    float gray = dot(color, vec3(uParameters.x));\n"
  "    source.rgb = color * uParameters.y + vec3(gray) * (1.0 - uParameters.y);\n"
  "    source.a = source.a * uParameters.y + gray * (1.0 - uParameters.y);\n"
  "  } else if (uMode == 5) {\n"
  "    // effect_glare.psh reused as one half of each separable 9-tap pass.\n"
  "    const float weights[9] = float[9](\n"
  "      1.0 / 25.0, 2.0 / 25.0, 3.0 / 25.0, 4.0 / 25.0, 5.0 / 25.0,\n"
  "      4.0 / 25.0, 3.0 / 25.0, 2.0 / 25.0, 1.0 / 25.0);\n"
  "    source = vec4(0.0);\n"
  "    for (int tap = 0; tap < 9; tap++) {\n"
  "      source += texture(uImage, uv + float(tap - 4) * uParameters.xy) * weights[tap];\n"
  "    }\n"
  "    source.a += weights[0];\n"
  "    source = clamp(source * uParameters.z, 0.0, 1.0);\n"
  "  } else if (uMode == 6) {\n"
  "    // effect_glare3.psh: independently tint glare and the untouched frame,\n"
  "    // then select additive/soft-add behavior with the authored scalar.\n"
  "    vec4 glare = texture(uImage, vUV) * uColor0;\n"
  "    vec4 original = texture(uMaterialTexture, uv) * uColor1;\n"
  "    source = glare + original - glare * original * uParameters.x;\n"
  "  } else if (uMode == 7) {\n",
  "    // Material11Insert's two framebuffer-alpha quads use a constant white\n"
  "    // source with zero alpha. Their blend factors do the actual clear/add.\n"
  "    source = vec4(1.0, 1.0, 1.0, 0.0);\n"
  "  }\n"
  "  outColor = source;\n"
  "}",
};

static const struct {
  const char *const *parts;
  unsigned n_parts, length;
} glr__shader_sources[] = { { glr__material_vertex_parts, 4u, 8952u },  { glr__material_fragment_parts, 7u, 18247u },
                            { glr__shadow_vertex_parts, 1u, 515u },     { glr__shadow_fragment_parts, 1u, 95u },
                            { glr__fullscreen_vertex_parts, 1u, 205u }, { glr__fullscreen_fragment_parts, 2u, 3198u } };
/* END GENERATED */

#include <stdlib.h>
#include <string.h>

#ifndef GLR_MALLOC
#define GLR_MALLOC(n) malloc(n)
#define GLR_FREE(p)   free(p)
#endif

/* ---- the GL function table ---- */

#define GLR_GL_FUNCTIONS(X)                                                                                              \
  X(GLRuint, glCreateShader, (GLRenum))                                                                                  \
  X(void, glShaderSource, (GLRuint, GLRsizei, const char *const *, const GLRint *))                                      \
  X(void, glCompileShader, (GLRuint))                                                                                    \
  X(void, glGetShaderiv, (GLRuint, GLRenum, GLRint *))                                                                   \
  X(void, glGetShaderInfoLog, (GLRuint, GLRsizei, GLRsizei *, char *))                                                   \
  X(void, glDeleteShader, (GLRuint))                                                                                     \
  X(GLRuint, glCreateProgram, (void))                                                                                    \
  X(void, glAttachShader, (GLRuint, GLRuint))                                                                            \
  X(void, glLinkProgram, (GLRuint))                                                                                      \
  X(void, glGetProgramiv, (GLRuint, GLRenum, GLRint *))                                                                  \
  X(void, glGetProgramInfoLog, (GLRuint, GLRsizei, GLRsizei *, char *))                                                  \
  X(void, glDeleteProgram, (GLRuint))                                                                                    \
  X(GLRint, glGetUniformLocation, (GLRuint, const char *))                                                               \
  X(void, glUseProgram, (GLRuint))                                                                                       \
  X(void, glGenVertexArrays, (GLRsizei, GLRuint *))                                                                      \
  X(void, glDeleteVertexArrays, (GLRsizei, const GLRuint *))                                                             \
  X(void, glBindVertexArray, (GLRuint))                                                                                  \
  X(void, glGenBuffers, (GLRsizei, GLRuint *))                                                                           \
  X(void, glDeleteBuffers, (GLRsizei, const GLRuint *))                                                                  \
  X(void, glGenFramebuffers, (GLRsizei, GLRuint *))                                                                      \
  X(void, glDeleteFramebuffers, (GLRsizei, const GLRuint *))                                                             \
  X(void, glBindFramebuffer, (GLRenum, GLRuint))                                                                         \
  X(void, glFramebufferTexture2D, (GLRenum, GLRenum, GLRenum, GLRuint, GLRint))                                          \
  X(void, glFramebufferRenderbuffer, (GLRenum, GLRenum, GLRenum, GLRuint))                                               \
  X(GLRenum, glCheckFramebufferStatus, (GLRenum))                                                                        \
  X(void, glGenRenderbuffers, (GLRsizei, GLRuint *))                                                                     \
  X(void, glDeleteRenderbuffers, (GLRsizei, const GLRuint *))                                                            \
  X(void, glBindRenderbuffer, (GLRenum, GLRuint))                                                                        \
  X(void, glRenderbufferStorage, (GLRenum, GLRenum, GLRsizei, GLRsizei))                                                 \
  X(void, glGenTextures, (GLRsizei, GLRuint *))                                                                          \
  X(void, glDeleteTextures, (GLRsizei, const GLRuint *))                                                                 \
  X(void, glBindTexture, (GLRenum, GLRuint))                                                                             \
  X(void, glActiveTexture, (GLRenum))                                                                                    \
  X(void, glTexImage2D, (GLRenum, GLRint, GLRint, GLRsizei, GLRsizei, GLRint, GLRenum, GLRenum, const void *))           \
  X(void, glTexImage3D, (GLRenum, GLRint, GLRint, GLRsizei, GLRsizei, GLRsizei, GLRint, GLRenum, GLRenum, const void *)) \
  X(void, glTexParameteri, (GLRenum, GLRenum, GLRint))                                                                   \
  X(void, glPixelStorei, (GLRenum, GLRint))                                                                              \
  X(void, glViewport, (GLRint, GLRint, GLRsizei, GLRsizei))                                                              \
  X(void, glScissor, (GLRint, GLRint, GLRsizei, GLRsizei))                                                               \
  X(void, glEnable, (GLRenum))                                                                                           \
  X(void, glDisable, (GLRenum))                                                                                          \
  X(void, glColorMask, (GLRboolean, GLRboolean, GLRboolean, GLRboolean))                                                 \
  X(void, glDepthMask, (GLRboolean))                                                                                     \
  X(void, glStencilMask, (GLRuint))                                                                                      \
  X(void, glClearColor, (GLRfloat, GLRfloat, GLRfloat, GLRfloat))                                                        \
  X(void, glClearDepthf, (GLRfloat))                                                                                     \
  X(void, glClearStencil, (GLRint))                                                                                      \
  X(void, glClear, (GLRuint))                                                                                            \
  X(void, glUniform1i, (GLRint, GLRint))                                                                                 \
  X(void, glUniform2f, (GLRint, GLRfloat, GLRfloat))                                                                     \
  X(void, glUniform4fv, (GLRint, GLRsizei, const GLRfloat *))                                                            \
  X(void, glDrawArrays, (GLRenum, GLRint, GLRsizei))                                                                     \
  X(void, glReadPixels, (GLRint, GLRint, GLRsizei, GLRsizei, GLRenum, GLRenum, void *))                                  \
  X(void, glBindBuffer, (GLRenum, GLRuint))                                                                              \
  X(void, glBufferData, (GLRenum, GLRsizeiptr, const void *, GLRenum))                                                   \
  X(void, glBufferSubData, (GLRenum, GLRintptr, GLRsizeiptr, const void *))                                              \
  X(void, glEnableVertexAttribArray, (GLRuint))                                                                          \
  X(void, glDisableVertexAttribArray, (GLRuint))                                                                         \
  X(void, glVertexAttribPointer, (GLRuint, GLRint, GLRenum, GLRboolean, GLRsizei, const void *))                         \
  X(void, glVertexAttribDivisor, (GLRuint, GLRuint))                                                                     \
  X(void, glVertexAttrib3f, (GLRuint, GLRfloat, GLRfloat, GLRfloat))                                                     \
  X(void, glVertexAttrib4f, (GLRuint, GLRfloat, GLRfloat, GLRfloat, GLRfloat))                                           \
  X(void, glVertexAttrib4fv, (GLRuint, const GLRfloat *))                                                                \
  X(void, glCullFace, (GLRenum))                                                                                         \
  X(void, glFrontFace, (GLRenum))                                                                                        \
  X(void, glDepthFunc, (GLRenum))                                                                                        \
  X(void, glBlendEquation, (GLRenum))                                                                                    \
  X(void, glBlendFunc, (GLRenum, GLRenum))                                                                               \
  X(void, glPolygonOffset, (GLRfloat, GLRfloat))                                                                         \
  X(void, glStencilFunc, (GLRenum, GLRint, GLRuint))                                                                     \
  X(void, glStencilOp, (GLRenum, GLRenum, GLRenum))                                                                      \
  X(void, glStencilOpSeparate, (GLRenum, GLRenum, GLRenum, GLRenum))                                                     \
  X(void, glUniform1f, (GLRint, GLRfloat))                                                                               \
  X(void, glUniform1fv, (GLRint, GLRsizei, const GLRfloat *))                                                            \
  X(void, glUniform1iv, (GLRint, GLRsizei, const GLRint *))                                                              \
  X(void, glUniform2iv, (GLRint, GLRsizei, const GLRint *))                                                              \
  X(void, glUniform3f, (GLRint, GLRfloat, GLRfloat, GLRfloat))                                                           \
  X(void, glUniform3fv, (GLRint, GLRsizei, const GLRfloat *))                                                            \
  X(void, glUniform4f, (GLRint, GLRfloat, GLRfloat, GLRfloat, GLRfloat))                                                 \
  X(void, glUniform4iv, (GLRint, GLRsizei, const GLRint *))                                                              \
  X(void, glUniformMatrix4fv, (GLRint, GLRsizei, GLRboolean, const GLRfloat *))                                          \
  X(void, glDrawElements, (GLRenum, GLRsizei, GLRenum, const void *))                                                    \
  X(void, glDrawElementsInstanced, (GLRenum, GLRsizei, GLRenum, const void *, GLRsizei))                                 \
  X(void, glBlitFramebuffer, (GLRint, GLRint, GLRint, GLRint, GLRint, GLRint, GLRint, GLRint, GLRuint, GLRenum))         \
  X(void, glCompressedTexImage2D, (GLRenum, GLRint, GLRenum, GLRsizei, GLRsizei, GLRint, GLRsizei, const void *))        \
  X(void, glTexParameterf, (GLRenum, GLRenum, GLRfloat))                                                                 \
  X(void, glGenerateMipmap, (GLRenum))

typedef struct glr_gl {
#define GLR_X(ret, name, args) ret(*name) args;
  GLR_GL_FUNCTIONS(GLR_X)
#undef GLR_X
} glr_gl;

/* ---- program inventory: the uniform names the JS Renderer asks for, in its order ---- */

static const char *const glr__material_uniforms[] = { "uViewProjection",
                                                      "uUVTransform",
                                                      "uTexture0",
                                                      "uTexture1",
                                                      "uTexture2",
                                                      "uTexture3",
                                                      "uPrelightTexture",
                                                      "uM11NormalizerCube",
                                                      "uM11AttenuationVolume",
                                                      "uTextureMask",
                                                      "uMode",
                                                      "uVertexColorMode",
                                                      "uUsePrelight",
                                                      "uMaterial20",
                                                      "uM20Flags",
                                                      "uM20EnvironmentFlags",
                                                      "uM20RuntimeEnvironmentFlags",
                                                      "uM20SamplerFlags",
                                                      "uM20SamplerScales",
                                                      "uM20UVTransform1[0]",
                                                      "uM20UVTransform2[0]",
                                                      "uMaterial11",
                                                      "uM11MultipassLight",
                                                      "uM11View",
                                                      "uM11WorldToModel",
                                                      "uM11ModelToWorld",
                                                      "uM11SpecialFlags",
                                                      "uLegacyLightingMode",
                                                      "uConditionLegacyBasis",
                                                      "uM11SamplerFlags",
                                                      "uM11SamplerScales",
                                                      "uM11UVTransform1[0]",
                                                      "uM11UVTransform2[0]",
                                                      "uDetailOps",
                                                      "uCombiners[0]",
                                                      "uAlphaCombiner",
                                                      "uMaterialColors[0]",
                                                      "uBaseColor",
                                                      "uAmbient",
                                                      "uCameraPosition",
                                                      "uMaterialCameraPosition",
                                                      "uFogColor",
                                                      "uFogRange",
                                                      "uFogEnabled",
                                                      "uAlphaCutoff",
                                                      "uSpecularPower",
                                                      "uSpecularColor",
                                                      "uSpecularStrength",
                                                      "uLightCount",
                                                      "uLightPosition[0]",
                                                      "uLightAttenuation[0]",
                                                      "uLightColor[0]",
                                                      "uLightSpecular[0]",
                                                      "uM11LightConstant" };
#define GLR__N_MATERIAL_UNIFORMS ((int)(sizeof glr__material_uniforms / sizeof glr__material_uniforms[0]))
enum {
  GLR__U_VIEW_PROJECTION,
  GLR__U_UV_TRANSFORM,
  GLR__U_TEXTURE0,
  GLR__U_TEXTURE1,
  GLR__U_TEXTURE2,
  GLR__U_TEXTURE3,
  GLR__U_PRELIGHT_TEXTURE,
  GLR__U_M11_NORMALIZER,
  GLR__U_M11_ATTENUATION,
  GLR__U_TEXTURE_MASK,
  GLR__U_MODE,
  GLR__U_VERTEX_COLOR_MODE,
  GLR__U_USE_PRELIGHT,
  GLR__U_MATERIAL20,
  GLR__U_M20_FLAGS,
  GLR__U_M20_ENVIRONMENT_FLAGS,
  GLR__U_M20_RUNTIME_ENVIRONMENT_FLAGS,
  GLR__U_M20_SAMPLER_FLAGS,
  GLR__U_M20_SAMPLER_SCALES,
  GLR__U_M20_UV_TRANSFORM1,
  GLR__U_M20_UV_TRANSFORM2,
  GLR__U_MATERIAL11,
  GLR__U_M11_MULTIPASS_LIGHT,
  GLR__U_M11_VIEW,
  GLR__U_M11_WORLD_TO_MODEL,
  GLR__U_M11_MODEL_TO_WORLD,
  GLR__U_M11_SPECIAL_FLAGS,
  GLR__U_LEGACY_LIGHTING_MODE,
  GLR__U_CONDITION_LEGACY_BASIS,
  GLR__U_M11_SAMPLER_FLAGS,
  GLR__U_M11_SAMPLER_SCALES,
  GLR__U_M11_UV_TRANSFORM1,
  GLR__U_M11_UV_TRANSFORM2,
  GLR__U_DETAIL_OPS,
  GLR__U_COMBINERS,
  GLR__U_ALPHA_COMBINER,
  GLR__U_MATERIAL_COLORS,
  GLR__U_BASE_COLOR,
  GLR__U_AMBIENT,
  GLR__U_CAMERA_POSITION,
  GLR__U_MATERIAL_CAMERA_POSITION,
  GLR__U_FOG_COLOR,
  GLR__U_FOG_RANGE,
  GLR__U_FOG_ENABLED,
  GLR__U_ALPHA_CUTOFF,
  GLR__U_SPECULAR_POWER,
  GLR__U_SPECULAR_COLOR,
  GLR__U_SPECULAR_STRENGTH,
  GLR__U_LIGHT_COUNT,
  GLR__U_LIGHT_POSITION,
  GLR__U_LIGHT_ATTENUATION,
  GLR__U_LIGHT_COLOR,
  GLR__U_LIGHT_SPECULAR,
  GLR__U_M11_LIGHT_CONSTANT
};

static const char *const glr__shadow_uniforms[] = { "uViewProjection", "uModel", "uLightPosition" };
static const char *const glr__fullscreen_uniforms[] = { "uImage",      "uMaterialTexture", "uMode",  "uColor0", "uColor1",
                                                        "uParameters", "uColorCorrect[0]", "uTexel", "uUVRect" };
enum {
  GLR__FS_IMAGE,
  GLR__FS_MATERIAL_TEXTURE,
  GLR__FS_MODE,
  GLR__FS_COLOR0,
  GLR__FS_COLOR1,
  GLR__FS_PARAMETERS,
  GLR__FS_COLOR_CORRECT,
  GLR__FS_TEXEL,
  GLR__FS_UV_RECT,
  GLR__N_FULLSCREEN_UNIFORMS
};

enum {
  GLR__SRC_MATERIAL_VERTEX,
  GLR__SRC_MATERIAL_FRAGMENT,
  GLR__SRC_SHADOW_VERTEX,
  GLR__SRC_SHADOW_FRAGMENT,
  GLR__SRC_FULLSCREEN_VERTEX,
  GLR__SRC_FULLSCREEN_FRAGMENT
};

#define GLR__M11_NORMALIZER_UNIT  5
#define GLR__M11_ATTENUATION_UNIT 6

/* ---- cache entries ---- */

typedef struct glr_tex {
  wz3_texture_entry entry;
  GLRuint texture;
  wz3_sampler_state sampler;
  int released; /* the host freed the pixels after the upload (glr_host.bitmap_uploaded): the texture is final */
} glr_tex;

typedef struct glr_topology {
  uint32_t *key;
  uint32_t n_key;
  wz3_shadow_topology t;
} glr_topology;

typedef struct glr_geom {
  GLRuint vao, buf[7];
  int dynamic;
  GLRenum index_type;
  uint32_t index_bytes;
  size_t bytes[7];
  uint8_t type[7];
  const void *positions_id, *indices_id, *shadow_id;
  uint32_t n_vertices, n_indices, version;
  wz3_geometry_group *groups;
  uint32_t n_groups;
  const wz3_geometry *g;
  glr_topology *topos;
  uint32_t n_topos, cap_topos;
} glr_geom;

typedef struct glr_pool {
  int32_t frame;
  uint32_t used;
  double *times;
  glr_geom **by_time;
  uint32_t n_times, cap_times, cap_by_time;
  glr_geom **entries;
  uint32_t n_entries, cap_entries;
} glr_pool;

typedef struct glr_water_statics {
  uint8_t *colors;
  float *tangents;
  wz3_geometry_group group;
} glr_water_statics;
typedef struct glr_inst_record {
  const void *job;
  const float *data;
  uint32_t bytes, offset;
} glr_inst_record;
typedef struct glr_job_cache {
  int ready;
  float *data;
  uint32_t count;
} glr_job_cache;

typedef struct glr_target {
  GLRuint framebuffer, color, depth;
  int has_depth, depth_stencil;
  int32_t width, height;
} glr_target;

struct glr {
  glr_gl gl;
  GLRuint program, shadow_program, fullscreen_program;
  GLRint material_uniforms[GLR__N_MATERIAL_UNIFORMS], shadow_uniforms[3], fullscreen_uniforms[GLR__N_FULLSCREEN_UNIFORMS];
  GLRuint shadow_vao, shadow_position_buffer, shadow_extrusion_buffer, shadow_index_buffer;
  GLRuint fullscreen_vao, instance_buffer;
  GLRuint normalizer_cube, attenuation_volume, fallback_texture, overlay_texture;
  glr_target master, prelight;
  int32_t canvas_width, canvas_height, width, height;
  wz3_region presentation;
  uint32_t draw_calls, triangles, outputs, unsupported_nodes;
  wz3_draw_stats culling;
  int no_shadows;
  int oom; /* an allocation failed since the frame began: the frame result is GLR_ERR_OUT_OF_MEMORY */
  wz3_texture_policy tex_policy;
  wz3_sampler_caps tex_caps;
  wz3_texture_stats tex_stats;
  wz3_sampler_state fallback_sampler;
  glr_tex **tex_by_op;
  glr_geom **geom_by_op;
  glr_pool **pool_by_op;
  glr_water_statics **water_by_op;
  uint32_t n_op_tables;
  glr_geom **all_geoms;
  uint32_t n_all_geoms, cap_all_geoms;
  int32_t geometry_frame;
  uint32_t instance_capacity, instance_used;
  int instance_active;
  glr_inst_record *inst_records;
  uint32_t n_inst_records, cap_inst_records;
  glr_job_cache *job_cache;
  uint32_t n_job_cache;
  float *job_scratch;
  glr_target **targets;
  uint32_t n_targets, cap_targets;
  glr_target glare[3];
  int glare_made[3];
  GLRuint layer_vao, layer_position_buffer, layer_uv_buffer, layer_index_buffer;
  GLRuint chain_vao, chain_position_buffer, chain_uv_buffer, chain_index_buffer;
  GLRuint current_prelight;
  wz3_pass_sort_ids sort_ids;
  wz3_draw_list list;
  char error[1024];
};

static glr_result glr__fail(glr *r, glr_result code, const char *what, const char *detail)
{
  size_t n = 0;
  for (; what[n] && n < sizeof r->error - 1; n++) r->error[n] = what[n];
  if (detail) {
    if (n + 2 < sizeof r->error) {
      r->error[n++] = ':';
      r->error[n++] = ' ';
    }
    for (size_t i = 0; detail[i] && n < sizeof r->error - 1; i++) r->error[n++] = detail[i];
  }
  r->error[n] = 0;
  return code;
}

static glr_result glr__load(glr *r, glr_loader_fn load)
{
#define GLR_X(ret, name, args)           \
  r->gl.name = (ret(*) args)load(#name); \
  if (!r->gl.name) return glr__fail(r, GLR_ERR_LOADER, "missing GL function", #name);
  GLR_GL_FUNCTIONS(GLR_X)
#undef GLR_X
  return GLR_OK;
}

/* shader compile and program link of the reference renderer, in the same call order */
static glr_result glr__link(glr *r, int vertex_source, int fragment_source, const char *label, GLRuint *program)
{
  const glr_gl *gl = &r->gl;
  GLRuint shaders[2];
  const int sources[2] = { vertex_source, fragment_source };
  const GLRenum types[2] = { GLR_VERTEX_SHADER, GLR_FRAGMENT_SHADER };
  for (int i = 0; i < 2; i++) {
    shaders[i] = gl->glCreateShader(types[i]);
    gl->glShaderSource(shaders[i], (GLRsizei)glr__shader_sources[sources[i]].n_parts, glr__shader_sources[sources[i]].parts, NULL);
    gl->glCompileShader(shaders[i]);
    GLRint ok = 0;
    gl->glGetShaderiv(shaders[i], GLR_COMPILE_STATUS, &ok);
    if (!ok) {
      char log[512] = "unknown shader compiler error";
      GLRsizei length = 0;
      gl->glGetShaderInfoLog(shaders[i], (GLRsizei)sizeof log - 1, &length, log);
      if (length > 0) log[length < (GLRsizei)sizeof log ? length : (GLRsizei)sizeof log - 1] = 0;
      gl->glDeleteShader(shaders[i]);
      if (i) gl->glDeleteShader(shaders[0]);
      return glr__fail(r, GLR_ERR_SHADER, label, log);
    }
  }
  *program = gl->glCreateProgram();
  gl->glAttachShader(*program, shaders[0]);
  gl->glAttachShader(*program, shaders[1]);
  gl->glLinkProgram(*program);
  gl->glDeleteShader(shaders[0]);
  gl->glDeleteShader(shaders[1]);
  GLRint ok = 0;
  gl->glGetProgramiv(*program, GLR_LINK_STATUS, &ok);
  if (!ok) {
    char log[512] = "unknown program linker error";
    GLRsizei length = 0;
    gl->glGetProgramInfoLog(*program, (GLRsizei)sizeof log - 1, &length, log);
    if (length > 0) log[length < (GLRsizei)sizeof log ? length : (GLRsizei)sizeof log - 1] = 0;
    gl->glDeleteProgram(*program);
    *program = 0;
    return glr__fail(r, GLR_ERR_LINK, label, log);
  }
  return GLR_OK;
}

static void glr__locations(const glr *r, GLRuint program, const char *const *names, int n, GLRint *out)
{
  for (int i = 0; i < n; i++) out[i] = r->gl.glGetUniformLocation(program, names[i]);
}

/* ---- RenderTarget ---- */

static glr_result glr__target_resize(glr *r, glr_target *t, int32_t width, int32_t height)
{
  const glr_gl *gl = &r->gl;
  width = width < 1 ? 1 : width;
  height = height < 1 ? 1 : height;
  if (width == t->width && height == t->height) return GLR_OK;
  t->width = width;
  t->height = height;
  gl->glBindTexture(GLR_TEXTURE_2D, t->color);
  gl->glTexImage2D(GLR_TEXTURE_2D, 0, GLR_RGBA8, width, height, 0, GLR_RGBA, GLR_UNSIGNED_BYTE, NULL);
  gl->glTexParameteri(GLR_TEXTURE_2D, GLR_TEXTURE_MIN_FILTER, GLR_LINEAR);
  gl->glTexParameteri(GLR_TEXTURE_2D, GLR_TEXTURE_MAG_FILTER, GLR_LINEAR);
  gl->glTexParameteri(GLR_TEXTURE_2D, GLR_TEXTURE_WRAP_S, GLR_CLAMP_TO_EDGE);
  gl->glTexParameteri(GLR_TEXTURE_2D, GLR_TEXTURE_WRAP_T, GLR_CLAMP_TO_EDGE);
  gl->glBindFramebuffer(GLR_FRAMEBUFFER, t->framebuffer);
  gl->glFramebufferTexture2D(GLR_FRAMEBUFFER, GLR_COLOR_ATTACHMENT0, GLR_TEXTURE_2D, t->color, 0);
  if (t->has_depth) {
    gl->glBindRenderbuffer(GLR_RENDERBUFFER, t->depth);
    gl->glRenderbufferStorage(GLR_RENDERBUFFER, t->depth_stencil ? GLR_DEPTH24_STENCIL8 : GLR_DEPTH_COMPONENT24, width, height);
    gl->glFramebufferRenderbuffer(GLR_FRAMEBUFFER, t->depth_stencil ? GLR_DEPTH_STENCIL_ATTACHMENT : GLR_DEPTH_ATTACHMENT, GLR_RENDERBUFFER, t->depth);
  }
  const GLRenum status = gl->glCheckFramebufferStatus(GLR_FRAMEBUFFER);
  gl->glBindFramebuffer(GLR_FRAMEBUFFER, 0);
  if (status != GLR_FRAMEBUFFER_COMPLETE) return glr__fail(r, GLR_ERR_FRAMEBUFFER, "framebuffer incomplete", NULL);
  return GLR_OK;
}

static glr_result glr__target_init(glr *r, glr_target *t, int32_t width, int32_t height, int has_depth)
{
  const glr_gl *gl = &r->gl;
  memset(t, 0, sizeof *t);
  gl->glGenFramebuffers(1, &t->framebuffer);
  gl->glGenTextures(1, &t->color);
  t->has_depth = has_depth;
  t->depth_stencil = 1;
  if (has_depth) gl->glGenRenderbuffers(1, &t->depth);
  return glr__target_resize(r, t, width, height);
}

static void glr__target_free(glr *r, glr_target *t)
{
  if (!r->gl.glDeleteTextures) return;
  if (t->depth) r->gl.glDeleteRenderbuffers(1, &t->depth);
  if (t->color) r->gl.glDeleteTextures(1, &t->color);
  if (t->framebuffer) r->gl.glDeleteFramebuffers(1, &t->framebuffer);
  memset(t, 0, sizeof *t);
}

/* ---- Material11 lookup textures ---- */

static glr_result glr__make_lookup_textures(glr *r)
{
  const glr_gl *gl = &r->gl;
  gl->glGenTextures(1, &r->normalizer_cube);
  gl->glGenTextures(1, &r->attenuation_volume);
  gl->glPixelStorei(GLR_UNPACK_ALIGNMENT, 1);
  uint8_t *cube = (uint8_t *)GLR_MALLOC(WZ3_CUBE_NORMALIZER_BYTES);
  uint8_t *volume = (uint8_t *)GLR_MALLOC(WZ3_ATTENUATION_VOLUME_BYTES);
  if (!cube || !volume) {
    GLR_FREE(cube);
    GLR_FREE(volume);
    return glr__fail(r, GLR_ERR_OUT_OF_MEMORY, "lookup texture scratch", NULL);
  }
  wz3_make_cube_normalizer(cube);
  gl->glBindTexture(GLR_TEXTURE_CUBE_MAP, r->normalizer_cube);
  for (int face = 0; face < 6; face++)
    gl->glTexImage2D(GLR_TEXTURE_CUBE_MAP_POSITIVE_X + face, 0, GLR_RGBA8, 64, 64, 0, GLR_RGBA, GLR_UNSIGNED_BYTE, cube + (size_t)face * 64 * 64 * 4);
  gl->glTexParameteri(GLR_TEXTURE_CUBE_MAP, GLR_TEXTURE_MIN_FILTER, GLR_LINEAR);
  gl->glTexParameteri(GLR_TEXTURE_CUBE_MAP, GLR_TEXTURE_MAG_FILTER, GLR_LINEAR);
  gl->glTexParameteri(GLR_TEXTURE_CUBE_MAP, GLR_TEXTURE_WRAP_S, GLR_REPEAT);
  gl->glTexParameteri(GLR_TEXTURE_CUBE_MAP, GLR_TEXTURE_WRAP_T, GLR_REPEAT);
  gl->glTexParameteri(GLR_TEXTURE_CUBE_MAP, GLR_TEXTURE_BASE_LEVEL, 0);
  gl->glTexParameteri(GLR_TEXTURE_CUBE_MAP, GLR_TEXTURE_MAX_LEVEL, 0);
  wz3_make_attenuation_volume(volume);
  gl->glBindTexture(GLR_TEXTURE_3D, r->attenuation_volume);
  gl->glTexImage3D(GLR_TEXTURE_3D, 0, GLR_RGBA8, 32, 32, 32, 0, GLR_RGBA, GLR_UNSIGNED_BYTE, volume);
  gl->glTexParameteri(GLR_TEXTURE_3D, GLR_TEXTURE_MIN_FILTER, GLR_LINEAR);
  gl->glTexParameteri(GLR_TEXTURE_3D, GLR_TEXTURE_MAG_FILTER, GLR_LINEAR);
  gl->glTexParameteri(GLR_TEXTURE_3D, GLR_TEXTURE_WRAP_S, GLR_CLAMP_TO_EDGE);
  gl->glTexParameteri(GLR_TEXTURE_3D, GLR_TEXTURE_WRAP_T, GLR_CLAMP_TO_EDGE);
  gl->glTexParameteri(GLR_TEXTURE_3D, GLR_TEXTURE_WRAP_R, GLR_CLAMP_TO_EDGE);
  gl->glTexParameteri(GLR_TEXTURE_3D, GLR_TEXTURE_BASE_LEVEL, 0);
  gl->glTexParameteri(GLR_TEXTURE_3D, GLR_TEXTURE_MAX_LEVEL, 0);
  GLR_FREE(cube);
  GLR_FREE(volume);
  return GLR_OK;
}

/* ---- lifetime ---- */

static void glr__free_caches(glr *r);

GLR_API void glr_destroy(glr *r)
{
  if (!r) return;
  if (r->gl.glDeleteTextures) {
    const glr_gl *gl = &r->gl;
    glr__free_caches(r);
    glr__target_free(r, &r->master);
    glr__target_free(r, &r->prelight);
    if (r->fallback_texture) gl->glDeleteTextures(1, &r->fallback_texture);
    if (r->overlay_texture) gl->glDeleteTextures(1, &r->overlay_texture);
    if (r->normalizer_cube) gl->glDeleteTextures(1, &r->normalizer_cube);
    if (r->attenuation_volume) gl->glDeleteTextures(1, &r->attenuation_volume);
    if (r->instance_buffer) gl->glDeleteBuffers(1, &r->instance_buffer);
    if (r->shadow_position_buffer) gl->glDeleteBuffers(1, &r->shadow_position_buffer);
    if (r->shadow_extrusion_buffer) gl->glDeleteBuffers(1, &r->shadow_extrusion_buffer);
    if (r->shadow_index_buffer) gl->glDeleteBuffers(1, &r->shadow_index_buffer);
    if (r->shadow_vao) gl->glDeleteVertexArrays(1, &r->shadow_vao);
    if (r->fullscreen_vao) gl->glDeleteVertexArrays(1, &r->fullscreen_vao);
    if (r->program) gl->glDeleteProgram(r->program);
    if (r->shadow_program) gl->glDeleteProgram(r->shadow_program);
    if (r->fullscreen_program) gl->glDeleteProgram(r->fullscreen_program);
  }
  GLR_FREE(r);
}

GLR_API const char *glr_last_error(const glr *r) { return r ? r->error : ""; }

static void glr__export_error(const glr *r, const glr_options *options)
{
  if (!options || !options->error || !options->error_size) return;
  size_t n = 0;
  for (; r->error[n] && n + 1 < options->error_size; n++) options->error[n] = r->error[n];
  options->error[n] = 0;
}

/* The call order is that of the Renderer constructor of the reference renderer. */
GLR_API glr *glr_create(glr_loader_fn load, const glr_options *options, glr_result *res)
{
  glr_result dummy;
  if (!res) res = &dummy;
  if (!load) {
    *res = GLR_ERR_ARGUMENT;
    return NULL;
  }
  glr *r = (glr *)GLR_MALLOC(sizeof *r);
  if (!r) {
    *res = GLR_ERR_OUT_OF_MEMORY;
    return NULL;
  }
  memset(r, 0, sizeof *r);
  if ((*res = glr__load(r, load)) != GLR_OK) {
    glr__export_error(r, options);
    memset(&r->gl, 0, sizeof r->gl);
    glr_destroy(r);
    return NULL;
  }
  const glr_gl *gl = &r->gl;
  r->no_shadows = options ? options->no_shadows : 0;
  wz3_texture_policy_make(&r->tex_policy, WZ3_DXT5_AUTO, options ? options->s3tc : 0);
  wz3_sampler_caps_make(&r->tex_caps, options && options->max_anisotropy > 0.0, options ? options->max_anisotropy : 0.0);
  if ((*res = glr__link(r, GLR__SRC_MATERIAL_VERTEX, GLR__SRC_MATERIAL_FRAGMENT, "Debris material", &r->program)) != GLR_OK) goto fail;
  glr__locations(r, r->program, glr__material_uniforms, GLR__N_MATERIAL_UNIFORMS, r->material_uniforms);
  if ((*res = glr__link(r, GLR__SRC_SHADOW_VERTEX, GLR__SRC_SHADOW_FRAGMENT, "Debris shadow volume", &r->shadow_program)) != GLR_OK) goto fail;
  glr__locations(r, r->shadow_program, glr__shadow_uniforms, 3, r->shadow_uniforms);
  gl->glGenVertexArrays(1, &r->shadow_vao);
  gl->glGenBuffers(1, &r->shadow_position_buffer);
  gl->glGenBuffers(1, &r->shadow_extrusion_buffer);
  gl->glGenBuffers(1, &r->shadow_index_buffer);
  if ((*res = glr__link(r, GLR__SRC_FULLSCREEN_VERTEX, GLR__SRC_FULLSCREEN_FRAGMENT, "Debris IPP", &r->fullscreen_program)) != GLR_OK) goto fail;
  glr__locations(r, r->fullscreen_program, glr__fullscreen_uniforms, GLR__N_FULLSCREEN_UNIFORMS, r->fullscreen_uniforms);
  gl->glGenVertexArrays(1, &r->fullscreen_vao);
  gl->glGenBuffers(1, &r->instance_buffer);
  if ((*res = glr__make_lookup_textures(r)) != GLR_OK) goto fail;
  /* sampler dimensionality must not alias: the lookup volumes live above the four authored maps and the prelight copy */
  gl->glUseProgram(r->program);
  gl->glActiveTexture(GLR_TEXTURE0 + GLR__M11_NORMALIZER_UNIT);
  gl->glBindTexture(GLR_TEXTURE_CUBE_MAP, r->normalizer_cube);
  gl->glActiveTexture(GLR_TEXTURE0 + GLR__M11_ATTENUATION_UNIT);
  gl->glBindTexture(GLR_TEXTURE_3D, r->attenuation_volume);
  gl->glUniform1i(r->material_uniforms[GLR__U_M11_NORMALIZER], GLR__M11_NORMALIZER_UNIT);
  gl->glUniform1i(r->material_uniforms[GLR__U_M11_ATTENUATION], GLR__M11_ATTENUATION_UNIT);
  if ((*res = glr__target_init(r, &r->master, 1, 1, 1)) != GLR_OK) goto fail;
  if ((*res = glr__target_init(r, &r->prelight, 1, 1, 0)) != GLR_OK) goto fail;
  r->canvas_width = r->canvas_height = r->width = r->height = 1;
  r->presentation.width = r->presentation.height = 1;
  *res = GLR_OK;
  return r;
fail:
  glr__export_error(r, options);
  glr_destroy(r);
  return NULL;
}

GLR_API glr_result glr_resize(glr *r, int32_t width, int32_t height, double presentation_aspect)
{
  if (!r) return GLR_ERR_ARGUMENT;
  width = width < 1 ? 1 : width;
  height = height < 1 ? 1 : height;
  r->canvas_width = width;
  r->canvas_height = height;
  wz3_fit_aspect_region(width, height, presentation_aspect, &r->presentation);
  r->width = r->presentation.width;
  r->height = r->presentation.height;
  glr_result res = glr__target_resize(r, &r->master, r->width, r->height);
  if (res == GLR_OK) res = glr__target_resize(r, &r->prelight, r->width, r->height);
  return res;
}

/* ---- passes ---- */

/* ---- texture and geometry caches, instance matrices (TextureCache, GeometryCache, Renderer instance buffer) ---- */

/* Generic helpers */
static GLRfloat glr__f(double v) { return (GLRfloat)v; }
static void *glr__alloc(glr *r, size_t n)
{
  void *p = GLR_MALLOC(n);
  if (!p) r->oom = 1;
  return p;
}
static void glr__note(glr *r, wz3_result res)
{
  if (res == WZ3_ERR_OUT_OF_MEMORY) r->oom = 1;
}
/* storage for at least `need` elements: p itself, or a larger copy (p is released). NULL when out of memory - p and *cap are then untouched,
   so the caller keeps what it had: never assign the result before checking it. */
static void *glr__grow(glr *r, void *p, uint32_t *cap, uint32_t need, size_t element)
{
  if (need <= *cap) return p;
  uint32_t n = *cap ? *cap : 8;
  while (n < need) n *= 2;
  void *q = glr__alloc(r, (size_t)n * element);
  if (!q) return NULL;
  if (p) memcpy(q, p, (size_t)*cap * element);
  GLR_FREE(p);
  *cap = n;
  return q;
}

/* ---- TextureCache ---- */

/* TextureCache.fallbackTexture(): the shared one-texel texture; its sampler state is mutated by binds like in JS */
static GLRuint glr__fallback_texture(glr *r)
{
  const glr_gl *gl = &r->gl;
  wz3_texture_plan plan;
  if (!wz3_texture_fallback(&r->tex_stats, &plan)) return r->fallback_texture;
  gl->glGenTextures(1, &r->fallback_texture);
  gl->glBindTexture(GLR_TEXTURE_2D, r->fallback_texture);
  uint8_t texel[4];
  wz3_texture_fallback_texel(texel);
  for (uint32_t i = 0; i < plan.n_commands; i++) {
    const wz3_texture_command *c = &plan.commands[i];
    if (c->kind == WZ3_TEXCMD_IMAGE)
      gl->glTexImage2D(GLR_TEXTURE_2D, c->level, (GLRint)c->internal_format, c->width, c->height, 0, c->format, c->type, texel);
    else if (c->kind == WZ3_TEXCMD_PARAMETER)
      gl->glTexParameteri(GLR_TEXTURE_2D, c->pname, c->value);
  }
  wz3_sampler_state_fallback(&r->fallback_sampler);
  return r->fallback_texture;
}

static void glr__sampler_calls(glr *r, wz3_sampler_state *state, int mipmapped, const wz3_sampler_request *req)
{
  wz3_sampler_call calls[WZ3_SAMPLER_MAX_CALLS];
  const uint32_t n = wz3_sampler_bind(state, &r->tex_caps, mipmapped, req, calls);
  for (uint32_t i = 0; i < n; i++) {
    if (calls[i].is_float)
      r->gl.glTexParameterf(GLR_TEXTURE_2D, calls[i].pname, glr__f(calls[i].value));
    else
      r->gl.glTexParameteri(GLR_TEXTURE_2D, calls[i].pname, (GLRint)calls[i].value);
  }
}

/* TextureCache.get for the bitmap of operator `op` (bitmap may be NULL); returns the texture to bind, 0 on error */
static GLRuint glr__texture_get(glr *r, const glr_host *host, int32_t op, wz3_sampler_state **sampler, int *mipmapped)
{
  const glr_gl *gl = &r->gl;
  if (op >= 0 && host && (uint32_t)op < host->n_ops && r->tex_by_op && r->tex_by_op[op] && r->tex_by_op[op]->released) {
    glr_tex *final = r->tex_by_op[op]; /* uploaded, pixels given back to the host */
    *sampler = &final->sampler;
    *mipmapped = final->entry.mipmapped;
    return final->texture;
  }
  const wz3_bitmap *bitmap = (op >= 0 && host && host->bitmap && (uint32_t)op < host->n_ops) ? host->bitmap(host->user, (uint32_t)op) : NULL;
  glr_tex *slot = NULL;
  if (bitmap && r->tex_by_op) {
    slot = r->tex_by_op[op];
    if (!slot) {
      slot = (glr_tex *)glr__alloc(r, sizeof *slot);
      if (!slot) return 0;
      memset(slot, 0, sizeof *slot);
      r->tex_by_op[op] = slot;
    }
  }
  wz3_texture_desc desc;
  wz3_texture_desc_from_bitmap(&desc, bitmap);
  if (!slot) {
    *sampler = &r->fallback_sampler;
    *mipmapped = 0;
    return glr__fallback_texture(r);
  }
  wz3_texture_plan plan;
  wz3_texture_action action;
  int32_t delete_old;
  const wz3_texture_entry entry_before = slot->entry;
  const wz3_texture_stats stats_before = r->tex_stats;
  const wz3_result res = wz3_texture_get(&action, &delete_old, &plan, &slot->entry, &r->tex_stats, &r->tex_policy, &desc);
  glr__note(r, res);
  if (res != WZ3_OK) return 0;
  if (action == WZ3_TEXTURE_HIT) {
    *sampler = &slot->sampler;
    *mipmapped = slot->entry.mipmapped;
    return slot->texture;
  }
  if (action == WZ3_TEXTURE_FALLBACK) {
    if (delete_old) {
      gl->glDeleteTextures(1, &slot->texture);
      slot->texture = 0;
    }
    *sampler = &r->fallback_sampler;
    *mipmapped = 0;
    return glr__fallback_texture(r);
  }
  GLRuint texture = 0;
  gl->glGenTextures(1, &texture);
  gl->glBindTexture(GLR_TEXTURE_2D, texture);
  wz3_texture_image img;
  const wz3_result prepared = wz3_texture_prepare(&img, bitmap, plan.allow_bc3);
  glr__note(r, prepared);
  if (prepared != WZ3_OK) { /* the cache goes back to what it was */
    gl->glDeleteTextures(1, &texture);
    slot->entry = entry_before;
    r->tex_stats = stats_before;
    return 0;
  }
  for (uint32_t i = 0; i < plan.n_commands; i++) {
    const wz3_texture_command *c = &plan.commands[i];
    switch (c->kind) {
    case WZ3_TEXCMD_PIXEL_STORE: gl->glPixelStorei(c->pname, c->value); break;
    case WZ3_TEXCMD_IMAGE:
      gl->glTexImage2D(GLR_TEXTURE_2D, c->level, (GLRint)c->internal_format, c->width, c->height, 0, c->format, c->type,
                       img.data + img.levels[c->level].offset);
      break;
    case WZ3_TEXCMD_COMPRESSED:
      gl->glCompressedTexImage2D(GLR_TEXTURE_2D, c->level, c->internal_format, c->width, c->height, 0, (GLRsizei)c->size,
                                 img.data + img.levels[c->level].offset);
      break;
    case WZ3_TEXCMD_GENERATE_MIPMAP: gl->glGenerateMipmap(GLR_TEXTURE_2D); break;
    default: gl->glTexParameteri(GLR_TEXTURE_2D, c->pname, c->value); break;
    }
  }
  wz3_texture_release(&img);
  if (delete_old) gl->glDeleteTextures(1, &slot->texture);
  slot->texture = texture;
  wz3_sampler_state_after_plan(&slot->sampler, &plan);
  *sampler = &slot->sampler;
  *mipmapped = slot->entry.mipmapped;
  if (host->bitmap_uploaded) {
    host->bitmap_uploaded(host->user, (uint32_t)op);
    slot->released = 1;
  }
  return texture;
}

/* TextureCache.bind(bitmap, unit, options) */
static void glr__texture_bind(glr *r, const glr_host *host, int32_t op, uint32_t unit, const wz3_sampler_request *req)
{
  const glr_gl *gl = &r->gl;
  gl->glActiveTexture(GLR_TEXTURE0 + unit);
  wz3_sampler_state *sampler = NULL;
  int mipmapped = 0;
  const GLRuint texture = glr__texture_get(r, host, op, &sampler, &mipmapped);
  gl->glBindTexture(GLR_TEXTURE_2D, texture);
  if (sampler) glr__sampler_calls(r, sampler, mipmapped, req);
}

/* ---- GeometryCache ---- */

typedef struct glr_streams {
  const void *data[7];
  size_t bytes[7];
  uint8_t type[7];
  uint32_t n_vertices, n_indices;
  const wz3_geometry_group *groups;
  uint32_t n_groups;
  uint32_t dynamic_mask; /* bit per stream; meaningful when partial */
  int partial;           /* the producer names the streams it changes (dynamicAttributes) */
  int uv1_is_uvs;
  const wz3_geometry *g;
  const void *shadow_id;
  uint32_t version;
  int has_version;
} glr_streams;

static const char *const glr__stream_names[7] = { "positions", "normals", "uvs", "colors", "tangents", "uv1", "indices" };
enum { GLR_S_POSITIONS, GLR_S_NORMALS, GLR_S_UVS, GLR_S_COLORS, GLR_S_TANGENTS, GLR_S_UV1, GLR_S_INDICES };
enum { GLR_T_F32, GLR_T_U8, GLR_T_U16, GLR_T_U32 };

/* normalizePreparedGeometry of a prepared geometry (Mesh / MinMesh); the 16-bit index stream is made at upload time */
static void glr__streams_from_geometry(glr_streams *s, const wz3_geometry *g, const void *shadow_id)
{
  memset(s, 0, sizeof *s);
  const size_t n = g->n_vertices;
  s->n_vertices = g->n_vertices;
  s->n_indices = g->n_indices;
  s->data[GLR_S_POSITIONS] = g->positions;
  s->bytes[GLR_S_POSITIONS] = n * 12;
  s->type[GLR_S_POSITIONS] = GLR_T_F32;
  s->data[GLR_S_NORMALS] = g->normals;
  s->bytes[GLR_S_NORMALS] = n * 12;
  s->type[GLR_S_NORMALS] = GLR_T_F32;
  s->data[GLR_S_UVS] = g->uv0;
  s->bytes[GLR_S_UVS] = n * 8;
  s->type[GLR_S_UVS] = GLR_T_F32;
  if (g->colors_u8) {
    s->data[GLR_S_COLORS] = g->colors_u8;
    s->bytes[GLR_S_COLORS] = n * 4;
    s->type[GLR_S_COLORS] = GLR_T_U8;
  } else {
    s->data[GLR_S_COLORS] = g->colors;
    s->bytes[GLR_S_COLORS] = n * 16;
    s->type[GLR_S_COLORS] = GLR_T_F32;
  }
  s->data[GLR_S_TANGENTS] = g->tangents;
  s->bytes[GLR_S_TANGENTS] = n * 16;
  s->type[GLR_S_TANGENTS] = GLR_T_F32;
  s->uv1_is_uvs = g->uv1_is_uv0;
  s->data[GLR_S_UV1] = g->uv1_is_uv0 ? g->uv0 : g->uv1;
  s->bytes[GLR_S_UV1] = n * 8;
  s->type[GLR_S_UV1] = GLR_T_F32;
  const int wide = !g->colors_u8 || g->n_vertices > 0xffffu; /* Mesh indices are Uint32, MinMesh ones Uint16 up to 65535 vertices */
  s->data[GLR_S_INDICES] = g->indices;
  s->bytes[GLR_S_INDICES] = (size_t)g->n_indices * (wide ? 4u : 2u);
  s->type[GLR_S_INDICES] = wide ? GLR_T_U32 : GLR_T_U16;
  s->groups = g->groups;
  s->n_groups = g->n_groups;
  s->g = g;
  s->shadow_id = shadow_id;
}

static GLRenum glr__stream_gl_type(uint8_t t) { return t == GLR_T_U8 ? GLR_UNSIGNED_BYTE : GLR_FLOAT; }

/* the bytes of stream `i` as the buffer holds them (16-bit indices are narrowed here); *owned receives a block to free */
static const void *glr__stream_bytes(glr *r, const glr_streams *s, int i, void **owned)
{
  *owned = NULL;
  if (i != GLR_S_INDICES || s->type[i] != GLR_T_U16 || !s->g) return s->data[i];
  uint16_t *narrow = (uint16_t *)glr__alloc(r, (size_t)s->n_indices * 2u + 2u);
  if (!narrow) return NULL;
  for (uint32_t k = 0; k < s->n_indices; k++) narrow[k] = (uint16_t)s->g->indices[k];
  *owned = narrow;
  return narrow;
}

static glr_geom *glr__geom_create(glr *r, const glr_streams *s, int dynamic)
{
  const glr_gl *gl = &r->gl;
  glr_geom *e = (glr_geom *)glr__alloc(r, sizeof *e);
  if (!e) return NULL;
  memset(e, 0, sizeof *e);
  static const struct {
    int size, location;
  } attributes[6] = { { 3, 0 }, { 3, 1 }, { 2, 2 }, { 4, 3 }, { 4, 4 }, { 2, 9 } };
  uint32_t mask = s->dynamic_mask;
  if (s->partial && (mask >> GLR_S_UVS & 1u) && s->uv1_is_uvs) mask |= 1u << GLR_S_UV1;
  gl->glGenVertexArrays(1, &e->vao);
  gl->glBindVertexArray(e->vao);
  for (int i = 0; i < 6; i++) {
    gl->glGenBuffers(1, &e->buf[i]);
    gl->glBindBuffer(GLR_ARRAY_BUFFER, e->buf[i]);
    const int dyn = dynamic || (s->partial && (mask >> i & 1u));
    gl->glBufferData(GLR_ARRAY_BUFFER, (GLRsizeiptr)s->bytes[i], s->data[i], dyn ? GLR_DYNAMIC_DRAW : GLR_STATIC_DRAW);
    gl->glEnableVertexAttribArray((GLRuint)attributes[i].location);
    gl->glVertexAttribPointer((GLRuint)attributes[i].location, attributes[i].size, glr__stream_gl_type(s->type[i]),
                              (GLRboolean)(i == GLR_S_COLORS && s->type[i] == GLR_T_U8), 0, NULL);
  }
  void *owned;
  gl->glGenBuffers(1, &e->buf[GLR_S_INDICES]);
  gl->glBindBuffer(GLR_ELEMENT_ARRAY_BUFFER, e->buf[GLR_S_INDICES]);
  const void *idx = glr__stream_bytes(r, s, GLR_S_INDICES, &owned);
  const int dyn_idx = dynamic || (s->partial && (mask >> GLR_S_INDICES & 1u));
  gl->glBufferData(GLR_ELEMENT_ARRAY_BUFFER, (GLRsizeiptr)s->bytes[GLR_S_INDICES], idx, dyn_idx ? GLR_DYNAMIC_DRAW : GLR_STATIC_DRAW);
  GLR_FREE(owned);
  gl->glBindBuffer(GLR_ARRAY_BUFFER, r->instance_buffer);
  for (int column = 0; column < 4; column++) {
    gl->glEnableVertexAttribArray((GLRuint)(5 + column));
    gl->glVertexAttribPointer((GLRuint)(5 + column), 4, GLR_FLOAT, 0, 64, (const void *)(uintptr_t)(column * 16));
    gl->glVertexAttribDivisor((GLRuint)(5 + column), 1);
  }
  gl->glBindVertexArray(0);
  e->dynamic = dynamic || s->partial;
  return e;
}

static void glr__geom_remember(glr_geom *e, const glr_streams *s)
{
  for (int i = 0; i < 7; i++) {
    e->bytes[i] = s->bytes[i];
    e->type[i] = s->type[i];
  }
  e->positions_id = s->data[GLR_S_POSITIONS];
  e->indices_id = s->data[GLR_S_INDICES];
  e->shadow_id = s->shadow_id;
  e->n_vertices = s->n_vertices;
  e->n_indices = s->n_indices;
  e->index_type = s->type[GLR_S_INDICES] == GLR_T_U16 ? GLR_UNSIGNED_SHORT : GLR_UNSIGNED_INT;
  e->index_bytes = s->type[GLR_S_INDICES] == GLR_T_U16 ? 2u : 4u;
  e->g = s->g;
  e->version = s->version;
}

static int glr__geom_copy_groups(glr *r, glr_geom *e, const glr_streams *s)
{
  wz3_geometry_group *copy = (wz3_geometry_group *)glr__alloc(r, sizeof *copy * (s->n_groups ? s->n_groups : 1u));
  if (!copy) return 0;
  if (s->n_groups) memcpy(copy, s->groups, sizeof *copy * s->n_groups);
  GLR_FREE(e->groups);
  e->groups = copy;
  e->n_groups = s->n_groups;
  return 1;
}

static void glr__topologies_free(glr_geom *e)
{
  for (uint32_t i = 0; i < e->n_topos; i++) {
    wz3_shadow_topology_free(&e->topos[i].t);
    GLR_FREE(e->topos[i].key);
  }
  e->n_topos = 0;
}

/* GeometryCache.updateEntry */
static int glr__geom_update(glr *r, glr_geom *e, const glr_streams *s, int dynamic)
{
  const glr_gl *gl = &r->gl;
  const int partial = s->partial;
  uint32_t dirty = partial ? s->dynamic_mask : 0xffffffffu;
  if (partial && (dirty >> GLR_S_UVS & 1u) && s->uv1_is_uvs) dirty |= 1u << GLR_S_UV1;
  if (partial)
    for (int i = 0; i < 7; i++)
      if (e->bytes[i] != s->bytes[i] || e->type[i] != s->type[i]) dirty |= 1u << i;
  /* refreshedShadowTopologies */
  int keep_topologies = 0;
  if (partial && !(dirty >> GLR_S_INDICES & 1u) && e->indices_id == s->data[GLR_S_INDICES] && e->shadow_id == s->shadow_id && e->g && s->g &&
      wz3_shadow_group_structure_matches(e->groups, e->n_groups, s->groups, s->n_groups)) {
    keep_topologies = 1;
    const int positions_changed = (dirty >> GLR_S_POSITIONS & 1u) || e->positions_id != s->data[GLR_S_POSITIONS];
    if (positions_changed) {
      for (uint32_t i = 0; i < e->n_topos && keep_topologies; i++) {
        wz3_shadow_topology *t = &e->topos[i].t;
        if (!t->uses_shadow_vertex_map || !wz3_shadow_topology_refresh_positions(t, (const float *)s->data[GLR_S_POSITIONS], s->n_vertices))
          keep_topologies = 0;
      }
    }
  }
  if (!keep_topologies) glr__topologies_free(e);
  gl->glBindVertexArray(e->vao);
  for (int i = 0; i < 7; i++) {
    if (!(dirty >> i & 1u)) continue;
    const GLRenum target = i == GLR_S_INDICES ? GLR_ELEMENT_ARRAY_BUFFER : GLR_ARRAY_BUFFER;
    gl->glBindBuffer(target, e->buf[i]);
    void *owned;
    const void *bytes = glr__stream_bytes(r, s, i, &owned);
    gl->glBufferData(target, (GLRsizeiptr)s->bytes[i], bytes, (dynamic || partial) ? GLR_DYNAMIC_DRAW : GLR_STATIC_DRAW);
    GLR_FREE(owned);
    if (i == GLR_S_COLORS) {
      const int packed = s->type[i] == GLR_T_U8;
      gl->glVertexAttribPointer(3, 4, packed ? GLR_UNSIGNED_BYTE : GLR_FLOAT, (GLRboolean)packed, 0, NULL);
    }
  }
  gl->glBindVertexArray(0);
  if (!glr__geom_copy_groups(r, e, s)) return 0;
  e->dynamic = e->dynamic || dynamic || partial;
  glr__geom_remember(e, s);
  return 1;
}

static void glr__geom_destroy(glr *r, glr_geom *e)
{
  if (!e) return;
  const glr_gl *gl = &r->gl;
  for (int i = 0; i < 7; i++)
    if (e->buf[i]) gl->glDeleteBuffers(1, &e->buf[i]);
  if (e->vao) gl->glDeleteVertexArrays(1, &e->vao);
  glr__topologies_free(e);
  GLR_FREE(e->topos);
  GLR_FREE(e->groups);
  GLR_FREE(e);
}

static int glr__geom_register(glr *r, glr_geom *e)
{
  glr_geom **all = (glr_geom **)glr__grow(r, r->all_geoms, &r->cap_all_geoms, r->n_all_geoms + 1, sizeof *all);
  if (!all) return 0;
  r->all_geoms = all;
  r->all_geoms[r->n_all_geoms++] = e;
  return 1;
}

/* GeometryCache.get for a static geometry (version equal: a hit) of operator `op` */
static glr_geom *glr__geom_get_static(glr *r, uint32_t op, const wz3_geometry *g, const wz3_shadow_data *shadow)
{
  glr_geom *e = r->geom_by_op[op];
  if (e && e->g == g) return e;
  glr_streams s;
  glr__streams_from_geometry(&s, g, shadow);
  if (e) {
    glr__geom_update(r, e, &s, 0);
    return e;
  }
  e = glr__geom_create(r, &s, 0);
  if (!e) return NULL;
  glr__geom_remember(e, &s);
  if (!glr__geom_copy_groups(r, e, &s) || !glr__geom_register(r, e)) {
    glr__geom_destroy(r, e);
    return NULL;
  }
  r->geom_by_op[op] = e;
  return e;
}

/* GeometryCache.get for an animated MinMesh: the pose of `time` in the next slot of the operator's pool */
static glr_geom *glr__geom_get_animated(glr *r, const glr_host *host, uint32_t op, double time, const wz3_shadow_data *shadow)
{
  glr_pool *pool = r->pool_by_op[op];
  if (!pool) {
    pool = (glr_pool *)glr__alloc(r, sizeof *pool);
    if (!pool) return NULL;
    memset(pool, 0, sizeof *pool);
    pool->frame = -1;
    r->pool_by_op[op] = pool;
  }
  if (pool->frame != r->geometry_frame) {
    pool->frame = r->geometry_frame;
    pool->used = 0;
    pool->n_times = 0;
  }
  const double key = time == time ? time : 0.0;
  for (uint32_t i = 0; i < pool->n_times; i++)
    if (pool->times[i] == key) return pool->by_time[i];
  const wz3_geometry *g = host->geometry(host->user, op, key, (int32_t)pool->used);
  if (!g) return NULL;
  glr_streams s;
  glr__streams_from_geometry(&s, g, shadow);
  s.partial = 1; /* MinMesh.prepare({ time }) declares positions, normals and tangents */
  s.dynamic_mask = 1u << GLR_S_POSITIONS | 1u << GLR_S_NORMALS | 1u << GLR_S_TANGENTS;
  glr_geom *e;
  if (pool->used < pool->n_entries) {
    e = pool->entries[pool->used];
    if (!glr__geom_update(r, e, &s, 1)) return NULL;
  } else {
    e = glr__geom_create(r, &s, 1);
    if (!e) return NULL;
    glr__geom_remember(e, &s);
    if (!glr__geom_copy_groups(r, e, &s)) {
      glr__geom_destroy(r, e);
      return NULL;
    }
    glr_geom **entries = (glr_geom **)glr__grow(r, pool->entries, &pool->cap_entries, pool->n_entries + 1, sizeof *entries);
    if (!entries || !glr__geom_register(r, e)) {
      glr__geom_destroy(r, e);
      return NULL;
    }
    pool->entries = entries;
    pool->entries[pool->n_entries++] = e;
  }
  double *times = (double *)glr__grow(r, pool->times, &pool->cap_times, pool->n_times + 1, sizeof *times);
  if (!times) return NULL;
  pool->times = times;
  glr_geom **by_time = (glr_geom **)glr__grow(r, pool->by_time, &pool->cap_by_time, pool->n_times + 1, sizeof *by_time);
  if (!by_time) return NULL;
  pool->by_time = by_time;
  pool->used++;
  pool->times[pool->n_times] = key;
  pool->by_time[pool->n_times++] = e;
  return e;
}

/* GeometryCache.get(job.mesh, job.time, job.opId) */
static glr_geom *glr__geometry_for_job(glr *r, const glr_host *host, const wz3_mesh_job *job)
{
  const uint32_t op = (uint32_t)job->mesh_op;
  if (!host->geometry || op >= host->n_ops) return NULL;
  const wz3_geometry *base = host->geometry(host->user, op, -1.0, 0);
  if (!base) return NULL;
  const wz3_shadow_data *shadow = host->shadow_data ? host->shadow_data(host->user, op) : NULL;
  if (base->animated) return glr__geom_get_animated(r, host, op, (double)job->time, shadow);
  return glr__geom_get_static(r, op, base, shadow);
}

/* ---- the instance buffer (Renderer.prepareInstanceUploads and friends) ---- */

enum { GLR_INSTANCE_MATRIX_BYTES = 64, GLR_MAX_RETAINED_INSTANCE_BUFFER = 4 * 1024 * 1024 };

typedef struct glr_instances {
  const float *data;
  uint32_t count; /* floats */
} glr_instances;

static uint32_t glr__instance_buffer_size(const glr *r, uint64_t required)
{
  if (required < GLR_INSTANCE_MATRIX_BYTES) required = GLR_INSTANCE_MATRIX_BYTES;
  const uint64_t aligned = (required + 63u) / 64u * 64u;
  if (aligned > GLR_MAX_RETAINED_INSTANCE_BUFFER) return (uint32_t)aligned;
  uint64_t rounded = 1;
  while (rounded < aligned) rounded *= 2;
  if (rounded > GLR_MAX_RETAINED_INSTANCE_BUFFER) rounded = GLR_MAX_RETAINED_INSTANCE_BUFFER;
  const uint64_t retained = r->instance_capacity;
  if (retained >= aligned && retained <= rounded * 4 && retained <= GLR_MAX_RETAINED_INSTANCE_BUFFER) return (uint32_t)retained;
  return (uint32_t)rounded;
}

static glr_inst_record *glr__instance_record_of(glr *r, const void *job)
{
  for (uint32_t i = 0; i < r->n_inst_records; i++)
    if (r->inst_records[i].job == job) return &r->inst_records[i];
  return NULL;
}

static void glr__ensure_instance_capacity(glr *r, uint64_t required)
{
  if (required <= r->instance_capacity) return;
  const glr_gl *gl = &r->gl;
  r->instance_capacity = glr__instance_buffer_size(r, required);
  gl->glBindBuffer(GLR_ARRAY_BUFFER, r->instance_buffer);
  gl->glBufferData(GLR_ARRAY_BUFFER, (GLRsizeiptr)r->instance_capacity, NULL, GLR_DYNAMIC_DRAW);
  for (uint32_t i = 0; i < r->n_inst_records; i++)
    gl->glBufferSubData(GLR_ARRAY_BUFFER, (GLRintptr)r->inst_records[i].offset, (GLRsizeiptr)r->inst_records[i].bytes, r->inst_records[i].data);
}

static glr_inst_record *glr__upload_instances(glr *r, const void *job, const float *data, uint32_t bytes)
{
  glr_inst_record *cached = job ? glr__instance_record_of(r, job) : NULL;
  if (cached && cached->data == data) return cached;
  const uint32_t offset = r->instance_used;
  glr__ensure_instance_capacity(r, (uint64_t)offset + bytes);
  const glr_gl *gl = &r->gl;
  gl->glBindBuffer(GLR_ARRAY_BUFFER, r->instance_buffer);
  gl->glBufferSubData(GLR_ARRAY_BUFFER, (GLRintptr)offset, (GLRsizeiptr)bytes, data);
  glr_inst_record *records = (glr_inst_record *)glr__grow(r, r->inst_records, &r->cap_inst_records, r->n_inst_records + 1, sizeof *records);
  if (!records) return NULL;
  r->inst_records = records;
  glr_inst_record *record = &r->inst_records[r->n_inst_records++];
  record->job = job;
  record->data = data;
  record->bytes = bytes;
  record->offset = offset;
  r->instance_used += bytes;
  return record;
}

static void glr__bind_instances(glr *r, const void *job, const float *data, uint32_t floats)
{
  const glr_gl *gl = &r->gl;
  uint32_t offset = 0;
  gl->glBindBuffer(GLR_ARRAY_BUFFER, r->instance_buffer);
  if (r->instance_active) {
    glr_inst_record *record = glr__upload_instances(r, job, data, floats * 4u);
    if (record) offset = record->offset;
  } else {
    gl->glBufferData(GLR_ARRAY_BUFFER, (GLRsizeiptr)floats * 4, data, GLR_DYNAMIC_DRAW);
  }
  for (int column = 0; column < 4; column++)
    gl->glVertexAttribPointer((GLRuint)(5 + column), 4, GLR_FLOAT, 0, GLR_INSTANCE_MATRIX_BYTES, (const void *)(uintptr_t)(offset + (uint32_t)column * 16u));
}

/* ---- materials: configureMaterialState and bindMaterial ---- */

static GLRenum glr__depth_function(int f)
{
  switch (f) {
  case WZ3_DEPTH_EQUAL: return GLR_EQUAL;
  case WZ3_DEPTH_LESS: return GLR_LESS;
  case WZ3_DEPTH_LEQUAL: return GLR_LEQUAL;
  default: return GLR_ALWAYS;
  }
}

static GLRenum glr__blend_factor(int f)
{
  switch (f) {
  case WZ3_BLEND_ZERO: return GLR_ZERO;
  case WZ3_BLEND_ONE: return GLR_ONE;
  case WZ3_BLEND_SRC_ALPHA: return GLR_SRC_ALPHA;
  case WZ3_BLEND_ONE_MINUS_SRC_ALPHA: return GLR_ONE_MINUS_SRC_ALPHA;
  case WZ3_BLEND_SRC_COLOR: return GLR_SRC_COLOR;
  case WZ3_BLEND_ONE_MINUS_SRC_COLOR: return GLR_ONE_MINUS_SRC_COLOR;
  case WZ3_BLEND_DST_COLOR: return GLR_DST_COLOR;
  case WZ3_BLEND_ONE_MINUS_DST_COLOR: return GLR_ONE_MINUS_DST_COLOR;
  default: return GLR_DST_ALPHA;
  }
}

#define GLR_LEGACY_DEPTH_BIAS    (1.0 / 65536.0)
#define GLR_DEPTH24_OFFSET_UNITS (GLR_LEGACY_DEPTH_BIAS * 16777216.0)

static void glr__configure_material_state(glr *r, const wz3_material_view *view)
{
  const glr_gl *gl = &r->gl;
  wz3_material_state state;
  wz3_material_state_make(view, &state);
  if (state.depth_test)
    gl->glEnable(GLR_DEPTH_TEST);
  else
    gl->glDisable(GLR_DEPTH_TEST);
  gl->glDepthFunc(glr__depth_function(state.depth_func));
  gl->glDepthMask(state.depth_write);
  const double depth_bias = (view->base_flags & WZ3_MBF_ZBIASFORE)   ? -GLR_LEGACY_DEPTH_BIAS
                            : (view->base_flags & WZ3_MBF_ZBIASBACK) ? GLR_LEGACY_DEPTH_BIAS
                                                                     : 0.0;
  if (depth_bias != 0.0) {
    gl->glEnable(GLR_POLYGON_OFFSET_FILL);
    gl->glPolygonOffset(glr__f(depth_bias), glr__f(depth_bias < 0 ? -GLR_DEPTH24_OFFSET_UNITS : GLR_DEPTH24_OFFSET_UNITS));
  } else
    gl->glDisable(GLR_POLYGON_OFFSET_FILL);
  gl->glFrontFace(GLR_CW);
  if (state.cull == WZ3_CULL_NONE)
    gl->glDisable(GLR_CULL_FACE);
  else {
    gl->glEnable(GLR_CULL_FACE);
    gl->glCullFace(state.cull == WZ3_CULL_FRONT ? GLR_FRONT : GLR_BACK);
  }
  if (!state.blend)
    gl->glDisable(GLR_BLEND);
  else {
    gl->glEnable(GLR_BLEND);
    gl->glBlendEquation(state.blend_equation == WZ3_BLEND_EQ_REVERSE_SUBTRACT ? GLR_FUNC_REVERSE_SUBTRACT : GLR_FUNC_ADD);
    gl->glBlendFunc(glr__blend_factor(state.blend_src), glr__blend_factor(state.blend_dst));
  }
  gl->glColorMask(state.color_write, state.color_write, state.color_write, state.color_write);
  if (state.stencil_test) {
    gl->glEnable(GLR_STENCIL_TEST);
    gl->glStencilMask(0);
    gl->glStencilFunc(GLR_EQUAL, 0, 0xff);
    gl->glStencilOp(GLR_KEEP, GLR_KEEP, GLR_KEEP);
  } else
    gl->glDisable(GLR_STENCIL_TEST);
}

/* colorRGB: a byte over 255, stored as a float */
static void glr__color_argb(uint32_t v, float out[4])
{
  out[0] = glr__f((double)((v >> 16) & 255u) / 255.0);
  out[1] = glr__f((double)((v >> 8) & 255u) / 255.0);
  out[2] = glr__f((double)(v & 255u) / 255.0);
  out[3] = glr__f((double)(v >> 24) / 255.0);
}
static void glr__color_rgb(uint32_t v, float out[3])
{
  out[0] = glr__f((double)((v >> 16) & 255u) / 255.0);
  out[1] = glr__f((double)((v >> 8) & 255u) / 255.0);
  out[2] = glr__f((double)(v & 255u) / 255.0);
}

static const float glr__identity_matrix[16] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 };
static const float glr__identity_uv_transform[8] = { 1, 0, 0, 0, 0, 1, 0, 0 };

/* Renderer.bindMaterial. lights: the lights of the pass (at most WZ3_MAX_LIGHTS). */
static void glr__bind_material(glr *r, const glr_host *host, const wz3_material_view *view, const wz3_ipp_node *viewport, const wz3_camera_mats *matrices,
                               const wz3_light_job *const *lights, uint32_t n_lights, const float *model_matrix, int animated_mesh, int shader_instanced)
{
  const glr_gl *gl = &r->gl;
  const GLRint *u = r->material_uniforms;
  float base_color[4];
  glr__color_argb((view->system == 20 && view->usage == WZ3_USAGE_AMBIENT) ? 0xffffffffu : view->color, base_color);
  if (view->generated_base_alpha == WZ3_GENERATED_ALPHA_ZERO)
    base_color[3] = 0.0f;
  else if (view->generated_base_alpha == WZ3_GENERATED_ALPHA_TEXTURE0)
    base_color[3] = 1.0f;
  const double light_scale = view->light_scale != 0.0 ? view->light_scale : 1.0;
  if (light_scale != 1.0)
    for (int i = 0; i < 3; i++) base_color[i] = glr__f((double)base_color[i] * light_scale);
  gl->glUniform4fv(u[GLR__U_BASE_COLOR], 1, base_color);
  const int mode = wz3_material_render_mode(view);
  gl->glUniform1i(u[GLR__U_MODE], mode);
  gl->glUniform1i(u[GLR__U_VERTEX_COLOR_MODE], view->vertex_color_mode);
  const int material20 = view->system == 20, material11 = view->system == 11;
  const float *legacy_matrix = (material20 || material11) && model_matrix ? model_matrix : NULL;
  const int legacy_lighting = mode == 2 && (material20 || material11);
  const int legacy_environment = mode == 4 && material20;
  const int condition_animated_basis = animated_mesh && !shader_instanced;
  gl->glUniform1i(u[GLR__U_LEGACY_LIGHTING_MODE], legacy_lighting ? (condition_animated_basis ? 2 : 1) : legacy_environment ? 3 : 0);
  gl->glUniform1i(u[GLR__U_CONDITION_LEGACY_BASIS], material11 && condition_animated_basis ? 1 : 0);
  gl->glUniform1i(u[GLR__U_MATERIAL20], material20);
  gl->glUniform1i(u[GLR__U_M20_FLAGS], material20 ? (GLRint)view->flags : 0);
  gl->glUniform1i(u[GLR__U_M20_ENVIRONMENT_FLAGS], material20 ? (GLRint)view->environment_flags : 0);
  gl->glUniform1i(u[GLR__U_M20_RUNTIME_ENVIRONMENT_FLAGS], material20 ? (GLRint)view->runtime_environment_flags : 0);
  gl->glUniform1i(u[GLR__U_MATERIAL11], material11);
  gl->glUniform1i(u[GLR__U_M11_MULTIPASS_LIGHT], material11 && mode == 2 ? 1 : 0);
  gl->glUniformMatrix4fv(u[GLR__U_M11_VIEW], 1, 0, matrices->view);
  float world_to_model[16];
  const float *world_to_model_ptr = glr__identity_matrix;
  if (model_matrix && wz3_mat4_inverse(model_matrix, world_to_model)) world_to_model_ptr = world_to_model;
  gl->glUniformMatrix4fv(u[GLR__U_M11_WORLD_TO_MODEL], 1, 0, world_to_model_ptr);
  gl->glUniformMatrix4fv(u[GLR__U_M11_MODEL_TO_WORLD], 1, 0, legacy_matrix ? legacy_matrix : glr__identity_matrix);
  gl->glUniform1i(u[GLR__U_M11_SPECIAL_FLAGS], material11 ? (GLRint)view->special_flags : 0);
  GLRint detail[2] = { view->detail_ops[0], view->detail_ops[1] };
  gl->glUniform2iv(u[GLR__U_DETAIL_OPS], 1, detail);
  GLRint combiners[13];
  for (int i = 0; i < 13; i++) combiners[i] = (GLRint)view->combiners[i];
  gl->glUniform1iv(u[GLR__U_COMBINERS], 13, combiners);
  gl->glUniform1i(u[GLR__U_ALPHA_COMBINER], (GLRint)view->alpha_combiner);
  float material_colors[16];
  for (int i = 0; i < 4; i++) glr__color_argb(material20 ? 0xffffffffu : view->colors[i], material_colors + i * 4);
  gl->glUniform4fv(u[GLR__U_MATERIAL_COLORS], 4, material_colors);
  gl->glUniform1f(u[GLR__U_ALPHA_CUTOFF], glr__f(view->alpha_cutoff != 0.0 ? view->alpha_cutoff : -1.0));
  float specular_color[3];
  glr__color_rgb(view->specular_color, specular_color);
  double specular_power = view->specular_power != 0.0 ? view->specular_power : 8.0;
  if (light_scale != 1.0) {
    for (int i = 0; i < 3; i++) specular_color[i] = glr__f((double)specular_color[i] * light_scale);
    specular_power *= light_scale;
  }
  gl->glUniform1f(u[GLR__U_SPECULAR_POWER], glr__f(specular_power));
  gl->glUniform3fv(u[GLR__U_SPECULAR_COLOR], 1, specular_color);
  gl->glUniform1f(u[GLR__U_SPECULAR_STRENGTH], glr__f(view->specular_strength));
  float ambient[3];
  glr__color_rgb(viewport->ambient, ambient);
  gl->glUniform3fv(u[GLR__U_AMBIENT], 1, ambient);
  const float *camera_space = viewport->camera.camera_space;
  gl->glUniform3f(u[GLR__U_CAMERA_POSITION], camera_space[12], camera_space[13], camera_space[14]);
  float material_camera[3];
  if (legacy_matrix) {
    const double position[3] = { camera_space[12], camera_space[13], camera_space[14] };
    wz3_legacy_transr_vector(legacy_matrix, position, 1, material_camera);
  } else {
    material_camera[0] = camera_space[12];
    material_camera[1] = camera_space[13];
    material_camera[2] = camera_space[14];
  }
  gl->glUniform3fv(u[GLR__U_MATERIAL_CAMERA_POSITION], 1, material_camera);
  float fog_color[3];
  glr__color_rgb(viewport->camera.fog_color, fog_color);
  gl->glUniform3fv(u[GLR__U_FOG_COLOR], 1, fog_color);
  gl->glUniform2f(u[GLR__U_FOG_RANGE], glr__f(viewport->camera.fog_start), glr__f(viewport->camera.fog_end));
  gl->glUniform1i(u[GLR__U_FOG_ENABLED], (view->base_flags & 0x40u) ? 1 : 0);

  wz3_sampler_setup setup;
  wz3_material_sampler_setup(view, &setup);
  gl->glUniform4iv(u[GLR__U_M20_SAMPLER_FLAGS], 1, setup.m20_flags);
  gl->glUniform4fv(u[GLR__U_M20_SAMPLER_SCALES], 1, setup.m20_scales);
  gl->glUniform4fv(u[GLR__U_M20_UV_TRANSFORM1], 2, material20 ? view->uv_transform1 : glr__identity_uv_transform);
  gl->glUniform4fv(u[GLR__U_M20_UV_TRANSFORM2], 2, material20 ? view->uv_transform2 : glr__identity_uv_transform);
  gl->glUniform4iv(u[GLR__U_M11_SAMPLER_FLAGS], 1, setup.m11_flags);
  gl->glUniform4fv(u[GLR__U_M11_SAMPLER_SCALES], 1, setup.m11_scales);
  gl->glUniform4fv(u[GLR__U_M11_UV_TRANSFORM1], 2, material11 ? view->uv_transform1 : glr__identity_uv_transform);
  gl->glUniform4fv(u[GLR__U_M11_UV_TRANSFORM2], 2, material11 ? view->uv_transform2 : glr__identity_uv_transform);
  static const char *const texture_uniforms[4] = { "uTexture0", "uTexture1", "uTexture2", "uTexture3" };
  (void)texture_uniforms;
  for (uint32_t i = 0; i < 4; i++) {
    wz3_sampler_request request;
    wz3_sampler_setup_request(view, &setup, (int)i, &request);
    glr__texture_bind(r, host, setup.texture_op[i], i, &request);
    gl->glUniform1i(u[GLR__U_TEXTURE0 + i], (GLRint)i);
  }
  gl->glUniform1i(u[GLR__U_TEXTURE_MASK], setup.texture_mask);
  const int use_prelight = view->use_prelight && r->current_prelight;
  gl->glActiveTexture(GLR_TEXTURE4);
  const GLRuint prelight_or_fallback = use_prelight ? r->current_prelight : glr__fallback_texture(r);
  gl->glBindTexture(GLR_TEXTURE_2D, prelight_or_fallback);
  gl->glUniform1i(u[GLR__U_PRELIGHT_TEXTURE], 4);
  gl->glUniform1i(u[GLR__U_USE_PRELIGHT], use_prelight ? 1 : 0);

  float positions[WZ3_MAX_LIGHTS * 4], attenuation[WZ3_MAX_LIGHTS * 4], colors[WZ3_MAX_LIGHTS * 4], specular[WZ3_MAX_LIGHTS];
  if (mode == 2) {
    for (uint32_t index = 0; index < n_lights; index++) {
      const wz3_light_job *light = lights[index];
      const uint32_t offset = index * 4;
      if (light->directional) {
        if (legacy_matrix) {
          const double d[3] = { light->direction[0], light->direction[1], light->direction[2] };
          wz3_legacy_transr_vector(legacy_matrix, d, 0, positions + offset);
        } else {
          positions[offset] = light->direction[0];
          positions[offset + 1] = light->direction[1];
          positions[offset + 2] = light->direction[2];
        }
        positions[offset + 3] = 0.0f;
      } else {
        if (legacy_matrix) {
          const double p[3] = { light->position[0], light->position[1], light->position[2] };
          wz3_legacy_transr_vector(legacy_matrix, p, 1, positions + offset);
        } else {
          positions[offset] = light->position[0];
          positions[offset + 1] = light->position[1];
          positions[offset + 2] = light->position[2];
        }
        positions[offset + 3] = 1.0f;
      }
      if (material20 && light->directional) {
        if (legacy_matrix) {
          const double p[3] = { light->position[0], light->position[1], light->position[2] };
          wz3_legacy_transr_vector(legacy_matrix, p, 1, attenuation + offset);
        } else {
          attenuation[offset] = light->position[0];
          attenuation[offset + 1] = light->position[1];
          attenuation[offset + 2] = light->position[2];
        }
      } else {
        attenuation[offset] = positions[offset];
        attenuation[offset + 1] = positions[offset + 1];
        attenuation[offset + 2] = positions[offset + 2];
      }
      float color[3];
      glr__color_rgb(light->color, color);
      colors[offset] = glr__f((double)color[0] * (double)light->amplify);
      colors[offset + 1] = glr__f((double)color[1] * (double)light->amplify);
      colors[offset + 2] = glr__f((double)color[2] * (double)light->amplify);
      const double inverse_range = light->range > 1e12 ? 0.0 : 1.0 / (light->range > 1e-5 ? light->range : 1e-5);
      colors[offset + 3] = glr__f(inverse_range);
      attenuation[offset + 3] = glr__f(inverse_range);
      specular[index] = glr__f((double)((light->color >> 24) & 255u) / 255.0 * (double)light->amplify);
    }
  }
  float m11_light_constant[4] = { 0, 0, 0, 0 };
  if (material11 && legacy_lighting && n_lights) wz3_material11_light_constant(view->color, lights[0]->color, (double)lights[0]->amplify, m11_light_constant);
  gl->glUniform1i(u[GLR__U_LIGHT_COUNT], (GLRint)n_lights);
  if (mode == 2 && n_lights) {
    gl->glUniform4fv(u[GLR__U_LIGHT_POSITION], (GLRsizei)n_lights, positions);
    gl->glUniform4fv(u[GLR__U_LIGHT_ATTENUATION], (GLRsizei)n_lights, attenuation);
    gl->glUniform4fv(u[GLR__U_LIGHT_COLOR], (GLRsizei)n_lights, colors);
    gl->glUniform1fv(u[GLR__U_LIGHT_SPECULAR], (GLRsizei)n_lights, specular);
  }
  gl->glUniform4fv(u[GLR__U_M11_LIGHT_CONSTANT], 1, m11_light_constant);
}

/* ---- render targets, the fullscreen pass, layers, effects and post-processing ---- */

static glr_target *glr__target_at(glr *r, uint32_t index)
{
  while (r->n_targets <= index) {
    glr_target *t = (glr_target *)glr__alloc(r, sizeof *t);
    if (!t) return NULL;
    memset(t, 0, sizeof *t);
    glr_target **all = (glr_target **)glr__grow(r, r->targets, &r->cap_targets, r->n_targets + 1, sizeof *all);
    if (!all) {
      GLR_FREE(t);
      return NULL;
    }
    r->targets = all;
    if (glr__target_init(r, t, r->width, r->height, 0) != GLR_OK) {
      glr__target_free(r, t);
      GLR_FREE(t);
      return NULL;
    }
    r->targets[r->n_targets++] = t;
  }
  return r->targets[index];
}

static glr_target *glr__glare_target(glr *r, int index, int32_t width, int32_t height)
{
  for (int i = 0; i <= index; i++) { /* glareTarget: `while (length <= index) push(...)` */
    if (r->glare_made[i]) continue;
    if (glr__target_init(r, &r->glare[i], 1, 1, 0) != GLR_OK) return NULL;
    r->glare_made[i] = 1;
  }
  glr__target_resize(r, &r->glare[index], width, height);
  return &r->glare[index];
}

static void glr__bind_destination(glr *r, const glr_target *destination)
{
  const glr_gl *gl = &r->gl;
  gl->glBindFramebuffer(GLR_FRAMEBUFFER, destination ? destination->framebuffer : 0);
  gl->glViewport(0, 0, destination ? destination->width : r->width, destination ? destination->height : r->height);
  gl->glDisable(GLR_SCISSOR_TEST);
}

static wz3_region glr__set_viewport_region(glr *r, const wz3_ipp_node *viewport)
{
  wz3_region region;
  wz3_viewport_region(viewport->crop, r->width, r->height, &region);
  r->gl.glViewport(region.x, region.y, region.width, region.height);
  r->gl.glEnable(GLR_SCISSOR_TEST);
  r->gl.glScissor(region.x, region.y, region.width, region.height);
  return region;
}

static void glr__set_light_scissor(glr *r, const double *rect, const wz3_region *viewport)
{
  wz3_region region;
  wz3_light_scissor_region(rect, viewport, &region);
  r->gl.glEnable(GLR_SCISSOR_TEST);
  r->gl.glScissor(region.x, region.y, region.width, region.height);
}

static glr_target *glr__post_target(glr *r, int32_t depth, const glr_target *destination)
{
  uint32_t index = depth > 0 ? (uint32_t)depth : 0u;
  for (;;) {
    glr_target *candidate = glr__target_at(r, index++);
    if (!candidate || candidate != destination) return candidate;
  }
}

static void glr__capture_framebuffer(glr *r, const glr_target *target, const glr_target *source)
{
  const glr_gl *gl = &r->gl;
  gl->glDisable(GLR_SCISSOR_TEST);
  gl->glBindFramebuffer(GLR_READ_FRAMEBUFFER, source ? source->framebuffer : 0);
  gl->glBindFramebuffer(GLR_DRAW_FRAMEBUFFER, target->framebuffer);
  gl->glBlitFramebuffer(0, 0, r->width, r->height, 0, 0, r->width, r->height, GLR_COLOR_BUFFER_BIT, GLR_NEAREST);
}

/* Renderer.configureFullscreenState(material): every caller passes null, so the blend (a Layer2D material's flags) never applies */
static void glr__configure_fullscreen_blend(glr *r)
{
  const glr_gl *gl = &r->gl;
  gl->glDisable(GLR_DEPTH_TEST);
  gl->glDepthMask(0);
  gl->glDisable(GLR_CULL_FACE);
  gl->glDisable(GLR_POLYGON_OFFSET_FILL);
  gl->glDisable(GLR_STENCIL_TEST);
  gl->glDisable(GLR_BLEND);
  gl->glColorMask(1, 1, 1, 1);
}

typedef struct glr_fs_options {
  GLRuint second_image;
  const float *color0, *color1, *parameters, *color_correct, *texel, *uv_rect;
} glr_fs_options;

/* Renderer.drawFullscreen(image, mode, options) */
static void glr__draw_fullscreen_ex(glr *r, GLRuint image, int mode, const glr_fs_options *o)
{
  const glr_gl *gl = &r->gl;
  static const GLRfloat white[4] = { 1, 1, 1, 1 }, zero[4] = { 0, 0, 0, 0 }, unit_rect[4] = { 0, 0, 1, 1 };
  static const GLRfloat correction[28] = { 0 };
  const GLRint *u = r->fullscreen_uniforms;
  gl->glUseProgram(r->fullscreen_program);
  gl->glBindVertexArray(r->fullscreen_vao);
  gl->glActiveTexture(GLR_TEXTURE0);
  gl->glBindTexture(GLR_TEXTURE_2D, image);
  gl->glUniform1i(u[GLR__FS_IMAGE], 0);
  gl->glActiveTexture(GLR_TEXTURE0 + 1);
  if (o && o->second_image)
    gl->glBindTexture(GLR_TEXTURE_2D, o->second_image);
  else {
    const GLRuint fallback = glr__fallback_texture(r);
    gl->glBindTexture(GLR_TEXTURE_2D, fallback);
  }
  gl->glUniform1i(u[GLR__FS_MATERIAL_TEXTURE], 1);
  gl->glUniform1i(u[GLR__FS_MODE], mode);
  gl->glUniform4fv(u[GLR__FS_COLOR0], 1, o && o->color0 ? o->color0 : white);
  gl->glUniform4fv(u[GLR__FS_COLOR1], 1, o && o->color1 ? o->color1 : white);
  gl->glUniform4fv(u[GLR__FS_PARAMETERS], 1, o && o->parameters ? o->parameters : zero);
  gl->glUniform4fv(u[GLR__FS_COLOR_CORRECT], 7, o && o->color_correct ? o->color_correct : correction);
  if (o && o->texel)
    gl->glUniform2f(u[GLR__FS_TEXEL], o->texel[0], o->texel[1]);
  else
    gl->glUniform2f(u[GLR__FS_TEXEL], glr__f(1.0 / (double)r->width), glr__f(1.0 / (double)r->height));
  gl->glUniform4fv(u[GLR__FS_UV_RECT], 1, o && o->uv_rect ? o->uv_rect : unit_rect);
  gl->glDrawArrays(GLR_TRIANGLES, 0, 3);
  r->draw_calls++;
}

static void glr__draw_fullscreen(glr *r, GLRuint image) { glr__draw_fullscreen_ex(r, image, 0, NULL); }

/* Renderer.drawMaterial11Insert */
static void glr__draw_material11_insert(glr *r, int action)
{
  if (action != WZ3_INSERT_ACTION_CLEAR_DESTINATION_ALPHA && action != WZ3_INSERT_ACTION_ADD_DESTINATION_ALPHA) return;
  const glr_gl *gl = &r->gl;
  gl->glDisable(GLR_DEPTH_TEST);
  gl->glDepthMask(0);
  gl->glDisable(GLR_CULL_FACE);
  gl->glDisable(GLR_POLYGON_OFFSET_FILL);
  gl->glDisable(GLR_STENCIL_TEST);
  gl->glEnable(GLR_BLEND);
  gl->glBlendEquation(GLR_FUNC_ADD);
  if (action == WZ3_INSERT_ACTION_CLEAR_DESTINATION_ALPHA)
    gl->glBlendFunc(GLR_ZERO, GLR_SRC_COLOR);
  else
    gl->glBlendFunc(GLR_DST_ALPHA, GLR_ONE);
  gl->glColorMask(1, 1, 1, 1);
  const GLRuint fallback = glr__fallback_texture(r);
  glr__draw_fullscreen_ex(r, fallback, 7, NULL);
}

/* Renderer.applyGlare */
static void glr__apply_glare(glr *r, GLRuint image, const double *params, uint32_t n_params, const float uv_rect[4], const glr_target *destination,
                             const wz3_ipp_node *viewport)
{
  wz3_glare_plan plan;
  wz3_glare_plan_make(params, n_params, &plan);
  static const int32_t sizes[2][2] = { { 512, 256 }, { 256, 128 } };
  static const float unit[4] = { 0, 0, 1, 1 };
  GLRuint current_color = image;
  int32_t current_w = r->width, current_h = r->height;
  float current_uv[4] = { uv_rect[0], uv_rect[1], uv_rect[2], uv_rect[3] };
  for (int level = 0; level < 2; level++) {
    if (!plan.downsample[level]) continue;
    glr_target *target = glr__glare_target(r, level, sizes[level][0], sizes[level][1]);
    if (!target) return;
    glr__bind_destination(r, target);
    const float texel[2] = { glr__f(1.0 / (double)current_w), glr__f(1.0 / (double)current_h) };
    glr_fs_options o;
    memset(&o, 0, sizeof o);
    o.uv_rect = current_uv;
    o.texel = texel;
    glr__draw_fullscreen_ex(r, current_color, plan.copy_downsample ? 0 : 1, &o);
    current_color = target->color;
    current_w = sizes[level][0];
    current_h = sizes[level][1];
    memcpy(current_uv, unit, sizeof unit);
  }
  const int32_t selected_w = current_w, selected_h = current_h;
  glr_target *ping = glr__glare_target(r, 2, selected_w, selected_h);
  if (!ping) return;
  GLRuint read_color = current_color;
  const glr_target *write_target = ping;
  GLRuint write_color = ping->color;
  /* the first read is `selected` (no target of its own when it is the image); later reads alternate with the ping target */
  const glr_target *read_target = NULL;
  {
    /* find the target behind current_color for swapping */
    for (int level = 0; level < 2; level++)
      if (r->glare_made[level] && r->glare[level].color == current_color) read_target = &r->glare[level];
  }
  const float threshold_params[4] = { plan.gray_scale, glr__f(plan.gray_mix), 0.0f, 0.0f };
#define GLR_SWAP_PASS(mode, opts)                            \
  do {                                                       \
    glr__bind_destination(r, write_target);                  \
    glr__draw_fullscreen_ex(r, read_color, (mode), &(opts)); \
    const GLRuint previous_color = read_color;               \
    const glr_target *previous_target = read_target;         \
    read_color = write_color;                                \
    read_target = write_target;                              \
    write_color = previous_color;                            \
    write_target = previous_target;                          \
  } while (0)
  {
    glr_fs_options o;
    memset(&o, 0, sizeof o);
    o.color0 = plan.threshold;
    o.parameters = threshold_params;
    o.uv_rect = unit;
    GLR_SWAP_PASS(4, o);
  }
  const double aspect = (double)selected_w / (double)selected_h;
  for (int stage = 0; stage < 2; stage++) {
    if (!(plan.blur[stage] > 0.0)) continue;
    {
      const float p[4] = { glr__f(plan.blur[stage]), 0.0f, 1.0f, 0.0f };
      glr_fs_options o;
      memset(&o, 0, sizeof o);
      o.parameters = p;
      o.uv_rect = unit;
      GLR_SWAP_PASS(5, o);
    }
    {
      const float p[4] = { 0.0f, glr__f(plan.blur[stage] * aspect), glr__f(plan.amplify[stage]), 0.0f };
      glr_fs_options o;
      memset(&o, 0, sizeof o);
      o.parameters = p;
      o.uv_rect = unit;
      GLR_SWAP_PASS(5, o);
    }
  }
#undef GLR_SWAP_PASS
  glr__bind_destination(r, destination);
  if (viewport) glr__set_viewport_region(r, viewport);
  const float composite[4] = { glr__f(plan.add_smooth), 0.0f, 0.0f, 0.0f };
  glr_fs_options o;
  memset(&o, 0, sizeof o);
  o.second_image = image;
  o.color0 = plan.glare_color;
  o.color1 = plan.original_color;
  o.parameters = composite;
  o.uv_rect = uv_rect;
  glr__draw_fullscreen_ex(r, read_color, 6, &o);
}

/* Renderer.applyPost */
static void glr__apply_post(glr *r, GLRuint image, const wz3_effect_output *out, const float uv_rect[4], const glr_target *destination,
                            const wz3_ipp_node *viewport)
{
  glr__configure_fullscreen_blend(r);
  if (out->kind == WZ3_EFFECT_OUT_GLARE)
    glr__apply_glare(r, image, out->params, out->n_params, uv_rect, destination, viewport);
  else if (out->kind == WZ3_EFFECT_OUT_COLOR_CORRECTION) {
    float constants[28];
    wz3_color_correction_constants(out->params, out->n_params, constants);
    glr_fs_options o;
    memset(&o, 0, sizeof o);
    o.color_correct = constants;
    o.uv_rect = uv_rect;
    glr__draw_fullscreen_ex(r, image, 2, &o);
  } else {
    glr_fs_options o;
    memset(&o, 0, sizeof o);
    o.uv_rect = uv_rect;
    glr__draw_fullscreen_ex(r, image, 0, &o);
  }
}

/* ---- Layer2D quads ---- */

static void glr__ensure_layer_geometry(glr *r)
{
  if (r->layer_vao) return;
  const glr_gl *gl = &r->gl;
  gl->glGenVertexArrays(1, &r->layer_vao);
  gl->glGenBuffers(1, &r->layer_position_buffer);
  gl->glGenBuffers(1, &r->layer_uv_buffer);
  gl->glGenBuffers(1, &r->layer_index_buffer);
  gl->glBindVertexArray(r->layer_vao);
  gl->glBindBuffer(GLR_ARRAY_BUFFER, r->layer_position_buffer);
  gl->glBufferData(GLR_ARRAY_BUFFER, 48, NULL, GLR_DYNAMIC_DRAW);
  gl->glEnableVertexAttribArray(0);
  gl->glVertexAttribPointer(0, 3, GLR_FLOAT, 0, 0, NULL);
  gl->glBindBuffer(GLR_ARRAY_BUFFER, r->layer_uv_buffer);
  gl->glBufferData(GLR_ARRAY_BUFFER, 32, NULL, GLR_DYNAMIC_DRAW);
  gl->glEnableVertexAttribArray(2);
  gl->glVertexAttribPointer(2, 2, GLR_FLOAT, 0, 0, NULL);
  gl->glEnableVertexAttribArray(9);
  gl->glVertexAttribPointer(9, 2, GLR_FLOAT, 0, 0, NULL);
  gl->glBindBuffer(GLR_ELEMENT_ARRAY_BUFFER, r->layer_index_buffer);
  static const uint16_t quad[6] = { 0, 1, 2, 0, 2, 3 };
  gl->glBufferData(GLR_ELEMENT_ARRAY_BUFFER, 12, quad, GLR_STATIC_DRAW);
  static const int disabled[7] = { 1, 3, 4, 5, 6, 7, 8 };
  for (int i = 0; i < 7; i++) {
    gl->glDisableVertexAttribArray((GLRuint)disabled[i]);
    gl->glVertexAttribDivisor((GLRuint)disabled[i], 0);
  }
  gl->glBindVertexArray(0);
}

static void glr__draw_layer_quad(glr *r, const glr_host *host, const wz3_ipp_node *layer, const wz3_material *material, const wz3_material_pass *pass)
{
  const glr_gl *gl = &r->gl;
  glr__ensure_layer_geometry(r);
  float positions[12], uvs[8];
  wz3_layer_quad_geometry(layer, positions, uvs);
  wz3_material_view view;
  wz3_material_view_make(material, NULL, pass, &view);
  glr__configure_material_state(r, &view);
  gl->glUseProgram(r->program);
  gl->glBindVertexArray(r->layer_vao);
  gl->glBindBuffer(GLR_ARRAY_BUFFER, r->layer_position_buffer);
  gl->glBufferSubData(GLR_ARRAY_BUFFER, 0, 48, positions);
  gl->glBindBuffer(GLR_ARRAY_BUFFER, r->layer_uv_buffer);
  gl->glBufferSubData(GLR_ARRAY_BUFFER, 0, 32, uvs);
  gl->glVertexAttrib3f(1, 0.0f, 0.0f, -1.0f);
  gl->glVertexAttrib4f(3, 0.0f, 0.0f, 0.0f, 0.0f);
  gl->glVertexAttrib4f(4, 0.0f, 1.0f, 0.0f, 1.0f);
  gl->glVertexAttrib4f(5, 1.0f, 0.0f, 0.0f, 0.0f);
  gl->glVertexAttrib4f(6, 0.0f, 1.0f, 0.0f, 0.0f);
  gl->glVertexAttrib4f(7, 0.0f, 0.0f, 1.0f, 0.0f);
  gl->glVertexAttrib4f(8, 0.0f, 0.0f, 0.0f, 1.0f);
  gl->glUniformMatrix4fv(r->material_uniforms[GLR__U_VIEW_PROJECTION], 1, 0, glr__identity_matrix);
  gl->glUniform4f(r->material_uniforms[GLR__U_UV_TRANSFORM], 1.0f, 1.0f, 0.0f, 0.0f);
  wz3_ipp_node viewport;
  memset(&viewport, 0, sizeof viewport);
  viewport.ambient = 0;
  memcpy(viewport.camera.camera_space, glr__identity_matrix, sizeof glr__identity_matrix);
  viewport.camera.fog_color = 0xff808080u;
  viewport.camera.fog_start = 0.0;
  viewport.camera.fog_end = 4096.0;
  wz3_camera_mats matrices;
  memcpy(matrices.view, glr__identity_matrix, sizeof glr__identity_matrix);
  memcpy(matrices.projection, glr__identity_matrix, sizeof glr__identity_matrix);
  memcpy(matrices.view_projection, glr__identity_matrix, sizeof glr__identity_matrix);
  glr__bind_material(r, host, &view, &viewport, &matrices, NULL, 0, NULL, 0, 0);
  gl->glDrawElements(GLR_TRIANGLES, 6, GLR_UNSIGNED_SHORT, NULL);
  r->draw_calls++;
  r->triangles += 2;
}

/* ---- meshes, shadow volumes and effect geometry ---- */

typedef struct glr_frame_ctx {
  const glr_host *host;
  const wz3_frame *frame;
  const wz3_ipp_node *viewport;
  const wz3_viewport_plan *plan;
  wz3_region viewport_pixels;
} glr_frame_ctx;

static void glr__view_of(const glr_host *host, int32_t material_op, int32_t pass_index, wz3_material_view *view)
{
  const wz3_material *m = material_op >= 0 && host->rt ? wz3_rt_material(host->rt, (uint32_t)material_op) : NULL;
  const wz3_material_pass *pass = (m && pass_index >= 0 && (uint32_t)pass_index < m->n_passes) ? &m->passes[pass_index] : NULL;
  const wz3_material *owner = pass ? wz3_rt_material(host->rt, pass->owner) : NULL;
  wz3_material_view_make(m, owner, pass, view);
}

/* composeInstanceMatrices, cached per job for the frame; returns the matrices (floats) or NULL when there are none */
static const float *glr__job_matrices(glr *r, const wz3_frame *frame, const wz3_mesh_job *job, uint32_t *floats)
{
  const uint32_t index = (uint32_t)(job - frame->mesh_jobs);
  if (index < r->n_job_cache && r->job_cache[index].ready) {
    *floats = r->job_cache[index].count * 16u;
    return r->job_cache[index].data;
  }
  const uint32_t n = job->has_instances ? job->n_instances : 1u;
  float *out = (float *)glr__alloc(r, (size_t)(n ? n : 1u) * 64u);
  if (!out) {
    *floats = 0;
    return NULL;
  }
  const uint32_t count = wz3_compose_instances(job->matrix, job->has_instances,
                                               job->has_instances ? frame->instances + (size_t)job->first_instance * 16u : NULL, job->n_instances, out);
  if (index < r->n_job_cache) {
    r->job_cache[index].ready = 1;
    r->job_cache[index].data = out;
    r->job_cache[index].count = count;
  } else {
    r->job_scratch = out;
  }
  *floats = count * 16u;
  return out;
}

/* Renderer.drawMeshItem for a pass of a geometry; `job_key` identifies the job for the upload cache */
static void glr__draw_mesh(glr *r, const glr_host *host, const wz3_ipp_node *viewport, const wz3_camera_mats *matrices, glr_geom *geometry,
                           const wz3_geometry_group *group, int32_t material_op, int32_t pass_index, const void *job_key, const float *instances,
                           uint32_t floats, const float *job_matrix, int animated, const wz3_light_job *const *lights, uint32_t n_lights)
{
  if (!group->count || !floats) return;
  const glr_gl *gl = &r->gl;
  wz3_material_view view;
  glr__view_of(host, material_op, pass_index, &view);
  glr__configure_material_state(r, &view);
  gl->glUseProgram(r->program);
  gl->glBindVertexArray(geometry->vao);
  glr__bind_instances(r, job_key, instances, floats);
  gl->glUniformMatrix4fv(r->material_uniforms[GLR__U_VIEW_PROJECTION], 1, 0, matrices->view_projection);
  const double uv_scale = (view.uv_scale == view.uv_scale && view.uv_scale != 0.0 && view.uv_scale - view.uv_scale == 0.0) ? view.uv_scale : 1.0;
  gl->glUniform4f(r->material_uniforms[GLR__U_UV_TRANSFORM], glr__f(uv_scale), glr__f(uv_scale), 0.0f, 0.0f);
  const wz3_material *m = material_op >= 0 ? wz3_rt_material(host->rt, (uint32_t)material_op) : NULL;
  const wz3_material_pass *pass = (m && pass_index >= 0 && (uint32_t)pass_index < m->n_passes) ? &m->passes[pass_index] : NULL;
  glr__bind_material(r, host, &view, viewport, matrices, lights, n_lights, job_matrix, animated, pass && pass->program == WZ3_PROGRAM_INSTANCES);
  const uint32_t instance_count = floats / 16u;
  gl->glDrawElementsInstanced(GLR_TRIANGLES, (GLRsizei)group->count, geometry->index_type,
                              (const void *)(uintptr_t)((size_t)group->start * geometry->index_bytes), (GLRsizei)instance_count);
  r->draw_calls++;
  r->triangles += group->count / 3u * instance_count;
}

static glr_topology *glr__topology_get(glr *r, glr_geom *e, const wz3_shadow_data *data, const uint32_t *ranges, uint32_t n_key)
{
  for (uint32_t i = 0; i < e->n_topos; i++)
    if (e->topos[i].n_key == n_key && !memcmp(e->topos[i].key, ranges, (size_t)n_key * 4u)) return &e->topos[i];
  if (!e->g) return NULL;
  glr_topology *topos = (glr_topology *)glr__grow(r, e->topos, &e->cap_topos, e->n_topos + 1, sizeof *topos);
  if (!topos) return NULL;
  e->topos = topos;
  glr_topology *t = &e->topos[e->n_topos];
  memset(t, 0, sizeof *t);
  t->key = (uint32_t *)glr__alloc(r, (size_t)(n_key ? n_key : 1u) * 4u);
  if (!t->key) return NULL;
  memcpy(t->key, ranges, (size_t)n_key * 4u);
  t->n_key = n_key;
  const wz3_result prepared = wz3_shadow_topology_prepare(e->g, data, ranges, n_key / 2u, e->dynamic, &t->t);
  glr__note(r, prepared);
  if (prepared != WZ3_OK) {
    GLR_FREE(t->key);
    return NULL;
  }
  e->n_topos++;
  return t;
}

/* Renderer.drawShadowItem */
static void glr__draw_shadow(glr *r, const glr_host *host, const wz3_camera_mats *matrices, glr_geom *geometry, const wz3_shadow_data *data,
                             const uint32_t *group_ids, uint32_t n_group_ids, const wz3_light_job *light, int zfail, const float *instances, uint32_t floats)
{
  if (!light || !n_group_ids) return;
  const glr_gl *gl = &r->gl;
  const GLRint *u = r->shadow_uniforms;
  uint32_t ranges[2 * 64];
  uint32_t *dynamic_ranges = NULL;
  uint32_t *use = ranges;
  if (n_group_ids > 64) {
    dynamic_ranges = (uint32_t *)glr__alloc(r, (size_t)n_group_ids * 8u);
    if (!dynamic_ranges) return;
    use = dynamic_ranges;
  }
  for (uint32_t i = 0; i < n_group_ids; i++) {
    use[2 * i] = geometry->groups[group_ids[i]].start;
    use[2 * i + 1] = geometry->groups[group_ids[i]].count;
  }
  glr_topology *topology = glr__topology_get(r, geometry, data, use, n_group_ids * 2u);
  GLR_FREE(dynamic_ranges);
  if (!topology) return;
  (void)host;
  gl->glUseProgram(r->shadow_program);
  gl->glBindVertexArray(r->shadow_vao);
  gl->glUniformMatrix4fv(u[0], 1, 0, matrices->view_projection);
  gl->glEnable(GLR_DEPTH_TEST);
  gl->glDepthFunc(GLR_LESS);
  gl->glDepthMask(0);
  gl->glColorMask(0, 0, 0, 0);
  gl->glDisable(GLR_BLEND);
  gl->glDisable(GLR_CULL_FACE);
  gl->glDisable(GLR_POLYGON_OFFSET_FILL);
  gl->glFrontFace(GLR_CW);
  gl->glEnable(GLR_STENCIL_TEST);
  gl->glStencilMask(0xff);
  gl->glStencilFunc(GLR_ALWAYS, 0, 0xff);
  if (zfail) {
    gl->glStencilOpSeparate(GLR_FRONT, GLR_KEEP, GLR_DECR_WRAP, GLR_KEEP);
    gl->glStencilOpSeparate(GLR_BACK, GLR_KEEP, GLR_INCR_WRAP, GLR_KEEP);
  } else {
    gl->glStencilOpSeparate(GLR_FRONT, GLR_KEEP, GLR_KEEP, GLR_INCR_WRAP);
    gl->glStencilOpSeparate(GLR_BACK, GLR_KEEP, GLR_KEEP, GLR_DECR_WRAP);
  }
  int uploaded = 0;
  for (uint32_t offset = 0; offset < floats; offset += 16) {
    wz3_shadow_volume volume;
    const wz3_result built = wz3_shadow_volume_build(&topology->t, light->position, instances + offset, zfail, &volume);
    glr__note(r, built);
    if (built != WZ3_OK) continue;
    if (!volume.n_indices) {
      wz3_shadow_volume_free(&volume);
      continue;
    }
    gl->glUniform3fv(u[2], 1, volume.light_position);
    if (!uploaded) {
      gl->glBindBuffer(GLR_ARRAY_BUFFER, r->shadow_position_buffer);
      gl->glBufferData(GLR_ARRAY_BUFFER, (GLRsizeiptr)topology->t.n_vertices * 24, topology->t.volume_positions, GLR_DYNAMIC_DRAW);
      gl->glEnableVertexAttribArray(0);
      gl->glVertexAttribPointer(0, 3, GLR_FLOAT, 0, 0, NULL);
      gl->glBindBuffer(GLR_ARRAY_BUFFER, r->shadow_extrusion_buffer);
      gl->glBufferData(GLR_ARRAY_BUFFER, (GLRsizeiptr)topology->t.n_vertices * 2, topology->t.extrusions, GLR_DYNAMIC_DRAW);
      gl->glEnableVertexAttribArray(1);
      gl->glVertexAttribPointer(1, 1, GLR_UNSIGNED_BYTE, 0, 0, NULL);
      uploaded = 1;
    }
    gl->glBindBuffer(GLR_ELEMENT_ARRAY_BUFFER, r->shadow_index_buffer);
    uint16_t *narrow = NULL;
    if (volume.wide)
      gl->glBufferData(GLR_ELEMENT_ARRAY_BUFFER, (GLRsizeiptr)volume.n_indices * 4, volume.indices, GLR_DYNAMIC_DRAW);
    else {
      narrow = (uint16_t *)glr__alloc(r, (size_t)volume.n_indices * 2u);
      if (!narrow) {
        wz3_shadow_volume_free(&volume);
        return;
      }
      for (uint32_t k = 0; k < volume.n_indices; k++) narrow[k] = (uint16_t)volume.indices[k];
      gl->glBufferData(GLR_ELEMENT_ARRAY_BUFFER, (GLRsizeiptr)volume.n_indices * 2, narrow, GLR_DYNAMIC_DRAW);
    }
    gl->glUniformMatrix4fv(u[1], 1, 0, instances + offset);
    gl->glDrawElements(GLR_TRIANGLES, (GLRsizei)volume.n_indices, volume.wide ? GLR_UNSIGNED_INT : GLR_UNSIGNED_SHORT, NULL);
    r->draw_calls++;
    r->triangles += volume.n_indices / 3u;
    GLR_FREE(narrow);
    wz3_shadow_volume_free(&volume);
  }
}

/* GeometryCache.get(effect.geometry) for the water grid of an effect operator */
static glr_geom *glr__water_geometry(glr *r, uint32_t op, const wz3_water_geometry *w)
{
  glr_geom *e = r->geom_by_op[op];
  if (e && e->version == w->version && e->n_vertices == w->n_vertices) return e;
  if (!r->water_by_op[op]) {
    glr_water_statics *ws = (glr_water_statics *)glr__alloc(r, sizeof *ws);
    if (!ws) return NULL;
    memset(ws, 0, sizeof *ws);
    ws->colors = (uint8_t *)glr__alloc(r, (size_t)w->n_vertices * 4u + 4u);
    ws->tangents = (float *)glr__alloc(r, (size_t)w->n_vertices * 16u + 16u);
    if (!ws->colors || !ws->tangents) {
      GLR_FREE(ws->colors);
      GLR_FREE(ws->tangents);
      GLR_FREE(ws);
      return NULL;
    }
    for (uint32_t i = 0; i < w->n_vertices; i++) {
      ws->colors[i * 4] = 0xff;
      ws->colors[i * 4 + 1] = 0xaa;
      ws->colors[i * 4 + 2] = 0x88;
      ws->colors[i * 4 + 3] = 0xff;
    }
    memset(ws->tangents, 0, (size_t)w->n_vertices * 16u);
    ws->group.count = w->n_indices;
    ws->group.material = WZ3_GROUP_MATERIAL_NONE;
    r->water_by_op[op] = ws;
  }
  glr_water_statics *ws = r->water_by_op[op];
  glr_streams s;
  memset(&s, 0, sizeof s);
  const size_t n = w->n_vertices;
  s.n_vertices = w->n_vertices;
  s.n_indices = w->n_indices;
  s.data[GLR_S_POSITIONS] = w->positions;
  s.bytes[GLR_S_POSITIONS] = n * 12;
  s.type[GLR_S_POSITIONS] = GLR_T_F32;
  s.data[GLR_S_NORMALS] = w->normals;
  s.bytes[GLR_S_NORMALS] = n * 12;
  s.type[GLR_S_NORMALS] = GLR_T_F32;
  s.data[GLR_S_UVS] = w->uvs;
  s.bytes[GLR_S_UVS] = n * 8;
  s.type[GLR_S_UVS] = GLR_T_F32;
  s.data[GLR_S_COLORS] = ws->colors;
  s.bytes[GLR_S_COLORS] = n * 4;
  s.type[GLR_S_COLORS] = GLR_T_U8;
  s.data[GLR_S_TANGENTS] = ws->tangents;
  s.bytes[GLR_S_TANGENTS] = n * 16;
  s.type[GLR_S_TANGENTS] = GLR_T_F32;
  s.data[GLR_S_UV1] = w->uvs;
  s.bytes[GLR_S_UV1] = n * 8;
  s.type[GLR_S_UV1] = GLR_T_F32;
  s.data[GLR_S_INDICES] = w->indices;
  s.bytes[GLR_S_INDICES] = (size_t)w->n_indices * 2u;
  s.type[GLR_S_INDICES] = GLR_T_U16;
  s.uv1_is_uvs = 1;
  s.groups = &ws->group;
  s.n_groups = 1;
  s.partial = 1;
  s.dynamic_mask = 1u << GLR_S_POSITIONS | 1u << GLR_S_NORMALS | (w->uv_changed ? 1u << GLR_S_UVS : 0u);
  s.version = w->version;
  if (e) {
    if (!glr__geom_update(r, e, &s, 0)) return NULL;
    return e;
  }
  e = glr__geom_create(r, &s, 0);
  if (!e) return NULL;
  glr__geom_remember(e, &s);
  if (!glr__geom_copy_groups(r, e, &s) || !glr__geom_register(r, e)) {
    glr__geom_destroy(r, e);
    return NULL;
  }
  r->geom_by_op[op] = e;
  return e;
}

/* Renderer.drawEffectGeometry: the water grid or the ribbon of a chain line */
static void glr__draw_effect_geometry(glr *r, const glr_host *host, const wz3_ipp_node *viewport, const wz3_camera_mats *matrices, const wz3_effect_output *out,
                                      const wz3_light_job *const *lights, uint32_t n_lights)
{
  const glr_gl *gl = &r->gl;
  const wz3_material *material = out->material_op >= 0 ? wz3_rt_material(host->rt, (uint32_t)out->material_op) : NULL;
  if (out->kind == WZ3_EFFECT_OUT_WATER && out->water) {
    glr_geom *geometry = glr__water_geometry(r, out->op, out->water);
    if (!geometry) return;
    const int32_t pass_index = (material && material->n_passes) ? 0 : -1;
    uint32_t floats = 0;
    float matrix[16];
    memcpy(matrix, out->matrix, sizeof matrix);
    float composed[16];
    wz3_compose_instances(matrix, 0, NULL, 0, composed);
    (void)floats;
    glr__draw_mesh(r, host, viewport, matrices, geometry, &geometry->groups[0], out->material_op, pass_index, NULL, composed, 16, matrix, 0, lights, n_lights);
    return;
  }
  if (out->kind != WZ3_EFFECT_OUT_CHAIN_LINE || !out->points) return;
  const uint32_t count = 32;
  if (!r->chain_vao) {
    gl->glGenVertexArrays(1, &r->chain_vao);
    gl->glGenBuffers(1, &r->chain_position_buffer);
    gl->glGenBuffers(1, &r->chain_uv_buffer);
    gl->glGenBuffers(1, &r->chain_index_buffer);
  }
  float positions[32 * 2 * 3], uvs[32 * 2 * 2];
  uint16_t indices[31 * 6];
  const uint32_t index_count = wz3_build_chain_ribbon(out->points, count, out->thickness, out->ripped, matrices->view, positions, uvs, indices);
  gl->glBindVertexArray(r->chain_vao);
  gl->glBindBuffer(GLR_ARRAY_BUFFER, r->chain_position_buffer);
  gl->glBufferData(GLR_ARRAY_BUFFER, (GLRsizeiptr)sizeof positions, positions, GLR_DYNAMIC_DRAW);
  gl->glEnableVertexAttribArray(0);
  gl->glVertexAttribPointer(0, 3, GLR_FLOAT, 0, 0, NULL);
  gl->glDisableVertexAttribArray(1);
  gl->glVertexAttrib3f(1, 0.0f, 1.0f, 0.0f);
  gl->glBindBuffer(GLR_ARRAY_BUFFER, r->chain_uv_buffer);
  gl->glBufferData(GLR_ARRAY_BUFFER, (GLRsizeiptr)sizeof uvs, uvs, GLR_DYNAMIC_DRAW);
  gl->glEnableVertexAttribArray(2);
  gl->glVertexAttribPointer(2, 2, GLR_FLOAT, 0, 0, NULL);
  gl->glEnableVertexAttribArray(9);
  gl->glVertexAttribPointer(9, 2, GLR_FLOAT, 0, 0, NULL);
  gl->glDisableVertexAttribArray(3);
  gl->glVertexAttrib4f(3, glr__f(64.0 / 255.0), glr__f(64.0 / 255.0), glr__f(64.0 / 255.0), 1.0f);
  gl->glDisableVertexAttribArray(4);
  gl->glVertexAttrib4f(4, 1.0f, 0.0f, 0.0f, 1.0f);
  gl->glBindBuffer(GLR_ELEMENT_ARRAY_BUFFER, r->chain_index_buffer);
  gl->glBufferData(GLR_ELEMENT_ARRAY_BUFFER, (GLRsizeiptr)index_count * 2, indices, GLR_DYNAMIC_DRAW);
  for (int column = 0; column < 4; column++) {
    gl->glDisableVertexAttribArray((GLRuint)(5 + column));
    gl->glVertexAttrib4fv((GLRuint)(5 + column), glr__identity_matrix + column * 4);
    gl->glVertexAttribDivisor((GLRuint)(5 + column), 0);
  }
  const int32_t pass_index = (material && material->n_passes) ? 0 : -1;
  wz3_material_view view;
  glr__view_of(host, out->material_op, pass_index, &view);
  glr__configure_material_state(r, &view);
  gl->glUseProgram(r->program);
  gl->glUniformMatrix4fv(r->material_uniforms[GLR__U_VIEW_PROJECTION], 1, 0, matrices->view_projection);
  gl->glUniform4f(r->material_uniforms[GLR__U_UV_TRANSFORM], 1.0f, 1.0f, 0.0f, 0.0f);
  glr__bind_material(r, host, &view, viewport, matrices, lights, n_lights, glr__identity_matrix, 0, 0);
  gl->glDrawElements(GLR_TRIANGLES, (GLRsizei)index_count, GLR_UNSIGNED_SHORT, NULL);
  r->draw_calls++;
  r->triangles += index_count / 3u;
}

typedef struct glr_source_ctx {
  glr *r;
  const glr_host *host;
  const wz3_frame *frame;
} glr_source_ctx;
static const wz3_material *glr__source_material(void *user, int32_t op)
{
  const glr_source_ctx *c = (const glr_source_ctx *)user;
  return op >= 0 ? wz3_rt_material(c->host->rt, (uint32_t)op) : NULL;
}
static const wz3_effect *glr__source_effect(void *user, uint32_t op) { return wz3_rt_effect(((const glr_source_ctx *)user)->host->rt, op); }
static const wz3_geometry *glr__source_geometry(void *user, const wz3_mesh_job *job)
{
  const glr_source_ctx *c = (const glr_source_ctx *)user;
  glr_geom *e = glr__geometry_for_job(c->r, c->host, job);
  return e ? e->g : NULL;
}

static void glr__free_caches(glr *r)
{
  const glr_gl *gl = &r->gl;
  for (uint32_t i = 0; i < r->n_all_geoms; i++) glr__geom_destroy(r, r->all_geoms[i]);
  GLR_FREE(r->all_geoms);
  for (uint32_t i = 0; i < r->n_op_tables; i++) {
    glr_pool *pool = r->pool_by_op ? r->pool_by_op[i] : NULL;
    if (pool) {
      GLR_FREE(pool->times);
      GLR_FREE(pool->by_time);
      GLR_FREE(pool->entries);
      GLR_FREE(pool);
    }
    glr_tex *tex = r->tex_by_op ? r->tex_by_op[i] : NULL;
    if (tex) {
      if (tex->texture) gl->glDeleteTextures(1, &tex->texture);
      GLR_FREE(tex);
    }
    glr_water_statics *ws = r->water_by_op ? r->water_by_op[i] : NULL;
    if (ws) {
      GLR_FREE(ws->colors);
      GLR_FREE(ws->tangents);
      GLR_FREE(ws);
    }
  }
  GLR_FREE(r->tex_by_op);
  GLR_FREE(r->geom_by_op);
  GLR_FREE(r->pool_by_op);
  GLR_FREE(r->water_by_op);
  for (uint32_t i = 0; i < r->n_targets; i++) {
    glr__target_free(r, r->targets[i]);
    GLR_FREE(r->targets[i]);
  }
  GLR_FREE(r->targets);
  for (int i = 0; i < 3; i++)
    if (r->glare_made[i]) glr__target_free(r, &r->glare[i]);
  if (r->layer_vao) gl->glDeleteVertexArrays(1, &r->layer_vao);
  if (r->layer_position_buffer) gl->glDeleteBuffers(1, &r->layer_position_buffer);
  if (r->layer_uv_buffer) gl->glDeleteBuffers(1, &r->layer_uv_buffer);
  if (r->layer_index_buffer) gl->glDeleteBuffers(1, &r->layer_index_buffer);
  if (r->chain_vao) gl->glDeleteVertexArrays(1, &r->chain_vao);
  if (r->chain_position_buffer) gl->glDeleteBuffers(1, &r->chain_position_buffer);
  if (r->chain_uv_buffer) gl->glDeleteBuffers(1, &r->chain_uv_buffer);
  if (r->chain_index_buffer) gl->glDeleteBuffers(1, &r->chain_index_buffer);
  GLR_FREE(r->inst_records);
  GLR_FREE(r->job_scratch);
  wz3_draw_list_free(&r->list);
  wz3_pass_sort_ids_free(&r->sort_ids);
}

/* ---- the viewport pass loop and the frame (Renderer.drawViewportScene, renderNode, render) ---- */

static void glr__draw_effect_item(glr *r, const glr_host *host, const wz3_frame *frame, const wz3_ipp_node *viewport, const wz3_camera_mats *matrices,
                                  const wz3_draw_item *item, const glr_target *destination, int32_t depth, const wz3_light_job *const *lights,
                                  uint32_t n_lights, glr_target *current_render_target)
{
  const wz3_effect_job *job = &frame->effect_jobs[viewport->first_effect + item->job];
  const wz3_effect_output *out = NULL;
  const wz3_result ran = wz3_rt_effect_run(host->rt, job, &out);
  glr__note(r, ran);
  if (ran != WZ3_OK || !out) return;
  const wz3_effect *effect = wz3_rt_effect(host->rt, job->effect_op);
  if (out->kind == WZ3_EFFECT_OUT_CHAIN_LINE || out->kind == WZ3_EFFECT_OUT_WATER) {
    glr__draw_effect_geometry(r, host, viewport, matrices, out, lights, n_lights);
  } else if (out->kind == WZ3_EFFECT_OUT_GLARE || out->kind == WZ3_EFFECT_OUT_COLOR_CORRECTION) {
    const int use_snapshot = effect && effect->need_current_render && current_render_target;
    glr_target *scratch = use_snapshot ? current_render_target : glr__post_target(r, depth, destination);
    if (!scratch) return;
    if (!use_snapshot) {
      glr__set_viewport_region(r, viewport);
      glr__capture_framebuffer(r, scratch, destination);
    }
    glr__bind_destination(r, destination);
    const wz3_region region = glr__set_viewport_region(r, viewport);
    glr__apply_post(r, scratch->color, out, region.uv, destination, viewport);
    glr__bind_destination(r, destination);
    glr__set_viewport_region(r, viewport);
  }
}

static void glr__prepare_instance_uploads(glr *r, const glr_host *host, const wz3_frame *frame, const wz3_ipp_node *viewport, const wz3_draw_list *list)
{
  uint64_t required = 0;
  uint32_t effect_slots = 0;
  uint32_t seen[256], n_seen = 0;
  uint32_t *seen_big = NULL, cap_big = 0;
  struct {
    uint32_t job;
    const float *data;
    uint32_t floats;
  } *jobs = NULL;
  uint32_t n_jobs = 0, cap_jobs = 0;
  (void)seen_big;
  (void)cap_big;
  for (uint32_t i = 0; i < list->n_items; i++) {
    const wz3_draw_item *item = &list->items[i];
    if (item->kind == WZ3_DRAW_EFFECT) {
      effect_slots++;
      continue;
    }
    if (item->kind == WZ3_DRAW_SHADOW || item->key.usage == WZ3_USAGE_SHADOW) continue;
    const wz3_mesh_job *job = &frame->mesh_jobs[viewport->first_mesh + item->job];
    glr_geom *geometry = glr__geometry_for_job(r, host, job);
    if (!geometry || item->group < 0 || !geometry->groups[item->group].count) continue;
    int duplicate = 0;
    for (uint32_t k = 0; k < n_seen; k++)
      if (seen[k] == item->job) {
        duplicate = 1;
        break;
      }
    if (duplicate) continue;
    if (n_seen < 256) seen[n_seen++] = item->job;
    uint32_t floats;
    const float *data = glr__job_matrices(r, frame, job, &floats);
    if (!floats) continue;
    void *grown = glr__grow(r, jobs, &cap_jobs, n_jobs + 1, sizeof *jobs);
    if (!grown) {
      GLR_FREE(jobs);
      return;
    }
    jobs = grown;
    jobs[n_jobs].job = item->job;
    jobs[n_jobs].data = data;
    jobs[n_jobs].floats = floats;
    n_jobs++;
    required += (uint64_t)floats * 4u;
  }
  required += (uint64_t)effect_slots * GLR_INSTANCE_MATRIX_BYTES;
  r->n_inst_records = 0;
  r->instance_used = 0;
  r->instance_active = required > 0;
  if (r->instance_active) {
    const glr_gl *gl = &r->gl;
    r->instance_capacity = glr__instance_buffer_size(r, required);
    gl->glBindBuffer(GLR_ARRAY_BUFFER, r->instance_buffer);
    gl->glBufferData(GLR_ARRAY_BUFFER, (GLRsizeiptr)r->instance_capacity, NULL, GLR_DYNAMIC_DRAW);
    for (uint32_t k = 0; k < n_jobs; k++) glr__upload_instances(r, &frame->mesh_jobs[viewport->first_mesh + jobs[k].job], jobs[k].data, jobs[k].floats * 4u);
  }
  GLR_FREE(jobs);
}

static void glr__draw_viewport_scene(glr *r, const glr_host *host, const wz3_frame *frame, const wz3_ipp_node *vp, const glr_target *destination, int32_t depth)
{
  const glr_gl *gl = &r->gl;
  r->current_prelight = 0;
  float clear[4];
  glr__color_argb(vp->clear_color, clear);
  const wz3_region viewport_pixels = glr__set_viewport_region(r, vp);
  gl->glEnable(GLR_DEPTH_TEST);
  gl->glDepthMask(1);
  gl->glColorMask(1, 1, 1, 1);
  gl->glDisable(GLR_BLEND);
  gl->glEnable(GLR_CULL_FACE);
  gl->glCullFace(GLR_BACK);
  gl->glClearColor(clear[0], clear[1], clear[2], clear[3]);
  gl->glClearDepthf(1.0f);
  gl->glClearStencil(0);
  gl->glStencilMask(0xff);
  const uint32_t clear_flags = wz3_viewport_clear_flags(vp->flags);
  GLRuint clear_mask = 0;
  if (clear_flags & 1u) clear_mask |= GLR_COLOR_BUFFER_BIT;
  if (clear_flags & 2u) clear_mask |= GLR_DEPTH_BUFFER_BIT | GLR_STENCIL_BUFFER_BIT;
  if (clear_mask) gl->glClear(clear_mask);

  wz3_draw_source source;
  memset(&source, 0, sizeof source);
  glr_source_ctx sc;
  sc.r = r;
  sc.host = host;
  sc.frame = frame;
  source.user = &sc;
  source.material = glr__source_material;
  source.effect = glr__source_effect;
  source.geometry = glr__source_geometry;
  wz3_viewport_plan plan;
  const wz3_result built = wz3_draw_list_build(&r->list, &source, frame, vp, &r->sort_ids, &plan, &r->culling);
  glr__note(r, built);
  if (built != WZ3_OK) return;
  wz3_draw_list_sort(&r->list);
  glr__prepare_instance_uploads(r, host, frame, vp, &r->list);
  wz3_insert_tracker tracker;
  wz3_insert_tracker_init(&tracker, r->list.items, r->list.n_items);
  uint8_t need_current[256];
  memset(need_current, 0, sizeof need_current);
  for (uint32_t i = 0; i < r->list.n_items; i++) {
    const wz3_draw_item *item = &r->list.items[i];
    if (item->kind != WZ3_DRAW_EFFECT) continue;
    const wz3_effect_job *job = &frame->effect_jobs[vp->first_effect + item->job];
    const wz3_effect *effect = wz3_rt_effect(host->rt, job->effect_op);
    if (effect && effect->need_current_render) need_current[item->key.render_pass & 255] = 1;
  }

  int stencil_active = 0;
  int32_t stencil_pass = 0, stencil_light = 0;
  const wz3_draw_item *previous = NULL;
  int32_t current_render_pass = -1;
  glr_target *current_render_target = NULL;
  int viewport_scissor_active = 1;
  int prelight_pending = 0;
  int32_t prelight_render_pass = -1;
#define GLR_RESTORE_SCISSOR()                                      \
  do {                                                             \
    if (!viewport_scissor_active) glr__set_viewport_region(r, vp); \
    viewport_scissor_active = 1;                                   \
  } while (0)
#define GLR_CAPTURE_PRELIGHT()                                \
  do {                                                        \
    if (prelight_pending) {                                   \
      GLR_RESTORE_SCISSOR();                                  \
      glr__capture_framebuffer(r, &r->prelight, destination); \
      r->current_prelight = r->prelight.color;                \
      glr__bind_destination(r, destination);                  \
      glr__set_viewport_region(r, vp);                        \
      viewport_scissor_active = 1;                            \
      prelight_pending = 0;                                   \
    }                                                         \
  } while (0)
  for (uint32_t index = 0; index < r->list.n_items; index++) {
    const wz3_draw_item *item = &r->list.items[index];
    const uint8_t usage = item->key.usage;
    if (usage == WZ3_USAGE_SHADOW && r->no_shadows) continue;
    const int usage_changed = wz3_render_usage_changed(previous ? &previous->key : NULL, &item->key);
    if (usage_changed) GLR_RESTORE_SCISSOR();
    const int insert_action = wz3_insert_tracker_transition(&tracker, item);
    if (insert_action) {
      GLR_RESTORE_SCISSOR();
      glr__draw_material11_insert(r, insert_action);
    }
    if (prelight_pending && (usage != WZ3_USAGE_PRELIGHT || item->key.render_pass != prelight_render_pass)) GLR_CAPTURE_PRELIGHT();
    if (item->key.render_pass != current_render_pass) {
      current_render_pass = item->key.render_pass;
      current_render_target = NULL;
      if (need_current[current_render_pass & 255]) {
        GLR_RESTORE_SCISSOR();
        current_render_target = glr__post_target(r, depth, destination);
        glr__capture_framebuffer(r, current_render_target, destination);
        glr__bind_destination(r, destination);
        glr__set_viewport_region(r, vp);
        viewport_scissor_active = 1;
      }
    }
    if (usage == WZ3_USAGE_SHADOW || usage == WZ3_USAGE_LIGHT) {
      if (!stencil_active || stencil_pass != item->key.render_pass || stencil_light != item->key.light_index) {
        GLR_RESTORE_SCISSOR();
        gl->glStencilMask(0xff);
        gl->glClear(GLR_STENCIL_BUFFER_BIT);
        stencil_active = 1;
        stencil_pass = item->key.render_pass;
        stencil_light = item->key.light_index;
      }
    } else if (stencil_active) {
      GLR_RESTORE_SCISSOR();
      gl->glStencilMask(0xff);
      gl->glClear(GLR_STENCIL_BUFFER_BIT);
      stencil_active = 0;
    }
    const wz3_light_job *light = (item->key.light_index >= 0 && (uint32_t)item->key.light_index < plan.n_lights) ? plan.lights[item->key.light_index] : NULL;
    if (usage_changed && light && (usage == WZ3_USAGE_SHADOW || usage == WZ3_USAGE_LIGHT)) {
      glr__set_light_scissor(r, plan.has_rect[item->key.light_index] ? plan.rect[item->key.light_index] : NULL, &viewport_pixels);
      viewport_scissor_active = 0;
    }
    if (item->kind == WZ3_DRAW_EFFECT) {
      glr__draw_effect_item(r, host, frame, vp, &plan.matrices, item, destination, depth, plan.lights, plan.n_lights, current_render_target);
    } else {
      const wz3_mesh_job *job = &frame->mesh_jobs[vp->first_mesh + item->job];
      glr_geom *geometry = glr__geometry_for_job(r, host, job);
      uint32_t floats = 0;
      const float *matrices = glr__job_matrices(r, frame, job, &floats);
      if (geometry && floats) {
        if (item->kind == WZ3_DRAW_SHADOW) {
          const wz3_shadow_data *data = host->shadow_data ? host->shadow_data(host->user, (uint32_t)job->mesh_op) : NULL;
          glr__draw_shadow(r, host, &plan.matrices, geometry, data, r->list.shadow_groups + item->shadow_first, item->shadow_count, light, item->shadow_zfail,
                           matrices, floats);
        } else {
          const wz3_light_job *single[1];
          const wz3_light_job *const *use_lights = plan.lights;
          uint32_t n_use = plan.n_lights;
          if (light) {
            single[0] = light;
            use_lights = single;
            n_use = 1;
          }
          glr__draw_mesh(r, host, vp, &plan.matrices, geometry, &geometry->groups[item->group], item->material_op, item->pass_index, job, matrices, floats,
                         job->matrix, geometry->g && geometry->g->animated, use_lights, n_use);
        }
      }
    }
    if (usage == WZ3_USAGE_PRELIGHT && item->material_op >= 0) {
      const wz3_material *m = wz3_rt_material(host->rt, (uint32_t)item->material_op);
      if (m && m->system == 20) {
        prelight_pending = 1;
        prelight_render_pass = item->key.render_pass;
      }
    }
    previous = item;
  }
  GLR_RESTORE_SCISSOR();
  const int final_action = wz3_insert_tracker_finish(&tracker);
  if (final_action) {
    GLR_RESTORE_SCISSOR();
    glr__draw_material11_insert(r, final_action);
  }
  GLR_CAPTURE_PRELIGHT();
  if (stencil_active) {
    GLR_RESTORE_SCISSOR();
    gl->glStencilMask(0xff);
    gl->glClear(GLR_STENCIL_BUFFER_BIT);
  }
  GLR_RESTORE_SCISSOR();
  gl->glColorMask(1, 1, 1, 1);
  gl->glDepthMask(1);
  gl->glDisable(GLR_BLEND);
  gl->glDisable(GLR_STENCIL_TEST);
  gl->glStencilMask(0xff);
  r->instance_active = 0;
#undef GLR_RESTORE_SCISSOR
#undef GLR_CAPTURE_PRELIGHT
}

static void glr__render_node(glr *r, const glr_host *host, const wz3_frame *frame, int32_t node_index, const glr_target *destination, int32_t depth);

static void glr__render_layer(glr *r, const glr_host *host, const wz3_frame *frame, const wz3_ipp_node *layer, const glr_target *destination, int32_t depth)
{
  const glr_gl *gl = &r->gl;
  if (layer->input >= 0) glr__render_node(r, host, frame, layer->input, destination, depth + 1);
  glr__bind_destination(r, destination);
  const int32_t flags = layer->clear_flags;
  if (flags) {
    gl->glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    GLRuint mask = 0;
    if (flags & 1) {
      gl->glColorMask(1, 1, 1, 1);
      mask |= GLR_COLOR_BUFFER_BIT;
    }
    if (flags & 2) {
      gl->glDepthMask(1);
      gl->glStencilMask(0xff);
      mask |= GLR_DEPTH_BUFFER_BIT | GLR_STENCIL_BUFFER_BIT;
    }
    gl->glClear(mask);
  }
  r->current_prelight = 0;
  const wz3_material *material = layer->material_op >= 0 ? wz3_rt_material(host->rt, (uint32_t)layer->material_op) : NULL;
  if (material)
    for (uint32_t i = 0; i < material->n_passes; i++) glr__draw_layer_quad(r, host, layer, material, &material->passes[i]);
}

static void glr__render_node(glr *r, const glr_host *host, const wz3_frame *frame, int32_t node_index, const glr_target *destination, int32_t depth)
{
  const glr_gl *gl = &r->gl;
  if (node_index < 0 || (uint32_t)node_index >= frame->n_nodes) {
    glr__bind_destination(r, destination);
    gl->glColorMask(1, 1, 1, 1);
    gl->glDepthMask(1);
    gl->glStencilMask(0xff);
    gl->glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    gl->glClear(GLR_COLOR_BUFFER_BIT | GLR_DEPTH_BUFFER_BIT | GLR_STENCIL_BUFFER_BIT);
    return;
  }
  const wz3_ipp_node *node = &frame->nodes[node_index];
  if (node->type == WZ3_IPP_LAYER2D)
    glr__render_layer(r, host, frame, node, destination, depth);
  else if (node->type == WZ3_IPP_VIEWPORT) {
    glr__bind_destination(r, destination);
    glr__draw_viewport_scene(r, host, frame, node, destination, depth);
  } else
    r->unsupported_nodes++;
}

static int glr__ensure_op_tables(glr *r, uint32_t n_ops)
{
  if (r->n_op_tables == n_ops) return 1;
  if (r->n_op_tables) return 0; /* the operator count of a document does not change */
  r->tex_by_op = (glr_tex **)glr__alloc(r, (size_t)n_ops * sizeof *r->tex_by_op + 8);
  r->geom_by_op = (glr_geom **)glr__alloc(r, (size_t)n_ops * sizeof *r->geom_by_op + 8);
  r->pool_by_op = (glr_pool **)glr__alloc(r, (size_t)n_ops * sizeof *r->pool_by_op + 8);
  r->water_by_op = (glr_water_statics **)glr__alloc(r, (size_t)n_ops * sizeof *r->water_by_op + 8);
  if (!r->tex_by_op || !r->geom_by_op || !r->pool_by_op || !r->water_by_op) {
    GLR_FREE(r->tex_by_op);
    GLR_FREE(r->geom_by_op);
    GLR_FREE(r->pool_by_op);
    GLR_FREE(r->water_by_op);
    r->tex_by_op = NULL;
    r->geom_by_op = NULL;
    r->pool_by_op = NULL;
    r->water_by_op = NULL;
    return 0;
  }
  memset(r->tex_by_op, 0, (size_t)n_ops * sizeof *r->tex_by_op);
  memset(r->geom_by_op, 0, (size_t)n_ops * sizeof *r->geom_by_op);
  memset(r->pool_by_op, 0, (size_t)n_ops * sizeof *r->pool_by_op);
  memset(r->water_by_op, 0, (size_t)n_ops * sizeof *r->water_by_op);
  r->n_op_tables = n_ops;
  return 1;
}

static void glr__clear_master(glr *r);

/* Renderer.render for the nodes `roots` of the frame jobs of host->rt (resize, clear, draw every root, present) */
static glr_result glr__render_roots(glr *r, const glr_host *host, const uint32_t *roots, uint32_t n_roots)
{
  const wz3_frame *frame = wz3_rt_frame_jobs(host->rt);
  r->draw_calls = 0;
  r->triangles = 0;
  r->unsupported_nodes = 0;
  memset(&r->culling, 0, sizeof r->culling);
  r->outputs = n_roots;
  r->oom = 0;
  r->geometry_frame++;
  uint32_t total_jobs = 0;
  for (uint32_t i = 0; i < frame->n_nodes; i++) {
    const uint32_t end = frame->nodes[i].first_mesh + frame->nodes[i].n_mesh;
    if (frame->nodes[i].type == WZ3_IPP_VIEWPORT && end > total_jobs) total_jobs = end;
  }
  r->job_cache = (glr_job_cache *)glr__alloc(r, (size_t)(total_jobs ? total_jobs : 1u) * sizeof *r->job_cache);
  if (!r->job_cache) return GLR_ERR_OUT_OF_MEMORY;
  memset(r->job_cache, 0, (size_t)(total_jobs ? total_jobs : 1u) * sizeof *r->job_cache);
  r->n_job_cache = total_jobs;
  glr__clear_master(r);
  for (uint32_t i = 0; i < n_roots; i++) glr__render_node(r, host, frame, (int32_t)roots[i], &r->master, 0);
  glr_present(r, r->master.color);
  for (uint32_t i = 0; i < total_jobs; i++) GLR_FREE(r->job_cache[i].data);
  GLR_FREE(r->job_cache);
  r->job_cache = NULL;
  r->n_job_cache = 0;
  GLR_FREE(r->job_scratch);
  r->job_scratch = NULL;
  return r->oom ? GLR_ERR_OUT_OF_MEMORY : GLR_OK;
}

GLR_API glr_result glr_render_frame(glr *r, const glr_host *host)
{
  if (!r || !host || !host->rt) return GLR_ERR_ARGUMENT;
  if (!glr__ensure_op_tables(r, host->n_ops)) return GLR_ERR_OUT_OF_MEMORY;
  const wz3_frame *frame = wz3_rt_frame_jobs(host->rt);
  return glr__render_roots(r, host, frame->outputs, frame->n_outputs);
}

GLR_API glr_result glr_render_bitmap(glr *r, const glr_host *host, uint32_t input, int32_t width, int32_t height, wz3_bitmap **out)
{
  if (!r || !host || !host->rt || !out || width < 1 || height < 1) return GLR_ERR_ARGUMENT;
  *out = NULL;
  if (!glr__ensure_op_tables(r, host->n_ops)) return GLR_ERR_OUT_OF_MEMORY;
  int32_t root = -1;
  const wz3_result begun = wz3_rt_render_begin(host->rt, input, (double)width / (double)height, &root);
  if (begun != WZ3_OK) return begun == WZ3_ERR_OUT_OF_MEMORY ? GLR_ERR_OUT_OF_MEMORY : GLR_ERR_ARGUMENT;
  glr_result res = GLR_OK;
  uint8_t *pixels = NULL;
  if (root >= 0) {
    const uint32_t node = (uint32_t)root;
    res = glr_resize(r, width, height, 0.0);
    if (res == GLR_OK) res = glr__render_roots(r, host, &node, 1);
    if (res == GLR_OK) {
      pixels = (uint8_t *)glr__alloc(r, (size_t)width * (size_t)height * 4u);
      if (!pixels)
        res = GLR_ERR_OUT_OF_MEMORY;
      else
        r->gl.glReadPixels(0, 0, width, height, GLR_RGBA, GLR_UNSIGNED_BYTE, pixels);
    }
  }
  wz3_result ended = wz3_rt_render_end(host->rt);
  if (ended != WZ3_OK) ended = wz3_rt_render_end(host->rt); /* the restore ran out of memory: once more */
  if (ended != WZ3_OK && res == GLR_OK) res = GLR_ERR_OUT_OF_MEMORY;
  if (res == GLR_OK) {
    *out = pixels ? wz3_bitmap_from_rgba8(width, height, pixels, width, 1) : wz3_bitmap_create(width, height);
    if (!*out) res = GLR_ERR_OUT_OF_MEMORY;
  }
  GLR_FREE(pixels);
  return res;
}

GLR_API glr_result glr_warm_texture(glr *r, const glr_host *host, uint32_t op)
{
  if (!r || !host || !host->bitmap || op >= host->n_ops) return GLR_ERR_ARGUMENT;
  if (!glr__ensure_op_tables(r, host->n_ops)) return GLR_ERR_OUT_OF_MEMORY;
  r->oom = 0;
  wz3_sampler_state *sampler = NULL;
  int mipmapped = 0;
  const GLRuint texture = glr__texture_get(r, host, (int32_t)op, &sampler, &mipmapped);
  return texture && !r->oom ? GLR_OK : GLR_ERR_OUT_OF_MEMORY;
}

GLR_API glr_result glr_warm_geometry(glr *r, const glr_host *host, uint32_t op)
{
  if (!r || !host || !host->geometry || op >= host->n_ops) return GLR_ERR_ARGUMENT;
  if (!glr__ensure_op_tables(r, host->n_ops)) return GLR_ERR_OUT_OF_MEMORY;
  const wz3_geometry *base = host->geometry(host->user, op, -1.0, 0);
  if (!base || base->animated) return GLR_OK;
  r->oom = 0;
  const wz3_shadow_data *shadow = host->shadow_data ? host->shadow_data(host->user, op) : NULL;
  return glr__geom_get_static(r, op, base, shadow) && !r->oom ? GLR_OK : GLR_ERR_OUT_OF_MEMORY;
}

static void glr__configure_fullscreen_state(glr *r)
{
  const glr_gl *gl = &r->gl;
  gl->glDisable(GLR_DEPTH_TEST);
  gl->glDepthMask(0);
  gl->glDisable(GLR_CULL_FACE);
  gl->glDisable(GLR_POLYGON_OFFSET_FILL);
  gl->glDisable(GLR_STENCIL_TEST);
  gl->glDisable(GLR_BLEND);
  gl->glColorMask(1, 1, 1, 1);
}

static void glr__bind_master(glr *r)
{
  const glr_gl *gl = &r->gl;
  gl->glBindFramebuffer(GLR_FRAMEBUFFER, r->master.framebuffer);
  gl->glViewport(0, 0, r->master.width, r->master.height);
  gl->glDisable(GLR_SCISSOR_TEST);
}

static void glr__clear_master(glr *r)
{
  const glr_gl *gl = &r->gl;
  glr__bind_master(r);
  gl->glColorMask(1, 1, 1, 1);
  gl->glDepthMask(1);
  gl->glStencilMask(0xff);
  gl->glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
  gl->glClearDepthf(1.0f);
  gl->glClearStencil(0);
  gl->glClear(GLR_COLOR_BUFFER_BIT | GLR_DEPTH_BUFFER_BIT | GLR_STENCIL_BUFFER_BIT);
}

GLR_API void glr_present(glr *r, GLRuint texture)
{
  const glr_gl *gl = &r->gl;
  gl->glBindFramebuffer(GLR_FRAMEBUFFER, 0);
  gl->glDisable(GLR_SCISSOR_TEST);
  gl->glViewport(0, 0, r->canvas_width, r->canvas_height);
  gl->glColorMask(1, 1, 1, 1);
  gl->glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
  gl->glClear(GLR_COLOR_BUFFER_BIT);
  glr__configure_fullscreen_state(r);
  gl->glViewport(r->presentation.x, r->presentation.y, r->presentation.width, r->presentation.height);
  glr__draw_fullscreen(r, texture);
}

GLR_API GLRuint glr_master_texture(const glr *r) { return r->master.color; }

GLR_API glr_result glr_present_overlay(glr *r, const uint8_t *rgba, int32_t width, int32_t height, int32_t x, int32_t y)
{
  if (!r || !rgba || width < 1 || height < 1 || width > 8192 || height > 8192) return GLR_ERR_ARGUMENT;
  const glr_gl *gl = &r->gl;
  const size_t row = (size_t)width * 4u;
  uint8_t *flipped = (uint8_t *)GLR_MALLOC(row * (size_t)height); /* GL rows run bottom-up */
  if (!flipped) return GLR_ERR_OUT_OF_MEMORY;
  for (int32_t j = 0; j < height; j++) memcpy(flipped + (size_t)(height - 1 - j) * row, rgba + (size_t)j * row, row);
  if (!r->overlay_texture) gl->glGenTextures(1, &r->overlay_texture);
  gl->glActiveTexture(GLR_TEXTURE0);
  gl->glBindTexture(GLR_TEXTURE_2D, r->overlay_texture);
  gl->glTexImage2D(GLR_TEXTURE_2D, 0, GLR_RGBA8, width, height, 0, GLR_RGBA, GLR_UNSIGNED_BYTE, flipped);
  gl->glTexParameteri(GLR_TEXTURE_2D, GLR_TEXTURE_MIN_FILTER, GLR_NEAREST);
  gl->glTexParameteri(GLR_TEXTURE_2D, GLR_TEXTURE_MAG_FILTER, GLR_NEAREST);
  gl->glTexParameteri(GLR_TEXTURE_2D, GLR_TEXTURE_WRAP_S, GLR_CLAMP_TO_EDGE);
  gl->glTexParameteri(GLR_TEXTURE_2D, GLR_TEXTURE_WRAP_T, GLR_CLAMP_TO_EDGE);
  GLR_FREE(flipped);
  gl->glBindFramebuffer(GLR_FRAMEBUFFER, 0);
  gl->glDisable(GLR_SCISSOR_TEST);
  gl->glColorMask(1, 1, 1, 1);
  glr__configure_fullscreen_state(r);
  gl->glEnable(GLR_BLEND);
  gl->glBlendEquation(GLR_FUNC_ADD);
  gl->glBlendFunc(GLR_ONE, GLR_ONE_MINUS_SRC_ALPHA);
  gl->glViewport(x, r->canvas_height - y - height, width, height);
  glr__draw_fullscreen(r, r->overlay_texture);
  gl->glDisable(GLR_BLEND);
  return GLR_OK;
}

/* Black screen, a dark track in the middle and a white run from..to (pixels along the track) over it. */
static void glr__draw_loader(glr *r, int32_t bw, int32_t bh, int32_t x0, int32_t y0, int32_t from, int32_t to)
{
  const glr_gl *gl = &r->gl;
  const int32_t cw = r->canvas_width, ch = r->canvas_height;
  from = from < 0 ? 0 : from;
  to = to > bw ? bw : to;
  gl->glBindFramebuffer(GLR_FRAMEBUFFER, 0);
  gl->glViewport(0, 0, cw, ch);
  gl->glDisable(GLR_SCISSOR_TEST);
  gl->glColorMask(1, 1, 1, 1);
  gl->glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
  gl->glClear(GLR_COLOR_BUFFER_BIT);
  gl->glEnable(GLR_SCISSOR_TEST);
  gl->glScissor(x0, y0, bw, bh);
  gl->glClearColor(0.2f, 0.2f, 0.2f, 1.0f);
  gl->glClear(GLR_COLOR_BUFFER_BIT);
  if (to > from) {
    gl->glScissor(x0 + from, y0, to - from, bh);
    gl->glClearColor(1.0f, 1.0f, 1.0f, 1.0f);
    gl->glClear(GLR_COLOR_BUFFER_BIT);
  }
  gl->glDisable(GLR_SCISSOR_TEST);
}

static void glr__loader_track(const glr *r, int32_t *bw, int32_t *bh, int32_t *x0, int32_t *y0)
{
  *bw = r->canvas_width / 3;
  *bh = r->canvas_height / 60;
  *bw = *bw < 8 ? 8 : *bw;
  *bh = *bh < 4 ? 4 : *bh;
  *x0 = (r->canvas_width - *bw) / 2;
  *y0 = (r->canvas_height - *bh) / 2;
}

GLR_API void glr_present_loading(glr *r, double phase)
{
  int32_t bw, bh, x0, y0;
  glr__loader_track(r, &bw, &bh, &x0, &y0);
  const int32_t seg = bw / 4;
  phase = phase - (double)(int64_t)phase;
  phase = phase < 0 ? phase + 1 : phase;
  const int32_t from = (int32_t)(phase * (double)(bw + seg)) - seg;
  glr__draw_loader(r, bw, bh, x0, y0, from, from + seg);
}

GLR_API void glr_present_progress(glr *r, double fraction)
{
  int32_t bw, bh, x0, y0;
  glr__loader_track(r, &bw, &bh, &x0, &y0);
  if (!(fraction > 0.0)) fraction = 0.0;
  if (fraction > 1.0) fraction = 1.0;
  glr__draw_loader(r, bw, bh, x0, y0, 0, (int32_t)(fraction * (double)bw));
}

GLR_API glr_result glr_render(glr *r, const wz3_frame *frame)
{
  if (!r) return GLR_ERR_ARGUMENT;
  r->draw_calls = 0;
  r->triangles = 0;
  r->outputs = frame ? frame->n_outputs : 0;
  r->unsupported_nodes = r->outputs;
  glr__clear_master(r);
  glr_present(r, r->master.color);
  return GLR_OK;
}

GLR_API int glr_read_pixels(glr *r, uint8_t *rgba, int32_t width, int32_t height)
{
  if (!r || !rgba || width < 1 || height < 1) return 0;
  const size_t row = (size_t)width * 4;
  r->gl.glBindFramebuffer(GLR_FRAMEBUFFER, 0);
  r->gl.glReadPixels(0, 0, width, height, GLR_RGBA, GLR_UNSIGNED_BYTE, rgba);
  for (int32_t y = 0; y < height / 2; y++) { /* GL rows run bottom-up */
    uint8_t *a = rgba + (size_t)y * row, *b = rgba + (size_t)(height - 1 - y) * row;
    for (size_t i = 0; i < row; i++) {
      const uint8_t t = a[i];
      a[i] = b[i];
      b[i] = t;
    }
  }
  return 1;
}

GLR_API void glr_get_stats(const glr *r, glr_stats *out)
{
  memset(out, 0, sizeof *out);
  out->draw_calls = r->draw_calls;
  out->triangles = r->triangles;
  out->outputs = r->outputs;
  out->unsupported_nodes = r->unsupported_nodes;
  out->culling = r->culling;
  out->width = r->canvas_width;
  out->height = r->canvas_height;
  out->render_width = r->width;
  out->render_height = r->height;
  out->presentation = r->presentation;
}

#endif /* GLR_IMPLEMENTATION_DONE */
#endif /* GLR_IMPLEMENTATION */
