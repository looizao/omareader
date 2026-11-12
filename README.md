# omareader

![omareader reading Alice's Adventures in Wonderland aloud, with the spoken word highlighted](docs/screenshot.png)

An ebook reader for [Omarchy](https://omarchy.org) that reads your books aloud
with [Kokoro](https://huggingface.co/hexgrad/Kokoro-82M) and highlights each
word as it's spoken, so you can follow along. Inspired by
[Readest](https://readest.com), built the way Omarchy's own apps are: one
window, C++ and Qt Quick, driven from the keyboard, following your theme and
language.

Everything runs on your machine. omareader has no accounts and talks to the
network only when you ask it to (setup, and adding a web page): books are
parsed locally and the voice is
[Kokoros](https://github.com/lucasjinreal/Kokoros), a Rust build of Kokoro that
runs on the CPU. If you have an NVIDIA GPU, you can add
[Qwen3-TTS](https://github.com/QwenLM/Qwen3-TTS) as a second voice engine,
for a more natural, expressive reading.

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

### Qwen3-TTS (optional)

```sh
omareader setup --engine qwen              # the 0.6B model, fits a 6 GB GPU
omareader setup --engine qwen --model 1.7b # richer, needs about 7 GB free
omareader doctor --engine qwen             # is it ready, and will it fit?
```

This builds a private Python 3.12 environment with PyTorch in
`~/.local/share/omareader/qwen` (about 14 GB, mostly CUDA libraries) and
downloads the CustomVoice model and the forced aligner, which times each
spoken word, into `~/.local/share/omareader/models/qwen`. Every package and
model file is pinned to a version and checked against its SHA-256 sum, and
rerunning the setup keeps what is already there and verified. It needs
`omarchy-pkg-add uv curl`. Afterwards Qwen3-TTS runs fully offline.

It wants an NVIDIA GPU with about 4 GB free for the 0.6B model;
`omareader doctor` says how much is free and what that means. Without enough,
it runs on the CPU, about three times slower than real time: fine for an
export left running, too slow to listen along. Kokoro stays the default and
keeps working whatever happens to Qwen3-TTS, and omareader never switches
engines on its own.

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

## Pasted text

For an article, a note or a message with no book file, press N in the library
(or Ctrl+V to paste the clipboard straight in), or click Add text. The text is
saved in `~/.local/share/omareader/texts`, readable only by you, and reads
aloud like any book, with its place remembered. Removing it (Delete, or the
× on its card, after a confirmation) moves it to the trash, so it can still
be restored.

Pasted text is read as Markdown, as text from chat apps and notes often is:
`**bold**`, `*italic*`, `` `code` ``, `~~struck~~`, headings, lists and links
show formatted, and the voice reads the words, never the marks. Headings stay
headings, but the text stays one chapter. Code blocks are skipped, as in
Markdown books. Lines with no blank line between them stay
separate paragraphs.

The title starts as the text's first few words (or its heading, when it opens
with one), and you can type your own.
If a coding agent with a read-only headless mode is installed (Claude Code,
Codex, Gemini or Cursor, with Omarchy's default agent first), the dialog
offers to have it suggest a title. That is off until you tick it, because the
agent sends the start of the text to its provider; omareader itself still
makes no network connection. The agent runs one turn in an empty folder with
no tools, and if it isn't signed in, fails or takes longer than 45 seconds,
the first words stay as the title.

## Web pages

To read an article from the web, press U in the library (or Ctrl+V with its
link copied), or click Add web page, and give its address. omareader fetches
that one page, finds the article in it the way Firefox's Reader View does,
and shows you its text and title to check before anything is saved. The
site's menus, adverts, sidebars, comments and related links are left out;
headings, paragraphs, lists and bold or italic words stay.

The page is fetched only when you press Fetch, and only the page itself:
nothing it links to (images, styles, scripts) is loaded, none of its scripts
run, and no cookies are kept from one fetch to the next. Redirects are
followed only to other web pages, never from https down to http. Once added,
it is kept like pasted text, with the address it came from, and reads
offline from then on; it is never fetched again. Pages that need a login, or
that build their text with scripts, have nothing to add, and the dialog says
so. Esc stops a fetch that is taking too long.

## Reading aloud

Press Space. Kokoros synthesises ahead of you in batches of sentences, and as
each sentence plays, its words light up in the theme's accent. Click any word
to start reading from there. The view follows the voice; scroll away and it
stays put until you press F.

Kokoro's voices cover American and British English, Spanish, French, Hindi,
Italian, Japanese, Brazilian Portuguese and Mandarin; pick one that matches
the book's language. Qwen3-TTS has nine voices that each read English,
Portuguese, Spanish, French, German, Italian, Russian, Chinese, Japanese and
Korean; pick the language separately.

Press A for the voice settings. Engine, voice, language and speed sit at the
top, with a preview button and a reset to the recommended settings. Advanced
opens the rest, each with its range, current value and what it does: pauses
and volume for both engines, a second voice to blend for Kokoro, and for
Qwen3-TTS the model, the device, how varied the intonation is, and a take
number for another reading of the same text. Each engine remembers its own
settings. Changing one while reading rebuilds the audio ahead from the
sentence you are on.

## Exporting audio

Press E (or the ⤓ button) to turn the whole book into audio for your phone, a
podcast app or an audiobook player. Choose one file with chapter markers or a
folder with one file per chapter, as MP3 (plays everywhere) or M4B (an AAC
audiobook). The files use the voice and speed you read with, carry the title,
author, track numbers and cover, and land in `~/Music/Audiobooks` unless you
pick another folder.

The voice reads the whole book first, which takes a while for a long book
(Kokoros runs several times faster than real time on a recent CPU, Qwen3-TTS
about twice as fast on a laptop GPU). An export keeps the voice settings it
started with, whatever you change afterwards. The export keeps
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
| N, Ctrl+V | In the library: add pasted text |
| U, Ctrl+V with a link | In the library: add a web page |
| B or Backspace | Back to the library |
| Enter, Delete | In the library: open, remove a book after a confirmation (the file stays; pasted text goes to the trash) |
| Esc | Close panels and messages |
| ? | Show every shortcut |
| Q | Quit |

## Command line

The commands need no display, so scripts and coding agents can use them. Each
exits 0 or 1 with errors on stderr, and `--json` prints one JSON object.

```sh
omareader info book.epub --json          # metadata and chapters
omareader text book.epub --chapter 3     # the text as omareader reads it
omareader fetch https://example.com/post # a web page's article, without adding it
omareader export book.epub --split       # audio, one MP3 per chapter
omareader export book.epub --format m4b  # one M4B audiobook with chapters
omareader export book.epub --engine qwen --voice ryan --tune temperature=0.7
omareader voices --engine qwen           # voices, languages and tuning ranges
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
- `src/qwen-worker.py` keeps Qwen3-TTS loaded in one Python process for the
  whole app; `src/qwen.cpp` feeds it batches of sentences, whatever someone
  is listening to first and exports in the gaps, and cancels what nobody
  needs any more. The forced aligner gives each word its real start and end.
- `src/synthesizer.cpp` puts both engines behind one contract, a sentence's
  audio plus its word timings, which narration, the preview and exports all
  use. It applies the speed (Qwen3-TTS is time-stretched, its word times
  scaled to match) and the volume.
- `src/voicesettings.cpp` lists each engine's voices, languages and tuning,
  with ranges and recommended values, and remembers them per engine.
- `src/speech.cpp` matches the spoken words back onto the words on the page and
  decodes the audio; `src/narrator.cpp` plays the sentences as one stream and
  maps the playback position to the word being heard.
- `src/library.cpp` keeps pasted text as small JSON files the loader reads
  like plain text; `src/titleagent.cpp` asks a coding agent for a title.
- `src/webimport.cpp` fetches a web page the user asked for, and only that
  page; `src/article.cpp` parses it without running anything, scores its
  blocks as Mozilla's Readability does to find the article, and writes it
  out as the Markdown pasted text is kept in.
- `src/highlight.cpp` colours that word inside the paragraph without changing
  its layout.
- `src/exporter.cpp` synthesises the book for exports and pipes the
  samples into `ffmpeg`, then joins the chapters into one file with chapter
  markers, or keeps them as a folder of tracks.

## Development

```sh
bin/build        # build/omareader
bin/test         # Qt Test suite, offscreen
bin/install      # ~/.local/bin and ~/.local/share, no sudo
```

CI builds and tests every push and pull request in an Arch Linux container,
builds the package, and lints the shell scripts, all at once, with a
compiler cache kept between runs. Every push to main
is a release: `bin/release patch` (or `minor` for a new feature) bumps
`VERSION` and the PKGBUILD, commits, and pushes, and CI tags the version and
publishes a GitHub release with the source tarball, the Arch package, and a
PKGBUILD that builds from the tarball. A push to main without a bump fails.

## License

MIT. The theme, language and file-chooser code follows Omacut and Monologue by
David Heinemeier Hansson, also MIT.
