// The Esc menu's tables: level names, kongs, progress columns and the report view texts.

export const LEVELS = [
  'Jungle Japes',
  'Angry Aztec',
  'Frantic Factory',
  'Gloomy Galleon',
  'Fungi Forest',
  'Crystal Caves',
  'Creepy Castle',
  'DK Isles',
  'Hideout Helm',
];

export interface Kong {
  id: string;
  name: string;
  color: string;
}

export const KONGS: Kong[] = [
  { id: 'dk', name: 'DK', color: '#f5c518' },
  { id: 'diddy', name: 'Diddy', color: '#e53935' },
  { id: 'lanky', name: 'Lanky', color: '#1e88e5' },
  { id: 'tiny', name: 'Tiny', color: '#8e44ad' },
  { id: 'chunky', name: 'Chunky', color: '#43a047' },
];

export const BLUEPRINT = ['–', 'Found', 'Turned in'];

// port_level_progress (one snapshot per open; the game is frozen until close)
export interface KongProgress {
  gb: number | null;
  bananas: number | null;
  medal: number | null;
  blueprint: number | null;
}

export interface Progress {
  ok: boolean;
  reason?: string;
  world: number;
  kong: number;
  kongs: KongProgress[];
  fairies: number;
  fairiesTotal: number;
  crowns: number;
  crownsTotal: number;
  keys: number;
  keysTotal: number;
}

interface Column {
  key: keyof KongProgress;
  label: string;
  icon: (kong: Kong) => string;
  text: (v: number) => string;
  done: (v: number) => boolean;
}

// field of each kong entry, label, icon (/menu/<name>.png), value text, complete
export const COLUMNS: Column[] = [
  { key: 'gb', label: 'Golden Bananas', icon: () => 'gb', text: (v) => `${v}/5`, done: (v) => v === 5 },
  {
    key: 'bananas',
    label: 'Bananas',
    icon: (kong) => `banana-${kong.id}`,
    text: (v) => `${v}/100`,
    done: (v) => v === 100,
  },
  { key: 'medal', label: 'Medal', icon: () => 'medal', text: (v) => (v ? '✓' : '–'), done: (v) => v === 1 },
  {
    key: 'blueprint',
    label: 'Blueprint',
    icon: () => 'blueprint',
    text: (v) => BLUEPRINT[v] || String(v),
    done: (v) => v === 2,
  },
];

interface Wide {
  key: 'fairies' | 'crowns' | 'keys';
  total: 'fairiesTotal' | 'crownsTotal' | 'keysTotal';
  icon: string;
  label: (t: number) => string;
}

// level-wide collectables: count field, total field (0 = none in this level)
export const WIDE: Wide[] = [
  { key: 'fairies', total: 'fairiesTotal', icon: 'fairy', label: () => 'Fairies' },
  { key: 'crowns', total: 'crownsTotal', icon: 'crown', label: (t) => (t === 1 ? 'Battle crown' : 'Battle crowns') },
  { key: 'keys', total: 'keysTotal', icon: 'key', label: () => 'Boss key' },
];

export type ReportKind = 'bug' | 'idea' | 'crash';

export const KIND_TEXT: Record<ReportKind, { title: string; intro: string; placeholder: string }> = {
  bug: {
    title: 'REPORT A BUG',
    intro:
      'Something look wrong, act weird or get stuck? Tell us what happened. The game adds a screenshot of this ' +
      'moment, your save and your location.',
    placeholder: 'What happened? What did you expect?',
  },
  idea: { title: 'SUGGEST AN IDEA', intro: 'What would make the game better?', placeholder: 'Your idea…' },
  crash: {
    title: 'UH OH!',
    intro: 'The game crashed. Sending a report gets it fixed faster.',
    placeholder: 'What were you doing when it crashed?',
  },
};

export const LIST = [
  'Your save file and exactly where you are, so we can load your game right there',
  'Game version, settings, browser and graphics card',
  'Recent game events and errors',
];

// menu/<name>.png under web/public
export function iconSrc(name: string): string {
  return '/menu/' + name + '.png';
}
