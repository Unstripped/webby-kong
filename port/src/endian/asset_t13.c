/* Asset table 13: actor animation scripts (one entry, index 0, 0x11634 bytes in the US ROM).
 *
 * Loaded once per map by actorInit (global/7CA80.c, assetLoad(0xD, 0, 1, 1) -> gActorScriptAsset), called
 * unconditionally from mapLoadLevel. Layout (all offsets from the asset start):
 *
 *   0x0  s32 offset[count]   script offset table; offset[0] is the first byte after the table, so
 *                            count = offset[0] / 4 (0x450 in the US ROM). The last entry is the end of the
 *                            data; every other entry starts a region that runs to the next entry.
 *                            actorScriptStart (global/19A00.c) reads ((s32 *)base)[scriptId] (scriptId 0 means
 *                            "no script" and is never dereferenced); entries 1..3 double as ActorAssetHeader
 *                            (include/actor.h) soundTableOffset / animTableOffset / indexTableOffset, read by
 *                            actorInit. Script ids come from the index table and from opcode operands.
 *   offset[1]..offset[2]  s16 sound table   gActorScriptSoundTable: sfxPlayScriptActorSoundForPlayer (19A00)
 *   offset[2]..offset[3]  u16 anim table    gActorScriptAnimTable: controllerAnimSetStateRemap,
 *                                            controllerAnimSetStateRemapDirect, controllerAnimSetFromTable (17B90)
 *   offset[3]..offset[4]  u16 index table   gActorScriptIndexTable (script ids): actorScriptStartByIndex (19A00),
 *                                            actorScriptStartByKongIndex (80150), playerInputUpdateMoveTargetWith-
 *                                            Deadzone2 (E4090)
 *
 * Script bytecode: one opcode byte, then operands read through scriptReadU8/U16/U32 (global/19A00.c), which copy
 * bytes in memory order into a native s16/u16 (U16) or f32 (U32) local; scriptReadU24 has no callers. A memory-
 * order copy is a native read, so every 16/32-bit operand is swapped in place here. No handler reads an operand
 * conditionally, so each opcode has a fixed operand list (kOpFormat below, taken from the handler bodies in
 * 19A00.c), except:
 *   0x35 scriptOpJumpTable    u16 offsets right after the opcode, indexed by the script register. The table
 *                             carries no length; it ends where its lowest target begins (in the US ROM all 47
 *                             tables follow 0x4C SetRegisterRandom(0, n - 1) and hold exactly n entries).
 *   0x36 scriptOpRunCommand, 0x37 scriptOpSetCommandHandler
 *                             s16 command index into sScriptCommandTable, then that command's operands
 *                             (kCmdFormat; handlers in 19A00.c and F56F0.c). SetCommandHandler keeps the PC of
 *                             those operands (scriptCommandPc) and the command re-reads them every frame.
 *   0x4D scriptOpBranchOnFrame, 0x4E scriptOpBranchOnResult -> scriptOpBranchOnCondition / scriptSkipBranchCases
 *                             case records { u8 kind, s16 value, s16 target-or-result } closed by a single 0 kind
 *                             byte (the game reads two bytes past it and backs up; they are not operands).
 * Jump operands (0x2F-0x33, jump tables, case kinds 1-3) are u16/s16 offsets from the script start.
 * Opcodes that never advance (0x27, 0x29, 0x34, 0x47, 0x54) end their path but the walk continues linearly: in
 * the US ROM all 1099 scripts decode exactly to their region end and every jump target lands on an opcode.
 * The walk swaps each field once, bounds-checks every operand against its script region and aborts on an
 * unknown opcode/command or a region that does not decode exactly. */
#include "endian/asset_swap.h"

#define SCRIPT_OPCODE_COUNT 0x5C
#define SCRIPT_COMMAND_COUNT 0x2C

/* operand formats: b = u8, h = 16-bit, w = 32-bit (f32); J = jump table, C = command operands, K = case list */
static const char *const kOpFormat[SCRIPT_OPCODE_COUNT] = {
    /* 00 */ "", "h", "", "", "", "b", "bb", "",
    /* 08 */ "h", "w", "w", "", "w", "", "", "",
    /* 10 */ "", "w", "w", "ww", "h", "hw", "b", "hb",
    /* 18 */ "h", "hw", "hb", "b", "b", "bb", "h", "h",
    /* 20 */ "w", "ww", "", "", "ww", "h", "h", "",
    /* 28 */ "h", "", "w", "h", "h", "h", "h", "h",
    /* 30 */ "h", "h", "h", "h", "", "J", "C", "C",
    /* 38 */ "hbbhb", "hb", "hbbbb", "hbbbbwbb", "hb", "hbhb", "hbbb", "hbbbb",
    /* 40 */ "b", "hbbb", "bwb", "bb", "b", "hb", "hbbbb", "",
    /* 48 */ "b", "", "b", "", "bb", "K", "K", "h",
    /* 50 */ "h", "", "", "", "", "", "w", "",
    /* 58 */ "h", "b", "b", "",
};

/* sScriptCommandTable operand formats */
static const char *const kCmdFormat[SCRIPT_COMMAND_COUNT] = {
    /* 00 */ "", "w", "", "", "", "", "wb", "b",
    /* 08 */ "w", "", "", "", "", "h", "bbhh", "b",
    /* 10 */ "b", "b", "", "", "h", "h", "hh", "bbhh",
    /* 18 */ "", "b", "b", "b", "", "", "", "",
    /* 20 */ "w", "bb", "b", "b", "b", "b", "hhbbbbh", "hhhh",
    /* 28 */ "", "b", "", "",
};

/* swaps one fixed operand list starting at *p; `end` is the script region end */
static void swapOperands(u8 *buf, u32 *p, u32 end, const char *fmt) {
    for (; *fmt != '\0'; fmt++) {
        switch (*fmt) {
        case 'b':
            assetCheck("script u8 operand", *p, 1, end);
            *p += 1;
            break;
        case 'h':
            assetCheck("script u16 operand", *p, 2, end);
            asw16(buf, *p);
            *p += 2;
            break;
        case 'w':
            assetCheck("script u32 operand", *p, 4, end);
            asw32(buf, *p);
            *p += 4;
            break;
        }
    }
}

static void swapScript(u8 *buf, u32 start, u32 end, s32 id) {
    u32 p = start;

    while (p < end) {
        u8 op = buf[p];
        const char *fmt;

        if (op >= SCRIPT_OPCODE_COUNT) {
            portAbort("PORT: actor script %d: bad opcode 0x%02x at 0x%x", (int)id, (unsigned)op, p);
        }
        p++;
        fmt = kOpFormat[op];
        if (fmt[0] == 'J') {
            u32 limit = end;
            while (p < limit) {
                u32 target;
                assetCheck("script jump table", p, 2, end);
                target = start + asw16(buf, p);
                if (target <= start || target > end) {
                    portAbort("PORT: actor script %d: jump table target 0x%x out of range at 0x%x", (int)id, target, p);
                }
                if (target < limit) {
                    limit = target;
                }
                p += 2;
            }
            if (p != limit) {
                portAbort("PORT: actor script %d: jump table overruns its first target at 0x%x", (int)id, p);
            }
        } else if (fmt[0] == 'C') {
            s16 cmd;
            assetCheck("script command index", p, 2, end);
            cmd = (s16)asw16(buf, p);
            p += 2;
            if (cmd < 0 || cmd >= SCRIPT_COMMAND_COUNT) {
                portAbort("PORT: actor script %d: bad command %d at 0x%x", (int)id, (int)cmd, p - 2);
            }
            swapOperands(buf, &p, end, kCmdFormat[cmd]);
        } else if (fmt[0] == 'K') {
            for (;;) {
                assetCheck("script case kind", p, 1, end);
                if (buf[p] == 0) {
                    p++;
                    break;
                }
                assetCheck("script case record", p, 5, end);
                asw16(buf, p + 1);
                asw16(buf, p + 3);
                p += 5;
            }
        } else {
            swapOperands(buf, &p, end, fmt);
        }
    }
    if (p != end) {
        portAbort("PORT: actor script %d: decode ends at 0x%x, region ends at 0x%x", (int)id, p, end);
    }
}

static u32 readU32(const u8 *buf, u32 off) {
    u32 v;
    memcpy(&v, buf + off, 4);
    return v;
}

void portSwapAssetTable13(s32 index, u8 *buf, u32 size) {
    u32 count, i;

    assetCheck("actor script offset[0]", 0, 4, size);
    count = abe32(buf, 0) / 4;
    if ((abe32(buf, 0) & 3) != 0 || count < 5) {
        portAbort("PORT: actor script asset %d: bad offset table size 0x%x", (int)index, abe32(buf, 0));
    }
    assetSwap32Range(buf, 0, count * 4, size);
    for (i = 0; i + 1 < count; i++) {
        if (readU32(buf, i * 4) > readU32(buf, (i + 1) * 4)) {
            portAbort("PORT: actor script asset %d: offset[%u] not ascending", (int)index, i);
        }
    }
    assetCheck("actor script data", 0, readU32(buf, (count - 1) * 4), size);

    assetSwap16Range(buf, readU32(buf, 4), readU32(buf, 8), size);   /* s16 sound table */
    assetSwap16Range(buf, readU32(buf, 8), readU32(buf, 12), size);  /* u16 anim table */
    assetSwap16Range(buf, readU32(buf, 12), readU32(buf, 16), size); /* u16 index table */
    for (i = 0; i + 1 < count; i++) {
        if (i >= 1 && i <= 3) {
            continue;
        }
        swapScript(buf, readU32(buf, i * 4), readU32(buf, (i + 1) * 4), (s32)i);
    }
}
