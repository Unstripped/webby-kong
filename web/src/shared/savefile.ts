// Save game files for export / import (the Esc menu's Export save / Import save, the dashboard's save tiles).
// A save game is DK64's 2048-byte EEPROM. Accepted files, neither byte-swapped:
// - .eep: the raw 2048 bytes (what Export writes; Project64 / mupen64plus 16 Kbit EEPROM files are the same bytes).
// - .srm: RetroArch mupen64plus, 296960 (0x48800) bytes laid out eeprom[0x800], mempack[4 * 0x8000], sram[0x8000],
//   flashram[0x20000] (libretro libretro_memory.h); its EEPROM is the first 2048 bytes.

const SIZE = 2048;
const SRM_SIZE = 0x48800;

// ArrayBuffer -> a copy of its 2048 EEPROM bytes; throws an Error whose message is for the player
export function parse(buffer: ArrayBuffer): Uint8Array<ArrayBuffer> {
  const n = buffer.byteLength;
  if (n !== SIZE && n !== SRM_SIZE) {
    throw new Error(
      'Not a DK64 save file: expected a 2048-byte .eep or a 296960-byte RetroArch .srm (got ' + n + ' bytes).',
    );
  }
  const bytes = new Uint8Array(buffer.slice(0, SIZE));
  if (bytes.every((b) => b === 0xff) || bytes.every((b) => b === 0)) {
    throw new Error('This file holds no save: its EEPROM is blank.');
  }
  return bytes;
}

// dk64-<slug of the save name>-<YYYYMMDD>-<HHMM>.eep, local time
export function fileName(saveName: string | null | undefined): string {
  const slug =
    String(saveName || '')
      .toLowerCase()
      .replace(/[^a-z0-9]+/g, '-')
      .replace(/^-+|-+$/g, '') || 'save';
  const d = new Date();
  const p = (n: number) => String(n).padStart(2, '0');
  const day = `${d.getFullYear()}${p(d.getMonth() + 1)}${p(d.getDate())}`;
  return `dk64-${slug}-${day}-${p(d.getHours())}${p(d.getMinutes())}.eep`;
}

export function download(bytes: Uint8Array<ArrayBuffer>, name: string): void {
  const a = document.createElement('a');
  a.href = URL.createObjectURL(new Blob([bytes], { type: 'application/octet-stream' }));
  a.download = name;
  a.click();
  setTimeout(() => URL.revokeObjectURL(a.href), 10000);
}
