/*
 * Copyright 2023 Hovav Shacham.  All rights reserved; see LICENSE file.
 */
#include <assert.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef uint8_t u8;

static u8 in[64];
static u8 in_byte_idx = 0;
static u8 in_bit_idx  = 0;

static u8
read_bits(u8 count)
{
  u8 retval = 0;

  assert(count <= 8);
  assert(in_byte_idx < 64);

  if (count == 0)
    return 0;

  if (in_bit_idx + count < 8) {
    retval = (in[in_byte_idx] >> in_bit_idx) & ((1 << count) - 1);
    in_bit_idx += count;
  } else if (in_bit_idx + count == 8) {
    retval = (in[in_byte_idx] >> in_bit_idx);
    in_byte_idx++; in_bit_idx = 0;
  } else {                      /* in_bit_idx + count > 8 */
    u8 first_byte_bits  = 8 - in_bit_idx;
    u8 second_byte_bits = count - first_byte_bits;

    retval = in[in_byte_idx] >> in_bit_idx;
    in_byte_idx++; in_bit_idx = 0;
    assert(in_byte_idx < 64);

    retval += (in[in_byte_idx]  & ((1 << second_byte_bits) - 1))
                << first_byte_bits;
    in_bit_idx += second_byte_bits;
  }

  return retval;
}

static void
read_and_discard_zero_bits(u8 count)
{
  u8 bits;

  while (count > 8) {
    bits = read_bits(8);
    assert(bits == 0);
    count -= 8;
  }

  bits = read_bits(count);
  assert(bits == 0);
}

struct bitbuffer {
  uint32_t buf;
  u8 bits_used;
};

static void
init_bitbuffer(struct bitbuffer *bb)
{
  assert(bb != NULL);
  bb->buf = 0; bb->bits_used = 0;
}

static void
buffer_bits(struct bitbuffer *bb, u8 val, u8 count)
{
  assert(bb != NULL);
  assert(count <= 8);
  assert(count + bb->bits_used <= 32);

  bb->buf |= ((uint32_t)val) << bb->bits_used;
  bb->bits_used += count;
}

static void
read_bits_into_buffer(struct bitbuffer *bb, u8 count)
{
  assert(bb != NULL);

  while (count > 8) {
    buffer_bits(bb, read_bits(8), 8);
    count -= 8;
  }

  if (count > 0)
    buffer_bits(bb, read_bits(count), count);
}

static u8
read_buffered_bits(struct bitbuffer *bb, u8 count)
{
  u8 retval = 0;

  assert(bb != NULL);
  assert(count <= 8);
  assert(bb->bits_used >= count);

  retval = (u8)(bb->buf & ( (((uint32_t)1) << count) - 1 ));
  bb->buf >>= count;
  bb->bits_used -= count;

  return retval;
}

static void
read_and_discard_buffered_zero_bits(struct bitbuffer *bb, u8 count)
{
  assert(bb != NULL);
  assert(count <= 8);
  assert(bb->bits_used >= count);

  assert(( bb->buf & ( (((uint32_t)1) << count) - 1 ) ) == 0);
  bb->buf >>= count;
  bb->bits_used -= count;
}

static u8 out[128];
static u8 out_idx = 0;

static void
write_pixel(u8 r, u8 g, u8 b, u8 a)
{
  assert(out_idx <= 31);

  out[out_idx*4]   = r;
  out[out_idx*4+1] = g;
  out[out_idx*4+2] = b;
  out[out_idx*4+3] = a;
  out_idx++;
}

static u8 block_order[] =
  {0,  1,  4,  5,  2,  3,  6,  7,
   8,  9,  12, 13, 10, 11, 14, 15,
   16, 17, 20, 21, 18, 19, 22, 23,
   24, 25, 28, 29, 26, 27, 30, 31};

static void
write_pixel_block_order(u8 r, u8 g, u8 b, u8 a)
{
  assert(out_idx <= 31);

  out[block_order[out_idx]*4]   = r;
  out[block_order[out_idx]*4+1] = g;
  out[block_order[out_idx]*4+2] = b;
  out[block_order[out_idx]*4+3] = a;
  out_idx++;
}

static void
decode_8th_gen(void)
{
  u8 skip_r = read_bits(1);
  u8 skip_g = read_bits(1);
  u8 skip_b = read_bits(1);
  u8 skip_a = read_bits(1);
  
  u8 base_r = read_bits(8);
  u8 base_g = read_bits(8);
  u8 base_b = read_bits(8);
  u8 base_a = read_bits(8);

  u8 delta_r_bits = read_bits(3);
  if (skip_r)
    assert(delta_r_bits == 0);
  else
    delta_r_bits++;
  u8 delta_g_bits = read_bits(3);
  if (skip_g)
    assert(delta_g_bits == 0);
  else
    delta_g_bits++;
  u8 delta_b_bits = read_bits(3);
  if (skip_b)
    assert(delta_b_bits == 0);
  else
    delta_b_bits++;
  u8 delta_a_bits = read_bits(3);
  if (skip_a)
    assert(delta_a_bits == 0);
  else
    delta_a_bits++;

  assert(delta_r_bits + delta_g_bits + delta_b_bits + delta_a_bits <= 14);

  u8 unused_bits = 14 - (delta_r_bits + delta_g_bits + delta_b_bits + delta_a_bits);
  
  u8 r, g, b, a;
  for (u8 pixel_idx = 0; pixel_idx < 32; pixel_idx++) {
    r = base_r;
    if (!skip_r)
      r += read_bits(delta_r_bits);
    g = base_g;
    if (!skip_g)
      g += read_bits(delta_g_bits);
    b = base_b;
    if (!skip_b)
      b += read_bits(delta_b_bits);
    a = base_a;
    if (!skip_a)
      a += read_bits(delta_a_bits);
    read_and_discard_zero_bits(unused_bits);

    write_pixel(r, g, b, a);
  }

  read_and_discard_zero_bits(16);
}

static void
decode_11th_gen_extension(u8 inter_pred, u8 extension_bits)
{
  struct bitbuffer ext_bb;
  init_bitbuffer(&ext_bb);
  buffer_bits(&ext_bb, extension_bits, 8);

  u8 subwindow_is_uniform[8];
  u8 num_uniform_subwindows = 0;
  u8 sw;
  for (sw = 0; sw < 8; sw++) {
    subwindow_is_uniform[sw] = read_buffered_bits(&ext_bb, 1);
    if (subwindow_is_uniform[sw])
      num_uniform_subwindows++;
  }
  assert(num_uniform_subwindows == 4); /* can it be more? */

  u8 delta_r_bits;
  u8 delta_g_bits;
  u8 delta_b_bits;
  u8 delta_a_bits;
  u8 unused_bits;

  if (inter_pred) {
    delta_b_bits = read_bits(4);
    delta_r_bits = read_bits(4);
    delta_g_bits = read_bits(4);
  } else {
    delta_r_bits = read_bits(4);
    delta_g_bits = read_bits(4);
    delta_b_bits = read_bits(4);
  }
  assert(delta_r_bits <= 8);
  assert(delta_g_bits <= 8);
  assert(delta_b_bits <= 8);
  assert(delta_r_bits + delta_g_bits + delta_b_bits <= 22);

  /* a bits not specified; limit to 8, discard remainder  */
  delta_a_bits = 22 - (delta_r_bits + delta_g_bits + delta_b_bits);
  if (delta_a_bits > 8) {
    unused_bits = delta_a_bits - 8;
    delta_a_bits = 8;
  } else {
    unused_bits = 0;
  }

  u8 base_r;
  u8 base_g;
  u8 base_b;
  u8 base_a;
  
  if (inter_pred) {
    base_b = read_bits(8);
    base_r = read_bits(8);
    base_g = read_bits(8);
    base_a = read_bits(8);
  } else {
    base_r = read_bits(8);
    base_g = read_bits(8);
    base_b = read_bits(8);
    base_a = read_bits(8);
  }

  struct bitbuffer delta_bb[20];
  u8 delta_bb_idx;
  for (delta_bb_idx = 0; delta_bb_idx < 20; delta_bb_idx++)
    init_bitbuffer(&delta_bb[delta_bb_idx]);

  /* this is the most Intel design of all time */
  for (delta_bb_idx = 0; delta_bb_idx < 20; delta_bb_idx++)
    read_bits_into_buffer(&delta_bb[delta_bb_idx], 14);
  for (delta_bb_idx = 0; delta_bb_idx < 20; delta_bb_idx++)
    read_bits_into_buffer(&delta_bb[delta_bb_idx], 8);
  read_and_discard_zero_bits(19);
  
  u8 r, g, b, a;

  delta_bb_idx = 0;
  for (sw = 0; sw < 8; sw++) {
    if (subwindow_is_uniform[sw]) {

      if (inter_pred) {
        b = base_b;
        b += read_buffered_bits(&delta_bb[delta_bb_idx], delta_b_bits);
        r = base_r + b;
        r += read_buffered_bits(&delta_bb[delta_bb_idx], delta_r_bits);
        g = base_g + (b+r)/2;
        g += read_buffered_bits(&delta_bb[delta_bb_idx], delta_g_bits);
      } else {
        r = base_r;
        r += read_buffered_bits(&delta_bb[delta_bb_idx], delta_r_bits);
        g = base_g;
        g += read_buffered_bits(&delta_bb[delta_bb_idx], delta_g_bits);
        b = base_b;
        b += read_buffered_bits(&delta_bb[delta_bb_idx], delta_b_bits);
      }
      a = base_a;
      a += read_buffered_bits(&delta_bb[delta_bb_idx], delta_a_bits);
      read_and_discard_buffered_zero_bits(&delta_bb[delta_bb_idx], unused_bits);
      delta_bb_idx++;

      /* all 2x2 subwindow pixels are the same */
      write_pixel_block_order(r, g, b, a);
      write_pixel_block_order(r, g, b, a);
      write_pixel_block_order(r, g, b, a);
      write_pixel_block_order(r, g, b, a);

    } else {

      for (u8 px = 0; px < 4; px++) {
        if (inter_pred) {
          b = base_b;
          b += read_buffered_bits(&delta_bb[delta_bb_idx], delta_b_bits);
          r = base_r + b;
          r += read_buffered_bits(&delta_bb[delta_bb_idx], delta_r_bits);
          g = base_g + (b+r)/2;
          g += read_buffered_bits(&delta_bb[delta_bb_idx], delta_g_bits);
        } else {
          r = base_r;
          r += read_buffered_bits(&delta_bb[delta_bb_idx], delta_r_bits);
          g = base_g;
          g += read_buffered_bits(&delta_bb[delta_bb_idx], delta_g_bits);
          b = base_b;
          b += read_buffered_bits(&delta_bb[delta_bb_idx], delta_b_bits);
        }
        a = base_a;
        a += read_buffered_bits(&delta_bb[delta_bb_idx], delta_a_bits);
        read_and_discard_buffered_zero_bits(&delta_bb[delta_bb_idx], unused_bits);
        delta_bb_idx++;

        write_pixel_block_order(r, g, b, a);
      }

    }
  }
}


static void
decode_11th_gen(int ccs)
{
  u8 bits_per_pixel;
  u8 first_cacheline_recovered;
  u8 second_cacheline_recovered;

  switch (ccs) {
  case 1:
    bits_per_pixel = 6;
    first_cacheline_recovered = 1;
    second_cacheline_recovered = 1;
    break;
  case 2:
    bits_per_pixel = 12;
    first_cacheline_recovered = 1;
    second_cacheline_recovered = 0;
    fprintf(stderr,
            "Warning: second cacheline not encoded in compressed payload.\n");
    break;
  case 6:
    bits_per_pixel = 14;
    first_cacheline_recovered = 1;
    second_cacheline_recovered = 1;
    break;
  case 8:
    bits_per_pixel = 12;
    first_cacheline_recovered = 0;
    second_cacheline_recovered = 1;
    fprintf(stderr,
            "Warning: first cacheline not encoded in compressed payload.\n");
    break;
  default:
    fprintf(stderr, "CCS mode %d not (yet) supported.\n", ccs);
    exit(EXIT_FAILURE);
  }

  u8 inter_pred = read_bits(1);

  u8 extension_bits = read_bits(8);
  if (extension_bits != 0) {
    assert(ccs == 6);           /* not observed in other variants */
    decode_11th_gen_extension(inter_pred, extension_bits);
    return;
  }

  u8 delta_r_bits;
  u8 delta_g_bits;
  u8 delta_b_bits;
  u8 delta_a_bits;
  u8 unused_bits;

  if (inter_pred) {
    delta_b_bits = read_bits(4);
    delta_r_bits = read_bits(4);
    delta_g_bits = read_bits(4);
  } else {
    delta_r_bits = read_bits(4);
    delta_g_bits = read_bits(4);
    delta_b_bits = read_bits(4);
  }
  assert(delta_r_bits <= 8);
  assert(delta_g_bits <= 8);
  assert(delta_b_bits <= 8);
  assert(delta_r_bits + delta_g_bits + delta_b_bits <= bits_per_pixel);

  /* a bits not specified; limit to 8, discard remainder  */
  delta_a_bits = bits_per_pixel - (delta_r_bits + delta_g_bits
                                   + delta_b_bits);
  if (delta_a_bits > 8) {
    unused_bits = delta_a_bits - 8;
    delta_a_bits = 8;
  } else {
    unused_bits = 0;
  }

  u8 base_r;
  u8 base_g;
  u8 base_b;
  u8 base_a;
  
  if (inter_pred) {
    base_b = read_bits(8);
    base_r = read_bits(8);
    base_g = read_bits(8);
    base_a = read_bits(8);
  } else {
    base_r = read_bits(8);
    base_g = read_bits(8);
    base_b = read_bits(8);
    base_a = read_bits(8);
  }
  
  u8 r, g, b, a;

  for (u8 pixel_idx = 0; pixel_idx < 32; pixel_idx++) {

    if (pixel_idx < 16 && !first_cacheline_recovered) {
      write_pixel_block_order(0, 0, 0, 0);
      continue;
    }
    if (pixel_idx >= 16 && !second_cacheline_recovered) {
      write_pixel_block_order(0, 0, 0, 0);
      continue;
    }

    if (inter_pred) {
      b = base_b;
      b += read_bits(delta_b_bits);
      r = base_r + b;
      r += read_bits(delta_r_bits);
      g = base_g + (b+r)/2;
      g += read_bits(delta_g_bits);
    } else {
      r = base_r;
      r += read_bits(delta_r_bits);
      g = base_g;
      g += read_bits(delta_g_bits);
      b = base_b;
      b += read_bits(delta_b_bits);
    }
    a = base_a;
    a += read_bits(delta_a_bits);
    read_and_discard_zero_bits(unused_bits);

    write_pixel_block_order(r, g, b, a);
  }

  u8 pixels_recovered = (first_cacheline_recovered  ? 16 : 0)
                      + (second_cacheline_recovered ? 16 : 0);

  read_and_discard_zero_bits(512 - 21 - 32
                             - pixels_recovered * bits_per_pixel);
}

static void
usage(void)
{
  printf("Usage: decode [-t] [-g 8|11] -c [1|2|6|8]\n");
  exit(EXIT_FAILURE);
}

static void
read_text(void)
{
  int rv;
  for (u8 i = 0; i < 64; i++) {
    rv = scanf(" %hhx", &in[i]);
    assert(rv == 1);
  }
}

static void
read_raw()
{
  size_t nb = fread(in, 1, 64, stdin);
  assert(nb == 64);
}

static void
write_text(void)
{
  u8 i;

  printf("First cacheline:\n");
  for (i = 0; i < 64; i++)
    printf("%02X%c", out[i], (i % 16 == 15)? '\n' : ' ');

  printf("\nSecond cacheline:\n");
  for (; i < 128; i++)
    printf("%02X%c", out[i], (i % 16 == 15)? '\n' : ' ');
}

static void
write_raw(void)
{
  size_t nb = fwrite(out, 1, 128, stdout);
  assert(nb == 128);
}

int
main(int argc, char *argv[])
{
  int text_mode = 0;
  int generation = 8;
  int ccs = -1;

  int opt;
  while ( (opt = getopt(argc, argv, "g:c:t")) != -1) {
    switch (opt) {
    case 't':
      text_mode = 1;
      break;
    case 'g':
      generation = atoi(optarg);
      break;
    case 'c':
      ccs = atoi(optarg);
      break;
    default:
      usage();
    }
  }

  if (text_mode)
    read_text();
  else
    read_raw();

  switch (generation) {
  case 8:
    decode_8th_gen();
    break;
  case 11:
    if (ccs == -1)
      usage();
    decode_11th_gen(ccs);
    break;
  default:
    usage();
  }

  if (text_mode)
    write_text();
  else
    write_raw();

  return 0;
}
