// wintext.c (part of MinTTY)
// Copyright 2008 Andy Koppe
// Adapted from code from PuTTY-0.60 by Simon Tatham.
// Licensed under the terms of the GNU General Public License v3 or later.

#include "winpriv.h"

#include "config.h"
#include "minibidi.h"
#include "unicode.h"

#include <winnls.h>

enum {
  FONT_NORMAL     = 0,
  FONT_BOLD       = 1,
  FONT_UNDERLINE  = 2,
  FONT_BOLDUND    = 3,
  FONT_WIDE       = 0x04,
  FONT_HIGH       = 0x08,
  FONT_NARROW     = 0x10,
  FONT_OEM        = 0x20,
  FONT_OEMBOLD    = 0x21,
  FONT_OEMUND     = 0x22,
  FONT_OEMBOLDUND = 0x23,
  FONT_MAXNO      = 0x2F,
  FONT_SHIFT      = 5
};

LOGFONT lfont;
static HFONT fonts[FONT_MAXNO];
static int fontflag[FONT_MAXNO];

enum bold_mode bold_mode;

static enum {
  UND_LINE, UND_FONT
} und_mode;
static int descent;

int font_width, font_height;
static int font_dualwidth;

COLORREF colours[NALLCOLOURS];

#define CLEARTYPE_QUALITY 5

static uint
get_font_quality(void) {
  switch (cfg.font_quality) {
    when FQ_ANTIALIASED: return ANTIALIASED_QUALITY;
    when FQ_NONANTIALIASED: return NONANTIALIASED_QUALITY;
    when FQ_CLEARTYPE: return CLEARTYPE_QUALITY;
    otherwise: return DEFAULT_QUALITY;
  }
}

static HFONT
create_font(int weight, bool underline)
{
  return 
    CreateFont(
      font_height, font_width, 0, 0, weight, false, underline, false,
      cfg.font.charset, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
      get_font_quality(), FIXED_PITCH | FF_DONTCARE,
      cfg.font.name
    );
}
 
/*
 * Initialise all the fonts we will need initially. There may be as many as
 * three or as few as one.  The other (poentially) twentyone fonts are done
 * if/when they are needed.
 *
 * We also:
 *
 * - check the font width and height, correcting our guesses if
 *   necessary.
 *
 * - verify that the bold font is the same width as the ordinary
 *   one, and engage shadow bolding if not.
 * 
 * - verify that the underlined font is the same width as the
 *   ordinary one (manual underlining by means of line drawing can
 *   be done in a pinch).
 */
void
win_init_fonts(void)
{
  TEXTMETRIC tm;
  CPINFO cpinfo;
  int fontsize[3];
  int i;
  int fw_dontcare, fw_bold;

  for (i = 0; i < FONT_MAXNO; i++)
    fonts[i] = null;

  bold_mode = cfg.bold_as_bright ? BOLD_COLOURS : BOLD_FONT;
  und_mode = UND_FONT;

  if (cfg.font.isbold) {
    fw_dontcare = FW_BOLD;
    fw_bold = FW_HEAVY;
  }
  else {
    fw_dontcare = FW_DONTCARE;
    fw_bold = FW_BOLD;
  }

  font_height = cfg.font.height;
  if (font_height > 0) {
    font_height = -MulDiv(font_height, GetDeviceCaps(hdc, LOGPIXELSY), 72);
  }
  font_width = 0;

  fonts[FONT_NORMAL] = create_font(fw_dontcare, false);

  GetObject(fonts[FONT_NORMAL], sizeof (LOGFONT), &lfont);

  SelectObject(hdc, fonts[FONT_NORMAL]);
  GetTextMetrics(hdc, &tm);

  font_height = tm.tmHeight;
  font_width = tm.tmAveCharWidth;
  font_dualwidth = (tm.tmAveCharWidth != tm.tmMaxCharWidth);

  {
    CHARSETINFO info;
    DWORD cset = tm.tmCharSet;
    memset(&info, 0xFF, sizeof (info));

   /* !!! Yes the next line is right */
    if (cset == OEM_CHARSET)
      ucsdata.font_codepage = GetOEMCP();
    else if (TranslateCharsetInfo((DWORD *) cset, &info, TCI_SRCCHARSET))
      ucsdata.font_codepage = info.ciACP;
    else
      ucsdata.font_codepage = -1;

    GetCPInfo(ucsdata.font_codepage, &cpinfo);
    ucsdata.dbcs_screenfont = (cpinfo.MaxCharSize > 1);
  }

  fonts[FONT_UNDERLINE] = create_font(fw_dontcare, true);

 /*
  * Some fonts, e.g. 9-pt Courier, draw their underlines
  * outside their character cell. We successfully prevent
  * screen corruption by clipping the text output, but then
  * we lose the underline completely. Here we try to work
  * out whether this is such a font, and if it is, we set a
  * flag that causes underlines to be drawn by hand.
  *
  * Having tried other more sophisticated approaches (such
  * as examining the TEXTMETRIC structure or requesting the
  * height of a string), I think we'll do this the brute
  * force way: we create a small bitmap, draw an underlined
  * space on it, and test to see whether any pixels are
  * foreground-coloured. (Since we expect the underline to
  * go all the way across the character cell, we only search
  * down a single column of the bitmap, half way across.)
  */
  {
    HDC und_dc;
    HBITMAP und_bm, und_oldbm;
    int i, gotit;
    COLORREF c;

    und_dc = CreateCompatibleDC(hdc);
    und_bm = CreateCompatibleBitmap(hdc, font_width, font_height);
    und_oldbm = SelectObject(und_dc, und_bm);
    SelectObject(und_dc, fonts[FONT_UNDERLINE]);
    SetTextAlign(und_dc, TA_TOP | TA_LEFT | TA_NOUPDATECP);
    SetTextColor(und_dc, RGB(255, 255, 255));
    SetBkColor(und_dc, RGB(0, 0, 0));
    SetBkMode(und_dc, OPAQUE);
    ExtTextOut(und_dc, 0, 0, ETO_OPAQUE, null, " ", 1, null);
    gotit = false;
    for (i = 0; i < font_height; i++) {
      c = GetPixel(und_dc, font_width / 2, i);
      if (c != RGB(0, 0, 0))
        gotit = true;
    }
    SelectObject(und_dc, und_oldbm);
    DeleteObject(und_bm);
    DeleteDC(und_dc);
    if (!gotit) {
      und_mode = UND_LINE;
      DeleteObject(fonts[FONT_UNDERLINE]);
      fonts[FONT_UNDERLINE] = 0;
    }
  }

  if (bold_mode == BOLD_FONT)
    fonts[FONT_BOLD] = create_font(fw_bold, false);

  descent = tm.tmAscent + 1;
  if (descent >= font_height)
    descent = font_height - 1;

  for (i = 0; i < 3; i++) {
    if (fonts[i]) {
      if (SelectObject(hdc, fonts[i]) && GetTextMetrics(hdc, &tm))
        fontsize[i] = tm.tmAveCharWidth + 256 * tm.tmHeight;
      else
        fontsize[i] = -i;
    }
    else
      fontsize[i] = -i;
  }

  if (fontsize[FONT_UNDERLINE] != fontsize[FONT_NORMAL]) {
    und_mode = UND_LINE;
    DeleteObject(fonts[FONT_UNDERLINE]);
    fonts[FONT_UNDERLINE] = 0;
  }

  if (bold_mode == BOLD_FONT && fontsize[FONT_BOLD] != fontsize[FONT_NORMAL]) {
    bold_mode = BOLD_SHADOW;
    DeleteObject(fonts[FONT_BOLD]);
    fonts[FONT_BOLD] = 0;
  }
  fontflag[0] = fontflag[1] = fontflag[2] = 1;

  init_ucs();
}

void
win_deinit_fonts(void)
{
  int i;
  for (i = 0; i < FONT_MAXNO; i++) {
    if (fonts[i])
      DeleteObject(fonts[i]);
    fonts[i] = 0;
    fontflag[i] = 0;
  }
}

/*
 * This is a wrapper to ExtTextOut() to force Windows to display
 * the precise glyphs we give it. Otherwise it would do its own
 * bidi and Arabic shaping, and we would end up uncertain which
 * characters it had put where.
 */
static void
exact_textout(int x, int y, CONST RECT * lprc, ushort * lpString,
              UINT cbCount, CONST INT * lpDx, int opaque)
{
  GCP_RESULTSW gcpr;
  char *buffer = newn(char, cbCount * 2 + 2);
  char *classbuffer = newn(char, cbCount);
  memset(&gcpr, 0, sizeof (gcpr));
  memset(buffer, 0, cbCount * 2 + 2);
  memset(classbuffer, GCPCLASS_NEUTRAL, cbCount);

  gcpr.lStructSize = sizeof (gcpr);
  gcpr.lpGlyphs = (void *) buffer;
  gcpr.lpClass = (void *) classbuffer;
  gcpr.nGlyphs = cbCount;

  GetCharacterPlacementW(hdc, lpString, cbCount, 0, &gcpr,
                         FLI_MASK | GCP_CLASSIN | GCP_DIACRITIC);

  ExtTextOut(hdc, x, y,
             ETO_GLYPH_INDEX | ETO_CLIPPED | (opaque ? ETO_OPAQUE : 0), lprc,
             buffer, cbCount, lpDx);
}

/*
 * The exact_textout() wrapper, unfortunately, destroys the useful
 * Windows `font linking' behaviour: automatic handling of Unicode
 * code points not supported in this font by falling back to a font
 * which does contain them. Therefore, we adopt a multi-layered
 * approach: for any potentially-bidi text, we use exact_textout(),
 * and for everything else we use a simple ExtTextOut as we did
 * before exact_textout() was introduced.
 */
static void
general_textout(int x, int y, CONST RECT * lprc, ushort * lpString,
                UINT cbCount, CONST INT * lpDx, int opaque)
{
  int i, j, xp, xn;
  RECT newrc;

  xp = xn = x;

  for (i = 0; i < (int) cbCount;) {
    int rtl = is_rtl(lpString[i]);

    xn += lpDx[i];

    for (j = i + 1; j < (int) cbCount; j++) {
      if (rtl != is_rtl(lpString[j]))
        break;
      xn += lpDx[j];
    }

   /*
    * Now [i,j) indicates a maximal substring of lpString
    * which should be displayed using the same textout
    * function.
    */
    if (rtl) {
      newrc.left = lprc->left + xp - x;
      newrc.right = lprc->left + xn - x;
      newrc.top = lprc->top;
      newrc.bottom = lprc->bottom;
      exact_textout(xp, y, &newrc, lpString + i, j - i, lpDx + i, opaque);
    }
    else {
      newrc.left = lprc->left + xp - x;
      newrc.right = lprc->left + xn - x;
      newrc.top = lprc->top;
      newrc.bottom = lprc->bottom;
      ExtTextOutW(hdc, xp, y, ETO_CLIPPED | (opaque ? ETO_OPAQUE : 0), &newrc,
                  lpString + i, j - i, lpDx + i);
    }

    i = j;
    xp = xn;
  }

  assert(xn - x == lprc->right - lprc->left);
}

static void
another_font(int fontno)
{
  int basefont;
  int fw_dontcare, fw_bold;
  int c, u, w, x;
  char *s;

  if (fontno < 0 || fontno >= FONT_MAXNO || fontflag[fontno])
    return;

  basefont = (fontno & ~(FONT_BOLDUND));
  if (basefont != fontno && !fontflag[basefont])
    another_font(basefont);

  if (cfg.font.isbold) {
    fw_dontcare = FW_BOLD;
    fw_bold = FW_HEAVY;
  }
  else {
    fw_dontcare = FW_DONTCARE;
    fw_bold = FW_BOLD;
  }

  c = cfg.font.charset;
  w = fw_dontcare;
  u = false;
  s = cfg.font.name;
  x = font_width;

  if (fontno & FONT_WIDE)
    x *= 2;
  if (fontno & FONT_NARROW)
    x = (x + 1) / 2;
  if (fontno & FONT_OEM)
    c = OEM_CHARSET;
  if (fontno & FONT_BOLD)
    w = fw_bold;
  if (fontno & FONT_UNDERLINE)
    u = true;

  fonts[fontno] =
    CreateFont(font_height * (1 + !!(fontno & FONT_HIGH)), x, 0, 0, w, false, u,
               false, c, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
               get_font_quality(), FIXED_PITCH | FF_DONTCARE, s);

  fontflag[fontno] = 1;
}

/*
 * Draw a line of text in the window, at given character
 * coordinates, in given attributes.
 *
 * We are allowed to fiddle with the contents of `text'.
 */
static void
win_text_internal(int x, int y, wchar * text, int len, uint attr, int lattr)
{
  COLORREF fg, bg, t;
  int nfg, nbg, nfont;
  RECT line_box;
  int force_manual_underline = 0;
  int fnt_width, char_width;
  int text_adjust = 0;
  static int *IpDx = 0, IpDxLEN = 0;

  lattr &= LATTR_MODE;

  char_width = fnt_width = font_width * (1 + (lattr != LATTR_NORM));

  if (attr & ATTR_WIDE)
    char_width *= 2;

  if (len > IpDxLEN || IpDx[0] != char_width) {
    int i;
    if (len > IpDxLEN) {
      free(IpDx);
      IpDx = newn(int, len + 16);
      IpDxLEN = (len + 16);
    }
    for (i = 0; i < IpDxLEN; i++)
      IpDx[i] = char_width;
  }

 /* Only want the left half of double width lines */
  if (lattr != LATTR_NORM && x * 2 >= term_cols())
    return;

  x *= fnt_width;
  y *= font_height;
  x += offset_width;
  y += offset_height;

  if ((attr & TATTR_ACTCURS) && (cfg.cursor_type == 0 || term_big_cursor())) {
    attr &= ~(ATTR_REVERSE | ATTR_BLINK | ATTR_COLOURS);
    if (bold_mode == BOLD_COLOURS)
      attr &= ~ATTR_BOLD;

   /* cursor fg and bg */
    attr |= (260 << ATTR_FGSHIFT) | (261 << ATTR_BGSHIFT);
  }

  nfont = 0;
  switch (lattr) {
    when LATTR_NORM: // do nothing
    when LATTR_WIDE: nfont |= FONT_WIDE;
    otherwise:       nfont |= FONT_WIDE + FONT_HIGH;
  }
  if (attr & ATTR_NARROW)
    nfont |= FONT_NARROW;

 /* Special hack for the VT100 linedraw glyphs. */
  if (text[0] >= 0x23BA && text[0] <= 0x23BD) {
    switch ((ubyte) (text[0])) {
      when 0xBA: text_adjust = -2 * font_height / 5;
      when 0xBB: text_adjust = -1 * font_height / 5;
      when 0xBC: text_adjust = font_height / 5;
      when 0xBD: text_adjust = 2 * font_height / 5;
    }
    if (lattr == LATTR_TOP || lattr == LATTR_BOT)
      text_adjust *= 2;
    text[0] = ucsdata.unitab_xterm['q'];
    if (attr & ATTR_UNDER) {
      attr &= ~ATTR_UNDER;
      force_manual_underline = 1;
    }
  }

 /* Anything left as an original character set is unprintable. */
  if (DIRECT_CHAR(text[0])) {
    int i;
    for (i = 0; i < len; i++)
      text[i] = 0xFFFD;
  }

 /* OEM CP */
  if ((text[0] & CSET_MASK) == CSET_OEMCP)
    nfont |= FONT_OEM;

  nfg = ((attr & ATTR_FGMASK) >> ATTR_FGSHIFT);
  nbg = ((attr & ATTR_BGMASK) >> ATTR_BGSHIFT);
  if (bold_mode == BOLD_FONT && (attr & ATTR_BOLD))
    nfont |= FONT_BOLD;
  if (und_mode == UND_FONT && (attr & ATTR_UNDER))
    nfont |= FONT_UNDERLINE;
  another_font(nfont);
  if (!fonts[nfont]) {
    if (nfont & FONT_UNDERLINE)
      force_manual_underline = 1;
   /* Don't do the same for manual bold, it could be bad news. */

    nfont &= ~(FONT_BOLD | FONT_UNDERLINE);
  }
  another_font(nfont);
  if (!fonts[nfont])
    nfont = FONT_NORMAL;
  if (attr & ATTR_REVERSE) {
    t = nfg;
    nfg = nbg;
    nbg = t;
  }
  if (bold_mode == BOLD_COLOURS) {
    if (attr & ATTR_BOLD) {
      if (nfg < 16)
        nfg |= 8;
      else if (nfg >= 256)
        nfg |= 1;
    }
    if (attr & ATTR_BLINK) {
      if (nbg < 16)
        nbg |= 8;
      else if (nbg >= 256)
        nbg |= 1;
    }
  }
  fg = colours[nfg];
  bg = colours[nbg];
  SelectObject(hdc, fonts[nfont]);
  SetTextColor(hdc, fg);
  SetBkColor(hdc, bg);
  if (attr & TATTR_COMBINING)
    SetBkMode(hdc, TRANSPARENT);
  else
    SetBkMode(hdc, OPAQUE);
  line_box.left = x;
  line_box.top = y;
  line_box.right = x + char_width * len;
  line_box.bottom = y + font_height;

 /* Only want the left half of double width lines */
  if (line_box.right > font_width * term_cols() + offset_width)
    line_box.right = font_width * term_cols() + offset_width;

 /* We're using a private area for direct to font. (512 chars.) */
  if (ucsdata.dbcs_screenfont && (text[0] & CSET_MASK) == CSET_ACP) {
   /* Ho Hum, dbcs fonts are a PITA! */
   /* To display on W9x I have to convert to UCS */
    static wchar *uni_buf = 0;
    static int uni_len = 0;
    int nlen, mptr;
    if (len > uni_len) {
      free(uni_buf);
      uni_len = len;
      uni_buf = newn(wchar, uni_len);
    }

    for (nlen = mptr = 0; mptr < len; mptr++) {
      uni_buf[nlen] = 0xFFFD;
      if (IsDBCSLeadByteEx(ucsdata.font_codepage, (BYTE) text[mptr])) {
        char dbcstext[2];
        dbcstext[0] = text[mptr] & 0xFF;
        dbcstext[1] = text[mptr + 1] & 0xFF;
        IpDx[nlen] += char_width;
        MultiByteToWideChar(ucsdata.font_codepage, MB_USEGLYPHCHARS, dbcstext,
                            2, uni_buf + nlen, 1);
        mptr++;
      }
      else {
        char dbcstext[1];
        dbcstext[0] = text[mptr] & 0xFF;
        MultiByteToWideChar(ucsdata.font_codepage, MB_USEGLYPHCHARS, dbcstext,
                            1, uni_buf + nlen, 1);
      }
      nlen++;
    }
    if (nlen <= 0)
      return;   /* Eeek! */

    ExtTextOutW(hdc, x, y - font_height * (lattr == LATTR_BOT) + text_adjust,
                ETO_CLIPPED | ETO_OPAQUE, &line_box, uni_buf, nlen, IpDx);
    if (bold_mode == BOLD_SHADOW && (attr & ATTR_BOLD)) {
      SetBkMode(hdc, TRANSPARENT);
      ExtTextOutW(hdc, x - 1,
                  y - font_height * (lattr == LATTR_BOT) + text_adjust,
                  ETO_CLIPPED, &line_box, uni_buf, nlen, IpDx);
    }

    IpDx[0] = -1;
  }
  else if (DIRECT_FONT(text[0])) {
    static char *directbuf = null;
    static int directlen = 0;
    int i;
    if (len > directlen) {
      directlen = len;
      directbuf = renewn(directbuf, directlen);
    }

    for (i = 0; i < len; i++)
      directbuf[i] = text[i] & 0xFF;

    ExtTextOut(hdc, x, y - font_height * (lattr == LATTR_BOT) + text_adjust,
               ETO_CLIPPED | ETO_OPAQUE, &line_box, directbuf, len, IpDx);
    if (bold_mode == BOLD_SHADOW && (attr & ATTR_BOLD)) {
      SetBkMode(hdc, TRANSPARENT);

     /* GRR: This draws the character outside it's box and can leave
      * 'droppings' even with the clip box! I suppose I could loop it
      * one character at a time ... yuk. 
      * 
      * Or ... I could do a test print with "W", and use +1 or -1 for this
      * shift depending on if the leftmost column is blank...
      */
      ExtTextOut(hdc, x - 1,
                 y - font_height * (lattr == LATTR_BOT) + text_adjust,
                 ETO_CLIPPED, &line_box, directbuf, len, IpDx);
    }
  }
  else {
   /* And 'normal' unicode characters */
    static WCHAR *wbuf = null;
    static int wlen = 0;
    int i;

    if (wlen < len) {
      free(wbuf);
      wlen = len;
      wbuf = newn(wchar, wlen);
    }

    for (i = 0; i < len; i++)
      wbuf[i] = text[i];

   /* print Glyphs as they are, without Windows' Shaping */
    general_textout(x,
                    y - font_height * (lattr == LATTR_BOT) + text_adjust,
                    &line_box, wbuf, len, IpDx, !(attr & TATTR_COMBINING));

   /* And the shadow bold hack. */
    if (bold_mode == BOLD_SHADOW && (attr & ATTR_BOLD)) {
      SetBkMode(hdc, TRANSPARENT);
      ExtTextOutW(hdc, x - 1,
                  y - font_height * (lattr == LATTR_BOT) + text_adjust,
                  ETO_CLIPPED, &line_box, wbuf, len, IpDx);
    }
  }
  if (lattr != LATTR_TOP &&
      (force_manual_underline ||
       (und_mode == UND_LINE && (attr & ATTR_UNDER)))) {
    HPEN oldpen;
    int dec = descent;
    if (lattr == LATTR_BOT)
      dec = dec * 2 - font_height;

    oldpen = SelectObject(hdc, CreatePen(PS_SOLID, 0, fg));
    MoveToEx(hdc, x, y + dec, null);
    LineTo(hdc, x + len * char_width, y + dec);
    oldpen = SelectObject(hdc, oldpen);
    DeleteObject(oldpen);
  }
}

/*
 * Wrapper that handles combining characters.
 */
void
win_text(int x, int y, wchar * text, int len, uint attr, int lattr)
{
  if (attr & TATTR_COMBINING) {
    uint a = 0;
    attr &= ~TATTR_COMBINING;
    while (len--) {
      win_text_internal(x, y, text, 1, attr | a, lattr);
      text++;
      a = TATTR_COMBINING;
    }
  }
  else
    win_text_internal(x, y, text, len, attr, lattr);
}

void
win_cursor(int x, int y, wchar * text, int len, uint attr, int lattr)
{
  int fnt_width;
  int char_width;
  int ctype = cfg.cursor_type;

  lattr &= LATTR_MODE;

  if ((attr & TATTR_ACTCURS) && (ctype == 0 || term_big_cursor())) {
    if (*text != UCSWIDE) {
      win_text(x, y, text, len, attr, lattr);
      return;
    }
    ctype = 2;
    attr |= TATTR_RIGHTCURS;
  }

  fnt_width = char_width = font_width * (1 + (lattr != LATTR_NORM));
  if (attr & ATTR_WIDE)
    char_width *= 2;
  x *= fnt_width;
  y *= font_height;
  x += offset_width;
  y += offset_height;

  if ((attr & TATTR_PASCURS) && (ctype == 0 || term_big_cursor())) {
    POINT pts[5];
    HPEN oldpen;
    pts[0].x = pts[1].x = pts[4].x = x;
    pts[2].x = pts[3].x = x + char_width - 1;
    pts[0].y = pts[3].y = pts[4].y = y;
    pts[1].y = pts[2].y = y + font_height - 1;
    oldpen = SelectObject(hdc, CreatePen(PS_SOLID, 0, colours[261]));
    Polyline(hdc, pts, 5);
    oldpen = SelectObject(hdc, oldpen);
    DeleteObject(oldpen);
  }
  else if ((attr & (TATTR_ACTCURS | TATTR_PASCURS)) && ctype != 0) {
    int startx, starty, dx, dy, length, i;
    if (ctype == 1) {
      startx = x;
      starty = y + descent;
      dx = 1;
      dy = 0;
      length = char_width;
    }
    else {
      int xadjust = 0;
      if (attr & TATTR_RIGHTCURS)
        xadjust = char_width - 1;
      startx = x + xadjust;
      starty = y;
      dx = 0;
      dy = 1;
      length = font_height;
    }
    if (attr & TATTR_ACTCURS) {
      HPEN oldpen;
      oldpen = SelectObject(hdc, CreatePen(PS_SOLID, 0, colours[261]));
      MoveToEx(hdc, startx, starty, null);
      LineTo(hdc, startx + dx * length, starty + dy * length);
      oldpen = SelectObject(hdc, oldpen);
      DeleteObject(oldpen);
    }
    else {
      for (i = 0; i < length; i++) {
        if (i % 2 == 0) {
          SetPixel(hdc, startx, starty, colours[261]);
        }
        startx += dx;
        starty += dy;
      }
    }
  }
}

/* This function gets the actual width of a character in the normal font.
 */
int
win_char_width(int uc)
{
  int ibuf = 0;

 /* If the font max is the same as the font ave width then this
  * function is a no-op.
  */
  if (!font_dualwidth)
    return 1;

  switch (uc & CSET_MASK) {
    when CSET_ASCII:   uc = ucsdata.unitab_line[uc & 0xFF];
    when CSET_LINEDRW: uc = ucsdata.unitab_xterm[uc & 0xFF];
    when CSET_SCOACS:  uc = ucsdata.unitab_scoacs[uc & 0xFF];
  }
  if (DIRECT_FONT(uc)) {
    if (ucsdata.dbcs_screenfont)
      return 1;

   /* Speedup, I know of no font where ascii is the wrong width */
    if ((uc & ~CSET_MASK) >= ' ' && (uc & ~CSET_MASK) <= '~')
      return 1;

    if ((uc & CSET_MASK) == CSET_ACP) {
      SelectObject(hdc, fonts[FONT_NORMAL]);
    }
    else if ((uc & CSET_MASK) == CSET_OEMCP) {
      another_font(FONT_OEM);
      if (!fonts[FONT_OEM])
        return 0;

      SelectObject(hdc, fonts[FONT_OEM]);
    }
    else
      return 0;

    if (GetCharWidth32(hdc, uc & ~CSET_MASK, uc & ~CSET_MASK, &ibuf) != 1 &&
        GetCharWidth(hdc, uc & ~CSET_MASK, uc & ~CSET_MASK, &ibuf) != 1)
      return 0;
  }
  else {
   /* Speedup, I know of no font where ascii is the wrong width */
    if (uc >= ' ' && uc <= '~')
      return 1;

    SelectObject(hdc, fonts[FONT_NORMAL]);
    if (GetCharWidth32W(hdc, uc, uc, &ibuf) == 1)
     /* Okay that one worked */ ;
    else if (GetCharWidthW(hdc, uc, uc, &ibuf) == 1)
     /* This should work on 9x too, but it's "less accurate" */ ;
    else
      return 0;
  }

  ibuf += font_width / 2 - 1;
  ibuf /= font_width;

  return ibuf;
}

void
win_set_sbar(int total, int start, int page)
{
  if (cfg.scrollbar) {
    SCROLLINFO si;
    si.cbSize = sizeof (si);
    si.fMask = SIF_ALL | SIF_DISABLENOSCROLL;	
    si.nMin = 0;
    si.nMax = total - 1;
    si.nPage = page;
    si.nPos = start;
    if (hwnd)
      SetScrollInfo(hwnd, SB_VERT, &si, true);
  }
}

static const COLORREF ansi_colours[16] = {
  0x000000, 0x0000BF, 0x00BF00, 0x00BFBF,
  0xBF0000, 0xBF00BF, 0xBFBF00, 0xBFBFBF,
  0x303040, 0x3030FF, 0x30FF30, 0x30FFFF,
  0xFF3030, 0xFF30FF, 0xFFFF30, 0xFFFFFF,
};


void
win_reconfig_palette(void)
{
  COLORREF rgb(colour c) { return RGB(c.red, c.green, c.blue); }
  colour brighter(colour c) {
    uint s = min(85, 255 - max(max(c.red, c.green), c.blue));
    return (colour){c.red + s, c.green + s, c.blue + s};
  }
  colours[256] = rgb(cfg.fg_colour);
  colours[257] = rgb(brighter(cfg.fg_colour));
  colours[258] = rgb(cfg.bg_colour);
  colours[259] = rgb(brighter(cfg.bg_colour));
  colours[260] = rgb(cfg.bg_colour);
  colours[261] = rgb(cfg.cursor_colour);
}

void
win_init_palette(void)
{
  // Initialise the colour cube and grayscale parts of the palette.
  // These aren't gonna change.
  int i = 16;
  for (uint r = 0; r < 6; r++)
    for (uint g = 0; g < 6; g++)
      for (uint b = 0; b < 6; b++)
        colours[i++] = RGB (r ? r * 40 + 55 : 0,
                            g ? g * 40 + 55 : 0,
                            b ? b * 40 + 55 : 0);
  for (uint s = 0; s < 24; s++) {
    uint c = s * 10 + 8;
    colours[i++] = RGB(c,c,c);
  }
  win_reset_palette();
  memcpy(colours, ansi_colours, sizeof ansi_colours);
  win_reconfig_palette();  
}

void
win_reset_palette(void)
{
  memcpy(colours, ansi_colours, sizeof ansi_colours);
  win_reconfig_palette();
 /* Default Background may have changed. Ensure any space between
  * text area and window border is redrawn. */
  InvalidateRect(hwnd, null, true);
}

void
win_set_palette(int n, int r, int g, int b)
{
  if (n >= 16)
    n += 256 - 16;
  if (n > NALLCOLOURS)
    return;
  colours[n] = RGB(r, g, b);
  if (n == (ATTR_DEFBG >> ATTR_BGSHIFT)) {
   /* If Default Background changes, we need to ensure any
    * space between the text area and the window border is
    * redrawn. */
    InvalidateRect(hwnd, null, true);
  }
}

