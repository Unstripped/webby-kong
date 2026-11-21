/*
 * naudio RSP audio HLE: executes the game's audio command lists (M_AUDTASK) on the host.
 *
 * Modelled on the exact microcode the game ships (global .text 0x80741310.., IMEM 0x1080..0x1CAF; a
 * disassembly is produced at build/ref/naudio_ucode.txt) together with its DMEM image (the task's
 * ucode_data, D_80760590). Every handler below is a transcription of the ucode's instruction stream: the
 * vector code runs on a small RSP vector-unit model (48-bit accumulators, VCO carries, element selection,
 * byte-exact vector loads/stores) so fixed-point rounding and saturation match the hardware bit for bit.
 *
 * Dispatch: the ucode jumps through the halfword table at DMEM[(w0 >> 23) & 0xFE]. Pristine table:
 *   op   IMEM    handler
 *   0x00 0x10E8  (none: straight to the dispatch loop, without consuming a command slot)
 *   0x01 0x138C  ADPCM       dmem out = (w1 & 0xFFF) + 0x4F0 (state there, samples after), in = ((w1 >> 12) & 0xF) + 0x4F0
 *   0x02 0x1190  CLEARBUFF   16-byte granules, at least one
 *   0x03 0x1A28  ENVMIXER    fixed buffers 0x4F0 -> 0x9D0/0xB40 (dry L/R), 0xCB0/0xE20 (wet L/R), 0x170 bytes
 *   0x04 0x11BC  LOADBUFF
 *   0x05 0x17C0  RESAMPLE    out = 0x4F0 or 0x660 (w1 & 3), 0x170 bytes, 4-tap table at DMEM 0xB0
 *   0x06 0x11FC  SAVEBUFF
 *   0x07 0x1C58  MIXER       (table entries 7 and 8 double as SETLOOP's address word, see opSetloop)
 *   0x08 0x1C58  MIXER
 *   0x09 0x126C  SETVOL      naudio form: parameters at DMEM 0xFE0..0xFF1
 *   0x0A 0x1338  DMEMMOVE    16-byte granules, at least one
 *   0x0B 0x123C  LOADADPCM   book -> DMEM 0x3F0
 *   0x0C 0x1C58  MIXER       0x170 bytes, out += in * gain
 *   0x0D 0x12C4  INTERLEAVE  fixed 0x9D0/0xB40 -> 0x4F0, 0x170 bytes per channel (w0/w1 ignored)
 *   0x0E 0x12B0  table entry 0x02B0: the PC is 12 bits, so this lands inside SETVOL's A_RATE tail and stores
 *                w0, a stale scalar $2 and w1 into DMEM 0xFE0/0xFE2/0xFE4. The pole filter the game means to
 *                run (code at IMEM 0x163C) is unreachable in this ucode.
 *   0x0F 0x1374  SETLOOP     address word at DMEM 0x0E
 * Ops 0x10..0x7F index ucode constants rather than handlers and are reported as unknown, except the port's own
 * 0x7E/0x7F (PORT_AOP_SPATIAL_BEGIN/END, port.h), which game code only emits with 3D sound on.
 *
 * Command lists are fetched 0x140 bytes at a time into DMEM 0x2B0. Op 0x00 (the game's ratio<<16|max
 * commands from n_alAuxBusPull/n_alResamplePull, written for a ucode with a handler there) does not
 * decrement the chunk counter, so each one makes the loop dispatch one extra slot past the chunk while the
 * RDRAM fetch pointer still advances; this is reproduced.
 *
 * Byte order: DMEM holds RDRAM bytes verbatim. Sample data, ADPCM books/state and ucode constants are
 * native s16 halfwords; ADPCM frame bytes are raw ROM bytes. Vector loads/stores therefore address the
 * hardware's big-endian byte b at host offset b ^ 1, except the ADPCM frame fetch, which reads raw bytes.
 */
#include "port_internal.h"
#include "audio/hrtf.h"
#include <string.h>

_Static_assert(__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__, "naudio DMEM model assumes a little-endian host");
_Static_assert(sizeof(OSTask_t) == 0x40, "OSTask layout");

/* Task ucode_data: DMEM image of the jump table and constants (0x000..0x2AF), in N64 byte order
 * (tools/gen_data.py RAW_FILES). Generated from the ROM. */
extern u8 D_80760590[];

#define DMEM_SIZE 0x1000
#define UCODE_DATA_SIZE 0x2B0
#define CMD_BUF 0x2B0
#define CMD_CHUNK 0x140
#define STATE 0xFA0 /* $24 */

static u8 sDmem[DMEM_SIZE] __attribute__((aligned(16))); /* sample buffers are native s16 arrays (hrtf.c) */
static u16 sUcodeData[UCODE_DATA_SIZE / 2];
static int sUcodeReady;
static u32 sR2;          /* scalar $2: read by op 0x0E (stored to DMEM 0xFE2) */
static u8 sLoggedOp[16]; /* one bit per command byte */
static int sLoggedImem, sLoggedOp0;

/* ---------------------------------------------------------------- DMEM ---- */

static inline s16 dmRd16(u32 a) {
    return (s16)(sDmem[a & 0xFFF] | (sDmem[(a + 1) & 0xFFF] << 8));
}

static inline void dmWr16(u32 a, u32 v) {
    sDmem[a & 0xFFF] = (u8)v;
    sDmem[(a + 1) & 0xFFF] = (u8)(v >> 8);
}

/* Hardware (big-endian) DMEM byte at address a. */
static inline u8 hwByte(u32 a) {
    return sDmem[(a & 0xFFF) ^ 1];
}

static inline void hwSetByte(u32 a, u8 b) {
    sDmem[(a & 0xFFF) ^ 1] = b;
}

/* SP DMA. lenReg is the RD_LEN/WR_LEN value: length-1 in bits 0..11 (rounded up to 8 bytes), row count-1
 * in bits 12..19, RDRAM skip in bits 20..31. Address low 3 bits are ignored as on hardware. */
static void spDma(int toRdram, u32 memAddr, u32 dramAddr, u32 lenReg) {
    u32 len = ((lenReg & 0xFFF) | 7) + 1;
    u32 rows = ((lenReg >> 12) & 0xFF) + 1;
    u32 skip = (lenReg >> 20) & 0xFFF;
    u32 mem = memAddr & 0x1FF8;
    u32 dram = dramAddr & 0xFFFFF8;
    u32 r, i;

    if (mem & 0x1000) {
        if (!sLoggedImem) {
            sLoggedImem = 1;
            portLog("PORT: naudio DMA into IMEM ignored (mem 0x%04x)", memAddr);
        }
        return;
    }
    for (r = 0; r < rows; r++) {
        u8 *host = (u8 *)(uintptr_t)(dram & 0xFFFFFF);
        if (mem + len <= 0x1000) { /* no DMEM wrap: one block */
            if (toRdram) {
                memcpy(host, &sDmem[mem], len);
            } else {
                memcpy(&sDmem[mem], host, len);
            }
            mem = (mem + len) & 0xFFF;
        } else {
            for (i = 0; i < len; i++) {
                if (toRdram) {
                    host[i] = sDmem[mem];
                } else {
                    sDmem[mem] = host[i];
                }
                mem = (mem + 1) & 0xFFF;
            }
        }
        dram += len + skip;
    }
}

/* ------------------------------------------------------- vector unit ---- */

static s16 sV[32][8];
static s64 sAcc[8]; /* 48-bit, kept sign-extended */
static u8 sVcoC;    /* VCO carry bits, one per lane */
static u8 sVcoNe;   /* VCO not-equal bits */

/* Element selectors as encoded in the instruction's e field. */
#define EW 0           /* whole vector */
#define EQ(n) (2 + (n)) /* [nq] */
#define EH(n) (4 + (n)) /* [nh] */
#define ES(n) (8 + (n)) /* [n] */
#define SAR_H 8
#define SAR_M 9

static inline int el(int e, int i) {
    if (e < 2) {
        return i;
    }
    if (e < 4) {
        return (i & 6) | (e & 1);
    }
    if (e < 8) {
        return (i & 4) | (e & 3);
    }
    return e & 7;
}

static inline s64 sext48(s64 x) {
    return (s64)((u64)x << 16) >> 16;
}

static inline s16 clampS16(s64 x) {
    return x < -32768 ? -32768 : x > 32767 ? 32767 : (s16)x;
}

/* Result = accumulator bits 31..16 clamped by bits 47..16 (VMULF/VMACF/VMxDH/VMxDM). */
static inline s16 satMid(s64 acc) {
    return clampS16(acc >> 16);
}

/* Result = accumulator bits 15..0, clamped to 0 / 0xFFFF by bits 47..16 (VMxDN/VMUDL). */
static inline s16 satLow(s64 acc) {
    s64 hi = acc >> 16;
    if (hi < -32768) {
        return 0;
    }
    if (hi > 32767) {
        return (s16)0xFFFF;
    }
    return (s16)(u16)acc;
}

static inline void setAccLow(int i, u16 v) {
    sAcc[i] = (sAcc[i] & ~(s64)0xFFFF) | v;
}

enum { M_MULF, M_MACF, M_MUDH, M_MADH, M_MUDN, M_MADN, M_MUDM, M_MADM, M_MUDL };

static void vmul(int kind, int vd, int vs, int vt, int e) {
    s16 out[8];
    int i;
    for (i = 0; i < 8; i++) {
        s32 s = sV[vs][i];
        s32 t = sV[vt][el(e, i)];
        s64 a = sAcc[i];
        switch (kind) {
        case M_MULF:
            a = (s64)s * t * 2 + 0x8000;
            out[i] = satMid(a);
            break;
        case M_MACF:
            a = sext48(a + (s64)s * t * 2);
            out[i] = satMid(a);
            break;
        case M_MUDH:
            a = (s64)s * t * 65536;
            out[i] = satMid(a);
            break;
        case M_MADH:
            a = sext48(a + (s64)s * t * 65536);
            out[i] = satMid(a);
            break;
        case M_MUDN:
            a = (s64)(u16)s * t;
            out[i] = satLow(a);
            break;
        case M_MADN:
            a = sext48(a + (s64)(u16)s * t);
            out[i] = satLow(a);
            break;
        case M_MUDM:
            a = (s64)s * (u16)t;
            out[i] = satMid(a);
            break;
        case M_MADM:
            a = sext48(a + (s64)s * (u16)t);
            out[i] = satMid(a);
            break;
        default: /* M_MUDL */
            a = ((u32)(u16)s * (u32)(u16)t) >> 16;
            out[i] = satLow(a);
            break;
        }
        sAcc[i] = a;
    }
    memcpy(sV[vd], out, sizeof(out));
}

#define vmulf(d, s, t, e) vmul(M_MULF, d, s, t, e)
#define vmacf(d, s, t, e) vmul(M_MACF, d, s, t, e)
#define vmudh(d, s, t, e) vmul(M_MUDH, d, s, t, e)
#define vmadh(d, s, t, e) vmul(M_MADH, d, s, t, e)
#define vmudn(d, s, t, e) vmul(M_MUDN, d, s, t, e)
#define vmadn(d, s, t, e) vmul(M_MADN, d, s, t, e)
#define vmudm(d, s, t, e) vmul(M_MUDM, d, s, t, e)
#define vmadm(d, s, t, e) vmul(M_MADM, d, s, t, e)
#define vmudl(d, s, t, e) vmul(M_MUDL, d, s, t, e)

static void vsar(int vd, int e) {
    int i;
    for (i = 0; i < 8; i++) {
        sV[vd][i] = (s16)(sAcc[i] >> (e == SAR_H ? 32 : e == SAR_M ? 16 : 0));
    }
}

enum { L_AND, L_XOR };

/* vt with the element selector applied: one branch per instruction instead of el()'s per lane */
static inline void vtLanes(int vt, int e, s16 *t) {
    int i;
    if (e < 2) {
        memcpy(t, sV[vt], 16);
        return;
    }
    for (i = 0; i < 8; i++) {
        t[i] = sV[vt][el(e, i)];
    }
}

static void vlogic(int kind, int vd, int vs, int vt, int e) {
    s16 out[8], tv[8];
    int i;
    vtLanes(vt, e, tv);
    for (i = 0; i < 8; i++) {
        u16 s = (u16)sV[vs][i];
        u16 t = (u16)tv[i];
        out[i] = (s16)(kind == L_AND ? (s & t) : (s ^ t));
        setAccLow(i, (u16)out[i]);
    }
    memcpy(sV[vd], out, sizeof(out));
}

#define vand(d, s, t, e) vlogic(L_AND, d, s, t, e)
#define vxor(d, s, t, e) vlogic(L_XOR, d, s, t, e)

static void vadd(int vd, int vs, int vt, int e) {
    s16 out[8], tv[8];
    int i;
    vtLanes(vt, e, tv);
    for (i = 0; i < 8; i++) {
        s32 r = sV[vs][i] + tv[i] + ((sVcoC >> i) & 1);
        setAccLow(i, (u16)r);
        out[i] = clampS16(r);
    }
    memcpy(sV[vd], out, sizeof(out));
    sVcoC = sVcoNe = 0;
}

static void vsub(int vd, int vs, int vt, int e) {
    s16 out[8], tv[8];
    int i;
    vtLanes(vt, e, tv);
    for (i = 0; i < 8; i++) {
        s32 r = sV[vs][i] - tv[i] - ((sVcoC >> i) & 1);
        setAccLow(i, (u16)r);
        out[i] = clampS16(r);
    }
    memcpy(sV[vd], out, sizeof(out));
    sVcoC = sVcoNe = 0;
}

static void vaddc(int vd, int vs, int vt, int e) {
    s16 out[8], tv[8];
    u8 carry = 0;
    int i;
    vtLanes(vt, e, tv);
    for (i = 0; i < 8; i++) {
        u32 r = (u32)(u16)sV[vs][i] + (u16)tv[i];
        carry |= (u8)((r >> 16) << i);
        setAccLow(i, (u16)r);
        out[i] = (s16)(u16)r;
    }
    memcpy(sV[vd], out, sizeof(out));
    sVcoC = carry;
    sVcoNe = 0;
}

/* VGE (ge = 1) / VLT (ge = 0). */
static void vcmp(int ge, int vd, int vs, int vt, int e) {
    s16 out[8], tv[8];
    int i;
    vtLanes(vt, e, tv);
    for (i = 0; i < 8; i++) {
        s16 s = sV[vs][i];
        s16 t = tv[i];
        int both = ((sVcoC >> i) & 1) && ((sVcoNe >> i) & 1);
        int pick = ge ? (s > t || (s == t && !both)) : (s < t || (s == t && both));
        out[i] = pick ? s : t;
        setAccLow(i, (u16)out[i]);
    }
    memcpy(sV[vd], out, sizeof(out));
    sVcoC = sVcoNe = 0;
}

#define vge(d, s, t, e) vcmp(1, d, s, t, e)
#define vlt(d, s, t, e) vcmp(0, d, s, t, e)

static inline u8 vByte(int r, int k) {
    u16 h = (u16)sV[r][(k >> 1) & 7];
    return (k & 1) ? (u8)h : (u8)(h >> 8);
}

static inline void vSetByte(int r, int k, u8 b) {
    u16 *h = (u16 *)&sV[r][(k >> 1) & 7];
    *h = (k & 1) ? (u16)((*h & 0xFF00) | b) : (u16)((*h & 0x00FF) | (b << 8));
}

/* DMEM keeps hardware halfwords native (hwByte's ^1), so with an even element offset and an even address a
 * vector load/store is whole halfword moves instead of byte merges */
static inline s16 dmHalf(u32 a) {
    u16 h;
    memcpy(&h, &sDmem[a & 0xFFE], 2);
    return (s16)h;
}

static inline void dmSetHalf(u32 a, s16 v) {
    memcpy(&sDmem[a & 0xFFE], &v, 2);
}

/* Vector loads/stores: `e` is the element byte offset, `a` the final DMEM address. */
static void lqv(int vt, int e, u32 a) {
    int k, end = e + 16 - (int)(a & 15);
    if (end > 16) {
        end = 16;
    }
    if (((e | (int)a) & 1) == 0) {
        for (k = e; k < end; k += 2, a += 2) {
            sV[vt][k >> 1] = dmHalf(a);
        }
        return;
    }
    for (k = e; k < end; k++) {
        vSetByte(vt, k, hwByte(a++));
    }
}

static void lrv(int vt, int e, u32 a) {
    int k = 16 - (int)((a & 15) - e);
    a &= ~15u;
    if ((k & 1) == 0) {
        for (; k < 16; k += 2, a += 2) {
            sV[vt][k >> 1] = dmHalf(a);
        }
        return;
    }
    for (; k < 16; k++) {
        vSetByte(vt, k, hwByte(a++));
    }
}

static void ldv(int vt, int e, u32 a) {
    int k;
    if (((e | (int)a) & 1) == 0) {
        for (k = e; k < e + 8; k += 2, a += 2) {
            sV[vt][(k & 15) >> 1] = dmHalf(a);
        }
        return;
    }
    for (k = e; k < e + 8; k++) {
        vSetByte(vt, k & 15, hwByte(a++));
    }
}

/* LDV of raw (byte-ordered) data: the ADPCM frame fetch. */
static void ldvRaw(int vt, int e, u32 a) {
    int k;
    for (k = e; k < e + 8; k++) {
        vSetByte(vt, k & 15, sDmem[a++ & 0xFFF]);
    }
}

static void lsv(int vt, int e, u32 a) {
    vSetByte(vt, e, hwByte(a));
    vSetByte(vt, (e + 1) & 15, hwByte(a + 1));
}

static void sqv(int vt, int e, u32 a) {
    int k, end = e + 16 - (int)(a & 15);
    if (((e | (int)a) & 1) == 0) {
        for (k = e; k < end; k += 2, a += 2) {
            dmSetHalf(a, sV[vt][(k & 15) >> 1]);
        }
        return;
    }
    for (k = e; k < end; k++) {
        hwSetByte(a++, vByte(vt, k & 15));
    }
}

static void sdv(int vt, int e, u32 a) {
    int k;
    if (((e | (int)a) & 1) == 0) {
        for (k = e; k < e + 8; k += 2, a += 2) {
            dmSetHalf(a, sV[vt][(k & 15) >> 1]);
        }
        return;
    }
    for (k = e; k < e + 8; k++) {
        hwSetByte(a++, vByte(vt, k & 15));
    }
}

static void ssv(int vt, int e, u32 a) {
    hwSetByte(a, vByte(vt, e));
    hwSetByte(a + 1, vByte(vt, (e + 1) & 15));
}

static void mtc2(int vt, int e, u32 v) {
    vSetByte(vt, e, (u8)(v >> 8));
    if (e != 15) {
        vSetByte(vt, e + 1, (u8)v);
    }
}

static s32 mfc2(int vt, int e) {
    return (s16)((vByte(vt, e) << 8) | vByte(vt, (e + 1) & 15));
}

static void vzero(int vd) {
    vxor(vd, vd, vd, EW);
}

/* ---------------------------------------------------------- handlers ---- */

/* 0x138C */
static void opAdpcm(u32 w0, u32 w1) {
    u32 in = ((w1 >> 12) & 0xF) + 0x4F0; /* $23 */
    u32 data = in + 1;                    /* $22 */
    u32 out = (w1 & 0xFFF) + 0x4F0;       /* $1 */
    s32 count = (w1 >> 16) & 0xFFF;       /* $21 */
    u32 state = w0 & 0xFFFFFF;            /* $20 */
    u32 flags = w1 >> 28;
    u32 book, row;
    s32 shiftCount;
    u8 hdr;
    int k;

    lqv(31, 0, 0x50);
    vzero(25);
    vzero(24);
    vzero(13);
    vzero(14);
    vzero(15);
    vzero(16);
    vzero(17);
    vzero(18);
    vzero(19);
    if (flags & 1) {
        vzero(27);
        ldv(25, 0, 0x20);
        ldv(24, 8, 0x20);
        ldv(23, 0, 0x28);
        ldv(23, 8, 0x28);
        sqv(27, 0, out);
        sqv(27, 0, out + 0x10);
    } else {
        u32 src = state;
        if (flags & 2) {
            src = ((u32)(u16)dmRd16(0x0E) << 16) | (u16)dmRd16(0x10);
        }
        spDma(0, out, src, 0x1F);
        ldv(25, 0, 0x20);
        ldv(24, 8, 0x20);
        ldv(23, 0, 0x28);
        ldv(23, 8, 0x28);
        lqv(27, 0, out + 0x10);
    }
    out += 0x20;
    if (count != 0) {
        /* prologue: frame 0 nibble masks, gain and predictor rows */
        ldvRaw(12, 0, data);
        hdr = sDmem[in & 0xFFF];
        vand(10, 25, 12, ES(0));
        book = ((hdr & 0xF) << 5) + 0x3F0;
        vand(9, 24, 12, ES(1));
        shiftCount = 12 - (hdr >> 4);
        vand(8, 25, 12, ES(2));
        vand(7, 24, 12, ES(3));
        mtc2(22, 0, 0x8000u >> ((u32)(shiftCount - 1) & 31));
        lqv(21, 0, book);
        lqv(20, 0, book + 0x10);
        for (k = 1; k <= 7; k++) {
            lrv(20 - k, 0, book - 2 * k + 0x20);
        }
        do {
            data += 9;
            vmudn(30, 10, 23, EW);
            in += 9;
            vmadn(30, 9, 23, EW);
            hdr = sDmem[in & 0xFFF];
            vmudn(29, 8, 23, EW);
            ldvRaw(12, 0, data);
            vmadn(29, 7, 23, EW);
            if (shiftCount > 0) {
                vmudm(30, 30, 22, ES(0));
                vmudm(29, 29, 22, ES(0));
            }
            /* first eight samples */
            vmudh(11, 21, 27, ES(6));
            vmadh(11, 20, 27, ES(7));
            vmadh(11, 19, 30, ES(0));
            vmadh(11, 18, 30, ES(1));
            vmadh(11, 17, 30, ES(2));
            vmadh(11, 16, 30, ES(3));
            vmadh(28, 15, 30, ES(4));
            vmadh(11, 14, 30, ES(5));
            vmadh(11, 13, 30, ES(6));
            vmadh(11, 30, 31, ES(3));
            book = ((hdr & 0xF) << 5) + 0x3F0;
            shiftCount = 12 - (hdr >> 4);
            vsar(26, SAR_M);
            mtc2(22, 0, 0x8000u >> ((u32)(shiftCount - 1) & 31));
            vsar(28, SAR_H);
            vand(10, 25, 12, ES(0));
            vand(9, 24, 12, ES(1));
            vand(8, 25, 12, ES(2));
            vand(7, 24, 12, ES(3));
            vmudn(11, 26, 31, ES(1));
            vmadh(28, 28, 31, ES(1));
            /* last eight samples; predictor rows for the next frame load as they free up */
            row = book - 2;
            vmudh(11, 19, 29, ES(0));
            vmadh(11, 18, 29, ES(1));
            lrv(19, 0, row + 0x20);
            row -= 2;
            vmadh(11, 17, 29, ES(2));
            vmadh(11, 16, 29, ES(3));
            lrv(18, 0, row + 0x20);
            vmadh(11, 15, 29, ES(4));
            row -= 2;
            vmadh(11, 14, 29, ES(5));
            lrv(17, 0, row + 0x20);
            vmadh(11, 13, 29, ES(6));
            row -= 2;
            vmadh(11, 29, 31, ES(3));
            lrv(16, 0, row + 0x20);
            vmadh(11, 21, 28, ES(6));
            row -= 2;
            vmadh(11, 20, 28, ES(7));
            lrv(15, 0, row + 0x20);
            vsar(26, SAR_M);
            row -= 2;
            vsar(27, SAR_H);
            lrv(14, 0, row + 0x20);
            row -= 2;
            lrv(13, 0, row + 0x20);
            lqv(21, 0, book);
            vmudn(11, 26, 31, ES(1));
            lqv(20, 0, book + 0x10);
            vmadh(27, 27, 31, ES(1));
            count -= 0x20;
            sqv(28, 0, out);
            out += 0x20;
            sqv(27, 0, out - 0x10);
        } while (count > 0);
    }
    out -= 0x20;
    spDma(1, out, state, 0x1F);
    sR2 = state;
}

/* 0x1190 */
static void opClearbuff(u32 w0, u32 w1) {
    s32 n = w1 & 0xFFFF;
    u32 a = (w0 & 0xFFFF) + 0x4F0;
    vzero(1);
    do {
        sdv(1, 0, a);
        sdv(1, 0, a + 8);
        n -= 0x10;
        a += 0x10;
    } while (n > 0);
    sR2 = (u32)n;
}

/* 0x1A28 */
static void opEnvmixer(u32 w0, u32 w1) {
    u32 state = w1 & 0xFFFFFF;
    int init = (w0 >> 16) & 1;
    u32 in = 0x4F0, dryL = 0x9D0, dryR = 0xB40, wetL = 0xCB0, wetR = 0xE20;
    s32 n = 0x170;
    s32 rateL, rateR; /* $23, $22: the sign selects the clamp direction */

    lqv(31, 0, 0x40);
    lqv(10, 0, 0x50);
    lqv(30, 0, 0xA0);
    vzero(0);
    if (!init) {
        spDma(0, STATE, state, 0x4F);
        lqv(20, 0, STATE + 0x00);
        lqv(21, 0, STATE + 0x10);
        lqv(18, 0, STATE + 0x20);
        lqv(19, 0, STATE + 0x30);
    }
    lqv(24, 0, STATE + 0x40);
    rateR = mfc2(24, 8);
    vand(9, 31, 24, ES(6));
    vsub(9, 0, 9, EW);
    vand(8, 31, 24, ES(7));
    vsub(8, 0, 8, EW);
    vxor(8, 8, 9, EW);
    rateL = mfc2(24, 2);
    if (init) {
        vzero(20);
        lsv(20, 14, STATE + 0x50);
        vzero(21);
        lqv(17, 0, in);
        vzero(18);
        mtc2(18, 14, w0);
        vmudl(23, 30, 24, ES(2));
        lqv(29, 0, dryL);
        vmadn(23, 30, 24, ES(1));
        lqv(27, 0, wetL);
        vmadh(20, 31, 20, ES(7));
        lqv(28, 0, dryR);
        vmadn(21, 31, 0, ES(0));
        vzero(19);
        if (rateL >= 0) {
            vlt(20, 20, 24, ES(0));
        } else {
            vge(20, 20, 24, ES(0));
        }
        vxor(17, 9, 17, EW);
        vmudl(23, 30, 24, ES(5));
        lqv(26, 0, wetR);
        vmadn(23, 30, 24, ES(4));
        n -= 0x10;
        in += 0x10;
        vmadh(18, 31, 18, ES(7));
        vmadn(19, 31, 0, ES(0));
        vmulf(16, 20, 24, ES(6));
        vmulf(15, 20, 24, ES(7));
        if (rateR >= 0) {
            vlt(18, 18, 24, ES(3));
        } else {
            vge(18, 18, 24, ES(3));
        }
        vmulf(29, 29, 10, ES(5));
        vmacf(29, 17, 16, EW);
        vmulf(27, 27, 10, ES(5));
        vmacf(27, 17, 15, EW);
        vxor(17, 8, 17, EW);
        vmulf(16, 18, 24, ES(6));
        vmulf(15, 18, 24, ES(7));
        sqv(29, 0, dryL);
        dryL += 0x10;
        vmulf(28, 28, 10, ES(5));
        vmacf(28, 17, 16, EW);
        sqv(27, 0, wetL);
        wetL += 0x10;
        vmulf(26, 26, 10, ES(5));
        vmacf(26, 17, 15, EW);
        sqv(28, 0, dryR);
        dryR += 0x10;
        sqv(26, 0, wetR);
        wetR += 0x10;
    }
    /* 0x1B84 */
    vaddc(21, 21, 24, ES(2));
    vadd(20, 20, 24, ES(1));
    for (;;) {
        /* 0x1B8C */
        lqv(29, 0, dryL);
        vaddc(19, 19, 24, ES(5));
        lqv(17, 0, in);
        vadd(18, 18, 24, ES(4));
        if (rateL >= 0) {
            vlt(20, 20, 24, ES(0));
        } else {
            vge(20, 20, 24, ES(0));
        }
        vxor(17, 9, 17, EW);
        lqv(27, 0, wetL);
        if (rateR >= 0) {
            vlt(18, 18, 24, ES(3));
        } else {
            vge(18, 18, 24, ES(3));
        }
        vmulf(16, 20, 24, ES(6));
        sqv(20, 0, STATE + 0x00);
        vmulf(15, 20, 24, ES(7));
        sqv(21, 0, STATE + 0x10);
        vmulf(29, 29, 10, ES(5));
        vmacf(29, 17, 16, EW);
        lqv(28, 0, dryR);
        vmulf(27, 27, 10, ES(5));
        lqv(26, 0, wetR);
        vmacf(27, 17, 15, EW);
        vxor(17, 8, 17, EW);
        n -= 0x10;
        in += 0x10;
        vaddc(21, 21, 24, ES(2));
        vadd(20, 20, 24, ES(1));
        sqv(29, 0, dryL);
        dryL += 0x10;
        vmulf(16, 18, 24, ES(6));
        vmulf(15, 18, 24, ES(7));
        sqv(27, 0, wetL);
        wetL += 0x10;
        vmulf(28, 28, 10, ES(5));
        vmacf(28, 17, 16, EW);
        vmulf(26, 26, 10, ES(5));
        vmacf(26, 17, 15, EW);
        sqv(28, 0, dryR);
        dryR += 0x10;
        sqv(26, 0, wetR);
        if (n <= 0) {
            break;
        }
        wetR += 0x10;
    }
    sqv(18, 0, STATE + 0x20);
    sqv(19, 0, STATE + 0x30);
    sqv(24, 0, STATE + 0x40);
    spDma(1, STATE, state, 0x4F);
    sR2 = state;
}

/* 0x11BC (LOADBUFF), 0x11FC (SAVEBUFF) */
static void opBuff(int save, u32 w0, u32 w1) {
    u32 count = (w0 >> 12) & 0xFFF;
    if (count == 0) {
        return;
    }
    sR2 = w1 & 0xFFFFFF;
    spDma(save, (w0 & 0xFFF) + 0x4F0, sR2, count - 1);
}

/* 0x17C0 */
static void opResample(u32 w0, u32 w1) {
    u32 state = w0 & 0xFFFFFF;
    u32 in = ((w1 >> 2) & 0xFFF) + 0x4E8; /* $20: input minus the 4 history samples */
    u32 out;
    u32 addr[8], coef[8];
    s32 n;
    int i;

    vzero(23);
    if ((w1 >> 30) == 0) {
        spDma(0, STATE, state, 0xF);
        lqv(31, 0, 0x40);
        lqv(25, 0, 0x30);
        ldv(19, 0, STATE);
        lsv(24, 14, STATE + 8);
    } else {
        lqv(31, 0, 0x40);
        vzero(19);
        lqv(25, 0, 0x30);
        vzero(24);
    }
    mtc2(21, 4, in);
    mtc2(21, 6, 0xB0);
    vsub(25, 25, 31, EW);
    mtc2(21, 8, w1 >> 14);
    mtc2(21, 10, 0x40);
    vsub(25, 25, 31, EW);
    lqv(30, 0, 0x50);
    lqv(29, 0, 0x60);
    lqv(28, 0, 0x70);
    vmudm(24, 31, 24, ES(7));
    lqv(27, 0, 0x80);
    vmadm(23, 25, 21, ES(4));
    lqv(26, 0, 0x90);
    vmadn(24, 31, 30, ES(0));
    sdv(19, 0, in);
    lqv(25, 0, 0x30);
    vmudn(22, 31, 21, ES(2));
    n = 0x170;
    vmadn(22, 23, 30, ES(2));
    vmudl(20, 24, 21, ES(5));
    out = (w1 & 3) ? 0x660 : 0x4F0;
    ssv(24, 7, STATE + 8); /* element byte 7 as encoded; overwritten on exit */
    vmudn(20, 20, 30, ES(4));
    sqv(22, 0, 0xFB0);
    vmadn(20, 31, 21, ES(3));
    sqv(20, 0, 0xFC0);
    for (i = 0; i < 8; i++) { /* the ucode's lh of what sqv just stored */
        addr[i] = (u32)(s32)sV[22][i];
        coef[i] = (u32)(s32)sV[20][i];
    }
    for (;;) {
        /* 0x1900 */
        ldv(19, 0, addr[0]);
        vmudm(24, 31, 24, ES(7));
        ldv(18, 0, coef[0]);
        vmadh(24, 31, 23, ES(7));
        ldv(19, 8, addr[4]);
        vmadm(23, 25, 21, ES(4));
        ldv(18, 8, coef[4]);
        vmadn(24, 31, 30, ES(0));
        ldv(17, 0, addr[1]);
        vmudn(22, 31, 21, ES(2));
        ldv(16, 0, coef[1]);
        ldv(17, 8, addr[5]);
        vmadn(22, 23, 30, ES(2));
        ldv(16, 8, coef[5]);
        vmudl(20, 24, 21, ES(5));
        ldv(15, 0, addr[2]);
        ldv(14, 0, coef[2]);
        ldv(15, 8, addr[6]);
        ldv(14, 8, coef[6]);
        vmudn(20, 20, 30, ES(4));
        ldv(13, 0, addr[3]);
        vmadn(20, 31, 21, ES(3));
        ldv(12, 0, coef[3]);
        ldv(13, 8, addr[7]);
        vmulf(11, 19, 18, EW);
        ldv(12, 8, coef[7]);
        vmulf(10, 17, 16, EW);
        sqv(22, 0, 0xFB0);
        vmulf(9, 15, 14, EW);
        sqv(20, 0, 0xFC0);
        for (i = 0; i < 8; i++) {
            addr[i] = (u32)(s32)sV[22][i];
            coef[i] = (u32)(s32)sV[20][i];
        }
        vmulf(8, 13, 12, EW);
        vadd(11, 11, 11, EQ(1));
        vadd(10, 10, 10, EQ(1));
        vadd(9, 9, 9, EQ(1));
        vadd(8, 8, 8, EQ(1));
        vadd(11, 11, 11, EH(2));
        vadd(10, 10, 10, EH(2));
        vadd(9, 9, 9, EH(2));
        vadd(8, 8, 8, EH(2));
        vmudn(7, 29, 11, EH(0));
        vmadn(7, 28, 10, EH(0));
        vmadn(7, 27, 9, EH(0));
        vmadn(7, 26, 8, EH(0));
        n -= 0x10;
        sqv(7, 0, out);
        if (n <= 0) {
            break;
        }
        out += 0x10;
    }
    ldv(19, 0, addr[0]);
    ssv(24, 0, STATE + 8);
    sdv(19, 0, STATE);
    spDma(1, STATE, state, 0xF);
    sR2 = state;
}

/* 0x1C58 */
static void opMixer(u32 w0, u32 w1) {
    u32 out = (w1 & 0xFFFF) + 0x4F0;
    u32 in = (w1 >> 16) + 0x4F0;
    s32 n = 0x170;

    lqv(31, 0, 0x50);
    lqv(28, 0, out);
    lqv(29, 0, in);
    mtc2(30, 0, w0);
    do {
        vmulf(27, 28, 31, ES(5));
        n -= 0x10;
        in += 0x10;
        out += 0x10;
        vmacf(27, 29, 30, ES(0));
        lqv(28, 0, out);
        lqv(29, 0, in);
        sqv(27, 0, out - 0x10);
    } while (n > 0);
}

/* 0x126C; op 0x0E enters at 0x12B0 */
static void opSetvol(u32 w0, u32 w1) {
    u32 flags = w0 >> 16;
    sR2 = w1 >> 16;
    if (flags & 4) {
        if (flags & 2) {
            dmWr16(STATE + 0x50, w0);
            dmWr16(STATE + 0x4C, sR2);
            dmWr16(STATE + 0x4E, w1);
        } else {
            dmWr16(STATE + 0x46, w0);
            dmWr16(STATE + 0x48, sR2);
            dmWr16(STATE + 0x4A, w1);
        }
    } else {
        dmWr16(STATE + 0x40, w0);
        dmWr16(STATE + 0x42, sR2);
        dmWr16(STATE + 0x44, w1);
    }
}

static void opSetvolTail(u32 w0, u32 w1) {
    dmWr16(STATE + 0x40, w0);
    dmWr16(STATE + 0x42, sR2);
    dmWr16(STATE + 0x44, w1);
}

/* 0x1338 */
static void opDmemmove(u32 w0, u32 w1) {
    s32 n = w1 & 0xFFFF;
    u32 src = (w0 & 0xFFFF) + 0x4F0;
    u32 dst = (w1 >> 16) + 0x4F0;
    do {
        ldv(1, 0, src);
        ldv(2, 0, src + 8);
        n -= 0x10;
        src += 0x10;
        sdv(1, 0, dst);
        sdv(2, 0, dst + 8);
        dst += 0x10;
    } while (n > 0);
    sR2 = src;
}

/* 0x123C */
static void opLoadadpcm(u32 w0, u32 w1) {
    sR2 = w1 & 0xFFFFFF;
    spDma(0, 0x3F0, sR2, (w0 & 0xFFFF) - 1);
}

/* 0x12C4 */
static void opInterleave(void) {
    s32 n = 0x170;
    u32 out = 0x4F0, l = 0x9D0, r = 0xB40;
    int k;
    do {
        lqv(1, 0, l);
        lqv(2, 0, r);
        n -= 0x10;
        l += 0x10;
        r += 0x10;
        for (k = 0; k < 8; k++) {
            ssv(1, 2 * k, out + 4 * k);
            ssv(2, 2 * k, out + 4 * k + 2);
        }
        out += 0x20;
    } while (n > 0);
    sR2 = l;
}

/* 0x1374: the word store at DMEM 0x0E overlays jump-table entries 7 and 8. */
static void opSetloop(u32 w1) {
    u32 a = w1 & 0xFFFFFF;
    dmWr16(0x0E, a >> 16);
    dmWr16(0x10, a);
}

/* PORT_AOP_SPATIAL_BEGIN/END (port-only, see port.h and hrtf.c): the dry buffers before one voice's ENVMIXER */
static s16 sSpatialDry[2][HRTF_FRAME];

static void opSpatialBegin(void) {
    memcpy(sSpatialDry[0], &sDmem[0x9D0], sizeof sSpatialDry[0]);
    memcpy(sSpatialDry[1], &sDmem[0xB40], sizeof sSpatialDry[1]);
}

static void opSpatialEnd(u32 w1) {
    portHrtfMix(w1, sSpatialDry[0], sSpatialDry[1], (s16 *)&sDmem[0x9D0], (s16 *)&sDmem[0xB40]);
}

/* ------------------------------------------------------------- task ---- */

static void loadUcodeData(void) {
    int i;

    if (!(D_80760590[0] == 0x10 && D_80760590[1] == 0xE8)) {
        portAbort("PORT: naudio ucode data D_80760590 not recognised");
    }
    for (i = 0; i < UCODE_DATA_SIZE / 2; i++) {
        sUcodeData[i] = (u16)((D_80760590[2 * i] << 8) | D_80760590[2 * i + 1]);
    }
    sUcodeReady = 1;
}

/* Command words from DMEM: the command buffer holds native u32 words copied from RDRAM; slots read past
 * it (op 0x00 overrun) see halfword data and are composed as the hardware's big-endian word. */
static u32 cmdWord(u32 a) {
    a &= 0xFFF;
    if (a >= CMD_BUF && a + 4 <= CMD_BUF + CMD_CHUNK) {
        u32 w;
        memcpy(&w, &sDmem[a], 4);
        return w;
    }
    return ((u32)(u16)dmRd16(a) << 16) | (u16)dmRd16(a + 2);
}

void portAudioRunTask(OSTask *t) {
    u32 words[16];
    u32 dram = (u32)(uintptr_t)t->t.data_ptr; /* $28 */
    s32 remain = (s32)t->t.data_size;        /* $27 */
    s32 chunk;                               /* $30 */
    u32 slot;                                /* $29 */
    int i;

    if (!sUcodeReady) {
        loadUcodeData();
    }
    if (remain <= 0) {
        return;
    }
    /* Task load: ucode_data over DMEM 0x000..0x7FF (only its first 0x2B0 bytes are meaningful; the rest
     * is zeroed), OSTask at 0xFC0. 0x800..0xFBF keeps what the previous audio task left. */
    for (i = 0; i < UCODE_DATA_SIZE / 2; i++) {
        dmWr16(2 * i, sUcodeData[i]);
    }
    memset(&sDmem[UCODE_DATA_SIZE], 0, 0x800 - UCODE_DATA_SIZE);
    memcpy(words, &t->t, sizeof(words));
    for (i = 0; i < 16; i++) {
        dmWr16(0xFC0 + 4 * i, words[i] >> 16);
        dmWr16(0xFC0 + 4 * i + 2, words[i]);
    }

    for (;;) {
        /* 0x112C: fetch the next chunk */
        chunk = remain > CMD_CHUNK ? CMD_CHUNK : remain;
        spDma(0, CMD_BUF, dram, (u32)chunk - 1);
        sR2 = 0;
        slot = CMD_BUF;
        do {
            /* 0x10C4 */
            u32 w0 = cmdWord(slot);
            u32 w1 = cmdWord(slot + 4);
            u32 op = (w0 >> 24) & 0x7F;
            u32 pc = 0x1000 | ((u16)dmRd16(op * 2) & 0xFFC);
            dram += 8;
            remain -= 8;
            if (op == PORT_AOP_SPATIAL_BEGIN || op == PORT_AOP_SPATIAL_END) {
                chunk -= 8;
                if (op == PORT_AOP_SPATIAL_BEGIN) {
                    opSpatialBegin();
                } else {
                    opSpatialEnd(w1);
                }
            } else if (pc == 0x10E8) {
                if (!sLoggedOp0) {
                    sLoggedOp0 = 1;
                    portLog("PORT: naudio op 0x%02x has no handler in this ucode (slot not consumed)", w0 >> 24);
                }
            } else {
                chunk -= 8;
                switch (pc) {
                case 0x138C:
                    opAdpcm(w0, w1);
                    break;
                case 0x1190:
                    opClearbuff(w0, w1);
                    break;
                case 0x1A28:
                    opEnvmixer(w0, w1);
                    break;
                case 0x11BC:
                    opBuff(0, w0, w1);
                    break;
                case 0x11FC:
                    opBuff(1, w0, w1);
                    break;
                case 0x17C0:
                    opResample(w0, w1);
                    break;
                case 0x1C58:
                    opMixer(w0, w1);
                    break;
                case 0x126C:
                    opSetvol(w0, w1);
                    break;
                case 0x12B0:
                    opSetvolTail(w0, w1);
                    break;
                case 0x1338:
                    opDmemmove(w0, w1);
                    break;
                case 0x123C:
                    opLoadadpcm(w0, w1);
                    break;
                case 0x12C4:
                    opInterleave();
                    break;
                case 0x1374:
                    opSetloop(w1);
                    break;
                default: {
                    u32 b = w0 >> 24;
                    if (!(sLoggedOp[b >> 3] & (1 << (b & 7)))) {
                        sLoggedOp[b >> 3] |= (u8)(1 << (b & 7));
                        portLog("PORT: naudio unknown op 0x%02x", b);
                    }
                    break;
                }
                }
            }
            /* 0x10E8 */
            slot += 8;
        } while (chunk > 0);
        if (remain <= 0) {
            break;
        }
    }
}
