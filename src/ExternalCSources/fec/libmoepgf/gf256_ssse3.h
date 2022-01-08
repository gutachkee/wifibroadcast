//
// Created by consti10 on 08.01.22.
//

#ifndef WIFIBROADCAST_GF256_SSE3_H
#define WIFIBROADCAST_GF256_SSE3_H

#include <immintrin.h>
#include "gf256tables285.h"
#include <stdint.h>
#include <iostream>

// also fast on x86

static const uint8_t tl[MOEPGF256_SIZE][16] = MOEPGF256_SHUFFLE_LOW_TABLE;
static const uint8_t th[MOEPGF256_SIZE][16] = MOEPGF256_SHUFFLE_HIGH_TABLE;

void
maddrc256_shuffle_ssse3(uint8_t *region1, const uint8_t *region2,
                        uint8_t constant, size_t length)
{
    uint8_t *end;
    register __m128i t1, t2, m1, m2, in1, in2, out, l, h;

    if (constant == 0)
        return;

    if (constant == 1) {
        xorr_sse2(region1, region2, length);
        return;
    }

    t1 = _mm_loadu_si128((void *)tl[constant]);
    t2 = _mm_loadu_si128((void *)th[constant]);
    m1 = _mm_set1_epi8(0x0f);
    m2 = _mm_set1_epi8(0xf0);

    for (end=region1+length; region1<end; region1+=16, region2+=16) {
        in2 = _mm_load_si128((void *)region2);
        in1 = _mm_load_si128((void *)region1);
        l = _mm_and_si128(in2, m1);
        l = _mm_shuffle_epi8(t1, l);
        h = _mm_and_si128(in2, m2);
        h = _mm_srli_epi64(h, 4);
        h = _mm_shuffle_epi8(t2, h);
        out = _mm_xor_si128(h, l);
        out = _mm_xor_si128(out, in1);
        _mm_store_si128((void *)region1, out);
    }
}

void
mulrc256_shuffle_ssse3(uint8_t *region, uint8_t constant, size_t length)
{
    uint8_t *end;
    register __m128i t1, t2, m1, m2, in, out, l, h;

    if (constant == 0) {
        memset(region, 0, length);
        return;
    }

    if (constant == 1)
        return;

    t1 = _mm_loadu_si128((void *)tl[constant]);
    t2 = _mm_loadu_si128((void *)th[constant]);
    m1 = _mm_set1_epi8(0x0f);
    m2 = _mm_set1_epi8(0xf0);

    for (end=region+length; region<end; region+=16) {
        in = _mm_load_si128((void *)region);
        l = _mm_and_si128(in, m1);
        l = _mm_shuffle_epi8(t1, l);
        h = _mm_and_si128(in, m2);
        h = _mm_srli_epi64(h, 4);
        h = _mm_shuffle_epi8(t2, h);
        out = _mm_xor_si128(h, l);
        _mm_store_si128((void *)region, out);
    }
}

void mulrc256_shuffle_sse3_x(uint8_t *region1,const uint8_t* region2,uint8_t constant, size_t length){
    memcpy(region1,region2,length);
    mulrc256_shuffle_ssse3(region1,constant,length);
}



#endif //WIFIBROADCAST_GF256_SSE3_H