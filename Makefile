name := mintty
version := 0.3.5

exe := $(name).exe
dir := $(name)-$(version)
stuff := docs/readme.html scripts/create_shortcut.js
srcs := $(wildcard Makefile *.c *.h *.rc *.mft icon/*.ico icon/*.png COPYING LICENSE* INSTALL) $(stuff)

c_srcs := $(wildcard *.c)
rc_srcs := $(wildcard *.rc)
objs := $(c_srcs:.c=.o) $(rc_srcs:.rc=.o)
deps := $(objs:.o=.d)

c_opts := -include std.h -std=gnu99 -Wall -Wextra -Werror -DNDEBUG
code_opts := -march=i586 -mtune=pentium-m -fomit-frame-pointer -Os
ld_opts := --gc-sections -s
libs := -mwindows -lcomctl32 -lcomdlg32 -limm32 -lwinspool -lole32 -luuid

cc := gcc
rc_cpp := $(cc) -E -MMD -xc-header -DRC_INVOKED -DVERSION=$(version)
rc := windres --preprocessor "$(rc_cpp)"

$(exe): $(objs)
	$(cc) -o $@ $^ $(ld_opts) $(libs)
	du -b $@

bin: $(dir)-cygwin.zip
src: $(dir)-src.tgz

$(dir)-cygwin.zip: $(exe) $(stuff)
	rm -f $@
	zip -9 -j $@ $^
	du -b $@

$(dir)-src.tgz: $(srcs)
	rm -rf $(dir)
	mkdir $(dir)
	cp -ax --parents $^ $(dir)
	rm -f $@
	tar czf $@ $(dir)
	rm -rf $(dir)

%.o %.d: %.c Makefile
	$(cc) $< -c -MMD -MP $(c_opts) $(code_opts) -DVERSION=$(version)

%.o %.d: %.rc Makefile
	$(rc) $< $(<:.rc=.o)

clean:
	rm -f *.d *.o *.exe *.zip *.tgz *.stackdump

.PHONY: bin src clean

include $(deps)
