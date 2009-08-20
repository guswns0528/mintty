// termmouse.c (part of MinTTY)
// Copyright 2008-09 Andy Koppe
// Based on code from PuTTY-0.60 by Simon Tatham and team.
// Licensed under the terms of the GNU General Public License v3 or later.

#include "termpriv.h"
#include "linedisc.h"
#include "win.h"

static bool
wordtype(int c)
{
  return iswalnum(c) || strchr("#-./\\_~", c);
}

static pos
sel_spread_word(pos p, int dir)
{
 /*
  * In this mode, the units are maximal runs of characters
  * whose `wordness' has the same value.
  */
  termline *ldata = lineptr(p.y);
  short wvalue = wordtype(UCSGET(ldata->chars, p.x));
  if (dir == 1) {
    for (;;) {
      int maxcols =
        (ldata->lattr & LATTR_WRAPPED2 ? term.cols - 1 : term.cols);
      if (p.x < maxcols - 1) {
        if (wordtype(UCSGET(ldata->chars, p.x + 1)) == wvalue)
          p.x++;
        else
          break;
      }
      else {
        if (ldata->lattr & LATTR_WRAPPED) {
          termline *ldata2;
          ldata2 = lineptr(p.y + 1);
          if (wordtype(UCSGET(ldata2->chars, 0))
              == wvalue) {
            p.x = 0;
            p.y++;
            unlineptr(ldata);
            ldata = ldata2;
          }
          else {
            unlineptr(ldata2);
            break;
          }
        }
        else
          break;
      }
    }
  }
  else {
    int topy = -sblines();
    for (;;) {
      if (p.x > 0) {
        if (wordtype(UCSGET(ldata->chars, p.x - 1)) == wvalue)
          p.x--;
        else
          break;
      }
      else {
        termline *ldata2;
        int maxcols;
        if (p.y <= topy)
          break;
        ldata2 = lineptr(p.y - 1);
        maxcols =
          (ldata2->lattr & LATTR_WRAPPED2 ? term.cols - 1 : term.cols);
        if (ldata2->lattr & LATTR_WRAPPED) {
          if (wordtype(UCSGET(ldata2->chars, maxcols - 1))
              == wvalue) {
            p.x = maxcols - 1;
            p.y--;
            unlineptr(ldata);
            ldata = ldata2;
          }
          else {
            unlineptr(ldata2);
            break;
          }
        }
        else
          break;
      }
    }
  }
  unlineptr(ldata);
  return p;
}

/*
 * Spread the selection outwards according to the selection mode.
 */
static pos
sel_spread_half(pos p, int dir)
{
  switch (term.mouse_state) {
    when MS_SEL_CHAR: {
     /*
      * In this mode, every character is a separate unit, except
      * for runs of spaces at the end of a non-wrapping line.
      */
      termline *ldata = lineptr(p.y);
      if (!(ldata->lattr & LATTR_WRAPPED)) {
        termchar *q = ldata->chars + term.cols;
        while (q > ldata->chars && q[-1].chr == ' ' && !q[-1].cc_next)
          q--;
        if (q == ldata->chars + term.cols)
          q--;
        if (p.x >= q - ldata->chars)
          p.x = (dir == -1 ? q - ldata->chars : term.cols - 1);
      }
      unlineptr(ldata);
    }
    when MS_SEL_WORD:
      p = sel_spread_word(p, dir); 
    when MS_SEL_LINE:
     /*
      * In this mode, every line is a unit.
      */
      p.x = (dir == -1 ? 0 : term.cols - 1);
    default:
     /* Shouldn't happen. */
      break;
  }
  return p;
}

static void
sel_spread(void)
{
  term.sel_start = sel_spread_half(term.sel_start, -1);
  decpos(term.sel_end);
  term.sel_end = sel_spread_half(term.sel_end, +1);
  incpos(term.sel_end);
}

static void
sel_drag(pos selpoint)
{
  term.selected = true;
  if (!term.sel_rect) {
   /*
    * For normal selection, we set (sel_start,sel_end) to
    * (selpoint,sel_anchor) in some order.
    */
    if (poslt(selpoint, term.sel_anchor)) {
      term.sel_start = selpoint;
      term.sel_end = term.sel_anchor;
    }
    else {
      term.sel_start = term.sel_anchor;
      term.sel_end = selpoint;
    }
    incpos(term.sel_end);
    sel_spread();
  }
  else {
   /*
    * For rectangular selection, we may need to
    * interchange x and y coordinates (if the user has
    * dragged in the -x and +y directions, or vice versa).
    */
    term.sel_start.x = min(term.sel_anchor.x, selpoint.x);
    term.sel_end.x = 1 + max(term.sel_anchor.x, selpoint.x);
    term.sel_start.y = min(term.sel_anchor.y, selpoint.y);
    term.sel_end.y = max(term.sel_anchor.y, selpoint.y);
  }
}

static void
sel_extend(pos selpoint)
{
  if (term.selected) {
    if (!term.sel_rect) {
     /*
      * For normal selection, we extend by moving
      * whichever end of the current selection is closer
      * to the mouse.
      */
      if (posdiff(selpoint, term.sel_start) <
          posdiff(term.sel_end, term.sel_start) / 2) {
        term.sel_anchor = term.sel_end;
        decpos(term.sel_anchor);
      }
      else
        term.sel_anchor = term.sel_start;
    }
    else {
     /*
      * For rectangular selection, we have a choice of
      * _four_ places to put sel_anchor and selpoint: the
      * four corners of the selection.
      */
      term.sel_anchor.x = 
        selpoint.x * 2 < term.sel_start.x + term.sel_end.x
        ? term.sel_end.x - 1
        : term.sel_start.x;
      term.sel_anchor.y = 
        selpoint.y * 2 < term.sel_start.y + term.sel_end.y
        ? term.sel_end.y
        : term.sel_start.y;
    }
  }
  else
    term.sel_anchor = selpoint;
  sel_drag(selpoint);
}

static void
send_mouse_event(char code, mod_keys mods, pos p)
{
  char buf[6] = "\e[M";
  buf[3] = code | (mods & ~cfg.click_target_mod) << 2;
  buf[4] = p.x + 33;
  buf[5] = p.y + 33;
  ldisc_send(buf, 6, 0);
}

static pos
box_pos(pos p)
{
  p.y = min(max(0, p.y), term.rows - 1);
  p.x =
    p.x < 0
    ? (p.y > 0 ? (--p.y, term.cols - 1) : 0)
    : min(p.x, term.cols - 1);
  return p;
}

static pos
get_selpoint(const pos p)
{
  pos sp = { .y = p.y + term.disptop, .x = p.x };
  termline *ldata = lineptr(sp.y);
  if ((ldata->lattr & LATTR_MODE) != LATTR_NORM)
    sp.x /= 2;

 /*
  * Transform x through the bidi algorithm to find the _logical_
  * click point from the physical one.
  */
  if (term_bidi_line(ldata, p.y) != null)
    sp.x = term.post_bidi_cache[p.y].backward[sp.x];
  
  unlineptr(ldata);
  return sp;
}

static void
send_keys(char *code, uint len, uint count, bool interactive)
{
  uint size = len * count;
  char buf[size], *p = buf;
  while (count--) { memcpy(p, code, len); p += len; }
  ldisc_send(buf, size, interactive);
}

static bool
is_app_mouse(mod_keys *mods_p)
{
  if (!term.mouse_mode)
    return false;
  bool override = *mods_p & cfg.click_target_mod;
  *mods_p &= ~cfg.click_target_mod;
  return cfg.clicks_target_app ^ override;
}

void
term_mouse_click(mouse_button b, mod_keys mods, pos p, int count)
{
  if (is_app_mouse(&mods)) {
    if (term.mouse_mode == MM_X10)
      mods = 0;
    send_mouse_event(0x1F + b, mods, box_pos(p));
    term.mouse_state = MS_CLICKED;
  }
  else {  
    bool alt = mods & MDK_ALT;
    bool shift_ctrl = mods & (MDK_SHIFT | MDK_CTRL);
    int rca = cfg.right_click_action;
    if (b == MBT_RIGHT && (rca == RC_SHOWMENU || shift_ctrl)) {
      if (!alt) 
        win_popup_menu();
    }
    else if (b == MBT_MIDDLE || (b == MBT_RIGHT && rca == RC_PASTE)) {
      if (!alt) {
        if (shift_ctrl)
          term_copy();
        else
          win_paste();
      }
    }
    else {
      // Only left clicks and extending right clicks should get here.
      p = get_selpoint(box_pos(p));
      term.mouse_state = count;
      term.sel_rect = alt;
      if (b == MBT_RIGHT || shift_ctrl)
        sel_extend(p);
      else if (count == 1) {
        term.selected = false;
        term.sel_anchor = p;
      }
      else {
        // Double or triple-click: select whole word or line
        term.selected = true;
        term.sel_rect = false;
        term.sel_start = term.sel_end = term.sel_anchor = p;
        incpos(term.sel_end);
        sel_spread();
      }
      win_capture_mouse();
      win_update();
    }
  }
}


void
term_mouse_release(mouse_button unused(b), mod_keys mods, pos p)
{
  p = box_pos(p);
  int state = term.mouse_state;
  term.mouse_state = MS_IDLE;
  if (state == MS_CLICKED) {
    if (term.mouse_mode >= MM_VT200)
      send_mouse_event(0x23, mods, p);
  }
  else if (state != MS_IDLE) {
    if (term.selected && cfg.copy_on_select)
      term_copy();
    
    // Flush any output held back during selection.
    term_write(0, 0);
    
    // "Clicks place cursor" implementation.
    if (!cfg.clicks_place_cursor || term.which_screen != 0 ||
        term.app_cursor_keys || term.editing)
      return;
    if (term.selected)
      p = term.sel_end;
    int y = p.y += term.disptop;
    if (y < 0)
      return;
    pos p0 = term.curs;
    int y0 = p0.y, dy = y - y0;
    if (dy < 0) {
      do {
        if (!(lineptr(y)->lattr & LATTR_WRAPPED))
          return;
      } while (++y < y0);
    }
    else {
      while (y-- > y0) {
        if (!(lineptr(y)->lattr & LATTR_WRAPPED))
          return;
      }
    }
    static pos last_p;
    if (state != MS_SEL_CHAR)
      p0 = last_p;
    last_p = p;
    int diff = (p.y - p0.y) * term.cols + (p.x - p0.x);
    if (diff)
      send_keys(diff < 0 ? "\e[D" : "\e[C", 3, abs(diff), false);
  }
}

static void
sel_scroll_cb(void)
{
  if (term_selecting() && term.sel_scroll != 0) {
    term_scroll(0, term.sel_scroll);
    sel_drag(get_selpoint(term.sel_pos));
    win_update();
    win_set_timer(sel_scroll_cb, 125);
  }
}

void
term_mouse_move(mouse_button b, mod_keys mods, pos p)
{
  static pos last_p;
  if (poseq(p, last_p))
    return;
  last_p = p;
  
  pos bp = box_pos(p);
  if (term_selecting()) {
    if (p.y < 0 || p.y >= term.rows) {
      if (term.sel_scroll == 0) 
        win_set_timer(sel_scroll_cb, 200);
      term.sel_scroll = p.y < 0 ? p.y : p.y - term.rows + 1;
      term.sel_pos = bp;
    }
    else    
      term.sel_scroll = 0;
    sel_drag(get_selpoint(bp));
    win_update();
  }
  else {
    if (term.mouse_state == MS_CLICKED) {
      if (term.mouse_mode >= MM_BTN_EVENT)
        send_mouse_event(0x3F + b, mods, bp);
    }
    else {
      if (term.mouse_mode == MM_ANY_EVENT)
        send_mouse_event(0x43, mods, bp);
    }
  }
}

void
term_mouse_wheel(int delta, int lines_per_notch, mod_keys mods, pos p)
{
  enum { NOTCH_DELTA = 120 };
  
  static int accu;
  accu += delta;
  
  if (!term.app_wheel && is_app_mouse(&mods)) {
    // Send as mouse events, with one event per notch.
    int notches = accu / NOTCH_DELTA;
    if (notches) {
      accu -= NOTCH_DELTA * notches;
      char code = 0x60 | (notches < 0);
      notches = abs(notches);
      do send_mouse_event(code, mods, p); while (--notches);
    }
  }
  else if (mods == MDK_CTRL) {
    int zoom = accu / NOTCH_DELTA;
    if (zoom) {
      accu -= NOTCH_DELTA * zoom;
      win_zoom_font(zoom);
    }
  }
  else if (!(mods & ~MDK_SHIFT)) {
    // Scroll, taking the lines_per_notch setting into account.
    // Scroll by a page per notch if setting is -1 or Shift is pressed.
    if (lines_per_notch == -1 || mods & MDK_SHIFT)
      lines_per_notch = term.rows;
    int lines = lines_per_notch * accu / NOTCH_DELTA;
    if (lines) {
      accu -= lines * NOTCH_DELTA / lines_per_notch;
      if (!term.app_wheel && (!term.which_screen || cfg.alt_screen_scroll))
        term_scroll(0, -lines);
      else {
        // Send scroll distance as CSI a/b events
        bool up = lines > 0;
        lines = abs(lines);
        int pages = lines / term.rows;
        lines -= pages * term.rows;
        if (term.app_wheel) {
          if (pages)
            send_keys(up ? "\e[1;2a" : "\e[1;2b", 6, pages, true);
          if (lines)
            send_keys(up ? "\eOa" : "\eOb", 3, lines, true);
        }
        else {
          if (pages)
            send_keys(up ? "\e[5~" : "\e[6~", 4, pages, true);
          if (lines) {
            char code[3] =
              {'\e',  term.app_cursor_keys ? 'O' : '[', up ? 'A' : 'B'};
            send_keys(code, 3, lines, true);
          }
        }
      }
    }
  }
}
