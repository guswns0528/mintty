// termclip.c (part of mintty)
// Copyright 2008-10 Andy Koppe
// Adapted from code from PuTTY-0.60 by Simon Tatham and team.
// Licensed under the terms of the GNU General Public License v3 or later.

#include "termpriv.h"

#include "win.h"
#include "child.h"
#include "charset.h"

/*
 * Helper routine for term_copy(): growing buffer.
 */
typedef struct {
  int buflen;   /* amount of allocated space in textbuf/attrbuf */
  int bufpos;   /* amount of actual data */
  wchar *textbuf;       /* buffer for copied text */
  wchar *textptr;       /* = textbuf + bufpos (current insertion point) */
  uint *attrbuf; /* buffer for copied attributes */
  uint *attrptr; /* = attrbuf + bufpos */
} clip_workbuf;

static void
clip_addchar(clip_workbuf * b, wchar chr, int attr)
{
  if (b->bufpos >= b->buflen) {
    b->buflen += 128;
    b->textbuf = renewn(b->textbuf, b->buflen);
    b->textptr = b->textbuf + b->bufpos;
    b->attrbuf = renewn(b->attrbuf, b->buflen);
    b->attrptr = b->attrbuf + b->bufpos;
  }
  *b->textptr++ = chr;
  *b->attrptr++ = attr;
  b->bufpos++;
}

static void
get_selection(clip_workbuf *buf)
{
  pos start = term.sel_start, end = term.sel_end;
  
  int old_top_x;
  int attr;

  buf->buflen = 5120;
  buf->bufpos = 0;
  buf->textptr = buf->textbuf = newn(wchar, buf->buflen);
  buf->attrptr = buf->attrbuf = newn(uint, buf->buflen);

  old_top_x = start.x;    /* needed for rect==1 */

  while (poslt(start, end)) {
    bool nl = false;
    termline *line = fetch_line(start.y);
    pos nlpos;

   /*
    * nlpos will point at the maximum position on this line we
    * should copy up to. So we start it at the end of the
    * line...
    */
    nlpos.y = start.y;
    nlpos.x = term.cols;

   /*
    * ... move it backwards if there's unused space at the end
    * of the line (and also set `nl' if this is the case,
    * because in normal selection mode this means we need a
    * newline at the end)...
    */
    if (!(line->attr & LATTR_WRAPPED)) {
      while (nlpos.x && line->chars[nlpos.x - 1].chr == ' ' &&
             !line->chars[nlpos.x - 1].cc_next && poslt(start, nlpos))
        decpos(nlpos);
      if (poslt(nlpos, end))
        nl = true;
    }
    else if (line->attr & LATTR_WRAPPED2) {
     /* Ignore the last char on the line in a WRAPPED2 line. */
      decpos(nlpos);
    }

   /*
    * ... and then clip it to the terminal x coordinate if
    * we're doing rectangular selection. (In this case we
    * still did the above, so that copying e.g. the right-hand
    * column from a table doesn't fill with spaces on the
    * right.)
    */
    if (term.sel_rect) {
      if (nlpos.x > end.x)
        nlpos.x = end.x;
      nl = (start.y < end.y);
    }

    while (poslt(start, end) && poslt(start, nlpos)) {
      wchar cbuf[16], *p;
      int x = start.x;

      if (line->chars[x].chr == UCSWIDE) {
        start.x++;
        continue;
      }

      while (1) {
        wchar c = line->chars[x].chr;
        attr = line->chars[x].attr;
        cbuf[0] = c;
        cbuf[1] = 0;

        for (p = cbuf; *p; p++)
          clip_addchar(buf, *p, attr);

        if (line->chars[x].cc_next)
          x += line->chars[x].cc_next;
        else
          break;
      }
      start.x++;
    }
    if (nl) {
      clip_addchar(buf, '\r', 0);
      clip_addchar(buf, '\n', 0);
    }
    start.y++;
    start.x = term.sel_rect ? old_top_x : 0;

    release_line(line);
  }
  clip_addchar(buf, 0, 0);
}

void
term_copy(void)
{
  if (!term.selected)
    return;
  
  clip_workbuf buf;
  get_selection(&buf);
  
 /* Finally, transfer all that to the clipboard. */
  win_copy(buf.textbuf, buf.attrbuf, buf.bufpos);
  free(buf.textbuf);
  free(buf.attrbuf);
}

void
term_open(void)
{
  if (!term.selected)
    return;
  clip_workbuf buf;
  get_selection(&buf);
  free(buf.attrbuf);
  
  // Don't bother opening if it's all whitespace.
  wchar *p = buf.textbuf;
  while (iswspace(*p))
    p++;
  if (*p)
    win_open(buf.textbuf); // textbuf is freed by win_open
  else
    free(buf.textbuf);
}

void
term_paste(wchar *data, uint len)
{
  term_cancel_paste();

  term.paste_buffer = newn(wchar, len);
  term.paste_len = term.paste_pos = 0;

  // Copy data to the paste buffer, converting both Windows-style \r\n and
  // Unix-style \n line endings to \r, because that's what the Enter key sends.
  for (uint i = 0; i < len; i++) {
    wchar wc = data[i];
    if (wc != '\n')
      term.paste_buffer[term.paste_len++] = wc;
    else if (i == 0 || data[i - 1] != '\r')
      term.paste_buffer[term.paste_len++] = '\r';
  }
  
  if (term.bracketed_paste)
    child_write("\e[200~", 6);
  term_send_paste();
}

void
term_cancel_paste(void)
{
  if (term.paste_buffer) {
    free(term.paste_buffer);
    term.paste_buffer = 0;
    if (term.bracketed_paste)
      child_write("\e[201~", 6);
  }
}

void
term_send_paste(void)
{
  int i = term.paste_pos;
  while (i < term.paste_len && term.paste_buffer[i++] != '\r');
  child_sendw(term.paste_buffer + term.paste_pos, i - term.paste_pos);
  if (i < term.paste_len)
    term.paste_pos = i;
  else
    term_cancel_paste();
}

void
term_select_all(void)
{
  term.sel_start = (pos){-sblines(), 0};
  term.sel_end = (pos){term_last_nonempty_line(), term.cols};
  term.selected = true;
  if (cfg.copy_on_select)
    term_copy();
}
