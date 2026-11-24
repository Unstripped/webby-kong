import { DebugPanel } from './debug/DebugPanel';
import { Menu } from './menu/Menu';
import { RomDropZone } from './RomDropZone';
import { TouchOverlay } from './touch/TouchOverlay';
import { GameToast } from './ui/GameToast';

export function GameUi() {
  return (
    <>
      <RomDropZone />
      <DebugPanel />
      <GameToast />
      <TouchOverlay />
      <Menu />
    </>
  );
}
