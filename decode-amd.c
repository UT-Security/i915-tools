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

#if 0
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
#endif

/* useful but opaquely named builtin: */
#define num_trailing_zero_bits __builtin_ctz

static u8 out[256];
static u8 out_idx = 0;

static void
write_pixel(u8 r, u8 g, u8 b, u8 a)
{
  assert(out_idx <= 63);

  out[out_idx*4]   = r;
  out[out_idx*4+1] = g;
  out[out_idx*4+2] = b;
  out[out_idx*4+3] = a;
  out_idx++;
}

static void
write_g_cr_cb_pixel(u8 g, u8 cr, u8 cb, u8 a)
{
  u8 r = cr + g;
  u8 b = cb + g;
  write_pixel(r, g, b, a);
}

#define NUM_CACHELINES 4
#define NUM_CHANNELS 4

#define CHAN_G  0
#define CHAN_CR 1
#define CHAN_CB 2
#define CHAN_A  3

struct color_channel_info {
  u8 left_header_present;
  u8 left_constant;
  u8 left_base;
  u8 left_bits;

  u8 right_header_present;
  u8 right_constant;
  u8 right_base;
  u8 right_bits;
};

/***************************************************************************************
 *
 * THE GREAT DECODER TABLE
 *
 *  lhp rhp lconst rconst meaning
 *  --- --- ------ ------ -------
 *
 *   0   0    0      0    [not encountered; unknown]
 *   0   0    0      1    left encoded in 7 bits with 1st entry in sign-magnitude;
 *                            right inherits from top right pixel of left
 *   0   0    1      0    [not encountered; unknown]
 *   0   0    1      1    left and right both all 0
 *
 *
 *   0   1    0      0    left encoded in 7 bits with 1st entry in sign-magnitude;
 *                            right encoded in #tz bits of header byte, with left top
 *                            pixel equal to base + 1st entry in absolute value
 *                            [not actually encountered, but conjectured]
 *   0   1    0      1    left encoded in 7 bits with 1st entry in sign-magnitude;
 *                            right is constant, equal to header byte
 *   0   1    1      0    [not encountered; unknown]
 *   0   1    1      1    [not encountered; unknown]
 *
 *
 *   1   0    0      0    left and right each encoded in #tz bits; left top left pixel
 *                            is base + 1st entry in absolute value; right inherits
 *                            from top right pixel of left
 *   1   0    0      1    left encoded in #tz bits; left top left pixel is base + 1st
 *                            entry in absolute value; right is constant, inherits from
 *                            top right pixel of left
 *   1   0    1      0    [not encountered; unknown]
 *   1   0    1      1    left and right both constant, all equal to header byte
 *
 *
 *   1   1    0      0    left encoded in #tz bits of first header byte, with left top
 *                            pixel equal to base + 1st entry in absolute value; right
 *                            encoded in #tz bits of second header byte, with left top
 *                            pixel equal to base + 1st entry in absolute value
 *   1   1    0      1    left encoded in #tz bits of first header byte, with left top
 *                            pixel equal to base + 1st entry in absolute value; right
 *                            is constant, equal to second header byte
 *   1   1    1      0    left is constant, equal to first header byte; right encoded
                              in #tz bits of second header byte, with left top pixel
 *                            equal to base + 1st entry in absolute value
 *   1   1    1      1    left and right both constant; left equal to first header byte,
 *                            right equal to second header byte
 *
 **************************************************************************************/

static void
decode_amd(int dcc)
{
  u8 cachelines_recovered;

  switch (dcc) {
  case 0x28:
    cachelines_recovered = 4;
    break;
  case 0xcc:
    cachelines_recovered = 3;
    fprintf(stderr,
            "Warning: Fourth cacheline not encoded in compressed payload.\n");
    break;
  case 0x66:
    cachelines_recovered = 2;
    fprintf(stderr,
            "Warning: Third and fourth cachelines not encoded in compressed payload.\n");
    break;
  default:
    fprintf(stderr, "DCC mode %x not (yet) supported.\n", dcc);
    exit(EXIT_FAILURE);
  }

  struct color_channel_info chan_info[NUM_CACHELINES][NUM_CHANNELS];
  u8 cl, chan;

  /* first header: 2 bytes per cacheline */
  for (cl = 0; cl < cachelines_recovered; cl++) {
    for (chan = 0; chan < NUM_CHANNELS; chan ++) {
      chan_info[cl][chan].left_header_present = read_bits(1);
      chan_info[cl][chan].right_header_present = read_bits(1);
    }

    for (chan = 0; chan < NUM_CHANNELS; chan ++) {
      chan_info[cl][chan].left_constant = read_bits(1);
      chan_info[cl][chan].right_constant = read_bits(1);
    }
  }

  /* consistency check on first header */
  for (cl = 0; cl < cachelines_recovered; cl++) {
    for (chan = 0; chan < NUM_CHANNELS; chan ++) {
      if (chan_info[cl][chan].left_header_present) {
        if (chan_info[cl][chan].right_header_present) {
          /* all cases handled */
        } else {                /* !rhp */
          /* note: case where left is constant but right is not is not handled. */
          assert(!chan_info[cl][chan].left_constant || chan_info[cl][chan].right_constant);
        }
      } else {                  /* !lhp */
        if (chan_info[cl][chan].right_header_present) {
          /* note: have only seen 01; have good guess for 00; other cases unhandled */
          assert(!chan_info[cl][chan].left_constant);
        } else {                /* !rhp */
          /* note: have only seen 11 and 01, other cases not handled. */
          assert(chan_info[cl][chan].right_constant);
        }
      }
    }
  }

  /* second header: number of bytes depends on first header */
  for (cl = 0; cl < cachelines_recovered; cl++) {
    for (chan = 0; chan < NUM_CHANNELS; chan ++) {

      if (chan_info[cl][chan].left_header_present) {
        u8 left_byte = read_bits(8);
        if (chan_info[cl][chan].left_constant) {
          chan_info[cl][chan].left_base  = left_byte;
          chan_info[cl][chan].left_bits  = 0;
        } else {
          assert (0 != left_byte);
          chan_info[cl][chan].left_base
            = left_byte & ~(1 << num_trailing_zero_bits(left_byte));
          chan_info[cl][chan].left_bits = num_trailing_zero_bits(left_byte);
        }
      } else {
        if (chan_info[cl][chan].left_constant) {
          chan_info[cl][chan].left_base = 0;
          chan_info[cl][chan].left_bits = 0;
        } else {
          chan_info[cl][chan].left_base = 0;
          chan_info[cl][chan].left_bits = 7;
        }
      }

      if (chan_info[cl][chan].right_header_present) {
        u8 right_byte = read_bits(8);
        if (chan_info[cl][chan].right_constant) {
          chan_info[cl][chan].right_base  = right_byte;
          chan_info[cl][chan].right_bits  = 0;
        } else {
          assert (0 != right_byte);
          chan_info[cl][chan].right_base
            = right_byte & ~(1 << num_trailing_zero_bits(right_byte));
          chan_info[cl][chan].right_bits = num_trailing_zero_bits(right_byte);
        }
      } else {
        if (chan_info[cl][chan].right_constant) {
          chan_info[cl][chan].right_base = 0;
          chan_info[cl][chan].right_bits = 0;
        } else {
          chan_info[cl][chan].right_base = chan_info[cl][chan].left_base;
          chan_info[cl][chan].right_bits = chan_info[cl][chan].left_bits;
        }
      }
    }
  }

  u8 upper_pixels[8][NUM_CHANNELS];
  u8 lower_pixels[8][NUM_CHANNELS];
  u8 p;

  for (cl = 0; cl < cachelines_recovered; cl++) {
    for (chan = 0; chan < NUM_CHANNELS; chan++) {
      u8 signs[8];
      u8 deltas[8];
      u8 b;

      /*
       * left side
       */
      if (chan_info[cl][chan].left_constant) {
        for (p = 0; p < 4; p++) {
          upper_pixels[p][chan] = chan_info[cl][chan].left_base;
          lower_pixels[p][chan] = chan_info[cl][chan].left_base;
        }
      } else {
        for (p = 0; p < 8; p++)
          signs[p] = read_bits(1);
        for (p = 0; p < 8; p++)
          deltas[p] = 0;
        for (b = 0; b < chan_info[cl][chan].left_bits; b++)
          for (p = 0; p < 8; p++)
            deltas[p] |= read_bits(1) << b;

        if (chan_info[cl][chan].left_header_present) {
          /* normally, top left pixel is not delta encoded, sign is lsb */
          upper_pixels[0][chan] = chan_info[cl][chan].left_base + (deltas[0] << 1) + signs[0];
        } else {
          /* with no left header byte, top left pixel _is_
             sign-and-magnitude encoded.  why? to mess with my head,
             that's why. */
          upper_pixels[0][chan] = (signs[0] ? 255 - deltas[0] : deltas[0]);
        }
        upper_pixels[1][chan] = upper_pixels[0][chan]  + (signs[1] ? 255 - deltas[1] : deltas[1]);

        lower_pixels[0][chan] = upper_pixels[0][chan]  + (signs[2] ? 255 - deltas[2] : deltas[2]);
        lower_pixels[1][chan] = lower_pixels[0][chan]  + (signs[3] ? 255 - deltas[3] : deltas[3]);

        upper_pixels[2][chan] = upper_pixels[1][chan]  + (signs[4] ? 255 - deltas[4] : deltas[4]);
        upper_pixels[3][chan] = upper_pixels[2][chan]  + (signs[5] ? 255 - deltas[5] : deltas[5]);

        lower_pixels[2][chan] = upper_pixels[2][chan]  + (signs[6] ? 255 - deltas[6] : deltas[6]);
        lower_pixels[3][chan] = lower_pixels[2][chan]  + (signs[7] ? 255 - deltas[7] : deltas[7]);
      }

      /*
       * right side
       */
      if (chan_info[cl][chan].right_constant) {
        if (chan_info[cl][chan].right_header_present) {
          for (p = 4; p < 8; p++) {
            upper_pixels[p][chan] = chan_info[cl][chan].right_base;
            lower_pixels[p][chan] = chan_info[cl][chan].right_base;
          }
        } else {
          /* Inherit from left half upper right pixel.  Note that if
             the left side is itself constant then this pixel's value
             is equal to left_base */
          for (p = 4; p < 8; p++) {
            upper_pixels[p][chan] = upper_pixels[3][chan];
            lower_pixels[p][chan] = upper_pixels[3][chan];
          }
        }
      } else {
        for (p = 0; p < 8; p++)
          signs[p] = read_bits(1);
        for (p = 0; p < 8; p++)
          deltas[p] = 0;
        for (b = 0; b < chan_info[cl][chan].right_bits; b++)
          for (p = 0; p < 8; p++)
            deltas[p] |= read_bits(1) << b;

        if (chan_info[cl][chan].right_header_present) {
          /* second half top left pixel is not delta encoded, sign is lsb */
          upper_pixels[4][chan] = chan_info[cl][chan].right_base + (deltas[0] << 1) + signs[0];
        } else {
          upper_pixels[4][chan] = upper_pixels[3][chan] + (signs[0] ? 255 - deltas[0] : deltas[0]);
        }
        upper_pixels[5][chan] = upper_pixels[4][chan]  + (signs[1] ? 255 - deltas[1] : deltas[1]);

        lower_pixels[4][chan] = upper_pixels[4][chan]  + (signs[2] ? 255 - deltas[2] : deltas[2]);
        lower_pixels[5][chan] = lower_pixels[4][chan]  + (signs[3] ? 255 - deltas[3] : deltas[3]);

        upper_pixels[6][chan] = upper_pixels[5][chan]  + (signs[4] ? 255 - deltas[4] : deltas[4]);
        upper_pixels[7][chan] = upper_pixels[6][chan]  + (signs[5] ? 255 - deltas[5] : deltas[5]);

        lower_pixels[6][chan] = upper_pixels[6][chan]  + (signs[6] ? 255 - deltas[6] : deltas[6]);
        lower_pixels[7][chan] = lower_pixels[6][chan]  + (signs[7] ? 255 - deltas[7] : deltas[7]);
      }
    }

    /* first quadrant */
    for (p = 0; p < 4; p++) {
      write_g_cr_cb_pixel(upper_pixels[p][CHAN_G],  upper_pixels[p][CHAN_CR],
                          upper_pixels[p][CHAN_CB], upper_pixels[p][CHAN_A]);
    }
    /* second quadrant */
    for (p = 0; p < 4; p++) {
      write_g_cr_cb_pixel(lower_pixels[p][CHAN_G],  lower_pixels[p][CHAN_CR],
                          lower_pixels[p][CHAN_CB], lower_pixels[p][CHAN_A]);
    }
    /* third quadrant */
    for (p = 4; p < 8; p++) {
      write_g_cr_cb_pixel(upper_pixels[p][CHAN_G],  upper_pixels[p][CHAN_CR],
                          upper_pixels[p][CHAN_CB], upper_pixels[p][CHAN_A]);
    }
    /* fourth quadrant */
    for (p = 4; p < 8; p++) {
      write_g_cr_cb_pixel(lower_pixels[p][CHAN_G],  lower_pixels[p][CHAN_CR],
                          lower_pixels[p][CHAN_CB], lower_pixels[p][CHAN_A]);
    }
  }
}

static void
usage(void)
{
  printf("Usage: decode-amd [-t] -d [28|66|cc]\n");
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
  int i;

  printf("First cacheline:\n");
  for (i = 0; i < 64; i++)
    printf("%02X%c", out[i], (i % 16 == 15)? '\n' : ' ');

  printf("\nSecond cacheline:\n");
  for (; i < 128; i++)
    printf("%02X%c", out[i], (i % 16 == 15)? '\n' : ' ');

  printf("\nThird cacheline:\n");
  for (; i < 192; i++)
    printf("%02X%c", out[i], (i % 16 == 15)? '\n' : ' ');

  printf("\nFourth cacheline:\n");
  for (; i < 256; i++)
    printf("%02X%c", out[i], (i % 16 == 15)? '\n' : ' ');
}

static void
write_raw(void)
{
  size_t nb = fwrite(out, 1, 256, stdout);
  assert(nb == 256);
}

int
main(int argc, char *argv[])
{
  int text_mode = 0;
  long dcc = -1;

  int opt;
  while ( (opt = getopt(argc, argv, "d:t")) != -1) {
    switch (opt) {
    case 't':
      text_mode = 1;
      break;
    case 'd':
      dcc = strtol(optarg, NULL, 16);
      break;
    default:
      usage();
    }
  }

  if (dcc < 0 || dcc > 255)
    usage();

  if (text_mode)
    read_text();
  else
    read_raw();

  decode_amd(dcc);

  if (text_mode)
    write_text();
  else
    write_raw();

  return 0;
}
