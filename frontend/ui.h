/* ui.h - small keyboard/gamepad/mouse driven tabbed menu for the launcher and pause screen */
#ifndef UI_H
#define UI_H
#include <SDL.h>

enum { UI_ACTION, UI_CHOICE, UI_INT, UI_KEY, UI_INFO };

typedef struct ui_item {
    const char *label;
    int kind;
    int id;                       /* reported to the application */
    int *value;                   /* CHOICE/INT: current value; KEY: SDL_Keycode */
    const char *const *choices;   /* CHOICE names */
    int nchoices;
    int min, max, step;           /* INT */
    const char *suffix;           /* INT: text after the number */
    char info[320];               /* INFO: free text, '\n' separates lines */
    int *pad;                     /* KEY: optional game-controller button bound alongside the key (-1 = none) */
} ui_item;

typedef struct {
    const char *name;
    ui_item *items;
    int nitems;
    int sel;
} ui_tab;

#define UI_MAX_TABS 8
#define UI_MAX_ITEMS 24

typedef struct ui_menu {
    ui_tab *tabs;
    int ntabs;
    int tab;                      /* selected tab */
    int focus;                    /* 0 = tab column, 1 = items */
    int capture;                  /* waiting for a key to bind */
    const char *title, *subtitle;
    const char *hint;             /* bottom line */
    char status[240];             /* line above the hint */
    int backdrop;                 /* 1: a picture is behind the menu, draw translucent panels */
    /* layout cache for the mouse, filled by ui_draw */
    SDL_Rect tab_rects[UI_MAX_TABS];
    SDL_Rect item_rects[UI_MAX_ITEMS];
    SDL_Rect left_arrow[UI_MAX_ITEMS], right_arrow[UI_MAX_ITEMS];
    int hover_tab, hover_item;
} ui_menu;

enum { UI_EV_NONE = 0, UI_EV_ACTIVATE, UI_EV_CHANGED, UI_EV_BACK };
typedef struct { int type; int id; } ui_event;

void     ui_draw(SDL_Renderer *r, ui_menu *m, int ww, int wh);
ui_event ui_handle_event(ui_menu *m, const SDL_Event *e);
void     ui_select_first(ui_menu *m);   /* move the item selection to the first selectable item of the tab */

#endif
