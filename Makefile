APXS ?= apxs
PKG_CONFIG ?= pkg-config
CMARK_CFLAGS := $(shell $(PKG_CONFIG) --cflags libcmark-gfm 2>/dev/null)
CMARK_LIBS := $(shell $(PKG_CONFIG) --libs libcmark-gfm 2>/dev/null)
WARN_CFLAGS = -Wc,-Wall -Wc,-Wextra -Wc,-Wconversion -Wc,-Wshadow
EXTRA_CFLAGS ?=
EXTRA_LDFLAGS ?=

.PHONY: all install clean
all:
	$(APXS) -c -Wc,-std=c11 $(WARN_CFLAGS) $(EXTRA_CFLAGS) $(CMARK_CFLAGS) mod_markdown.c $(CMARK_LIBS) $(EXTRA_LDFLAGS)

install: all
	$(APXS) -i -a -n markdown .libs/mod_markdown.so

clean:
	rm -f *.o *.lo *.slo *.la
	rm -rf .libs
