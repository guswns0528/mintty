// win.c (part of mintty)
// Copyright 2008-10 Andy Koppe
// Based on code from PuTTY-0.60 by Simon Tatham and team.
// Licensed under the terms of the GNU General Public License v3 or later.

#include "winpriv.h"

#include "term.h"
#include "config.h"
#include "appinfo.h"
#include "child.h"
#include "charset.h"

#include <locale.h>
#include <getopt.h>
#include <pwd.h>
#include <imm.h>
#include <winnls.h>
#include <shellapi.h>

#include <sys/cygwin.h>

#if CYGWIN_VERSION_API_MINOR >= 91
#include <argz.h>
#else
int argz_create (char *const argv[], char **argz, size_t *argz_len);
void argz_stringify (char *argz, size_t argz_len, int sep);
#endif

HWND wnd;
HINSTANCE inst;
HDC dc;
static ATOM class_atom;

bool win_is_full_screen;
static bool fullscr_on_max;

static int extra_width, extra_height;

static bool resizing;

static HBITMAP caretbm;
static int caret_x = -1, caret_y = -1;

static char **main_argv;


#if WINVER < 0x500

#define MONITOR_DEFAULTTONEAREST 2 

#define FLASHW_STOP 0
#define FLASHW_CAPTION 1
#define FLASHW_TRAY 2
#define FLASHW_TIMER 4

typedef struct {
  UINT  cbSize;
  HWND  hwnd;
  DWORD dwFlags;
  UINT  uCount;
  DWORD dwTimeout;
} FLASHWINFO, *PFLASHWINFO;

#define LWA_ALPHA	0x02

typedef struct {
  int cxLeftWidth;
  int cxRightWidth;
  int cyTopHeight;
  int cyBottomHeight;
} MARGINS;

#endif

static HMONITOR (WINAPI *pMonitorFromWindow)(HWND,DWORD);
static BOOL (WINAPI *pGetMonitorInfo)(HMONITOR,LPMONITORINFO);
static BOOL (WINAPI *pFlashWindowEx)(PFLASHWINFO);
static BOOL (WINAPI *pSetLayeredWindowAttributes)(HWND,COLORREF,BYTE,DWORD);
static HRESULT (WINAPI *pDwmIsCompositionEnabled)(BOOL *);
static HRESULT (WINAPI *pDwmExtendFrameIntoClientArea)(HWND, const MARGINS *);

static void
load_funcs(void)
{
  HMODULE user = LoadLibrary("user32");
  pMonitorFromWindow = (void *)GetProcAddress(user, "MonitorFromWindow");
  pGetMonitorInfo = (void *)GetProcAddress(user, "GetMonitorInfoA");
  pFlashWindowEx = (void *)GetProcAddress(user, "FlashWindowEx");
  pSetLayeredWindowAttributes =
    (void *)GetProcAddress(user, "SetLayeredWindowAttributes");
  
  HMODULE dwm = LoadLibrary("dwmapi");
  pDwmIsCompositionEnabled =
    (void *)GetProcAddress(dwm, "DwmIsCompositionEnabled");
  pDwmExtendFrameIntoClientArea =
    (void *)GetProcAddress(dwm, "DwmExtendFrameIntoClientArea");
}

void
win_set_timer(void (*cb)(void), uint ticks)
{ SetTimer(wnd, (UINT_PTR)cb, ticks, null); }

void
win_set_title(char *title)
{
  wchar wtitle[strlen(title) + 1];
  if (cs_mbstowcs(wtitle, title, sizeof wtitle))
    SetWindowTextW(wnd, wtitle);
}

void
win_copy_title(void)
{
  int wlen = GetWindowTextLengthW(wnd);
  wchar wtext[wlen + 1];
  wlen = GetWindowTextW(wnd, wtext, wlen + 1);
  win_copy(wtext, 0, wlen + 1);
}

/*
 * Minimise or restore the window in response to a server-side
 * request.
 */
void
win_set_iconic(bool iconic)
{
  if (iconic ^ IsIconic(wnd))
    ShowWindow(wnd, iconic ? SW_MINIMIZE : SW_RESTORE);
}

/*
 * Move the window in response to a server-side request.
 */
void
win_move(int x, int y)
{
  if (!IsZoomed(wnd))
    SetWindowPos(wnd, null, x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER);
}

/*
 * Move the window to the top or bottom of the z-order in response
 * to a server-side request.
 */
void
win_set_zorder(bool top)
{
  SetWindowPos(wnd, top ? HWND_TOP : HWND_BOTTOM, 0, 0, 0, 0,
               SWP_NOMOVE | SWP_NOSIZE);
}

/*
 *  Switch to next or previous application window in z-order
 */
void
win_switch(bool back)
{
  HWND first_wnd = 0, last_wnd;
  BOOL CALLBACK enum_proc(HWND curr_wnd, LPARAM unused(lp)) {
    if (curr_wnd != wnd && !IsIconic(curr_wnd)) {
      WINDOWINFO curr_wnd_info;
      curr_wnd_info.cbSize = sizeof(WINDOWINFO);
      GetWindowInfo(curr_wnd, &curr_wnd_info);
      if (class_atom == curr_wnd_info.atomWindowType) {
        first_wnd = first_wnd ?: curr_wnd;
        last_wnd = curr_wnd;
      }
    }
    return true;
  }
  EnumWindows(enum_proc, 0);
  if (first_wnd) {
    if (back)
      first_wnd = last_wnd;
    else
      SetWindowPos(wnd, last_wnd, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
    BringWindowToTop(first_wnd);
  }
}

/*
 * Report whether the window is iconic, for terminal reports.
 */
bool
win_is_iconic(void)
{ return IsIconic(wnd); }

/*
 * Report the window's position, for terminal reports.
 */
void
win_get_pos(int *x, int *y)
{
  RECT r;
  GetWindowRect(wnd, &r);
  *x = r.left;
  *y = r.top;
}

/*
 * Report the window's pixel size, for terminal reports.
 */
void
win_get_pixels(int *x, int *y)
{
  RECT r;
  GetWindowRect(wnd, &r);
  *x = r.right - r.left;
  *y = r.bottom - r.top;
}

// Clockwork
int get_tick_count(void) { return GetTickCount(); }
int cursor_blink_ticks(void) { return GetCaretBlinkTime(); }

static void
flash_taskbar(bool enable)
{
  static bool enabled;
  if (enable != enabled && pFlashWindowEx) {
    
    pFlashWindowEx(&(FLASHWINFO){
      .cbSize = sizeof(FLASHWINFO),
      .hwnd = wnd,
      .dwFlags = enable ? FLASHW_TRAY | FLASHW_TIMER : FLASHW_STOP,
      .uCount = 1,
      .dwTimeout = 0
    });
    enabled = enable;
  }
}

/*
 * Bell.
 */
void
win_bell(void)
{
  if (cfg.bell_sound)
    MessageBeep(MB_OK);
  if (cfg.bell_taskbar && !term.has_focus)
    flash_taskbar(true);
}

static void
update_sys_cursor(void)
{
  if (term.has_focus && caret_x >= 0 && caret_y >= 0) {
    SetCaretPos(caret_x, caret_y);

    HIMC imc = ImmGetContext(wnd);
    COMPOSITIONFORM cf = {
      .dwStyle = CFS_POINT,
      .ptCurrentPos = {caret_x, caret_y}
    };
    ImmSetCompositionWindow(imc, &cf);
    ImmReleaseContext(wnd, imc);
  }
}

/*
 * Move the system caret. (We maintain one, even though it's
 * invisible, for the benefit of blind people: apparently some
 * helper software tracks the system caret, so we should arrange to
 * have one.)
 */
void
win_set_sys_cursor(int x, int y)
{
  int cx = x * font_width + PADDING;
  int cy = y * font_height + PADDING;
  if (cx != caret_x || cy != caret_y) {
    caret_x = cx;
    caret_y = cy;
    update_sys_cursor();
  }
}

/* Get the rect/size of a full screen window using the nearest available
 * monitor in multimon systems; default to something sensible if only
 * one monitor is present. */
static void
get_fullscreen_rect(RECT *rect)
{
  if (pMonitorFromWindow) {
    HMONITOR mon;
    MONITORINFO mi;
    mon = pMonitorFromWindow(wnd, MONITOR_DEFAULTTONEAREST);
    mi.cbSize = sizeof (mi);
    pGetMonitorInfo(mon, &mi);
    *rect = mi.rcMonitor;
  }
  else
    GetClientRect(GetDesktopWindow(), rect);
}

void
win_resize(int rows, int cols)
{
 /* If the window is maximized suppress resizing attempts */
  if (IsZoomed(wnd) || (rows == term.rows && cols == term.cols)) 
    return;
  
  int width = extra_width + font_width * cols + 2 * PADDING;
  int height = extra_height + font_height * rows + 2 * PADDING;
  SetWindowPos(wnd, null, 0, 0, width, height,
               SWP_NOACTIVATE | SWP_NOCOPYBITS | SWP_NOMOVE | SWP_NOZORDER);
}

void
win_invalidate_all(void)
{
  InvalidateRect(wnd, null, true);
}

static void
resize_window(bool forced)
{
  if (IsIconic(wnd))
    return;

 /* Current window sizes ... */
  RECT cr, wr;
  GetClientRect(wnd, &cr);
  GetWindowRect(wnd, &wr);
  int client_width = cr.right - cr.left;
  int client_height = cr.bottom - cr.top;
  extra_width = wr.right - wr.left - client_width;
  extra_height = wr.bottom - wr.top - client_height;
  int term_width = client_width - 2 * PADDING;
  int term_height = client_height - 2 * PADDING;
  
  if (IsZoomed(wnd) || forced) {
   /* We're fullscreen, or we were told to resize,
    * this means we must not change the size of
    * the window so the terminal has to change.
    */
    int cols = max(1, term_width / font_width);
    int rows = max(1, term_height / font_height);
    if (rows != term.rows || cols != term.cols) {
      term_resize(rows, cols);
      struct winsize ws = {rows, cols, cols * font_width, rows * font_height};
      child_resize(&ws);
    }
    win_invalidate_all();
  }
  else if (term_width != term.cols * font_width ||
           term_height != term.rows * font_height) {
   /* Window size isn't what's needed. Let's change it then. */
    SetWindowPos(wnd, null, 0, 0,
                 font_width * term.cols + 2 * PADDING + extra_width, 
                 font_height * term.rows + 2 * PADDING + extra_height,
                 SWP_NOMOVE | SWP_NOZORDER);
    win_invalidate_all();
  }
}

static void
reinit_fonts(void)
{
  win_deinit_fonts();
  win_init_fonts();
  resize_window(false);
}

bool
win_is_glass_available(void)
{
  BOOL result = false;
  if (pDwmIsCompositionEnabled)
    pDwmIsCompositionEnabled(&result);
  return result;
}

static void
update_glass(void)
{
  if (pDwmExtendFrameIntoClientArea) {
    bool enabled =
      cfg.transparency < 0 && !win_is_fullscreen &&
      !(cfg.opaque_when_focused && term.has_focus);
    pDwmExtendFrameIntoClientArea(wnd, &(MARGINS){enabled ? -1 : 0, 0, 0, 0});
  }
}

/*
 * Go full-screen. This should only be called when we are already
 * maximised.
 */
static void
make_fullscreen(void)
{
  win_is_fullscreen = true;

 /* Remove the window furniture. */
  long style = GetWindowLong(wnd, GWL_STYLE);
  style &= ~(WS_CAPTION | WS_BORDER | WS_THICKFRAME);
  SetWindowLong(wnd, GWL_STYLE, style);

 /* The glass effect doesn't work for fullscreen windows */
  update_glass();

 /* Resize ourselves to exactly cover the nearest monitor. */
  RECT ss;
  get_fullscreen_rect(&ss);
  SetWindowPos(wnd, HWND_TOP, ss.left, ss.top, ss.right - ss.left,
               ss.bottom - ss.top, SWP_FRAMECHANGED);
}

/*
 * Clear the full-screen attributes.
 */
static void
clear_fullscreen(void)
{
  win_is_fullscreen = false;
  update_glass();

 /* Reinstate the window furniture. */
  long style = GetWindowLong(wnd, GWL_STYLE);
  style |= WS_CAPTION | WS_BORDER | WS_THICKFRAME;
  SetWindowLong(wnd, GWL_STYLE, style);
  SetWindowPos(wnd, null, 0, 0, 0, 0,
               SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);
}

/*
 * Maximise or restore the window in response to a server-side.
 * Argument value of 2 means go fullscreen.
 */
void
win_maximise(int max)
{
  if (IsZoomed(wnd)) {
    if (!max)
      ShowWindow(wnd, SW_RESTORE);
    else if (max == 2 && !win_is_fullscreen)
      make_fullscreen();
  }
  else if (max) {
    if (max == 2)
      fullscr_on_max = true;
    ShowWindow(wnd, SW_MAXIMIZE);
  }
}

/*
 * Go back to configured window size.
 */
static void
default_size(void)
{
  if (IsZoomed(wnd))
    ShowWindow(wnd, SW_RESTORE);
  win_resize(cfg.rows, cfg.cols);
}

static void
reset_term(void)
{
  term_reset();
  term_deselect();
  term_clear_scrollback();
  win_update();
}

static void
update_transparency(void)
{
  bool opaque = cfg.opaque_when_focused && term.has_focus;
  if (pSetLayeredWindowAttributes) {
    int trans = max(cfg.transparency, 0);
    long exstyle = GetWindowLong(wnd, GWL_EXSTYLE);
    SetWindowLong(wnd, GWL_EXSTYLE,
                  trans ? exstyle | WS_EX_LAYERED : exstyle & ~WS_EX_LAYERED);
    if (trans) {
      uchar alpha = opaque ? 255 : 255 - 16 * trans;
      pSetLayeredWindowAttributes(wnd, 0, alpha, LWA_ALPHA);
    }
  }
  update_glass();
}

void
win_update_scrollbar(void)
{
  int scrollbar = term.show_scrollbar ? cfg.scrollbar : 0;
  long style = GetWindowLong(wnd, GWL_STYLE);
  SetWindowLong(wnd, GWL_STYLE,
                scrollbar ? style | WS_VSCROLL : style & ~WS_VSCROLL);
  long exstyle = GetWindowLong(wnd, GWL_EXSTYLE);
  SetWindowLong(wnd, GWL_EXSTYLE,
                scrollbar < 0 ? exstyle | WS_EX_LEFTSCROLLBAR 
                              : exstyle & ~WS_EX_LEFTSCROLLBAR);
  SetWindowPos(wnd, null, 0, 0, 0, 0,
               SWP_NOACTIVATE | SWP_NOMOVE |
               SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);
}

void
win_reconfig(void)
{
 /* Pass new config data to the terminal */
  term_reconfig();
  
  bool font_changed =
    memcmp(&new_cfg.font, &cfg.font, sizeof cfg.font) ||
    new_cfg.bold_as_bright != cfg.bold_as_bright ||
    new_cfg.font_quality != cfg.font_quality;
  
  /* Copy the new config and refresh everything */
  cfg = new_cfg;
  if (font_changed) {
    font_size = cfg.font.size;
    reinit_fonts();
  }
  win_update_scrollbar();
  win_reconfig_palette();
  update_transparency();
  win_update_mouse();

  bool old_ambig_wide = cs_ambig_wide;
  cs_reconfig();
  if (term.report_ambig_width && old_ambig_wide != cs_ambig_wide)
    child_write(cs_ambig_wide ? "\e[2W" : "\e[1W", 4);
}

uint
win_get_font_size(void)
{
  return abs(font_size);
}

void
win_set_font_size(int size)
{
  font_size = size ? sgn(font_size) * min(size, 72) : cfg.font.size;
  reinit_fonts();
}

void
win_zoom_font(int zoom)
{
  win_set_font_size(zoom ? max(1, abs(font_size) + zoom) : 0);
}

static bool
confirm_exit(void)
{
  return
    !child_is_parent() ||
    MessageBox(
      wnd,
      "Processes are running in session.\n"
      "Exit anyway?",
      APPNAME, MB_ICONWARNING | MB_OKCANCEL | MB_DEFBUTTON2
    ) == IDOK;
}

void
win_show_about(void)
{
  char *text = asform("%s\n" ABOUT_TEXT, VERSION_TEXT);
  MessageBoxIndirect(&(MSGBOXPARAMS){
    .cbSize = sizeof(MSGBOXPARAMS),
    .hwndOwner = config_wnd,
    .hInstance = inst,
    .lpszCaption = APPNAME,
    .dwStyle = MB_USERICON | MB_OK,
    .lpszIcon = MAKEINTRESOURCE(IDI_MAINICON),
    .lpszText = text
  });
  free(text);
}

static LRESULT CALLBACK
win_proc(HWND wnd, UINT message, WPARAM wp, LPARAM lp)
{
  switch (message) {
    when WM_TIMER: {
      KillTimer(wnd, wp);
      void_fn cb = (void_fn)wp;
      cb();
      return 0;
    }
    when WM_CLOSE:
      win_show_mouse();
      if (!cfg.confirm_exit || confirm_exit())
        child_kill((GetKeyState(VK_SHIFT) & 0x80) != 0);
      return 0;
    when WM_COMMAND or WM_SYSCOMMAND:
      switch (wp & ~0xF) {  /* low 4 bits reserved to Windows */
        when IDM_OPEN: term_open();
        when IDM_COPY: term_copy();
        when IDM_PASTE: win_paste();
        when IDM_SELALL: term_select_all();
        when IDM_RESET: reset_term();
        when IDM_DEFSIZE: default_size();
        when IDM_FULLSCREEN: win_maximise(win_is_fullscreen ? 0 : 2);
        when IDM_FLIPSCREEN: term_flip_screen();
        when IDM_OPTIONS: win_open_config();
        when IDM_NEW: child_fork(main_argv);
        when IDM_COPYTITLE: win_copy_title();
      }
    when WM_VSCROLL:
      switch (LOWORD(wp)) {
        when SB_BOTTOM:   term_scroll(-1, 0);
        when SB_TOP:      term_scroll(+1, 0);
        when SB_LINEDOWN: term_scroll(0, +1);
        when SB_LINEUP:   term_scroll(0, -1);
        when SB_PAGEDOWN: term_scroll(0, +term.rows);
        when SB_PAGEUP:   term_scroll(0, -term.rows);
        when SB_THUMBPOSITION or SB_THUMBTRACK: {
          SCROLLINFO info;
          info.cbSize = sizeof(SCROLLINFO);
          info.fMask = SIF_TRACKPOS;
          GetScrollInfo(wnd, SB_VERT, &info);
          term_scroll(1, info.nTrackPos);
        }
      }
    when WM_LBUTTONDOWN: win_mouse_click(MBT_LEFT, lp);
    when WM_RBUTTONDOWN: win_mouse_click(MBT_RIGHT, lp);
    when WM_MBUTTONDOWN: win_mouse_click(MBT_MIDDLE, lp);
    when WM_LBUTTONUP: win_mouse_release(MBT_LEFT, lp);
    when WM_RBUTTONUP: win_mouse_release(MBT_RIGHT, lp);
    when WM_MBUTTONUP: win_mouse_release(MBT_MIDDLE, lp);
    when WM_MOUSEMOVE: win_mouse_move(false, lp);
    when WM_NCMOUSEMOVE: win_mouse_move(true, lp);
    when WM_MOUSEWHEEL: win_mouse_wheel(wp, lp);
    when WM_KEYDOWN or WM_SYSKEYDOWN:
      if (win_key_down(wp, lp))
        return 0;
    when WM_KEYUP or WM_SYSKEYUP:
      if (win_key_up(wp, lp))
        return 0;
    when WM_CHAR or WM_SYSCHAR:
      {
        wchar wc = wp;
        child_sendw(&wc, 1);
        return 0;
      }
    when WM_INPUTLANGCHANGE:
      update_sys_cursor();
    when WM_IME_STARTCOMPOSITION:
      {
        HIMC imc = ImmGetContext(wnd);
        ImmSetCompositionFont(imc, &lfont);
        ImmReleaseContext(wnd, imc);
      }
    when WM_IME_COMPOSITION:
      if (lp & GCS_RESULTSTR) {
        HIMC imc = ImmGetContext(wnd);
        LONG len = ImmGetCompositionStringW(imc, GCS_RESULTSTR, null, 0);
        if (len > 0) {
          char buf[len];
          ImmGetCompositionStringW(imc, GCS_RESULTSTR, buf, len);
          child_sendw((wchar *)buf, len / 2);
        }
        ImmReleaseContext(wnd, imc);
        return 1;
      }
    when WM_PAINT:
      win_paint();
      return 0;
    when WM_SETFOCUS:
      term_set_focus(true);
      CreateCaret(wnd, caretbm, 0, 0);
      ShowCaret(wnd);
      flash_taskbar(false);  /* stop */
      win_update();
      update_transparency();
    when WM_KILLFOCUS:
      win_show_mouse();
      term_set_focus(false);
      DestroyCaret();
      caret_x = caret_y = -1;   /* ensure caret is replaced next time */
      win_update();
      update_transparency();
    when WM_MOVE: update_sys_cursor();
    when WM_ENTERSIZEMOVE:
      win_enable_tip();
      resizing = true;
    when WM_EXITSIZEMOVE:
      win_disable_tip();
      resizing = false;
      resize_window(true);
    when WM_SIZING: {
     /*
      * This does two jobs:
      * 1) Keep the sizetip uptodate
      * 2) Make sure the window size is _stepped_ in units of the font size.
      */
      LPRECT r = (LPRECT) lp;
      int width = r->right - r->left - extra_width - 2 * PADDING;
      int height = r->bottom - r->top - extra_height - 2 * PADDING;
      int cols = max(1, (float)width / font_width + 0.5);
      int rows = max(1, (float)height / font_height + 0.5);
      
      int ew = width - cols * font_width;
      int eh = height - rows * font_height;
      
      if (wp >= WMSZ_BOTTOM) {
        wp -= WMSZ_BOTTOM;
        r->bottom -= eh;
      }
      else if (wp >= WMSZ_TOP) {
        wp -= WMSZ_TOP;
        r->top += eh;
      }
      
      if (wp == WMSZ_RIGHT)
        r->right -= ew;
      else if (wp == WMSZ_LEFT)
        r->left += ew;
      
      win_update_tip(r->left + extra_width, r->top + extra_height, cols, rows);
      
      return ew || eh;
    }
    when WM_SIZE: {
      if (wp == SIZE_RESTORED && win_is_fullscreen)
        clear_fullscreen();
      else if (wp == SIZE_MAXIMIZED && fullscr_on_max) {
        fullscr_on_max = false;
        make_fullscreen();
      }
      
      if (!resizing)
        resize_window(true);

      update_sys_cursor();
      return 0;
    }
    when WM_INITMENU:
      win_update_menus();
      return 0;
  }
 /*
  * Any messages we don't process completely above are passed through to
  * DefWindowProc() for default processing.
  */
  return DefWindowProcW(wnd, message, wp, lp);
}

static const char help[] =
  "Usage: " APPNAME " [OPTION]... [ PROGRAM [ARG]... | - ]\n"
  "\n"
  "Start a new terminal session running the specified program or the user's shell.\n"
  "If a dash is given instead of a program, invoke the shell as a login shell.\n"
  "\n"
  "Options:\n"
  "  -e, --exec            Treat remaining arguments as the command to execute\n"
  "  -p, --position X,Y    Open window at specified coordinates\n"
  "  -s, --size COLS,ROWS  Set screen size in characters\n"
  "  -w, --window normal|min|max|full  Set initial window state\n"
  "  -t, --title TITLE     Set window title (default: the invoked command)\n"
  "      --class CLASS     Set window class name (default: " APPNAME ")\n"
  "  -i, --icon FILE[,IX]  Load window icon from file, optionally with index\n"
  "  -l, --log FILE        Log output to file\n"
  "  -u, --utmp            Create a utmp entry\n"
  "  -h, --hold never|always|error  Keep window open after command terminates?\n"
  "  -c, --config FILE     Load specified config file\n"
  "  -o, --option OPT=VAL  Override config file option with given value\n"
  "  -H, --help            Display help and exit\n"
  "  -V, --version         Print version information and exit\n"
;

static const char short_opts[] = "+:HVuec:o:p:s:w:t:i:l:h:";

static const struct option
opts[] = { 
  {"help",     no_argument,       0, 'H'},
  {"version",  no_argument,       0, 'V'},
  {"exec",     no_argument,       0, 'e'},
  {"utmp",     no_argument,       0, 'u'},
  {"config",   required_argument, 0, 'c'},
  {"option",   required_argument, 0, 'o'},
  {"position", required_argument, 0, 'p'},
  {"size",     required_argument, 0, 's'},
  {"window",   required_argument, 0, 'w'},
  {"title",    required_argument, 0, 't'},
  {"class",    required_argument, 0, 'C'},
  {"icon",     required_argument, 0, 'i'},
  {"log",      required_argument, 0, 'l'},
  {"hold",     required_argument, 0, 'h'},
  {0, 0, 0, 0}
};

static void
show_msg(FILE *stream, const char *msg)
{
  if (fputs(msg, stream) < 0 || fflush(stream) < 0)
    MessageBox(0, msg, APPNAME, MB_OK);
}

static no_return __attribute__((format(printf, 2, 3)))
error(bool syntax, char *format, ...) 
{
  char *msg;
  va_list va;
  va_start(va, format);
  vasprintf(&msg, format, va);
  va_end(va);
  msg = asform("%s: %s\n%s", main_argv[0], msg,
               syntax ? "Try '--help' for more information.\n" : "");
  show_msg(stderr, msg);
  exit(1);
}

typedef struct {
  const char *name;
  uint val;
} optarg_mapping;

optarg_mapping
window_optargs[] = {
  {"normal", SW_SHOWNORMAL},
  {"min", SW_SHOWMINIMIZED},
  {"max", SW_SHOWMAXIMIZED},
  {"full", 0},
  {0, 0}
};

optarg_mapping
hold_optargs[] = {
  {"always", HOLD_ALWAYS},
  {"never", HOLD_NEVER},
  {"error", HOLD_ERROR},
  {0, 0}
};

static uint
lookup_optarg(char *opt, char *arg, optarg_mapping *mappings)
{
  int len = strlen(arg);
  while (mappings->name) {
    if (!memcmp(arg, mappings->name, len))
      return mappings->val;
    mappings++;
  }
  error(true, "invalid argument '%s' to option '%s'", arg, opt);
}

int
main(int argc, char *argv[])
{
  main_argv = argv;
  setlocale(LC_CTYPE, "");

  STARTUPINFO sui;
  GetStartupInfo(&sui);
  uint show = sui.dwFlags & STARTF_USESHOWWINDOW ? sui.wShowWindow : SW_SHOW;
  
  char *title = 0, *icon_file = 0;
  int x = CW_USEDEFAULT, y = CW_USEDEFAULT;
  uint rows = 0, cols = 0;
  wchar *class_name = _W(APPNAME);
  
#if CYGWIN_VERSION_DLL_MAJOR >= 1005
  // Before Cygwin 1.5, the passwd structure is faked.
  struct passwd *pw = getpwuid(getuid());
#endif
  
  home =
    getenv("HOME") ?:
#if CYGWIN_VERSION_DLL_MAJOR >= 1005
    (pw && pw->pw_dir && *pw->pw_dir) ? pw->pw_dir :
#endif
    asform("/home/%s", getlogin());

  load_config("/etc/minttyrc");
  
  char *rc_file = asform("%s/.minttyrc", home);
  load_config(rc_file);
  free(rc_file);

  for (;;) {
    int opt = getopt_long(argc, argv, short_opts, opts, 0);
    if (opt == -1 || opt == 'e')
      break;
    char *longopt = argv[optind - 1], *shortopt = (char[]){'-', optopt, 0};
    switch (opt) {
      when 'c': load_config(optarg);
      when 'o': parse_option(optarg);
      when 't': title = optarg;
      when 'i': icon_file = optarg;
      when 'l': log_file = optarg;
      when 'u': utmp_enabled = true;
      when 'p':
        if (sscanf(optarg, "%i,%i%1s", &x, &y, (char[2]){}) != 2)
          error(true, "syntax error in position argument '%s'", optarg);
      when 's':
        if (sscanf(optarg, "%u,%u%1s", &cols, &rows, (char[2]){}) != 2)
          error(true, "syntax error in size argument '%s'", optarg);
      when 'w':
        show = lookup_optarg("window", optarg, window_optargs);
      when 'h':
        hold = lookup_optarg("hold", optarg, hold_optargs);
      when 'C': {
        int len = mbstowcs(0, optarg, 0);
        if (len < 0)
          error(false, "invalid character in class name '%s'", optarg);
        class_name = newn(wchar, len + 1);
        mbstowcs(class_name, optarg, len + 1);
      }
      when 'H':
        show_msg(stdout, help);
        return 0;
      when 'V':
        show_msg(stdout, VERSION_TEXT);
        return 0;
      when '?':
        error(true, "unknown option '%s'", optopt ? shortopt : longopt);
      when ':':
        error(true, "option '%s' requires an argument",
              longopt[1] == '-' ? longopt : shortopt);
    }
  }
  
  load_funcs();

  HICON small_icon = 0, large_icon = 0;
  if (icon_file) {
    uint icon_index = 0;
    char *comma = strrchr(icon_file, ',');
    if (comma) {
      char *start = comma + 1, *end;
      icon_index = strtoul(start, &end, 0);
      if (start != end && !*end)
        *comma = 0;
      else
        icon_index = 0;
    }
#if CYGWIN_VERSION_API_MINOR >= 181
    wchar *win_icon_file = cygwin_create_path(CCP_POSIX_TO_WIN_W, icon_file);
    if (!win_icon_file)
      error(false, "invalid icon file path '%s'", icon_file);
    ExtractIconExW(win_icon_file, icon_index, &large_icon, &small_icon, 1);
    free(win_icon_file);
#else
    char win_icon_file[MAX_PATH];
    cygwin_conv_to_win32_path(icon_file, win_icon_file);
    ExtractIconExA(win_icon_file, icon_index, &large_icon, &small_icon, 1);
#endif
    if (!small_icon || !large_icon)
      error(false, "could not load icon from '%s'", icon_file);
  }

  // Work out what to execute.
  argv += optind;
  if (*argv && (argv[1] || strcmp(*argv, "-")))
    cmd = *argv;
  else {
    // Look up the user's shell.
    cmd =
      getenv("SHELL") ?:
#if CYGWIN_VERSION_DLL_MAJOR >= 1005
      (pw && pw->pw_shell && *pw->pw_shell) ? pw->pw_shell :
#endif
      "/bin/sh";
    char *slash = strrchr(cmd, '/');
    char *arg0 = slash ? slash + 1 : cmd;
    if (*argv)
      arg0 = asform("-%s", arg0);
    argv = (char *[]){arg0, 0};
  }
  
  // Put child command line into window title if we haven't got one already.
  if (!title) {
    size_t len;
    argz_create(argv, &title, &len);
    argz_stringify(title, len, ' ');
  }

  wchar wtitle[strlen(title) + 1];
  if (mbstowcs(wtitle, title, sizeof wtitle) == (size_t)-1)
    *wtitle = 0;

  rows = rows ?: max(1, cfg.rows);
  cols = cols ?: max(1, cfg.cols);

  inst = GetModuleHandle(NULL);

  // The window class.
  class_atom = RegisterClassExW(&(WNDCLASSEXW){
    .cbSize = sizeof(WNDCLASSEXW),
    .style = 0,
    .lpfnWndProc = win_proc,
    .cbClsExtra = 0,
    .cbWndExtra = 0,
    .hInstance = inst,
    .hIcon = large_icon ?: LoadIcon(inst, MAKEINTRESOURCE(IDI_MAINICON)),
    .hIconSm = small_icon,
    .hCursor = LoadCursor(null, IDC_IBEAM),
    .hbrBackground = null,
    .lpszMenuName = null,
    .lpszClassName = class_name,
  });

  // Initialise the fonts, thus also determining their width and height.
  font_size = cfg.font.size;
  win_init_fonts();
  
  // Determine window size.
  int term_width = font_width * cols;
  int term_height = font_height * rows;

  RECT cr = {0, 0, term_width + 2 * PADDING, term_height + 2 * PADDING};
  RECT wr = cr;
  AdjustWindowRect(&wr, WS_OVERLAPPEDWINDOW, false);
  int width = wr.right - wr.left;
  int height = wr.bottom - wr.top;
  
  if (cfg.scrollbar)
    width += GetSystemMetrics(SM_CXVSCROLL);
  
  extra_width = width - (cr.right - cr.left);
  extra_height = height - (cr.bottom - cr.top);
  
  // Create initial window.
  wnd = CreateWindowExW(cfg.scrollbar < 0 ? WS_EX_LEFTSCROLLBAR : 0,
                        class_name, wtitle,
                        WS_OVERLAPPEDWINDOW | (cfg.scrollbar ? WS_VSCROLL : 0),
                        x, y, width, height,
                        null, null, inst, null);

  // Initialise the terminal.
  term_reset();
  term_resize(rows, cols);

  // Initialise the scroll bar.
  SetScrollInfo(
    wnd, SB_VERT,
    &(SCROLLINFO){
      .cbSize = sizeof(SCROLLINFO),
      .fMask = SIF_ALL | SIF_DISABLENOSCROLL,
      .nMin = 0, .nMax = rows - 1,
      .nPage = rows, .nPos = 0,
    },
    false
  );

  // Set up an empty caret bitmap. We're painting the cursor manually.
  caretbm = CreateBitmap(1, font_height, 1, 1, newn(short, font_height));
  CreateCaret(wnd, caretbm, 0, 0);

  // Initialise various other stuff.
  win_reset_colours();
  win_init_drop_target();
  win_init_menus();
  update_transparency();
  
  // Create child process.
  child_create(argv, &(struct winsize){rows, cols, term_width, term_height});

  // Finally show the window!
  fullscr_on_max = !show;
  ShowWindow(wnd, show ?: SW_SHOWMAXIMIZED);

  // Message loop.
  for (;;) {
    MSG msg;
    while (PeekMessage(&msg, null, 0, 0, PM_REMOVE)) {
      if (msg.message == WM_QUIT)
        return msg.wParam;
      if (!IsDialogMessage(config_wnd, &msg))
        DispatchMessage(&msg);
    }
    child_proc();
  }
}
