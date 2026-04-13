#-----------------------------------------------------------------------
#  Optimised Makefile for ekilo
#-----------------------------------------------------------------------

# Compiler & toolchain
CC       ?= gcc
CXX      ?= g++

# ---- Flags tuned for maximum performance -----------------------------
# NOTE: If building a .deb package for distribution to other machines,
# remove -march=native and -mtune=native to ensure portability.
CFLAGS   := -O3 -Ofast -march=native -mtune=native \
            -ffast-math -funroll-loops -ftree-vectorize \
            -pipe -fomit-frame-pointer \
            -fno-semantic-interposition \
            -flto -fno-guess-branch-probability

LDFLAGS  := -flto -s

WARNINGS := -Wall -Wextra -Wpedantic -std=c99
CFLAGS   += $(WARNINGS)

#-----------------------------------------------------------------------
# Installation Directories
#-----------------------------------------------------------------------
PREFIX  ?= /usr/local
DESTDIR ?=
BINDIR  ?= $(PREFIX)/bin
DATADIR ?= $(PREFIX)/share

#-----------------------------------------------------------------------
# Targets
#-----------------------------------------------------------------------
.PHONY: all clean install uninstall

all: ekilo

ekilo: ekilo.c
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $<

# Generate the desktop entry file for GNOME/KDE integration
ekilo.desktop:
	@echo "Generating ekilo.desktop..."
	@printf '[Desktop Entry]\n'                     > $@
	@printf 'Type=Application\n'                   >> $@
	@printf 'Name=Ekilo\n'                         >> $@
	@printf 'Comment=Fast terminal-based text editor\n' >> $@
	@printf 'Exec=ekilo %%F\n'                     >> $@
	@printf 'Icon=utilities-terminal\n'            >> $@
	@printf 'Terminal=true\n'                      >> $@
	@printf 'Categories=Utility;TextEditor;\n'     >> $@
	@printf 'MimeType=text/plain;\n'               >> $@
	@printf 'Keywords=text;editor;terminal;\n'     >> $@

install: ekilo ekilo.desktop
	@echo "Installing ekilo to $(DESTDIR)$(PREFIX)..."
	install -Dm 755 ekilo "$(DESTDIR)$(BINDIR)/ekilo"
	install -Dm 644 ekilo.desktop "$(DESTDIR)$(DATADIR)/applications/ekilo.desktop"
	@echo "Installation complete."
	@echo "Run 'update-desktop-database' if needed."

uninstall:
	@echo "Uninstalling ekilo..."
	rm -f "$(DESTDIR)$(BINDIR)/ekilo"
	rm -f "$(DESTDIR)$(DATADIR)/applications/ekilo.desktop"
	@echo "Uninstallation complete."

clean:
	rm -f ekilo ekilo.desktop

#-----------------------------------------------------------------------
# Optional: PGO stubs (unchanged)
#-----------------------------------------------------------------------
