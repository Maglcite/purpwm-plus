# purpwm+ — build system
#
# A dwm-inspired dynamic tiling Wayland compositor written in C11 (gnu11 dialect), plus its
# companion polkit agent, screenshot portal and Secret Service (keyring) helper.
#
#   make                 build every binary into build/
#   make protocols       generate build/protocols/* (wayland-scanner) and build/embedded_font.c
#   make test            build and run the deterministic C unit tests (build/test_purpwm_plus)
#   make check-headers   prove include/*.h compile standalone, server- and client-side
#   make install         install binaries under $(DESTDIR)$(PREFIX)
#   make clean           remove build/
#
# Every object is compiled with -Wall -Wextra -Werror; pass WERROR= to relax that locally.
#
# Header layering (enforced by `make check-headers`):
#   config.h types.h wm.h ipc.h ui.h host_backend.h  never include <wayland-server*.h> or
#                                                    <wayland-client*.h>, so the nested host
#                                                    backend and the unit tests can use them.
#   purpwm.h render.h                                server-side: <wayland-server-core.h> plus
#                                                    the generated *-server-protocol.h headers.

VERSION         := 0.1.0

PREFIX          ?= /usr/local
BINDIR          ?= $(PREFIX)/bin
DATADIR         ?= $(PREFIX)/share
DESTDIR         ?=

CC              ?= cc
PKG_CONFIG      ?= pkg-config
PYTHON          ?= python3
WAYLAND_SCANNER ?= $(shell $(PKG_CONFIG) --variable=wayland_scanner wayland-scanner 2>/dev/null || echo wayland-scanner)
INSTALL         ?= install

BUILD           := build
OBJ             := $(BUILD)/obj
PROTO_XML_DIR   := protocols
PROTO_OUT       := $(BUILD)/protocols
FONT_FILE       := assets/fonts/DejaVuSansMono.ttf
FONT_C          := $(BUILD)/embedded_font.c

# ---------------------------------------------------------------------------------------------
# Flags
# ---------------------------------------------------------------------------------------------

OPTFLAGS        ?= -O2 -g
WERROR          ?= -Werror
WARNFLAGS       := -Wall -Wextra $(WERROR) -Wshadow -Wpointer-arith -Wstrict-prototypes \
                   -Wmissing-prototypes -Wformat=2 -Wno-unused-parameter
DEFS            := -D_GNU_SOURCE -DEGL_NO_X11 -DMESA_EGL_NO_X11_HEADERS \
                   -DPURPWM_VERSION=\"$(VERSION)\"
BASE_CFLAGS     := -std=gnu11 $(OPTFLAGS) $(WARNFLAGS) $(DEFS) -Iinclude -I$(PROTO_OUT) \
                   -MMD -MP
CFLAGS          ?=
LDFLAGS         ?=

COMP_PKGS       := wayland-server wayland-client wayland-egl egl glesv2 libdrm gbm libinput \
                   libseat libudev xkbcommon freetype2 fontconfig gdk-pixbuf-2.0 libpng xcursor \
                   gio-2.0 glib-2.0 pam
POLKIT_PKGS     := gtk4 polkit-agent-1 polkit-gobject-1 gio-2.0 glib-2.0
PORTAL_PKGS     := gtk4 gio-2.0 glib-2.0
KEYRING_PKGS    := libsecret-1 gio-2.0 glib-2.0
# Libraries the unit-test binary needs for the pure modules it links (no Wayland, no EGL/DRM).
TEST_PKGS       := freetype2 fontconfig gdk-pixbuf-2.0 libpng xcursor gio-2.0 glib-2.0 pam \
                   xkbcommon

COMP_CFLAGS     := $(shell $(PKG_CONFIG) --cflags $(COMP_PKGS))
COMP_LIBS       := $(shell $(PKG_CONFIG) --libs $(COMP_PKGS)) -lm -lpthread
POLKIT_CFLAGS   := $(shell $(PKG_CONFIG) --cflags $(POLKIT_PKGS)) \
                   -DPOLKIT_AGENT_I_KNOW_API_IS_SUBJECT_TO_CHANGE
POLKIT_LIBS     := $(shell $(PKG_CONFIG) --libs $(POLKIT_PKGS))
PORTAL_CFLAGS   := $(shell $(PKG_CONFIG) --cflags $(PORTAL_PKGS))
PORTAL_LIBS     := $(shell $(PKG_CONFIG) --libs $(PORTAL_PKGS))
KEYRING_CFLAGS  := $(shell $(PKG_CONFIG) --cflags $(KEYRING_PKGS))
KEYRING_LIBS    := $(shell $(PKG_CONFIG) --libs $(KEYRING_PKGS))
TEST_LIBS       := $(shell $(PKG_CONFIG) --libs $(TEST_PKGS)) -lm -lpthread

# ---------------------------------------------------------------------------------------------
# Wayland protocols
# ---------------------------------------------------------------------------------------------

# Every XML in protocols/ gets a server header and marshalling code; xdg-shell additionally
# gets a client header for the nested host backend (src/backend_wayland.c).
PROTOCOLS       := xdg-shell \
                   xdg-decoration-unstable-v1 \
                   xdg-activation-v1 \
                   xdg-output-unstable-v1 \
                   wlr-layer-shell-unstable-v1 \
                   wlr-data-control-unstable-v1 \
                   primary-selection-unstable-v1 \
                   idle-inhibit-unstable-v1 \
                   linux-dmabuf-v1 \
                   viewporter \
                   fractional-scale-v1 \
                   single-pixel-buffer-v1 \
                   cursor-shape-v1 \
                   tablet-v2
CLIENT_PROTOCOLS := xdg-shell

PROTO_SERVER_H  := $(PROTOCOLS:%=$(PROTO_OUT)/%-server-protocol.h)
PROTO_CLIENT_H  := $(CLIENT_PROTOCOLS:%=$(PROTO_OUT)/%-client-protocol.h)
PROTO_C         := $(PROTOCOLS:%=$(PROTO_OUT)/%-protocol.c)
PROTO_OBJS      := $(PROTOCOLS:%=$(OBJ)/protocols/%-protocol.o)
GENERATED       := $(PROTO_SERVER_H) $(PROTO_CLIENT_H) $(PROTO_C) $(FONT_C)

# ---------------------------------------------------------------------------------------------
# Sources
# ---------------------------------------------------------------------------------------------

# Slice 1: Wayland core & protocols
CORE_SRCS       := src/server.c src/surface.c src/xdg_shell.c src/layer_shell.c src/seat_data.c
# Slice 2: backends & GLES2 renderer
BACKEND_SRCS    := src/main.c src/render.c src/backend_drm.c src/backend_wayland.c
# Slice 3: WM tiling, input, glide, IPC, capture, cursor & wallpaper
WM_SRCS         := src/wm.c src/input.c src/glide.c src/ipc.c src/capture.c src/cursor.c \
                   src/wallpaper.c
# Slice 4: UI surfaces, font, PAM lock & notifications
UI_SRCS         := src/font.c src/bar.c src/lock.c src/notify.c src/help.c

COMP_SRCS       := $(CORE_SRCS) $(BACKEND_SRCS) $(WM_SRCS) $(UI_SRCS)
COMP_OBJS       := $(COMP_SRCS:%.c=$(OBJ)/%.o) $(PROTO_OBJS) $(OBJ)/embedded_font.o

# Pure modules linked into the unit tests: they include only config.h, types.h, wm.h, ipc.h
# and ui.h and never reference a symbol defined by the server, render or backend modules.
TEST_MODULES    := src/wm.c src/glide.c src/ipc.c src/capture.c src/cursor.c src/wallpaper.c \
                   src/font.c src/bar.c src/lock.c src/notify.c src/help.c
TEST_SRCS       := tests/test_purpwm_plus.c
TEST_OBJS       := $(TEST_SRCS:%.c=$(OBJ)/%.o) $(TEST_MODULES:%.c=$(OBJ)/%.o) \
                   $(OBJ)/embedded_font.o

COMPOSITOR      := $(BUILD)/purpwm+
POLKIT_AGENT    := $(BUILD)/purpwm+-polkit-agent
PORTAL          := $(BUILD)/purpwm+-portal
KEYRING         := $(BUILD)/purpwm+-keyring
TEST_BIN        := $(BUILD)/test_purpwm_plus
BINARIES        := $(COMPOSITOR) $(POLKIT_AGENT) $(PORTAL) $(KEYRING)

# ---------------------------------------------------------------------------------------------
# Top-level targets
# ---------------------------------------------------------------------------------------------

.PHONY: all protocols compositor tools test check check-headers install uninstall clean
.DEFAULT_GOAL := all

all: $(BINARIES) $(TEST_BIN)

protocols: $(GENERATED)

compositor: $(COMPOSITOR)

tools: $(POLKIT_AGENT) $(PORTAL) $(KEYRING)

test: $(TEST_BIN)
	./$(TEST_BIN)

check: check-headers test

# ---------------------------------------------------------------------------------------------
# Generated sources
# ---------------------------------------------------------------------------------------------

$(PROTO_OUT)/%-server-protocol.h: $(PROTO_XML_DIR)/%.xml
	@mkdir -p $(@D)
	$(WAYLAND_SCANNER) server-header $< $@

$(PROTO_OUT)/%-client-protocol.h: $(PROTO_XML_DIR)/%.xml
	@mkdir -p $(@D)
	$(WAYLAND_SCANNER) client-header $< $@

$(PROTO_OUT)/%-protocol.c: $(PROTO_XML_DIR)/%.xml
	@mkdir -p $(@D)
	$(WAYLAND_SCANNER) private-code $< $@

# The compiled-in fallback font (`purp_bundled_font`, see include/ui.h), used when Fontconfig
# finds no usable monospace face.
define EMBED_FONT_PY
import sys
src, dst = sys.argv[1], sys.argv[2]
data = open(src, "rb").read()
with open(dst + ".tmp", "w") as out:
    out.write("/* Generated from %s by `make protocols`; do not edit. */\n" % src)
    out.write("#include <stddef.h>\n\n")
    out.write("const unsigned char purp_bundled_font[] __attribute__((aligned(16))) = {\n")
    for i in range(0, len(data), 16):
        out.write("    " + ", ".join("0x%02x" % b for b in data[i:i + 16]) + ",\n")
    out.write("};\n\nconst size_t purp_bundled_font_len = %d;\n" % len(data))
endef
export EMBED_FONT_PY

$(FONT_C): $(FONT_FILE)
	@mkdir -p $(@D)
	$(PYTHON) -c "$$EMBED_FONT_PY" $< $@
	@mv -f $@.tmp $@

# ---------------------------------------------------------------------------------------------
# Objects
# ---------------------------------------------------------------------------------------------

# Every compositor object waits for the generated headers (order-only), -MMD tracks the rest.
$(OBJ)/src/%.o: src/%.c | $(PROTO_SERVER_H) $(PROTO_CLIENT_H)
	@mkdir -p $(@D)
	$(CC) $(BASE_CFLAGS) $(COMP_CFLAGS) $(CFLAGS) -c $< -o $@

$(OBJ)/tests/%.o: tests/%.c | $(PROTO_SERVER_H) $(PROTO_CLIENT_H)
	@mkdir -p $(@D)
	$(CC) $(BASE_CFLAGS) $(COMP_CFLAGS) $(CFLAGS) -c $< -o $@

# wayland-scanner output is third-party code: compile it without the project warning set.
$(OBJ)/protocols/%-protocol.o: $(PROTO_OUT)/%-protocol.c
	@mkdir -p $(@D)
	$(CC) -std=gnu11 $(OPTFLAGS) $(COMP_CFLAGS) $(CFLAGS) -c $< -o $@

$(OBJ)/embedded_font.o: $(FONT_C)
	@mkdir -p $(@D)
	$(CC) -std=gnu11 $(OPTFLAGS) $(CFLAGS) -c $< -o $@

# ---------------------------------------------------------------------------------------------
# Binaries
# ---------------------------------------------------------------------------------------------

$(COMPOSITOR): $(COMP_OBJS)
	$(CC) $(LDFLAGS) -o $@ $^ $(COMP_LIBS)

$(TEST_BIN): $(TEST_OBJS)
	$(CC) $(LDFLAGS) -o $@ $^ $(TEST_LIBS)

# Companion tools are single-file programs; their dependency files go to build/obj/tools/.
$(POLKIT_AGENT): tools/purpwm-plus-polkit-agent.c
	@mkdir -p $(@D) $(OBJ)/tools
	$(CC) $(BASE_CFLAGS) -MF $(OBJ)/tools/$(@F).d $(POLKIT_CFLAGS) $(CFLAGS) $(LDFLAGS) \
	    -o $@ $< $(POLKIT_LIBS)

$(PORTAL): tools/purpwm-plus-portal.c
	@mkdir -p $(@D) $(OBJ)/tools
	$(CC) $(BASE_CFLAGS) -MF $(OBJ)/tools/$(@F).d $(PORTAL_CFLAGS) $(CFLAGS) $(LDFLAGS) \
	    -o $@ $< $(PORTAL_LIBS)

$(KEYRING): tools/purpwm-plus-keyring.c
	@mkdir -p $(@D) $(OBJ)/tools
	$(CC) $(BASE_CFLAGS) -MF $(OBJ)/tools/$(@F).d $(KEYRING_CFLAGS) $(CFLAGS) $(LDFLAGS) \
	    -o $@ $< $(KEYRING_LIBS)

# ---------------------------------------------------------------------------------------------
# Header self-containment and Wayland symbol-collision checks
# ---------------------------------------------------------------------------------------------

HEADER_CHECK_FLAGS := -std=gnu11 -Wall -Wextra -Werror -fsyntax-only $(DEFS) -Iinclude \
                      -I$(PROTO_OUT) $(COMP_CFLAGS)
PURE_HEADERS       := config.h types.h wm.h ipc.h ui.h host_backend.h

check-headers: $(PROTO_SERVER_H) $(PROTO_CLIENT_H)
	@for h in $(PURE_HEADERS) purpwm.h render.h; do \
	    printf '#include "%s"\n' "$$h" | $(CC) $(HEADER_CHECK_FLAGS) -x c - || exit 1; \
	    echo "  ok  $$h (standalone)"; \
	done
	@for h in $(PURE_HEADERS); do \
	    printf '#include "%s"\n#ifdef WAYLAND_SERVER_CORE_H\n#error "%s pulls in wayland-server"\n#endif\n#ifdef WAYLAND_CLIENT_CORE_H\n#error "%s pulls in wayland-client"\n#endif\n' \
	        "$$h" "$$h" "$$h" | $(CC) $(HEADER_CHECK_FLAGS) -x c - || exit 1; \
	done
	@echo "  ok  pure headers are free of libwayland"
	@printf '#include "purpwm.h"\n#include "render.h"\n#include "config.h"\n#include "types.h"\n#include "wm.h"\n#include "ipc.h"\n#include "ui.h"\n#include "host_backend.h"\n' \
	    | $(CC) $(HEADER_CHECK_FLAGS) -x c - && echo "  ok  server side: purpwm.h + render.h"
	@printf '#include "host_backend.h"\n#include <wayland-client.h>\n#include <wayland-egl.h>\n#include "xdg-shell-client-protocol.h"\n#include "wm.h"\n#include "ipc.h"\n#include "ui.h"\n' \
	    | $(CC) $(HEADER_CHECK_FLAGS) -x c - && echo "  ok  client side: host_backend.h + wayland-client + xdg-shell-client"

# ---------------------------------------------------------------------------------------------
# Install / clean
# ---------------------------------------------------------------------------------------------

install: $(BINARIES)
	$(INSTALL) -d $(DESTDIR)$(BINDIR)
	$(INSTALL) -m 0755 $(BINARIES) $(DESTDIR)$(BINDIR)/
	$(INSTALL) -d $(DESTDIR)$(DATADIR)/licenses/purpwm+
	$(INSTALL) -m 0644 LICENSE assets/fonts/DejaVuSansMono-LICENSE \
	    $(DESTDIR)$(DATADIR)/licenses/purpwm+/

uninstall:
	rm -f $(BINARIES:$(BUILD)/%=$(DESTDIR)$(BINDIR)/%)
	rm -rf $(DESTDIR)$(DATADIR)/licenses/purpwm+

clean:
	rm -rf $(BUILD)

-include $(shell find $(OBJ) -name '*.d' 2>/dev/null)
