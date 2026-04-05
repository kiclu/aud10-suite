#!/usr/bin/env bash
# Install build/runtime prerequisites for aud10-suite (C++ JACK client).
# Requires: root for package installs, or run the printed commands yourself.
set -euo pipefail

usage() {
    cat <<'EOF'
Usage: install-prereqs.sh [--dry-run]

  --dry-run   Print package commands only; do not install.

Detects the OS/distro and installs packages needed to build aud10 with CMake:
  cmake (>= 3.16), C++17 compiler, pkg-config, ncurses dev, JACK dev.
EOF
}

DRY_RUN=0
for a in "$@"; do
    case "$a" in
        -h|--help) usage; exit 0 ;;
        --dry-run) DRY_RUN=1 ;;
        *) echo "Unknown option: $a" >&2; usage >&2; exit 2 ;;
    esac
done

run() {
    if [[ "$DRY_RUN" -eq 1 ]]; then
        printf 'DRY-RUN: '; printf '%q ' "$@"; echo
        return 0
    fi
    "$@"
}

if [[ "${EUID:-$(id -u)}" -ne 0 ]] && [[ "$DRY_RUN" -eq 0 ]]; then
    echo "This script installs system packages and should be run as root (sudo)." >&2
    echo "Re-run with: sudo $0" >&2
    echo "Or use --dry-run to see commands." >&2
    exit 1
fi

if [[ "$(uname -s)" != "Linux" ]]; then
    echo "Non-Linux detected. Install manually:" >&2
    echo "  - CMake >= 3.16, C++17 compiler (GCC or Clang)" >&2
    echo "  - pkg-config, ncurses dev headers/libs, JACK dev (libjack)" >&2
    echo "  macOS (Homebrew): brew install cmake pkg-config ncurses jack" >&2
    exit 1
fi

if [[ ! -r /etc/os-release ]]; then
    echo "Cannot read /etc/os-release; install cmake, g++, pkg-config, ncurses-devel, jack-devel manually." >&2
    exit 1
fi

# shellcheck source=/dev/null
. /etc/os-release

ID_LIKE="${ID_LIKE:-}"
case "${ID:-}" in
    fedora|rhel|centos|rocky|almalinux|ol)
        # RHEL 9 / Fedora family
        PKGS=(cmake gcc-c++ make pkgconf-pkg-config ncurses-devel jack-audio-connection-kit-devel)
        run dnf install -y "${PKGS[@]}"
        ;;
    debian|ubuntu|linuxmint|pop)
        PKGS=(build-essential cmake pkg-config libncurses-dev libjack-jackd2-dev)
        if ! run apt-get update; then
            echo "apt-get update failed." >&2
            exit 1
        fi
        run apt-get install -y "${PKGS[@]}"
        ;;
    arch|manjaro)
        PKGS=(base-devel cmake pkgconf ncurses jack2)
        run pacman -S --needed --noconfirm "${PKGS[@]}"
        ;;
    opensuse*|sles)
        PKGS=(cmake gcc-c++ make pkg-config ncurses-devel libjack-devel)
        run zypper --non-interactive install "${PKGS[@]}"
        ;;
    alpine)
        PKGS=(build-base cmake pkgconf ncurses-dev jack-dev)
        run apk add --no-cache "${PKGS[@]}"
        ;;
    *)
        if echo "$ID_LIKE" | grep -q debian; then
            PKGS=(build-essential cmake pkg-config libncurses-dev libjack-jackd2-dev)
            run apt-get update
            run apt-get install -y "${PKGS[@]}"
        elif echo "$ID_LIKE" | grep -q rhel; then
            PKGS=(cmake gcc-c++ make pkgconf-pkg-config ncurses-devel jack-audio-connection-kit-devel)
            run dnf install -y "${PKGS[@]}"
        else
            echo "Unsupported distro ID='${ID:-unknown}'. Install:" >&2
            echo "  cmake >= 3.16, C++17 compiler, pkg-config, ncurses dev, JACK dev" >&2
            exit 1
        fi
        ;;
esac

if [[ "$DRY_RUN" -eq 0 ]]; then
    echo "Prerequisites installed. Build with:"
    echo "  cd /path/to/aud10 && cmake -S . -B build && cmake --build build"
fi
