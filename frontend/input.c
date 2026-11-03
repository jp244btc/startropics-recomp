/*
 * input.c - opens every game controller SDL can see (XInput, HIDAPI, mapped
 * DirectInput) and merges their state, so a pad that shows up twice on Windows
 * (for example an Xbox controller over Bluetooth) still works whichever
 * instance reports input. Pads without a mapping fall back to a generic
 * joystick layout.
 */
#include "input.h"
#include <stdio.h>
#include <string.h>

#define MAX_DEVICES 8

static SDL_GameController *pads[MAX_DEVICES];
static int npads;
static SDL_Joystick *sticks[MAX_DEVICES];
static int nsticks;
static char summary[240] = "none detected";

static void rebuild_summary(void) {
    if (!npads && !nsticks) { snprintf(summary, sizeof summary, "none detected"); return; }
    summary[0] = 0;
    int shown = 0;
    for (int i = 0; i < npads && shown < 2; i++, shown++) {
        const char *n = SDL_GameControllerName(pads[i]);
        snprintf(summary + strlen(summary), sizeof summary - strlen(summary), "%s%s", shown ? ", " : "", n ? n : "controller");
    }
    for (int i = 0; i < nsticks && shown < 2; i++, shown++) {
        const char *n = SDL_JoystickName(sticks[i]);
        snprintf(summary + strlen(summary), sizeof summary - strlen(summary), "%s%s (generic)", shown ? ", " : "", n ? n : "joystick");
    }
    if (npads + nsticks > shown)
        snprintf(summary + strlen(summary), sizeof summary - strlen(summary), " +%d more", npads + nsticks - shown);
}

static int is_open(SDL_JoystickID id) {
    for (int i = 0; i < npads; i++) if (SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(pads[i])) == id) return 1;
    for (int i = 0; i < nsticks; i++) if (SDL_JoystickInstanceID(sticks[i]) == id) return 1;
    return 0;
}

static void open_device(int index) {
    if (index < 0 || index >= SDL_NumJoysticks()) return;
    if (is_open(SDL_JoystickGetDeviceInstanceID(index))) return;
    if (SDL_IsGameController(index)) {
        if (npads >= MAX_DEVICES) return;
        SDL_GameController *c = SDL_GameControllerOpen(index);
        if (c) { pads[npads++] = c; printf("controller: %s\n", SDL_GameControllerName(c)); }
    } else {
        if (nsticks >= MAX_DEVICES) return;
        SDL_Joystick *j = SDL_JoystickOpen(index);
        if (j) { sticks[nsticks++] = j; printf("joystick (generic layout): %s\n", SDL_JoystickName(j)); }
    }
    rebuild_summary();
}

static void close_instance(SDL_JoystickID id) {
    for (int i = 0; i < npads; i++) {
        if (SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(pads[i])) == id) {
            SDL_GameControllerClose(pads[i]);
            pads[i] = pads[--npads];
            break;
        }
    }
    for (int i = 0; i < nsticks; i++) {
        if (SDL_JoystickInstanceID(sticks[i]) == id) {
            SDL_JoystickClose(sticks[i]);
            sticks[i] = sticks[--nsticks];
            break;
        }
    }
    rebuild_summary();
}

void input_init(void) {
    for (int i = 0; i < SDL_NumJoysticks(); i++) open_device(i);
    rebuild_summary();
}

void input_shutdown(void) {
    for (int i = 0; i < npads; i++) SDL_GameControllerClose(pads[i]);
    for (int i = 0; i < nsticks; i++) SDL_JoystickClose(sticks[i]);
    npads = nsticks = 0;
    rebuild_summary();
}

void input_handle_event(const SDL_Event *e) {
    switch (e->type) {
    case SDL_CONTROLLERDEVICEADDED: open_device(e->cdevice.which); break;       /* which = device index */
    case SDL_JOYDEVICEADDED: open_device(e->jdevice.which); break;
    case SDL_CONTROLLERDEVICEREMOVED: close_instance(e->cdevice.which); break;  /* which = instance id */
    case SDL_JOYDEVICEREMOVED: close_instance(e->jdevice.which); break;
    default: break;
    }
}

uint8_t input_read(const int key[NES_BUTTONS], const int pad[NES_BUTTONS]) {
    const int dead = 10000;
    uint8_t b = 0;
    const Uint8 *ks = SDL_GetKeyboardState(NULL);
    for (int i = 0; i < NES_BUTTONS; i++) {
        SDL_Scancode sc = SDL_GetScancodeFromKey((SDL_Keycode)key[i]);
        if (sc != SDL_SCANCODE_UNKNOWN && ks[sc]) b |= (uint8_t)(1 << i);
    }
    for (int p = 0; p < npads; p++) {
        SDL_GameController *c = pads[p];
        for (int i = 0; i < NES_BUTTONS; i++)
            if (pad[i] >= 0 && pad[i] < SDL_CONTROLLER_BUTTON_MAX && SDL_GameControllerGetButton(c, (SDL_GameControllerButton)pad[i]))
                b |= (uint8_t)(1 << i);
        int ax = SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_LEFTX);
        int ay = SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_LEFTY);
        if (ax < -dead) b |= 0x40; else if (ax > dead) b |= 0x80;
        if (ay < -dead) b |= 0x10; else if (ay > dead) b |= 0x20;
    }
    for (int s = 0; s < nsticks; s++) {
        /* generic layout: buttons 0/1/2 = A/B/B, 6 = select, 7 = start, hat 0 = d-pad, axes 0/1 = stick */
        SDL_Joystick *j = sticks[s];
        int nb = SDL_JoystickNumButtons(j);
        if (nb > 0 && SDL_JoystickGetButton(j, 0)) b |= 0x01;
        if (nb > 1 && SDL_JoystickGetButton(j, 1)) b |= 0x02;
        if (nb > 2 && SDL_JoystickGetButton(j, 2)) b |= 0x02;
        if (nb > 6 && SDL_JoystickGetButton(j, 6)) b |= 0x04;
        if (nb > 7 && SDL_JoystickGetButton(j, 7)) b |= 0x08;
        if (SDL_JoystickNumHats(j) > 0) {
            Uint8 h = SDL_JoystickGetHat(j, 0);
            if (h & SDL_HAT_UP) b |= 0x10;
            if (h & SDL_HAT_DOWN) b |= 0x20;
            if (h & SDL_HAT_LEFT) b |= 0x40;
            if (h & SDL_HAT_RIGHT) b |= 0x80;
        }
        if (SDL_JoystickNumAxes(j) > 1) {
            int ax = SDL_JoystickGetAxis(j, 0), ay = SDL_JoystickGetAxis(j, 1);
            if (ax < -dead) b |= 0x40; else if (ax > dead) b |= 0x80;
            if (ay < -dead) b |= 0x10; else if (ay > dead) b |= 0x20;
        }
    }
    return b;
}

int input_held(int key, int pad) {
    const Uint8 *ks = SDL_GetKeyboardState(NULL);
    SDL_Scancode sc = SDL_GetScancodeFromKey((SDL_Keycode)key);
    if (sc != SDL_SCANCODE_UNKNOWN && ks[sc]) return 1;
    if (pad >= 0 && pad < SDL_CONTROLLER_BUTTON_MAX)
        for (int p = 0; p < npads; p++) if (SDL_GameControllerGetButton(pads[p], (SDL_GameControllerButton)pad)) return 1;
    return 0;
}

/* NES A (jump) on the right face button, NES B (attack) on the bottom one: the NES layout */
void input_default_pads(int pad[NES_BUTTONS]) {
    pad[0] = SDL_CONTROLLER_BUTTON_B;
    pad[1] = SDL_CONTROLLER_BUTTON_A;
    pad[2] = SDL_CONTROLLER_BUTTON_BACK;
    pad[3] = SDL_CONTROLLER_BUTTON_START;
    pad[4] = SDL_CONTROLLER_BUTTON_DPAD_UP;
    pad[5] = SDL_CONTROLLER_BUTTON_DPAD_DOWN;
    pad[6] = SDL_CONTROLLER_BUTTON_DPAD_LEFT;
    pad[7] = SDL_CONTROLLER_BUTTON_DPAD_RIGHT;
}

int input_controller_count(void) { return npads + nsticks; }
const char *input_controller_summary(void) { return summary; }

static const struct { int button; const char *name; } pad_names[] = {
    { SDL_CONTROLLER_BUTTON_A, "A" }, { SDL_CONTROLLER_BUTTON_B, "B" },
    { SDL_CONTROLLER_BUTTON_X, "X" }, { SDL_CONTROLLER_BUTTON_Y, "Y" },
    { SDL_CONTROLLER_BUTTON_BACK, "Back" }, { SDL_CONTROLLER_BUTTON_GUIDE, "Guide" },
    { SDL_CONTROLLER_BUTTON_START, "Start" },
    { SDL_CONTROLLER_BUTTON_LEFTSTICK, "LS" }, { SDL_CONTROLLER_BUTTON_RIGHTSTICK, "RS" },
    { SDL_CONTROLLER_BUTTON_LEFTSHOULDER, "LB" }, { SDL_CONTROLLER_BUTTON_RIGHTSHOULDER, "RB" },
    { SDL_CONTROLLER_BUTTON_DPAD_UP, "D-pad Up" }, { SDL_CONTROLLER_BUTTON_DPAD_DOWN, "D-pad Down" },
    { SDL_CONTROLLER_BUTTON_DPAD_LEFT, "D-pad Left" }, { SDL_CONTROLLER_BUTTON_DPAD_RIGHT, "D-pad Right" },
    { SDL_CONTROLLER_BUTTON_MISC1, "Misc" },
    { SDL_CONTROLLER_BUTTON_PADDLE1, "Paddle 1" }, { SDL_CONTROLLER_BUTTON_PADDLE2, "Paddle 2" },
    { SDL_CONTROLLER_BUTTON_PADDLE3, "Paddle 3" }, { SDL_CONTROLLER_BUTTON_PADDLE4, "Paddle 4" },
    { SDL_CONTROLLER_BUTTON_TOUCHPAD, "Touchpad" },
};

const char *input_pad_name(int button) {
    for (size_t i = 0; i < sizeof pad_names / sizeof pad_names[0]; i++)
        if (pad_names[i].button == button) return pad_names[i].name;
    return "none";
}

int input_pad_from_name(const char *name) {
    for (size_t i = 0; i < sizeof pad_names / sizeof pad_names[0]; i++)
        if (SDL_strcasecmp(pad_names[i].name, name) == 0) return pad_names[i].button;
    int b = SDL_GameControllerGetButtonFromString(name);   /* SDL's own names, e.g. "dpup" */
    return b == SDL_CONTROLLER_BUTTON_INVALID ? -1 : b;
}
