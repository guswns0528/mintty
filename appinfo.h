#ifndef APPINFO_H
#define APPINFO_H

#define xstringify(s) #s
#define stringify(s) xstringify(s)

#define APPNAME "MinTTY"
#define APPVER stringify(VERSION)
#define APPDESC "Cygwin terminal"
#define WEBSITE "http://mintty.googlecode.com"
#define COPYRIGHT "Copyright (C) 2008-09 Andy Koppe"

#define LICENSE \
"Licensed under GPL version 3 or later.\n" \
"There is no warranty, to the extent permitted by law." \

#define APPINFO \
  LICENSE "\n" \
  "\n" \
  "Thanks to Simon Tatham and the other contributors for their\n"\
  "great work on PuTTY, which MinTTY is largely based on.\n" \
  "Thanks also to KDE's Oxygen team for the program icon.\n" \
  "\n" \
  "Please report bugs or request enhancements through the\n" \
  "issue tracker on the MinTTY project page located at\n" \
  WEBSITE "."

#endif
