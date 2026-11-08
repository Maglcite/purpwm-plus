#!/bin/sh
# purpwm+ installer — builds with make and installs the compositor, its
# companion daemons and the session entry.
#
#   ./install.sh [--user|--system] [--prefix DIR] [--destdir DIR]
#                [--no-build] [--binary FILE] [--dry-run] [--help]
#
# Builds run as the invoking user; only system file copies use sudo.

set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)

SCOPE=""
PREFIX=""
DESTDIR=""
NO_BUILD=0
DRY_RUN=0
BINARY=""

usage() {
    cat >&2 <<'EOF'
Usage: ./install.sh [options]
  --user       Install under ~/.local (default for regular users)
  --system     Install under /usr/local system-wide (default for root)
  --prefix DIR Override the install prefix
  --destdir DIR Stage files under DIR (runtime paths are unchanged)
  --no-build   Skip `make` and install an existing binary
  --binary FILE Existing purpwm+ executable (implies --no-build location)
  --dry-run    Show the plan without building or changing files
  --help       Show this help
EOF
    exit "${1:-2}"
}

while [ $# -gt 0 ]; do
    case "$1" in
        --user) SCOPE=user ;;
        --system) SCOPE=system ;;
        --prefix) shift; [ $# -gt 0 ] || usage; PREFIX=$1 ;;
        --prefix=*) PREFIX=${1#*=} ;;
        --destdir) shift; [ $# -gt 0 ] || usage; DESTDIR=$1 ;;
        --destdir=*) DESTDIR=${1#*=} ;;
        --no-build) NO_BUILD=1 ;;
        --binary) shift; [ $# -gt 0 ] || usage; BINARY=$1 ;;
        --binary=*) BINARY=${1#*=} ;;
        --dry-run) DRY_RUN=1 ;;
        --help|-h) usage 0 ;;
        *) echo "install.sh: unknown option '$1'" >&2; usage ;;
    esac
    shift
done

if [ -z "$SCOPE" ]; then
    if [ "$(id -u)" -eq 0 ]; then SCOPE=system; else SCOPE=user; fi
fi

if [ -n "$BINARY" ]; then NO_BUILD=1; fi

if [ -z "$PREFIX" ]; then
    if [ "$SCOPE" = system ]; then PREFIX=/usr/local; else PREFIX=$HOME/.local; fi
fi
case "$PREFIX" in /*) ;; *) echo "install.sh: --prefix must be absolute" >&2; exit 1 ;; esac
if [ -n "$DESTDIR" ]; then
    case "$DESTDIR" in /*) ;; *) DESTDIR=$(CDPATH= cd -- "$(dirname -- "$DESTDIR")" && pwd)/$(basename -- "$DESTDIR") ;; esac
    if [ "$DESTDIR" = / ]; then
        echo "install.sh: DESTDIR must be a staging directory, not /" >&2; exit 1
    fi
fi

BIN_DIR="$PREFIX/bin"
SHARE_DIR="$PREFIX/share/purpwm+"
if [ "$SCOPE" = system ]; then
    SESSION_DIR=/usr/share/wayland-sessions
else
    SESSION_DIR="${XDG_DATA_HOME:-$HOME/.local/share}/wayland-sessions"
fi
PAM_DIR=/etc/pam.d

# Run a command, escalating with sudo for system installs when needed.
run() {
    if [ "$SCOPE" = system ] && [ -z "$DESTDIR" ] && [ "$(id -u)" -ne 0 ]; then
        sudo "$@"
    else
        "$@"
    fi
}

# Install SOURCE as MODE DEST, through a sibling temp + atomic rename so a
# running compositor binary is never truncated mid-swap.
install_file() {
    src=$1; dst=$2; mode=$3
    target="$DESTDIR$dst"
    if [ -d "$target" ]; then
        echo "install.sh: cannot replace a directory: $target" >&2; exit 1
    fi
    tmp=$(dirname -- "$target")/.$(basename -- "$target").install-$$
    if [ "$DRY_RUN" = 1 ]; then
        printf '  install -m %s %s -> %s\n' "$mode" "$src" "$target"
        return
    fi
    run install -D -m "$mode" -- "$src" "$tmp"
    run mv -fT -- "$tmp" "$target"
}

# Locate a built executable in the tree or on PATH.
find_artifact() {
    name=$1
    for candidate in "$SCRIPT_DIR/build/$name" "$SCRIPT_DIR/$name" \
                     "$SCRIPT_DIR/bin/$name" "$SCRIPT_DIR/tools/$name"; do
        if [ -x "$candidate" ] && [ ! -d "$candidate" ]; then
            printf '%s\n' "$candidate"
            return 0
        fi
    done
    command -v "$name" 2>/dev/null || true
}

printf 'Install scope: %s\nExecutables:    %s\nSession config: %s\nLogin session:  %s\nLock PAM:       %s\n' \
    "$SCOPE" "$DESTDIR$BIN_DIR" "$DESTDIR$SHARE_DIR/session.conf" \
    "$DESTDIR$SESSION_DIR/purpwm+.desktop" "$DESTDIR$PAM_DIR/purpwm+"
[ -n "$DESTDIR" ] && printf 'Staging root:   %s (runtime paths remain as shown above)\n' "$DESTDIR"
if [ "$DRY_RUN" = 1 ]; then
    if [ "$NO_BUILD" = 0 ]; then printf '  make\n'; fi
    printf 'Dry run: no changes made.\n'
fi

# ---- build -----------------------------------------------------------------
if [ "$NO_BUILD" = 0 ] && [ "$DRY_RUN" = 0 ]; then
    if [ "$(id -u)" -eq 0 ]; then
        echo "install.sh: run without sudo so the build stays in your user account." >&2
        exit 1
    fi
    if ! command -v make >/dev/null 2>&1; then
        echo "install.sh: make is missing; install it or pass --no-build." >&2
        exit 1
    fi
    make -C "$SCRIPT_DIR"
fi

if [ -n "$BINARY" ]; then
    COMPOSITOR=$BINARY
else
    COMPOSITOR=$(find_artifact purpwm+)
fi
if [ "$DRY_RUN" = 0 ]; then
    if [ -z "$COMPOSITOR" ] || [ ! -x "$COMPOSITOR" ]; then
        echo "install.sh: purpwm+ executable not found. Build first, or use --no-build --binary /path/to/purpwm+." >&2
        exit 1
    fi
    TOOL_POLKIT=$(find_artifact "purpwm+-polkit-agent")
    TOOL_PORTAL=$(find_artifact "purpwm+-portal")
    TOOL_KEYRING=$(find_artifact "purpwm+-keyring")
    if [ -z "$TOOL_POLKIT" ] || [ -z "$TOOL_PORTAL" ] || [ -z "$TOOL_KEYRING" ]; then
        echo "install.sh: companion tools not found. Build first (make), or pass --no-build with tools already built." >&2
        exit 1
    fi
fi
# ---- staging area ------------------------------------------------------------
STAGE=""
cleanup() { [ -n "$STAGE" ] && [ -d "$STAGE" ] && rm -rf -- "$STAGE"; }
if [ "$DRY_RUN" = 0 ]; then
    STAGE=$(mktemp -d /tmp/purpwm+-install.XXXXXX)
    trap cleanup EXIT INT TERM
fi


# ---- staged files ------------------------------------------------------------
if [ "$DRY_RUN" = 0 ]; then
# The launcher reads a baked-in session.conf path after install.
sed "s|# INSTALLER_SESSION_CONFIG|PURPWM_SESSION_CONFIG=\${PURPWM_SESSION_CONFIG-\"$SHARE_DIR/session.conf\"}|" \
    "$SCRIPT_DIR/scripts/start-purpwm+" > "$STAGE/start-purpwm+"

cat > "$STAGE/session.conf" <<EOF
# Generated by purpwm+ install.sh. Environment variables override these defaults.
export PURPWM_AUTOSTART=\${PURPWM_AUTOSTART:-1}
export PURPWM_DEFAULT_SCALE=\${PURPWM_DEFAULT_SCALE:-1.5}
export PURPWM_REFRESH_RATE=\${PURPWM_REFRESH_RATE:-120}
export PURPWM_RESOLUTION=\${PURPWM_RESOLUTION:-auto}
export PURPWM_BACKEND=\${PURPWM_BACKEND:-drm}
EOF

# Desktop entry with the real Exec path; keep DesktopNames under the X- prefix
# so desktop-file-validate accepts the Application entry.
sed -e "s|^Exec=.*|Exec=$BIN_DIR/start-purpwm+|" \
    -e "s|^TryExec=.*|TryExec=$BIN_DIR/start-purpwm+|" \
    "$SCRIPT_DIR/purpwm+.desktop" > "$STAGE/purpwm+.desktop"

# Lock PAM service: reuse a distro stack when one is visible.
PAM_CONTENT=""
if [ "$SCOPE" = system ] && [ ! -f "$DESTDIR$PAM_DIR/purpwm+" ]; then
    if [ -f "$PAM_DIR/common-auth" ] && [ -f "$PAM_DIR/common-account" ]; then
        PAM_CONTENT="common"
    elif [ -f "$PAM_DIR/system-auth" ]; then
        PAM_CONTENT="system-auth"
    elif [ -f "$PAM_DIR/password-auth" ]; then
        PAM_CONTENT="password-auth"
    fi
    case "$PAM_CONTENT" in
        common)
            cat > "$STAGE/pam" <<'EOF'
#%PAM-1.0
auth       include      common-auth
account    include      common-account
EOF
            ;;
        system-auth|password-auth)
            printf '#%%PAM-1.0\nauth       include      %s\naccount    include      %s\n' \
                "$PAM_CONTENT" "$PAM_CONTENT" > "$STAGE/pam"
            ;;
    esac
    if [ -f "$PAM_DIR/purpwm" ]; then
        # Keep parity with purpwm v1 lock entries.
        cp -- "$PAM_DIR/purpwm" "$STAGE/pam-purpwm" 2>/dev/null || true
    fi
fi
fi

# ---- install -----------------------------------------------------------------
if [ "$DRY_RUN" = 0 ]; then
install_file "$COMPOSITOR" "$BIN_DIR/purpwm+" 755
install_file "$STAGE/start-purpwm+" "$BIN_DIR/start-purpwm+" 755
install_file "$SCRIPT_DIR/scripts/purpwm+-bt" "$BIN_DIR/purpwm+-bt" 755
install_file "$SCRIPT_DIR/scripts/purpwm+-wifi" "$BIN_DIR/purpwm+-wifi" 755
install_file "$TOOL_POLKIT" "$BIN_DIR/purpwm+-polkit-agent" 755
install_file "$TOOL_PORTAL" "$BIN_DIR/purpwm+-portal" 755
install_file "$TOOL_KEYRING" "$BIN_DIR/purpwm+-keyring" 755
install_file "$STAGE/session.conf" "$SHARE_DIR/session.conf" 644
install_file "$STAGE/purpwm+.desktop" "$SESSION_DIR/purpwm+.desktop" 644
else
printf '  install -m 755 purpwm+, start-purpwm+, purpwm+-* -> %s\n' "$DESTDIR$BIN_DIR"
printf '  install -m 644 session.conf -> %s\n' "$DESTDIR$SHARE_DIR/session.conf"
printf '  install -m 644 purpwm+.desktop -> %s\n' "$DESTDIR$SESSION_DIR/purpwm+.desktop"
fi

if [ "$SCOPE" = system ]; then
    if [ -f "$STAGE/pam" ]; then
        install_file "$STAGE/pam" "$PAM_DIR/purpwm+" 644
    elif [ "$DRY_RUN" = 0 ] && [ ! -f "$DESTDIR$PAM_DIR/purpwm+" ]; then
        echo "install.sh: no supported PAM stack found; configure $PAM_DIR/purpwm+ manually." >&2
    fi
    if [ -f "$STAGE/pam-purpwm" ] && [ ! -f "$DESTDIR$PAM_DIR/purpwm" ]; then
        install_file "$STAGE/pam-purpwm" "$PAM_DIR/purpwm" 644
    fi
fi

if [ "$DRY_RUN" = 0 ]; then
    printf '\npurpwm+ installed. Session settings: %s\n' "$DESTDIR$SHARE_DIR/session.conf"
    if [ -n "$DESTDIR" ]; then
        printf 'Files were staged only; the live system was not installed.\n'
    elif [ "$SCOPE" = system ]; then
        printf 'Log out and select purpwm+ at the login screen.\n'
    else
        printf 'Try it nested: %s --no-autostart\n' "$BIN_DIR/purpwm+"
    fi
fi
