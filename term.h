#ifndef TERM_H
#define TERM_H

#include "termline.h"
#include "tree234.h"
#include "bufchain.h"
#include "minibidi.h"

/*
 * UCSWIDE is a special value used in the terminal data to signify
 * the character cell containing the right-hand half of a CJK wide
 * character.
 */
#define UCSWIDE 0

/* Three attribute types: 
 * The ATTRs (normal attributes) are stored with the characters in
 * the main display arrays
 *
 * The TATTRs (temporary attributes) are generated on the fly, they
 * can overlap with characters but not with normal attributes.
 *
 * The LATTRs (line attributes) are an entirely disjoint space of
 * flags.
 * 
 * The DATTRs (display attributes) are internal to terminal.c (but
 * defined here because their values have to match the others
 * here); they reuse the TATTR_* space but are always masked off
 * before sending to the front end.
 *
 * ATTR_INVALID is an illegal colour combination.
 */

#define TATTR_ACTCURS 	    0x40000000UL        /* active cursor (block) */
#define TATTR_PASCURS 	    0x20000000UL        /* passive cursor (box) */
#define TATTR_RIGHTCURS	    0x10000000UL        /* cursor-on-RHS */
#define TATTR_COMBINING	    0x80000000UL        /* combining characters */

#define DATTR_STARTRUN      0x80000000UL        /* start of redraw run */

#define TDATTR_MASK         0xF0000000UL
#define TATTR_MASK (TDATTR_MASK)
#define DATTR_MASK (TDATTR_MASK)

#define LATTR_NORM   0x00000000UL
#define LATTR_WIDE   0x00000001UL
#define LATTR_TOP    0x00000002UL
#define LATTR_BOT    0x00000003UL
#define LATTR_MODE   0x00000003UL
#define LATTR_WRAPPED 0x00000010UL      /* this line wraps to next */
#define LATTR_WRAPPED2 0x00000020UL     /* with WRAPPED: CJK wide character
                                         * wrapped to next line, so last
                                         * single-width cell is empty */
#define ATTR_FGSHIFT   0
#define ATTR_BGSHIFT   9
#define ATTR_FGMASK    0x00001FFU
#define ATTR_BGMASK    0x003FE00U
#define ATTR_COLOURS   0x003FFFFU
#define ATTR_INVALID   0x003FFFFU
#define ATTR_BOLD      0x0040000U
#define ATTR_DIM       0x0080000U
#define ATTR_INVISIBLE 0x0100000U
#define ATTR_UNDER     0x0200000U
#define ATTR_REVERSE   0x0400000U
#define ATTR_BLINK     0x0800000U
#define ATTR_WIDE      0x1000000U
#define ATTR_NARROW    0x2000000U

/*
 * The definitive list of colour numbers stored in terminal
 * attribute words is kept here. It is:
 * 
 *  - 0-7 are ANSI colours (KRGYBMCW).
 *  - 8-15 are the bold versions of those colours.
 *  - 16-255 are the remains of the xterm 256-colour mode (a
 *    216-colour cube with R at most significant and B at least,
 *    followed by a uniform series of grey shades running between
 *    black and white but not including either on grounds of
 *    redundancy).
 *  - 256 is default foreground
 *  - 257 is default bold foreground
 *  - 258 is default background
 *  - 259 is default bold background
 *  - 260 is cursor foreground
 *  - 261 is cursor background
 */

#define ATTR_DEFFG   (256 << ATTR_FGSHIFT)
#define ATTR_DEFBG   (258 << ATTR_BGSHIFT)
#define ATTR_DEFAULT (ATTR_DEFFG | ATTR_DEFBG)

#define TTYPE termchar
#define TSIZE (sizeof(TTYPE))

typedef enum {
  CSET_ASCII = 'B',   /* Normal ASCII charset */
  CSET_GBCHR = 'A',   /* UK variant */
  CSET_LINEDRW = '0', /* Line drawing charset */
  CSET_OEM = 'U'      /* OEM Codepage 437 */
} cset;

typedef struct {
  int y, x;
} pos;

typedef enum {
  MBT_NONE = 0, MBT_LEFT = 1, MBT_MIDDLE = 2, MBT_RIGHT = 3
} mouse_button;

typedef enum {
  MDK_SHIFT = 1, MDK_ALT = 2, MDK_CTRL = 4
} mod_keys;

enum {
  ARGS_MAX = 32,     /* max # of esc sequence arguments */
  ARG_DEFAULT = 0   /* if an arg isn't specified */
};

enum { OSC_STR_MAX = 2048 };

typedef struct belltime {
  struct belltime *next;
  uint ticks;
} belltime;

struct term {
  tree234 *scrollback;  /* lines scrolled off top of screen */
  tree234 *screen;      /* lines on primary screen */
  tree234 *alt_screen;  /* lines on alternate screen */
  int disptop;  /* distance scrolled back (0 or -ve) */
  int tempsblines;      /* number of lines of .scrollback that
                         * can be retrieved onto the terminal
                         * ("temporary scrollback") */

  termline **disptext;  /* buffer of text on real screen */
  int dispcursx, dispcursy;     /* location of cursor on real screen */
  int curstype; /* type of cursor on real screen */

  int default_attr, curr_attr, save_attr;
  termchar basic_erase_char, erase_char;

  bufchain *inbuf;      /* terminal input buffer */
  pos  curs;     /* cursor */
  pos  savecurs; /* saved cursor position */
  int  marg_t, marg_b;   /* scroll margins */
  bool dec_om;   /* DEC origin mode flag */
  bool wrap, wrapnext;   /* wrap flags */
  bool insert;   /* insert-mode flag */
  bool cset_i;   /* 0 or 1: which char set */
  bool save_cset_i;
  cset save_cset;   /* saved with cursor position */
  bool save_utf, save_wnext;     /* saved with cursor position */
  bool rvideo;   /* global reverse video flag */
  bool cursor_on;        /* cursor enabled flag */
  bool deccolm_allowed;  /* DECCOLM sequence for 80/132 cols allowed? */
  bool reset_132;        /* Flag ESC c resets to 80 cols */
  bool use_bce;  /* Use Background coloured erase */
  bool cblinker; /* When blinking is the cursor on ? */
  bool tblinker; /* When the blinking text is on */
  bool blink_is_real;    /* Actually blink blinking text */
  bool echoing;  /* Does terminal want local echo? */
  bool editing;  /* Does terminal want local edit? */
  int  oem_acs, save_oem_acs;    /* CSI 10,11,12m -> OEM charset */
  bool utf;      /* Are we in toggleable UTF-8 mode? */
  bool printing, only_printing;  /* Are we doing ANSI printing? */
  int  print_state;      /* state of print-end-sequence scan */
  bufchain *printer_buf;        /* buffered data for printer */

 /* ESC 7 saved state for the alternate screen */
  pos  alt_savecurs;
  int  alt_save_attr;
  bool alt_save_cset_i;
  cset alt_save_cset;
  bool alt_save_utf, alt_save_wnext;
  int  alt_save_oem_acs;
  int  alt_x, alt_y;
  bool alt_om, alt_wrap, alt_wnext, alt_ins;
  bool alt_cset_i;
  int  alt_oem_acs;
  bool alt_utf;
  int  alt_t, alt_b;
  int  alt_sblines;
  bool which_screen;

  int  rows, cols;
  bool has_focus;
  bool in_vbell;
  bool seen_disp_event;

  bool shortcut_override;
  bool backspace_sends_bs;
  bool escape_sends_fs;
  bool app_escape_key;
  bool app_cursor_keys;
  bool app_keypad;
  bool app_wheel;
  int  modify_other_keys;
  bool newline_mode;
  bool report_focus;
  bool report_ambig_width;

  int  cursor_type;
  int  cursor_blinks;

  int  csets[2];

  int  esc_args[ARGS_MAX];
  int  esc_nargs;
  int  esc_query;

  int  osc_strlen;
  char osc_string[OSC_STR_MAX + 1];
  bool osc_w;

  uchar *tabs;

  enum {
    TOPLEVEL, SEEN_ESC, SEEN_CSI,
    SEEN_OSC, SEEN_OSC_W, SEEN_OSC_P, OSC_STRING, OSC_MAYBE_ST,
    SEEN_DCS, DCS_MAYBE_ST
  } state;

  enum {
    MM_NONE, MM_X10, MM_VT200, MM_BTN_EVENT, MM_ANY_EVENT
  } mouse_mode;

  enum {
    MS_OPENING = -2, MS_CLICKED = -1, MS_IDLE = 0,
    MS_SEL_CHAR = 1, MS_SEL_WORD = 2, MS_SEL_LINE = 3
  } mouse_state;
  
  bool sel_rect, selected;
  pos sel_start, sel_end, sel_anchor;
  
 /* Scroll steps during selection when cursor out of window. */
  int sel_scroll;
  pos sel_pos;

 /* Mask of attributes to pay attention to when painting. */
  int attr_mask;

  wchar *paste_buffer;
  int paste_len, paste_pos;

 /*
  * child_read calls term_write, but it can also be called from
  * the ldisc if the ldisc is called _within_ term_out. So we
  * have to guard against re-entrancy - if from_backend is
  * called recursively like this, it will simply add data to the
  * end of the buffer term_out is in the process of working
  * through.
  */
  bool in_term_write;
  
 /* True when we've seen part of a multibyte input char */
  bool in_mb_char;
  
 /* Non-zero when we've seen the first half of a surrogate pair */
  wchar high_surrogate;

 /*
  * These are buffers used by the bidi and Arabic shaping code.
  */
  termchar *ltemp;
  int ltemp_size;
  bidi_char *wcFrom, *wcTo;
  int wcFromTo_size;
  bidi_cache_entry *pre_bidi_cache, *post_bidi_cache;
  int bidi_cache_size;
};

extern struct term term;

void term_init(void);
void term_resize(int, int);
void term_scroll(int, int);
void term_reset(void);
void term_clear_scrollback(void);
void term_mouse_click(mouse_button, mod_keys, pos, int count);
void term_mouse_release(mouse_button, mod_keys, pos);
void term_mouse_move(mouse_button, mod_keys, pos);
void term_mouse_wheel(int delta, int lines_per_notch, mod_keys, pos);
void term_deselect(void);
void term_select_all(void);
void term_paint(void);
void term_update(void);
void term_invalidate(int left, int top, int right, int bottom);
void term_blink(int set_cursor);
void term_open(void);
void term_copy(void);
void term_paste(wchar *, uint len);
void term_send_paste(void);
void term_cancel_paste(void);
void term_reconfig(void);
void term_seen_key_event(void);
void term_write(const char *, int len);
void term_set_focus(bool has_focus);
int  term_cursor_type(void);
bool term_cursor_blinks(void);

#endif
