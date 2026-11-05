---
name: omareader
description: Read ebooks (EPUB, PDF, FB2, DOCX, ODT, Markdown, HTML, TXT, and Kindle formats through Calibre) as plain text from the command line with omareader, and check its offline Kokoro read-aloud setup. Use when asked to extract a book's text or chapters, inspect a book's metadata, export a book as an MP3 or M4B audiobook, or troubleshoot omareader's voice.
---

# omareader

omareader is an offline ebook reader for Omarchy that reads books aloud with
Kokoros and highlights each word as it's spoken. Its commands need no display.

| Command | What it does |
| --- | --- |
| `omareader info <book> [--json]` | Title, author, format, chapters with paragraph, sentence and word counts |
| `omareader text <book> [--chapter N] [--json]` | The book's text as omareader reads it, one paragraph per line, chapters numbered from 1 |
| `omareader export <book> [--split] [--format mp3\|m4b] [--voice ID] [--speed X] [--output DIR] [--chapters N-M] [--json]` | Reads the book aloud into one audio file with chapter markers, or with `--split` a folder named after the book with one file per chapter; prints the path. Takes a while: Kokoros synthesises every sentence. Needs ffmpeg |
| `omareader doctor [--json]` | Whether read-aloud is ready: the koko binary, the model, the voices |
| `omareader setup [--dry-run]` | Installs Kokoros and the Kokoro model (the one step that uses the network) |
| `omareader open [book]` | Opens the window |

Every command exits 0 on success and 1 on failure, with the reason on stderr.
With `--json` the result is a single JSON object on stdout.

The text is what the narrator speaks: formatting, images, and footnote markers
are dropped, and headings come out as their own paragraphs. omareader never
modifies books; its library lives in `~/.local/share/omareader/library.json`,
and text pasted into it in `~/.local/share/omareader/texts/*.json`, which
`info` and `text` read like any book.
