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
bin/install          # builds the Arch package and installs it
omareader setup      # once: builds Kokoros and downloads the model
```

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

## Development

```sh
bin/build        # build/omareader
bin/test         # Qt Test suite, offscreen
bin/install      # makepkg -fsi
```

CI builds and tests every push and pull request in an Arch Linux container and
builds the package. To release, run `bin/release 0.2.0` on a clean main: it
bumps `VERSION` and the PKGBUILD and pushes, and CI tags `v0.2.0` and publishes
a GitHub release with the source tarball, the Arch package, and a PKGBUILD
that builds from the tarball.

## License

MIT. The theme, language and file-chooser code follows Omacut and Monologue by
David Heinemeier Hansson, also MIT.
