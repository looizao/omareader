---
name: omareader
description: Read ebooks (EPUB, PDF, FB2, DOCX, ODT, Markdown, HTML, TXT, and Kindle formats through Calibre) as plain text from the command line with omareader, check its offline read-aloud setup (Kokoro, and optionally Qwen3-TTS), and look words up or translate them in offline dictionaries. Use when asked to extract a book's text or chapters, inspect a book's metadata, export a book as an MP3 or M4B audiobook, look up or translate a word, or troubleshoot omareader's voice.
---

# omareader

omareader is an offline ebook reader for Omarchy that reads books aloud with
Kokoros or, optionally, Qwen3-TTS on an NVIDIA GPU, and highlights each word
as it's spoken. Its commands need no display.

| Command | What it does |
| --- | --- |
| `omareader info <book> [--json]` | Title, author, format, chapters with paragraph, sentence and word counts |
| `omareader text <book> [--chapter N] [--json]` | The book's text as omareader reads it, one paragraph per line, chapters numbered from 1 |
| `omareader export <book> [--split] [--format mp3\|m4b] [--engine kokoro\|qwen] [--voice ID] [--language L] [--speed X] [--tune key=value]... [--output DIR] [--chapters N-M] [--json]` | Reads the book aloud into one audio file with chapter markers, or with `--split` a folder named after the book with one file per chapter; prints the path. Uses the window's engine and voice settings unless overridden. Takes a while: every sentence is synthesised. Needs ffmpeg |
| `omareader fetch <url> [--json]` | A web page's article as the window would add it: title, author, the address it ended up at, then the text as Markdown. Uses the network for that one page; adds nothing to the library |
| `omareader voices [--engine kokoro\|qwen] [--json]` | The engine's voices, languages and tuning keys with their ranges and current values, for `export` |
| `omareader doctor [--engine kokoro\|qwen] [--json]` | Whether read-aloud is ready: Kokoro's binary and model, Qwen3-TTS's environment and models, and whether the GPU has room. Exits 1 when the chosen engine is not ready |
| `omareader setup [--engine kokoro\|qwen] [--model 0.6b\|1.7b] [--dry-run]` | Installs an engine and its models, pinned and checksummed (uses the network). Kokoro is the default; Qwen3-TTS takes about 14 GB and wants an NVIDIA GPU |
| `omareader setup --dictionary <from-to> [--dry-run]` | Installs one of FreeDict's bilingual dictionaries, such as `eng-por` (two-letter codes like `en-pt` work too), pinned and checksummed (uses the network) |
| `omareader dictionaries [--available] [--json]` | The installed dictionaries (StarDict and dictd, found in omareader's folder, `~/.stardict/dic`, `/usr/share/stardict/dic` and `/usr/share/dictd`), or with `--available` every one setup can install |
| `omareader lookup <word> [--json]` | What each installed dictionary says about the word, offline. A dictionary without the word as written is asked for its base form ("running" finds "run"); the headword shows which. Exits 1 when none has it |
| `omareader open [book]` | Opens the window |

Every command exits 0 on success and 1 on failure, with the reason on stderr.
With `--json` the result is a single JSON object on stdout.

The text is what the narrator speaks: formatting, images, and footnote markers
are dropped, and headings come out as their own paragraphs. omareader never
modifies books; its library lives in `~/.local/share/omareader/library.json`,
and text pasted into it, or web pages added to it, in
`~/.local/share/omareader/texts/*.json`, which `info` and `text` read like any
book; `info` shows a web page's address as its source.
