import { describe, expect, it } from 'vitest';
import { stickVec } from './pad';

describe('stickVec', () => {
  it('applies the deadzone and scales to radius 80', () => {
    expect(stickVec(0.05, 0)).toEqual([0, 0]);
    expect(stickVec(1, 0)).toEqual([80, 0]);
    expect(stickVec(0, -1)).toEqual([0, -80]);
  });
});
