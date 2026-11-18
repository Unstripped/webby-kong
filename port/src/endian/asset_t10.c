/* Table 10: per-map cutscene/model scripts. Consumer: csScriptLoad (global/42630.c), called from
 * modelSlotsLoadMap (global/36880.c); the copied commands are read by csCondEval / csCmdExec (42630.c).
 *
 * Layout: a pure s16 stream (csScriptLoad walks it as s16 *):
 *   s16 scriptCount
 *   per script: s16 modelId, s16 nodeCount, s16 unk9C (CsStateFull.unk9C)
 *     per node: s16 condCount, CsCond[condCount] (s16 op, s16 arg[3]),
 *               s16 cmdCount,  CsCmd[cmdCount]   (u16 op, s16 a, b, c)
 * Conditions and commands are memcpy'd into CsNode.unk2 / unk2C and only ever read as s16/u16 fields
 * (no byte or s32 reinterpretation in 42630.c). ROM entries are padded to a multiple of 16 bytes after the
 * stream; the padding is never read, so the whole entry is swapped as u16 words. */
#include "endian/asset_swap.h"

void portSwapAssetTable10(s32 index, u8 *buf, u32 size) {
    (void)index;
    assetSwap16Range(buf, 0, size & ~1u, size);
}
