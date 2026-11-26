---
name: omareader
description: Read ebooks (EPUB, PDF, FB2, DOCX, ODT, Markdown, HTML, TXT, and Kindle formats through Calibre) as plain text from the command line with omareader, check its offline read-aloud setup (Kokoro, and optionally Qwen3-TTS), and look words up or translate them in offline dictionaries. Use when asked to extract a book's text or chapters, inspect a book's metadata, list the bookmarks, highlights and notes made in a book, export a book as an MP3 or M4B audiobook, look up or translate a word, teach the voice how to say a name or term, or troubleshoot omareader's voice.
---

# omareader

omareader is an offline ebook reader for Omarchy that reads books aloud with
Kokoros or, optionally, Qwen3-TTS on an NVIDIA GPU, and highlights each word
as it's spoken. Its commands need no display.

| Command | What it does |
| --- | --- |
| `omareader info <book> [--json]` | Title, author, format, chapters with paragraph, sentence and word counts |
| `omareader text <book> [--chapter N] [--json]` | The book's text as omareader shows it, one paragraph per line, chapters numbered from 1; PDF footnotes are marked `"note": true` in JSON |
| `omareader text <book> --spoken [--dialogue] [--chapter N] [--json]` | The text as the voice reads it, one sentence per line: no footnotes, citation markers, web addresses or cross-references, and the reader's pronunciations respelled. With `--dialogue`, what a second voice reads as the characters' speech is marked «like this» (in JSON, each sentence's `pieces` with `dialogue` true or false) |
| `omareader rsvp <book> [--chapter N] [--wpm N] [--size N] [--json]` | The book as RSVP shows it, one flash (a word, or up to `--size` words) per line: how many milliseconds it stays up at `--wpm` words a minute, then its text with the recognition point in brackets (`240  he[l]lo,`). The first five ease in from half the rate. In JSON, each flash's `text`, `recognition` (a character index into it), `ms`, `chapter` and `sentence`. Rate and size default to the window's |
| `omareader pronounce [<written> <spoken>] [--remove <written>] [--json]` | Lists how names and terms are said when reading aloud and exporting, or teaches one ("Hermione" "Her-my-oh-nee") or forgets one. Matches whole words in any case; an entry in capitals matches only in capitals. Kept in `~/.local/share/omareader/omareader/pronunciations.json` |
| `omareader notes <book> [--json]` | The bookmarks, highlights and notes made in the book in the window, in reading order with their chapters, including those synced from other computers. Reads only |
| `omareader export <book> [--split] [--format mp3\|m4b] [--engine kokoro\|qwen] [--voice ID] [--language L] [--speed X] [--dialogue-voice ID\|off] [--tune key=value]... [--output DIR] [--chapters N-M] [--json]` | Reads the book aloud into one audio file with chapter markers, or with `--split` a folder named after the book with one file per chapter; prints the path. Uses the window's engine and voice settings unless overridden; `--dialogue-voice` reads the characters' speech (in quotation marks or after a dialogue dash) in another of the engine's voices. Takes a while: every sentence is synthesised. Needs ffmpeg |
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
modifies books; its library lives in `~/.local/share/omareader/omareader/library.json`,
and text pasted into it, or web pages added to it, in
`~/.local/share/omareader/omareader/texts/*.json`, which `info` and `text` read like any
book; `info` shows a web page's address as its source.
