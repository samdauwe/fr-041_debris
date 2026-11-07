/* Shadow volumes against the released C++ (EngMesh::UpdateShadowCacheJob), via tools/oracles/shadow_volume_oracle.cpp.
 *
 *   test_wz3_shadow_oracle gen <input> <expected>      writes shadow jobs (planes, edges, caps) of closed solids and random lights
 *   test_wz3_shadow_oracle check <expected> <output>   compares the oracle's index lists with the library's
 *
 * The jobs are the library's own topologies of closed manifold solids (a cube, an octahedron and subdivided spheres built as
 * polygon fans) so both sides extract the silhouette and caps of the same data; lights are random and include positions
 * inside the solid, on the axes and far away. Native edges are the form EngMesh::Job::Edges holds: doubled vertex indices
 * and the two polygons of the edge.
 */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define WZ3_IMPLEMENTATION
#include <debris/core/wz3.h>
#include "support/gate_common.h"

static uint32_t g_rng = 0x2545f491u;
static float frand(float lo, float hi)
{
  g_rng = g_rng * 1664525u + 1013904223u;
  return lo + (hi - lo) * ((float)(g_rng >> 8) / 16777216.0f);
}

typedef struct solid {
  float *positions;
  uint32_t *indices;
  uint32_t *face_map;
  uint32_t nv, ni;
} solid;

static void solid_cube(solid *s)
{
  static const float p[8][3] = { { 0, 0, 0 }, { 1, 0, 0 }, { 1, 1, 0 }, { 0, 1, 0 }, { 0, 0, 1 }, { 1, 0, 1 }, { 1, 1, 1 }, { 0, 1, 1 } };
  static const uint32_t q[6][4] = { { 0, 3, 2, 1 }, { 4, 5, 6, 7 }, { 0, 1, 5, 4 }, { 1, 2, 6, 5 }, { 2, 3, 7, 6 }, { 3, 0, 4, 7 } };
  s->nv = 8;
  s->ni = 36;
  s->positions = (float *)malloc(8 * 12);
  s->indices = (uint32_t *)malloc(36 * 4);
  s->face_map = (uint32_t *)malloc(12 * 4);
  memcpy(s->positions, p, sizeof p);
  uint32_t n = 0;
  for (uint32_t f = 0; f < 6; f++) {
    s->indices[n] = q[f][0];
    s->indices[n + 1] = q[f][1];
    s->indices[n + 2] = q[f][2];
    s->face_map[n / 3] = f;
    n += 3;
    s->indices[n] = q[f][0];
    s->indices[n + 1] = q[f][2];
    s->indices[n + 2] = q[f][3];
    s->face_map[n / 3] = f;
    n += 3;
  }
}

/* an octahedron whose triangles are subdivided `levels` times (every triangle into four) and pushed onto the sphere */
static void solid_sphere(solid *s, int levels)
{
  static const float p[6][3] = { { 1, 0, 0 }, { -1, 0, 0 }, { 0, 1, 0 }, { 0, -1, 0 }, { 0, 0, 1 }, { 0, 0, -1 } };
  static const uint32_t t[8][3] = { { 0, 2, 4 }, { 2, 1, 4 }, { 1, 3, 4 }, { 3, 0, 4 }, { 2, 0, 5 }, { 1, 2, 5 }, { 3, 1, 5 }, { 0, 3, 5 } };
  uint32_t cap_t = 8, nt = 8;
  for (int l = 0; l < levels; l++) cap_t *= 4;
  float *pos = (float *)malloc((size_t)(6 + cap_t * 3) * 12);
  uint32_t *tri = (uint32_t *)malloc((size_t)cap_t * 3 * 4), *tri2 = (uint32_t *)malloc((size_t)cap_t * 3 * 4);
  uint32_t nv = 6;
  memcpy(pos, p, sizeof p);
  for (uint32_t i = 0; i < 8; i++) {
    tri[i * 3] = t[i][0];
    tri[i * 3 + 1] = t[i][1];
    tri[i * 3 + 2] = t[i][2];
  }
  for (int l = 0; l < levels; l++) {
    uint32_t m = 0;
    for (uint32_t i = 0; i < nt; i++) {
      uint32_t v[3] = { tri[i * 3], tri[i * 3 + 1], tri[i * 3 + 2] }, mid[3];
      for (int k = 0; k < 3; k++) {
        const float *a = pos + (size_t)v[k] * 3, *b = pos + (size_t)v[(k + 1) % 3] * 3;
        float c[3] = { (a[0] + b[0]) / 2, (a[1] + b[1]) / 2, (a[2] + b[2]) / 2 };
        const float len = (float)sqrt((double)c[0] * c[0] + (double)c[1] * c[1] + (double)c[2] * c[2]);
        uint32_t found = 0xffffffffu;
        for (uint32_t u = 6; u < nv; u++)
          if (pos[u * 3] == c[0] / len && pos[u * 3 + 1] == c[1] / len && pos[u * 3 + 2] == c[2] / len) found = u;
        if (found == 0xffffffffu) {
          pos[nv * 3] = c[0] / len;
          pos[nv * 3 + 1] = c[1] / len;
          pos[nv * 3 + 2] = c[2] / len;
          found = nv++;
        }
        mid[k] = found;
      }
      const uint32_t out[4][3] = { { v[0], mid[0], mid[2] }, { mid[0], v[1], mid[1] }, { mid[2], mid[1], v[2] }, { mid[0], mid[1], mid[2] } };
      for (int k = 0; k < 4; k++) {
        tri2[m * 3] = out[k][0];
        tri2[m * 3 + 1] = out[k][1];
        tri2[m * 3 + 2] = out[k][2];
        m++;
      }
    }
    memcpy(tri, tri2, (size_t)m * 12);
    nt = m;
  }
  s->nv = nv;
  s->ni = nt * 3;
  s->positions = pos;
  s->indices = tri;
  free(tri2);
  s->face_map = (uint32_t *)malloc((size_t)nt * 4);
  for (uint32_t i = 0; i < nt; i++) s->face_map[i] = i;
}

static void solid_free(solid *s)
{
  free(s->positions);
  free(s->indices);
  free(s->face_map);
}

static int g_gen_cases;

static void gen(const char *input_path, const char *expected_path)
{
  FILE *in = fopen(input_path, "w"), *ex = fopen(expected_path, "w");
  if (!in || !ex) {
    printf("cannot write %s / %s\n", input_path, expected_path);
    exit(2);
  }
  int id = 0;
  for (int which = 0; which < 5; which++) {
    solid s;
    if (which == 0)
      solid_cube(&s);
    else
      solid_sphere(&s, which - 1);
    wz3_geometry g;
    memset(&g, 0, sizeof g);
    g.n_vertices = s.nv;
    g.n_indices = s.ni;
    g.positions = s.positions;
    g.indices = s.indices;
    wz3_shadow_data data;
    memset(&data, 0, sizeof data);
    data.n_triangles = s.ni / 3;
    data.face_map = s.face_map;
    const uint32_t range[2] = { 0, s.ni };
    wz3_shadow_topology t;
    CHECK(wz3_shadow_topology_prepare(&g, &data, range, 1, 0, &t) == WZ3_OK, "topology %d", which);
    CHECK(t.boundary_edges == 0 && t.non_manifold_edges == 0 && t.winding_conflict_edges == 0, "solid %d is closed", which);
    const int lights = which == 0 ? 400 : 200;
    for (int li = 0; li < lights; li++) {
      float light[3];
      const float spread = li % 4 == 0 ? 1.2f : li % 4 == 1 ? 3.0f : li % 4 == 2 ? 40.0f : 0.6f;
      for (int k = 0; k < 3; k++) light[k] = frand(-spread, spread) + (which == 0 ? 0.5f : 0.0f);
      wz3_shadow_volume v;
      CHECK(wz3_shadow_volume_build(&t, light, NULL, 1, &v) == WZ3_OK, "volume");
      fprintf(in, "case %d\nplanes %u", id, t.n_polygons);
      for (uint32_t i = 0; i < t.n_polygons * 4; i++) {
        uint32_t w;
        memcpy(&w, &t.polygon_planes[i], 4);
        fprintf(in, " %x", w);
      }
      fprintf(in, "\nedges %u", t.n_edges);
      for (uint32_t e = 0; e < t.n_edges; e++) fprintf(in, " %u %u %u %u", t.edges[e].a0 * 2, t.edges[e].b0 * 2, t.edges[e].f0, t.edges[e].f1);
      fprintf(in, "\ncaps %u", t.n_triangles * 3);
      for (uint32_t i = 0; i < t.n_triangles * 3; i++) fprintf(in, " %u", t.faces[i] * 2);
      for (uint32_t i = 0; i < t.n_triangles; i++) fprintf(in, " %u", t.triangle_polygon[i]);
      uint32_t lw[3];
      memcpy(lw, v.light_position, 12);
      fprintf(in, "\nlight %x %x %x\n", lw[0], lw[1], lw[2]);
      uint32_t h = 0x811c9dc5u;
      for (uint32_t i = 0; i < v.n_indices; i++) h = (h ^ v.indices[i]) * 0x01000193u;
      fprintf(ex, "case %d sil %u cap %u hash %08x\n", id, v.silhouette_index_count, v.cap_index_count, h);
      wz3_shadow_volume_free(&v);
      id++;
    }
    wz3_shadow_topology_free(&t);
    solid_free(&s);
  }
  fprintf(ex, "cases %d\n", id);
  g_gen_cases = id;
  fclose(in);
  fclose(ex);
}

int main(int argc, char **argv)
{
  if (argc >= 4 && !strcmp(argv[1], "gen")) {
    gen(argv[2], argv[3]);
    printf("shadow oracle: wrote %d cases, %d checks, %d failures\n", g_gen_cases, g_checks, g_failures);
    return g_failures ? 1 : 0;
  }
  if (argc >= 4 && !strcmp(argv[1], "check")) {
    FILE *a = fopen(argv[2], "r"), *b = fopen(argv[3], "r");
    if (!a || !b) {
      printf("cannot open the result files\n");
      return 2;
    }
    char la[256], lb[256];
    unsigned compared = 0, total_sil = 0, total_cap = 0;
    while (fgets(la, sizeof la, a)) {
      if (!fgets(lb, sizeof lb, b)) {
        printf("oracle output is shorter\n");
        return 1;
      }
      CHECK(!strcmp(la, lb), "library %soracle  %s", la, lb);
      unsigned id, sil, cap;
      if (sscanf(la, "case %u sil %u cap %u", &id, &sil, &cap) == 3) {
        compared++;
        total_sil += sil;
        total_cap += cap;
      }
    }
    CHECK(fgets(lb, sizeof lb, b) == NULL, "oracle output is longer");
    CHECK(compared >= 1000 && total_sil > 20000 && total_cap > 100000, "%u cases, %u silhouette indices, %u cap indices", compared, total_sil, total_cap);
    printf("shadow oracle: %u volumes identical to EngMesh::UpdateShadowCacheJob (%u silhouette, %u cap indices), %d checks, %d failures\n", compared,
           total_sil, total_cap, g_checks, g_failures);
    return g_failures ? 1 : 0;
  }
  printf("usage: test_wz3_shadow_oracle gen <input> <expected> | check <expected> <output>\n");
  return 2;
}
