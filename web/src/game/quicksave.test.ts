import { describe, expect, it } from 'vitest';
import { pickResume, type ResumeRecord, type Snapshot } from './quicksave';

const point = (time: string): ResumeRecord => ({
  slot: 0, kong: 0, map: 7, exit: 0, hasPos: 0, x: 0, y: 0, z: 0, yaw: 0, time,
});
const snapshot = (time: string): Snapshot => ({ buildId: 'b1', time, map: 7, bytes: new Uint8Array(4) });

describe('pickResume', () => {
  it('restores a same-build snapshot newer than the server resume point', () => {
    const choice = pickResume({
      auto: true, local: null, server: point('2026-01-01T00:00:00.000Z'),
      snapshot: snapshot('2026-01-02T00:00:00.000Z'), buildId: 'b1',
    });
    expect(choice).toEqual({ kind: 'snapshot' });
  });

  it('takes the newest resume point over an older snapshot', () => {
    const local = point('2026-01-02T00:00:00.000Z');
    const server = point('2026-01-03T00:00:00.000Z');
    const choice = pickResume({
      auto: true, local, server, snapshot: snapshot('2026-01-01T00:00:00.000Z'), buildId: 'b1',
    });
    expect(choice).toEqual({ kind: 'resume', record: server });
  });

  it('does nothing without auto-resume, a snapshot or a server point', () => {
    const choice = pickResume({ auto: false, local: point('2026-01-01T00:00:00.000Z'), server: null, snapshot: null, buildId: 'b1' });
    expect(choice).toEqual({ kind: 'none' });
  });
});
