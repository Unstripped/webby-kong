// A community audio pack as tools/packs.py lists it (GET /api/packs); the format is docs/audio-packs.md.
export interface ServerPack {
  id: number;
  name: string;
  author: string;
  version: string;
  description: string;
  songs: number;
  sfx: number;
  size: number;
  sha1: string;
  created_at: string;
  uploader: string | null;
  can_delete: boolean;
}
