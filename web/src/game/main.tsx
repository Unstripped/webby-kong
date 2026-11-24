// The game page: index.html's #app. Listener registration order matters: debug's capture-phase keydown runs before
// the menu's.
import { render } from 'preact';
import { installBoot } from './boot';
import { installDebugKeys } from './debug/api';
import { GameUi } from './GameUi';
import { exposeGlobals } from './globals';
import { installMenuControl } from './menu/control';
import { installReport } from './report';
import { loadSettings } from './settings';
import { installTouch } from './touch/pad';

loadSettings();
exposeGlobals();
installDebugKeys();
installReport();
installTouch();
installMenuControl();
render(<GameUi />, document.getElementById('app')!);
installBoot();
