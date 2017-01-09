/*
 * HEVC video decoder
 *
 * Copyright (C) 2012 - 2013 Guillaume Martres
 * Copyright (C) 2013 - 2014 Seppo Tomperi
 *
 * This file is part of FFmpeg.
 *
 * FFmpeg is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * FFmpeg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with FFmpeg; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#include "get_bits.h"
#include "hevc.h"

#include "bit_depth_template.c"
#include "hevcdsp.h"


static void FUNC(put_pcm)(uint8_t *_dst, ptrdiff_t stride, int width, int height,
                          GetBitContext *gb, int pcm_bit_depth)
{
    int x, y;
    pixel *dst = (pixel *)_dst;

    stride /= sizeof(pixel);

    for (y = 0; y < height; y++) {
        for (x = 0; x < width; x++)
            dst[x] = get_bits(gb, pcm_bit_depth) << (BIT_DEPTH - pcm_bit_depth);
        dst += stride;
    }
}

static av_always_inline void FUNC(transquant_bypass)(uint8_t *_dst, int16_t *coeffs,
                                                     ptrdiff_t stride, int size)
{
    int x, y;
    pixel *dst = (pixel *)_dst;

    stride /= sizeof(pixel);

    for (y = 0; y < size; y++) {
        for (x = 0; x < size; x++) {
            dst[x] = av_clip_pixel(dst[x] + *coeffs);
            coeffs++;
        }
        dst += stride;
    }
}

static void FUNC(transform_add4x4)(uint8_t *_dst, int16_t *coeffs,
                                       ptrdiff_t stride)
{
    FUNC(transquant_bypass)(_dst, coeffs, stride, 4);
}

static void FUNC(transform_add8x8)(uint8_t *_dst, int16_t *coeffs,
                                       ptrdiff_t stride)
{
    FUNC(transquant_bypass)(_dst, coeffs, stride, 8);
}

static void FUNC(transform_add16x16)(uint8_t *_dst, int16_t *coeffs,
                                         ptrdiff_t stride)
{
    FUNC(transquant_bypass)(_dst, coeffs, stride, 16);
}

static void FUNC(transform_add32x32)(uint8_t *_dst, int16_t *coeffs,
                                         ptrdiff_t stride)
{
    FUNC(transquant_bypass)(_dst, coeffs, stride, 32);
}


static void FUNC(transform_rdpcm)(int16_t *_coeffs, int16_t log2_size, int mode)
{
    int16_t *coeffs = (int16_t *) _coeffs;
    int x, y;
    int size = 1 << log2_size;

    if (mode) {
        coeffs += size;
        for (y = 0; y < size - 1; y++) {
            for (x = 0; x < size; x++)
                coeffs[x] += coeffs[x - size];
            coeffs += size;
        }
    } else {
        for (y = 0; y < size; y++) {
            for (x = 1; x < size; x++)
                coeffs[x] += coeffs[x - 1];
            coeffs += size;
        }
    }
}

static void FUNC(transform_skip)(int16_t *_coeffs, int16_t log2_size)
{
    int shift  = 15 - BIT_DEPTH - log2_size;
    int x, y;
    int size = 1 << log2_size;
    int16_t *coeffs = _coeffs;


    if (shift > 0) {
        int offset = 1 << (shift - 1);
        for (y = 0; y < size; y++) {
            for (x = 0; x < size; x++) {
                *coeffs = (*coeffs + offset) >> shift;
                coeffs++;
            }
        }
    } else {
        for (y = 0; y < size; y++) {
            for (x = 0; x < size; x++) {
                *coeffs = *coeffs << -shift;
                coeffs++;
            }
        }
    }
}

#define SET(dst, x)   (dst) = (x)
#define SCALE(dst, x) (dst) = av_clip_int16(((x) + add) >> shift)
#define ADD_AND_SCALE(dst, x)                                           \
    (dst) = av_clip_pixel((dst) + av_clip_int16(((x) + add) >> shift))

#define TR_4x4_LUMA(dst, src, step, assign)                             \
    do {                                                                \
        int c0 = src[0 * step] + src[2 * step];                         \
        int c1 = src[2 * step] + src[3 * step];                         \
        int c2 = src[0 * step] - src[3 * step];                         \
        int c3 = 74 * src[1 * step];                                    \
                                                                        \
        assign(dst[2 * step], 74 * (src[0 * step] -                     \
                                    src[2 * step] +                     \
                                    src[3 * step]));                    \
        assign(dst[0 * step], 29 * c0 + 55 * c1 + c3);                  \
        assign(dst[1 * step], 55 * c2 - 29 * c1 + c3);                  \
        assign(dst[3 * step], 55 * c0 + 29 * c2 - c3);                  \
    } while (0)

static void FUNC(transform_4x4_luma)(int16_t *coeffs)
{
    int i;
    int shift    = 7;
    int add      = 1 << (shift - 1);
    int16_t *src = coeffs;

    for (i = 0; i < 4; i++) {
        TR_4x4_LUMA(src, src, 4, SCALE);
        src++;
    }

    shift = 20 - BIT_DEPTH;
    add   = 1 << (shift - 1);
    for (i = 0; i < 4; i++) {
        TR_4x4_LUMA(coeffs, coeffs, 1, SCALE);
        coeffs += 4;
    }
}

#undef TR_4x4_LUMA

////////////////////////////////////////////////////////////////////////////////
//
////////////////////////////////////////////////////////////////////////////////
#define TR_4(dst, src, dstep, sstep, assign, end)                              \
    do {                                                                       \
        const int e0 = 64 * src[0 * sstep] + 64 * src[2 * sstep];              \
        const int e1 = 64 * src[0 * sstep] - 64 * src[2 * sstep];              \
        const int o0 = 83 * src[1 * sstep] + 36 * src[3 * sstep];              \
        const int o1 = 36 * src[1 * sstep] - 83 * src[3 * sstep];              \
                                                                               \
        assign(dst[0 * dstep], e0 + o0);                                       \
        assign(dst[1 * dstep], e1 + o1);                                       \
        assign(dst[2 * dstep], e1 - o1);                                       \
        assign(dst[3 * dstep], e0 - o0);                                       \
    } while (0)

#define TR_8(dst, src, dstep, sstep, assign, end)                              \
    do {                                                                       \
        int i, j;                                                              \
        int e_8[4];                                                            \
        int o_8[4] = { 0 };                                                    \
        for (i = 0; i < 4; i++)                                                \
            for (j = 1; j < end; j += 2)                                       \
                o_8[i] += transform[4 * j][i] * src[j * sstep];                \
        TR_4(e_8, src, 1, 2 * sstep, SET, 4);                                  \
                                                                               \
        for (i = 0; i < 4; i++) {                                              \
            assign(dst[i * dstep], e_8[i] + o_8[i]);                           \
            assign(dst[(7 - i) * dstep], e_8[i] - o_8[i]);                     \
        }                                                                      \
    } while (0)

#define TR_16(dst, src, dstep, sstep, assign, end)                             \
    do {                                                                       \
        int i, j;                                                              \
        int e_16[8];                                                           \
        int o_16[8] = { 0 };                                                   \
        for (i = 0; i < 8; i++)                                                \
            for (j = 1; j < end; j += 2)                                       \
                o_16[i] += transform[2 * j][i] * src[j * sstep];               \
        TR_8(e_16, src, 1, 2 * sstep, SET, 8);                                 \
                                                                               \
        for (i = 0; i < 8; i++) {                                              \
            assign(dst[i * dstep], e_16[i] + o_16[i]);                         \
            assign(dst[(15 - i) * dstep], e_16[i] - o_16[i]);                  \
        }                                                                      \
    } while (0)

#define TR_32(dst, src, dstep, sstep, assign, end)                             \
    do {                                                                       \
        int i, j;                                                              \
        int e_32[16];                                                          \
        int o_32[16] = { 0 };                                                  \
        for (i = 0; i < 16; i++)                                               \
            for (j = 1; j < end; j += 2)                                       \
                o_32[i] += transform[j][i] * src[j * sstep];                   \
        TR_16(e_32, src, 1, 2 * sstep, SET, end/2);                            \
                                                                               \
        for (i = 0; i < 16; i++) {                                             \
            assign(dst[i * dstep], e_32[i] + o_32[i]);                         \
            assign(dst[(31 - i) * dstep], e_32[i] - o_32[i]);                  \
        }                                                                      \
    } while (0)

#define IDCT_VAR4(H)                                                          \
    int      limit2   = FFMIN(col_limit + 4, H)
#define IDCT_VAR8(H)                                                          \
        int      limit   = FFMIN(col_limit, H);                               \
        int      limit2   = FFMIN(col_limit + 4, H)
#define IDCT_VAR16(H)   IDCT_VAR8(H)
#define IDCT_VAR32(H)   IDCT_VAR8(H)

#define IDCT(H)                                                              \
static void FUNC(idct_##H ##x ##H )(                                         \
                   int16_t *coeffs, int col_limit) {                         \
    int i;                                                                   \
    int      shift   = 7;                                                    \
    int      add     = 1 << (shift - 1);                                     \
    int16_t *src     = coeffs;                                               \
    IDCT_VAR ##H(H);                                                         \
                                                                             \
    for (i = 0; i < H; i++) {                                                \
        TR_ ## H(src, src, H, H, SCALE, limit2);                             \
        if (limit2 < H && i%4 == 0 && !!i)                                   \
            limit2 -= 4;                                                     \
        src++;                                                               \
    }                                                                        \
                                                                             \
    shift   = 20 - BIT_DEPTH;                                                \
    add     = 1 << (shift - 1);                                              \
    for (i = 0; i < H; i++) {                                                \
        TR_ ## H(coeffs, coeffs, 1, 1, SCALE, limit);                        \
        coeffs += H;                                                         \
    }                                                                        \
}

#define IDCT_DC(H)                                                           \
static void FUNC(idct_##H ##x ##H ##_dc)(                                    \
                   int16_t *coeffs) {                                        \
    int i, j;                                                                \
    int      shift   = 14 - BIT_DEPTH;                                       \
    int      add     = 1 << (shift - 1);                                     \
    int      coeff   = (((coeffs[0] + 1) >> 1) + add) >> shift;              \
                                                                             \
    for (j = 0; j < H; j++) {                                                \
        for (i = 0; i < H; i++) {                                            \
            coeffs[i+j*H] = coeff;                                           \
        }                                                                    \
    }                                                                        \
}

IDCT( 4)
IDCT( 8)
IDCT(16)
IDCT(32)

IDCT_DC( 4)
IDCT_DC( 8)
IDCT_DC(16)
IDCT_DC(32)

#undef TR_4
#undef TR_8
#undef TR_16
#undef TR_32

#undef SET
#undef SCALE
#undef ADD_AND_SCALE

////////////////////////////////////////////////////////////////////////////////
//
////////////////////////////////////////////////////////////////////////////////
static void FUNC(sao_band_filter_0)(uint8_t *_dst, uint8_t *_src,
                                    ptrdiff_t stride_dst, ptrdiff_t stride_src,
                                    SAOParams *sao,
                                    int *borders, int width, int height,
                                    int c_idx)
{
    pixel *dst = (pixel *)_dst;
    pixel *src = (pixel *)_src;
    int offset_table[32] = { 0 };
    int k, y, x;
    int shift  = BIT_DEPTH - 5;
    int16_t *sao_offset_val = sao->offset_val[c_idx];
    uint8_t sao_left_class  = sao->band_position[c_idx];

    stride_src /= sizeof(pixel);
    stride_dst /= sizeof(pixel);

    for (k = 0; k < 4; k++)
        offset_table[(k + sao_left_class) & 31] = sao_offset_val[k + 1];
    for (y = 0; y < height; y++) {
        for (x = 0; x < width; x++)
            dst[x] = av_clip_pixel(src[x] + offset_table[src[x] >> shift]);
        dst += stride_dst;
        src += stride_src;
    }
}

////////////////////////////////////////////////////////////////////////////////
//
////////////////////////////////////////////////////////////////////////////////
#define CMP(a, b) ((a) > (b) ? 1 : ((a) == (b) ? 0 : -1))

static void FUNC(sao_edge_filter)(uint8_t *_dst, uint8_t *_src,
                                  ptrdiff_t stride_dst, ptrdiff_t stride_src,
                                  SAOParams *sao,
                                  int width, int height,
                                  int c_idx) {
    static const uint8_t edge_idx[] = { 1, 2, 0, 3, 4 };
    static const int8_t pos[4][2][2] = {
        { { -1,  0 }, {  1, 0 } }, // horizontal
        { {  0, -1 }, {  0, 1 } }, // vertical
        { { -1, -1 }, {  1, 1 } }, // 45 degree
        { {  1, -1 }, { -1, 1 } }, // 135 degree
    };
    int16_t *sao_offset_val = sao->offset_val[c_idx];
    uint8_t eo = sao->eo_class[c_idx];
    pixel *dst = (pixel *)_dst;
    pixel *src = (pixel *)_src;

    int a_stride, b_stride;
    int src_offset = 0;
    int dst_offset = 0;
    int x, y;
    stride_src /= sizeof(pixel);
    stride_dst /= sizeof(pixel);

    a_stride = pos[eo][0][0] + pos[eo][0][1] * stride_src;
    b_stride = pos[eo][1][0] + pos[eo][1][1] * stride_src;
    for (y = 0; y < height; y++) {
        for (x = 0; x < width; x++) {
            int diff0         = CMP(src[x + src_offset], src[x + src_offset + a_stride]);
            int diff1         = CMP(src[x + src_offset], src[x + src_offset + b_stride]);
            int offset_val    = edge_idx[2 + diff0 + diff1];
            dst[x + dst_offset] = av_clip_pixel(src[x + src_offset] + sao_offset_val[offset_val]);
        }
        src_offset += stride_src;
        dst_offset += stride_dst;
    }
}

static void FUNC(sao_edge_restore_0)(uint8_t *_dst, uint8_t *_src,
                                    ptrdiff_t stride_dst,  ptrdiff_t stride_src,
                                    SAOParams *sao,
                                    int *borders, int _width, int _height,
                                    int c_idx, uint8_t *vert_edge,
                                    uint8_t *horiz_edge, uint8_t *diag_edge)
{
    int x, y;
    pixel *dst = (pixel *)_dst;
    pixel *src = (pixel *)_src;
    int16_t *sao_offset_val = sao->offset_val[c_idx];
    uint8_t sao_eo_class    = sao->eo_class[c_idx];
    int init_x = 0, /*init_y = 0,*/ width = _width, height = _height;

    stride_src /= sizeof(pixel);
    stride_dst /= sizeof(pixel);

    if (sao_eo_class != SAO_EO_VERT) {
        if (borders[0]) {
            int offset_val = sao_offset_val[0];
            int y_stride_src   = 0;
            int y_stride_dst   = 0;
            for (y = 0; y < height; y++) {
                dst[y_stride_dst] = av_clip_pixel(src[y_stride_src] + offset_val);
                y_stride_src     += stride_src;
                y_stride_dst     += stride_dst;
            }
            init_x = 1;
        }
        if (borders[2]) {
            int offset_val = sao_offset_val[0];
            int x_stride_src   = width - 1;
            int x_stride_dst   = width - 1;
            for (x = 0; x < height; x++) {
                dst[x_stride_dst] = av_clip_pixel(src[x_stride_src] + offset_val);
                x_stride_src     += stride_src;
                x_stride_dst     += stride_dst;
            }
            width--;
        }
    }
    if (sao_eo_class != SAO_EO_HORIZ) {
        if (borders[1]) {
            int offset_val = sao_offset_val[0];
            for (x = init_x; x < width; x++)
                dst[x] = av_clip_pixel(src[x] + offset_val);
        }
        if (borders[3]) {
            int offset_val = sao_offset_val[0];
            int y_stride_src   = stride_src * (height - 1);
            int y_stride_dst   = stride_dst * (height - 1);
            for (x = init_x; x < width; x++)
                dst[x + y_stride_dst] = av_clip_pixel(src[x + y_stride_src] + offset_val);
            height--;
        }
    }
}

static void FUNC(sao_edge_restore_1)(uint8_t *_dst, uint8_t *_src,
                                    ptrdiff_t stride_dst, ptrdiff_t stride_src, 
                                    SAOParams *sao,
                                    int *borders, int _width, int _height,
                                    int c_idx, uint8_t *vert_edge,
                                    uint8_t *horiz_edge, uint8_t *diag_edge)
{
    int x, y;
    pixel *dst = (pixel *)_dst;
    pixel *src = (pixel *)_src;
    int16_t *sao_offset_val = sao->offset_val[c_idx];
    uint8_t sao_eo_class    = sao->eo_class[c_idx];
    int init_x = 0, init_y = 0, width = _width, height = _height;

    stride_src /= sizeof(pixel);
    stride_dst /= sizeof(pixel);

    if (sao_eo_class != SAO_EO_VERT) {
        if (borders[0]) {
            int offset_val = sao_offset_val[0];
            int y_stride_src   = 0;
            int y_stride_dst   = 0;
            for (y = 0; y < height; y++) {
                dst[y_stride_dst] = av_clip_pixel(src[y_stride_src] + offset_val);
                y_stride_src     += stride_src;
                y_stride_dst     += stride_dst;
            }
            init_x = 1;
        }
        if (borders[2]) {
            int offset_val = sao_offset_val[0];
            int x_stride_src   = width - 1;
            int x_stride_dst   = width - 1;
            for (x = 0; x < height; x++) {
                dst[x_stride_dst] = av_clip_pixel(src[x_stride_src] + offset_val);
                x_stride_src     += stride_src;
                x_stride_dst     += stride_dst;
            }
            width--;
        }
    }
    if (sao_eo_class != SAO_EO_HORIZ) {
        if (borders[1]) {
            int offset_val = sao_offset_val[0];
            for (x = init_x; x < width; x++)
                dst[x] = av_clip_pixel(src[x] + offset_val);
        }
        if (borders[3]) {
            int offset_val = sao_offset_val[0];
            int y_stride_src   = stride_src * (height - 1);
            int y_stride_dst   = stride_dst * (height - 1);
            for (x = init_x; x < width; x++)
                dst[x + y_stride_dst] = av_clip_pixel(src[x + y_stride_src] + offset_val);
            height--;
        }
    }

    {
        int save_upper_left  = !diag_edge[0] && sao_eo_class == SAO_EO_135D && !borders[0] && !borders[1];
        int save_upper_right = !diag_edge[1] && sao_eo_class == SAO_EO_45D  && !borders[1] && !borders[2];
        int save_lower_right = !diag_edge[2] && sao_eo_class == SAO_EO_135D && !borders[2] && !borders[3];
        int save_lower_left  = !diag_edge[3] && sao_eo_class == SAO_EO_45D  && !borders[0] && !borders[3];

        // Restore pixels that can't be modified
        if(vert_edge[0] && sao_eo_class != SAO_EO_VERT) {
            for(y = init_y+save_upper_left; y< height-save_lower_left; y++)
                dst[y*stride_dst] = src[y*stride_src];
        }
        if(vert_edge[1] && sao_eo_class != SAO_EO_VERT) {
            for(y = init_y+save_upper_right; y< height-save_lower_right; y++)
                dst[y*stride_dst+width-1] = src[y*stride_src+width-1];
        }

        if(horiz_edge[0] && sao_eo_class != SAO_EO_HORIZ) {
            for(x = init_x+save_upper_left; x < width-save_upper_right; x++)
                dst[x] = src[x];
        }
        if(horiz_edge[1] && sao_eo_class != SAO_EO_HORIZ) {
            for(x = init_x+save_lower_left; x < width-save_lower_right; x++)
                dst[(height-1)*stride_dst+x] = src[(height-1)*stride_src+x];
        }
        if(diag_edge[0] && sao_eo_class == SAO_EO_135D)
            dst[0] = src[0];
        if(diag_edge[1] && sao_eo_class == SAO_EO_45D)
            dst[width-1] = src[width-1];
        if(diag_edge[2] && sao_eo_class == SAO_EO_135D)
            dst[stride_dst*(height-1)+width-1] = src[stride_src*(height-1)+width-1];
        if(diag_edge[3] && sao_eo_class == SAO_EO_45D)
            dst[stride_dst*(height-1)] = src[stride_src*(height-1)];

    }
}

#undef CMP

#if COM16_C806_EMT


#define DEFINE_DST4x4_MATRIX(a,b,c,d) \
{ \
  {  a,  b,  c,  d }, \
  {  c,  c,  0, -c }, \
  {  d, -a, -c,  b }, \
  {  b, -d,  c, -a }, \
}

#define DEFINE_DCT4x4_MATRIX(a,b,c) \
{ \
  { a,  a,  a,  a}, \
  { b,  c, -c, -b}, \
  { a, -a, -a,  a}, \
  { c, -b,  b, -c}  \
}

#define DEFINE_DCT8x8_MATRIX(a,b,c,d,e,f,g) \
{ \
  { a,  a,  a,  a,  a,  a,  a,  a}, \
  { d,  e,  f,  g, -g, -f, -e, -d}, \
  { b,  c, -c, -b, -b, -c,  c,  b}, \
  { e, -g, -d, -f,  f,  d,  g, -e}, \
  { a, -a, -a,  a,  a, -a, -a,  a}, \
  { f, -d,  g,  e, -e, -g,  d, -f}, \
  { c, -b,  b, -c, -c,  b, -b,  c}, \
  { g, -f,  e, -d,  d, -e,  f, -g}  \
}

#define DEFINE_DCT16x16_MATRIX(a,b,c,d,e,f,g,h,i,j,k,l,m,n,o) \
{ \
  { a,  a,  a,  a,  a,  a,  a,  a,  a,  a,  a,  a,  a,  a,  a,  a}, \
  { h,  i,  j,  k,  l,  m,  n,  o, -o, -n, -m, -l, -k, -j, -i, -h}, \
  { d,  e,  f,  g, -g, -f, -e, -d, -d, -e, -f, -g,  g,  f,  e,  d}, \
  { i,  l,  o, -m, -j, -h, -k, -n,  n,  k,  h,  j,  m, -o, -l, -i}, \
  { b,  c, -c, -b, -b, -c,  c,  b,  b,  c, -c, -b, -b, -c,  c,  b}, \
  { j,  o, -k, -i, -n,  l,  h,  m, -m, -h, -l,  n,  i,  k, -o, -j}, \
  { e, -g, -d, -f,  f,  d,  g, -e, -e,  g,  d,  f, -f, -d, -g,  e}, \
  { k, -m, -i,  o,  h,  n, -j, -l,  l,  j, -n, -h, -o,  i,  m, -k}, \
  { a, -a, -a,  a,  a, -a, -a,  a,  a, -a, -a,  a,  a, -a, -a,  a}, \
  { l, -j, -n,  h, -o, -i,  m,  k, -k, -m,  i,  o, -h,  n,  j, -l}, \
  { f, -d,  g,  e, -e, -g,  d, -f, -f,  d, -g, -e,  e,  g, -d,  f}, \
  { m, -h,  l,  n, -i,  k,  o, -j,  j, -o, -k,  i, -n, -l,  h, -m}, \
  { c, -b,  b, -c, -c,  b, -b,  c,  c, -b,  b, -c, -c,  b, -b,  c}, \
  { n, -k,  h, -j,  m,  o, -l,  i, -i,  l, -o, -m,  j, -h,  k, -n}, \
  { g, -f,  e, -d,  d, -e,  f, -g, -g,  f, -e,  d, -d,  e, -f,  g}, \
  { o, -n,  m, -l,  k, -j,  i, -h,  h, -i,  j, -k,  l, -m,  n, -o}  \
}

#define DEFINE_DCT32x32_MATRIX(a,b,c,d,e,f,g,h,i,j,k,l,m,n,o,p,q,r,s,t,u,v,w,x,y,z,A,B,C,D,E) \
{ \
  { a,  a,  a,  a,  a,  a,  a,  a,  a,  a,  a,  a,  a,  a,  a,  a,  a,  a,  a,  a,  a,  a,  a,  a,  a,  a,  a,  a,  a,  a,  a,  a}, \
  { p,  q,  r,  s,  t,  u,  v,  w,  x,  y,  z,  A,  B,  C,  D,  E, -E, -D, -C, -B, -A, -z, -y, -x, -w, -v, -u, -t, -s, -r, -q, -p}, \
  { h,  i,  j,  k,  l,  m,  n,  o, -o, -n, -m, -l, -k, -j, -i, -h, -h, -i, -j, -k, -l, -m, -n, -o,  o,  n,  m,  l,  k,  j,  i,  h}, \
  { q,  t,  w,  z,  C, -E, -B, -y, -v, -s, -p, -r, -u, -x, -A, -D,  D,  A,  x,  u,  r,  p,  s,  v,  y,  B,  E, -C, -z, -w, -t, -q}, \
  { d,  e,  f,  g, -g, -f, -e, -d, -d, -e, -f, -g,  g,  f,  e,  d,  d,  e,  f,  g, -g, -f, -e, -d, -d, -e, -f, -g,  g,  f,  e,  d}, \
  { r,  w,  B, -D, -y, -t, -p, -u, -z, -E,  A,  v,  q,  s,  x,  C, -C, -x, -s, -q, -v, -A,  E,  z,  u,  p,  t,  y,  D, -B, -w, -r}, \
  { i,  l,  o, -m, -j, -h, -k, -n,  n,  k,  h,  j,  m, -o, -l, -i, -i, -l, -o,  m,  j,  h,  k,  n, -n, -k, -h, -j, -m,  o,  l,  i}, \
  { s,  z, -D, -w, -p, -v, -C,  A,  t,  r,  y, -E, -x, -q, -u, -B,  B,  u,  q,  x,  E, -y, -r, -t, -A,  C,  v,  p,  w,  D, -z, -s}, \
  { b,  c, -c, -b, -b, -c,  c,  b,  b,  c, -c, -b, -b, -c,  c,  b,  b,  c, -c, -b, -b, -c,  c,  b,  b,  c, -c, -b, -b, -c,  c,  b}, \
  { t,  C, -y, -p, -x,  D,  u,  s,  B, -z, -q, -w,  E,  v,  r,  A, -A, -r, -v, -E,  w,  q,  z, -B, -s, -u, -D,  x,  p,  y, -C, -t}, \
  { j,  o, -k, -i, -n,  l,  h,  m, -m, -h, -l,  n,  i,  k, -o, -j, -j, -o,  k,  i,  n, -l, -h, -m,  m,  h,  l, -n, -i, -k,  o,  j}, \
  { u, -E, -t, -v,  D,  s,  w, -C, -r, -x,  B,  q,  y, -A, -p, -z,  z,  p,  A, -y, -q, -B,  x,  r,  C, -w, -s, -D,  v,  t,  E, -u}, \
  { e, -g, -d, -f,  f,  d,  g, -e, -e,  g,  d,  f, -f, -d, -g,  e,  e, -g, -d, -f,  f,  d,  g, -e, -e,  g,  d,  f, -f, -d, -g,  e}, \
  { v, -B, -p, -C,  u,  w, -A, -q, -D,  t,  x, -z, -r, -E,  s,  y, -y, -s,  E,  r,  z, -x, -t,  D,  q,  A, -w, -u,  C,  p,  B, -v}, \
  { k, -m, -i,  o,  h,  n, -j, -l,  l,  j, -n, -h, -o,  i,  m, -k, -k,  m,  i, -o, -h, -n,  j,  l, -l, -j,  n,  h,  o, -i, -m,  k}, \
  { w, -y, -u,  A,  s, -C, -q,  E,  p,  D, -r, -B,  t,  z, -v, -x,  x,  v, -z, -t,  B,  r, -D, -p, -E,  q,  C, -s, -A,  u,  y, -w}, \
  { a, -a, -a,  a,  a, -a, -a,  a,  a, -a, -a,  a,  a, -a, -a,  a,  a, -a, -a,  a,  a, -a, -a,  a,  a, -a, -a,  a,  a, -a, -a,  a}, \
  { x, -v, -z,  t,  B, -r, -D,  p, -E, -q,  C,  s, -A, -u,  y,  w, -w, -y,  u,  A, -s, -C,  q,  E, -p,  D,  r, -B, -t,  z,  v, -x}, \
  { l, -j, -n,  h, -o, -i,  m,  k, -k, -m,  i,  o, -h,  n,  j, -l, -l,  j,  n, -h,  o,  i, -m, -k,  k,  m, -i, -o,  h, -n, -j,  l}, \
  { y, -s, -E,  r, -z, -x,  t,  D, -q,  A,  w, -u, -C,  p, -B, -v,  v,  B, -p,  C,  u, -w, -A,  q, -D, -t,  x,  z, -r,  E,  s, -y}, \
  { f, -d,  g,  e, -e, -g,  d, -f, -f,  d, -g, -e,  e,  g, -d,  f,  f, -d,  g,  e, -e, -g,  d, -f, -f,  d, -g, -e,  e,  g, -d,  f}, \
  { z, -p,  A,  y, -q,  B,  x, -r,  C,  w, -s,  D,  v, -t,  E,  u, -u, -E,  t, -v, -D,  s, -w, -C,  r, -x, -B,  q, -y, -A,  p, -z}, \
  { m, -h,  l,  n, -i,  k,  o, -j,  j, -o, -k,  i, -n, -l,  h, -m, -m,  h, -l, -n,  i, -k, -o,  j, -j,  o,  k, -i,  n,  l, -h,  m}, \
  { A, -r,  v, -E, -w,  q, -z, -B,  s, -u,  D,  x, -p,  y,  C, -t,  t, -C, -y,  p, -x, -D,  u, -s,  B,  z, -q,  w,  E, -v,  r, -A}, \
  { c, -b,  b, -c, -c,  b, -b,  c,  c, -b,  b, -c, -c,  b, -b,  c,  c, -b,  b, -c, -c,  b, -b,  c,  c, -b,  b, -c, -c,  b, -b,  c}, \
  { B, -u,  q, -x,  E,  y, -r,  t, -A, -C,  v, -p,  w, -D, -z,  s, -s,  z,  D, -w,  p, -v,  C,  A, -t,  r, -y, -E,  x, -q,  u, -B}, \
  { n, -k,  h, -j,  m,  o, -l,  i, -i,  l, -o, -m,  j, -h,  k, -n, -n,  k, -h,  j, -m, -o,  l, -i,  i, -l,  o,  m, -j,  h, -k,  n}, \
  { C, -x,  s, -q,  v, -A, -E,  z, -u,  p, -t,  y, -D, -B,  w, -r,  r, -w,  B,  D, -y,  t, -p,  u, -z,  E,  A, -v,  q, -s,  x, -C}, \
  { g, -f,  e, -d,  d, -e,  f, -g, -g,  f, -e,  d, -d,  e, -f,  g,  g, -f,  e, -d,  d, -e,  f, -g, -g,  f, -e,  d, -d,  e, -f,  g}, \
  { D, -A,  x, -u,  r, -p,  s, -v,  y, -B,  E,  C, -z,  w, -t,  q, -q,  t, -w,  z, -C, -E,  B, -y,  v, -s,  p, -r,  u, -x,  A, -D}, \
  { o, -n,  m, -l,  k, -j,  i, -h,  h, -i,  j, -k,  l, -m,  n, -o, -o,  n, -m,  l, -k,  j, -i,  h, -h,  i, -j,  k, -l,  m, -n,  o}, \
  { E, -D,  C, -B,  A, -z,  y, -x,  w, -v,  u, -t,  s, -r,  q, -p,  p, -q,  r, -s,  t, -u,  v, -w,  x, -y,  z, -A,  B, -C,  D, -E}  \
}

//Declare only once
#if BIT_DEPTH<9
const  int16_t g_aiT4[TRANSFORM_NUMBER_OF_DIRECTIONS][4][4]   =
{
  DEFINE_DCT4x4_MATRIX  (   64,    83,    36),
  DEFINE_DCT4x4_MATRIX  (   64,    83,    36)
};

const  int16_t g_aiT8[TRANSFORM_NUMBER_OF_DIRECTIONS][8][8]   =
{
  DEFINE_DCT8x8_MATRIX  (   64,    83,    36,    89,    75,    50,    18),
  DEFINE_DCT8x8_MATRIX  (   64,    83,    36,    89,    75,    50,    18)
};

const int16_t g_aiT16[TRANSFORM_NUMBER_OF_DIRECTIONS][16][16] =
{
  DEFINE_DCT16x16_MATRIX(   64,    83,    36,    89,    75,    50,    18,    90,    87,    80,    70,    57,    43,    25,     9),
  DEFINE_DCT16x16_MATRIX(   64,    83,    36,    89,    75,    50,    18,    90,    87,    80,    70,    57,    43,    25,     9)
};

const  int16_t g_aiT32[TRANSFORM_NUMBER_OF_DIRECTIONS][32][32] =
{
  DEFINE_DCT32x32_MATRIX(   64,    83,    36,    89,    75,    50,    18,    90,    87,    80,    70,    57,    43,    25,     9,    90,    90,    88,    85,    82,    78,    73,    67,    61,    54,    46,    38,    31,    22,    13,     4),
  DEFINE_DCT32x32_MATRIX(   64,    83,    36,    89,    75,    50,    18,    90,    87,    80,    70,    57,    43,    25,     9,    90,    90,    88,    85,    82,    78,    73,    67,    61,    54,    46,    38,    31,    22,    13,     4)
};

const  int16_t g_as_DST_MAT_4[TRANSFORM_NUMBER_OF_DIRECTIONS][4][4] =
{
  DEFINE_DST4x4_MATRIX(   29,    55,    74,    84),
  DEFINE_DST4x4_MATRIX(   29,    55,    74,    84)
};

const int emt_Tr_Set_V[35] =
{
    2, 1, 0, 1, 0, 1, 0, 1, 0, 0, 0, 0, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 2, 2, 2, 2, 2, 1, 0, 1, 0, 1, 0
};

const int emt_Tr_Set_H[35] =
{
    2, 1, 0, 1, 0, 1, 0, 1, 2, 2, 2, 2, 2, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 0, 0, 0, 0, 1, 0, 1, 0, 1, 0
};
#endif
/*
 * Fast inverse DCT2 4-8-16-32-64
 */
static void FUNC(fastInverseDCT2_B4)(int16_t *src, int16_t *dst, int shift, int line, int zo, int use, const int outputMinimum, const int outputMaximum)
{
    int j;
    int E[2],O[2];
    int add = ( 1<<(shift-1) );

#if COM16_C806_EMT
    const int16_t *iT = use ? g_aiTr4[DCT_II][0] : g_aiT4[0][0];
#else
    const uint16_t *iT = g_aiT4[0][0];
#endif

    for (j=0; j<line; j++)
    {
        O[0] = iT[1*4+0]*src[line] + iT[3*4+0]*src[3*line];
        O[1] = iT[1*4+1]*src[line] + iT[3*4+1]*src[3*line];
        E[0] = iT[0*4+0]*src[0] + iT[2*4+0]*src[2*line];
        E[1] = iT[0*4+1]*src[0] + iT[2*4+1]*src[2*line];

        dst[0] = av_clip(((E[0] + O[0] + add)>>shift), outputMinimum, outputMaximum);
        dst[1] = av_clip(((E[1] + O[1] + add)>>shift), outputMinimum, outputMaximum);
        dst[2] = av_clip(((E[1] - O[1] + add)>>shift), outputMinimum, outputMaximum);
        dst[3] = av_clip(((E[0] - O[0] + add)>>shift), outputMinimum, outputMaximum);

        src   ++;
        dst += 4;
    }
}

static void FUNC(fastInverseDCT2_B8)(int16_t *src, int16_t *dst, int shift, int line, int zo, int use, const int outputMinimum, const int outputMaximum)
{
    int j,k;
    int E[4],O[4];
    int EE[2],EO[2];
    int add = ( 1<<(shift-1) );

#if COM16_C806_EMT
    const int16_t *iT = use ? g_aiTr8[DCT_II][0] : g_aiT8[0][0];
#else
    const uint16_t *iT = g_aiT8[0][0];
#endif

    for (j=0; j<line; j++)
    {
        for (k=0;k<4;k++)
        {
            O[k] = iT[ 1*8+k]*src[line] + iT[ 3*8+k]*src[3*line] + iT[ 5*8+k]*src[5*line] + iT[ 7*8+k]*src[7*line];
        }

        EO[0] = iT[2*8+0]*src[ 2*line ] + iT[6*8+0]*src[ 6*line ];
        EO[1] = iT[2*8+1]*src[ 2*line ] + iT[6*8+1]*src[ 6*line ];
        EE[0] = iT[0*8+0]*src[ 0      ] + iT[4*8+0]*src[ 4*line ];
        EE[1] = iT[0*8+1]*src[ 0      ] + iT[4*8+1]*src[ 4*line ];

        E[0] = EE[0] + EO[0];
        E[3] = EE[0] - EO[0];
        E[1] = EE[1] + EO[1];
        E[2] = EE[1] - EO[1];
        for (k=0;k<4;k++)
        {
            dst[k] = av_clip( ((E[k] + O[k] + add)>>shift), outputMinimum, outputMaximum);
            dst[k+4] = av_clip( ((E[3-k] - O[3-k] + add)>>shift), outputMinimum, outputMaximum);
        }
        src ++;
        dst += 8;
    }
}

static void FUNC(fastInverseDCT2_B16)(int16_t *src, int16_t *dst, int shift, int line, int zo, int use, const int outputMinimum, const int outputMaximum)
{
    int j,k;
    int E[8],O[8];
    int EE[4],EO[4];
    int EEE[2],EEO[2];
    int add = ( 1<<(shift-1) );

#if COM16_C806_EMT
    const int16_t *iT = use ? g_aiTr16[DCT_II][0] : g_aiT16[0][0];
#else
    const uint16_t *iT = g_aiT16[0][0];
#endif

    for (j=0; j<line; j++)
    {
        for (k=0;k<8;k++)
        {
            O[k] = iT[ 1*16+k]*src[ line] + iT[ 3*16+k]*src[ 3*line] + iT[ 5*16+k]*src[ 5*line] + iT[ 7*16+k]*src[ 7*line] + iT[ 9*16+k]*src[ 9*line] + iT[11*16+k]*src[11*line] + iT[13*16+k]*src[13*line] + iT[15*16+k]*src[15*line];
        }
        for (k=0;k<4;k++)
        {
            EO[k] = iT[ 2*16+k]*src[ 2*line] + iT[ 6*16+k]*src[ 6*line] + iT[10*16+k]*src[10*line] + iT[14*16+k]*src[14*line];
        }
        EEO[0] = iT[4*16]*src[ 4*line ] + iT[12*16]*src[ 12*line ];
        EEE[0] = iT[0]*src[ 0 ] + iT[ 8*16]*src[ 8*line ];
        EEO[1] = iT[4*16+1]*src[ 4*line ] + iT[12*16+1]*src[ 12*line ];
        EEE[1] = iT[0*16+1]*src[ 0 ] + iT[ 8*16+1]*src[ 8*line  ];

        for (k=0;k<2;k++)
        {
            EE[k] = EEE[k] + EEO[k];
            EE[k+2] = EEE[1-k] - EEO[1-k];
        }
        for (k=0;k<4;k++)
        {
            E[k] = EE[k] + EO[k];
            E[k+4] = EE[3-k] - EO[3-k];
        }
        for (k=0;k<8;k++)
        {
            dst[k] = av_clip( ((E[k] + O[k] + add)>>shift), outputMinimum, outputMaximum);
            dst[k+8] = av_clip( ((E[7-k] - O[7-k] + add)>>shift), outputMinimum, outputMaximum);
        }
        src ++;
        dst += 16;
    }
}

static void FUNC(fastInverseDCT2_B32)(int16_t *src, int16_t *dst, int shift, int line, int zo, int use, const int outputMinimum, const int outputMaximum)
{
    int j,k;
    int E[16],O[16];
    int EE[8],EO[8];
    int EEE[4],EEO[4];
    int EEEE[2],EEEO[2];
    int add = ( 1<<(shift-1) );

#if COM16_C806_EMT
    const int16_t *iT = use ? g_aiTr32[DCT_II][0] : g_aiT32[0][0];
#else
    const uint16_t *iT = g_aiT32[0][0];
#endif

    for (j=0; j<line; j++)
    {
        for (k=0;k<16;k++)
        {
            O[k] = iT[ 1*32+k]*src[ line  ] + iT[ 3*32+k]*src[ 3*line  ] + iT[ 5*32+k]*src[ 5*line  ] + iT[ 7*32+k]*src[ 7*line  ] +
                    iT[ 9*32+k]*src[ 9*line  ] + iT[11*32+k]*src[ 11*line ] + iT[13*32+k]*src[ 13*line ] + iT[15*32+k]*src[ 15*line ] +
                    iT[17*32+k]*src[ 17*line ] + iT[19*32+k]*src[ 19*line ] + iT[21*32+k]*src[ 21*line ] + iT[23*32+k]*src[ 23*line ] +
                    iT[25*32+k]*src[ 25*line ] + iT[27*32+k]*src[ 27*line ] + iT[29*32+k]*src[ 29*line ] + iT[31*32+k]*src[ 31*line ];
        }
        for (k=0;k<8;k++)
        {
            EO[k] = iT[ 2*32+k]*src[ 2*line  ] + iT[ 6*32+k]*src[ 6*line  ] + iT[10*32+k]*src[ 10*line ] + iT[14*32+k]*src[ 14*line ] +
                    iT[18*32+k]*src[ 18*line ] + iT[22*32+k]*src[ 22*line ] + iT[26*32+k]*src[ 26*line ] + iT[30*32+k]*src[ 30*line ];
        }
        for (k=0;k<4;k++)
        {
            EEO[k] = iT[4*32+k]*src[ 4*line ] + iT[12*32+k]*src[ 12*line ] + iT[20*32+k]*src[ 20*line ] + iT[28*32+k]*src[ 28*line ];
        }
        EEEO[0] = iT[8*32+0]*src[ 8*line ] + iT[24*32+0]*src[ 24*line ];
        EEEO[1] = iT[8*32+1]*src[ 8*line ] + iT[24*32+1]*src[ 24*line ];
        EEEE[0] = iT[0*32+0]*src[ 0      ] + iT[16*32+0]*src[ 16*line ];
        EEEE[1] = iT[0*32+1]*src[ 0      ] + iT[16*32+1]*src[ 16*line ];

        EEE[0] = EEEE[0] + EEEO[0];
        EEE[3] = EEEE[0] - EEEO[0];
        EEE[1] = EEEE[1] + EEEO[1];
        EEE[2] = EEEE[1] - EEEO[1];

        for (k=0;k<4;k++)
        {
            EE[k] = EEE[k] + EEO[k];
            EE[k+4] = EEE[3-k] - EEO[3-k];
        }
        for (k=0;k<8;k++)
        {
            E[k] = EE[k] + EO[k];
            E[k+8] = EE[7-k] - EO[7-k];
        }
        for (k=0;k<16;k++)
        {
            dst[k] = av_clip( ((E[k] + O[k] + add)>>shift), outputMinimum, outputMaximum);
            dst[k+16] = av_clip( ((E[15-k] - O[15-k] + add)>>shift), outputMinimum, outputMaximum);
        }
        src ++;
        dst += 32;
    }
}

static void FUNC(fastInverseDCT2_B64)(int16_t *coeff, int16_t *block, int shift, int line, int zo, int use, const int outputMinimum, const int outputMaximum)
{
    int rnd_factor = ( 1<<(shift-1) );
    const int uiTrSize = 64;
    const int16_t *iT = NULL;
    //av_assert2(0);

    int j, k;
    int E[32],O[32];
    int EE[16],EO[16];
    int EEE[8],EEO[8];
    int EEEE[4],EEEO[4];
    int EEEEE[2],EEEEO[2];
    for (j=0; j<(line>>(2==zo?1:0)); j++)
    {
        for (k=0;k<32;k++)
        {
            O[k] = iT[ 1*64+k]*coeff[ line  ] + iT[ 3*64+k]*coeff[ 3*line  ] + iT[ 5*64+k]*coeff[ 5*line  ] + iT[ 7*64+k]*coeff[ 7*line  ] +
                    iT[ 9*64+k]*coeff[ 9*line  ] + iT[11*64+k]*coeff[ 11*line ] + iT[13*64+k]*coeff[ 13*line ] + iT[15*64+k]*coeff[ 15*line ] +
                    iT[17*64+k]*coeff[ 17*line ] + iT[19*64+k]*coeff[ 19*line ] + iT[21*64+k]*coeff[ 21*line ] + iT[23*64+k]*coeff[ 23*line ] +
                    iT[25*64+k]*coeff[ 25*line ] + iT[27*64+k]*coeff[ 27*line ] + iT[29*64+k]*coeff[ 29*line ] + iT[31*64+k]*coeff[ 31*line ] +
                    ( zo ? 0 : (
                               iT[33*64+k]*coeff[ 33*line ] + iT[35*64+k]*coeff[ 35*line ] + iT[37*64+k]*coeff[ 37*line ] + iT[39*64+k]*coeff[ 39*line ] +
                    iT[41*64+k]*coeff[ 41*line ] + iT[43*64+k]*coeff[ 43*line ] + iT[45*64+k]*coeff[ 45*line ] + iT[47*64+k]*coeff[ 47*line ] +
                    iT[49*64+k]*coeff[ 49*line ] + iT[51*64+k]*coeff[ 51*line ] + iT[53*64+k]*coeff[ 53*line ] + iT[55*64+k]*coeff[ 55*line ] +
                    iT[57*64+k]*coeff[ 57*line ] + iT[59*64+k]*coeff[ 59*line ] + iT[61*64+k]*coeff[ 61*line ] + iT[63*64+k]*coeff[ 63*line ] ) );
        }
        for (k=0;k<16;k++)
        {
            EO[k] = iT[ 2*64+k]*coeff[ 2*line  ] + iT[ 6*64+k]*coeff[ 6*line  ] + iT[10*64+k]*coeff[ 10*line ] + iT[14*64+k]*coeff[ 14*line ] +
                    iT[18*64+k]*coeff[ 18*line ] + iT[22*64+k]*coeff[ 22*line ] + iT[26*64+k]*coeff[ 26*line ] + iT[30*64+k]*coeff[ 30*line ] +
                    ( zo ? 0 : (
                               iT[34*64+k]*coeff[ 34*line ] + iT[38*64+k]*coeff[ 38*line ] + iT[42*64+k]*coeff[ 42*line ] + iT[46*64+k]*coeff[ 46*line ] +
                    iT[50*64+k]*coeff[ 50*line ] + iT[54*64+k]*coeff[ 54*line ] + iT[58*64+k]*coeff[ 58*line ] + iT[62*64+k]*coeff[ 62*line ] ) );
        }
        for (k=0;k<8;k++)
        {
            EEO[k] = iT[4*64+k]*coeff[ 4*line ] + iT[12*64+k]*coeff[ 12*line ] + iT[20*64+k]*coeff[ 20*line ] + iT[28*64+k]*coeff[ 28*line ] +
                    ( zo ? 0 : (
                               iT[36*64+k]*coeff[ 36*line ] + iT[44*64+k]*coeff[ 44*line ] + iT[52*64+k]*coeff[ 52*line ] + iT[60*64+k]*coeff[ 60*line ] ) );
        }
        for (k=0;k<4;k++)
        {
            EEEO[k] = iT[8*64+k]*coeff[ 8*line ] + iT[24*64+k]*coeff[ 24*line ] + ( zo ? 0 : ( iT[40*64+k]*coeff[ 40*line ] + iT[56*64+k]*coeff[ 56*line ] ) );
        }
        EEEEO[0] = iT[16*64+0]*coeff[ 16*line ] + ( zo ? 0 : iT[48*64+0]*coeff[ 48*line ] );
        EEEEO[1] = iT[16*64+1]*coeff[ 16*line ] + ( zo ? 0 : iT[48*64+1]*coeff[ 48*line ] );
        EEEEE[0] = iT[ 0*64+0]*coeff[  0      ] + ( zo ? 0 : iT[32*64+0]*coeff[ 32*line ] );
        EEEEE[1] = iT[ 0*64+1]*coeff[  0      ] + ( zo ? 0 : iT[32*64+1]*coeff[ 32*line ] );

        for (k=0;k<2;k++)
        {
            EEEE[k] = EEEEE[k] + EEEEO[k];
            EEEE[k+2] = EEEEE[1-k] - EEEEO[1-k];
        }
        for (k=0;k<4;k++)
        {
            EEE[k] = EEEE[k] + EEEO[k];
            EEE[k+4] = EEEE[3-k] - EEEO[3-k];
        }
        for (k=0;k<8;k++)
        {
            EE[k] = EEE[k] + EEO[k];
            EE[k+8] = EEE[7-k] - EEO[7-k];
        }
        for (k=0;k<16;k++)
        {
            E[k] = EE[k] + EO[k];
            E[k+16] = EE[15-k] - EO[15-k];
        }
        for (k=0;k<32;k++)
        {
            block[k] = av_clip( ((E[k] + O[k] + rnd_factor)>>shift), outputMinimum, outputMaximum);
            block[k+32] = av_clip( ((E[31-k] - O[31-k] + rnd_factor)>>shift), outputMinimum, outputMaximum);
        }
        coeff ++;
        block += uiTrSize;
    }
}
/*
 * End DCT2
 */

/*
 * Fast inverse DCT5 4-8-16-32
 */
static void FUNC(fastInverseDCT5_B4)(int16_t *coeff, int16_t *block, int shift, int line, int zo, int use, const int outputMinimum, const int outputMaximum)
{
    int i, j, k, iSum;
    int rnd_factor = 1<<(shift-1);

    const int16_t *iT = g_aiTr4[DCT_V][0];
    const int uiTrSize = 4;

    for (i=0; i<line; i++)
    {
        for (j=0; j<uiTrSize; j++)
        {
            iSum = 0;
            for (k=0; k<uiTrSize; k++)
            {
                iSum += coeff[k*line]*iT[k*uiTrSize+j];
            }
            block[j] = av_clip(((iSum + rnd_factor)>>shift), outputMinimum, outputMaximum);
        }
        block+=uiTrSize;
        coeff++;
    }
}

static void FUNC(fastInverseDCT5_B8)(int16_t *coeff, int16_t *block, int shift, int line, int zo, int use, const int outputMinimum, const int outputMaximum)
{
    int i, j, k, iSum;
    int rnd_factor = 1<<(shift-1);

    const int uiTrSize = 8;
    const int16_t *iT = g_aiTr8[DCT_V][0];

    for (i=0; i<line; i++)
    {
        for (j=0; j<uiTrSize; j++)
        {
            iSum = 0;
            for (k=0; k<uiTrSize; k++)
            {
                iSum += coeff[k*line]*iT[k*uiTrSize+j];
            }
            block[j] = av_clip(((iSum + rnd_factor)>>shift), outputMinimum, outputMaximum);
        }
        block+=uiTrSize;
        coeff++;
    }
}

static void FUNC(fastInverseDCT5_B16)(int16_t *coeff, int16_t *block, int shift, int line, int zo, int use, const int outputMinimum, const int outputMaximum)
{
    int i, j, k, iSum;
    int rnd_factor = (1<<(shift-1));

    const int uiTrSize = 16;
    const int16_t *iT = g_aiTr16[DCT_V][0];

    for (i=0; i<line; i++)
    {
        for (j=0; j<uiTrSize; j++)
        {
            iSum = 0;
            for (k=0; k<uiTrSize; k++)
            {
                iSum += coeff[k*line]*iT[k*uiTrSize+j];
            }
            block[j] = av_clip( ((iSum + rnd_factor)>>shift), outputMinimum, outputMaximum);
        }
        block+=uiTrSize;
        coeff++;
    }
}

static void FUNC(fastInverseDCT5_B32)(int16_t *coeff, int16_t *block, int shift, int line, int zo, int use, const int outputMinimum, const int outputMaximum)
{
    int i, j, k, iSum;
    int rnd_factor = (1<<(shift-1));

    const int uiTrSize = 32;
    const int16_t *iT = g_aiTr32[DCT_V][0];

    for (i=0; i<line; i++)
    {
        for (j=0; j<uiTrSize; j++)
        {
            iSum = 0;
            for (k=0; k<uiTrSize; k++)
            {
                iSum += (coeff[k*line] * iT[k*uiTrSize+j]);
            }
            block[j] = av_clip( ((iSum + rnd_factor)>>shift), outputMinimum, outputMaximum);
        }
        block+=uiTrSize;
        coeff++;
    }
}
/*
 * End DCT5
 */

/*
 * Fast inverse DCT8 4-8-16-32
 */
static void FUNC(fastInverseDCT8_B4)(int16_t *coeff, int16_t *block, int shift, int line, int zo, int use, const int outputMinimum, const int outputMaximum)
{
    int i;
    int rnd_factor = ( 1<<(shift-1) );

    const int16_t *iT = g_aiTr4[DCT_VIII][0];

    int c[4];
    for (i=0; i<line; i++)
    {
        c[0] = coeff[ 0] + coeff[12];
        c[1] = coeff[ 8] + coeff[ 0];
        c[2] = coeff[12] - coeff[ 8];
        c[3] = iT[1]* coeff[4];

        block[0] = av_clip( ((iT[3] * c[0] + iT[2] * c[1] + c[3] + rnd_factor ) >> shift), outputMinimum, outputMaximum);
        block[1] = av_clip( ((iT[1] * (coeff[0] - coeff[8] - coeff[12]) + rnd_factor ) >> shift), outputMinimum, outputMaximum);
        block[2] = av_clip( ((iT[3] * c[2] + iT[2] * c[0] - c[3] + rnd_factor) >> shift), outputMinimum, outputMaximum);
        block[3] = av_clip( ((iT[3] * c[1] - iT[2] * c[2] - c[3] + rnd_factor ) >> shift), outputMinimum, outputMaximum);

        block+=4;
        coeff++;
    }
}

static void FUNC(fastInverseDCT8_B8)(int16_t *coeff, int16_t *block, int shift, int line, int zo, int use, const int outputMinimum, const int outputMaximum)
{
    int i, j, k, iSum;
    int rnd_factor = ( 1<<(shift-1) );

    const int uiTrSize = 8;
    const int16_t *iT = g_aiTr8[DCT_VIII][0];

    for (i=0; i<line; i++)
    {
        for (j=0; j<uiTrSize; j++)
        {
            iSum = 0;
            for (k=0; k<uiTrSize; k++)
            {
                iSum += coeff[k*line]*iT[k*uiTrSize+j];
            }
            block[j] =  av_clip( ((iSum + rnd_factor)>>shift), outputMinimum, outputMaximum);
        }
        block+=uiTrSize;
        coeff++;
    }
}

static void FUNC(fastInverseDCT8_B16)(int16_t *coeff, int16_t *block, int shift, int line, int zo, int use, const int outputMinimum, const int outputMaximum)
{
    int i, j, k, iSum;
    int rnd_factor = ( 1<<(shift-1) );

    const int uiTrSize = 16;
    const int16_t *iT = g_aiTr16[DCT_VIII][0];

    for (i=0; i<line; i++)
    {
        for (j=0; j<uiTrSize; j++)
        {
            iSum = 0;
            for (k=0; k<uiTrSize; k++)
            {
                iSum += coeff[k*line]*iT[k*uiTrSize+j];
            }
            block[j] =  av_clip( ((iSum + rnd_factor)>>shift), outputMinimum, outputMaximum);
        }
        block+=uiTrSize;
        coeff++;
    }
}

static void FUNC(fastInverseDCT8_B32)(int16_t *coeff, int16_t *block, int shift, int line, int zo, int use, const int outputMinimum, const int outputMaximum)
{
    int i, j, k, iSum;
    int rnd_factor = ( 1<<(shift-1) );

    const int uiTrSize = 32;
    const int16_t *iT = g_aiTr32[DCT_VIII][0];

    if ( zo )
    {
        for (i=0; i<(line >> (zo-1) ); i++)
        {
            for (j=0; j<uiTrSize; j++)
            {
                iSum = 0;
                for (k=0; k<( uiTrSize / 2 ); k++)
                {
                    iSum += coeff[k*line]*iT[k*uiTrSize+j];
                }
                block[j] =  av_clip( ((iSum + rnd_factor)>>shift), outputMinimum, outputMaximum);
            }
            block+=uiTrSize;
            coeff++;
        }
        if( zo==2 )
        {
            memset( block, 0, sizeof(int16_t)*uiTrSize*uiTrSize/2 );
        }
    }
    else
    {
        for (i=0; i<line; i++)
        {
            for (j=0; j<uiTrSize; j++)
            {
                iSum = 0;
                for (k=0; k<uiTrSize; k++)
                {
                    iSum += coeff[k*line]*iT[k*uiTrSize+j];
                }
                block[j] =  av_clip( ((iSum + rnd_factor)>>shift), outputMinimum, outputMaximum);
            }
            block+=uiTrSize;
            coeff++;
        }
    }
}
/*
 * End DCT8
 */

/*
 * Fast inverse DST1 4-8-16-32
 */
static void FUNC(fastInverseDST1_B4)(int16_t *coeff, int16_t *block, int shift, int line, int zo, int use, const int outputMinimum, const int outputMaximum)
{
    int i;
    int rnd_factor = ( 1<<(shift-1) );

    const int16_t *iT = g_aiTr4[DST_I][0];

    int E[2],O[2];
    for (i=0; i<line; i++)
    {
        E[0] = coeff[0*4] + coeff[3*4];
        O[0] = coeff[0*4] - coeff[3*4];
        E[1] = coeff[1*4] + coeff[2*4];
        O[1] = coeff[1*4] - coeff[2*4];

        block[0] = av_clip( ((E[0]*iT[0] + E[1]*iT[1] + rnd_factor)>>shift), outputMinimum, outputMaximum);
        block[1] = av_clip( ((O[0]*iT[1] + O[1]*iT[0] + rnd_factor)>>shift), outputMinimum, outputMaximum);
        block[2] = av_clip( ((E[0]*iT[1] - E[1]*iT[0] + rnd_factor)>>shift), outputMinimum, outputMaximum);
        block[3] = av_clip( ((O[0]*iT[0] - O[1]*iT[1] + rnd_factor)>>shift), outputMinimum, outputMaximum);

        block += 4;
        coeff ++;
    }
}
static void FUNC(fastInverseDST1_B8)(int16_t *coeff, int16_t *block, int shift, int line, int zo, int use, const int outputMinimum, const int outputMaximum)
{
    int i, j, k, iSum;
    int rnd_factor = ( 1<<(shift-1) );

    const int uiTrSize = 8;
    const int16_t *iT = g_aiTr8[DST_I][0];

    for (i=0; i<line; i++)
    {
        for (j=0; j<uiTrSize; j++)
        {
            iSum = 0;
            for (k=0; k<uiTrSize; k++)
            {
                iSum += coeff[k*line+i]*iT[k*uiTrSize+j];
            }
            block[i*uiTrSize+j] = av_clip( ((iSum + rnd_factor)>>shift), outputMinimum, outputMaximum);
        }
    }
}
static void FUNC(fastInverseDST1_B16)(int16_t *coeff, int16_t *block, int shift, int line, int zo, int use, const int outputMinimum, const int outputMaximum)
{
    int i, j, k, iSum;
    int rnd_factor = ( 1<<(shift-1) );

    const int uiTrSize = 16;
    const int16_t *iT = g_aiTr16[DST_I][0];

    for (i=0; i<line; i++)
    {
        for (j=0; j<uiTrSize; j++)
        {
            iSum = 0;
            for (k=0; k<uiTrSize; k++)
            {
                iSum += coeff[k*line+i]*iT[k*uiTrSize+j];
            }
            block[i*uiTrSize+j] =  av_clip( ((iSum + rnd_factor)>>shift), outputMinimum, outputMaximum);
        }
    }
}
static void FUNC(fastInverseDST1_B32)(int16_t *coeff, int16_t *block, int shift, int line, int zo, int use, const int outputMinimum, const int outputMaximum)
{
    int i, j, k, iSum;
    int rnd_factor = ( 1<<(shift-1) );

    const int uiTrSize = 32;
    const int16_t *iT = g_aiTr32[DST_I][0];

    for (i=0; i<line; i++)
    {
        for (j=0; j<uiTrSize; j++)
        {
            iSum = 0;
            for (k=0; k<uiTrSize; k++)
            {
                iSum += coeff[k*line+i]*iT[k*uiTrSize+j];
            }
            block[i*uiTrSize+j] =  av_clip( ((iSum + rnd_factor)>>shift), outputMinimum, outputMaximum);
        }
    }
}
/*
 * End DCT8
 */

/*
 * Fast inverse DSTVII 4-8-16-32
 */
static void FUNC(fastInverseDST7_B4)(int16_t *coeff, int16_t *block, int shift, int line, int zo, int use, const int outputMinimum, const int outputMaximum)
{
    int i, c[4];
    int rnd_factor = ( 1<<(shift-1) );

#if COM16_C806_EMT
    const int16_t *iT = use ? g_aiTr4[DST_VII][0] : g_as_DST_MAT_4[0][0];
#else
    const uint16_t *iT = g_as_DST_MAT_4[0][0];
#endif

    for (i=0; i<line; i++)
    {
        c[0] = coeff[0] + coeff[ 8];
        c[1] = coeff[8] + coeff[12];
        c[2] = coeff[0] - coeff[12];
        c[3] = iT[2]* coeff[4];

        block[0] = av_clip( (( iT[0] * c[0] + iT[1] * c[1] + c[3] + rnd_factor ) >> shift), outputMinimum, outputMaximum);
        block[1] = av_clip( (( iT[1] * c[2] - iT[0] * c[1] + c[3] + rnd_factor ) >> shift), outputMinimum, outputMaximum);
        block[2] = av_clip( (( iT[2] * (coeff[0] - coeff[8]  + coeff[12]) + rnd_factor ) >> shift ), outputMinimum, outputMaximum);
        block[3] = av_clip( (( iT[1] * c[0] + iT[0] * c[2] - c[3] + rnd_factor ) >> shift ), outputMinimum, outputMaximum);

        block+=4;
        coeff++;
    }
}

static void FUNC(fastInverseDST7_B8)(int16_t *coeff, int16_t *block, int shift, int line, int zo, int use, const int outputMinimum, const int outputMaximum)
{
    int i, j, k, iSum;
    int rnd_factor = ( 1<<(shift-1) );

    const int uiTrSize = 8;
    const int16_t *iT = g_aiTr8[DST_VII][0];

    for (i=0; i<line; i++)
    {
        for (j=0; j<uiTrSize; j++)
        {
            iSum = 0;
            for (k=0; k<uiTrSize; k++)
            {
                iSum += coeff[k*line]*iT[k*uiTrSize+j];
            }
            block[j] =  av_clip( ((iSum + rnd_factor)>>shift), outputMinimum, outputMaximum);
        }
        block+=uiTrSize;
        coeff++;
    }
}

static void FUNC(fastInverseDST7_B16)(int16_t *coeff, int16_t *block, int shift, int line, int zo, int use, const int outputMinimum, const int outputMaximum)
{
    int i, j, k, iSum;
    int rnd_factor = ( 1<<(shift-1) );

    const int uiTrSize = 16;
    const int16_t *iT = g_aiTr16[DST_VII][0];
    for (i=0; i<line; i++)
    {
        for (j=0; j<uiTrSize; j++)
        {
            iSum = 0;
            for (k=0; k<uiTrSize; k++)
            {
                iSum += coeff[k*line]*iT[k*uiTrSize+j];
            }
            block[j] =  av_clip( ((iSum + rnd_factor)>>shift), outputMinimum, outputMaximum);
        }
        block+=uiTrSize;
        coeff++;
    }
}

static void FUNC(fastInverseDST7_B32)(int16_t *coeff, int16_t *block, int shift, int line, int zo, int use, const int outputMinimum, const int outputMaximum)
{
    int i, j, k, iSum;
    int rnd_factor = ( 1<<(shift-1) );

    const int uiTrSize = 32;
    const int16_t *iT = g_aiTr32[DST_VII][0];

    if ( zo )
    {
        for (i=0; i<(line>>(zo-1)); i++)
        {
            for (j=0; j<uiTrSize; j++)
            {
                iSum = 0;
                for (k=0; k<( uiTrSize / 2 ); k++)
                {
                    iSum += coeff[k*line]*iT[k*uiTrSize+j];
                }
                block[j] = av_clip( ((iSum + rnd_factor)>>shift), outputMinimum, outputMaximum);
            }
            block+=uiTrSize;
            coeff++;
        }
    }
    else
    {
        for (i=0; i<line; i++)
        {
            for (j=0; j<uiTrSize; j++)
            {
                iSum = 0;
                for (k=0; k<uiTrSize; k++)
                {
                    iSum += coeff[k*line]*iT[k*uiTrSize+j];
                }
                block[j] = av_clip( ((iSum + rnd_factor)>>shift), outputMinimum, outputMaximum);
            }
            block+=uiTrSize;
            coeff++;
        }
    }
}

InvTrans FUNC(*fastInvTrans)[7][5] =
{
  {FUNC(fastInverseDCT2_B4), FUNC(fastInverseDCT2_B8), FUNC(fastInverseDCT2_B16), FUNC(fastInverseDCT2_B32), FUNC(fastInverseDCT2_B64)},//DCT_II
  {NULL			     , NULL			     , NULL			      , NULL			   , NULL               },//DCT_III
  {NULL			     , NULL			     , NULL			      , NULL			   , NULL               },//DCT_I
  {FUNC(fastInverseDST1_B4), FUNC(fastInverseDST1_B8), FUNC(fastInverseDST1_B16), FUNC(fastInverseDST1_B32), NULL               },//DST_I
  {FUNC(fastInverseDST7_B4), FUNC(fastInverseDST7_B8), FUNC(fastInverseDST7_B16), FUNC(fastInverseDST7_B32), NULL               },//DST_VII
  {FUNC(fastInverseDCT8_B4), FUNC(fastInverseDCT8_B8), FUNC(fastInverseDCT8_B16), FUNC(fastInverseDCT8_B32), NULL               },//DCT_VIII
  {FUNC(fastInverseDCT5_B4), FUNC(fastInverseDCT5_B8), FUNC(fastInverseDCT5_B16), FUNC(fastInverseDCT5_B32), NULL               },//DCT_V
};
/*
 * End DSTVII
 */

static void FUNC(idct_emt)(int16_t *coeffs, int16_t *dst, int log2_trafo_size, int nLog2SizeMinus2, int maxLog2TrDynamicRange, int emt_tu_idx_h,int emt_tu_idx_v, int z0_h,int z0_v)
{
    int tr_size          = (1 << log2_trafo_size) ;
    const int shift_v    = EMT_TRANSFORM_MATRIX_SHIFT + 1 + COM16_C806_TRANS_PREC;
    const int shift_h    = (EMT_TRANSFORM_MATRIX_SHIFT + maxLog2TrDynamicRange - 1) - BIT_DEPTH + COM16_C806_TRANS_PREC;

    const int clipMinimum  = -(1 << maxLog2TrDynamicRange);
    const int clipMaximum  =  (1 << maxLog2TrDynamicRange) - 1;

    int16_t tmp[ MAX_TU_SIZE * MAX_TU_SIZE ];

    FUNC(fastInvTrans)[emt_tu_idx_v][nLog2SizeMinus2]( coeffs, tmp, shift_v, tr_size, z0_v, 1, clipMinimum, clipMaximum );
    FUNC(fastInvTrans)[emt_tu_idx_h][nLog2SizeMinus2]( tmp,    dst, shift_h, tr_size, z0_h, 1, clipMinimum, clipMaximum );
}
#endif

static void FUNC(put_hevc_pel_pixels)(int16_t *dst,
                                      uint8_t *_src, ptrdiff_t _srcstride,
                                      int height, intptr_t mx, intptr_t my, int width)
{
    int x, y;
    pixel *src          = (pixel *)_src;
    ptrdiff_t srcstride = _srcstride / sizeof(pixel);

    for (y = 0; y < height; y++) {
        for (x = 0; x < width; x++)
            dst[x] = src[x] << (14 - BIT_DEPTH);
        src += srcstride;
        dst += MAX_PB_SIZE;
    }
}

static void FUNC(put_hevc_pel_uni_pixels)(uint8_t *_dst, ptrdiff_t _dststride, uint8_t *_src, ptrdiff_t _srcstride,
                                          int height, intptr_t mx, intptr_t my, int width)
{
    int y;
    pixel *src          = (pixel *)_src;
    ptrdiff_t srcstride = _srcstride / sizeof(pixel);
    pixel *dst          = (pixel *)_dst;
    ptrdiff_t dststride = _dststride / sizeof(pixel);

    for (y = 0; y < height; y++) {
        memcpy(dst, src, width * sizeof(pixel));
        src += srcstride;
        dst += dststride;
    }
}

static void FUNC(put_hevc_pel_bi_pixels)(uint8_t *_dst, ptrdiff_t _dststride, uint8_t *_src, ptrdiff_t _srcstride,
                                         int16_t *src2,
                                         int height, intptr_t mx, intptr_t my, int width)
{
    int x, y;
    pixel *src          = (pixel *)_src;
    ptrdiff_t srcstride = _srcstride / sizeof(pixel);
    pixel *dst          = (pixel *)_dst;
    ptrdiff_t dststride = _dststride / sizeof(pixel);

    int shift = 14  + 1 - BIT_DEPTH;
#if BIT_DEPTH < 14
    int offset = 1 << (shift - 1);
#else
    int offset = 0;
#endif

    for (y = 0; y < height; y++) {
        for (x = 0; x < width; x++)
            dst[x] = av_clip_pixel(((src[x] << (14 - BIT_DEPTH)) + src2[x] + offset) >> shift);
        src  += srcstride;
        dst  += dststride;
        src2 += MAX_PB_SIZE;
    }
}

static void FUNC(put_hevc_pel_uni_w_pixels)(uint8_t *_dst, ptrdiff_t _dststride, uint8_t *_src, ptrdiff_t _srcstride,
                                            int height, int denom, int wx, int ox, intptr_t mx, intptr_t my, int width)
{
    int x, y;
    pixel *src          = (pixel *)_src;
    ptrdiff_t srcstride = _srcstride / sizeof(pixel);
    pixel *dst          = (pixel *)_dst;
    ptrdiff_t dststride = _dststride / sizeof(pixel);
    int shift = denom + 14 - BIT_DEPTH;
#if BIT_DEPTH < 14
    int offset = 1 << (shift - 1);
#else
    int offset = 0;
#endif

    ox     = ox * (1 << (BIT_DEPTH - 8));
    for (y = 0; y < height; y++) {
        for (x = 0; x < width; x++)
            dst[x] = av_clip_pixel((((src[x] << (14 - BIT_DEPTH)) * wx + offset) >> shift) + ox);
        src += srcstride;
        dst += dststride;
    }
}

static void FUNC(put_hevc_pel_bi_w_pixels)(uint8_t *_dst, ptrdiff_t _dststride, uint8_t *_src, ptrdiff_t _srcstride,
                                           int16_t *src2,
                                           int height, int denom, int wx0, int wx1,
                                           int ox0, int ox1, intptr_t mx, intptr_t my, int width)
{
    int x, y;
    pixel *src          = (pixel *)_src;
    ptrdiff_t srcstride = _srcstride / sizeof(pixel);
    pixel *dst          = (pixel *)_dst;
    ptrdiff_t dststride = _dststride / sizeof(pixel);

    int shift = 14  + 1 - BIT_DEPTH;
    int log2Wd = denom + shift - 1;

    ox0     = ox0 * (1 << (BIT_DEPTH - 8));
    ox1     = ox1 * (1 << (BIT_DEPTH - 8));
    for (y = 0; y < height; y++) {
        for (x = 0; x < width; x++) {
            dst[x] = av_clip_pixel(( (src[x] << (14 - BIT_DEPTH)) * wx1 + src2[x] * wx0 + ((ox0 + ox1 + 1) << log2Wd)) >> (log2Wd + 1));
        }
        src  += srcstride;
        dst  += dststride;
        src2 += MAX_PB_SIZE;
    }
}

////////////////////////////////////////////////////////////////////////////////
//
////////////////////////////////////////////////////////////////////////////////
#define QPEL_FILTER(src, stride)                                               \
    (filter[0] * src[x - 3 * stride] +                                         \
     filter[1] * src[x - 2 * stride] +                                         \
     filter[2] * src[x -     stride] +                                         \
     filter[3] * src[x             ] +                                         \
     filter[4] * src[x +     stride] +                                         \
     filter[5] * src[x + 2 * stride] +                                         \
     filter[6] * src[x + 3 * stride] +                                         \
     filter[7] * src[x + 4 * stride])

static void FUNC(put_hevc_qpel_h)(int16_t *dst,
                                  uint8_t *_src, ptrdiff_t _srcstride,
                                  int height, intptr_t mx, intptr_t my, int width)
{
    int x, y;
    pixel        *src       = (pixel*)_src;
    ptrdiff_t     srcstride = _srcstride / sizeof(pixel);
    const int8_t *filter    = ff_hevc_qpel_filters[mx - 1];
    for (y = 0; y < height; y++) {
        for (x = 0; x < width; x++)
            dst[x] = QPEL_FILTER(src, 1) >> (BIT_DEPTH - 8);
        src += srcstride;
        dst += MAX_PB_SIZE;
    }
}

static void FUNC(put_hevc_qpel_v)(int16_t *dst,
                                  uint8_t *_src, ptrdiff_t _srcstride,
                                  int height, intptr_t mx, intptr_t my, int width)
{
    int x, y;
    pixel        *src       = (pixel*)_src;
    ptrdiff_t     srcstride = _srcstride / sizeof(pixel);
    const int8_t *filter    = ff_hevc_qpel_filters[my - 1];
    for (y = 0; y < height; y++)  {
        for (x = 0; x < width; x++)
            dst[x] = QPEL_FILTER(src, srcstride) >> (BIT_DEPTH - 8);
        src += srcstride;
        dst += MAX_PB_SIZE;
    }
}

static void FUNC(put_hevc_qpel_hv)(int16_t *dst,
                                   uint8_t *_src,
                                   ptrdiff_t _srcstride,
                                   int height, intptr_t mx,
                                   intptr_t my, int width)
{
    int x, y;
    const int8_t *filter;
    pixel *src = (pixel*)_src;
    ptrdiff_t srcstride = _srcstride / sizeof(pixel);
    int16_t tmp_array[(MAX_PB_SIZE + QPEL_EXTRA) * MAX_PB_SIZE];
    int16_t *tmp = tmp_array;

    src   -= QPEL_EXTRA_BEFORE * srcstride;
    filter = ff_hevc_qpel_filters[mx - 1];
    for (y = 0; y < height + QPEL_EXTRA; y++) {
        for (x = 0; x < width; x++)
            tmp[x] = QPEL_FILTER(src, 1) >> (BIT_DEPTH - 8);
        src += srcstride;
        tmp += MAX_PB_SIZE;
    }

    tmp    = tmp_array + QPEL_EXTRA_BEFORE * MAX_PB_SIZE;
    filter = ff_hevc_qpel_filters[my - 1];
    for (y = 0; y < height; y++) {
        for (x = 0; x < width; x++)
            dst[x] = QPEL_FILTER(tmp, MAX_PB_SIZE) >> 6;
        tmp += MAX_PB_SIZE;
        dst += MAX_PB_SIZE;
    }
}

static void FUNC(put_hevc_qpel_uni_h)(uint8_t *_dst,  ptrdiff_t _dststride,
                                      uint8_t *_src, ptrdiff_t _srcstride,
                                      int height, intptr_t mx, intptr_t my, int width)
{
    int x, y;
    pixel        *src       = (pixel*)_src;
    ptrdiff_t     srcstride = _srcstride / sizeof(pixel);
    pixel *dst          = (pixel *)_dst;
    ptrdiff_t dststride = _dststride / sizeof(pixel);
    const int8_t *filter    = ff_hevc_qpel_filters[mx - 1];
    int shift = 14 - BIT_DEPTH;

#if BIT_DEPTH < 14
    int offset = 1 << (shift - 1);
#else
    int offset = 0;
#endif

    for (y = 0; y < height; y++) {
        for (x = 0; x < width; x++)
            dst[x] = av_clip_pixel(((QPEL_FILTER(src, 1) >> (BIT_DEPTH - 8)) + offset) >> shift);
        src += srcstride;
        dst += dststride;
    }
}

static void FUNC(put_hevc_qpel_bi_h)(uint8_t *_dst, ptrdiff_t _dststride, uint8_t *_src, ptrdiff_t _srcstride,
                                     int16_t *src2,
                                     int height, intptr_t mx, intptr_t my, int width)
{
    int x, y;
    pixel        *src       = (pixel*)_src;
    ptrdiff_t     srcstride = _srcstride / sizeof(pixel);
    pixel *dst          = (pixel *)_dst;
    ptrdiff_t dststride = _dststride / sizeof(pixel);

    const int8_t *filter    = ff_hevc_qpel_filters[mx - 1];

    int shift = 14  + 1 - BIT_DEPTH;
#if BIT_DEPTH < 14
    int offset = 1 << (shift - 1);
#else
    int offset = 0;
#endif

    for (y = 0; y < height; y++) {
        for (x = 0; x < width; x++)
            dst[x] = av_clip_pixel(((QPEL_FILTER(src, 1) >> (BIT_DEPTH - 8)) + src2[x] + offset) >> shift);
        src  += srcstride;
        dst  += dststride;
        src2 += MAX_PB_SIZE;
    }
}

static void FUNC(put_hevc_qpel_uni_v)(uint8_t *_dst,  ptrdiff_t _dststride,
                                     uint8_t *_src, ptrdiff_t _srcstride,
                                     int height, intptr_t mx, intptr_t my, int width)
{
    int x, y;
    pixel        *src       = (pixel*)_src;
    ptrdiff_t     srcstride = _srcstride / sizeof(pixel);
    pixel *dst          = (pixel *)_dst;
    ptrdiff_t dststride = _dststride / sizeof(pixel);
    const int8_t *filter    = ff_hevc_qpel_filters[my - 1];
    int shift = 14 - BIT_DEPTH;

#if BIT_DEPTH < 14
    int offset = 1 << (shift - 1);
#else
    int offset = 0;
#endif

    for (y = 0; y < height; y++) {
        for (x = 0; x < width; x++)
            dst[x] = av_clip_pixel(((QPEL_FILTER(src, srcstride) >> (BIT_DEPTH - 8)) + offset) >> shift);
        src += srcstride;
        dst += dststride;
    }
}


static void FUNC(put_hevc_qpel_bi_v)(uint8_t *_dst, ptrdiff_t _dststride, uint8_t *_src, ptrdiff_t _srcstride,
                                     int16_t *src2,
                                     int height, intptr_t mx, intptr_t my, int width)
{
    int x, y;
    pixel        *src       = (pixel*)_src;
    ptrdiff_t     srcstride = _srcstride / sizeof(pixel);
    pixel *dst          = (pixel *)_dst;
    ptrdiff_t dststride = _dststride / sizeof(pixel);

    const int8_t *filter    = ff_hevc_qpel_filters[my - 1];

    int shift = 14 + 1 - BIT_DEPTH;
#if BIT_DEPTH < 14
    int offset = 1 << (shift - 1);
#else
    int offset = 0;
#endif

    for (y = 0; y < height; y++) {
        for (x = 0; x < width; x++)
            dst[x] = av_clip_pixel(((QPEL_FILTER(src, srcstride) >> (BIT_DEPTH - 8)) + src2[x] + offset) >> shift);
        src  += srcstride;
        dst  += dststride;
        src2 += MAX_PB_SIZE;
    }
}

static void FUNC(put_hevc_qpel_uni_hv)(uint8_t *_dst,  ptrdiff_t _dststride,
                                       uint8_t *_src, ptrdiff_t _srcstride,
                                       int height, intptr_t mx, intptr_t my, int width)
{
    int x, y;
    const int8_t *filter;
    pixel *src = (pixel*)_src;
    ptrdiff_t srcstride = _srcstride / sizeof(pixel);
    pixel *dst          = (pixel *)_dst;
    ptrdiff_t dststride = _dststride / sizeof(pixel);
    int16_t tmp_array[(MAX_PB_SIZE + QPEL_EXTRA) * MAX_PB_SIZE];
    int16_t *tmp = tmp_array;
    int shift =  14 - BIT_DEPTH;

#if BIT_DEPTH < 14
    int offset = 1 << (shift - 1);
#else
    int offset = 0;
#endif

    src   -= QPEL_EXTRA_BEFORE * srcstride;
    filter = ff_hevc_qpel_filters[mx - 1];
    for (y = 0; y < height + QPEL_EXTRA; y++) {
        for (x = 0; x < width; x++)
            tmp[x] = QPEL_FILTER(src, 1) >> (BIT_DEPTH - 8);
        src += srcstride;
        tmp += MAX_PB_SIZE;
    }

    tmp    = tmp_array + QPEL_EXTRA_BEFORE * MAX_PB_SIZE;
    filter = ff_hevc_qpel_filters[my - 1];

    for (y = 0; y < height; y++) {
        for (x = 0; x < width; x++)
            dst[x] = av_clip_pixel(((QPEL_FILTER(tmp, MAX_PB_SIZE) >> 6) + offset) >> shift);
        tmp += MAX_PB_SIZE;
        dst += dststride;
    }
}

static void FUNC(put_hevc_qpel_bi_hv)(uint8_t *_dst, ptrdiff_t _dststride, uint8_t *_src, ptrdiff_t _srcstride,
                                      int16_t *src2,
                                      int height, intptr_t mx, intptr_t my, int width)
{
    int x, y;
    const int8_t *filter;
    pixel *src = (pixel*)_src;
    ptrdiff_t srcstride = _srcstride / sizeof(pixel);
    pixel *dst          = (pixel *)_dst;
    ptrdiff_t dststride = _dststride / sizeof(pixel);
    int16_t tmp_array[(MAX_PB_SIZE + QPEL_EXTRA) * MAX_PB_SIZE];
    int16_t *tmp = tmp_array;
    int shift = 14 + 1 - BIT_DEPTH;
#if BIT_DEPTH < 14
    int offset = 1 << (shift - 1);
#else
    int offset = 0;
#endif

    src   -= QPEL_EXTRA_BEFORE * srcstride;
    filter = ff_hevc_qpel_filters[mx - 1];
    for (y = 0; y < height + QPEL_EXTRA; y++) {
        for (x = 0; x < width; x++)
            tmp[x] = QPEL_FILTER(src, 1) >> (BIT_DEPTH - 8);
        src += srcstride;
        tmp += MAX_PB_SIZE;
    }

    tmp    = tmp_array + QPEL_EXTRA_BEFORE * MAX_PB_SIZE;
    filter = ff_hevc_qpel_filters[my - 1];

    for (y = 0; y < height; y++) {
        for (x = 0; x < width; x++)
            dst[x] = av_clip_pixel(((QPEL_FILTER(tmp, MAX_PB_SIZE) >> 6) + src2[x] + offset) >> shift);
        tmp  += MAX_PB_SIZE;
        dst  += dststride;
        src2 += MAX_PB_SIZE;
    }
}

static void FUNC(put_hevc_qpel_uni_w_h)(uint8_t *_dst,  ptrdiff_t _dststride,
                                        uint8_t *_src, ptrdiff_t _srcstride,
                                        int height, int denom, int wx, int ox,
                                        intptr_t mx, intptr_t my, int width)
{
    int x, y;
    pixel        *src       = (pixel*)_src;
    ptrdiff_t     srcstride = _srcstride / sizeof(pixel);
    pixel *dst          = (pixel *)_dst;
    ptrdiff_t dststride = _dststride / sizeof(pixel);
    const int8_t *filter    = ff_hevc_qpel_filters[mx - 1];
    int shift = denom + 14 - BIT_DEPTH;
#if BIT_DEPTH < 14
    int offset = 1 << (shift - 1);
#else
    int offset = 0;
#endif

    ox = ox * (1 << (BIT_DEPTH - 8));
    for (y = 0; y < height; y++) {
        for (x = 0; x < width; x++)
            dst[x] = av_clip_pixel((((QPEL_FILTER(src, 1) >> (BIT_DEPTH - 8)) * wx + offset) >> shift) + ox);
        src += srcstride;
        dst += dststride;
    }
}

static void FUNC(put_hevc_qpel_bi_w_h)(uint8_t *_dst, ptrdiff_t _dststride, uint8_t *_src, ptrdiff_t _srcstride,
                                       int16_t *src2,
                                       int height, int denom, int wx0, int wx1,
                                       int ox0, int ox1, intptr_t mx, intptr_t my, int width)
{
    int x, y;
    pixel        *src       = (pixel*)_src;
    ptrdiff_t     srcstride = _srcstride / sizeof(pixel);
    pixel *dst          = (pixel *)_dst;
    ptrdiff_t dststride = _dststride / sizeof(pixel);

    const int8_t *filter    = ff_hevc_qpel_filters[mx - 1];

    int shift = 14  + 1 - BIT_DEPTH;
    int log2Wd = denom + shift - 1;

    ox0     = ox0 * (1 << (BIT_DEPTH - 8));
    ox1     = ox1 * (1 << (BIT_DEPTH - 8));
    for (y = 0; y < height; y++) {
        for (x = 0; x < width; x++)
            dst[x] = av_clip_pixel(((QPEL_FILTER(src, 1) >> (BIT_DEPTH - 8)) * wx1 + src2[x] * wx0 +
                                    ((ox0 + ox1 + 1) << log2Wd)) >> (log2Wd + 1));
        src  += srcstride;
        dst  += dststride;
        src2 += MAX_PB_SIZE;
    }
}

static void FUNC(put_hevc_qpel_uni_w_v)(uint8_t *_dst,  ptrdiff_t _dststride,
                                        uint8_t *_src, ptrdiff_t _srcstride,
                                        int height, int denom, int wx, int ox,
                                        intptr_t mx, intptr_t my, int width)
{
    int x, y;
    pixel        *src       = (pixel*)_src;
    ptrdiff_t     srcstride = _srcstride / sizeof(pixel);
    pixel *dst          = (pixel *)_dst;
    ptrdiff_t dststride = _dststride / sizeof(pixel);
    const int8_t *filter    = ff_hevc_qpel_filters[my - 1];
    int shift = denom + 14 - BIT_DEPTH;
#if BIT_DEPTH < 14
    int offset = 1 << (shift - 1);
#else
    int offset = 0;
#endif

    ox = ox * (1 << (BIT_DEPTH - 8));
    for (y = 0; y < height; y++) {
        for (x = 0; x < width; x++)
            dst[x] = av_clip_pixel((((QPEL_FILTER(src, srcstride) >> (BIT_DEPTH - 8)) * wx + offset) >> shift) + ox);
        src += srcstride;
        dst += dststride;
    }
}

static void FUNC(put_hevc_qpel_bi_w_v)(uint8_t *_dst, ptrdiff_t _dststride, uint8_t *_src, ptrdiff_t _srcstride,
                                       int16_t *src2,
                                       int height, int denom, int wx0, int wx1,
                                       int ox0, int ox1, intptr_t mx, intptr_t my, int width)
{
    int x, y;
    pixel        *src       = (pixel*)_src;
    ptrdiff_t     srcstride = _srcstride / sizeof(pixel);
    pixel *dst          = (pixel *)_dst;
    ptrdiff_t dststride = _dststride / sizeof(pixel);

    const int8_t *filter    = ff_hevc_qpel_filters[my - 1];

    int shift = 14 + 1 - BIT_DEPTH;
    int log2Wd = denom + shift - 1;

    ox0     = ox0 * (1 << (BIT_DEPTH - 8));
    ox1     = ox1 * (1 << (BIT_DEPTH - 8));
    for (y = 0; y < height; y++) {
        for (x = 0; x < width; x++)
            dst[x] = av_clip_pixel(((QPEL_FILTER(src, srcstride) >> (BIT_DEPTH - 8)) * wx1 + src2[x] * wx0 +
                                    ((ox0 + ox1 + 1) << log2Wd)) >> (log2Wd + 1));
        src  += srcstride;
        dst  += dststride;
        src2 += MAX_PB_SIZE;
    }
}

static void FUNC(put_hevc_qpel_uni_w_hv)(uint8_t *_dst,  ptrdiff_t _dststride,
                                         uint8_t *_src, ptrdiff_t _srcstride,
                                         int height, int denom, int wx, int ox,
                                         intptr_t mx, intptr_t my, int width)
{
    int x, y;
    const int8_t *filter;
    pixel *src = (pixel*)_src;
    ptrdiff_t srcstride = _srcstride / sizeof(pixel);
    pixel *dst          = (pixel *)_dst;
    ptrdiff_t dststride = _dststride / sizeof(pixel);
    int16_t tmp_array[(MAX_PB_SIZE + QPEL_EXTRA) * MAX_PB_SIZE];
    int16_t *tmp = tmp_array;
    int shift = denom + 14 - BIT_DEPTH;
#if BIT_DEPTH < 14
    int offset = 1 << (shift - 1);
#else
    int offset = 0;
#endif

    src   -= QPEL_EXTRA_BEFORE * srcstride;
    filter = ff_hevc_qpel_filters[mx - 1];
    for (y = 0; y < height + QPEL_EXTRA; y++) {
        for (x = 0; x < width; x++)
            tmp[x] = QPEL_FILTER(src, 1) >> (BIT_DEPTH - 8);
        src += srcstride;
        tmp += MAX_PB_SIZE;
    }

    tmp    = tmp_array + QPEL_EXTRA_BEFORE * MAX_PB_SIZE;
    filter = ff_hevc_qpel_filters[my - 1];

    ox = ox * (1 << (BIT_DEPTH - 8));
    for (y = 0; y < height; y++) {
        for (x = 0; x < width; x++)
            dst[x] = av_clip_pixel((((QPEL_FILTER(tmp, MAX_PB_SIZE) >> 6) * wx + offset) >> shift) + ox);
        tmp += MAX_PB_SIZE;
        dst += dststride;
    }
}

static void FUNC(put_hevc_qpel_bi_w_hv)(uint8_t *_dst, ptrdiff_t _dststride, uint8_t *_src, ptrdiff_t _srcstride,
                                        int16_t *src2,
                                        int height, int denom, int wx0, int wx1,
                                        int ox0, int ox1, intptr_t mx, intptr_t my, int width)
{
    int x, y;
    const int8_t *filter;
    pixel *src = (pixel*)_src;
    ptrdiff_t srcstride = _srcstride / sizeof(pixel);
    pixel *dst          = (pixel *)_dst;
    ptrdiff_t dststride = _dststride / sizeof(pixel);
    int16_t tmp_array[(MAX_PB_SIZE + QPEL_EXTRA) * MAX_PB_SIZE];
    int16_t *tmp = tmp_array;
    int shift = 14 + 1 - BIT_DEPTH;
    int log2Wd = denom + shift - 1;

    src   -= QPEL_EXTRA_BEFORE * srcstride;
    filter = ff_hevc_qpel_filters[mx - 1];
    for (y = 0; y < height + QPEL_EXTRA; y++) {
        for (x = 0; x < width; x++)
            tmp[x] = QPEL_FILTER(src, 1) >> (BIT_DEPTH - 8);
        src += srcstride;
        tmp += MAX_PB_SIZE;
    }

    tmp    = tmp_array + QPEL_EXTRA_BEFORE * MAX_PB_SIZE;
    filter = ff_hevc_qpel_filters[my - 1];

    ox0     = ox0 * (1 << (BIT_DEPTH - 8));
    ox1     = ox1 * (1 << (BIT_DEPTH - 8));
    for (y = 0; y < height; y++) {
        for (x = 0; x < width; x++)
            dst[x] = av_clip_pixel(((QPEL_FILTER(tmp, MAX_PB_SIZE) >> 6) * wx1 + src2[x] * wx0 +
                                    ((ox0 + ox1 + 1) << log2Wd)) >> (log2Wd + 1));
        tmp  += MAX_PB_SIZE;
        dst  += dststride;
        src2 += MAX_PB_SIZE;
    }
}

////////////////////////////////////////////////////////////////////////////////
//
////////////////////////////////////////////////////////////////////////////////
#define EPEL_FILTER(src, stride)                                               \
    (filter[0] * src[x - stride] +                                             \
     filter[1] * src[x]          +                                             \
     filter[2] * src[x + stride] +                                             \
     filter[3] * src[x + 2 * stride])

static void FUNC(put_hevc_epel_h)(int16_t *dst,
                                  uint8_t *_src, ptrdiff_t _srcstride,
                                  int height, intptr_t mx, intptr_t my, int width)
{
    int x, y;
    pixel *src = (pixel *)_src;
    ptrdiff_t srcstride  = _srcstride / sizeof(pixel);
    const int8_t *filter = ff_hevc_epel_filters[mx - 1];
    for (y = 0; y < height; y++) {
        for (x = 0; x < width; x++)
            dst[x] = EPEL_FILTER(src, 1) >> (BIT_DEPTH - 8);
        src += srcstride;
        dst += MAX_PB_SIZE;
    }
}

static void FUNC(put_hevc_epel_v)(int16_t *dst,
                                  uint8_t *_src, ptrdiff_t _srcstride,
                                  int height, intptr_t mx, intptr_t my, int width)
{
    int x, y;
    pixel *src = (pixel *)_src;
    ptrdiff_t srcstride = _srcstride / sizeof(pixel);
    const int8_t *filter = ff_hevc_epel_filters[my - 1];

    for (y = 0; y < height; y++) {
        for (x = 0; x < width; x++)
            dst[x] = EPEL_FILTER(src, srcstride) >> (BIT_DEPTH - 8);
        src += srcstride;
        dst += MAX_PB_SIZE;
    }
}

static void FUNC(put_hevc_epel_hv)(int16_t *dst,
                                   uint8_t *_src, ptrdiff_t _srcstride,
                                   int height, intptr_t mx, intptr_t my, int width)
{
    int x, y;
    pixel *src = (pixel *)_src;
    ptrdiff_t srcstride = _srcstride / sizeof(pixel);
    const int8_t *filter = ff_hevc_epel_filters[mx - 1];
    int16_t tmp_array[(MAX_PB_SIZE + EPEL_EXTRA) * MAX_PB_SIZE];
    int16_t *tmp = tmp_array;

    src -= EPEL_EXTRA_BEFORE * srcstride;

    for (y = 0; y < height + EPEL_EXTRA; y++) {
        for (x = 0; x < width; x++)
            tmp[x] = EPEL_FILTER(src, 1) >> (BIT_DEPTH - 8);
        src += srcstride;
        tmp += MAX_PB_SIZE;
    }

    tmp      = tmp_array + EPEL_EXTRA_BEFORE * MAX_PB_SIZE;
    filter = ff_hevc_epel_filters[my - 1];

    for (y = 0; y < height; y++) {
        for (x = 0; x < width; x++)
            dst[x] = EPEL_FILTER(tmp, MAX_PB_SIZE) >> 6;
        tmp += MAX_PB_SIZE;
        dst += MAX_PB_SIZE;
    }
}

static void FUNC(put_hevc_epel_uni_h)(uint8_t *_dst, ptrdiff_t _dststride, uint8_t *_src, ptrdiff_t _srcstride,
                                      int height, intptr_t mx, intptr_t my, int width)
{
    int x, y;
    pixel *src = (pixel *)_src;
    ptrdiff_t srcstride  = _srcstride / sizeof(pixel);
    pixel *dst          = (pixel *)_dst;
    ptrdiff_t dststride = _dststride / sizeof(pixel);
    const int8_t *filter = ff_hevc_epel_filters[mx - 1];
    int shift = 14 - BIT_DEPTH;
#if BIT_DEPTH < 14
    int offset = 1 << (shift - 1);
#else
    int offset = 0;
#endif

    for (y = 0; y < height; y++) {
        for (x = 0; x < width; x++)
            dst[x] = av_clip_pixel(((EPEL_FILTER(src, 1) >> (BIT_DEPTH - 8)) + offset) >> shift);
        src += srcstride;
        dst += dststride;
    }
}

static void FUNC(put_hevc_epel_bi_h)(uint8_t *_dst, ptrdiff_t _dststride, uint8_t *_src, ptrdiff_t _srcstride,
                                     int16_t *src2,
                                     int height, intptr_t mx, intptr_t my, int width)
{
    int x, y;
    pixel *src = (pixel *)_src;
    ptrdiff_t srcstride  = _srcstride / sizeof(pixel);
    pixel *dst          = (pixel *)_dst;
    ptrdiff_t dststride = _dststride / sizeof(pixel);
    const int8_t *filter = ff_hevc_epel_filters[mx - 1];
    int shift = 14 + 1 - BIT_DEPTH;
#if BIT_DEPTH < 14
    int offset = 1 << (shift - 1);
#else
    int offset = 0;
#endif

    for (y = 0; y < height; y++) {
        for (x = 0; x < width; x++) {
            dst[x] = av_clip_pixel(((EPEL_FILTER(src, 1) >> (BIT_DEPTH - 8)) + src2[x] + offset) >> shift);
        }
        dst  += dststride;
        src  += srcstride;
        src2 += MAX_PB_SIZE;
    }
}

static void FUNC(put_hevc_epel_uni_v)(uint8_t *_dst, ptrdiff_t _dststride, uint8_t *_src, ptrdiff_t _srcstride,
                                      int height, intptr_t mx, intptr_t my, int width)
{
    int x, y;
    pixel *src = (pixel *)_src;
    ptrdiff_t srcstride  = _srcstride / sizeof(pixel);
    pixel *dst          = (pixel *)_dst;
    ptrdiff_t dststride = _dststride / sizeof(pixel);
    const int8_t *filter = ff_hevc_epel_filters[my - 1];
    int shift = 14 - BIT_DEPTH;
#if BIT_DEPTH < 14
    int offset = 1 << (shift - 1);
#else
    int offset = 0;
#endif

    for (y = 0; y < height; y++) {
        for (x = 0; x < width; x++)
            dst[x] = av_clip_pixel(((EPEL_FILTER(src, srcstride) >> (BIT_DEPTH - 8)) + offset) >> shift);
        src += srcstride;
        dst += dststride;
    }
}

static void FUNC(put_hevc_epel_bi_v)(uint8_t *_dst, ptrdiff_t _dststride, uint8_t *_src, ptrdiff_t _srcstride,
                                     int16_t *src2,
                                     int height, intptr_t mx, intptr_t my, int width)
{
    int x, y;
    pixel *src = (pixel *)_src;
    ptrdiff_t srcstride  = _srcstride / sizeof(pixel);
    const int8_t *filter = ff_hevc_epel_filters[my - 1];
    pixel *dst          = (pixel *)_dst;
    ptrdiff_t dststride = _dststride / sizeof(pixel);
    int shift = 14 + 1 - BIT_DEPTH;
#if BIT_DEPTH < 14
    int offset = 1 << (shift - 1);
#else
    int offset = 0;
#endif

    for (y = 0; y < height; y++) {
        for (x = 0; x < width; x++)
            dst[x] = av_clip_pixel(((EPEL_FILTER(src, srcstride) >> (BIT_DEPTH - 8)) + src2[x] + offset) >> shift);
        dst  += dststride;
        src  += srcstride;
        src2 += MAX_PB_SIZE;
    }
}

static void FUNC(put_hevc_epel_uni_hv)(uint8_t *_dst, ptrdiff_t _dststride, uint8_t *_src, ptrdiff_t _srcstride,
                                       int height, intptr_t mx, intptr_t my, int width)
{
    int x, y;
    pixel *src = (pixel *)_src;
    ptrdiff_t srcstride = _srcstride / sizeof(pixel);
    pixel *dst          = (pixel *)_dst;
    ptrdiff_t dststride = _dststride / sizeof(pixel);
    const int8_t *filter = ff_hevc_epel_filters[mx - 1];
    int16_t tmp_array[(MAX_PB_SIZE + EPEL_EXTRA) * MAX_PB_SIZE];
    int16_t *tmp = tmp_array;
    int shift = 14 - BIT_DEPTH;
#if BIT_DEPTH < 14
    int offset = 1 << (shift - 1);
#else
    int offset = 0;
#endif

    src -= EPEL_EXTRA_BEFORE * srcstride;

    for (y = 0; y < height + EPEL_EXTRA; y++) {
        for (x = 0; x < width; x++)
            tmp[x] = EPEL_FILTER(src, 1) >> (BIT_DEPTH - 8);
        src += srcstride;
        tmp += MAX_PB_SIZE;
    }

    tmp      = tmp_array + EPEL_EXTRA_BEFORE * MAX_PB_SIZE;
    filter = ff_hevc_epel_filters[my - 1];

    for (y = 0; y < height; y++) {
        for (x = 0; x < width; x++)
            dst[x] = av_clip_pixel(((EPEL_FILTER(tmp, MAX_PB_SIZE) >> 6) + offset) >> shift);
        tmp += MAX_PB_SIZE;
        dst += dststride;
    }
}

static void FUNC(put_hevc_epel_bi_hv)(uint8_t *_dst, ptrdiff_t _dststride, uint8_t *_src, ptrdiff_t _srcstride,
                                      int16_t *src2,
                                      int height, intptr_t mx, intptr_t my, int width)
{
    int x, y;
    pixel *src = (pixel *)_src;
    ptrdiff_t srcstride = _srcstride / sizeof(pixel);
    pixel *dst          = (pixel *)_dst;
    ptrdiff_t dststride = _dststride / sizeof(pixel);
    const int8_t *filter = ff_hevc_epel_filters[mx - 1];
    int16_t tmp_array[(MAX_PB_SIZE + EPEL_EXTRA) * MAX_PB_SIZE];
    int16_t *tmp = tmp_array;
    int shift = 14 + 1 - BIT_DEPTH;
#if BIT_DEPTH < 14
    int offset = 1 << (shift - 1);
#else
    int offset = 0;
#endif

    src -= EPEL_EXTRA_BEFORE * srcstride;

    for (y = 0; y < height + EPEL_EXTRA; y++) {
        for (x = 0; x < width; x++)
            tmp[x] = EPEL_FILTER(src, 1) >> (BIT_DEPTH - 8);
        src += srcstride;
        tmp += MAX_PB_SIZE;
    }

    tmp      = tmp_array + EPEL_EXTRA_BEFORE * MAX_PB_SIZE;
    filter = ff_hevc_epel_filters[my - 1];

    for (y = 0; y < height; y++) {
        for (x = 0; x < width; x++)
            dst[x] = av_clip_pixel(((EPEL_FILTER(tmp, MAX_PB_SIZE) >> 6) + src2[x] + offset) >> shift);
        tmp  += MAX_PB_SIZE;
        dst  += dststride;
        src2 += MAX_PB_SIZE;
    }
}

static void FUNC(put_hevc_epel_uni_w_h)(uint8_t *_dst, ptrdiff_t _dststride, uint8_t *_src, ptrdiff_t _srcstride,
                                        int height, int denom, int wx, int ox, intptr_t mx, intptr_t my, int width)
{
    int x, y;
    pixel *src = (pixel *)_src;
    ptrdiff_t srcstride  = _srcstride / sizeof(pixel);
    pixel *dst          = (pixel *)_dst;
    ptrdiff_t dststride = _dststride / sizeof(pixel);
    const int8_t *filter = ff_hevc_epel_filters[mx - 1];
    int shift = denom + 14 - BIT_DEPTH;
#if BIT_DEPTH < 14
    int offset = 1 << (shift - 1);
#else
    int offset = 0;
#endif

    ox     = ox * (1 << (BIT_DEPTH - 8));
    for (y = 0; y < height; y++) {
        for (x = 0; x < width; x++) {
            dst[x] = av_clip_pixel((((EPEL_FILTER(src, 1) >> (BIT_DEPTH - 8)) * wx + offset) >> shift) + ox);
        }
        dst += dststride;
        src += srcstride;
    }
}

static void FUNC(put_hevc_epel_bi_w_h)(uint8_t *_dst, ptrdiff_t _dststride, uint8_t *_src, ptrdiff_t _srcstride,
                                       int16_t *src2,
                                       int height, int denom, int wx0, int wx1,
                                       int ox0, int ox1, intptr_t mx, intptr_t my, int width)
{
    int x, y;
    pixel *src = (pixel *)_src;
    ptrdiff_t srcstride  = _srcstride / sizeof(pixel);
    pixel *dst          = (pixel *)_dst;
    ptrdiff_t dststride = _dststride / sizeof(pixel);
    const int8_t *filter = ff_hevc_epel_filters[mx - 1];
    int shift = 14 + 1 - BIT_DEPTH;
    int log2Wd = denom + shift - 1;

    ox0     = ox0 * (1 << (BIT_DEPTH - 8));
    ox1     = ox1 * (1 << (BIT_DEPTH - 8));
    for (y = 0; y < height; y++) {
        for (x = 0; x < width; x++)
            dst[x] = av_clip_pixel(((EPEL_FILTER(src, 1) >> (BIT_DEPTH - 8)) * wx1 + src2[x] * wx0 +
                                    ((ox0 + ox1 + 1) << log2Wd)) >> (log2Wd + 1));
        src  += srcstride;
        dst  += dststride;
        src2 += MAX_PB_SIZE;
    }
}

static void FUNC(put_hevc_epel_uni_w_v)(uint8_t *_dst, ptrdiff_t _dststride, uint8_t *_src, ptrdiff_t _srcstride,
                                        int height, int denom, int wx, int ox, intptr_t mx, intptr_t my, int width)
{
    int x, y;
    pixel *src = (pixel *)_src;
    ptrdiff_t srcstride  = _srcstride / sizeof(pixel);
    pixel *dst          = (pixel *)_dst;
    ptrdiff_t dststride = _dststride / sizeof(pixel);
    const int8_t *filter = ff_hevc_epel_filters[my - 1];
    int shift = denom + 14 - BIT_DEPTH;
#if BIT_DEPTH < 14
    int offset = 1 << (shift - 1);
#else
    int offset = 0;
#endif

    ox     = ox * (1 << (BIT_DEPTH - 8));
    for (y = 0; y < height; y++) {
        for (x = 0; x < width; x++) {
            dst[x] = av_clip_pixel((((EPEL_FILTER(src, srcstride) >> (BIT_DEPTH - 8)) * wx + offset) >> shift) + ox);
        }
        dst += dststride;
        src += srcstride;
    }
}

static void FUNC(put_hevc_epel_bi_w_v)(uint8_t *_dst, ptrdiff_t _dststride, uint8_t *_src, ptrdiff_t _srcstride,
                                       int16_t *src2,
                                       int height, int denom, int wx0, int wx1,
                                       int ox0, int ox1, intptr_t mx, intptr_t my, int width)
{
    int x, y;
    pixel *src = (pixel *)_src;
    ptrdiff_t srcstride  = _srcstride / sizeof(pixel);
    const int8_t *filter = ff_hevc_epel_filters[my - 1];
    pixel *dst          = (pixel *)_dst;
    ptrdiff_t dststride = _dststride / sizeof(pixel);
    int shift = 14 + 1 - BIT_DEPTH;
    int log2Wd = denom + shift - 1;

    ox0     = ox0 * (1 << (BIT_DEPTH - 8));
    ox1     = ox1 * (1 << (BIT_DEPTH - 8));
    for (y = 0; y < height; y++) {
        for (x = 0; x < width; x++)
            dst[x] = av_clip_pixel(((EPEL_FILTER(src, srcstride) >> (BIT_DEPTH - 8)) * wx1 + src2[x] * wx0 +
                                    ((ox0 + ox1 + 1) << log2Wd)) >> (log2Wd + 1));
        src  += srcstride;
        dst  += dststride;
        src2 += MAX_PB_SIZE;
    }
}

static void FUNC(put_hevc_epel_uni_w_hv)(uint8_t *_dst, ptrdiff_t _dststride, uint8_t *_src, ptrdiff_t _srcstride,
                                         int height, int denom, int wx, int ox, intptr_t mx, intptr_t my, int width)
{
    int x, y;
    pixel *src = (pixel *)_src;
    ptrdiff_t srcstride = _srcstride / sizeof(pixel);
    pixel *dst          = (pixel *)_dst;
    ptrdiff_t dststride = _dststride / sizeof(pixel);
    const int8_t *filter = ff_hevc_epel_filters[mx - 1];
    int16_t tmp_array[(MAX_PB_SIZE + EPEL_EXTRA) * MAX_PB_SIZE];
    int16_t *tmp = tmp_array;
    int shift = denom + 14 - BIT_DEPTH;
#if BIT_DEPTH < 14
    int offset = 1 << (shift - 1);
#else
    int offset = 0;
#endif

    src -= EPEL_EXTRA_BEFORE * srcstride;

    for (y = 0; y < height + EPEL_EXTRA; y++) {
        for (x = 0; x < width; x++)
            tmp[x] = EPEL_FILTER(src, 1) >> (BIT_DEPTH - 8);
        src += srcstride;
        tmp += MAX_PB_SIZE;
    }

    tmp      = tmp_array + EPEL_EXTRA_BEFORE * MAX_PB_SIZE;
    filter = ff_hevc_epel_filters[my - 1];

    ox     = ox * (1 << (BIT_DEPTH - 8));
    for (y = 0; y < height; y++) {
        for (x = 0; x < width; x++)
            dst[x] = av_clip_pixel((((EPEL_FILTER(tmp, MAX_PB_SIZE) >> 6) * wx + offset) >> shift) + ox);
        tmp += MAX_PB_SIZE;
        dst += dststride;
    }
}

static void FUNC(put_hevc_epel_bi_w_hv)(uint8_t *_dst, ptrdiff_t _dststride, uint8_t *_src, ptrdiff_t _srcstride,
                                        int16_t *src2,
                                        int height, int denom, int wx0, int wx1,
                                        int ox0, int ox1, intptr_t mx, intptr_t my, int width)
{
    int x, y;
    pixel *src = (pixel *)_src;
    ptrdiff_t srcstride = _srcstride / sizeof(pixel);
    pixel *dst          = (pixel *)_dst;
    ptrdiff_t dststride = _dststride / sizeof(pixel);
    const int8_t *filter = ff_hevc_epel_filters[mx - 1];
    int16_t tmp_array[(MAX_PB_SIZE + EPEL_EXTRA) * MAX_PB_SIZE];
    int16_t *tmp = tmp_array;
    int shift = 14 + 1 - BIT_DEPTH;
    int log2Wd = denom + shift - 1;

    src -= EPEL_EXTRA_BEFORE * srcstride;

    for (y = 0; y < height + EPEL_EXTRA; y++) {
        for (x = 0; x < width; x++)
            tmp[x] = EPEL_FILTER(src, 1) >> (BIT_DEPTH - 8);
        src += srcstride;
        tmp += MAX_PB_SIZE;
    }

    tmp      = tmp_array + EPEL_EXTRA_BEFORE * MAX_PB_SIZE;
    filter = ff_hevc_epel_filters[my - 1];

    ox0     = ox0 * (1 << (BIT_DEPTH - 8));
    ox1     = ox1 * (1 << (BIT_DEPTH - 8));
    for (y = 0; y < height; y++) {
        for (x = 0; x < width; x++)
            dst[x] = av_clip_pixel(((EPEL_FILTER(tmp, MAX_PB_SIZE) >> 6) * wx1 + src2[x] * wx0 +
                                    ((ox0 + ox1 + 1) << log2Wd)) >> (log2Wd + 1));
        tmp  += MAX_PB_SIZE;
        dst  += dststride;
        src2 += MAX_PB_SIZE;
    }
}// line zero
#define P3 pix[-4 * xstride]
#define P2 pix[-3 * xstride]
#define P1 pix[-2 * xstride]
#define P0 pix[-1 * xstride]
#define Q0 pix[0 * xstride]
#define Q1 pix[1 * xstride]
#define Q2 pix[2 * xstride]
#define Q3 pix[3 * xstride]

// line three. used only for deblocking decision
#define TP3 pix[-4 * xstride + 3 * ystride]
#define TP2 pix[-3 * xstride + 3 * ystride]
#define TP1 pix[-2 * xstride + 3 * ystride]
#define TP0 pix[-1 * xstride + 3 * ystride]
#define TQ0 pix[0  * xstride + 3 * ystride]
#define TQ1 pix[1  * xstride + 3 * ystride]
#define TQ2 pix[2  * xstride + 3 * ystride]
#define TQ3 pix[3  * xstride + 3 * ystride]

static void FUNC(hevc_loop_filter_luma)(uint8_t *_pix,
                                        ptrdiff_t _xstride, ptrdiff_t _ystride,
                                        int beta, int *_tc,
                                        uint8_t *_no_p, uint8_t *_no_q)
{
    int d, j;
    pixel *pix        = (pixel *)_pix;
    ptrdiff_t xstride = _xstride / sizeof(pixel);
    ptrdiff_t ystride = _ystride / sizeof(pixel);

    beta <<= BIT_DEPTH - 8;

    for (j = 0; j < 2; j++) {
        const int dp0  = abs(P2  - 2 * P1  + P0);
        const int dq0  = abs(Q2  - 2 * Q1  + Q0);
        const int dp3  = abs(TP2 - 2 * TP1 + TP0);
        const int dq3  = abs(TQ2 - 2 * TQ1 + TQ0);
        const int d0   = dp0 + dq0;
        const int d3   = dp3 + dq3;
        const int tc   = _tc[j]   << (BIT_DEPTH - 8);
        const int no_p = _no_p[j];
        const int no_q = _no_q[j];

        if (d0 + d3 >= beta) {
            pix += 4 * ystride;
            continue;
        } else {
            const int beta_3 = beta >> 3;
            const int beta_2 = beta >> 2;
            const int tc25   = ((tc * 5 + 1) >> 1);

            if (abs(P3  -  P0) + abs(Q3  -  Q0) < beta_3 && abs(P0  -  Q0) < tc25 &&
                abs(TP3 - TP0) + abs(TQ3 - TQ0) < beta_3 && abs(TP0 - TQ0) < tc25 &&
                                      (d0 << 1) < beta_2 &&      (d3 << 1) < beta_2) {
                // strong filtering
                const int tc2 = tc << 1;
                for (d = 0; d < 4; d++) {
                    const int p3 = P3;
                    const int p2 = P2;
                    const int p1 = P1;
                    const int p0 = P0;
                    const int q0 = Q0;
                    const int q1 = Q1;
                    const int q2 = Q2;
                    const int q3 = Q3;
                    if (!no_p) {
                        P0 = p0 + av_clip(((p2 + 2 * p1 + 2 * p0 + 2 * q0 + q1 + 4) >> 3) - p0, -tc2, tc2);
                        P1 = p1 + av_clip(((p2 + p1 + p0 + q0 + 2) >> 2) - p1, -tc2, tc2);
                        P2 = p2 + av_clip(((2 * p3 + 3 * p2 + p1 + p0 + q0 + 4) >> 3) - p2, -tc2, tc2);
                    }
                    if (!no_q) {
                        Q0 = q0 + av_clip(((p1 + 2 * p0 + 2 * q0 + 2 * q1 + q2 + 4) >> 3) - q0, -tc2, tc2);
                        Q1 = q1 + av_clip(((p0 + q0 + q1 + q2 + 2) >> 2) - q1, -tc2, tc2);
                        Q2 = q2 + av_clip(((2 * q3 + 3 * q2 + q1 + q0 + p0 + 4) >> 3) - q2, -tc2, tc2);
                    }
                    pix += ystride;
                }
            } else { // normal filtering
                int nd_p = 1;
                int nd_q = 1;
                const int tc_2 = tc >> 1;
                if (dp0 + dp3 < ((beta + (beta >> 1)) >> 3))
                    nd_p = 2;
                if (dq0 + dq3 < ((beta + (beta >> 1)) >> 3))
                    nd_q = 2;

                for (d = 0; d < 4; d++) {
                    const int p2 = P2;
                    const int p1 = P1;
                    const int p0 = P0;
                    const int q0 = Q0;
                    const int q1 = Q1;
                    const int q2 = Q2;
                    int delta0   = (9 * (q0 - p0) - 3 * (q1 - p1) + 8) >> 4;
                    if (abs(delta0) < 10 * tc) {
                        delta0 = av_clip(delta0, -tc, tc);
                        if (!no_p)
                            P0 = av_clip_pixel(p0 + delta0);
                        if (!no_q)
                            Q0 = av_clip_pixel(q0 - delta0);
                        if (!no_p && nd_p > 1) {
                            const int deltap1 = av_clip((((p2 + p0 + 1) >> 1) - p1 + delta0) >> 1, -tc_2, tc_2);
                            P1 = av_clip_pixel(p1 + deltap1);
                        }
                        if (!no_q && nd_q > 1) {
                            const int deltaq1 = av_clip((((q2 + q0 + 1) >> 1) - q1 - delta0) >> 1, -tc_2, tc_2);
                            Q1 = av_clip_pixel(q1 + deltaq1);
                        }
                    }
                    pix += ystride;
                }
            }
        }
    }
}

static void FUNC(hevc_loop_filter_chroma)(uint8_t *_pix, ptrdiff_t _xstride,
                                          ptrdiff_t _ystride, int *_tc,
                                          uint8_t *_no_p, uint8_t *_no_q)
{
    int d, j, no_p, no_q;
    pixel *pix        = (pixel *)_pix;
    ptrdiff_t xstride = _xstride / sizeof(pixel);
    ptrdiff_t ystride = _ystride / sizeof(pixel);

    for (j = 0; j < 2; j++) {
        const int tc = _tc[j] << (BIT_DEPTH - 8);
        if (tc <= 0) {
            pix += 4 * ystride;
            continue;
        }
        no_p = _no_p[j];
        no_q = _no_q[j];

        for (d = 0; d < 4; d++) {
            int delta0;
            const int p1 = P1;
            const int p0 = P0;
            const int q0 = Q0;
            const int q1 = Q1;
            delta0 = av_clip((((q0 - p0) * 4) + p1 - q1 + 4) >> 3, -tc, tc);
            if (!no_p)
                P0 = av_clip_pixel(p0 + delta0);
            if (!no_q)
                Q0 = av_clip_pixel(q0 - delta0);
            pix += ystride;
        }
    }
}

static void FUNC(hevc_h_loop_filter_chroma)(uint8_t *pix, ptrdiff_t stride,
                                            int *tc, uint8_t *no_p,
                                            uint8_t *no_q)
{
    FUNC(hevc_loop_filter_chroma)(pix, stride, sizeof(pixel), tc, no_p, no_q);
}

static void FUNC(hevc_v_loop_filter_chroma)(uint8_t *pix, ptrdiff_t stride,
                                            int *tc, uint8_t *no_p,
                                            uint8_t *no_q)
{
    FUNC(hevc_loop_filter_chroma)(pix, sizeof(pixel), stride, tc, no_p, no_q);
}

static void FUNC(hevc_h_loop_filter_luma)(uint8_t *pix, ptrdiff_t stride,
                                          int beta, int *tc, uint8_t *no_p,
                                          uint8_t *no_q)
{
    FUNC(hevc_loop_filter_luma)(pix, stride, sizeof(pixel),
                                beta, tc, no_p, no_q);
}

static void FUNC(hevc_v_loop_filter_luma)(uint8_t *pix, ptrdiff_t stride,
                                          int beta, int *tc, uint8_t *no_p,
                                          uint8_t *no_q)
{
    FUNC(hevc_loop_filter_luma)(pix, sizeof(pixel), stride,
                                beta, tc, no_p, no_q);
}

#undef P3
#undef P2
#undef P1
#undef P0
#undef Q0
#undef Q1
#undef Q2
#undef Q3

#undef TP3
#undef TP2
#undef TP1
#undef TP0
#undef TQ0
#undef TQ1
#undef TQ2
#undef TQ3

#ifdef SVC_EXTENSION


#define LumVer_FILTER(pel, coeff) \
(pel[0]*coeff[0] + pel[1]*coeff[1] + pel[2]*coeff[2] + pel[3]*coeff[3] + pel[4]*coeff[4] + pel[5]*coeff[5] + pel[6]*coeff[6] + pel[7]*coeff[7])
#define CroVer_FILTER(pel, coeff) \
(pel[0]*coeff[0] + pel[1]*coeff[1] + pel[2]*coeff[2] + pel[3]*coeff[3])
#define CroVer_FILTER1(pel, coeff, widthEL) \
(pel[0]*coeff[0] + pel[widthEL]*coeff[1] + pel[widthEL*2]*coeff[2] + pel[widthEL*3]*coeff[3])
#define LumVer_FILTER1(pel, coeff, width) \
(pel[0]*coeff[0] + pel[width]*coeff[1] + pel[width*2]*coeff[2] + pel[width*3]*coeff[3] + pel[width*4]*coeff[4] + pel[width*5]*coeff[5] + pel[width*6]*coeff[6] + pel[width*7]*coeff[7])

// Define the function for up-sampling
#define LumHor_FILTER_Block(pel, coeff) \
(pel[-3]*coeff[0] + pel[-2]*coeff[1] + pel[-1]*coeff[2] + pel[0]*coeff[3] + pel[1]*coeff[4] + pel[2]*coeff[5] + pel[3]*coeff[6] + pel[4]*coeff[7])
#define CroHor_FILTER_Block(pel, coeff) \
(pel[-1]*coeff[0] + pel[0]*coeff[1] + pel[1]*coeff[2] + pel[2]*coeff[3])
#define LumVer_FILTER_Block(pel, coeff, width) \
(pel[-3*width]*coeff[0] + pel[-2*width]*coeff[1] + pel[-width]*coeff[2] + pel[0]*coeff[3] + pel[width]*coeff[4] + pel[2*width]*coeff[5] + pel[3*width]*coeff[6] + pel[4*width]*coeff[7])
#define CroVer_FILTER_Block(pel, coeff, width) \
(pel[-width]*coeff[0] + pel[0]*coeff[1] + pel[width]*coeff[2] + pel[2*width]*coeff[3])
#define LumHor_FILTER(pel, coeff) \
(pel[0]*coeff[0] + pel[1]*coeff[1] + pel[2]*coeff[2] + pel[3]*coeff[3] + pel[4]*coeff[4] + pel[5]*coeff[5] + pel[6]*coeff[6] + pel[7]*coeff[7])
#define CroHor_FILTER(pel, coeff) \
(pel[0]*coeff[0] + pel[1]*coeff[1] + pel[2]*coeff[2] + pel[3]*coeff[3])

/*      ------- Spatial horizontal upsampling filter  --------    */
static void FUNC(upsample_filter_block_luma_h_all)( int16_t *_dst, ptrdiff_t _dststride, uint8_t *_src, ptrdiff_t _srcstride,
                                        int x_EL, int x_BL, int block_w, int block_h, int widthEL,
                                        const struct HEVCWindow *Enhscal, struct UpsamplInf *up_info/*, int y_BL, short * buffer_frame*/) {
    int rightEndL  = widthEL - Enhscal->right_offset;
    int leftStartL = Enhscal->left_offset;
    int x, i, j, phase, refPos16, refPos, shift = up_info->shift_up[0];
    int16_t*   dst_tmp;
    pixel*   src_tmp, *src = (pixel *) _src;
    const int8_t*   coeff;
    //short * srcY1;

    for( i = 0; i < block_w; i++ )	{
        x        = av_clip_c(i+x_EL, leftStartL, rightEndL);
        refPos16 = (((x - leftStartL)*up_info->scaleXLum - up_info->addXLum) >> 12);
        phase    = refPos16 & 15;
        //printf("x %d phase %d \n", x, phase);
        coeff    = up_sample_filter_luma[phase];
        refPos   = (refPos16 >> 4) - x_BL;
        dst_tmp  = _dst  + i;
        src_tmp  = src   + refPos;
       // srcY1 = buffer_frame + y_BL*widthEL+ x_EL+i;
        for( j = 0; j < block_h ; j++ ) {
            *dst_tmp  = (LumHor_FILTER_Block(src_tmp, coeff)>>shift);
            //if(*srcY1 != *dst_tmp)
                //printf("--- %d %d %d %d %d %d %d %d %d \n",refPos, i, j, *srcY1, *dst_tmp, src_tmp[-3], src_tmp[-2], src_tmp[-1], src_tmp[0]);
            src_tmp  += _srcstride;
            dst_tmp  += _dststride;
            //srcY1    += widthEL;
        }
    }
}

static void FUNC(upsample_filter_block_luma_h_all_8)( int16_t *_dst, ptrdiff_t _dststride, uint8_t *_src, ptrdiff_t _srcstride,
                                        int x_EL, int x_BL, int block_w, int block_h, int widthEL,
                                        const struct HEVCWindow *Enhscal, struct UpsamplInf *up_info/*, int y_BL, short * buffer_frame*/) {
    int rightEndL  = widthEL - Enhscal->right_offset;
    int leftStartL = Enhscal->left_offset;
    int x, i, j, phase, refPos16, refPos, shift = up_info->shift_up[0];
    int16_t*   dst_tmp;
    uint8_t*   src_tmp, *src = (uint8_t *) _src;
    const int8_t*   coeff;
    //short * srcY1;

    for( i = 0; i < block_w; i++ )	{
        x        = av_clip_c(i+x_EL, leftStartL, rightEndL);
        refPos16 = (((x - leftStartL)*up_info->scaleXLum - up_info->addXLum) >> 12);
        phase    = refPos16 & 15;
        //printf("x %d phase %d \n", x, phase);
        coeff    = up_sample_filter_luma[phase];
        refPos   = (refPos16 >> 4) - x_BL;
        dst_tmp  = _dst  + i;
        src_tmp  = src   + refPos;
       // srcY1 = buffer_frame + y_BL*widthEL+ x_EL+i;
        for( j = 0; j < block_h ; j++ ) {
            *dst_tmp  = (LumHor_FILTER_Block(src_tmp, coeff)>>shift);
            //if(*srcY1 != *dst_tmp)
                //printf("--- %d %d %d %d %d %d %d %d %d \n",refPos, i, j, *srcY1, *dst_tmp, src_tmp[-3], src_tmp[-2], src_tmp[-1], src_tmp[0]);
            src_tmp  += _srcstride;
            dst_tmp  += _dststride;
            //srcY1    += widthEL;
        }
    }
}

static void FUNC(upsample_filter_block_cr_h_all)(  int16_t *dst, ptrdiff_t dststride, uint8_t *_src, ptrdiff_t _srcstride,
                                                 int x_EL, int x_BL, int block_w, int block_h, int widthEL,
                                                 const struct HEVCWindow *Enhscal, struct UpsamplInf *up_info) {
    int leftStartC = Enhscal->left_offset>>1;
    int rightEndC  = widthEL - (Enhscal->right_offset>>1);
    int x, i, j, phase, refPos16, refPos, shift = up_info->shift_up[1];
    int16_t*  dst_tmp;
    pixel*   src_tmp, *src = (pixel *) _src;
    const int8_t*  coeff;
    
    for( i = 0; i < block_w; i++ )	{
        x        = av_clip_c(i+x_EL, leftStartC, rightEndC);
        refPos16 = (((x - leftStartC)*up_info->scaleXCr - up_info->addXCr) >> 12);
        phase    = refPos16 & 15;
        coeff    = up_sample_filter_chroma[phase];
        refPos   = (refPos16 >> 4) - (x_BL);
        dst_tmp  = dst  + i;
        src_tmp  = src + refPos;
        for( j = 0; j < block_h ; j++ ) {
            *dst_tmp   =  (CroHor_FILTER_Block(src_tmp, coeff)>>shift);
            src_tmp  +=  _srcstride;
            dst_tmp   +=  dststride;
        }
    }
}

static void FUNC(upsample_filter_block_cr_h_all_8)(  int16_t *dst, ptrdiff_t dststride, uint8_t *_src, ptrdiff_t _srcstride,
                                                 int x_EL, int x_BL, int block_w, int block_h, int widthEL,
                                                 const struct HEVCWindow *Enhscal, struct UpsamplInf *up_info) {
    int leftStartC = Enhscal->left_offset>>1;
    int rightEndC  = widthEL - (Enhscal->right_offset>>1);
    int x, i, j, phase, refPos16, refPos, shift = up_info->shift_up[1];
    int16_t*  dst_tmp;
    uint8_t*   src_tmp, *src = (uint8_t *) _src;
    const int8_t*  coeff;

    for( i = 0; i < block_w; i++ )	{
        x        = av_clip_c(i+x_EL, leftStartC, rightEndC);
        refPos16 = (((x - leftStartC)*up_info->scaleXCr - up_info->addXCr) >> 12);
        phase    = refPos16 & 15;
        coeff    = up_sample_filter_chroma[phase];
        refPos   = (refPos16 >> 4) - (x_BL);
        dst_tmp  = dst  + i;
        src_tmp  = src + refPos;
        for( j = 0; j < block_h ; j++ ) {
            *dst_tmp   =  (CroHor_FILTER_Block(src_tmp, coeff)>>shift);
            src_tmp  +=  _srcstride;
            dst_tmp   +=  dststride;
        }
    }
}

/*      ------- Spatial vertical upsampling filter  --------    */
static void FUNC(upsample_filter_block_luma_v_all)( uint8_t *_dst, ptrdiff_t _dststride, int16_t *_src, ptrdiff_t _srcstride,
                                                   int y_BL, int x_EL, int y_EL, int block_w, int block_h, int widthEL, int heightEL,
                                                   const struct HEVCWindow *Enhscal, struct UpsamplInf *up_info) {
    int topStartL  = Enhscal->top_offset;
    int bottomEndL = heightEL - Enhscal->bottom_offset;
    int rightEndL  = widthEL - Enhscal->right_offset;
    int leftStartL = Enhscal->left_offset;
    _dststride /= sizeof(pixel);
    int y, i, j, phase, refPos16, refPos;
    const int8_t  *   coeff;
    uint16_t *dst_tmp, *dst    = (uint16_t *)_dst;
    int16_t *   src_tmp;
    for( j = 0; j < block_h; j++ )	{
    	y        =   av_clip_c(y_EL+j, topStartL, bottomEndL-1);
    	refPos16 = ((( y - topStartL )* up_info->scaleYLum - up_info->addYLum) >> 12);
        phase    = refPos16 & 15;
        coeff    = up_sample_filter_luma[phase];
        refPos   = (refPos16 >> 4) - y_BL;
        src_tmp  = _src  + refPos  * _srcstride;
        dst_tmp  =  dst  + (y_EL+j) * _dststride + x_EL;
        for( i = 0; i < block_w; i++ )	{
            *dst_tmp = av_clip_pixel( (LumVer_FILTER_Block(src_tmp, coeff, _srcstride) + I_OFFSET) >> (N_SHIFT));

           /* uint8_t dst_tmp0;
            dst_tmp0 = av_clip_pixel( (LumVer_FILTER_Block(src_tmp, coeff, _srcstride) + I_OFFSET) >> (N_SHIFT));
            if(dst_tmp0 != *dst_tmp)
                printf("%d %d   --  %d %d \n", j, i, dst_tmp0, *dst_tmp);
            */
            if( ((x_EL+i) >= leftStartL) && ((x_EL+i) <= rightEndL-2) ){
                src_tmp++;
            }
            dst_tmp++;
        }
    }
}

static void FUNC(upsample_filter_block_cr_v_all)( uint8_t *_dst, ptrdiff_t dststride, int16_t *_src, ptrdiff_t _srcstride,
                                                 int y_BL, int x_EL, int y_EL, int block_w, int block_h, int widthEL, int heightEL,
                                                 const struct HEVCWindow *Enhscal, struct UpsamplInf *up_info) {
    int leftStartC = Enhscal->left_offset>>1;
    int rightEndC  = widthEL - (Enhscal->right_offset>>1);
    int topStartC  = Enhscal->top_offset>>1;
    int bottomEndC = heightEL - (Enhscal->bottom_offset>>1);
    int y, i, j, phase, refPos16, refPos;
    const int8_t* coeff;
    int16_t *   src_tmp;
    pixel *dst_tmp, *dst    = (pixel *)_dst;
    dststride /= sizeof(pixel);
    for( j = 0; j < block_h; j++ ) {
        y =   av_clip_c(y_EL+j, topStartC, bottomEndC-1);
        refPos16 = ((( y - topStartC )* up_info->scaleYCr - up_info->addYCr) >> 12); //-4;
        phase    = refPos16 & 15;
        coeff    = up_sample_filter_chroma[phase];
        refPos   = (refPos16>>4) - y_BL;
        src_tmp  = _src  + refPos  * _srcstride;
        dst_tmp  =  dst  + y* dststride + x_EL;
        for( i = 0; i < block_w; i++ )	{
            *dst_tmp = av_clip_pixel( (CroVer_FILTER_Block(src_tmp, coeff, _srcstride) + I_OFFSET) >> (N_SHIFT));
            if( ((x_EL+i) >= leftStartC) && ((x_EL+i) <= rightEndC-2) )
                src_tmp++;
            dst_tmp++;
        }
    }
}

/*      ------- x2 spatial horizontal upsampling filter  --------    */
static void FUNC(upsample_filter_block_luma_h_x2)( int16_t *_dst, ptrdiff_t _dststride, uint8_t *_src, ptrdiff_t _srcstride,
                                                  int x_EL, int x_BL, int block_w, int block_h, int widthEL,
                                                  const struct HEVCWindow *Enhscal, struct UpsamplInf *up_info) {
    //int rightEndL  = widthEL - Enhscal->right_offset;
    int leftStartL = Enhscal->left_offset;
    int x, i, j, shift = up_info->shift_up[0];
    int16_t*   dst_tmp;
    pixel*   src_tmp, *src = (pixel *) _src - x_BL;
    const int8_t*   coeff;
    for( i = 0; i < block_w; i++ )	{
        x        = i+x_EL;  //av_clip_c(i+x_EL, leftStartL, rightEndL);
        coeff    = up_sample_filter_luma_x2[x&0x01];
        dst_tmp  = _dst  + i;
        src_tmp  = src + ((x-leftStartL)>>1);
        for( j = 0; j < block_h ; j++ ) {
            *dst_tmp  = (LumHor_FILTER_Block(src_tmp, coeff)>>shift);
            src_tmp  += _srcstride;
            dst_tmp  += _dststride;
        }
    }
}

static void FUNC(upsample_filter_block_luma_h_x2_8)( int16_t *_dst, ptrdiff_t _dststride, uint8_t *_src, ptrdiff_t _srcstride,
                                                  int x_EL, int x_BL, int block_w, int block_h, int widthEL,
                                                  const struct HEVCWindow *Enhscal, struct UpsamplInf *up_info) {
    //int rightEndL  = widthEL - Enhscal->right_offset;
    int leftStartL = Enhscal->left_offset;
    int x, i, j, shift = up_info->shift_up[0];
    int16_t*   dst_tmp;
    uint8_t*   src_tmp, *src = (uint8_t *) _src - x_BL;
    const int8_t*   coeff;
    for( i = 0; i < block_w; i++ )	{
        x        = i+x_EL;  //av_clip_c(i+x_EL, leftStartL, rightEndL);
        coeff    = up_sample_filter_luma_x2[x&0x01];
        dst_tmp  = _dst  + i;
        src_tmp  = src + ((x-leftStartL)>>1);
        for( j = 0; j < block_h ; j++ ) {
            *dst_tmp  = (LumHor_FILTER_Block(src_tmp, coeff)>>shift);
            src_tmp  += _srcstride;
            dst_tmp  += _dststride;
        }
    }
}

static void FUNC(upsample_filter_block_cr_h_x2)(  int16_t *dst, ptrdiff_t dststride, uint8_t *_src, ptrdiff_t _srcstride,
                                                int x_EL, int x_BL, int block_w, int block_h, int widthEL,
                                                const struct HEVCWindow *Enhscal, struct UpsamplInf *up_info) {
    //int leftStartC = Enhscal->left_offset>>1;
    //int rightEndC  = widthEL - (Enhscal->right_offset>>1);
    int x, i, j, shift = up_info->shift_up[1];
    int16_t*  dst_tmp;
    pixel*   src_tmp, *src = (pixel *) _src - x_BL;
    const int8_t*  coeff;
    
    for( i = 0; i < block_w; i++ )	{
        x        = i+x_EL; //av_clip_c(i+x_EL, leftStartC, rightEndC);
        coeff    = up_sample_filter_chroma_x2_h[x&0x01];
        dst_tmp  = dst  + i;
        src_tmp  = src + (x>>1);
        for( j = 0; j < block_h ; j++ ) {
            *dst_tmp   =  (CroHor_FILTER_Block(src_tmp, coeff)>>shift);
            src_tmp  +=  _srcstride;
            dst_tmp   +=  dststride;
        }
    }
}

static void FUNC(upsample_filter_block_cr_h_x2_8)(  int16_t *dst, ptrdiff_t dststride, uint8_t *_src, ptrdiff_t _srcstride,
                                                int x_EL, int x_BL, int block_w, int block_h, int widthEL,
                                                const struct HEVCWindow *Enhscal, struct UpsamplInf *up_info) {
    //int leftStartC = Enhscal->left_offset>>1;
    //int rightEndC  = widthEL - (Enhscal->right_offset>>1);
    int x, i, j, shift = up_info->shift_up[1];
    int16_t*  dst_tmp;
    uint8_t*   src_tmp, *src = (uint8_t *) _src - x_BL;
    const int8_t*  coeff;

    for( i = 0; i < block_w; i++ )	{
        x        = i+x_EL; //av_clip_c(i+x_EL, leftStartC, rightEndC);
        coeff    = up_sample_filter_chroma_x2_h[x&0x01];
        dst_tmp  = dst  + i;
        src_tmp  = src + (x>>1);
        for( j = 0; j < block_h ; j++ ) {
            *dst_tmp   =  (CroHor_FILTER_Block(src_tmp, coeff)>>shift);
            src_tmp  +=  _srcstride;
            dst_tmp   +=  dststride;
        }
    }
}

/*      ------- x2 spatial vertical upsampling filter  --------    */

static void FUNC(upsample_filter_block_luma_v_x2)( uint8_t *_dst, ptrdiff_t _dststride, int16_t *_src, ptrdiff_t _srcstride,
                                                  int y_BL, int x_EL, int y_EL, int block_w, int block_h, int widthEL, int heightEL,
                                                  const struct HEVCWindow *Enhscal, struct UpsamplInf *up_info) {
    int topStartL  = Enhscal->top_offset;
    //int bottomEndL = heightEL - Enhscal->bottom_offset;
    int rightEndL  = widthEL - Enhscal->right_offset;
    int leftStartL = Enhscal->left_offset;
    int y, i, j;
    const int8_t  *   coeff;
    _dststride /= sizeof(pixel);
    pixel *dst_tmp, *dst    = (pixel *)_dst + y_EL * _dststride + x_EL;
    int16_t *   src_tmp;

    for( j = 0; j < block_h; j++ ) {
    	y        = y_EL+j; //av_clip_c(y_EL+j, topStartL, bottomEndL-1);
        coeff    = up_sample_filter_luma_x2[(y-topStartL)&0x01];

        src_tmp  = _src  + (((y-topStartL)>>1)-y_BL)  * _srcstride;
        dst_tmp  =  dst;
        for( i = 0; i < block_w; i++ )	{
            *dst_tmp = av_clip_pixel( (LumVer_FILTER_Block(src_tmp, coeff, _srcstride) + I_OFFSET) >> (N_SHIFT));
            if( ((x_EL+i) >= leftStartL) && ((x_EL+i) <= rightEndL-2) )
                src_tmp++;
            dst_tmp++;
        }
        dst  +=  _dststride;
    }
}


static void FUNC(upsample_filter_block_cr_v_x2)( uint8_t *_dst, ptrdiff_t dststride, int16_t *_src, ptrdiff_t _srcstride,
                                                int y_BL, int x_EL, int y_EL, int block_w, int block_h, int widthEL, int heightEL,
                                                const struct HEVCWindow *Enhscal, struct UpsamplInf *up_info) {
    int leftStartC = Enhscal->left_offset>>1;
    int rightEndC  = widthEL - (Enhscal->right_offset>>1);
    int topStartC  = Enhscal->top_offset>>1;
    //int bottomEndC = heightEL - (Enhscal->bottom_offset>>1);
    int y, i, j, refPos16, refPos;
    const int8_t* coeff;
    int16_t *   src_tmp;
    pixel *dst_tmp, *dst    = (pixel *)_dst;
    dststride /= sizeof(pixel);
    for( j = 0; j < block_h; j++ ) {
        y =   y_EL+j; //av_clip_c(y_EL+j, topStartC, bottomEndC-1);
        refPos16 = ((( y - topStartC )* up_info->scaleYCr - up_info->addYCr) >> 12); //-4;
        coeff = up_sample_filter_chroma_x2_v[y&0x01];
        refPos   = (refPos16>>4) - y_BL;
        src_tmp  = _src  + refPos  * _srcstride;
        dst_tmp  =  dst  + y* dststride + x_EL;
        for( i = 0; i < block_w; i++ ) {
            *dst_tmp = av_clip_pixel( (CroVer_FILTER_Block(src_tmp, coeff, _srcstride) + I_OFFSET) >> (N_SHIFT));
            if( ((x_EL+i) >= leftStartC) && ((x_EL+i) <= rightEndC-2) )
                src_tmp++;
            dst_tmp++;
        }
    }
}
/*      ------- x1.5 spatial horizontal upsampling filter  --------    */
static void FUNC(upsample_filter_block_luma_h_x1_5)( int16_t *_dst, ptrdiff_t _dststride, uint8_t *_src, ptrdiff_t _srcstride,
                                                  int x_EL, int x_BL, int block_w, int block_h, int widthEL,
                                                  const struct HEVCWindow *Enhscal, struct UpsamplInf *up_info) {
    int rightEndL  = widthEL - Enhscal->right_offset;
    int leftStartL = Enhscal->left_offset;
    int x, i, j, shift = up_info->shift_up[0];
    int16_t*   dst_tmp;
    pixel*   src_tmp, *src = (pixel *) _src - x_BL;
    const int8_t*   coeff;

    for( i = 0; i < block_w; i++ )	{
        x        = av_clip_c(i+x_EL, leftStartL, rightEndL);
        coeff    = up_sample_filter_luma_x1_5[(x-leftStartL)%3];
        dst_tmp  = _dst  + i;
        src_tmp  = src + (((x-leftStartL)<<1)/3);

        for( j = 0; j < block_h ; j++ ) {
            *dst_tmp  = (LumHor_FILTER_Block(src_tmp, coeff)>>shift);
            src_tmp  += _srcstride;
            dst_tmp  += _dststride;
        }
    }
}

static void FUNC(upsample_filter_block_luma_h_x1_5_8)( int16_t *_dst, ptrdiff_t _dststride, uint8_t *_src, ptrdiff_t _srcstride,
                                                  int x_EL, int x_BL, int block_w, int block_h, int widthEL,
                                                  const struct HEVCWindow *Enhscal, struct UpsamplInf *up_info) {
    int rightEndL  = widthEL - Enhscal->right_offset;
    int leftStartL = Enhscal->left_offset;
    int x, i, j, shift = up_info->shift_up[0];
    int16_t*   dst_tmp;
    uint8_t*   src_tmp, *src = (uint8_t *) _src - x_BL;
    const int8_t*   coeff;

    for( i = 0; i < block_w; i++ )	{
        x        = av_clip_c(i+x_EL, leftStartL, rightEndL);
        coeff    = up_sample_filter_luma_x1_5[(x-leftStartL)%3];
        dst_tmp  = _dst  + i;
        src_tmp  = src + (((x-leftStartL)<<1)/3);

        for( j = 0; j < block_h ; j++ ) {
            *dst_tmp  = (LumHor_FILTER_Block(src_tmp, coeff)>>shift);
            src_tmp  += _srcstride;
            dst_tmp  += _dststride;
        }
    }
}

static void FUNC(upsample_filter_block_cr_h_x1_5)(  int16_t *dst, ptrdiff_t dststride, uint8_t *_src, ptrdiff_t _srcstride,
                                                  int x_EL, int x_BL, int block_w, int block_h, int widthEL,
                                                  const struct HEVCWindow *Enhscal, struct UpsamplInf *up_info) {
    int16_t*  dst_tmp;
    int leftStartC = Enhscal->left_offset>>1;
    int rightEndC  = widthEL - (Enhscal->right_offset>>1);
    int x, i, j, shift = up_info->shift_up[1];;
    pixel*   src_tmp, *src = (pixel *) _src - x_BL;
    const int8_t*  coeff;

    for( i = 0; i < block_w; i++ )	{
        x        = av_clip_c(i+x_EL, leftStartC, rightEndC);
        coeff    = up_sample_filter_chroma_x1_5_h[(x-leftStartC)%3];
        dst_tmp  = dst  + i;
        src_tmp  = src + (((x-leftStartC)<<1)/3);
        for( j = 0; j < block_h ; j++ ) {
            *dst_tmp   =  (CroHor_FILTER_Block(src_tmp, coeff)>>shift);
            src_tmp  +=  _srcstride;
            dst_tmp   +=  dststride;
        }
    }
}

static void FUNC(upsample_filter_block_cr_h_x1_5_8)(  int16_t *dst, ptrdiff_t dststride, uint8_t *_src, ptrdiff_t _srcstride,
                                                  int x_EL, int x_BL, int block_w, int block_h, int widthEL,
                                                  const struct HEVCWindow *Enhscal, struct UpsamplInf *up_info) {
    int16_t*  dst_tmp;
    int leftStartC = Enhscal->left_offset>>1;
    int rightEndC  = widthEL - (Enhscal->right_offset>>1);
    int x, i, j, shift = up_info->shift_up[1];;
    uint8_t*   src_tmp, *src = (uint8_t *) _src - x_BL;
    const int8_t*  coeff;

    for( i = 0; i < block_w; i++ )	{
        x        = av_clip_c(i+x_EL, leftStartC, rightEndC);
        coeff    = up_sample_filter_chroma_x1_5_h[(x-leftStartC)%3];
        dst_tmp  = dst  + i;
        src_tmp  = src + (((x-leftStartC)<<1)/3);
        for( j = 0; j < block_h ; j++ ) {
            *dst_tmp   =  (CroHor_FILTER_Block(src_tmp, coeff)>>shift);
            src_tmp  +=  _srcstride;
            dst_tmp   +=  dststride;
        }
    }
}

static void FUNC(upsample_filter_block_luma_v_x1_5)( uint8_t *_dst, ptrdiff_t _dststride, int16_t *_src, ptrdiff_t _srcstride,
                                                    int y_BL, int x_EL, int y_EL, int block_w, int block_h, int widthEL, int heightEL,
                                                    const struct HEVCWindow *Enhscal, struct UpsamplInf *up_info) {
    int topStartL  = Enhscal->top_offset;
    int bottomEndL = heightEL - Enhscal->bottom_offset;
    int rightEndL  = widthEL - Enhscal->right_offset;
    int leftStartL = Enhscal->left_offset;
    int y, i, j;
    const int8_t  *   coeff;
    _dststride /= sizeof(pixel);
    pixel *dst_tmp, *dst    = (pixel *)_dst + x_EL + y_EL * _dststride;
    int16_t *   src_tmp;
    for( j = 0; j < block_h; j++ )	{
    	y        =   av_clip_c(y_EL+j, topStartL, bottomEndL-1);
        coeff    = up_sample_filter_luma_x1_5[(y - topStartL)%3];
        src_tmp  = _src  + ((( y - topStartL )<<1)/3 - y_BL )  * _srcstride;
        dst_tmp  =  dst;
        for( i = 0; i < block_w; i++ )	{
            *dst_tmp = av_clip_pixel( (LumVer_FILTER_Block(src_tmp, coeff, _srcstride) + I_OFFSET) >> (N_SHIFT));
            if( ((x_EL+i) >= leftStartL) && ((x_EL+i) <= rightEndL-2) )
                src_tmp++;
            dst_tmp++;
        }
        dst  += _dststride;
    }
}

static void FUNC(upsample_filter_block_cr_v_x1_5)( uint8_t *_dst, ptrdiff_t dststride, int16_t *_src, ptrdiff_t _srcstride,
                                                  int y_BL, int x_EL, int y_EL, int block_w, int block_h, int widthEL, int heightEL,
                                                  const struct HEVCWindow *Enhscal, struct UpsamplInf *up_info) {
    int leftStartC = Enhscal->left_offset>>1;
    int rightEndC  = widthEL - (Enhscal->right_offset>>1);
    int topStartC  = Enhscal->top_offset>>1;
    int bottomEndC = heightEL - (Enhscal->bottom_offset>>1);
    int y, i, j, refPos16, refPos;
    const int8_t* coeff;
    int16_t *   src_tmp;
    pixel *dst_tmp, *dst    = (pixel *)_dst;
    dststride /= sizeof(pixel);
    for ( j = 0; j < block_h; j++ ) {
        y        = av_clip_c(y_EL+j, topStartC, bottomEndC-1);
        refPos16 = ((( y - topStartC )* up_info->scaleYCr + up_info->addYCr) >> 12)-4;
        coeff    = up_sample_filter_chroma_x1_5_v[y%3];
        refPos   = (refPos16>>4) - y_BL;
        src_tmp  = _src  + refPos  * _srcstride;
        dst_tmp  =  dst  + y* dststride + x_EL;
        for ( i = 0; i < block_w; i++ ) {
            *dst_tmp = av_clip_pixel( (CroVer_FILTER_Block(src_tmp, coeff, _srcstride) + I_OFFSET) >> (N_SHIFT));
            if( ((x_EL+i) >= leftStartC) && ((x_EL+i) <= rightEndC-2) )
                src_tmp++;
            dst_tmp++;
        }
    }
}

static void FUNC(upsample_base_layer_frame)(struct AVFrame *FrameEL, struct AVFrame *FrameBL, short *Buffer[3], const struct HEVCWindow *Enhscal, struct UpsamplInf *up_info, int channel)
{
    int i,j, k;

    int widthBL =  FrameBL->width;
    int heightBL = FrameBL->height;
    int strideBL = FrameBL->linesize[0]/sizeof(pixel);
    int widthEL =  FrameEL->width - Enhscal->left_offset - Enhscal->right_offset;
    int heightEL = FrameEL->height - Enhscal->top_offset - Enhscal->bottom_offset;
    int strideEL = FrameEL->linesize[0]/sizeof(pixel);
    pixel *srcBufY = (pixel*)FrameBL->data[0];
    pixel *dstBufY = (pixel*)FrameEL->data[0];
    short *tempBufY = Buffer[0];
    pixel *srcY;
    pixel *dstY;
    short *dstY1;
    short *srcY1;
    pixel *srcBufU = (pixel*)FrameBL->data[1];
    pixel *dstBufU = (pixel*)FrameEL->data[1];
    short *tempBufU = Buffer[1];
    pixel *srcU;
    pixel *dstU;
    short *dstU1;
    short *srcU1;
    
    pixel *srcBufV = (pixel*)FrameBL->data[2];
    pixel *dstBufV = (pixel*)FrameEL->data[2];
    short *tempBufV = Buffer[2];
    pixel *srcV;
    pixel *dstV;
    short *dstV1;
    short *srcV1;
    
    int refPos16 = 0;
    int phase    = 0;
    int refPos   = 0;
    const int8_t* coeff;
    int leftStartL = Enhscal->left_offset;
    int rightEndL  = FrameEL->width - Enhscal->right_offset;
    //int topStartL  = Enhscal->top_offset;
    //int bottomEndL = FrameEL->height - Enhscal->bottom_offset;
    pixel buffer[8];

    const int nShift = 20-BIT_DEPTH;// TO DO ass the appropiate bit depth  bit  depth

    int iOffset = 1 << (nShift - 1);
    short buffer1[8];

    int leftStartC = Enhscal->left_offset>>1;
    //int rightEndC  = (FrameEL->width>>1) - (Enhscal->right_offset>>1);
    int topStartC  = Enhscal->top_offset>>1;
    //int bottomEndC = (FrameEL->height>>1) - (Enhscal->bottom_offset>>1);
    int shift1 = up_info->shift_up[0];

    widthEL   = FrameEL->width;  //pcUsPic->getWidth ();
    heightEL  = FrameEL->height; //pcUsPic->getHeight();

    widthBL   = FrameBL->width;
    heightBL  = FrameBL->height <= heightEL ? FrameBL->height:heightEL;  // min( FrameBL->height, heightEL);

    for( i = 0; i < widthEL; i++ ) {
        int x = i; //av_clip_c(i, leftStartL, rightEndL);
        refPos16 = ((x *up_info->scaleXLum - up_info->addXLum) >> 12);
        phase    = refPos16 & 15;
        refPos   = refPos16 >> 4;
        coeff = up_sample_filter_luma[phase];
        refPos -= ((NTAPS_LUMA>>1) - 1);
        srcY = srcBufY + refPos;
        dstY1 = tempBufY + i;
        if(refPos < 0)
            for( j = 0; j < heightBL ; j++ ) {
                for(k=0; k<-refPos; k++ )
                  buffer[k] = srcY[-refPos];
                for(k=-refPos; k<NTAPS_LUMA; k++ )
                  buffer[k] = srcY[k];
                *dstY1 = LumHor_FILTER(buffer, coeff)>>shift1;
                srcY += strideBL;
                dstY1 += widthEL;//strideEL;
            } else if (refPos+8 > widthBL )
                for ( j = 0; j < heightBL ; j++ ) {
                    memcpy(buffer, srcY, (widthBL-refPos)*sizeof(pixel));
                    for(k=widthBL-refPos; k<NTAPS_LUMA; k++ )
                      buffer[k] = srcY[widthBL-refPos-1];
                    *dstY1 = LumHor_FILTER(buffer, coeff)>>shift1;
                    srcY += strideBL;
                    dstY1 += widthEL;//strideEL;
                } else
                    for ( j = 0; j < heightBL ; j++ ) {
                      *dstY1 = LumHor_FILTER(srcY, coeff)>>shift1;
                      srcY  += strideBL;
                      dstY1 += widthEL;//strideEL;
                    }
    }
    for ( j = 0; j < heightEL; j++ ) {
        int y = j; //av_clip_c(j, topStartL, bottomEndL-1);
        refPos16 = (( y *up_info->scaleYLum - up_info->addYLum) >> 12);

        phase    = refPos16 & 15;
        refPos   = refPos16 >> 4;
        coeff = up_sample_filter_luma[phase];
        refPos -= ((NTAPS_LUMA>>1) - 1);
        srcY1 = tempBufY + refPos *widthEL;
        dstY = dstBufY + j * strideEL;
        if (refPos < 0)
            for ( i = 0; i < widthEL; i++ ) {
                
                for(k= 0; k<-refPos ; k++)
                    buffer1[k] = srcY1[-refPos*widthEL]; //srcY1[(-refPos+k)*strideEL];
                for(k= 0; k<8+refPos ; k++)
                    buffer1[-refPos+k] = srcY1[(-refPos+k)*widthEL];
                *dstY = av_clip_pixel( (LumVer_FILTER(buffer1, coeff) + iOffset) >> (nShift));

                if( (i >= leftStartL) && (i <= rightEndL-2) )
                    srcY1++;
                dstY++;
            } else if(refPos+8 > heightBL )
                for( i = 0; i < widthEL; i++ ) {
                    for(k= 0; k<heightBL-refPos ; k++)
                        buffer1[k] = srcY1[k*widthEL];
                    for(k= 0; k<8-(heightBL-refPos) ; k++)
                        buffer1[heightBL-refPos+k] = srcY1[(heightBL-refPos-1)*widthEL];
                    *dstY = av_clip_pixel( (LumVer_FILTER(buffer1, coeff) + iOffset) >> (nShift));
                    srcY1++;
                    dstY++;
                } else
                    for ( i = 0; i < widthEL; i++ ) {
                        *dstY = av_clip_pixel( (LumVer_FILTER1(srcY1, coeff, widthEL) + iOffset) >> (nShift));
                        srcY1++;
                        dstY++;
                    }
    }
    widthBL   = FrameBL->width;
    heightBL  = FrameBL->height;
    
    widthEL   = FrameEL->width - Enhscal->right_offset - Enhscal->left_offset;
    heightEL  = FrameEL->height - Enhscal->top_offset - Enhscal->bottom_offset;
    
    shift1 = up_info->shift_up[1];

    widthEL  >>= 1;
    heightEL >>= 1;
    widthBL  >>= 1;
    heightBL >>= 1;
    strideBL  = FrameBL->linesize[1]/sizeof(pixel);
    strideEL  = FrameEL->linesize[1]/sizeof(pixel);
    widthEL   = FrameEL->width >> 1;
    heightEL  = FrameEL->height >> 1;
    widthBL   = FrameBL->width >> 1;
    heightBL  = FrameBL->height > heightEL ? FrameBL->height:heightEL;
    
    
    heightBL >>= 1;
    
    //========== horizontal upsampling ===========
    for( i = 0; i < widthEL; i++ )	{
        int x = i; //av_clip_c(i, leftStartC, rightEndC - 1);
        refPos16 = (((x - leftStartC)*up_info->scaleXCr - up_info->addXCr) >> 12);
        phase    = refPos16 & 15;
        refPos   = refPos16 >> 4;
        coeff = up_sample_filter_chroma[phase];

        refPos -= ((NTAPS_CHROMA>>1) - 1);
        srcU = srcBufU + refPos; // -((NTAPS_CHROMA>>1) - 1);
        srcV = srcBufV + refPos; // -((NTAPS_CHROMA>>1) - 1);
        dstU1 = tempBufU + i;
        dstV1 = tempBufV + i;
        
        if(refPos < 0)
            for( j = 0; j < heightBL ; j++ ) {
                for(k=0; k < -refPos; k++)
                  buffer[k] = srcU[-refPos];
                for(k=-refPos; k < 4; k++)
                  buffer[k] = srcU[k];
                for(k=0; k < -refPos; k++)
                  buffer[k+4] = srcV[-refPos];
                for(k=-refPos; k < 4; k++)
                  buffer[k+4] = srcV[k];

                *dstU1 = CroHor_FILTER(buffer, coeff)>>shift1;
                *dstV1 = CroHor_FILTER((buffer+4), coeff)>>shift1;

                srcU += strideBL;
                srcV += strideBL;
                dstU1 += widthEL;
                dstV1 += widthEL;
            }else if(refPos+4 > widthBL )
                for( j = 0; j < heightBL ; j++ ) {
                    for(k=0; k < widthBL-refPos; k++)
                      buffer[k] = srcU[k];
                    for(k=0; k < 4-(widthBL-refPos); k++)
                      buffer[widthBL-refPos+k] = srcU[widthBL-refPos-1];

                    for(k=0; k < widthBL-refPos; k++)
                      buffer[k+4] = srcV[k];
                    for(k=0; k < 4-(widthBL-refPos); k++)
                      buffer[widthBL-refPos+k+4] = srcV[widthBL-refPos-1];

                    *dstU1 = CroHor_FILTER(buffer, coeff)>>shift1;
                    *dstV1 = CroHor_FILTER((buffer+4), coeff)>>shift1;
                    srcU += strideBL;
                    srcV += strideBL;
                    dstU1 += widthEL;
                    dstV1 += widthEL;
                } else
                    for ( j = 0; j < heightBL ; j++ ) {
                        *dstU1 = CroHor_FILTER(srcU, coeff)>>shift1;
                        *dstV1 = CroHor_FILTER(srcV, coeff)>>shift1;
                        srcU  += strideBL;
                        srcV  += strideBL;
                        dstU1 += widthEL;
                        dstV1 += widthEL;
                    }
    }
    for( j = 0; j < heightEL; j++ )	{
        int y = j; //av_clip_c(j, topStartC, bottomEndC - 1);
        refPos16 = (((y - topStartC)*up_info->scaleYCr - up_info->addYCr) >> 12); // - 4;
        phase    = refPos16 & 15;
        refPos   = refPos16 >> 4;
         
        coeff   = up_sample_filter_chroma[phase];
        refPos -= ((NTAPS_CHROMA>>1) - 1);
        srcU1   = tempBufU  + refPos *widthEL;
        srcV1   = tempBufV  + refPos *widthEL;
        dstU    = dstBufU + j*strideEL;
        dstV    = dstBufV + j*strideEL;
        if (refPos < 0)
            for ( i = 0; i < widthEL; i++ ) {
                for(k= 0; k<-refPos ; k++){
                    buffer1[k] = srcU1[(-refPos)*widthEL];
                    buffer1[k+4] = srcV1[(-refPos)*widthEL];
                }
                for(k= 0; k<4+refPos ; k++){
                    buffer1[-refPos+k] = srcU1[(-refPos+k)*widthEL];
                    buffer1[-refPos+k+4] = srcV1[(-refPos+k)*widthEL];
                }

                *dstU = av_clip_pixel( (CroVer_FILTER(buffer1, coeff) + iOffset) >> (nShift));
                *dstV = av_clip_pixel( (CroVer_FILTER((buffer1+4), coeff) + iOffset) >> (nShift));

                srcU1++;
                srcV1++;
                dstU++;
                dstV++;
            } else if(refPos+4 > heightBL )
                for( i = 0; i < widthEL; i++ ) {
                    for (k= 0; k< heightBL-refPos ; k++) {
                        buffer1[k] = srcU1[k*widthEL];
                        buffer1[k+4] = srcV1[k*widthEL];
                    }
                    for (k= 0; k<4-(heightBL-refPos) ; k++) {
                        buffer1[heightBL-refPos+k] = srcU1[(heightBL-refPos-1)*widthEL];
                        buffer1[heightBL-refPos+k+4] = srcV1[(heightBL-refPos-1)*widthEL];
                    }

                    *dstU = av_clip_pixel( (CroVer_FILTER(buffer1, coeff) + iOffset) >> (nShift));
                    *dstV = av_clip_pixel( (CroVer_FILTER((buffer1+4), coeff) + iOffset) >> (nShift));
                    srcU1++;
                    srcV1++;

                    dstU++;
                    dstV++;
                } else
                    for ( i = 0; i < widthEL; i++ ) {
                        *dstU = av_clip_pixel( (CroVer_FILTER1(srcU1, coeff, widthEL) + iOffset) >> (nShift));
                        *dstV = av_clip_pixel( (CroVer_FILTER1(srcV1, coeff, widthEL) + iOffset) >> (nShift));

                        srcU1++;
                        srcV1++;
                        dstU++;
                        dstV++;
                    }
    }
}

#undef LumHor_FILTER
#undef LumCro_FILTER
#undef LumVer_FILTER
#undef CroVer_FILTER

static void FUNC(colorMapping)(void * pc3DAsymLUT_, struct AVFrame *src, struct AVFrame *dst) {

    TCom3DAsymLUT *pc3DAsymLUT = (TCom3DAsymLUT *)pc3DAsymLUT_;

    int i, j, k;

    const int width  = src->width;
    const int height = src->height;

    const int src_stride  = src->linesize[0]/sizeof(pixel);
    const int src_stridec = src->linesize[1]/sizeof(pixel);

    const int dst_stride  = dst->linesize[0]/sizeof(pixel);
    const int dst_stridec = dst->linesize[1]/sizeof(pixel);

    pixel srcYaver, tmpU, tmpV;

    pixel *src_Y = (pixel*)src->data[0];
    pixel *src_U = (pixel*)src->data[1];
    pixel *src_V = (pixel*)src->data[2];

    pixel *dst_Y = (pixel*)dst->data[0];
    pixel *dst_U = (pixel*)dst->data[1];
    pixel *dst_V = (pixel*)dst->data[2];

    pixel *src_U_prev = (pixel*)src->data[1];
    pixel *src_V_prev = (pixel*)src->data[2];

    pixel *src_U_next = (pixel*)src->data[1] + src_stridec;
    pixel *src_V_next = (pixel*)src->data[2] + src_stridec;

    const int octant_depth1 = pc3DAsymLUT->cm_octant_depth == 1 ? 1 : 0;

    const int YShift2Idx = pc3DAsymLUT->YShift2Idx;
    const int UShift2Idx = pc3DAsymLUT->UShift2Idx;
    const int VShift2Idx = pc3DAsymLUT->VShift2Idx;

    const int nAdaptCThresholdU = pc3DAsymLUT->nAdaptCThresholdU;
    const int nAdaptCThresholdV = pc3DAsymLUT->nAdaptCThresholdV;

    const int nMappingOffset = pc3DAsymLUT->nMappingOffset;
    const int nMappingShift  = pc3DAsymLUT->nMappingShift;

    const int iMaxValY = (1 << pc3DAsymLUT->cm_output_luma_bit_depth  ) - 1;
    const int iMaxValC = (1 << pc3DAsymLUT->cm_output_chroma_bit_depth) - 1;

    for(i = 0; i < height; i += 2){
        for(j = 0, k = 0; j < width; j += 2, k++){
            SCuboid rCuboid;
            SYUVP dstUV;
            short a, b;

            int knext = (k == (width >> 1) - 1) ? k : k+1;

            uint16_t val[6], val_dst[6], val_prev[2];

            val[0] = src_Y[j];
            val[1] = src_Y[j+1];

            val[2] = src_Y[j + src_stride];
            val[3] = src_Y[j + src_stride + 1];

            val[4] = src_U[k];
            val[5] = src_V[k];

            srcYaver = (val[0] + val[2] + 1 ) >> 1;;

            val_prev[0]  = src_U_prev[k];
            val_prev[1]  = src_V_prev[k];

            tmpU =  (val_prev[0] + val[4] + (val[4] << 1) + 2 ) >> 2;
            tmpV =  (val_prev[1] + val[5] + (val[5] << 1) + 2 ) >> 2;

            rCuboid = pc3DAsymLUT->S_Cuboid[val[0] >> YShift2Idx]
                    [octant_depth1 ? tmpU >= nAdaptCThresholdU : tmpU >> UShift2Idx]
                    [octant_depth1 ? tmpV >= nAdaptCThresholdV : tmpV >> VShift2Idx];

            val_dst[0] = ((rCuboid.P[0].Y * val[0] + rCuboid.P[1].Y * tmpU
                    + rCuboid.P[2].Y * tmpV + nMappingOffset) >> nMappingShift)
                    + rCuboid.P[3].Y;

            a = src_U[knext] + val[4];
            b = src_V[knext] + val[5];

            tmpU =  ((a << 1) + a + val_prev[0] + src_U_prev[knext] + 4 ) >> 3;
            tmpV =  ((b << 1) + b + val_prev[1] + src_V_prev[knext] + 4 ) >> 3;

            rCuboid = pc3DAsymLUT->S_Cuboid[val[1] >> YShift2Idx]
                    [octant_depth1 ? tmpU >= nAdaptCThresholdU : tmpU >> UShift2Idx]
                    [octant_depth1 ? tmpV >= nAdaptCThresholdV : tmpV >> VShift2Idx];

            val_dst[1] = ((rCuboid.P[0].Y * val[1] + rCuboid.P[1].Y * tmpU
                    + rCuboid.P[2].Y * tmpV + nMappingOffset) >> nMappingShift)
                    + rCuboid.P[3].Y;

            tmpU =  (src_U_next[k] + val[4] + (val[4]<<1) + 2 ) >> 2;
            tmpV =  (src_V_next[k] + val[5] + (val[5]<<1) + 2 ) >> 2;

            rCuboid = pc3DAsymLUT->S_Cuboid[val[2] >> YShift2Idx]
                    [octant_depth1 ? tmpU >= nAdaptCThresholdU : tmpU >> UShift2Idx]
                    [octant_depth1 ? tmpV >= nAdaptCThresholdV : tmpV >> VShift2Idx];

            val_dst[2] = ((rCuboid.P[0].Y * val[2] + rCuboid.P[1].Y * tmpU
                    + rCuboid.P[2].Y * tmpV + nMappingOffset) >> nMappingShift)
                    + rCuboid.P[3].Y;

            tmpU =  ((a << 1) + a + src_U_next[k] + src_U_next[knext] + 4 ) >> 3;
            tmpV =  ((b << 1) + b + src_V_next[k] + src_V_next[knext] + 4 ) >> 3;

            rCuboid = pc3DAsymLUT->S_Cuboid[val[3] >> YShift2Idx]
                    [octant_depth1 ? tmpU >= nAdaptCThresholdU : tmpU >> UShift2Idx]
                    [octant_depth1 ? tmpV >= nAdaptCThresholdV : tmpV >> VShift2Idx];

            val_dst[3] = ((rCuboid.P[0].Y * val[3] + rCuboid.P[1].Y * tmpU
                    + rCuboid.P[2].Y * tmpV + nMappingOffset) >> nMappingShift)
                    + rCuboid.P[3].Y;

            rCuboid = pc3DAsymLUT->S_Cuboid[srcYaver >> YShift2Idx]
                    [octant_depth1 ? val[4] >= nAdaptCThresholdU : val[4] >> UShift2Idx]
                    [octant_depth1 ? val[5] >= nAdaptCThresholdV : val[5] >> VShift2Idx];

            dstUV.Y = 0;

            dstUV.U = ((rCuboid.P[0].U * srcYaver + rCuboid.P[1].U * val[4]
                    + rCuboid.P[2].U * val[5] + nMappingOffset) >> nMappingShift)
                    + rCuboid.P[3].U;

            dstUV.V = ((rCuboid.P[0].V * srcYaver + rCuboid.P[1].V * val[4]
                    + rCuboid.P[2].V * val[5] + nMappingOffset) >> nMappingShift)
                    + rCuboid.P[3].V;

            dst_Y[j]     = av_clip(val_dst[0], 0, iMaxValY);
            dst_Y[j + 1] = av_clip(val_dst[1], 0, iMaxValY);

            dst_Y[j + dst_stride]     = av_clip(val_dst[2] , 0, iMaxValY);
            dst_Y[j + dst_stride + 1] = av_clip(val_dst[3] , 0, iMaxValY);

            dst_U[k] = av_clip(dstUV.U, 0, iMaxValC);
            dst_V[k] = av_clip(dstUV.V, 0, iMaxValC);
        }

        src_Y += src_stride << 1;

        src_U_prev = src_U;
        src_V_prev = src_V;

        src_U = src_U_next;
        src_V = src_V_next;

        if((i < height - 4)){
            src_U_next += src_stridec;
            src_V_next += src_stridec;
        }

        dst_Y += dst_stride << 1;
        dst_U += dst_stridec;
        dst_V += dst_stridec;
    }
}

static void FUNC(map_color_block)(void *pc3DAsymLUT_,
                                   uint8_t *src_y, uint8_t *src_u, uint8_t *src_v,
                                   uint8_t *dst_y, uint8_t *dst_u, uint8_t *dst_v,
                                   int src_stride, int src_stride_c,
                                   int dst_stride, int dst_stride_c,
                                   int dst_width, int dst_height,
                                   int is_bound_r,int is_bound_b, int is_bound_t,
                                   int is_bound_l){

    TCom3DAsymLUT *pc3DAsymLUT = (TCom3DAsymLUT *)pc3DAsymLUT_;

    int i, j, k;

//    const int width  = src->width;
//    const int height = src->height;

//    const int src_stride  = src->linesize[0]/sizeof(pixel);
//    const int src_stridec = src->linesize[1]/sizeof(pixel);

//    const int dst_stride  = dst->linesize[0]/sizeof(pixel);
//    const int dst_stridec = dst->linesize[1]/sizeof(pixel);

    pixel srcYaver, tmpU, tmpV;

    pixel *src_Y = (pixel*)src_y;
    pixel *src_U = (pixel*)src_u;
    pixel *src_V = (pixel*)src_v;

    pixel *dst_Y = (pixel*)dst_y;
    pixel *dst_U = (pixel*)dst_u;
    pixel *dst_V = (pixel*)dst_v;

    pixel *src_U_prev;
    pixel *src_V_prev;

    pixel *src_U_next = (pixel*)src_u + src_stride_c;
    pixel *src_V_next = (pixel*)src_v + src_stride_c;

    const int octant_depth1 = pc3DAsymLUT->cm_octant_depth == 1 ? 1 : 0;

    const int YShift2Idx = pc3DAsymLUT->YShift2Idx;
    const int UShift2Idx = pc3DAsymLUT->UShift2Idx;
    const int VShift2Idx = pc3DAsymLUT->VShift2Idx;

    const int nAdaptCThresholdU = pc3DAsymLUT->nAdaptCThresholdU;
    const int nAdaptCThresholdV = pc3DAsymLUT->nAdaptCThresholdV;

    const int nMappingOffset = pc3DAsymLUT->nMappingOffset;
    const int nMappingShift  = pc3DAsymLUT->nMappingShift;

    const int iMaxValY = (1 << pc3DAsymLUT->cm_output_luma_bit_depth  ) - 1;
    const int iMaxValC = (1 << pc3DAsymLUT->cm_output_chroma_bit_depth) - 1;

    if(!is_bound_t){
        src_U_prev = (pixel*)src_u - src_stride_c;
        src_V_prev = (pixel*)src_v - src_stride_c;
    } else {
        src_U_prev = (pixel*)src_u;
        src_V_prev = (pixel*)src_v;
    }

    for(i = 0; i < dst_height; i += 2){
        for(j = 0, k = 0; j < dst_width; j += 2, k++){
            SCuboid rCuboid;
            SYUVP dstUV;
            short a, b;

            int knext = (is_bound_r && (k == (dst_width >> 1) - 1)) ? k : k+1;

            uint16_t val[6], val_dst[6], val_prev[2];

            val[0] = src_Y[j];
            val[1] = src_Y[j+1];

            val[2] = src_Y[j + src_stride];
            val[3] = src_Y[j + src_stride + 1];

            val[4] = src_U[k];
            val[5] = src_V[k];

            srcYaver = (val[0] + val[2] + 1 ) >> 1;;

            val_prev[0]  = src_U_prev[k];
            val_prev[1]  = src_V_prev[k];

            tmpU =  (val_prev[0] + val[4] + (val[4] << 1) + 2 ) >> 2;
            tmpV =  (val_prev[1] + val[5] + (val[5] << 1) + 2 ) >> 2;

            rCuboid = pc3DAsymLUT->S_Cuboid[val[0] >> YShift2Idx]
                    [octant_depth1 ? tmpU >= nAdaptCThresholdU : tmpU >> UShift2Idx]
                    [octant_depth1 ? tmpV >= nAdaptCThresholdV : tmpV >> VShift2Idx];

            val_dst[0] = ((rCuboid.P[0].Y * val[0] + rCuboid.P[1].Y * tmpU
                    + rCuboid.P[2].Y * tmpV + nMappingOffset) >> nMappingShift)
                    + rCuboid.P[3].Y;

            a = src_U[knext] + val[4];
            b = src_V[knext] + val[5];

            tmpU =  ((a << 1) + a + val_prev[0] + src_U_prev[knext] + 4 ) >> 3;
            tmpV =  ((b << 1) + b + val_prev[1] + src_V_prev[knext] + 4 ) >> 3;

            rCuboid = pc3DAsymLUT->S_Cuboid[val[1] >> YShift2Idx]
                    [octant_depth1 ? tmpU >= nAdaptCThresholdU : tmpU >> UShift2Idx]
                    [octant_depth1 ? tmpV >= nAdaptCThresholdV : tmpV >> VShift2Idx];

            val_dst[1] = ((rCuboid.P[0].Y * val[1] + rCuboid.P[1].Y * tmpU
                    + rCuboid.P[2].Y * tmpV + nMappingOffset) >> nMappingShift)
                    + rCuboid.P[3].Y;

            tmpU =  (src_U_next[k] + val[4] + (val[4]<<1) + 2 ) >> 2;
            tmpV =  (src_V_next[k] + val[5] + (val[5]<<1) + 2 ) >> 2;

            rCuboid = pc3DAsymLUT->S_Cuboid[val[2] >> YShift2Idx]
                    [octant_depth1 ? tmpU >= nAdaptCThresholdU : tmpU >> UShift2Idx]
                    [octant_depth1 ? tmpV >= nAdaptCThresholdV : tmpV >> VShift2Idx];

            val_dst[2] = ((rCuboid.P[0].Y * val[2] + rCuboid.P[1].Y * tmpU
                    + rCuboid.P[2].Y * tmpV + nMappingOffset) >> nMappingShift)
                    + rCuboid.P[3].Y;

            tmpU =  ((a << 1) + a + src_U_next[k] + src_U_next[knext] + 4 ) >> 3;
            tmpV =  ((b << 1) + b + src_V_next[k] + src_V_next[knext] + 4 ) >> 3;

            rCuboid = pc3DAsymLUT->S_Cuboid[val[3] >> YShift2Idx]
                    [octant_depth1 ? tmpU >= nAdaptCThresholdU : tmpU >> UShift2Idx]
                    [octant_depth1 ? tmpV >= nAdaptCThresholdV : tmpV >> VShift2Idx];

            val_dst[3] = ((rCuboid.P[0].Y * val[3] + rCuboid.P[1].Y * tmpU
                    + rCuboid.P[2].Y * tmpV + nMappingOffset) >> nMappingShift)
                    + rCuboid.P[3].Y;

            rCuboid = pc3DAsymLUT->S_Cuboid[srcYaver >> YShift2Idx]
                    [octant_depth1 ? val[4] >= nAdaptCThresholdU : val[4] >> UShift2Idx]
                    [octant_depth1 ? val[5] >= nAdaptCThresholdV : val[5] >> VShift2Idx];

            dstUV.Y = 0;

            dstUV.U = ((rCuboid.P[0].U * srcYaver + rCuboid.P[1].U * val[4]
                    + rCuboid.P[2].U * val[5] + nMappingOffset) >> nMappingShift)
                    + rCuboid.P[3].U;

            dstUV.V = ((rCuboid.P[0].V * srcYaver + rCuboid.P[1].V * val[4]
                    + rCuboid.P[2].V * val[5] + nMappingOffset) >> nMappingShift)
                    + rCuboid.P[3].V;

            dst_Y[j]     = av_clip(val_dst[0], 0, iMaxValY);
            dst_Y[j + 1] = av_clip(val_dst[1], 0, iMaxValY);

            dst_Y[j + dst_stride]     = av_clip(val_dst[2] , 0, iMaxValY);
            dst_Y[j + dst_stride + 1] = av_clip(val_dst[3] , 0, iMaxValY);

            dst_U[k] = av_clip(dstUV.U, 0, iMaxValC);
            dst_V[k] = av_clip(dstUV.V, 0, iMaxValC);
        }

        src_Y += src_stride << 1;

        src_U_prev = src_U;
        src_V_prev = src_V;

        src_U = src_U_next;
        src_V = src_V_next;

        if(!is_bound_b || (is_bound_b && (i < dst_height - 4))){
            src_U_next += src_stride_c;
            src_V_next += src_stride_c;
        }

        dst_Y += dst_stride << 1;
        dst_U += dst_stride_c;
        dst_V += dst_stride_c;
    }
}

static void FUNC(map_color_block_8)(void *pc3DAsymLUT_,
                                   uint8_t *src_y, uint8_t *src_u, uint8_t *src_v,
                                   uint8_t *dst_y, uint8_t *dst_u, uint8_t *dst_v,
                                   int src_stride, int src_stride_c,
                                   int dst_stride, int dst_stride_c,
                                   int dst_width, int dst_height,
                                   int is_bound_r,int is_bound_b, int is_bound_t,
                                   int is_bound_l){

    TCom3DAsymLUT *pc3DAsymLUT = (TCom3DAsymLUT *)pc3DAsymLUT_;

    int i, j, k;

//    const int width  = src->width;
//    const int height = src->height;

//    const int src_stride  = src->linesize[0]/sizeof(pixel);
//    const int src_stridec = src->linesize[1]/sizeof(pixel);

//    const int dst_stride  = dst->linesize[0]/sizeof(pixel);
//    const int dst_stridec = dst->linesize[1]/sizeof(pixel);

    pixel srcYaver, tmpU, tmpV;

    uint8_t *src_Y = (uint8_t*)src_y;
    uint8_t *src_U = (uint8_t*)src_u;
    uint8_t *src_V = (uint8_t*)src_v;

    uint16_t *dst_Y = (uint16_t*)dst_y;
    uint16_t *dst_U = (uint16_t*)dst_u;
    uint16_t *dst_V = (uint16_t*)dst_v;

    uint8_t *src_U_prev;
    uint8_t *src_V_prev;

    uint8_t *src_U_next = (uint8_t*)src_u + src_stride_c;
    uint8_t *src_V_next = (uint8_t*)src_v + src_stride_c;

    const int octant_depth1 = pc3DAsymLUT->cm_octant_depth == 1 ? 1 : 0;

    const int YShift2Idx = pc3DAsymLUT->YShift2Idx;
    const int UShift2Idx = pc3DAsymLUT->UShift2Idx;
    const int VShift2Idx = pc3DAsymLUT->VShift2Idx;

    const int nAdaptCThresholdU = pc3DAsymLUT->nAdaptCThresholdU;
    const int nAdaptCThresholdV = pc3DAsymLUT->nAdaptCThresholdV;

    const int nMappingOffset = pc3DAsymLUT->nMappingOffset;
    const int nMappingShift  = pc3DAsymLUT->nMappingShift;

    const int iMaxValY = (1 << pc3DAsymLUT->cm_output_luma_bit_depth  ) - 1;
    const int iMaxValC = (1 << pc3DAsymLUT->cm_output_chroma_bit_depth) - 1;

    if(!is_bound_t){
        src_U_prev = (uint8_t*)src_u - src_stride_c;
        src_V_prev = (uint8_t*)src_v - src_stride_c;
    } else {
        src_U_prev = (uint8_t*)src_u;
        src_V_prev = (uint8_t*)src_v;
    }

    for(i = 0; i < dst_height; i += 2){
        for(j = 0, k = 0; j < dst_width; j += 2, k++){
            SCuboid rCuboid;
            SYUVP dstUV;
            short a, b;

            int knext = (is_bound_r && (k == (dst_width >> 1) - 1)) ? k : k+1;

            uint16_t val[6], val_dst[6], val_prev[2];

            val[0] = src_Y[j];
            val[1] = src_Y[j+1];

            val[2] = src_Y[j + src_stride];
            val[3] = src_Y[j + src_stride + 1];

            val[4] = src_U[k];
            val[5] = src_V[k];

            srcYaver = (val[0] + val[2] + 1 ) >> 1;;

            val_prev[0]  = src_U_prev[k];
            val_prev[1]  = src_V_prev[k];

            tmpU =  (val_prev[0] + val[4] + (val[4] << 1) + 2 ) >> 2;
            tmpV =  (val_prev[1] + val[5] + (val[5] << 1) + 2 ) >> 2;

            rCuboid = pc3DAsymLUT->S_Cuboid[val[0] >> YShift2Idx]
                    [octant_depth1 ? tmpU >= nAdaptCThresholdU : tmpU >> UShift2Idx]
                    [octant_depth1 ? tmpV >= nAdaptCThresholdV : tmpV >> VShift2Idx];

            val_dst[0] = ((rCuboid.P[0].Y * val[0] + rCuboid.P[1].Y * tmpU
                    + rCuboid.P[2].Y * tmpV + nMappingOffset) >> nMappingShift)
                    + rCuboid.P[3].Y;

            a = src_U[knext] + val[4];
            b = src_V[knext] + val[5];

            tmpU =  ((a << 1) + a + val_prev[0] + src_U_prev[knext] + 4 ) >> 3;
            tmpV =  ((b << 1) + b + val_prev[1] + src_V_prev[knext] + 4 ) >> 3;

            rCuboid = pc3DAsymLUT->S_Cuboid[val[1] >> YShift2Idx]
                    [octant_depth1 ? tmpU >= nAdaptCThresholdU : tmpU >> UShift2Idx]
                    [octant_depth1 ? tmpV >= nAdaptCThresholdV : tmpV >> VShift2Idx];

            val_dst[1] = ((rCuboid.P[0].Y * val[1] + rCuboid.P[1].Y * tmpU
                    + rCuboid.P[2].Y * tmpV + nMappingOffset) >> nMappingShift)
                    + rCuboid.P[3].Y;

            tmpU =  (src_U_next[k] + val[4] + (val[4]<<1) + 2 ) >> 2;
            tmpV =  (src_V_next[k] + val[5] + (val[5]<<1) + 2 ) >> 2;

            rCuboid = pc3DAsymLUT->S_Cuboid[val[2] >> YShift2Idx]
                    [octant_depth1 ? tmpU >= nAdaptCThresholdU : tmpU >> UShift2Idx]
                    [octant_depth1 ? tmpV >= nAdaptCThresholdV : tmpV >> VShift2Idx];

            val_dst[2] = ((rCuboid.P[0].Y * val[2] + rCuboid.P[1].Y * tmpU
                    + rCuboid.P[2].Y * tmpV + nMappingOffset) >> nMappingShift)
                    + rCuboid.P[3].Y;

            tmpU =  ((a << 1) + a + src_U_next[k] + src_U_next[knext] + 4 ) >> 3;
            tmpV =  ((b << 1) + b + src_V_next[k] + src_V_next[knext] + 4 ) >> 3;

            rCuboid = pc3DAsymLUT->S_Cuboid[val[3] >> YShift2Idx]
                    [octant_depth1 ? tmpU >= nAdaptCThresholdU : tmpU >> UShift2Idx]
                    [octant_depth1 ? tmpV >= nAdaptCThresholdV : tmpV >> VShift2Idx];

            val_dst[3] = ((rCuboid.P[0].Y * val[3] + rCuboid.P[1].Y * tmpU
                    + rCuboid.P[2].Y * tmpV + nMappingOffset) >> nMappingShift)
                    + rCuboid.P[3].Y;

            rCuboid = pc3DAsymLUT->S_Cuboid[srcYaver >> YShift2Idx]
                    [octant_depth1 ? val[4] >= nAdaptCThresholdU : val[4] >> UShift2Idx]
                    [octant_depth1 ? val[5] >= nAdaptCThresholdV : val[5] >> VShift2Idx];

            dstUV.Y = 0;

            dstUV.U = ((rCuboid.P[0].U * srcYaver + rCuboid.P[1].U * val[4]
                    + rCuboid.P[2].U * val[5] + nMappingOffset) >> nMappingShift)
                    + rCuboid.P[3].U;

            dstUV.V = ((rCuboid.P[0].V * srcYaver + rCuboid.P[1].V * val[4]
                    + rCuboid.P[2].V * val[5] + nMappingOffset) >> nMappingShift)
                    + rCuboid.P[3].V;

            dst_Y[j]     = av_clip(val_dst[0], 0, iMaxValY);
            dst_Y[j + 1] = av_clip(val_dst[1], 0, iMaxValY);

            dst_Y[j + dst_stride]     = av_clip(val_dst[2] , 0, iMaxValY);
            dst_Y[j + dst_stride + 1] = av_clip(val_dst[3] , 0, iMaxValY);

            dst_U[k] = av_clip(dstUV.U, 0, iMaxValC);
            dst_V[k] = av_clip(dstUV.V, 0, iMaxValC);
        }

        src_Y += src_stride << 1;

        src_U_prev = src_U;
        src_V_prev = src_V;

        src_U = src_U_next;
        src_V = src_V_next;

        if(!is_bound_b || (is_bound_b && (i < dst_height - 4))){
            src_U_next += src_stride_c;
            src_V_next += src_stride_c;
        }

        dst_Y += dst_stride << 1;
        dst_U += dst_stride_c;
        dst_V += dst_stride_c;
    }
}
#endif
