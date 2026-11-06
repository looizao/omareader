# The .omabook phone bundle

omareader on the desktop exports a book for the Android app as one
`.omabook` file: the voice's audio, the text, and the time of every word, so
the phone plays the audio and highlights each word as it is heard without
synthesising anything itself. You move the file yourself (Google Drive,
Dropbox, a cable), and the app imports it.

## Container

A ZIP archive whose entries are all **stored** (no compression): the audio is
already compressed, and stored entries need no zlib on either side. No ZIP64,
so a bundle stays under 4 GiB, which is several days of speech. Entries, in
this order:

| Entry | What it holds |
| --- | --- |
| `manifest.json` | The book and its chapters (first, so a reader can identify the file early) |
| `cover.jpg` | The cover, at most 1400 px, when the book has one |
| `audio/NNN.m4a` | Chapter N's audio: AAC-LC, mono, 24 kHz, about 64 kbit/s, in MP4 |
| `text/NNN.json` | Chapter N's text and word timings |

`NNN` counts from `001` and is wider when a book has more than 999 chapters.
Only chapters with something to read aloud are included, so a cover page or
an empty section has no entry. A reader rejects any other entry name.

## manifest.json

```json
{
  "format": "omabook",
  "version": 1,
  "id": "25e69d67a7541dbd",
  "title": "Taking Charge of Adult ADHD",
  "author": "Russell A. Barkley",
  "created": "2026-11-06T00:59:14Z",
  "voice": {"engine": "kokoro", "voice": "af_heart", "language": "", "speed": 1.0},
  "cover": "cover.jpg",
  "chapters": [
    {"title": "Acknowledgments", "audio": "audio/001.m4a", "text": "text/001.json", "duration": 101775}
  ]
}
```

- `id` is derived from the title and author, so exporting the same book
  again, with another voice or speed, replaces it on the phone and keeps the
  place reached.
- `duration` is in milliseconds.
- `cover` is absent when there is none.
- A reader refuses a `version` newer than it knows.

## text/NNN.json

```json
{
  "title": "Author’s Note",
  "paragraphs": [{"text": "Author’s Note", "level": 1}, {"text": "In this book…", "level": 0}],
  "words": [0, 0, 8, 0, 562,   0, 9, 4, 562, 1441,   1, 0, 2, 2675, 2852],
  "sentences": [0, 2]
}
```

- `paragraphs` is every paragraph of the chapter as the desktop shows it;
  `level` is 1 to 6 for headings and 0 for body text.
- `words` is flat, five integers per spoken word in reading order:
  paragraph index, offset of the word in that paragraph's text (in UTF-16
  code units, as both Qt and Java count), its length, and when it starts and
  ends in the chapter's audio, in milliseconds. Start times never decrease.
- `sentences` holds the index (in words, not integers) of each sentence's
  first word, for moving by sentence.

The times are those the desktop highlights with: the engine's word
timestamps aligned onto the text, with the speed, the pauses after
paragraphs and headings, and each sentence's place in the chapter applied.
