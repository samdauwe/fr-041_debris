/* Stand-ins for the suites that need EGL and OpenGL ES development files, used when the machine has none. */

#include "suite.h"

int suite_glr_egl(int argc, char **argv);
int suite_glr_egl_frames(int argc, char **argv);
int suite_player_scene_egl(int argc, char **argv);

int suite_glr_egl(int argc, char **argv)
{
  (void)argc;
  (void)argv;
  return DEBRIS_TEST_SKIP;
}

int suite_glr_egl_frames(int argc, char **argv)
{
  (void)argc;
  (void)argv;
  return DEBRIS_TEST_SKIP;
}

int suite_player_scene_egl(int argc, char **argv)
{
  (void)argc;
  (void)argv;
  return DEBRIS_TEST_SKIP;
}
