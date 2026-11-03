/*
 * ui_bind_test.c - feeds synthetic keyboard / game-controller events through the
 * menu code and checks that rebinding produces the expected key and pad bindings.
 * Build: see tests/run_ui_bind_test.sh (needs SDL2; no window or device is opened).
 */
#define SDL_MAIN_HANDLED
#include <SDL.h>
#include <stdio.h>
#include <string.h>
#include "ui.h"
#include "input.h"

static int failures;
#define CHECK(cond, msg) do { if (!(cond)) { printf("FAIL: %s\n", msg); failures++; } else printf("ok:   %s\n", msg); } while (0)

static ui_event key(ui_menu *m, SDL_Keycode k, int repeat) {
    SDL_Event e; memset(&e, 0, sizeof e);
    e.type = SDL_KEYDOWN; e.key.keysym.sym = k; e.key.repeat = (Uint8)repeat;
    return ui_handle_event(m, &e);
}

static ui_event pad(ui_menu *m, int button) {
    SDL_Event e; memset(&e, 0, sizeof e);
    e.type = SDL_CONTROLLERBUTTONDOWN; e.cbutton.button = (Uint8)button;
    return ui_handle_event(m, &e);
}

int main(int argc, char **argv) {
    (void)argc; (void)argv;
    SDL_SetMainReady();
    int keyval = SDLK_x, padval = SDL_CONTROLLER_BUTTON_B, other_key = SDLK_z, other_pad = SDL_CONTROLLER_BUTTON_A;
    ui_item items[3];
    memset(items, 0, sizeof items);
    items[0] = (ui_item){ "A (jump)", UI_KEY, 7, &keyval, NULL, 0, 0, 0, 0, NULL, "", &padval };
    items[1] = (ui_item){ "B (attack)", UI_KEY, 8, &other_key, NULL, 0, 0, 0, 0, NULL, "", &other_pad };
    items[2] = (ui_item){ "Reset", UI_ACTION, 9, NULL, NULL, 0, 0, 0, 0, NULL, "", NULL };
    ui_tab tab = { "Controls", items, 3, 0 };
    ui_menu m; memset(&m, 0, sizeof m);
    m.tabs = &tab; m.ntabs = 1; m.focus = 1; m.hover_tab = m.hover_item = -1;

    /* 1. Enter on a row starts capture */
    ui_event e = key(&m, SDLK_RETURN, 0);
    CHECK(e.type == UI_EV_NONE && m.capture == 1, "Enter starts capturing on the A row");

    /* 2. a held Enter's auto-repeat must not be captured as the new key */
    e = key(&m, SDLK_RETURN, 1);
    CHECK(m.capture == 1 && keyval == SDLK_x, "auto-repeated Enter is ignored while capturing");

    /* 3. a controller button press is bound to the pad slot, key untouched */
    e = pad(&m, SDL_CONTROLLER_BUTTON_X);
    CHECK(e.type == UI_EV_CHANGED && e.id == 7 && padval == SDL_CONTROLLER_BUTTON_X && keyval == SDLK_x && m.capture == 0,
          "pad X press rebinds the A row's pad button and reports CHANGED");

    /* 4. keyboard capture: Enter then 'q' binds the key, pad untouched */
    key(&m, SDLK_RETURN, 0);
    e = key(&m, SDLK_q, 0);
    CHECK(e.type == UI_EV_CHANGED && keyval == SDLK_q && padval == SDL_CONTROLLER_BUTTON_X && m.capture == 0,
          "key q rebinds the A row's key");

    /* 5. Escape cancels a capture without changing anything */
    key(&m, SDLK_RETURN, 0);
    e = key(&m, SDLK_ESCAPE, 0);
    CHECK(e.type == UI_EV_NONE && m.capture == 0 && keyval == SDLK_q, "Escape cancels capture");

    /* 6. moving down with the d-pad and binding the B row with the pad's B button (even B, which normally means back) */
    e = pad(&m, SDL_CONTROLLER_BUTTON_DPAD_DOWN);
    CHECK(tab.sel == 1, "d-pad down selects the B row");
    e = pad(&m, SDL_CONTROLLER_BUTTON_A);          /* A = confirm = start capture */
    CHECK(m.capture == 1, "pad A starts capture on the B row");
    e = pad(&m, SDL_CONTROLLER_BUTTON_B);          /* while capturing, B is a binding, not 'back' */
    CHECK(e.type == UI_EV_CHANGED && e.id == 8 && other_pad == SDL_CONTROLLER_BUTTON_B && m.capture == 0,
          "pad B is captured as a binding instead of acting as back");

    /* 7. names round-trip through the ini format */
    CHECK(input_pad_from_name(input_pad_name(SDL_CONTROLLER_BUTTON_DPAD_LEFT)) == SDL_CONTROLLER_BUTTON_DPAD_LEFT, "pad name round-trip (D-pad Left)");
    CHECK(input_pad_from_name("none") == -1 && input_pad_from_name("dpup") == SDL_CONTROLLER_BUTTON_DPAD_UP, "pad names: none and SDL's own names");
    CHECK(SDL_GetKeyFromName(SDL_GetKeyName(SDLK_RSHIFT)) == SDLK_RSHIFT, "key name round-trip (Right Shift)");

    /* 8. the merged NES pad read with no devices: keyboard only (no SDL video, so no keys are down) */
    int keys[8] = { SDLK_x, SDLK_z, SDLK_RSHIFT, SDLK_RETURN, SDLK_UP, SDLK_DOWN, SDLK_LEFT, SDLK_RIGHT }, pads[8];
    input_default_pads(pads);
    CHECK(pads[0] == SDL_CONTROLLER_BUTTON_B && pads[1] == SDL_CONTROLLER_BUTTON_A && pads[3] == SDL_CONTROLLER_BUTTON_START,
          "default pad layout: NES A = pad B, NES B = pad A, Start = Start");
    (void)keys;

    printf("%s (%d failure%s)\n", failures ? "UI BINDING TEST FAILED" : "UI BINDING TEST PASSED", failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
