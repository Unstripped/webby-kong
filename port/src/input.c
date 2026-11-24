/* Pad 0 = keyboard OR first SDL game controller (browser); scripted buttons (headless). */
#include "port_internal.h"

u16 gPortHeadlessButtons;
s32 gPortCameraStickX, gPortCameraStickY; /* right-stick camera; headless never writes them, so they stay 0 */

#ifdef PORT_HEADLESS
void portInputPoll(void) {
    gPortPad.button = gPortHeadlessButtons;
    gPortPad.stick_x = 0;
    gPortPad.stick_y = 0;
}

u16 portInputTakeTaps(void) {
    return 0;
}

int portInputQsRanges(QsRange *out) {
    (void)out;
    return 0;
}
#else
#include <SDL2/SDL.h>
#include <emscripten.h>

static SDL_GameController *sPad;
static int sInit;

static const struct { SDL_Scancode sc; u16 button; } kKeys[] = {
    { SDL_SCANCODE_X, A_BUTTON }, { SDL_SCANCODE_C, B_BUTTON }, { SDL_SCANCODE_Z, Z_TRIG },
    { SDL_SCANCODE_A, L_TRIG }, { SDL_SCANCODE_S, R_TRIG }, { SDL_SCANCODE_RETURN, START_BUTTON },
    { SDL_SCANCODE_I, U_CBUTTONS }, { SDL_SCANCODE_K, D_CBUTTONS }, { SDL_SCANCODE_J, L_CBUTTONS },
    { SDL_SCANCODE_L, R_CBUTTONS }, { SDL_SCANCODE_T, U_JPAD }, { SDL_SCANCODE_G, D_JPAD },
    { SDL_SCANCODE_F, L_JPAD }, { SDL_SCANCODE_H, R_JPAD },
};
#define NUM_KEYS (sizeof kKeys / sizeof kKeys[0])
/* a tap that ends before the game samples the pad still registers: a key-down keeps its button held for
 * the next KEY_TAP_READS controller reads */
#define KEY_TAP_READS 2
static u8 sTapReads[NUM_KEYS];
/* on-screen controller (web/src/game/touch/ via port_touch_pad): held buttons, stick, per-bit tap latches */
static struct { u16 held; s8 x, y; u8 tapReads[16]; } sTouch;

int portInputQsRanges(QsRange *out) {
    out[0] = QS_RANGE(sPad);
    out[1] = QS_RANGE(sInit);
    out[2] = QS_RANGE(sTapReads);
    out[3] = QS_RANGE(sTouch);
    return 4;
}

u16 portInputTakeTaps(void) {
    u16 b = 0;
    size_t i;
    for (i = 0; i < NUM_KEYS; i++) {
        if (sTapReads[i] != 0) {
            b |= kKeys[i].button;
            sTapReads[i]--;
        }
    }
    for (i = 0; i < 16; i++) {
        if (sTouch.tapReads[i] != 0) {
            b |= (u16)(1u << i);
            sTouch.tapReads[i]--;
        }
    }
    return b;
}

EMSCRIPTEN_KEEPALIVE void port_touch_pad(u32 buttons, s32 x, s32 y) {
    u16 b = (u16)buttons;
    u16 pressed = b & (u16)~sTouch.held;
    int i;
    for (i = 0; i < 16; i++) {
        if (pressed & (1u << i)) {
            sTouch.tapReads[i] = KEY_TAP_READS;
        }
    }
    sTouch.held = b;
    sTouch.x = (s8)(x > 80 ? 80 : x < -80 ? -80 : x);
    sTouch.y = (s8)(y > 80 ? 80 : y < -80 ? -80 : y);
}

static void openFirstController(void) {
    int i;
    for (i = 0; i < SDL_NumJoysticks(); i++) {
        if (SDL_IsGameController(i)) {
            sPad = SDL_GameControllerOpen(i);
            if (sPad != NULL) {
                return;
            }
        }
    }
}

static s8 axisToStick(Sint16 v) {
    float f = v / 32767.0f;
    if (f > -0.15f && f < 0.15f) {
        return 0;
    }
    f *= 80.0f;
    if (f > 80.0f) {
        f = 80.0f;
    }
    if (f < -80.0f) {
        f = -80.0f;
    }
    return (s8)f;
}

void portInputPoll(void) {
    SDL_Event ev;
    const Uint8 *k;
    u16 b = 0;
    int sx = 0, sy = 0;
    int camX = 0, camY = 0;
    if (!sInit) {
        SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER | SDL_INIT_EVENTS);
        sInit = 1;
        openFirstController();
    }
    while (SDL_PollEvent(&ev)) {
        if (ev.type == SDL_KEYDOWN && !ev.key.repeat) {
            size_t i;
            for (i = 0; i < NUM_KEYS; i++) {
                if (kKeys[i].sc == ev.key.keysym.scancode) {
                    sTapReads[i] = KEY_TAP_READS;
                }
            }
        } else if (ev.type == SDL_CONTROLLERDEVICEADDED && sPad == NULL) {
            openFirstController();
        } else if (ev.type == SDL_CONTROLLERDEVICEREMOVED && sPad != NULL &&
                   ev.cdevice.which == SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(sPad))) {
            SDL_GameControllerClose(sPad);
            sPad = NULL;
            openFirstController();
        }
    }
    k = SDL_GetKeyboardState(NULL);
    {
        size_t i;
        for (i = 0; i < NUM_KEYS; i++) {
            if (k[kKeys[i].sc]) {
                b |= kKeys[i].button;
            }
        }
    }
    if (k[SDL_SCANCODE_LEFT]) sx -= 80;
    if (k[SDL_SCANCODE_RIGHT]) sx += 80;
    if (k[SDL_SCANCODE_UP]) sy += 80;
    if (k[SDL_SCANCODE_DOWN]) sy -= 80;
    b |= sTouch.held;
    if (sTouch.x != 0 || sTouch.y != 0) {
        sx = sTouch.x;
        sy = sTouch.y;
    }
    if (sPad != NULL) {
        SDL_GameController *p = sPad;
        Sint16 rx = SDL_GameControllerGetAxis(p, SDL_CONTROLLER_AXIS_RIGHTX);
        Sint16 ry = SDL_GameControllerGetAxis(p, SDL_CONTROLLER_AXIS_RIGHTY);
        int lx = axisToStick(SDL_GameControllerGetAxis(p, SDL_CONTROLLER_AXIS_LEFTX));
        int ly = -axisToStick(SDL_GameControllerGetAxis(p, SDL_CONTROLLER_AXIS_LEFTY));
        if (lx != 0) sx = lx;
        if (ly != 0) sy = ly;
        if (SDL_GameControllerGetButton(p, SDL_CONTROLLER_BUTTON_A)) b |= A_BUTTON;
        if (SDL_GameControllerGetButton(p, SDL_CONTROLLER_BUTTON_B)) b |= B_BUTTON;
        if (SDL_GameControllerGetButton(p, SDL_CONTROLLER_BUTTON_X)) b |= B_BUTTON;
        if (SDL_GameControllerGetAxis(p, SDL_CONTROLLER_AXIS_TRIGGERLEFT) > 16384) b |= Z_TRIG;
        /* right-stick camera on: LB keeps L (menus read it; every gameplay handler set ignores it) and adds D-pad Left,
         * RB is D-pad Right (RT still gives R), so the kong swap moves to the bumpers */
        if (SDL_GameControllerGetButton(p, SDL_CONTROLLER_BUTTON_LEFTSHOULDER)) b |= gPortStickCamera ? (L_TRIG | L_JPAD) : L_TRIG;
        if (SDL_GameControllerGetButton(p, SDL_CONTROLLER_BUTTON_RIGHTSHOULDER)) b |= gPortStickCamera ? R_JPAD : R_TRIG;
        if (SDL_GameControllerGetAxis(p, SDL_CONTROLLER_AXIS_TRIGGERRIGHT) > 16384) b |= R_TRIG;
        if (SDL_GameControllerGetButton(p, SDL_CONTROLLER_BUTTON_START)) b |= START_BUTTON;
        if (gPortStickCamera) {
            /* the right stick turns and tilts the camera (patches/0028, camPortRightStick); the D-pad is the C buttons */
            camX = axisToStick(rx);
            camY = -axisToStick(ry);
            if (gPortCameraInvert & 1) camX = -camX;
            if (gPortCameraInvert & 2) camY = -camY;
            if (SDL_GameControllerGetButton(p, SDL_CONTROLLER_BUTTON_DPAD_UP)) b |= U_CBUTTONS;
            if (SDL_GameControllerGetButton(p, SDL_CONTROLLER_BUTTON_DPAD_DOWN)) b |= D_CBUTTONS;
            if (SDL_GameControllerGetButton(p, SDL_CONTROLLER_BUTTON_DPAD_LEFT)) b |= L_CBUTTONS;
            if (SDL_GameControllerGetButton(p, SDL_CONTROLLER_BUTTON_DPAD_RIGHT)) b |= R_CBUTTONS;
        } else {
            if (ry < -16384) b |= U_CBUTTONS;
            if (ry > 16384) b |= D_CBUTTONS;
            if (rx < -16384) b |= L_CBUTTONS;
            if (rx > 16384) b |= R_CBUTTONS;
            if (SDL_GameControllerGetButton(p, SDL_CONTROLLER_BUTTON_DPAD_UP)) b |= U_JPAD;
            if (SDL_GameControllerGetButton(p, SDL_CONTROLLER_BUTTON_DPAD_DOWN)) b |= D_JPAD;
            if (SDL_GameControllerGetButton(p, SDL_CONTROLLER_BUTTON_DPAD_LEFT)) b |= L_JPAD;
            if (SDL_GameControllerGetButton(p, SDL_CONTROLLER_BUTTON_DPAD_RIGHT)) b |= R_JPAD;
        }
    }
    gPortCameraStickX = camX;
    gPortCameraStickY = camY;
    gPortPad.button = b;
    gPortPad.stick_x = (s8)(sx > 80 ? 80 : sx < -80 ? -80 : sx);
    gPortPad.stick_y = (s8)(sy > 80 ? 80 : sy < -80 ? -80 : sy);
}
#endif
