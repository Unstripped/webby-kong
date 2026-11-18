/* Asset table 18: per-map volume triggers, indexed by map. Loaded by volumeTriggerTableInit (1332B0.c), copied
 * record by record into gVolumeTriggerTable by volumeTriggerTableLoad, then freed. Compressed by the asset table
 * (D_80748E18[18] == 1), so assetLoad inflates it before the swap.
 *
 * Layout (the US ROM entries are exactly 2 + count * 0x38 bytes):
 *   0x00  s16 count                                  volumeTriggerTableLoad (`*data`)
 *   0x02  count x VolumeTriggerDef (0x38 bytes):
 *     0x00 s16 x, y, z, radius, height, id           native s16: volumeTriggerUpdate, volumeTriggerUpdateAll,
 *                                                    volumeTriggerSetEnabled (id)
 *     0x0C u8 mode, 0x0D u8 startEnabled, 0x0E u8 actionCount, 0x0F pad   bytes
 *     0x10 4 x VolumeTriggerAction (0xA bytes; all four swapped, unused ones carry data in some maps):
 *       +0 s16 hookIndex                             native s16: index into gVolumeTriggerHooks
 *       +2 u8 payload[8]                             forwarded by actorAppendCallbackMemcpy8 and read through
 *                                                    gActorGlobalState.volumeTriggerQueueId[] as s16[4]
 *                                                    (1295B0.c:64,877, 1332B0.c:140, 80150.c, 8F4B0.c, 9CE20.c,
 *                                                    A2F10.c, BD820.c:136, C4620.c:204); func_80724994 builds the
 *                                                    same payload natively as s16[4]. ROM values confirm halfwords
 *                                                    (0xFFFF, 0xFF56, 0x008C ...). Swapped as 4 x s16.
 *
 * Payload byte readers: bonus/0.c:188 and C2A90.c:59 read `((u8 *)volumeTriggerQueueId[i])[3]`, i.e. the low
 * byte of payload s16[1] in big-endian order. With native payloads (both this table and func_80724994's) those two
 * sites need a game patch to `(u8)volumeTriggerQueueId[i][1]`. */
#include "endian/asset_swap.h"

#define VOLUME_TRIGGER_SIZE 0x38
#define VOLUME_TRIGGER_ACTION_SIZE 0xA

void portSwapAssetTable18(s32 index, u8 *buf, u32 size) {
    s32 count;
    s32 i, j;
    u32 rec;
    (void)index;

    assetCheck("t18 count", 0, 2, size);
    count = (s16)asw16(buf, 0);
    if (count > 0) {
        assetCheck("t18 triggers", 2, (u32)count * VOLUME_TRIGGER_SIZE, size);
    }
    for (i = 0; i < count; i++) {
        rec = 2 + (u32)i * VOLUME_TRIGGER_SIZE;
        assetSwap16Range(buf, rec, rec + 0x0C, size); /* x, y, z, radius, height, id */
        for (j = 0; j < 4; j++) {
            u32 act = rec + 0x10 + (u32)j * VOLUME_TRIGGER_ACTION_SIZE;
            assetSwap16Range(buf, act, act + VOLUME_TRIGGER_ACTION_SIZE, size); /* hookIndex + payload s16[4] */
        }
    }
}
