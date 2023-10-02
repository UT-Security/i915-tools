/*
 * Copyright 2023 Hovav Shacham.  All rights reserved; see LICENSE file.
 */
#include <assert.h>
#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <EGL/egl.h>
#include <GL/gl.h>

#include "minidc.h"

#define MAX_CONFIGS 1

static void
egl_init(void)
{
  EGLBoolean api_result;
  
  EGLDisplay d = eglGetDisplay(EGL_DEFAULT_DISPLAY);
  assert(d != EGL_NO_DISPLAY);

  api_result = eglInitialize(d, NULL, NULL);
  assert(api_result);

  api_result = eglBindAPI(EGL_OPENGL_API);
  assert(api_result);

  const int config_attribs[] = {
    EGL_CONFORMANT,      EGL_OPENGL_BIT,
    EGL_RED_SIZE,        1,
    EGL_GREEN_SIZE,      1,
    EGL_BLUE_SIZE,       1,
    EGL_ALPHA_SIZE,      1,
    EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT,
    EGL_NONE
  };

  int num_configs = 0;
  EGLConfig configs[MAX_CONFIGS];

  eglChooseConfig(d, config_attribs, configs, MAX_CONFIGS, &num_configs);
  assert(num_configs > 0);

  const int context_attribs[] = {
    EGL_CONTEXT_MAJOR_VERSION, 4,
    EGL_CONTEXT_MINOR_VERSION, 3,
    EGL_CONTEXT_OPENGL_PROFILE_MASK,
    EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT,
    EGL_NONE,
  };

  EGLContext ctx = eglCreateContext(d, configs[0], EGL_NO_CONTEXT,
                                    context_attribs);
  assert(ctx != NULL);
  
  api_result = eglMakeCurrent(d, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx);
  assert(api_result);
}

static uint8_t *minidc_prf_seed = NULL;

static void
set_prf_seed(char *hex_seed)
{
  minidc_prf_seed = malloc(PRF_KEYLEN);
  assert(minidc_prf_seed != NULL);

  int i;
  int rv;
  for (i = 0; i < PRF_KEYLEN; i++) {
    assert(isxdigit(hex_seed[2*i]));
    assert(isxdigit(hex_seed[2*i+1]));
    rv = sscanf(&hex_seed[2*i], "%2hhx", &minidc_prf_seed[i]);
    assert(rv == 1);
  }
}

static int width  = 1024;
static int height = 512;
static char *prefix = NULL;

uint8_t *c2_base = NULL;
ssize_t c2_len = -1;

static void
dump_files(void)
{
  FILE *maps = fopen("/proc/self/maps", "r");
  assert(maps != NULL);

  char *line = NULL;
  size_t len = 0;
  ssize_t nread;

  unsigned long start, end;

  int candidate = 1;
  while ( (nread = getline(&line, &len, maps)) != -1) {
    if (nread < 21 || 0 != strcmp(line+nread-21, " anon_inode:i915.gem\n"))
      continue;

    int rv = sscanf(line, "%lx-%lx", &start, &end);
    assert(rv == 2);
    assert(start <= end);

    if (end - start < height * width * 4)
      continue;

    char namebuf[64];
    if (prefix == NULL)
      rv = snprintf(namebuf, 64, "candidate-%d.raw", candidate);
    else
      rv = snprintf(namebuf, 64, "%s-candidate-%d.raw", prefix, candidate);
    assert(rv > 0);

    FILE *dump = fopen(namebuf, "w");
    assert (dump != NULL);

    size_t bytes_written = fwrite((void *)start, 1, end-start, dump);
    assert(bytes_written == end - start);

    rv = fclose(dump);
    assert(rv == 0);

    if (candidate == 2) {
      c2_base = (uint8_t *)start;
      c2_len = end - start;
    }

    candidate++;
  }
  free(line);
  fclose(maps);
}

static char *r_prog = NULL;
static char *g_prog = NULL;
static char *b_prog = NULL;
static char *a_prog = NULL;

static GLubyte
get_prog_value_at(char *prog, int chan,
                  int row, int col, int i)
{
  if (prog == NULL)
    return 0;

  reset_for_prog(prog);
  push(chan);                   /* prf domain separation */
  push(row); push(col); push(i);
  eval();
  word wval = pop();
  return wval & 0xff;
}

static GLubyte *pixels = NULL;
static GLubyte *tweaked_pixels = NULL;

static void
compute_pixels(void)
{
  init_dc(minidc_prf_seed);     /* seed is NULL unless -s option given */

  pixels = malloc(width * height * 4);
  assert(pixels != NULL);
  tweaked_pixels = malloc(width * height * 4);
  assert(tweaked_pixels != NULL);

  int i = 0;
  for (int row = 0; row < height; row++) {
    for (int col = 0; col < width; col++) {
      pixels[4*i+0] = get_prog_value_at(r_prog, 0, row, col, i);
      pixels[4*i+1] = get_prog_value_at(g_prog, 1, row, col, i);
      pixels[4*i+2] = get_prog_value_at(b_prog, 2, row, col, i);
      pixels[4*i+3] = get_prog_value_at(a_prog, 3, row, col, i);
      i++;
    }
  }
}

static void
make_tweaks(int argc, char *argv[])
{
  assert(c2_base != NULL);

  for (int i = 0; i < argc-1; i += 2) {
    int pos = atoi(argv[i]);
    char *tweak_prog = argv[i+1];
    assert(pos >=0 && pos < c2_len);

    reset_for_prog(tweak_prog);
    push(c2_base[pos]);
    eval();
    word wval = pop();
    c2_base[pos] = wval & 0xff;
  }
}

static void
dump_tweaked_pixels(void)
{
  int rv;
  char namebuf[64];

  if (prefix == NULL)
    rv = snprintf(namebuf, 64, "tweaked.raw");
  else
    rv = snprintf(namebuf, 64, "%s-tweaked.raw", prefix);
  assert(rv > 0);

  FILE *tweaked_dump = fopen(namebuf, "w");
  assert (tweaked_dump != NULL);

  size_t bytes_written = fwrite(tweaked_pixels, 1, width * height * 4,
                                tweaked_dump);
  assert(bytes_written == width * height * 4);

  rv = fclose(tweaked_dump);
  assert(rv == 0);
}

static void
usage(void)
{
  fprintf(stderr, "Usage: tweak [-p prefix] [-s seed] [-w width] [-h height]\n"
                  "             [-r r_prog] [-g g_prog] [-b b_prog] [-a a_prog]\n"
                  "             [pos1 tweakprog1] [pos2 tweakprog2] ...\n");
  exit(EXIT_FAILURE);
}

static void
parse_args(int argc, char *argv[])
{
  int opt;

  while ( (opt = getopt(argc, argv, "p:s:w:h:r:g:b:a:")) != -1) {
    switch (opt) {
    case 'p':
      prefix = optarg;
      break;
    case 's':
      if (strlen(optarg) < 2*PRF_KEYLEN)
        usage();
      set_prf_seed(optarg);
      break;
    case 'w':
      width = atoi(optarg);
      assert(width > 0);
      assert(width < 10000);
      break;
    case 'h':
      height = atoi(optarg);
      assert(height > 0);
      assert(height < 10000);
      break;
    case 'r':
      r_prog = optarg;
      break;
    case 'g':
      g_prog = optarg;
      break;
    case 'b':
      b_prog = optarg;
      break;
    case 'a':
      a_prog = optarg;
      break;
    default:
      usage();
    }
  }
}

int
main(int argc, char *argv[])
{
  parse_args(argc, argv);

  compute_pixels();

  egl_init();

  GLuint texture_name;
  glGenTextures(1, &texture_name);
  glBindTexture(GL_TEXTURE_2D, texture_name);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA,
               GL_UNSIGNED_BYTE, pixels);
  glFlush();

  dump_files();

  make_tweaks(argc-optind, argv+optind);

  glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE,
                 tweaked_pixels);

  dump_tweaked_pixels();

  return 0;
}
