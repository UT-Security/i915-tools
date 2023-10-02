/*
 * Copyright (c) 2003, Otto Moerbeek <otto@drijf.net>;
 * modifications copyright 2023 Hovav Shacham.
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include <assert.h>
#include <err.h>
#include <limits.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/random.h>

#include "siphash.h"
#include "minidc.h"

static uint8_t prf_key[PRF_KEYLEN];

struct bmachine {
  word *stack;
  size_t stacksize;
  ssize_t sp;

  unsigned int ibase;
  char *progstr;
  size_t progpos;
  int lastchar;
};

static struct bmachine bmachine;

static void stack_init(void);
static void stack_clear(void);
static size_t stack_size(void);
static void stack_push(word v);
static word stack_pop(void);
static void stack_dup(void);
static void stack_swap(void);
static void stack_rotate(ssize_t amount);

static int readch(void);
static void unreadch(void);
static word readnumber(word base);

static void clear_stack(void);
static void dup(void);
static void swap(void);
static void drop(void);
static void rot(void);

static void get_ibase(void);
static void set_ibase(void);
static void stackdepth(void);
static void badd(void);
static void bsub(void);
static void bmul(void);
static void bdiv(void);
static void bmod(void);
static void bdivmod(void);
static void not(void);
static void or(void);
static void and(void);
static void bitwise_or(void);
static void bitwise_and(void);
static void bitwise_xor(void);
static void bitwise_lshift(void);
static void bitwise_rshift(void);
static void equal_numbers(void);
static void less_numbers(void);
static void lesseq_numbers(void);
static void more_numbers(void);
static void moreeq_numbers(void);
static void nop(void);
static void parse_number(void);
static void prf(void);
static void unknown(void);

typedef void (*opcode_function)(void);

struct jump_entry {
  u_char ch;
  opcode_function f;
};

static opcode_function jump_table[UCHAR_MAX + 1];

static const struct jump_entry jump_table_data[] = {
	{ ' ',	nop		},
     /* { '!',	not_compare	}, */
     /* { '#',	comment		}, */
	{ '$',	prf		},
	{ '%',	bmod		},
        { '&',	bitwise_and	},
	{ '(',	less_numbers	},
	{ ')',	more_numbers	},
	{ '*',	bmul		},
	{ '+',	badd		},
	{ '-',	bsub		},
     /* { '.',	parse_number	}, */
	{ '/',	bdiv		},
	{ '0',	parse_number	},
	{ '1',	parse_number	},
	{ '2',	parse_number	},
	{ '3',	parse_number	},
	{ '4',	parse_number	},
	{ '5',	parse_number	},
	{ '6',	parse_number	},
	{ '7',	parse_number	},
	{ '8',	parse_number	},
	{ '9',	parse_number	},
     /* { ':',	store_array	}, */
     /* { ';',	load_array	}, */
        { '<',	bitwise_lshift	},
     /* { '<',	less		}, */
     /* { '=',	equal		}, */
        { '>',	bitwise_rshift	},
     /* { '>',	greater		}, */
     /* { '?',	eval_line	}, */
	{ 'A',	parse_number	},
	{ 'B',	parse_number	},
	{ 'C',	parse_number	},
	{ 'D',	parse_number	},
	{ 'E',	parse_number	},
	{ 'F',	parse_number	},
	{ 'G',	equal_numbers	},
	{ 'I',	get_ibase	},
     /* { 'J',	skipN		}, */
     /* { 'K',	get_scale	}, */
     /* { 'L',	load_stack	}, */
     /* { 'M',	nop		}, */
        { 'M',  and		},
	{ 'N',	not		},
     /* { 'O',	get_obase	}, */
     /* { 'P',	pop_print	}, */
     /* { 'Q',	quitN		}, */
	{ 'R',	drop		},
     /* { 'S',	store_stack	}, */
     /* { 'X',	push_scale	}, */
     /* { 'Z',	num_digits	}, */
     /* { '[',	push_line	}, */
	{ '\f',	nop		},
	{ '\n',	nop		},
	{ '\r',	nop		},
	{ '\t',	nop		},
        { '^',	bitwise_xor	},
     /* { '^',	bexp		}, */
	{ '_',	parse_number	},
     /* { 'a',	to_ascii	}, */
	{ 'c',	clear_stack	},
	{ 'd',	dup		},
     /* { 'e',	print_err	}, */
     /* { 'f',	print_stack	}, */
	{ 'i',	set_ibase	},
     /* { 'k',	set_scale	}, */
     /* { 'l',	load		}, */
        { 'm',  or		},
     /* { 'n',	pop_printn	}, */
     /* { 'o',	set_obase	}, */
     /* { 'p',	print_tos	}, */
     /* { 'q',	quit		}, */
	{ 'r',	swap		},
     /* { 's',	store		}, */
	{ 't',	rot		},
     /* { 'v',	bsqrt		}, */
     /* { 'x',	eval_tos	}, */
	{ 'z',	stackdepth	},
	{ '{',	lesseq_numbers	},
        { '|',	bitwise_or	},
	{ '}',	moreeq_numbers	},
	{ '~',	bdivmod		}
};

#ifndef nitems
#define nitems(a)	(sizeof((a)) / sizeof((a)[0]))
#endif

void
init_dc(uint8_t *prf_key_val)
{
  int i;

  for (i = 0; i < nitems(jump_table); i++)
    jump_table[i] = unknown;

  for (i = 0; i < nitems(jump_table_data); i++) {
    assert((unsigned int)jump_table_data[i].ch < nitems(jump_table));
    assert(jump_table[jump_table_data[i].ch] == unknown);
    jump_table[jump_table_data[i].ch] = jump_table_data[i].f;
  }

  bmachine.progstr = NULL;
  bmachine.progpos = 0;
  bmachine.lastchar = -1;

  stack_init();
  bmachine.ibase = 10;

  if (prf_key_val != NULL) {
    memcpy(prf_key, prf_key_val, PRF_KEYLEN);
  } else {
    int rv;
    rv = getentropy(prf_key, PRF_KEYLEN);
    assert(rv != -1);
  }
}

void
reset_for_prog(char *prog)
{
  assert(prog != NULL);
  bmachine.progstr = prog;
  bmachine.progpos = 0;
  bmachine.lastchar = -1;

  stack_clear();
  bmachine.ibase = 10;
}

static void
stack_init(void)
{
  bmachine.stacksize = 0;
  bmachine.sp = -1;
  bmachine.stack = NULL;
}

static void
stack_clear(void)
{
  bmachine.sp = -1;
}

static size_t
stack_size(void)
{
  return bmachine.sp + 1;
}

#define MAX_STACK 1048576

static void
stack_push(word v)
{
  if (++bmachine.sp == bmachine.stacksize) {
    size_t new_size = bmachine.stacksize * 2 + 1;
    assert(new_size <= MAX_STACK);
    bmachine.stack = reallocarray(bmachine.stack,
                                   new_size, sizeof(*bmachine.stack));
    bmachine.stacksize = new_size;
  }

  bmachine.stack[bmachine.sp] = v;
}

static word
stack_pop(void)
{
  assert(bmachine.sp != -1);
  return bmachine.stack[bmachine.sp--];
}

static void
stack_dup(void)
{
  assert(bmachine.sp != -1);
  word value = bmachine.stack[bmachine.sp];
  stack_push(value);
}

static void
stack_swap(void)
{
  assert(bmachine.sp >= 1);
  word copy = bmachine.stack[bmachine.sp];
  bmachine.stack[bmachine.sp] = bmachine.stack[bmachine.sp-1];
  bmachine.stack[bmachine.sp-1] = copy;
}

static void
stack_rotate(ssize_t amount)
{
  assert(amount > 1);
  assert(bmachine.sp >= amount-1);
  word copy = bmachine.stack[bmachine.sp-amount+1];
  memmove(&bmachine.stack[bmachine.sp-amount+1], 
          &bmachine.stack[bmachine.sp-amount+2],
          (amount-1) * sizeof(*bmachine.stack));
  bmachine.stack[bmachine.sp] = copy;
}

static int
readch(void)
{
  assert(bmachine.progstr != NULL);
  
  bmachine.lastchar = (unsigned char)bmachine.progstr[bmachine.progpos];
  if (bmachine.lastchar == '\0') {
    return EOF;
  } else {
    bmachine.progpos++;
    return bmachine.lastchar;
  }
}

static void
unreadch(void)
{
  assert(bmachine.progstr != NULL);

  if (bmachine.progpos > 0)
    if (bmachine.lastchar != '\0')
      bmachine.progpos--;
}

static word
readnumber(word base)
{
  word n, v;
  int ch;
  bool sign = false;

  n = 0;
  while ( (ch = readch()) != EOF) {

    if ('0' <= ch && ch <= '9')
      v = ch - '0';
    else if ('A' <= ch && ch <= 'F')
      v = ch - 'A' + 10;
    else if (ch == '_') {
      sign = true;
      continue;
    } else {
      unreadch();
      break;
    }

    __builtin_mul_overflow(n, base, &n);
    __builtin_add_overflow(n, v, &n);
  }
  if (sign)
    __builtin_mul_overflow(n, -1, &n);
  return n;
}

void
push(word v)
{
  stack_push(v);
}

word
pop(void)
{
  return stack_pop();
}

static void
clear_stack(void)
{
  stack_clear();
}

static void
dup(void)
{
  stack_dup();
}

static void
swap(void)
{
  stack_swap();
}

static void
rot(void)
{
  stack_rotate(3);
}

static void
drop(void)
{
  (void)pop();
}

static void
get_ibase(void)
{
  push(bmachine.ibase);
}

static void
set_ibase(void)
{
  word a = pop();
  assert(a >= 2 && a <= 16);
  bmachine.ibase = a;
}

static void
stackdepth(void)
{
  word depth = stack_size();
  push(depth);
}

static void
badd(void)
{
  word a = pop();
  word b = pop();
  word sum;

  __builtin_add_overflow(b, a, &sum);

  push(sum);
}

static void
bsub(void)
{
  word a = pop();
  word b = pop();
  word difference;

  __builtin_sub_overflow(b, a, &difference);

  push(difference);
}

static void
bmul(void)
{
  word a = pop();
  word b = pop();
  word product;

  __builtin_mul_overflow(b, a, &product);

  push(product);
}

static void
bdiv(void)
{
  word a = pop();
  word b = pop();
  assert(a != 0);
  ldiv_t res = ldiv(b, a);
  push(res.quot);
}

static void
bmod(void)
{
  word a = pop();
  word b = pop();
  assert(a != 0);
  ldiv_t res = ldiv(b, a);
  push(res.rem);
}

static void
bdivmod(void)
{
  word a = pop();
  word b = pop();
  assert(a != 0);
  ldiv_t res = ldiv(b, a);
  push(res.quot);
  push(res.rem);
}

static void
not(void)
{
  word a = pop();
  push(!a);
}

static void
or(void)
{
  word a = pop();
  word b = pop();
  push(a || b);
}

static void
and(void)
{
  word a = pop();
  word b = pop();
  push(a && b);
}

static void
bitwise_or(void)
{
  word a = pop();
  word b = pop();
  push(a | b);
}

static void
bitwise_and(void)
{
  word a = pop();
  word b = pop();
  push(a & b);
}

static void
bitwise_xor(void)
{
  word a = pop();
  word b = pop();
  push(a ^ b);
}

static void
bitwise_lshift(void)
{
  word shiftby = pop();
  word val = pop();
  assert(shiftby >= 0 && shiftby <= 63);
  push(val << shiftby);
}

static void
bitwise_rshift(void)
{
  word shiftby = pop();
  word val = pop();
  assert(shiftby >= 0 && shiftby <= 63);
  push(val >> shiftby);
}

static void
equal_numbers(void)
{
  word a = pop();
  word b = pop();
  push(a == b);
}

static void
less_numbers(void)
{
  word a = pop();
  word b = pop();
  push(a < b);
}

static void
lesseq_numbers(void)
{
  word a = pop();
  word b = pop();
  push(a <= b);
}

static void
more_numbers(void)
{
  word a = pop();
  word b = pop();
  push(a > b);
}

static void
moreeq_numbers(void)
{
  word a = pop();
  word b = pop();
  push(a >= b);
}

static void
nop(void)
{
}

static void
parse_number(void)
{
  unreadch();
  word val = readnumber(bmachine.ibase);
  push(val);
}

static void
prf(void)
{
  word range = pop();
  assert(range > 0);

  uint64_t prf_out;
  siphash(bmachine.stack, (bmachine.sp+1)*sizeof(word),
          prf_key, (uint8_t *)&prf_out, 8);

  uint64_t modout = prf_out % (uint64_t)range;
  push((word)modout);
}

static void
unknown(void)
{
  
}

void
eval(void)
{
  int ch;

  for (;;) {
    ch = readch();
    if (ch == EOF)
      return;

    if (0 <= ch && ch < nitems(jump_table))
      (*jump_table[ch])();
    else
      unknown();
  }
}
