#ifndef APPINFO_H
#define APPINFO_H

#define CONCAT_(a,b) a##b
#define CONCAT(a,b) CONCAT_(a,b)
#define STRINGIFY_(s) #s
#define STRINGIFY(s) STRINGIFY_(s)

#define APPNAME "mintty"
#define APPDESC "Terminal"
#define WEBSITE "http://mintty.googlecode.com"
#define AUTHOR  "Andy Koppe"
#define YEAR    "2013"

#define MAJOR_VERSION  1
#define MINOR_VERSION  2
#define PATCH_NUMBER   0
#define BUILD_NUMBER   1

#if defined SVN_DIR && defined SVN_REV
  #undef BUILD_NUMBER
  #define BUILD_NUMBER SVN_REV
  #define VERSION STRINGIFY(svn-SVN_DIR-CONCAT(r,SVN_REV))
#elif PATCH_NUMBER
  #define VERSION STRINGIGY(MAJOR_VERSION.MINOR_VERSION.PATCH_NUMBER)
#elif BUILD_NUMBER
  #define VERSION STRINGIFY(MAJOR_VERSION.MINOR_VERSION-CONCAT(beta,BUILD_NUMBER))
#else
  #define VERSION STRINGIFY(MAJOR_VERSION.MINOR_VERSION-alpha)
#endif

#define POINT_VERSION \
  STRINGIFY(MAJOR_VERSION.MINOR_VERSION.PATCH_NUMBER.BUILD_NUMBER)
#define COMMA_VERSION \
  MAJOR_VERSION,MINOR_VERSION,PATCH_NUMBER,BUILD_NUMBER
#define DECIMAL_VERSION \
  (MAJOR_VERSION * 10000 + MINOR_VERSION * 100 + PATCH_NUMBER)

#define COPYRIGHT "(C) " YEAR " " AUTHOR

#define VERSION_TEXT \
  APPNAME " " VERSION " (" STRINGIFY(TARGET) ")\n" \
  COPYRIGHT "\n" \
  "License GPLv3+: GNU GPL version 3 or later\n" \
  "There is no warranty, to the extent permitted by law.\n"

#define ABOUT_TEXT \
  "Thanks to Simon Tatham and the other contributors for their\n"\
  "great work on PuTTY, which mintty is largely based on. Thanks\n" \
  "also to Thomas Wolff and Chris Sutcliffe for lots of testing and\n" \
  "advice, and to KDE's Oxygen team for the program icon.\n" \
  "\n" \
  "Please report bugs or request enhancements through the\n" \
  "issue tracker on the mintty project page located at\n" \
  WEBSITE ".\n"

#endif
