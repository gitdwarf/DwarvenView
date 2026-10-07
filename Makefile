# Makefile wrapper around Meson for src2pkg, make, and other build tools.
# Standard usage: ./configure && make && make install DESTDIR=/tmp/pkg

PREFIX  ?= /usr
DESTDIR ?=
BUILDDIR = build

.PHONY: all install clean

# meson compile is incremental, so this is always safe to run. It is phony
# on purpose: make never needs to know where meson puts the binary.
all: $(BUILDDIR)/build.ninja
	meson compile -C $(BUILDDIR)

$(BUILDDIR)/build.ninja:
	meson setup $(BUILDDIR) \
	  --buildtype=release \
	  --prefix=$(PREFIX)

install: all
	meson install -C $(BUILDDIR) --destdir=$(DESTDIR)

clean:
	rm -rf $(BUILDDIR)
