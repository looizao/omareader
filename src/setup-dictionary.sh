#!/bin/sh
# Installs one of FreeDict's dictionaries, for looking words up while
# reading. omareader setup --dictionary passes the dictionary's name, the
# address of its StarDict release and the release's SHA-512, all pinned in
# omareader (src/dictionaries.tsv). This step uses the network; looking a
# word up never does.
#
# Safe to run again: a dictionary already in place and verified is left
# alone. Pass --dry-run first to see what it would do without changing
# anything.
set -eu

DRY_RUN=0
if [ "${1:-}" = "--dry-run" ]; then
  DRY_RUN=1
  shift
fi
if [ "$#" -ne 3 ]; then
  echo "Use: setup-dictionary.sh [--dry-run] <name> <url> <sha512>" >&2
  exit 1
fi
NAME="$1"
URL="$2"
SHA512="$3"

DATA_DIR="${XDG_DATA_HOME:-$HOME/.local/share}/omareader"
TARGET="$DATA_DIR/dictionaries/$NAME"

say() { printf '%s\n' "$*"; }

case "$NAME" in
  "" | *[!a-z-]*)
    say "Not a dictionary name: $NAME"
    exit 1
    ;;
esac

missing=""
for tool in curl sha512sum tar xz; do
  command -v "$tool" >/dev/null 2>&1 || missing="$missing $tool"
done
if [ -n "$missing" ]; then
  say "Missing:$missing"
  say "Install them first, for example: omarchy-pkg-add curl tar xz"
  exit 1
fi

if [ -f "$TARGET/.sha512" ] && [ "$(cat "$TARGET/.sha512")" = "$SHA512" ]; then
  say "Dictionary $NAME: already in place ($TARGET)"
  exit 0
fi

if [ "$DRY_RUN" = 1 ]; then
  say "would download $URL"
  say "would verify sha512 $SHA512"
  say "would unpack it into $TARGET"
  exit 0
fi

# Built beside the dictionaries, not among them, so a lookup meanwhile never
# finds half a dictionary, and moved into place once it is whole.
mkdir -p "$DATA_DIR/dictionaries"
work="$(mktemp -d "$DATA_DIR/.dictionary-XXXXXX")"
trap 'rm -rf "$work"' EXIT INT TERM

say "Dictionary $NAME: downloading $URL"
curl -fL --retry 3 -o "$work/release.tar.xz" "$URL"
if ! printf '%s  %s\n' "$SHA512" "$work/release.tar.xz" | sha512sum -c --status; then
  say "The download of $NAME did not match its checksum; nothing was installed."
  exit 1
fi
mkdir "$work/unpacked"
tar -xJf "$work/release.tar.xz" -C "$work/unpacked" --no-same-owner --no-same-permissions
# FreeDict's releases unpack into a folder named after the dictionary.
if [ ! -d "$work/unpacked/$NAME" ]; then
  say "The release of $NAME did not hold the folder $NAME; nothing was installed."
  exit 1
fi
printf '%s\n' "$SHA512" > "$work/unpacked/$NAME/.sha512"
if [ -e "$TARGET" ]; then
  mv "$TARGET" "$work/previous"
fi
mv "$work/unpacked/$NAME" "$TARGET"
say "Dictionary $NAME: installed in $TARGET"
