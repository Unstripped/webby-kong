/* 16 Kbit EEPROM in memory, persisted by save.c. */
#include "port_internal.h"

u8 gPortEeprom[0x800];

s32 osEepromProbe(OSMesgQueue *mq) {
    (void)mq;
    return EEPROM_TYPE_16K;
}

s32 osEepromRead(OSMesgQueue *mq, u8 address, u8 *buffer) {
    (void)mq;
    memcpy(buffer, &gPortEeprom[(u32)address * 8], 8);
    return 0;
}

s32 osEepromWrite(OSMesgQueue *mq, u8 address, u8 *buffer) {
    (void)mq;
    memcpy(&gPortEeprom[(u32)address * 8], buffer, 8);
    portSaveWritten();
    return 0;
}

s32 osEepromLongRead(OSMesgQueue *mq, u8 address, u8 *buffer, int nbytes) {
    (void)mq;
    if ((u32)address * 8 + nbytes > sizeof gPortEeprom) {
        return -1;
    }
    memcpy(buffer, &gPortEeprom[(u32)address * 8], nbytes);
    return 0;
}

s32 osEepromLongWrite(OSMesgQueue *mq, u8 address, u8 *buffer, int nbytes) {
    (void)mq;
    if ((u32)address * 8 + nbytes > sizeof gPortEeprom) {
        return -1;
    }
    memcpy(&gPortEeprom[(u32)address * 8], buffer, nbytes);
    portSaveWritten();
    return 0;
}
