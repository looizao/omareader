# omareader

![omareader reading Alice's Adventures in Wonderland aloud, with the spoken word highlighted](docs/screenshot.png)

An ebook reader for [Omarchy](https://omarchy.org) that reads your books aloud
with [Kokoro](https://huggingface.co/hexgrad/Kokoro-82M) and highlights each
word as it's spoken, so you can follow along. Inspired by
[Readest](https://readest.com), built the way Omarchy's own apps are: one
window, C++ and Qt Quick, driven from the keyboard, following your theme and
language.

Everything runs on your machine. omareader has no accounts, no sync, and no
network code: books are parsed locally and the voice is
[Kokoros](https://github.com/lucasjinreal/Kokoros), a Rust build of Kokoro that
runs on the CPU.

## Install

```sh
git clone https://github.com/looizao/omareader.git
cd omareader
bin/install          # builds it and installs it for you, no sudo
omareader setup      # once: builds Kokoros and downloads the model
```

`bin/install` puts the binary in `~/.local/bin` and the icon and launcher
entry in `~/.local/share`; `bin/install --uninstall` removes them. It needs
the packages in the PKGBUILD's `depends` and prints the `pacman` command for
any that are missing. To install system-wide instead, build the package with
`cd pkgbuild && makepkg -si`, or use the one from a GitHub release.

`omareader setup` is the only step that uses the network. It builds the `koko`
command into `~/.local/bin` from a pinned Kokoros commit and downloads the
Kokoro v1.0 model with word timestamps (about 330 MB) into
`~/.local/share/omareader/models`, checking both downloads against pinned
SHA-256 sums. Run `omareader setup --dry-run` to see what it would do, and
`omareader doctor` to check the result. Building Kokoros needs
`omarchy-pkg-add rust pkgconf opus`.

omareader then shows up in the app launcher (Super + Space). You can also run
`omareader book.epub`.

## Books

| Format | How it opens |
| --- | --- |
| EPUB | Parsed directly, with the table of contents and cover |
| PDF | Text through poppler's `pdftotext`, reflowed into paragraphs |
| FB2, DOCX, ODT | Parsed directly |
| Markdown, HTML, plain text | Parsed directly; Project Gutenberg headers are dropped |
| MOBI, AZW3 and other Kindle formats | Converted once through Calibre's `ebook-convert`, if installed |

Every format becomes the same reflowable text in your chosen size, spacing,
column width and typeface. Books are never modified or copied; the library
only remembers where they are and where you stopped.

## Reading aloud

Press Space. Kokoros synthesises ahead of you in batches of sentences, and as
each sentence plays, its words light up in the theme's accent. Click any word
to start reading from there. The view follows the voice; scroll away and it
stays put until you press F.

Voices cover American and British English, Spanish, French, Hindi, Italian,
Japanese, Brazilian Portuguese and Mandarin. Pick one that matches the book's
language.

## Exporting audio

Press E (or the ⤓ button) to turn the whole book into audio for your phone, a
podcast app or an audiobook player. Choose one file with chapter markers or a
folder with one file per chapter, as MP3 (plays everywhere) or M4B (an AAC
audiobook). The files use the voice and speed you read with, carry the title,
author, track numbers and cover, and land in `~/Music/Audiobooks` unless you
pick another folder.

Kokoros reads the whole book first, which takes a while for a long book (it
runs several times faster than real time on a recent CPU). The export keeps
going after you close the dialog, at a lower priority so reading aloud stays
smooth. It is built in a hidden folder and renamed into place at the end, so
a cancelled export leaves nothing behind and an earlier export is never
replaced. Exports need `ffmpeg`.

## Keyboard

| Key | Action |
| --- | --- |
| Space | Read aloud, pause, resume |
| Click a word | Read aloud from there |
| `,` / `.` | Previous / next sentence |
| `[` / `]` | Slower / faster |
| S | Stop reading aloud |
| F | Back to the voice after scrolling |
| ← / → (or H / L) | Previous / next chapter |
| ↑ ↓ (or K / J), PgUp, PgDn | Scroll |
| T | Contents |
| A | Voice and appearance |
| E | Export as audio |
| + / − | Bigger / smaller text |
| O | Open a book |
| B or Backspace | Back to the library |
| Enter, Delete | In the library: open, forget a book (the file stays) |
| Esc | Close panels and messages |
| ? | Show every shortcut |
| Q | Quit |

## Command line

The commands need no display, so scripts and coding agents can use them. Each
exits 0 or 1 with errors on stderr, and `--json` prints one JSON object.

```sh
omareader info book.epub --json          # metadata and chapters
omareader text book.epub --chapter 3     # the text as omareader reads it
omareader export book.epub --split       # audio, one MP3 per chapter
omareader export book.epub --format m4b  # one M4B audiobook with chapters
omareader doctor --json                  # is read-aloud ready?
omareader skill install                  # agent skill for ~/.claude/skills
```

## How it works

- `src/bookloader.cpp` reduces every format to chapters of plain paragraphs;
  `src/book.cpp` splits those into sentences and words.
- `src/kokoro.cpp` runs `koko file` with `--timestamps` on a batch of
  sentences, one per line, and picks up each sentence's WAV and word-timing TSV
  as Kokoros writes them. Kokoros would download a missing model by itself, so
  omareader only starts it once both model files are already on disk.
- `src/speech.cpp` matches Kokoro's words back onto the words on the page and
  decodes the audio; `src/narrator.cpp` plays the sentences as one stream and
  maps the playback position to the word being heard.
- `src/highlight.cpp` colours that word inside the paragraph without changing
  its layout.
- `src/exporter.cpp` runs Kokoros once per chapter for exports and pipes the
  samples into `ffmpeg`, then joins the chapters into one file with chapter
  markers, or keeps them as a folder of tracks.

## Development

```sh
bin/build        # build/omareader
bin/test         # Qt Test suite, offscreen
bin/install      # ~/.local/bin and ~/.local/share, no sudo
```

CI builds and tests every push and pull request in an Arch Linux container and
builds the package. To release, run `bin/release 0.2.0` on a clean main: it
bumps `VERSION` and the PKGBUILD and pushes, and CI tags `v0.2.0` and publishes
a GitHub release with the source tarball, the Arch package, and a PKGBUILD
that builds from the tarball.

## License

MIT. The theme, language and file-chooser code follows Omacut and Monologue by
David Heinemeier Hansson, also MIT.
