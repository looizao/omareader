#!/bin/sh
# Installs what omareader reads aloud with: Kokoros (the koko command) and the
# Kokoro v1.0 model with word timestamps. This is the only step that uses the
# network; afterwards omareader reads fully offline.
#
# Safe to run again: anything already in place is checked and left alone.
# Pass --dry-run to see what it would do without changing anything.
set -eu

DRY_RUN=0
[ "${1:-}" = "--dry-run" ] && DRY_RUN=1

KOKOROS_REPO="https://github.com/lucasjinreal/Kokoros.git"
KOKOROS_REV="dda39518d210ba48a63782e524eaee435eab17a7"
MODEL_URL="https://huggingface.co/onnx-community/Kokoro-82M-v1.0-ONNX-timestamped/resolve/main/onnx/model.onnx"
MODEL_SHA256="651ea8291843a92276a4a003581a215cb07d15e47dde6fcfb1b768f9a1682054"
VOICES_URL="https://github.com/thewh1teagle/kokoro-onnx/releases/download/model-files-v1.0/voices-v1.0.bin"
VOICES_SHA256="bca610b8308e8d99f32e6fe4197e7ec01679264efed0cac9140fe9c29f1fbf7d"

MODEL_DIR="${OMAREADER_MODELS:-${XDG_DATA_HOME:-$HOME/.local/share}/omareader/models}"
PREFIX="$HOME/.local"

say() { printf '%s\n' "$*"; }
run() {
  if [ "$DRY_RUN" = 1 ]; then
    say "would run: $*"
  else
    "$@"
  fi
}

missing=""
for tool in curl sha256sum; do
  command -v "$tool" >/dev/null 2>&1 || missing="$missing $tool"
done

if command -v koko >/dev/null 2>&1 || [ -x "$PREFIX/bin/koko" ]; then
  say "Kokoros: already installed ($(command -v koko 2>/dev/null || echo "$PREFIX/bin/koko"))"
  need_koko=0
else
  need_koko=1
  for tool in cargo pkg-config; do
    command -v "$tool" >/dev/null 2>&1 || missing="$missing $tool"
  done
  pkg-config --exists opus 2>/dev/null || missing="$missing opus"
fi

if [ -n "$missing" ]; then
  say "Missing:$missing"
  say "Install them first, for example: omarchy-pkg-add rust pkgconf opus curl"
  exit 1
fi

if [ "$need_koko" = 1 ]; then
  say "Kokoros: building koko $KOKOROS_REV (a few minutes)"
  run cargo install --locked --git "$KOKOROS_REPO" --rev "$KOKOROS_REV" koko --root "$PREFIX"
fi

fetch() {
  url="$1"; sum="$2"; target="$3"
  if [ -f "$target" ] && printf '%s  %s\n' "$sum" "$target" | sha256sum -c --status; then
    say "Model: $(basename "$target") already in place"
    return
  fi
  say "Model: downloading $(basename "$target")"
  run mkdir -p "$(dirname "$target")"
  run curl -fL --retry 3 -o "$target.part" "$url"
  if [ "$DRY_RUN" = 1 ]; then
    say "would verify sha256 $sum and move into $target"
    return
  fi
  if ! printf '%s  %s\n' "$sum" "$target.part" | sha256sum -c --status; then
    rm -f "$target.part"
    say "The download of $(basename "$target") did not match its checksum; nothing was installed."
    exit 1
  fi
  mv "$target.part" "$target"
}

fetch "$MODEL_URL" "$MODEL_SHA256" "$MODEL_DIR/kokoro-v1.0.onnx"
fetch "$VOICES_URL" "$VOICES_SHA256" "$MODEL_DIR/voices-v1.0.bin"

case ":$PATH:" in
  *":$PREFIX/bin:"*) ;;
  *) say "Note: $PREFIX/bin is not on your PATH; omareader looks there anyway." ;;
esac
say "Done. omareader can read aloud offline now."
