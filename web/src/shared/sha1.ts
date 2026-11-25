// SHA-1 as lowercase hex (WebCrypto): ROM and wasm identity (game/boot.ts), audio pack identity (game/audiopack/).
export async function sha1Hex(bytes: BufferSource): Promise<string> {
  const digest = await crypto.subtle.digest('SHA-1', bytes);
  return Array.from(new Uint8Array(digest), (x) => x.toString(16).padStart(2, '0')).join('');
}
