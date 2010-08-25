#ifndef WINPRIV_H
#define WINPRIV_H

#include "win.h"
#include "winids.h"

#include <winbase.h>
#include <wingdi.h>
#include <winuser.h>

extern HWND wnd;        // the main terminal window
extern HWND config_wnd; // the options window
extern HINSTANCE inst;  // The all-important instance handle

extern COLORREF colours[COLOUR_NUM];

extern LOGFONT lfont;

extern enum bold_mode { BOLD_COLOURS, BOLD_SHADOW, BOLD_FONT } bold_mode;

extern int font_size;
extern int font_width, font_height;
extern bool font_ambig_wide;

enum { PADDING = 1 };

void win_paint(void);

void win_init_fonts(void);
void win_deinit_fonts(void);

void win_open_config(void);

void win_enable_tip(void);
void win_disable_tip(void);
void win_update_tip(int x, int y, int cols, int rows);

void win_init_menus(void);
void win_update_menus(void);

void win_show_mouse(void);
void win_mouse_click(mouse_button, LPARAM);
void win_mouse_release(mouse_button, LPARAM);
void win_mouse_wheel(WPARAM, LPARAM);
void win_mouse_move(bool nc, LPARAM);

bool win_key_down(WPARAM, LPARAM);
bool win_key_up(WPARAM, LPARAM);

void win_init_drop_target(void);

void win_copy_title(void);

void win_switch(bool back);

bool win_is_fullscreen;

#endif
