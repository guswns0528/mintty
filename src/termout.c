// termout.c (part of MinTTY)
// Copyright 2008 Andy Koppe
// Adapted from code from PuTTY-0.60 by Simon Tatham.
// Licensed under the terms of the GNU General Public License v3 or later.

#include "termpriv.h"

#include "linedisc.h"
#include "win.h"

/*
 * Terminal emulator.
 */

#define ANSI(x,y)	((x)+((y)<<8))
#define ANSI_QUE(x)	ANSI(x,true)

enum {
  CL_ANSIMIN   = 0x0001, /* Codes in all ANSI like terminals. */
  CL_VT100     = 0x0002, /* VT100 */
  CL_VT100AVO  = 0x0004, /* VT100 +AVO; 132x24 (not 132x14) & attrs */
  CL_VT102     = 0x0008, /* VT102 */
  CL_VT220     = 0x0010, /* VT220 */
  CL_VT320     = 0x0020, /* VT320 */
  CL_VT420     = 0x0040, /* VT420 */
  CL_VT510     = 0x0080, /* VT510, NB VT510 includes ANSI */
  CL_VT340TEXT = 0x0100, /* VT340 extensions that appear in the VT420 */
  CL_SCOANSI   = 0x1000, /* SCOANSI not in ANSIMIN. */
  CL_ANSI      = 0x2000, /* ANSI ECMA-48 not in the VT100..VT420 */
  CL_OTHER     = 0x4000, /* Others, Xterm, linux, putty, dunno, etc */
};

enum {
  TM_VT100    = CL_ANSIMIN | CL_VT100,
  TM_VT100AVO = TM_VT100 | CL_VT100AVO,
  TM_VT102    = TM_VT100AVO | CL_VT102,
  TM_VT220    = TM_VT102 | CL_VT220,
  TM_VTXXX    = TM_VT220 | CL_VT340TEXT | CL_VT510 | CL_VT420 | CL_VT320,
  TM_SCOANSI  = CL_ANSIMIN | CL_SCOANSI,
  TM_PUTTY    = 0xFFFF,
};

#define compatibility(x) \
    if ( ((CL_##x)&term.compatibility_level) == 0 ) { \
       term.state=TOPLEVEL; \
       break; \
    }
#define compatibility2(x,y) \
    if ( ((CL_##x|CL_##y)&term.compatibility_level) == 0 ) { \
       term.state=TOPLEVEL; \
       break; \
    }

#define has_compat(x) ( ((CL_##x)&term.compatibility_level) != 0 )


const char sco2ansicolour[] = { 0, 4, 2, 6, 1, 5, 3, 7 };

/*
 * Move the cursor to a given position, clipping at boundaries. We
 * may or may not want to clip at the scroll margin: marg_clip is 0
 * not to, 1 to disallow _passing_ the margins, and 2 to disallow
 * even _being_ outside the margins.
 */
static void
move(int x, int y, int marg_clip)
{
  if (x < 0)
    x = 0;
  if (x >= term.cols)
    x = term.cols - 1;
  if (marg_clip) {
    if ((term.curs.y >= term.marg_t || marg_clip == 2) && y < term.marg_t)
      y = term.marg_t;
    if ((term.curs.y <= term.marg_b || marg_clip == 2) && y > term.marg_b)
      y = term.marg_b;
  }
  if (y < 0)
    y = 0;
  if (y >= term.rows)
    y = term.rows - 1;
  term.curs.x = x;
  term.curs.y = y;
  term.wrapnext = false;
}

static void
set_erase_char(void)
{
  term.erase_char = term.basic_erase_char;
  if (term.use_bce)
    term.erase_char.attr = (term.curr_attr & (ATTR_FGMASK | ATTR_BGMASK));
}

/*
 * Save or restore the cursor and SGR mode.
 */
static void
save_cursor(int save)
{
  if (save) {
    term.savecurs = term.curs;
    term.save_attr = term.curr_attr;
    term.save_cset = term.cset;
    term.save_utf = term.utf;
    term.save_wnext = term.wrapnext;
    term.save_csattr = term.cset_attr[term.cset];
    term.save_sco_acs = term.sco_acs;
  }
  else {
    term.curs = term.savecurs;
   /* Make sure the window hasn't shrunk since the save */
    if (term.curs.x >= term.cols)
      term.curs.x = term.cols - 1;
    if (term.curs.y >= term.rows)
      term.curs.y = term.rows - 1;

    term.curr_attr = term.save_attr;
    term.cset = term.save_cset;
    term.utf = term.save_utf;
    term.wrapnext = term.save_wnext;
   /*
    * wrapnext might reset to False if the x position is no
    * longer at the rightmost edge.
    */
    if (term.wrapnext && term.curs.x < term.cols - 1)
      term.wrapnext = false;
    term.cset_attr[term.cset] = term.save_csattr;
    term.sco_acs = term.save_sco_acs;
    set_erase_char();
  }
}

/*
 * Call this whenever the terminal window state changes, to queue
 * an update.
 */
static void
seen_disp_event(void)
{
  term.seen_disp_event = true;  /* for scrollback-reset-on-activity */
  term_schedule_update();
}

/*
 * Insert or delete characters within the current line. n is +ve if
 * insertion is desired, and -ve for deletion.
 */
static void
insch(int n)
{
  int dir = (n < 0 ? -1 : +1);
  int m, j;
  pos cursplus;
  termline *ldata;

  n = (n < 0 ? -n : n);
  if (n > term.cols - term.curs.x)
    n = term.cols - term.curs.x;
  m = term.cols - term.curs.x - n;
  cursplus.y = term.curs.y;
  cursplus.x = term.curs.x + n;
  term_check_selection(term.curs, cursplus);
  term_check_boundary(term.curs.x, term.curs.y);
  if (dir < 0)
    term_check_boundary(term.curs.x + n, term.curs.y);
  ldata = scrlineptr(term.curs.y);
  if (dir < 0) {
    for (j = 0; j < m; j++)
      move_termchar(ldata, ldata->chars + term.curs.x + j,
                    ldata->chars + term.curs.x + j + n);
    while (n--)
      copy_termchar(ldata, term.curs.x + m++, &term.erase_char);
  }
  else {
    for (j = m; j--;)
      move_termchar(ldata, ldata->chars + term.curs.x + j + n,
                    ldata->chars + term.curs.x + j);
    while (n--)
      copy_termchar(ldata, term.curs.x + n, &term.erase_char);
  }
}

/*
 * Toggle terminal mode `mode' to state `state'. (`query' indicates
 * whether the mode is a DEC private one or a normal one.)
 */
static void
toggle_mode(int mode, int query, int state)
{
  if (query) {
    switch (mode) {
      when 1:  /* DECCKM: application cursor keys */
        term.app_cursor_keys = state;
      when 2:  /* DECANM: VT52 mode */
        // IGNORE
      when 3:  /* DECCOLM: 80/132 columns */
        term_deselect();
        win_resize(term.rows, state ? 132 : 80);
        term.reset_132 = state;
        term.alt_t = term.marg_t = 0;
        term.alt_b = term.marg_b = term.rows - 1;
        move(0, 0, 0);
        term_erase_lots(false, true, true);
      when 5:  /* DECSCNM: reverse video */
       /*
        * Toggle reverse video. If we receive an OFF within the
        * visual bell timeout period after an ON, we trigger an
        * effective visual bell, so that ESC[?5hESC[?5l will
        * always be an actually _visible_ visual bell.
        */
        if (term.rvideo && !state) {
         /* This is an OFF, so set up a vbell */
          term_schedule_vbell(true, term.rvbell_startpoint);
        }
        else if (!term.rvideo && state) {
         /* This is an ON, so we notice the time and save it. */
          term.rvbell_startpoint = get_tick_count();
        }
        term.rvideo = state;
        seen_disp_event();
      when 6:  /* DECOM: DEC origin mode */
        term.dec_om = state;
      when 7:  /* DECAWM: auto wrap */
        term.wrap = state;
      when 8:  /* DECARM: auto key repeat */
        // ignore
        //term.repeat_off = !state;
      when 9:  /* X10_MOUSE */
        term.mouse_tracking = state ? MT_X10 : 0;
      when 10: /* DECEDM: set local edit mode */
        term.editing = state;
        ldisc_send(null, 0, 0);
      when 25: /* DECTCEM: enable/disable cursor */
        compatibility2(OTHER, VT220);
        term.cursor_on = state;
        seen_disp_event();
      when 47: /* alternate screen */
        compatibility(OTHER);
        term_deselect();
        term_swap_screen(state, false, false);
        term.disptop = 0;
      when 1000: /* VT200_MOUSE */
        term.mouse_tracking = state ? MT_VT200 : 0;
      when 1002: /* BTN_EVENT_MOUSE */
        term.mouse_tracking = state ? MT_BTN_EVENT : 0;
      when 1003: /* ANY_EVENT_MOUSE */
        term.mouse_tracking = state ? MT_ANY_EVENT : 0;
      when 1047:       /* alternate screen */
        compatibility(OTHER);
        term_deselect();
        term_swap_screen(state, true, true);
        term.disptop = 0;
      when 1048:       /* save/restore cursor */
        save_cursor(state);
        if (!state)
          seen_disp_event();
      when 1049:       /* cursor & alternate screen */
        if (state)
          save_cursor(state);
        if (!state)
          seen_disp_event();
        compatibility(OTHER);
        term_deselect();
        term_swap_screen(state, true, false);
        if (!state)
          save_cursor(state);
        term.disptop = 0;
    }
  }
  else {
    switch (mode) {
      when 4:  /* IRM: set insert mode */
        compatibility(VT102);
        term.insert = state;
      when 12: /* SRM: set echo mode */
        term.echoing = !state;
        ldisc_send(null, 0, 0);
      when 20: /* LNM: Return sends ... */
        term.cr_lf_return = state;
      when 34: /* WYULCURM: Make cursor BIG */
        compatibility2(OTHER, VT220);
        term.big_cursor = !state;
    }
  }
}

/*
 * Process an OSC sequence: set window title or icon name.
 */
static void
do_osc(void)
{
  if (!term.osc_w) { // "wordness" is ignored
    term.osc_string[term.osc_strlen] = '\0';
    switch (term.esc_args[0]) {
      when 0 or 2 or 21:
        win_set_title(term.osc_string);
       // icon title is ignored
    }
  }
}

static void
out_bell(void)
{
  belltime *newbell;
  uint ticks = get_tick_count();

  if (!term.bell_overloaded) {
    newbell = new(belltime);
    newbell->ticks = ticks;
    newbell->next = null;
    if (!term.bellhead)
      term.bellhead = newbell;
    else
      term.belltail->next = newbell;
    term.belltail = newbell;
    term.nbells++;
  }

 /*
  * Throw out any bells that happened more than t seconds ago.
  */
  while (term.bellhead &&
         term.bellhead->ticks < ticks - BELLOVL_T) {
    belltime *tmp = term.bellhead;
    term.bellhead = tmp->next;
    free(tmp);
    if (!term.bellhead)
      term.belltail = null;
    term.nbells--;
  }

  if (term.bell_overloaded &&
      ticks - term.lastbell >= (uint) BELLOVL_S) {
   /*
    * If we're currently overloaded and the
    * last bell was more than s seconds ago,
    * leave overload mode.
    */
    term.bell_overloaded = false;
  }
  else if (!term.bell_overloaded &&
           term.nbells >= BELLOVL_N) {
   /*
    * Now, if we have n or more bells
    * remaining in the queue, go into overload
    * mode.
    */
    term.bell_overloaded = true;
  }
  term.lastbell = ticks;

 /*
  * Perform an actual bell if we're not overloaded.
  */
  if (!term.bell_overloaded) {
    win_bell(term.cfg.bell);

    if (term.cfg.bell == BELL_VISUAL) {
      term_schedule_vbell(false, 0);
    }
  }
  seen_disp_event();
}

static void
out_backspace(void)
{
  if (term.curs.x == 0 && (term.curs.y == 0 || term.wrap == 0))
   /* do nothing */ ;
  else if (term.curs.x == 0 && term.curs.y > 0)
    term.curs.x = term.cols - 1, term.curs.y--;
  else if (term.wrapnext)
    term.wrapnext = false;
  else
    term.curs.x--;
  seen_disp_event();
}

static void
out_tab(void)
{
  pos old_curs = term.curs;
  termline *ldata = scrlineptr(term.curs.y);
  do {
    term.curs.x++;
  } while (term.curs.x < term.cols - 1 && !term.tabs[term.curs.x]);
  
  if ((ldata->lattr & LATTR_MODE) != LATTR_NORM) {
    if (term.curs.x >= term.cols / 2)
      term.curs.x = term.cols / 2 - 1;
  }
  else {
    if (term.curs.x >= term.cols)
      term.curs.x = term.cols - 1;
  }
  
  term_check_selection(old_curs, term.curs);
  seen_disp_event();
}

static void
out_return(void)
{
  term.curs.x = 0;
  term.wrapnext = false;
  seen_disp_event();
  term.paste_hold = 0;
}

static void
out_linefeed(void)
{
  if (term.curs.y == term.marg_b)
    term_do_scroll(term.marg_t, term.marg_b, 1, true);
  else if (term.curs.y < term.rows - 1)
    term.curs.y++;
  term.wrapnext = false;
  seen_disp_event();
  term.paste_hold = 0;
}

static void
out_formfeed(void)
{
  move(0, 0, 0);
  term_erase_lots(false, false, true);
  term.disptop = 0;
  term.wrapnext = false;
  seen_disp_event();
}

static void
write_char(termline *cline, wchar c)
{
  clear_cc(cline, term.curs.x);
  cline->chars[term.curs.x].chr = c;
  cline->chars[term.curs.x].attr = term.curr_attr;
}  

/*
 * Output a character.
 * Return true if anything is actually written to the screen.
 */
static bool
out_char(wchar c)
{
  termline *cline = scrlineptr(term.curs.y);
  int width = 0;
  if (DIRECT_CHAR(c))
    width = 1;
  if (!width)
    width = wcwidth((wchar) c);
  if (term.wrapnext && term.wrap && width > 0) {
    cline->lattr |= LATTR_WRAPPED;
    if (term.curs.y == term.marg_b)
      term_do_scroll(term.marg_t, term.marg_b, 1, true);
    else if (term.curs.y < term.rows - 1)
      term.curs.y++;
    term.curs.x = 0;
    term.wrapnext = false;
    cline = scrlineptr(term.curs.y);
  }
  if (term.insert && width > 0)
    insch(width);
  if (term.selected) {
    pos cursplus = term.curs;
    incpos(cursplus);
    term_check_selection(term.curs, cursplus);
  }
  switch (width) {
    when 1:  // Normal character.
      term_check_boundary(term.curs.x, term.curs.y);
      term_check_boundary(term.curs.x + 1, term.curs.y);
      write_char(cline, c);
    when 2:  // Double-width character.
     /*
      * If we're about to display a double-width
      * character starting in the rightmost
      * column, then we do something special
      * instead. We must print a space in the
      * last column of the screen, then wrap;
      * and we also set LATTR_WRAPPED2 which
      * instructs subsequent cut-and-pasting not
      * only to splice this line to the one
      * after it, but to ignore the space in the
      * last character position as well.
      * (Because what was actually output to the
      * terminal was presumably just a sequence
      * of CJK characters, and we don't want a
      * space to be pasted in the middle of
      * those just because they had the
      * misfortune to start in the wrong parity
      * column. xterm concurs.)
      */
      term_check_boundary(term.curs.x, term.curs.y);
      term_check_boundary(term.curs.x + 2, term.curs.y);
      if (term.curs.x == term.cols - 1) {
        copy_termchar(cline, term.curs.x, &term.erase_char);
        cline->lattr |= LATTR_WRAPPED | LATTR_WRAPPED2;
        if (term.curs.y == term.marg_b)
          term_do_scroll(term.marg_t, term.marg_b, 1, true);
        else if (term.curs.y < term.rows - 1)
          term.curs.y++;
        term.curs.x = 0;
        cline = scrlineptr(term.curs.y);
       /* Now we must term_check_boundary again, of course. */
        term_check_boundary(term.curs.x, term.curs.y);
        term_check_boundary(term.curs.x + 2, term.curs.y);
      }
      write_char(cline, c);
      term.curs.x++;
      write_char(cline, UCSWIDE);
    when 0:  // Combining character.
      if (term.curs.x > 0) {
        int x = term.curs.x - 1;
       /* If we're in wrapnext state, the character
        * to combine with is _here_, not to our left. */
        if (term.wrapnext)
          x++;
       /*
        * If the previous character is
        * UCSWIDE, back up another one.
        */
        if (cline->chars[x].chr == UCSWIDE) {
          assert(x > 0);
          x--;
        }
        add_cc(cline, x, c);
        seen_disp_event();
      }
      return false;
    otherwise:  // Anything else. Probably shouldn't get here.
      return false;
  }
  term.curs.x++;
  if (term.curs.x == term.cols) {
    term.curs.x--;
    term.wrapnext = true;
  }
  seen_disp_event();
  return true;
}

static void
out_align_pattern(void)
{
  termline *ldata;
  int i, j;
  pos scrtop, scrbot;

  for (i = 0; i < term.rows; i++) {
    ldata = scrlineptr(i);
    for (j = 0; j < term.cols; j++) {
      copy_termchar(ldata, j, &term.basic_erase_char);
      ldata->chars[j].chr = 'E';
    }
    ldata->lattr = LATTR_NORM;
  }
  term.disptop = 0;
  seen_disp_event();
  scrtop.x = scrtop.y = 0;
  scrbot.x = 0;
  scrbot.y = term.rows;
  term_check_selection(scrtop, scrbot);
}

void
term_write(const char *data, int len)
{
  bufchain_add(term.inbuf, data, len);

  if (term.in_term_write)
    return;
    
  // Reset cursor blinking.
  seen_disp_event();
  term.cblinker = 1;
  term_schedule_cblink();

 /*
  * During drag-selects, we do not process terminal input,
  * because the user will want the screen to hold still to
  * be selected.
  */
  if (term_selecting())
    return;
    
 /*
  * Remove everything currently in `inbuf' and stick it up on the
  * in-memory display. There's a big state machine in here to
  * process escape sequences...
  */
  term.in_term_write = true;
  int unget = -1;
  char *chars = null; /* placate compiler warnings */
  int nchars = 0;
  while (nchars > 0 || unget != -1 || bufchain_size(term.inbuf) > 0) {
    uint c;
    if (unget == -1) {
      if (nchars == 0) {
        void *ret;
        bufchain_prefix(term.inbuf, &ret, &nchars);
        char localbuf[256];
        nchars = min(nchars, (int) sizeof localbuf);
        memcpy(localbuf, ret, nchars);
        bufchain_consume(term.inbuf, nchars);
        chars = localbuf;
        assert(chars != null);
      }
      c = *chars++;
      nchars--;
    }
    else {
      c = unget;
      unget = -1;
    }

   /* Note only VT220+ are 8-bit VT102 is seven bit, it shouldn't even
    * be able to display 8-bit characters, but I'll let that go 'cause
    * of i18n.
    */

   /*
    * If we're printing, add the character to the printer
    * buffer.
    */
    if (term.printing) {
      bufchain_add(term.printer_buf, &c, 1);

     /*
      * If we're in print-only mode, we use a much simpler
      * state machine designed only to recognise the ESC[4i
      * termination sequence.
      */
      if (term.only_printing) {
        if (c == '\033')
          term.print_state = 1;
        else if (c == (ubyte) '\233')
          term.print_state = 2;
        else if (c == '[' && term.print_state == 1)
          term.print_state = 2;
        else if (c == '4' && term.print_state == 2)
          term.print_state = 3;
        else if (c == 'i' && term.print_state == 3)
          term.print_state = 4;
        else
          term.print_state = 0;
        if (term.print_state == 4) {
          term_print_finish();
        }
        continue;
      }
    }

   /* First see about all those translations. */
    if (term.state == TOPLEVEL) {
      if (term_in_utf()) {
        switch (term.utf_state) {
          when 0: {
            if (c < 0x80) {
             /* UTF-8 must be stateless so we ignore iso2022. */
              if (ucsdata.unitab_ctrl[c] != 0xFF)
                c = ucsdata.unitab_ctrl[c];
              else
                c = ((ubyte) c) | CSET_ASCII;
              break;
            }
            else if ((c & 0xe0) == 0xc0) {
              term.utf_size = term.utf_state = 1;
              term.utf_char = (c & 0x1f);
            }
            else if ((c & 0xf0) == 0xe0) {
              term.utf_size = term.utf_state = 2;
              term.utf_char = (c & 0x0f);
            }
            else if ((c & 0xf8) == 0xf0) {
              term.utf_size = term.utf_state = 3;
              term.utf_char = (c & 0x07);
            }
            else if ((c & 0xfc) == 0xf8) {
              term.utf_size = term.utf_state = 4;
              term.utf_char = (c & 0x03);
            }
            else if ((c & 0xfe) == 0xfc) {
              term.utf_size = term.utf_state = 5;
              term.utf_char = (c & 0x01);
            }
            else {
              c = UCSERR;
              break;
            }
            continue;
          }
          when 1 or 2 or 3 or 4 or 5: {
            if ((c & 0xC0) != 0x80) {
              unget = c;
              c = UCSERR;
              term.utf_state = 0;
              break;
            }
            term.utf_char = (term.utf_char << 6) | (c & 0x3f);
            if (--term.utf_state)
              continue;

            c = term.utf_char;

           /* Is somebody trying to be evil! */
            if (c < 0x80 || (c < 0x800 && term.utf_size >= 2) ||
                (c < 0x10000 && term.utf_size >= 3) || (c < 0x200000 &&
                                                        term.utf_size >= 4) ||
                (c < 0x4000000 && term.utf_size >= 5))
              c = UCSERR;

           /* Unicode line separator and paragraph separator are CR-LF */
            if (c == 0x2028 || c == 0x2029)
              c = 0x85;

           /* High controls are probably a Baaad idea too. */
            if (c < 0xA0)
              c = 0xFFFD;

           /* The UTF-16 surrogates are not nice either. */
           /*       The standard give the option of decoding these: 
            *       I don't want to! */
            if (c >= 0xD800 && c < 0xE000)
              c = UCSERR;

           /* ISO 10646 characters now limited to UTF-16 range. */
            if (c > 0x10FFFF)
              c = UCSERR;

           /* This is currently a TagPhobic application.. */
            if (c >= 0xE0000 && c <= 0xE007F)
              continue;

           /* U+FEFF is best seen as a null. */
            if (c == 0xFEFF)
              continue;
           /* But U+FFFE is an error. */
            if (c == 0xFFFE || c == 0xFFFF)
              c = UCSERR;
          }
        }
      }
     /* Are we in the nasty ACS mode? Note: no sco in utf mode. */
      else if (term.sco_acs &&
               (c != '\033' && c != '\012' && c != '\015' && c != '\b')) {
        if (term.sco_acs == 2)
          c |= 0x80;
        c |= CSET_SCOACS;
      }
      else {
        int cset_attr = term.cset_attr[term.cset]; 
        switch (cset_attr) {
           /* 
            * Linedraw characters are different from 'ESC ( B'
            * only for a small range. For ones outside that
            * range, make sure we use the same font as well as
            * the same encoding.
            */
          when CSET_LINEDRW:
            if (ucsdata.unitab_ctrl[c] != 0xFF)
              c = ucsdata.unitab_ctrl[c];
            else
              c = ((ubyte) c) | CSET_LINEDRW;
          when  CSET_ASCII or CSET_GBCHR:
            /* If UK-ASCII, make the '#' a LineDraw Pound */
            if (c == '#' && cset_attr == CSET_GBCHR)
              c = '}' | CSET_LINEDRW;
            else if (ucsdata.unitab_ctrl[c] != 0xFF)
              c = ucsdata.unitab_ctrl[c];
            else
              c = ((ubyte) c) | CSET_ASCII;
          when CSET_SCOACS:
            if (c >= ' ')
              c = ((ubyte) c) | CSET_SCOACS;
        }
      }
    }

   /*
    * How about C1 controls? 
    * Explicitly ignore SCI (0x9a), which we don't translate to DECID.
    */
    if ((c & -32) == 0x80 && term.state < DO_CTRLS && has_compat(VT220)) {
      if (c == 0x9a)
        c = 0;
      else {
        term.state = SEEN_ESC;
        term.esc_query = false;
        c = '@' + (c & 0x1F);
      }
    }

   /* Or the GL control. */
    if (c == '\177' && term.state < DO_CTRLS && has_compat(OTHER)) {
      if (term.curs.x && !term.wrapnext)
        term.curs.x--;
      term.wrapnext = false;
      term_check_boundary(term.curs.x, term.curs.y);
      term_check_boundary(term.curs.x + 1, term.curs.y);
      copy_termchar(scrlineptr(term.curs.y), term.curs.x, &term.erase_char);
    }
    else if ((c & ~0x1F) == 0 && term.state < DO_CTRLS) {
     /* Or normal C0 controls. */
      switch (c) {
        when '\005':   /* ENQ: terminal type query */
         /* 
          * Strictly speaking this is VT100 but a VT100 defaults to
          * no response. Other terminals respond at their option.
          *
          * Don't put a CR in the default string as this tends to
          * upset some weird software.
          */
          compatibility(ANSIMIN);
          lpage_send(ansi_codepage, ANSWERBACK, lenof(ANSWERBACK), 0);

        when '\a':   /* BEL: Bell */
          out_bell();
        when '\b':     /* BS: Back space */
          out_backspace();
        when '\016':   /* LS1: Locking-shift one */
          compatibility(VT100);
          term.cset = 1;
        when '\017':   /* LS0: Locking-shift zero */
          compatibility(VT100);
          term.cset = 0;
        when '\e':   /* ESC: Escape */
          compatibility(ANSIMIN);
          term.state = SEEN_ESC;
          term.esc_query = false;
        when '\r':   /* CR: Carriage return */
          out_return();
        when '\f':   /* FF: Form feed */
          if (has_compat(SCOANSI))
            out_formfeed();
          else
            out_linefeed();
        when '\v':   /* VT: Line tabulation */
          compatibility(VT100);
          out_linefeed();
        when '\n':   /* LF: Line feed */
          out_linefeed();
        when '\t':     /* HT: Character tabulation */
          out_tab();
      }
    }
    else {
      switch (term.state) {
        when TOPLEVEL:
         /* Only graphic characters get this far;
          * ctrls are stripped above */
          if (!out_char(c))
            continue;
        when SEEN_ESC or OSC_MAYBE_ST: {
         /*
          * OSC_MAYBE_ST is virtually identical to SEEN_ESC, with the
          * exception that we have an OSC sequence in the pipeline,
          * and _if_ we see a backslash, we process it.
          */
          if (c == '\\' && term.state == OSC_MAYBE_ST) {
            do_osc();
            term.state = TOPLEVEL;
            break;
          }
          if (c >= ' ' && c <= '/') {
            if (term.esc_query)
              term.esc_query = -1;
            else
              term.esc_query = c;
            break;
          }
          term.state = TOPLEVEL;
          switch (ANSI(c, term.esc_query)) {
            when '[':  /* enter CSI mode */
              term.state = SEEN_CSI;
              term.esc_nargs = 1;
              term.esc_args[0] = ARG_DEFAULT;
              term.esc_query = false;
            when ']':  /* OSC: xterm escape sequences */
             /* Compatibility is nasty here, xterm, linux, decterm yuk! */
              compatibility(OTHER);
              term.state = SEEN_OSC;
              term.esc_args[0] = 0;
            when '7':  /* DECSC: save cursor */
              compatibility(VT100);
              save_cursor(true);
            when '8':  /* DECRC: restore cursor */
              compatibility(VT100);
              save_cursor(false);
              seen_disp_event();
            when '=':  /* DECKPAM: Keypad application mode */
              // compatibility(VT100);
              // AK: Ignore keypad application mode 
            when '>':  /* DECKPNM: Keypad numeric mode */
              // compatibility(VT100);
            when 'D':  /* IND: exactly equivalent to LF */
              compatibility(VT100);
              out_linefeed();
            when 'E':  /* NEL: exactly equivalent to CR-LF */
              compatibility(VT100);
              out_return();
              out_linefeed();
            when 'M':  /* RI: reverse index - backwards LF */
              compatibility(VT100);
              if (term.curs.y == term.marg_t)
                term_do_scroll(term.marg_t, term.marg_b, -1, true);
              else if (term.curs.y > 0)
                term.curs.y--;
              term.wrapnext = false;
              seen_disp_event();
            when 'Z':  /* DECID: terminal type query */
              compatibility(VT100);
              ldisc_send(term.id_string, strlen(term.id_string), 0);
            when 'c':  /* RIS: restore power-on settings */
              compatibility(VT100);
              term_reset();
              ldisc_send(null, 0, 0);
              if (term.reset_132) {
                win_resize(term.rows, 80);
                term.reset_132 = 0;
              }
              seen_disp_event();
            when 'H':  /* HTS: set a tab */
              compatibility(VT100);
              term.tabs[term.curs.x] = true;
            when ANSI('8', '#'):    /* DECALN: fills screen with Es :-) */
              compatibility(VT100);
              out_align_pattern();
            when ANSI('3', '#'):  /* DECDHL: 2*height, top */
              compatibility(VT100);
              scrlineptr(term.curs.y)->lattr = LATTR_TOP;
            when ANSI('4', '#'):  /* DECDHL: 2*height, bottom */
              compatibility(VT100);
              scrlineptr(term.curs.y)->lattr = LATTR_BOT;
            when ANSI('5', '#'):  /* DECSWL: normal */
              compatibility(VT100);
              scrlineptr(term.curs.y)->lattr = LATTR_NORM;
            when ANSI('6', '#'):  /* DECDWL: 2*width */
              compatibility(VT100);
              scrlineptr(term.curs.y)->lattr = LATTR_WIDE;
            when ANSI('A', '('):  /* GZD4: G0 designate 94-set */
              compatibility(VT100);
              term.cset_attr[0] = CSET_GBCHR;
            when ANSI('B', '('):
              compatibility(VT100);
              term.cset_attr[0] = CSET_ASCII;
            when ANSI('0', '('):
              compatibility(VT100);
              term.cset_attr[0] = CSET_LINEDRW;
            when ANSI('U', '('):
              compatibility(OTHER);
              term.cset_attr[0] = CSET_SCOACS;
            when ANSI('A', ')'):  /* G1D4: G1-designate 94-set */
              compatibility(VT100);
              term.cset_attr[1] = CSET_GBCHR;
            when ANSI('B', ')'):
              compatibility(VT100);
              term.cset_attr[1] = CSET_ASCII;
            when ANSI('0', ')'):
              compatibility(VT100);
              term.cset_attr[1] = CSET_LINEDRW;
            when ANSI('U', ')'):
              compatibility(OTHER);
              term.cset_attr[1] = CSET_SCOACS;
            when ANSI('8', '%') or ANSI('G', '%'):
              compatibility(OTHER);
              term.utf = 1;
            when ANSI('@', '%'):
              compatibility(OTHER);
              term.utf = 0;
          }
        }
        when SEEN_CSI: {
          term.state = TOPLEVEL;    /* default */
          if (isdigit(c)) {
            if (term.esc_nargs <= ARGS_MAX) {
              if (term.esc_args[term.esc_nargs - 1] == ARG_DEFAULT)
                term.esc_args[term.esc_nargs - 1] = 0;
              term.esc_args[term.esc_nargs - 1] =
                10 * term.esc_args[term.esc_nargs - 1] + c - '0';
            }
            term.state = SEEN_CSI;
          }
          else if (c == ';') {
            if (++term.esc_nargs <= ARGS_MAX)
              term.esc_args[term.esc_nargs - 1] = ARG_DEFAULT;
            term.state = SEEN_CSI;
          }
          else if (c < '@') {
            if (term.esc_query)
              term.esc_query = -1;
            else if (c == '?')
              term.esc_query = true;
            else
              term.esc_query = c;
            term.state = SEEN_CSI;
          }
          else {
            switch (ANSI(c, term.esc_query)) {
              when 'A':        /* CUU: move up N lines */
                move(term.curs.x, term.curs.y - def(term.esc_args[0], 1), 1);
                seen_disp_event();
              when 'e':        /* VPR: move down N lines */
                compatibility(ANSI);
                move(term.curs.x, term.curs.y + def(term.esc_args[0], 1), 1);
                seen_disp_event();
              when 'B':        /* CUD: Cursor down */
                move(term.curs.x, term.curs.y + def(term.esc_args[0], 1), 1);
                seen_disp_event();
              when ANSI('c', '>'):     /* DA: report xterm version */
                compatibility(OTHER);
               /* this reports xterm version 136 so that VIM can
                * use the drag messages from the mouse reporting */
                ldisc_send("\033[>0;136;0c", 11, 0);
              when 'a':        /* HPR: move right N cols */
                compatibility(ANSI);
                move(term.curs.x + def(term.esc_args[0], 1), term.curs.y, 1);
                seen_disp_event();
              when 'C':        /* CUF: Cursor right */
                move(term.curs.x + def(term.esc_args[0], 1), term.curs.y, 1);
                seen_disp_event();
              when 'D':        /* CUB: move left N cols */
                move(term.curs.x - def(term.esc_args[0], 1), term.curs.y, 1);
                seen_disp_event();
              when 'E':        /* CNL: move down N lines and CR */
                compatibility(ANSI);
                move(0, term.curs.y + def(term.esc_args[0], 1), 1);
                seen_disp_event();
              when 'F':        /* CPL: move up N lines and CR */
                compatibility(ANSI);
                move(0, term.curs.y - def(term.esc_args[0], 1), 1);
                seen_disp_event();
              when 'G' or '`':  /* CHA or HPA: set horizontal posn */
                compatibility(ANSI);
                move(def(term.esc_args[0], 1) - 1, term.curs.y, 0);
                seen_disp_event();
              when 'd':        /* VPA: set vertical posn */
                compatibility(ANSI);
                move(term.curs.x,
                     ((term.dec_om ? term.marg_t : 0) +
                      def(term.esc_args[0], 1) - 1), (term.dec_om ? 2 : 0));
                seen_disp_event();
              when 'H' or 'f':  /* CUP or HVP: set horz and vert posns at once */
                if (term.esc_nargs < 2)
                  term.esc_args[1] = ARG_DEFAULT;
                move(def(term.esc_args[1], 1) - 1,
                     ((term.dec_om ? term.marg_t : 0) +
                      def(term.esc_args[0], 1) - 1), (term.dec_om ? 2 : 0));
                seen_disp_event();
              when 'J': {      /* ED: erase screen or parts of it */
                uint i = def(term.esc_args[0], 0);
                if (i == 3) {
                 /* Erase Saved Lines (xterm) */
                  term_clear_scrollback();
                }
                else {
                  if (++i > 3)
                    i = 0;
                  term_erase_lots(false, !!(i & 2), !!(i & 1));
                }
                term.disptop = 0;
                seen_disp_event();
              }
              when 'K': {      /* EL: erase line or parts of it */
                uint i = def(term.esc_args[0], 0) + 1;
                if (i > 3)
                  i = 0;
                term_erase_lots(true, !!(i & 2), !!(i & 1));
                seen_disp_event();
              }
              when 'L':        /* IL: insert lines */
                compatibility(VT102);
                if (term.curs.y <= term.marg_b) {
                  term_do_scroll(term.curs.y, term.marg_b,
                                 -def(term.esc_args[0], 1), false);
                }
                seen_disp_event();
              when 'M':        /* DL: delete lines */
                compatibility(VT102);
                if (term.curs.y <= term.marg_b) {
                  term_do_scroll(term.curs.y, term.marg_b,
                                 def(term.esc_args[0], 1), true);
                }
                seen_disp_event();
              when '@':        /* ICH: insert chars */
               /* XXX VTTEST says this is vt220, vt510 manual says vt102 */
                compatibility(VT102);
                insch(def(term.esc_args[0], 1));
                seen_disp_event();
              when 'P':        /* DCH: delete chars */
                compatibility(VT102);
                insch(-def(term.esc_args[0], 1));
                seen_disp_event();
              when 'c':        /* DA: terminal type query */
                compatibility(VT100);
               /* This is the response for a VT102 */
                ldisc_send(term.id_string, strlen(term.id_string), 0);
              when 'n':        /* DSR: cursor position query */
                if (term.esc_args[0] == 6) {
                  char buf[32];
                  sprintf(buf, "\033[%d;%dR", term.curs.y + 1, term.curs.x + 1);
                  ldisc_send(buf, strlen(buf), 0);
                }
                else if (term.esc_args[0] == 5) {
                  ldisc_send("\033[0n", 4, 0);
                }
              when 'h' or ANSI_QUE('h'):  /* SM: toggle modes to high */
                compatibility(VT100);
                for (int i = 0; i < term.esc_nargs; i++)
                  toggle_mode(term.esc_args[i], term.esc_query, true);
              when 'i' or ANSI_QUE('i'):  /* MC: Media copy */
                compatibility(VT100);
                if (term.esc_nargs == 1) {
                  if (term.esc_args[0] == 5 && *term.cfg.printer) {
                    term.printing = true;
                    term.only_printing = !term.esc_query;
                    term.print_state = 0;
                    term_print_setup();
                  }
                  else if (term.esc_args[0] == 4 && term.printing)
                    term_print_finish();
                }
              when 'l' or ANSI_QUE('l'):  /* RM: toggle modes to low */
                compatibility(VT100);
                for (int i = 0; i < term.esc_nargs; i++)
                  toggle_mode(term.esc_args[i], term.esc_query, false);
              when 'g':        /* TBC: clear tabs */
                compatibility(VT100);
                if (term.esc_nargs == 1) {
                  if (term.esc_args[0] == 0) {
                    term.tabs[term.curs.x] = false;
                  }
                  else if (term.esc_args[0] == 3) {
                    int i;
                    for (i = 0; i < term.cols; i++)
                      term.tabs[i] = false;
                  }
                }
              when 'r':        /* DECSTBM: set scroll margins */
                compatibility(VT100);
                if (term.esc_nargs <= 2) {
                  int top = def(term.esc_args[0], 1) - 1;
                  int bot = (term.esc_nargs <= 1 ||
                         term.esc_args[1] ==
                         0 ? term.rows : def(term.esc_args[1], term.rows)) - 1;
                  if (bot >= term.rows)
                    bot = term.rows - 1;
                 /* VTTEST Bug 9 - if region is less than 2 lines
                  * don't change region.
                  */
                  if (bot - top > 0) {
                    term.marg_t = top;
                    term.marg_b = bot;
                    term.curs.x = 0;
                   /*
                    * I used to think the cursor should be
                    * placed at the top of the newly marginned
                    * area. Apparently not: VMS TPU falls over
                    * if so.
                    *
                    * Well actually it should for
                    * Origin mode - RDB
                    */
                    term.curs.y = (term.dec_om ? term.marg_t : 0);
                    seen_disp_event();
                  }
                }
              when 'm': {      /* SGR: set graphics rendition */
               /* 
                * A VT100 without the AVO only had one
                * attribute, either underline or
                * reverse video depending on the
                * cursor type, this was selected by
                * CSI 7m.
                *
                * when 2:
                *  This is sometimes DIM, eg on the
                *  GIGI and Linux
                * when 8:
                *  This is sometimes INVIS various ANSI.
                * when 21:
                *  This like 22 disables BOLD, DIM and INVIS
                *
                * The ANSI colours appear on any
                * terminal that has colour (obviously)
                * but the interaction between sgr0 and
                * the colours varies but is usually
                * related to the background colour
                * erase item. The interaction between
                * colour attributes and the mono ones
                * is also very implementation
                * dependent.
                *
                * The 39 and 49 attributes are likely
                * to be unimplemented.
                */
                for (int i = 0; i < term.esc_nargs; i++) {
                  switch (def(term.esc_args[i], 0)) {
                    when 0:  /* restore defaults */
                      term.curr_attr = term.default_attr;
                    when 1:  /* enable bold */
                      compatibility(VT100AVO);
                      term.curr_attr |= ATTR_BOLD;
                    when 4 or 21:  /* enable underline */
                      compatibility(VT100AVO);
                      term.curr_attr |= ATTR_UNDER;
                    when 5:  /* enable blink */
                      compatibility(VT100AVO);
                      term.curr_attr |= ATTR_BLINK;
                    when 6:  /* SCO light bkgrd */
                      compatibility(SCOANSI);
                      term.blink_is_real = false;
                      term.curr_attr |= ATTR_BLINK;
                      term_schedule_tblink();
                    when 7:  /* enable reverse video */
                      term.curr_attr |= ATTR_REVERSE;
                    when 10: /* SCO acs off */
                      compatibility(SCOANSI);
                      term.sco_acs = 0;
                    when 11: /* SCO acs on */
                      compatibility(SCOANSI);
                      term.sco_acs = 1;
                    when 12: /* SCO acs on, |0x80 */
                      compatibility(SCOANSI);
                      term.sco_acs = 2;
                    when 22: /* disable bold */
                      compatibility2(OTHER, VT220);
                      term.curr_attr &= ~ATTR_BOLD;
                    when 24: /* disable underline */
                      compatibility2(OTHER, VT220);
                      term.curr_attr &= ~ATTR_UNDER;
                    when 25: /* disable blink */
                      compatibility2(OTHER, VT220);
                      term.curr_attr &= ~ATTR_BLINK;
                    when 27: /* disable reverse video */
                      compatibility2(OTHER, VT220);
                      term.curr_attr &= ~ATTR_REVERSE;
                    when 30 ... 37:
                     /* foreground */
                      term.curr_attr &= ~ATTR_FGMASK;
                      term.curr_attr |=
                        (term.esc_args[i] - 30) << ATTR_FGSHIFT;
                    when 90 ... 97:
                     /* aixterm-style bright foreground */
                      term.curr_attr &= ~ATTR_FGMASK;
                      term.curr_attr |= ((term.esc_args[i] - 90 + 8)
                                         << ATTR_FGSHIFT);
                    when 39: /* default-foreground */
                      term.curr_attr &= ~ATTR_FGMASK;
                      term.curr_attr |= ATTR_DEFFG;
                    when 40 ... 47:
                     /* background */
                      term.curr_attr &= ~ATTR_BGMASK;
                      term.curr_attr |=
                        (term.esc_args[i] - 40) << ATTR_BGSHIFT;
                    when 100 ... 107:
                     /* aixterm-style bright background */
                      term.curr_attr &= ~ATTR_BGMASK;
                      term.curr_attr |= ((term.esc_args[i] - 100 + 8)
                                         << ATTR_BGSHIFT);
                    when 49: /* default-background */
                      term.curr_attr &= ~ATTR_BGMASK;
                      term.curr_attr |= ATTR_DEFBG;
                    when 38: /* xterm 256-colour mode */
                      if (i + 2 < term.esc_nargs && term.esc_args[i + 1] == 5) {
                        term.curr_attr &= ~ATTR_FGMASK;
                        term.curr_attr |= ((term.esc_args[i + 2] & 0xFF)
                                           << ATTR_FGSHIFT);
                        i += 2;
                      }
                    when 48: /* xterm 256-colour mode */
                      if (i + 2 < term.esc_nargs && term.esc_args[i + 1] == 5) {
                        term.curr_attr &= ~ATTR_BGMASK;
                        term.curr_attr |= ((term.esc_args[i + 2] & 0xFF)
                                           << ATTR_BGSHIFT);
                        i += 2;
                      }
                  }
                }
                set_erase_char();
              }
              when 's':        /* save cursor */
                save_cursor(true);
              when 'u':        /* restore cursor */
                save_cursor(false);
                seen_disp_event();
              when 't': {      /* DECSLPP: set page size - ie window height */
               /*
                * VT340/VT420 sequence DECSLPP, DEC only allows values
                *  24/25/36/48/72/144 other emulators (eg dtterm) use
                * illegal values (eg first arg 1..9) for window changing 
                * and reports.
                */
                if (term.esc_nargs <= 1 &&
                    (term.esc_args[0] < 1 || term.esc_args[0] >= 24)) {
                  compatibility(VT340TEXT);
                  win_resize(def(term.esc_args[0], 24), term.cols);
                  term_deselect();
                }
                else if (term.esc_nargs >= 1 && term.esc_args[0] >= 1 &&
                         term.esc_args[0] < 24) {
                  compatibility(OTHER);
                  int x, y, len;
                  char buf[80];
                  switch (term.esc_args[0]) {
                    when 1: win_set_iconic(false);
                    when 2: win_set_iconic(true);
                    when 3:
                      if (term.esc_nargs >= 3) {
                        win_move(def(term.esc_args[1], 0),
                                    def(term.esc_args[2], 0));
                      }
                    when 4:
                     /* We should resize the window to a given
                      * size in pixels here, but currently our
                      * resizing code isn't healthy enough to
                      * manage it. */
                    when 5:
                     /* move to top */
                      win_set_zorder(true);
                    when 6:
                     /* move to bottom */
                      win_set_zorder(false);
                    when 7:
                      win_refresh();
                    when 8:
                      if (term.esc_nargs >= 3) {
                        win_resize(def(term.esc_args[1], term.cfg.rows),
                                       def(term.esc_args[2], term.cfg.cols));
                      }
                    when 9:
                      if (term.esc_nargs >= 2)
                        win_set_zoom(term.esc_args[1] ? true : false);
                    when 11:
                      ldisc_send(win_is_iconic()
                                 ? "\033[1t" : "\033[2t", 4, 0);
                    when 13:
                      win_get_pos(&x, &y);
                      len = sprintf(buf, "\033[3;%d;%dt", x, y);
                      ldisc_send(buf, len, 0);
                    when 14:
                      win_get_pixels(&x, &y);
                      len = sprintf(buf, "\033[4;%d;%dt", x, y);
                      ldisc_send(buf, len, 0);
                    when 18:
                      len = sprintf(buf, "\033[8;%d;%dt", term.rows, term.cols);
                      ldisc_send(buf, len, 0);
                    when 19:
                     /*
                      * Hmmm. Strictly speaking we
                      * should return `the size of the
                      * screen in characters', but
                      * that's not easy: (a) window
                      * furniture being what it is it's
                      * hard to compute, and (b) in
                      * resize-font mode maximising the
                      * window wouldn't change the
                      * number of characters. *shrug*. I
                      * think we'll ignore it for the
                      * moment and see if anyone
                      * complains, and then ask them
                      * what they would like it to do.
                      */
                    when 20 or 21:
                      ldisc_send("\033]l\033\\", 5, 0);
                  }
                }
              }
              when 'S':        /* SU: Scroll up */
                compatibility(SCOANSI);
                term_do_scroll(term.marg_t, term.marg_b,
                               def(term.esc_args[0], 1), true);
                term.wrapnext = false;
                seen_disp_event();
              when 'T':        /* SD: Scroll down */
                compatibility(SCOANSI);
                term_do_scroll(term.marg_t, term.marg_b,
                               -def(term.esc_args[0], 1), true);
                term.wrapnext = false;
                seen_disp_event();
              when ANSI('|', '*'):     /* DECSNLS */
               /* 
                * Set number of lines on screen
                * VT420 uses VGA like hardware and can
                * support any size in reasonable range
                * (24..49 AIUI) with no default specified.
                */
                compatibility(VT420);
                if (term.esc_nargs == 1 && term.esc_args[0] > 0) {
                  win_resize(def(term.esc_args[0], term.cfg.rows),
                                 term.cols);
                  term_deselect();
                }
              when ANSI('|', '$'):     /* DECSCPP */
               /*
                * Set number of columns per page
                * Docs imply range is only 80 or 132, but
                * I'll allow any.
                */
                compatibility(VT340TEXT);
                if (term.esc_nargs <= 1) {
                  win_resize(term.rows,
                                 def(term.esc_args[0], term.cfg.cols));
                  term_deselect();
                }
              when 'X': {      /* ECH: write N spaces w/o moving cursor */
               /* XXX VTTEST says this is vt220, vt510 manual
                * says vt100 */
                compatibility(ANSIMIN);
                int n = def(term.esc_args[0], 1);
                pos cursplus;
                int p = term.curs.x;
                termline *cline = scrlineptr(term.curs.y);

                if (n > term.cols - term.curs.x)
                  n = term.cols - term.curs.x;
                cursplus = term.curs;
                cursplus.x += n;
                term_check_boundary(term.curs.x, term.curs.y);
                term_check_boundary(term.curs.x + n, term.curs.y);
                term_check_selection(term.curs, cursplus);
                while (n--)
                  copy_termchar(cline, p++, &term.erase_char);
                seen_disp_event();
              }
              when 'x': {      /* DECREQTPARM: report terminal characteristics */
                compatibility(VT100);
                int i = def(term.esc_args[0], 0);
                if (i == 0 || i == 1) {
                  char buf[20] = "\033[2;1;1;112;112;1;0x";
                  buf[2] += i;
                  ldisc_send(buf, 20, 0);
                }
              }
              when 'Z': {       /* CBT */
                compatibility(OTHER);
                pos old_curs = term.curs;
                int i = def(term.esc_args[0], 1); 
                while (--i >= 0 && term.curs.x > 0) {
                  do {
                    term.curs.x--;
                  } while (term.curs.x > 0 && !term.tabs[term.curs.x]);
                }
                term_check_selection(old_curs, term.curs);
              }
              when ANSI('c', '='):     /* Hide or Show Cursor */
                compatibility(SCOANSI);
                switch (term.esc_args[0]) {
                  when 0:      /* hide cursor */
                    term.cursor_on = false;
                  when 1:      /* restore cursor */
                    term.big_cursor = false;
                    term.cursor_on = true;
                  when 2:      /* block cursor */
                    term.big_cursor = true;
                    term.cursor_on = true;
                }
              when ANSI('C', '='):
               /*
                * set cursor start on scanline esc_args[0] and
                * end on scanline esc_args[1].If you set
                * the bottom scan line to a value less than
                * the top scan line, the cursor will disappear.
                */
                compatibility(SCOANSI);
                if (term.esc_nargs >= 2) {
                  if (term.esc_args[0] > term.esc_args[1])
                    term.cursor_on = false;
                  else
                    term.cursor_on = true;
                }
              when ANSI('D', '='):
                compatibility(SCOANSI);
                term.blink_is_real = false;
                term_schedule_tblink();
                if (term.esc_args[0] >= 1)
                  term.curr_attr |= ATTR_BLINK;
                else
                  term.curr_attr &= ~ATTR_BLINK;
              when ANSI('E', '='):
                compatibility(SCOANSI);
                term.blink_is_real = (term.esc_args[0] >= 1);
                term_schedule_tblink();
              when ANSI('F', '='):     /* set normal foreground */
                compatibility(SCOANSI);
                if (term.esc_args[0] >= 0 && term.esc_args[0] < 16) {
                  int colour =
                    (sco2ansicolour[term.esc_args[0] & 0x7] |
                     (term.esc_args[0] & 0x8)) << ATTR_FGSHIFT;
                  term.curr_attr &= ~ATTR_FGMASK;
                  term.curr_attr |= colour;
                  term.default_attr &= ~ATTR_FGMASK;
                  term.default_attr |= colour;
                  set_erase_char();
                }
              when ANSI('G', '='):     /* set normal background */
                compatibility(SCOANSI);
                if (term.esc_args[0] >= 0 && term.esc_args[0] < 16) {
                  int colour =
                    (sco2ansicolour[term.esc_args[0] & 0x7] |
                     (term.esc_args[0] & 0x8)) << ATTR_BGSHIFT;
                  term.curr_attr &= ~ATTR_BGMASK;
                  term.curr_attr |= colour;
                  term.default_attr &= ~ATTR_BGMASK;
                  term.default_attr |= colour;
                  set_erase_char();
                }
              when ANSI('L', '='):
                compatibility(SCOANSI);
                term.use_bce = (term.esc_args[0] <= 0);
                set_erase_char();
              when ANSI('p', '"'): {   /* DECSCL: set compat level */
               /*
                * Allow the host to make this emulator a
                * 'perfect' VT102. This first appeared in
                * the VT220, but we do need to get back to
                * PuTTY mode so I won't check it.
                *
                * The arg in 40..42,50 are a PuTTY extension.
                * The 2nd arg, 8bit vs 7bit is not checked.
                *
                * Setting VT102 mode should also change
                * the Fkeys to generate PF* codes as a
                * real VT102 has no Fkeys. The VT220 does
                * this, F11..F13 become ESC,BS,LF other
                * Fkeys send nothing.
                *
                * Note ESC c will NOT change this!
                */
                switch (term.esc_args[0]) {
                  when 61:
                    term.compatibility_level &= ~TM_VTXXX;
                    term.compatibility_level |= TM_VT102;
                  when 62:
                    term.compatibility_level &= ~TM_VTXXX;
                    term.compatibility_level |= TM_VT220;
                  when 40:
                    term.compatibility_level &= TM_VTXXX;
                  when 41:
                    term.compatibility_level = TM_PUTTY;
                  when 42:
                    term.compatibility_level = TM_SCOANSI;
                  when ARG_DEFAULT:
                    term.compatibility_level = TM_PUTTY;
                  when 50: // ignore
                  otherwise:
                    if (term.esc_args[0] > 60 && term.esc_args[0] < 70)
                      term.compatibility_level |= TM_VTXXX;
                }
               /* Change the response to CSI c */
                if (term.esc_args[0] == 50) {
                  char lbuf[64];
                  strcpy(term.id_string, "\033[?");
                  for (int i = 1; i < term.esc_nargs; i++) {
                    if (i != 1)
                      strcat(term.id_string, ";");
                    sprintf(lbuf, "%d", term.esc_args[i]);
                    strcat(term.id_string, lbuf);
                  }
                  strcat(term.id_string, "c");
                }
              }
            }
          }
        }
        when SEEN_OSC: {
          term.osc_w = false;
          switch (c) {
            when 'P':  /* Linux palette sequence */
              term.state = SEEN_OSC_P;
              term.osc_strlen = 0;
            when 'R':  /* Linux palette reset */
              win_reset_palette();
              term_invalidate();
              term.state = TOPLEVEL;
            when 'W':  /* word-set */
              term.state = SEEN_OSC_W;
              term.osc_w = true;
            when '0' ... '9':
              term.esc_args[0] = 10 * term.esc_args[0] + c - '0';
            otherwise: {
              if (c == 'L' && term.esc_args[0] == 2) {
                // Grotty hack to support xterm and DECterm title
                // sequences concurrently.
                term.esc_args[0] = 1;
              }
              else {
                term.state = OSC_STRING;
                term.osc_strlen = 0;
              }
            }
          }
        }
        when OSC_STRING: {
         /*
          * This OSC stuff is EVIL. It takes just one character to get into
          * sysline mode and it's not initially obvious how to get out.
          * So I've added CR and LF as string aborts.
          * This shouldn't effect compatibility as I believe embedded 
          * control characters are supposed to be interpreted (maybe?) 
          * and they don't display anything useful anyway.
          *
          * -- RDB
          */
          switch (c) {
           /*
            * These characters terminate the string; ST and BEL
            * terminate the sequence and trigger instant
            * processing of it, whereas ESC goes back to SEEN_ESC
            * mode unless it is followed by \, in which case it is
            * synonymous with ST in the first place.
            */
            when '\n' or '\r':
              term.state = TOPLEVEL;
            when '\a' or 0234:
              do_osc();
              term.state = TOPLEVEL;
            when '\e':
              term.state = OSC_MAYBE_ST;
            otherwise:
              if (term.osc_strlen < OSC_STR_MAX)
                term.osc_string[term.osc_strlen++] = c;
          }
        }
        when SEEN_OSC_P: {
          uint max = (term.osc_strlen == 0 ? 21 : 15);
          uint val;
          if (c >= '0' && c <= '9')
            val = c - '0';
          else if (c >= 'A' && c <= 'A' + max - 10)
            val = c - 'A' + 10;
          else if (c >= 'a' && c <= 'a' + max - 10)
            val = c - 'a' + 10;
          else {
            term.state = TOPLEVEL;
            break;
          }
          term.osc_string[term.osc_strlen++] = val;
          if (term.osc_strlen >= 7) {
            win_set_palette(term.osc_string[0],
                        term.osc_string[1] * 16 + term.osc_string[2],
                        term.osc_string[3] * 16 + term.osc_string[4],
                        term.osc_string[5] * 16 + term.osc_string[6]);
            term_invalidate();
            term.state = TOPLEVEL;
          }
        }
        when SEEN_OSC_W:
          if ('0' <= c && c <= '9')
            term.esc_args[0] = 10 * term.esc_args[0] + c - '0';
          else {
            term.state = OSC_STRING;
            term.osc_strlen = 0;
          }
        when DO_CTRLS:
          break;
      }
    }
    if (term.selected) {
      pos cursplus = term.curs;
      incpos(cursplus);
      term_check_selection(term.curs, cursplus);
    }
  }
  term.in_term_write = false;
  term_print_flush();
}
