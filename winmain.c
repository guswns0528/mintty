// win.c (part of MinTTY)
// Copyright 2008-09 Andy Koppe
// Based on code from PuTTY-0.60 by Simon Tatham and team.
// Licensed under the terms of the GNU General Public License v3 or later.

#include "winpriv.h"

#include "term.h"
#include "config.h"
#include "appinfo.h"
#include "linedisc.h"
#include "child.h"

#include <process.h>
#include <getopt.h>
#include <imm.h>
#include <winnls.h>

HWND wnd;
HINSTANCE inst;
HDC dc;
static HBITMAP caretbm;

int offset_width, offset_height;
static int extra_width, extra_height;
static int caret_x = -1, caret_y = -1;

static bool resizing;
static bool was_zoomed;
static int prev_rows, prev_cols;

static bool flashing;

static bool child_dead;

static char **main_argv;

void
win_set_timer(void (*cb)(void), uint ticks)
{ SetTimer(wnd, (UINT_PTR)cb, ticks, null); }

void
win_set_title(char *title)
{ SetWindowText(wnd, title); }

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
 * Maximise or restore the window in response to a server-side
 * request.
 */
void
win_set_zoom(bool zoom)
{
  if (zoom ^ IsZoomed(wnd))
    ShowWindow(wnd, zoom ? SW_MAXIMIZE : SW_RESTORE);
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

static BOOL
flash_window_ex(DWORD dwFlags, UINT uCount, DWORD dwTimeout)
{
  FLASHWINFO fi;
  fi.cbSize = sizeof (fi);
  fi.hwnd = wnd;
  fi.dwFlags = dwFlags;
  fi.uCount = uCount;
  fi.dwTimeout = dwTimeout;
  return FlashWindowEx(&fi);
}

/*
 * Manage window caption / taskbar flashing, if enabled.
 * 0 = stop, 1 = maintain, 2 = start
 */
static void
flash_window(int mode)
{
  if ((mode == 0) || (cfg.bell_ind == B_IND_DISABLED)) {
   /* stop */
    if (flashing) {
      flashing = 0;
      flash_window_ex(FLASHW_STOP, 0, 0);
    }
  }
  else if (mode == 2) {
   /* start */
    if (!flashing) {
      flashing = 1;
     /* For so-called "steady" mode, we use uCount=2, which
      * seems to be the traditional number of flashes used
      * by user notifications (e.g., by Explorer).
      * uCount=0 appears to enable continuous flashing, per
      * "flashing" mode, although I haven't seen this
      * documented. */
      flash_window_ex(FLASHW_ALL | FLASHW_TIMER,
                      (cfg.bell_ind == B_IND_FLASH ? 0 : 2),
                      0 /* system cursor blink rate */ );
    }
  }
}

/*
 * Bell.
 */
void
win_bell(int mode)
{
  if (mode == BELL_SOUND) {
   /*
    * For MessageBeep style bells, we want to be careful of timing,
    * because they don't have the nice property that each one cancels out 
    * the previous active one. So we limit the rate to one per 50ms or so.
    */
    static int lastbell = 0;
    int belldiff = GetTickCount() - lastbell;
    if (belldiff >= 0 && belldiff < 50)
      return;
    MessageBeep(MB_OK);
    lastbell = GetTickCount();
  }

  if (!term_has_focus())
    flash_window(2);
}

/*
 * Move the system caret. (We maintain one, even though it's
 * invisible, for the benefit of blind people: apparently some
 * helper software tracks the system caret, so we should arrange to
 * have one.)
 */

static void
sys_cursor_update(void)
{
  COMPOSITIONFORM cf;
  HIMC hIMC;

  if (!term_has_focus())
    return;

  if (caret_x < 0 || caret_y < 0)
    return;

  SetCaretPos(caret_x, caret_y);

  hIMC = ImmGetContext(wnd);
  cf.dwStyle = CFS_POINT;
  cf.ptCurrentPos.x = caret_x;
  cf.ptCurrentPos.y = caret_y;
  ImmSetCompositionWindow(hIMC, &cf);

  ImmReleaseContext(wnd, hIMC);
}

void
win_sys_cursor(int x, int y)
{
  if (!term_has_focus())
    return;

 /*
  * Avoid gratuitously re-updating the cursor position and IMM
  * window if there's no actual change required.
  */
  int cx = x * font_width + offset_width;
  int cy = y * font_height + offset_height;
  if (cx == caret_x && cy == caret_y)
    return;
  caret_x = cx;
  caret_y = cy;

  sys_cursor_update();
}

/* Get the rect/size of a full screen window using the nearest available
 * monitor in multimon systems; default to something sensible if only
 * one monitor is present. */
static int
get_fullscreen_rect(RECT * ss)
{
  HMONITOR mon;
  MONITORINFO mi;
  mon = MonitorFromWindow(wnd, MONITOR_DEFAULTTONEAREST);
  mi.cbSize = sizeof (mi);
  GetMonitorInfo(mon, &mi);

 /* structure copy */
  *ss = mi.rcMonitor;
  return true;
}

static void
notify_resize(int rows, int cols)
{
  term_resize(rows, cols);
  win_update();
  struct winsize ws = {rows, cols, cols * font_width, rows * font_height};
  child_resize(&ws);
}

void
win_resize(int rows, int cols)
{
 /* If the window is maximized supress resizing attempts */
  if (IsZoomed(wnd) || (rows == term_rows() && cols == term_cols())) 
    return;
    
 /* Sanity checks ... */
  static int first_time = 1;
  static RECT ss;
  switch (first_time) {
    when 1:
     /* Get the size of the screen */
      if (!get_fullscreen_rect(&ss))
        first_time = 2;
    when 0: {
     /* Make sure the values are sane */
      int max_cols = (ss.right - ss.left - extra_width) / 4;
      int max_rows = (ss.bottom - ss.top - extra_height) / 6;
      if (cols > max_cols || rows > max_rows)
        return;
      if (cols < 15)
        cols = 15;
      if (rows < 1)
        rows = 1;
    }
  }

  notify_resize(rows, cols);
  int width = extra_width + font_width * cols;
  int height = extra_height + font_height * rows;
  SetWindowPos(wnd, null, 0, 0, width, height,
               SWP_NOACTIVATE | SWP_NOCOPYBITS | SWP_NOMOVE | SWP_NOZORDER);
  InvalidateRect(wnd, null, true);
}

static void
reset_window(int reinit)
{
 /*
  * This function decides how to resize or redraw when the 
  * user changes something. 
  *
  * This function doesn't like to change the terminal size but if the
  * font size is locked that may be it's only soluion.
  */

 /* Current window sizes ... */
  RECT cr, wr;
  GetWindowRect(wnd, &wr);
  GetClientRect(wnd, &cr);

  int win_width = cr.right - cr.left;
  int win_height = cr.bottom - cr.top;

 /* Are we being forced to reload the fonts ? */
  if (reinit > 1) {
    win_deinit_fonts();
    win_init_fonts();
  }

 /* Oh, looks like we're minimised */
  if (win_width == 0 || win_height == 0)
    return;

  int cols = term_cols(), rows = term_rows();

 /* Is the window out of position ? */
  if (!reinit &&
      (offset_width != (win_width - font_width * cols) / 2 ||
       offset_height != (win_height - font_height * rows) / 2)) {
    offset_width = (win_width - font_width * cols) / 2;
    offset_height = (win_height - font_height * rows) / 2;
    InvalidateRect(wnd, null, true);
  }

  if (IsZoomed(wnd)) {
   /* We're fullscreen, this means we must not change the size of
    * the window so it's the font size or the terminal itself.
    */

    extra_width = wr.right - wr.left - cr.right + cr.left;
    extra_height = wr.bottom - wr.top - cr.bottom + cr.top;

    if (font_width * cols != win_width ||
        font_height * rows != win_height) {
     /* Our only choice at this point is to change the 
      * size of the terminal; Oh well.
      */
      rows = win_height / font_height;
      cols = win_width / font_width;
      offset_height = (win_height % font_height) / 2;
      offset_width = (win_width % font_width) / 2;
      notify_resize(rows, cols);
      InvalidateRect(wnd, null, true);
    }
    return;
  }

 /* Hmm, a force re-init means we should ignore the current window
  * so we resize to the default font size.
  */
  if (reinit > 0) {
    offset_width = offset_height = 0;
    extra_width = wr.right - wr.left - cr.right + cr.left;
    extra_height = wr.bottom - wr.top - cr.bottom + cr.top;

    if (win_width != font_width * cols ||
        win_height != font_height * rows) {

     /* If this is too large windows will resize it to the maximum
      * allowed window size, we will then be back in here and resize
      * the font or terminal to fit.
      */
      SetWindowPos(wnd, null, 0, 0, font_width * cols + extra_width,
                   font_height * rows + extra_height,
                   SWP_NOMOVE | SWP_NOZORDER);
    }

    InvalidateRect(wnd, null, true);
    return;
  }

 /* Okay the user doesn't want us to change the font so we try the 
  * window. But that may be too big for the screen which forces us
  * to change the terminal.
  */
  offset_width = offset_height = 0;
  extra_width = wr.right - wr.left - cr.right + cr.left;
  extra_height = wr.bottom - wr.top - cr.bottom + cr.top;

  if (win_width != font_width * cols ||
      win_height != font_height * rows) {

    static RECT ss;
    get_fullscreen_rect(&ss);

    int win_rows = (ss.bottom - ss.top - extra_height) / font_height;
    int win_cols = (ss.right - ss.left - extra_width) / font_width;

   /* Grrr too big */
    if (rows > win_rows || cols > win_cols) {
      rows = min(rows, win_rows);
      cols = min(cols, win_cols);
      notify_resize(rows, cols);
    }

    SetWindowPos(wnd, null, 0, 0,
                 font_width * cols + extra_width,
                 font_height * rows + extra_height,
                 SWP_NOMOVE | SWP_NOZORDER);

    InvalidateRect(wnd, null, true);
  }
}

/*
 * Go full-screen. This should only be called when we are already
 * maximised.
 */
static void
make_full_screen()
{
 /* Remove the window furniture. */
  DWORD style = GetWindowLongPtr(wnd, GWL_STYLE);
  style &= ~(WS_CAPTION | WS_BORDER | WS_THICKFRAME);
  SetWindowLongPtr(wnd, GWL_STYLE, style);

 /* Resize ourselves to exactly cover the nearest monitor. */
  RECT ss;
  get_fullscreen_rect(&ss);
  SetWindowPos(wnd, HWND_TOP, ss.left, ss.top, ss.right - ss.left,
               ss.bottom - ss.top, SWP_FRAMECHANGED);

 /* We may have changed size as a result */
  reset_window(0);
}

/*
 * Clear the full-screen attributes.
 */
static void
clear_full_screen()
{
  DWORD oldstyle, style;

 /* Reinstate the window furniture. */
  style = oldstyle = GetWindowLongPtr(wnd, GWL_STYLE);
  style |= WS_CAPTION | WS_BORDER | WS_THICKFRAME;
  if (style != oldstyle) {
    SetWindowLongPtr(wnd, GWL_STYLE, style);
    SetWindowPos(wnd, null, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);
  }
}

/*
 * Toggle full-screen mode.
 */
static void
flip_full_screen()
{
  if (IsZoomed(wnd)) {
    if (GetWindowLongPtr(wnd, GWL_STYLE) & WS_CAPTION)
      make_full_screen();
    else
      ShowWindow(wnd, SW_RESTORE);
  }
  else {
    SendMessage(wnd, WM_FULLSCR_ON_MAX, 0, 0);
    ShowWindow(wnd, SW_MAXIMIZE);
  }
}

static void
update_transparency()
{
  uchar trans = cfg.transparency;
  SetWindowLong(wnd, GWL_EXSTYLE, trans ? WS_EX_LAYERED : 0);
  if (trans) {
    bool opaque = cfg.opaque_when_focused && term_has_focus();
    uchar alpha = opaque ? 255 : 255 - 16 * trans;
    SetLayeredWindowAttributes(wnd, 0, alpha, LWA_ALPHA);
  }
}

void
win_reconfig(void)
{
 /*
  * Flush the line discipline's edit buffer in the
  * case where local editing has just been disabled.
  */
  ldisc_send(null, 0, 0);

 /* Pass new config data to the terminal */
  term_reconfig();

 /* Enable or disable the scroll bar, etc */
  int init_lvl = 1;
  if (new_cfg.scrollbar != cfg.scrollbar) {
    LONG flag = GetWindowLongPtr(wnd, GWL_STYLE);
    if (new_cfg.scrollbar)
      flag |= WS_VSCROLL;
    else
      flag &= ~WS_VSCROLL;
    SetWindowLongPtr(wnd, GWL_STYLE, flag);
    SetWindowPos(wnd, null, 0, 0, 0, 0,
                SWP_NOACTIVATE | SWP_NOCOPYBITS | SWP_NOMOVE |
                 SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);
    init_lvl = 2;
  }
  
  if (memcmp(&new_cfg.font, &cfg.font, sizeof cfg.font) != 0 ||
      strcmp(new_cfg.codepage, cfg.codepage) != 0)
    init_lvl = 2;

  /* Copy the new config and refresh everything */
  cfg = new_cfg;
  win_reconfig_palette();
  update_transparency();
  InvalidateRect(wnd, null, true);
  reset_window(init_lvl);
  win_update_mouse();
}

static bool
confirm_close(void)
{
  // Only ask once.
  static bool confirmed = false;
  confirmed |=
    !child_is_parent() ||
    MessageBox(
      wnd,
      "Processes are running in session.\n"
      "Exit anyway?",
      APPNAME, MB_ICONWARNING | MB_OKCANCEL | MB_DEFBUTTON2
    ) == IDOK;
  return confirmed;
}

static LRESULT CALLBACK
win_proc(HWND wnd, UINT message, WPARAM wp, LPARAM lp)
{
  static int ignore_clip = false;
  static int need_backend_resize = false;
  static int fullscr_on_max = false;

  switch (message) {
    when WM_TIMER: {
      KillTimer(wnd, wp);
      void_fn cb = (void_fn)wp;
      cb();
      return 0;
    }
    when WM_CLOSE:
      win_show_mouse();
      if (child_dead)
        exit(0);
      if (confirm_close())
        child_kill();
      return 0;
    when WM_COMMAND or WM_SYSCOMMAND:
      switch (wp & ~0xF) {  /* low 4 bits reserved to Windows */
        when IDM_COPY: term_copy();
        when IDM_PASTE: win_paste();
        when IDM_SELALL:
          term_select_all();
          win_update();
        when IDM_RESET:
          term_reset();
          term_deselect();
          term_clear_scrollback();
          win_update();
          ldisc_send(null, 0, 0);
        when IDM_ABOUT: win_show_about();
        when IDM_FULLSCREEN: flip_full_screen();
        when IDM_OPTIONS: win_open_config();
        when IDM_DUPLICATE:
          spawnv(_P_DETACH, "/proc/self/exe", (void *) main_argv);
          
      }
    when WM_VSCROLL:
      if (term_which_screen() == 0) {
        switch (LOWORD(wp)) {
          when SB_BOTTOM:   term_scroll(-1, 0);
          when SB_TOP:      term_scroll(+1, 0);
          when SB_LINEDOWN: term_scroll(0, +1);
          when SB_LINEUP:   term_scroll(0, -1);
          when SB_PAGEDOWN: term_scroll(0, +term_rows());
          when SB_PAGEUP:   term_scroll(0, -term_rows());
          when SB_THUMBPOSITION or SB_THUMBTRACK: term_scroll(1, HIWORD(wp));
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
    when WM_KEYDOWN or WM_SYSKEYDOWN: if (win_key_down(wp, lp)) return 0;
    when WM_KEYUP or WM_SYSKEYUP: if (win_key_up(wp, lp)) return 0;
    when WM_CHAR or WM_SYSCHAR: { // TODO: handle wchar and WM_UNICHAR
      char c = (uchar) wp;
      term_seen_key_event();
      lpage_send(CP_ACP, &c, 1, 1);
      return 0;
    }
    when WM_INPUTLANGCHANGE:
      sys_cursor_update();
    when WM_IME_STARTCOMPOSITION: {
      HIMC hImc = ImmGetContext(wnd);
      ImmSetCompositionFont(hImc, &lfont);
      ImmReleaseContext(wnd, hImc);
    }
    when WM_IGNORE_CLIP:
      ignore_clip = wp;     /* don't panic on DESTROYCLIPBOARD */
    when WM_DESTROYCLIPBOARD:
      if (!ignore_clip)
        term_deselect();
      ignore_clip = false;
      return 0;
    when WM_PAINT:
      win_paint();
      return 0;
    when WM_SETFOCUS:
      if (!child_dead) {
        term_set_focus(true);
        CreateCaret(wnd, caretbm, font_width, font_height);
        ShowCaret(wnd);
      }
      flash_window(0);  /* stop */
      win_update();
      update_transparency();
    when WM_KILLFOCUS:
      win_show_mouse();
      term_set_focus(false);
      DestroyCaret();
      caret_x = caret_y = -1;   /* ensure caret is replaced next time */
      win_update();
      update_transparency();
    when WM_FULLSCR_ON_MAX: fullscr_on_max = true;
    when WM_MOVE: sys_cursor_update();
    when WM_ENTERSIZEMOVE:
      win_enable_tip();
      resizing = true;
      need_backend_resize = false;
    when WM_EXITSIZEMOVE:
      win_disable_tip();
      resizing = false;
      if (need_backend_resize) {
        notify_resize(cfg.rows, cfg.cols);
        InvalidateRect(wnd, null, true);
      }
    when WM_SIZING: {
     /*
      * This does two jobs:
      * 1) Keep the sizetip uptodate
      * 2) Make sure the window size is _stepped_ in units of the font size.
      */
      LPRECT r = (LPRECT) lp;
      int width = r->right - r->left - extra_width;
      int height = r->bottom - r->top - extra_height;
      int w = (width + font_width / 2) / font_width;
      if (w < 1)
        w = 1;
      int h = (height + font_height / 2) / font_height;
      if (h < 1)
        h = 1;
      win_update_tip(wnd, w, h);
      int ew = width - w * font_width;
      int eh = height - h * font_height;
      if (ew != 0) {
        if (wp == WMSZ_LEFT || wp == WMSZ_BOTTOMLEFT ||
            wp == WMSZ_TOPLEFT)
          r->left += ew;
        else
          r->right -= ew;
      }
      if (eh != 0) {
        if (wp == WMSZ_TOP || wp == WMSZ_TOPRIGHT ||
            wp == WMSZ_TOPLEFT)
          r->top += eh;
        else
          r->bottom -= eh;
      }
      return ew || eh;
    }
    when WM_SIZE: {
      if (wp == SIZE_RESTORED)
        clear_full_screen();
      if (wp == SIZE_MAXIMIZED && fullscr_on_max) {
        fullscr_on_max = false;
        make_full_screen();
      }
      int width = LOWORD(lp);
      int height = HIWORD(lp);
      if (resizing) {
       /*
        * Don't call child_size in mid-resize. (To prevent
        * massive numbers of resize events getting sent
        * down the connection during an NT opaque drag.)
        */
        need_backend_resize = true;
      }
      else if (wp == SIZE_MAXIMIZED && !was_zoomed) {
        was_zoomed = 1;
        prev_rows = term_rows();
        prev_cols = term_cols();
        int rows = max(1, height / font_height);
        int cols = max(1, width / font_width);
        notify_resize(rows, cols);
        reset_window(0);
      }
      else if (wp == SIZE_RESTORED && was_zoomed) {
        was_zoomed = 0;
        notify_resize(prev_rows, prev_cols);
        reset_window(2);
      }
      else {
       /* This is an unexpected resize, these will normally happen
        * if the window is too large. Probably either the user
        * selected a huge font or the screen size has changed.
        *
        * This is also called with minimize.
        */
        reset_window(-1);
      }
      sys_cursor_update();
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
  return DefWindowProc(wnd, message, wp, lp);
}

static const char *help =
  "Usage: %s [OPTION]... [-e] [ - | COMMAND [ARG]... ]\n"
  "\n"
  "If no command is given, the user's default shell is invoked as a non-login\n"
  "shell. If the command is a single dash, the default shell is invoked as a\n"
  "login shell. Otherwise, the command is invoked with the given arguments.\n"
  "The command can be preceded by -e (for execute), but that is not required.\n"
  "\n"
  "Options:\n"
  "  -c, --config=FILE     Use specified config file (default: ~/.minttyrc)\n"
  "  -p, --pos=X,Y         Open window at specified position\n"
  "  -s, --size=COLS,ROWS  Set screen size in characters\n"
  "  -t, --title=TITLE     Set window title (default: the invoked command)\n"
  "  -v, --version         Print version information and exit\n"
  "  -h, --help            Display this help message and exit\n"
;

static const char short_opts[] = "+hvec:p:s:t:";

static const struct option
opts[] = { 
  {"help",    no_argument,       0, 'h'},
  {"version", no_argument,       0, 'v'},
  {"config",  required_argument, 0, 'c'},
  {"pos",     required_argument, 0, 'p'},
  {"size",    required_argument, 0, 's'},
  {"title",   required_argument, 0, 't'},
  {0, 0, 0, 0}
};

int
main(int argc, char *argv[])
{
  char *title = 0;
  int x = CW_USEDEFAULT, y = CW_USEDEFAULT;
  bool size_override = false;
  uint rows = 0, cols = 0;

  for (;;) {
    int opt = getopt_long(argc, argv, short_opts, opts, 0);
    if (opt == -1 || opt == 'e')
      break;
    switch (opt) {
      when 'c':
        config_filename = optarg;
      when 'p':
        if (sscanf(optarg, "%i,%i%1s", &x, &y, (char[2]){}) != 2) {
          fprintf(stderr, "%s: syntax error in position argument -- %s\n",
                          *argv, optarg);
          exit(1);
        }
      when 's':
        if (sscanf(optarg, "%u,%u%1s", &cols, &rows, (char[2]){}) != 2) {
          fprintf(stderr, "%s: syntax error in size argument -- %s\n",
                          *argv, optarg);
          exit(1);
        }
        size_override = true;
      when 't':
        title = optarg;
      when 'h':
        printf(help, *argv);
        return 0;
      when 'v':
        puts(APPNAME " " APPVER "\n" COPYRIGHT);
        return 0;
      otherwise:
        exit(1);
    }
  }
  
  main_argv = argv;
  
  if (!config_filename)
    asprintf(&config_filename, "%s/.minttyrc", getenv("HOME"));

  load_config();
  
  if (!size_override) {
    rows = cfg.rows;
    cols = cfg.cols;
  }

  inst = GetModuleHandle(NULL);

 /* Create window class. */
  {
    WNDCLASS wndclass;
    wndclass.style = 0;
    wndclass.lpfnWndProc = win_proc;
    wndclass.cbClsExtra = 0;
    wndclass.cbWndExtra = 0;
    wndclass.hInstance = inst;
    wndclass.hIcon = LoadIcon(inst, MAKEINTRESOURCE(IDI_MAINICON));
    wndclass.hCursor = LoadCursor(null, IDC_IBEAM);
    wndclass.hbrBackground = null;
    wndclass.lpszMenuName = null;
    wndclass.lpszClassName = APPNAME;
    RegisterClass(&wndclass);
  }

 /*
  * Guess some defaults for the window size. This all gets
  * updated later, so we don't really care too much. However, we
  * do want the font width/height guesses to correspond to a
  * large font rather than a small one...
  */
  {
    int guess_width = 25 + 20 * cols;
    int guess_height = 28 + 20 * rows;
    RECT r;
    get_fullscreen_rect(&r);
    if (guess_width > r.right - r.left)
      guess_width = r.right - r.left;
    if (guess_height > r.bottom - r.top)
      guess_height = r.bottom - r.top;
    uint style = WS_OVERLAPPEDWINDOW | (cfg.scrollbar ? WS_VSCROLL : 0);
    wnd = CreateWindow(APPNAME, APPNAME, style, x, y,
                        guess_width, guess_height, null, null, inst, null);
  }

  win_init_menus();
  
 /*
  * Initialise the terminal. (We have to do this _after_
  * creating the window, since the terminal is the first thing
  * which will call schedule_timer(), which will in turn call
  * timer_change_cb() which will expect wnd to exist.)
  */
  term_init();
  term_resize(rows, cols);
  ldisc_init();
  
 /*
  * Initialise the fonts, simultaneously correcting the guesses
  * for font_{width,height}.
  */
  win_init_fonts();
  win_init_palette();

 /*
  * Correct the guesses for extra_{width,height}.
  */
  {
    RECT cr, wr;
    GetWindowRect(wnd, &wr);
    GetClientRect(wnd, &cr);
    offset_width = offset_height = 0;
    extra_width = wr.right - wr.left - cr.right + cr.left;
    extra_height = wr.bottom - wr.top - cr.bottom + cr.top;
  }
  
 /*
  * Set up a caret bitmap, with no content.
  */
  {
    int size = (font_width + 15) / 16 * 2 * font_height;
    char *bits = newn(char, size);
    memset(bits, 0, size);
    caretbm = CreateBitmap(font_width, font_height, 1, 1, bits);
    free(bits);
    CreateCaret(wnd, caretbm, font_width, font_height);
  }

 /*
  * Initialise the scroll bar.
  */
  {
    SCROLLINFO si;
    si.cbSize = sizeof (si);
    si.fMask = SIF_ALL | SIF_DISABLENOSCROLL;
    si.nMin = 0;
    si.nMax = term_rows() - 1;
    si.nPage = term_rows();
    si.nPos = 0;
    SetScrollInfo(wnd, SB_VERT, &si, false);
  }

 /*
  * Resize the window, now we know what size we _really_ want it to be.
  */
  int term_width = font_width * term_cols();
  int term_height = extra_height + font_height * term_rows();
  SetWindowPos(wnd, null, 0, 0,
               term_width + extra_width, term_height + extra_height,
               SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);

  // Enable drag & drop.
  win_init_drop_target();

  // Finally show the window!
  update_transparency();
  ShowWindow(wnd, SW_SHOWDEFAULT);

  // Create child process.
  struct winsize ws = {term_rows(), term_cols(), term_width, term_height};
  char *cmd = child_create(argv + optind, &ws);
  
  // Set window title.
  SetWindowText(wnd, title ?: cmd);
  free(cmd);
  
  // Message loop.
  // Also monitoring child events.
  for (;;) {
    DWORD wakeup =
      MsgWaitForMultipleObjects(1, &child_event, false, INFINITE, QS_ALLINPUT);
    if (wakeup == WAIT_OBJECT_0) {
      if (child_proc()) {
        child_dead = true;
        term_set_focus(false);
      }
    }
    MSG msg;
    while (PeekMessage(&msg, null, 0, 0, PM_REMOVE)) {
      if (msg.message == WM_QUIT)
        return msg.wParam;      
      if (!IsDialogMessage(config_wnd, &msg))
        DispatchMessage(&msg);
      term_send_paste();
    }
  }
}


/*
  MSG msg;
  int gm;
  while ((gm = GetMessage(&msg, null, 0, 0)) > 0) {
    uint flags = GetWindowLongPtr(wnd, BOXFLAGS);
    if (!(flags & DF_END) && !IsDialogMessage(wnd, &msg))
      DispatchMessage(&msg);
    if (flags & DF_END)
      break;
  }
*/

