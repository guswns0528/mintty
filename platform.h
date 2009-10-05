#ifndef PLATFORM_H
#define PLATFORM_H

#ifdef __CYGWIN__
 #include <cygwin/version.h>
 #if CYGWIN_VERSION_DLL_MAJOR >= 1007
  #define HAS_LOCALES 1
  #define HAS_UTF8_C_LOCALE 1
 #else
  #define HAS_LOCALES 0
  #define HAS_UTF8_C_LOCALE 0
  typedef uint32_t xchar;
  int xcwidth(xchar c);
 #endif
#else
 #error Platform not configured.
#endif

// Colours

typedef uint32 colour;

static inline colour
make_colour(uint8 r, uint8 g, uint8 b) { return r | g << 8 | b << 16; }

static inline uint8 red(colour c) { return c & 0xff; }
static inline uint8 green(colour c) { return c >> 8 & 0xff; }
static inline uint8 blue(colour c) { return c >> 16 & 0xff; }

int get_tick_count(void);
int cursor_blink_ticks(void);

#endif
