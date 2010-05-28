OBJS	= alix-leds

CC	= gcc
STRIP	= strip
OBJDUMP	= objdump
SSTRIP	= sstrip
DIET	= diet

CFLAGS	= -fomit-frame-pointer -Wall -Os -mpreferred-stack-boundary=2
LDFLAGS	= -s -Wl,--gc-sections #-Wl,--sort-section=alignment

VERSION := $(shell [ -d .git/. ] && ref=`(git describe --tags) 2>/dev/null` && ref=$${ref%-g*} && echo "$${ref\#v}")

CC_ORIG := $(CC)
override CC := $(DIET) $(CC)

all:	$(OBJS)

%:	%.c
	$(CC) $(LDFLAGS) $(CFLAGS) -o $@ $<
	$(STRIP) -x --strip-unneeded -R .comment -R .note $@
	$(OBJDUMP) -h $@ | grep -q '\.data[ ]*00000000' && $(STRIP) -R .data $@ || true
	$(OBJDUMP) -h $@ | grep -q '\.sbss[ ]*00000000' && $(STRIP) -R .sbss $@ || true
	-if [ -n "$(SSTRIP)" ]; then $(SSTRIP) $@ ; fi

%-debug:	%.c
	$(CC) $(LDFLAGS) $(CFLAGS) -DDEBUG -o $@ $<
	$(STRIP) -x --strip-unneeded -R .comment -R .note $@
	$(OBJDUMP) -h $@ | grep -q '\.data[ ]*00000000' && $(STRIP) -R .data $@ || true
	$(OBJDUMP) -h $@ | grep -q '\.sbss[ ]*00000000' && $(STRIP) -R .sbss $@ || true

clean:
	@rm -f *.[ao] *~ core
	@rm -f $(OBJS)

git-tar: clean
	git archive --format=tar --prefix=alix-leds-$(VERSION)/ HEAD | gzip -9 > alix-leds-$(VERSION).tar.gz

