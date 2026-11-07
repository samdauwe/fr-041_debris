/* v2m_tool: small helpers that prepare test inputs; plain C, no dependency on v2.h.

     v2m_tool extract-kx  IN.kx  OUT.v2m
         the V2M embedded in a debris .kx payload (after the 40-byte KX header;
         the little-endian payload length is the header's final word)
     v2m_tool move-channel IN.v2m OUT.v2m FROM TO
         move the MIDI stream of channel FROM into the empty channel slot TO
         (e.g. 14 -> 15 routes a song's music through the Ronan speech synth) */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int read_file(const char *path, uint8_t **data, size_t *size)
{
  FILE *f = fopen(path, "rb");
  if (!f) return 0;
  fseek(f, 0, SEEK_END);
  long len = ftell(f);
  fseek(f, 0, SEEK_SET);
  *data = (uint8_t *)malloc(len > 0 ? (size_t)len : 1);
  if (!*data || (len > 0 && fread(*data, 1, (size_t)len, f) != (size_t)len)) {
    fclose(f);
    return 0;
  }
  *size = (size_t)len;
  fclose(f);
  return 1;
}

static int write_file(const char *path, const uint8_t *data, size_t size)
{
  FILE *f = fopen(path, "wb");
  if (!f) return 0;
  int ok = !size || fwrite(data, 1, size, f) == size;
  return fclose(f) == 0 && ok;
}

static uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }

static int extract_kx(const char *in, const char *out)
{
  uint8_t *d;
  size_t n;
  if (!read_file(in, &d, &n)) {
    perror(in);
    return 1;
  }
  if (n < 40) {
    fprintf(stderr, "%s: too short for a KX header\n", in);
    return 1;
  }
  size_t len = rd32(d + 36);
  if (len > n - 40) {
    fprintf(stderr, "%s: truncated V2M payload\n", in);
    return 1;
  }
  if (!write_file(out, d + 40, len)) {
    perror(out);
    return 1;
  }
  free(d);
  return 0;
}

/* byte range [start,end) of each of the 16 channel slots; -1 on a malformed file */
static int channel_slots(const uint8_t *d, size_t n, size_t start[16], size_t end[16], uint32_t notes[16])
{
  if (n < 12) return -1;
  size_t pos = 12 + (size_t)10 * rd32(d + 8);
  for (int ch = 0; ch < 16; ch++) {
    start[ch] = pos;
    if (pos + 4 > n) return -1;
    notes[ch] = rd32(d + pos);
    pos += 4;
    if (notes[ch]) {
      pos += (size_t)5 * notes[ch];
      if (pos + 4 > n) return -1;
      pos += 4 + (size_t)4 * rd32(d + pos);
      if (pos + 4 > n) return -1;
      pos += 4 + (size_t)5 * rd32(d + pos);
      for (int cc = 0; cc < 7; cc++) {
        if (pos + 4 > n) return -1;
        pos += 4 + (size_t)4 * rd32(d + pos);
      }
    }
    if (pos > n) return -1;
    end[ch] = pos;
  }
  return 0;
}

static int move_channel(const char *in, const char *out, int from, int to)
{
  uint8_t *d;
  size_t n;
  size_t st[16], en[16];
  uint32_t notes[16];
  if (from < 0 || from > 15 || to < 0 || to > 15 || from == to) {
    fprintf(stderr, "bad channel\n");
    return 1;
  }
  if (!read_file(in, &d, &n)) {
    perror(in);
    return 1;
  }
  if (channel_slots(d, n, st, en, notes)) {
    fprintf(stderr, "%s: malformed v2m\n", in);
    return 1;
  }
  if (notes[to]) {
    fprintf(stderr, "channel %d is not empty\n", to);
    return 1;
  }

  uint8_t *o = (uint8_t *)malloc(n);
  size_t w = 0, pos = 0;
  for (int ch = 0; ch < 16; ch++) {
    memcpy(o + w, d + pos, st[ch] - pos);
    w += st[ch] - pos;
    int src = ch == from ? to : ch == to ? from : ch;
    memcpy(o + w, d + st[src], en[src] - st[src]);
    w += en[src] - st[src];
    pos = en[ch];
  }
  memcpy(o + w, d + pos, n - pos);
  w += n - pos;
  int ok = write_file(out, o, w);
  if (!ok) perror(out);
  free(o);
  free(d);
  return ok ? 0 : 1;
}

int main(int argc, char **argv)
{
  if (argc == 4 && !strcmp(argv[1], "extract-kx")) return extract_kx(argv[2], argv[3]);
  if (argc == 6 && !strcmp(argv[1], "move-channel")) return move_channel(argv[2], argv[3], atoi(argv[4]), atoi(argv[5]));
  fprintf(stderr, "usage: %s extract-kx IN.kx OUT.v2m\n       %s move-channel IN.v2m OUT.v2m FROM TO\n", argv[0], argv[0]);
  return 2;
}
