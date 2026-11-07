/* ======================================================================== */
/*  frame-job state: types the runtime embeds; the code is in               */
/*  frame.inc (materials), scene.inc (scene, IPP) and effects.inc           */
/* ======================================================================== */

/* a growable array of fixed-size elements; owned by the runtime, freed in wz3_rt_destroy */
typedef struct wz3__fj_vec {
  void *p;
  uint32_t n, cap;
} wz3__fj_vec;

/* What a scene operator's output contributes to its consumers. genscene.cpp keeps whole child Scene objects, but the
   traversal only ever reads a child's drawMesh, so that is all that is copied (operator index, -1 none). */
typedef struct wz3__fj_scene {
  int32_t draw_mesh, effect;
  uint32_t n_children;
  int32_t *child_mesh;
} wz3__fj_scene;

typedef struct wz3__fj_material {
  wz3_material pub;
  double *store;                    /* params followed by initial */
  wz3_material_pass *passes;
  uint32_t cap_passes;
} wz3__fj_material;

typedef struct wz3__fj {
  double aspect;
  float markers[32][16];            /* environment.markers */
  int32_t render_pass_adjust, last_output;
  uint32_t ambient;
  wz3_camera game_camera, current_camera;
  /* Jobs of every viewport of the frame. A viewport owns the slice that was appended while it traversed its scene
     (frame.meshJobs.slice() in JavaScript); b_* mark where the current traversal began. */
  wz3__fj_vec mesh, effect, light, instance, variables, marker_tables, nodes, outputs, order;
  uint32_t b_mesh, b_effect, b_light;
  int32_t cur_table;                /* marker table of the current traversal (frame.effectMarkers), -1 none yet */
  int32_t *ipp_node;                /* environment.ippOutputs: node index per operator, -1 none */
  wz3_frame view;
  /* deferred-effect results */
  wz3_effect_output out;
  float line_points[96];
  double post_params[256];
} wz3__fj;
