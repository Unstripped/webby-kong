/*
 * C transcription of decomp/asm/3390.s (boot, handwritten): Rare's LZSS decoder (asset compressType 2)
 * and a gunzip wrapper.
 *
 * Stream format (from the asm): an MSB-first bit stream read one byte at a time. Per token, one flag bit:
 *   1 -> literal: next 8 bits are a byte; emit it and store it in the ring window.
 *   0 -> match: next `windowBits` bits are a window offset; 0 ends the stream. Otherwise the next
 *        (16 - windowBits) bits are a length L; copy L + 3 bytes from window[(offset + i) & mask].
 * The ring window (`scratch`, 1 << windowBits bytes) is not initialised by the decoder; the write
 * position starts at 1. After the end token the input pointer is rounded up to an even address.
 * The stream is read byte-wise from raw ROM bytes, so no byte swapping applies.
 */
#include "port_internal.h"

void gunzip(u8 **inBufPtr, u8 **outBufPtr, void *huftStorage);

/* Bit-reader state that the asm keeps in $a0 (src), $t3 (current byte) and $t4 (bit mask). */
typedef struct LzBits {
    u8 *src;
    u32 cur;
    u32 mask;
} LzBits;

/* func_800029EC: read bits MSB-first into the positions of `topMask` down to bit 0. */
static u32 lzReadBits(LzBits *b, u32 topMask) {
    u32 v = 0;

    do {
        if (b->mask == 0x80) {
            b->cur = *b->src;
            b->src += 1;
        }
        if (b->cur & b->mask) {
            v |= topMask;
        }
        b->mask >>= 1;
        topMask >>= 1;
        if (b->mask == 0) {
            b->mask = 0x80;
        }
    } while (topMask != 0);
    return v;
}

/*
 * func_800028E0: decode one stream. Its results are the advanced src/dst left in $a0/$a1 (non-ABI),
 * modelled here as the in/out pointers. Shift amounts are masked to 5 bits like MIPS sllv.
 */
static void lzDecode(u8 **srcPtr, u8 **dstPtr, u8 *window, s32 windowBits) {
    LzBits b;
    u8 *dst = *dstPtr;
    u32 winMask = (1u << ((u32)windowBits & 31)) - 1;        /* $s0 */
    u32 lenTop = 1u << ((u32)(15 - windowBits) & 31);       /* $t6 */
    u32 offTop = 1u << ((u32)(windowBits - 1) & 31);        /* $t5 */
    u32 pos = 1;                                            /* $t1 */

    b.src = *srcPtr;
    b.cur = 0;
    b.mask = 0x80;
    for (;;) {
        u32 flag;

        if (b.mask == 0x80) {
            b.cur = *b.src;
            b.src += 1;
        }
        flag = b.cur & b.mask;
        b.mask >>= 1;
        if (b.mask == 0) {
            b.mask = 0x80;
        }
        if (flag != 0) {
            u8 c = (u8)lzReadBits(&b, 0x80);

            *dst++ = c;
            window[pos] = c;
            pos = (pos + 1) & winMask;
        } else {
            u32 off = lzReadBits(&b, offTop);
            s32 last;
            s32 i;

            if (off == 0) {
                break;
            }
            last = (s32)lzReadBits(&b, lenTop) + 2;           /* $t9 */
            i = 0;                                             /* $s7 */
            do {
                u8 c = window[(off + (u32)i) & winMask];
                u32 wr = pos;

                i++;
                pos = (pos + 1) & winMask;
                *dst++ = c;
                window[wr] = c;
            } while (!(last < i));
        }
    }
    if ((u32)b.src & 1) {
        b.src += 1;
    }
    *srcPtr = b.src;
    *dstPtr = dst;
}

/* func_80002790: decode the LZ stream at *inBuf into *outBuf with ring window `scratch` of 1 << flag
 * bytes; advances both pointers (FE40.c dmaReadAssetDecompress, compressType 2). */
void func_80002790(void *inBuf, void *outBuf, void *scratch, s32 flag) {
    lzDecode((u8 **)inBuf, (u8 **)outBuf, (u8 *)scratch, flag);
}

extern u8 *D_80013B00;
extern u8 *D_80013B04;

/*
 * func_80002828: gunzip(&D_80013B00, &D_80013B04, huft) after storing src/dst there. The asm also
 * leaves the advanced pointers in $a0/$a1 (non-ABI); they remain readable in D_80013B00/D_80013B04.
 * No C code references this entry.
 */
void func_80002828(u8 *src, u8 *dst, void *huftStorage) {
    D_80013B00 = src;
    D_80013B04 = dst;
    gunzip(&D_80013B00, &D_80013B04, huftStorage);
}
