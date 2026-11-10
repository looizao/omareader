# Sync through Dropbox or Google Drive

omareader on the desktop and on Android can keep each other up to date
through the user's own Dropbox or Google Drive: books exported for the phone arrive on it by
themselves, the place reached in a book follows you from one device to the
other, removing a book removes it everywhere, and the listening speed and
text size carry over. There is no omareader server. Each device talks to
the provider directly, and only to omareader's own files there.

The two providers hold the same files and run the same merge; this document
describes Dropbox and then, in [Google Drive](#google-drive), how Drive
stands in for each Dropbox call. A device syncs through one provider at a
time, and the user connects the same one on every device.

Sync is off until the user connects a provider in the settings. It is the only
part of omareader that touches the network besides `omareader setup` and the
optional title agent; reading and narration stay offline.

## Dropbox app

One Dropbox app serves both platforms:

- Access type **App folder**, so the token reaches `Apps/omareader/` and
  nothing else in the user's Dropbox.
- Permissions: `files.metadata.read`, `files.content.read`,
  `files.content.write` (and the default `account_info.read`, to show who is
  connected).
- **Allow public clients** on, for PKCE without a client secret.
- Redirect URIs `http://localhost:53682/dropbox`,
  `http://localhost:53683/dropbox` and `http://localhost:53684/dropbox`.
  Dropbox only allows plain HTTP for localhost and matches the URI exactly,
  so these three ports are fixed; a device uses the first one it can listen
  on.

The app key is public (PKCE needs no secret) and lives in `DROPBOX_APP_KEY`
at the top of the repository, which both builds read. When it is empty,
the build has no sync.

## Connecting

OAuth 2 with PKCE (S256), `token_access_type=offline`:

1. The device makes a random verifier (64 characters of `[A-Za-z0-9-._~]`)
   and a random `state`, starts listening on `127.0.0.1` at one of the three
   ports, and opens
   `https://www.dropbox.com/oauth2/authorize?client_id=…&response_type=code&code_challenge=…&code_challenge_method=S256&token_access_type=offline&redirect_uri=…&state=…`
   in the system browser (never a web view).
2. Dropbox sends the browser to the redirect URI with `code` and `state`.
   The listener checks `state`, answers with a short page saying the device
   is connected, and stops listening. It accepts one request and gives up
   after five minutes.
3. The device exchanges the code at `https://api.dropboxapi.com/oauth2/token`
   (`grant_type=authorization_code`, `code`, `code_verifier`, `client_id`,
   `redirect_uri`) for a refresh token and a short-lived access token.

The refresh token is the only credential kept: the desktop stores it in the
Secret Service keyring (gnome-keyring on Omarchy), the phone encrypts it
with a key in the Android Keystore. Access tokens are refreshed when they
expire (`grant_type=refresh_token`) and are never written down.
Disconnecting revokes the token (`/2/auth/token/revoke`) and forgets it, and
leaves the files in Dropbox alone.

## Files in the app folder

| Path | What it holds | Written by |
| --- | --- | --- |
| `/books/<id>.omabook` | A book exported for the phone ([omabook.md](omabook.md)) | the desktop |
| `/state/<id>.json` | Whether the book is in the library, and where it was left | every device |
| `/settings.json` | Settings that follow the user | every device |

`<id>` is the book id from the bundle manifest: the first 16 hex digits of
the SHA-1 of the title and author, so the same book has the same id on every
device whichever file or export it came from. Any other file in the folder
is left alone.

## Registers

Everything synced is a **register**: a value with the time it was set (`at`,
milliseconds since 1970 UTC, from the device's clock) and the device that
set it (`by`, a random 16-hex-digit id each install makes once). Merging two
copies of a register keeps the one with the larger `at`; on a tie, the larger
`by` (compared as strings); on a tie of both, they are the same change. The
most recent change wins, as the user chose; there is nothing to resolve by
hand.

A device keeps `at` and `by` with its own copy of each value. Setting a value
locally stamps it with the current time and the device's id. Taking a value
from Dropbox keeps the `at` and `by` it came with, so the same change never
bounces back as a newer one.

## state/&lt;id&gt;.json

```json
{
  "format": "omareader-state",
  "version": 1,
  "id": "25e69d67a7541dbd",
  "title": "Taking Charge of Adult ADHD",
  "author": "Russell A. Barkley",
  "present": {"value": true, "at": 1762690000000, "by": "4f1c2a9e0b7d3c55"},
  "position": {
    "chapter": 4, "paragraph": 12, "offset": 0, "progress": 0.31,
    "at": 1762693512000, "by": "a03be71c9d2e4f60"
  }
}
```

- `present` says whether the book is in the user's library. A device sets
  it to true when the book is added (opened on the desktop, exported, or
  imported on the phone) and to false when the user removes it. A device
  that receives false removes its copy: the desktop forgets the book (its
  file stays where it is; pasted text goes to the trash), the phone deletes
  its imported copy. Any device that sees false while
  `/books/<id>.omabook` still exists deletes that bundle.
- `position` is where reading stopped, in the desktop book's own terms, so
  it means the same on every device: `chapter` is the chapter's index in the
  book (counting chapters that have nothing to read aloud, unlike the
  bundle's chapter list), `paragraph` the paragraph's index in that chapter,
  `offset` the UTF-16 offset of the word in that paragraph. `progress` (0 to
  1) is how far through the book that is, for showing on a device that
  doesn't have the book open. The desktop saves the start of the sentence
  being read; the phone the start of the word being heard.
- Either register may be missing. A reader refuses a `version` newer than it
  knows and leaves the file alone.

To turn a position into a place in its bundle, the phone needs each bundle
chapter's index in the book: bundle manifests carry it as `chapter`
([omabook.md](omabook.md)). In a bundle exported before that field, the
phone syncs the book's presence but not its position.

## settings.json

```json
{
  "format": "omareader-settings",
  "version": 1,
  "speed": {"value": 1.25, "at": 1762693512000, "by": "a03be71c9d2e4f60"},
  "textSize.desktop": {"value": 22, "at": 1762600000000, "by": "4f1c2a9e0b7d3c55"},
  "textSize.phone": {"value": 19, "at": 1762650000000, "by": "a03be71c9d2e4f60"}
}
```

- `speed` is how fast the user listens, relative to the voice's natural
  pace (0.5 to 2.0). On the desktop it is the narration speed. On the phone
  it is what the speed button shows, and the player runs each book at
  `speed / voice.speed` from its manifest, so a book exported at 1.25 and
  heard at 1.25 plays its audio as it is.
- Text size is per kind of device, since a phone and a monitor want
  different sizes: `textSize.desktop` is the desktop reader's font size in
  pixels (12 to 48), `textSize.phone` the phone reader's in sp (one of the
  sizes it offers).
- Keys a device doesn't know are kept as they are.

## A sync, step by step

A device syncs when it starts, when it comes back to the foreground, a few
seconds after the user changes something that syncs, and every 15 minutes
while it is running (the phone through a periodic job). One sync:

1. **Upload.** The desktop sends exports waiting to go (step 6 of an
   export). A bundle up to 150 MiB goes in one `/2/files/upload`; a larger
   one in an upload session, in 64 MiB pieces. Both write with
   `mode: overwrite`. Once it is up, the book's `present` is set to true.
2. **List.** `/2/files/list_folder` on `""`, recursive, every page. The
   device keeps the `rev` of each state file and bundle it last read.
3. **Merge each book.** For every id that has a state file or a local
   record: download the state file if its `rev` changed since last time,
   merge each register with the local one, and apply the registers Dropbox
   won to the local library. If the merged record differs from the one in
   Dropbox, upload it with `mode: {".tag": "update", "update": <rev>}` and
   `strict_conflict: true` (or `mode: add` for a new file). A 409
   `path/conflict` means another device wrote in between: download again,
   merge again, and retry, up to three times.
4. **Merge the settings** the same way, in `/settings.json`.
5. **Remove.** Delete `/books/<id>.omabook` for each id whose `present` is
   false. A 409 `path_lookup/not_found` means someone already did.
6. **Download** (the phone). For each bundle whose `present` is true and
   whose `rev` differs from the copy on the phone, download it, check its
   `content_hash` against Dropbox's (SHA-256 of each 4 MiB block, then
   SHA-256 of those digests, in hex), and import it as if the user had
   opened it. A download resumes where it stopped (`Range`). Bundles only
   download on an unmetered network unless the user allows mobile data.

The position taken from Dropbox moves the reader only while it is not
reading aloud; while it is, the device's own position is newer anyway.

Failures leave everything as it was and are retried at the next sync:
401 refreshes the access token once, 429 and 5xx wait for `Retry-After` (or
a minute), and a revoked token disconnects with a message asking the user to
connect again.

## How each device fits in

The desktop:

- Keeps its sync records in `~/.local/share/omareader/sync.json` (mode
  0600) and the refresh token in the Secret Service keyring.
- Leaves a book's state alone when the file in Dropbox has a newer
  `version` or can't be read, so an older app never undoes a newer one.
- Sends a phone export when **Send books exported for the phone** is on
  (the default once connected). `omareader sync send <bundle>` sends one
  by hand.

The phone:

- Seals the refresh token with an AES key in the Android Keystore; Android
  backups are off, so it never leaves the phone.
- Answers 404 to any other request on the loopback port (browsers ask for a
  favicon) and keeps waiting for `/dropbox`. The page it returns links back
  to the app through `omareader://sync`.
- Runs its 15-minute job only while the app has been opened since boot; it
  holds no boot permission.
- Counts a book it had before sync as added at its folder's modification
  time, and its saved place as set when it was last opened.
- Keeps a book's files while that book is playing, even once its removal is
  recorded, and deletes them when playback stops. A place that arrives for
  the playing book is not applied; the phone's own, newer place wins the
  next merge.
- Applies a place for a book it doesn't have yet once the bundle arrives.
- Converts places only for bundles with per-chapter `chapter` indices
  (exports from this version on). Older bundles sync their presence only.
- Snaps `textSize.phone` to the nearest of its sizes (15, 17, 19, 22, 26
  sp), and keeps `speed` within 0.5 to 2, the desktop's range.
- Forgets its saved `rev`s on disconnect, so the next connection does a full
  merge.

## Google Drive

Drive has no app folder like Dropbox's, no conditional write, and allows two
files with the same name in a folder, so the Drive client does a little
more to give the engine the same calls.

### Google Cloud project

One project serves both platforms, with the **Google Drive API** enabled
and an OAuth consent screen that asks for one scope,
`https://www.googleapis.com/auth/drive.file`. That scope is non-sensitive:
it reaches only files omareader created, and nothing else in the user's
Drive. The project has two OAuth clients:

- A **Desktop app** client. Its id goes in `GOOGLE_CLIENT_ID` at the top of
  the repository. Google calls its secret non-confidential, but GitHub's
  secret scanning blocks it, so it stays out of git: the build reads it from
  `GOOGLE_CLIENT_SECRET` (ignored by git) or the `OMAREADER_GOOGLE_SECRET`
  environment variable, and CI writes it from a repository secret. A build
  without both has no Drive.
- An **Android** client for `io.github.looizao.omareader`, with the SHA-1 of
  every key that signs the app (the debug key and Play's app signing key).
  It has no secret; Play services checks the app's signature instead.

While the consent screen's publishing status is **Testing**, only its test
users can connect and Google expires their sign-in after 7 days. Publishing
it needs Google's basic app verification (a homepage and the privacy
policy), not a security assessment.

### Connecting to Drive

The desktop uses Google's loopback flow for desktop apps, like Dropbox's:
PKCE (S256), a random `state`, a listener on `127.0.0.1` at any free port,
and `https://accounts.google.com/o/oauth2/v2/auth?client_id=…&redirect_uri=http://127.0.0.1:<port>/drive&response_type=code&scope=https://www.googleapis.com/auth/drive.file&code_challenge=…&code_challenge_method=S256&access_type=offline&prompt=consent&state=…`.
The code goes to `https://oauth2.googleapis.com/token` with the client id,
the secret and the verifier. The refresh token is kept as Dropbox's is (the
keyring, service `drive`), and disconnecting posts it to
`https://oauth2.googleapis.com/revoke`.

Google doesn't allow that flow on Android, so the phone asks Play services
(`Identity.getAuthorizationClient(…).authorize(…)` with the `drive.file`
scope). The user agrees once in Google's own sheet; after that the phone
asks again before each sync and gets a fresh access token without showing
anything. It keeps no refresh token. If Play services needs the user again
(the grant was revoked), a background sync stops and the sync screen asks
the user to connect again. Drive needs Google Play services on the phone.

The phone keeps the account's address (in the app's private preferences,
which Android doesn't back up) so it asks Play services for the same
account each time and revokes that account's grant when the user
disconnects. On a phone without Play services the Drive button says so.

The account shown in the settings comes from
`GET /drive/v3/about?fields=user(displayName,emailAddress)`.

### Files in Drive

Everything lives in a folder named **omareader** at the top of the user's
Drive, so the user can see it. Each file omareader makes carries public
file properties (`properties`, which every client of the project can read):

| Property | Value |
| --- | --- |
| `omareader` | `folder` on the folder, `file` on everything in it |
| `omareaderPath` | the Dropbox-style path: `/books/<id>.omabook`, `/state/<id>.json`, `/settings.json` |

The path, not the name, says what a file is. Names are for people: a bundle
is named after its book (`Dune.omabook`), a state file `<id>.json`, and the
settings `settings.json`.

- **Find the folder.** The files with `omareader=folder`, not trashed. If
  there are several (two devices made one at the same moment), the one
  created first, then the smaller id, is the folder, and new files go there.
  If there is none, make it (`mimeType`
  `application/vnd.google-apps.folder`, in `root`).
- **List.** `GET /drive/v3/files` with
  `q=properties has {key='omareader' and value='file'} and trashed=false`,
  `spaces=drive`, `pageSize=1000`, and
  `fields=nextPageToken,files(id,name,properties,headRevisionId,sha256Checksum,size,createdTime,modifiedTime)`,
  every page. This sees files in every omareader folder, so a second folder
  loses nothing. A file's `rev` is its `headRevisionId`, which changes with
  every new content. Drive gives none for a file without content of its
  own; then the `rev` is `m:` and its `modifiedTime`.
- **Two files for one path.** Keep one: for a state file or the settings,
  the one created first (then the smaller id); for a bundle, the one
  modified last (then the larger id). Trash the others. Nothing is lost:
  the device that wrote a trashed copy still has its registers, sees that
  the kept file isn't the one it wrote, merges and writes again at its next
  sync.
- **Read.** `GET /drive/v3/files/<fileId>?alt=media`; `Range` resumes a
  download. The `rev` is the one the listing gave.
- **Add** (Dropbox's `mode: add`): if the listing has a file for the path,
  that is a conflict. The phone also asks Drive for the path
  (`properties has {key='omareaderPath' and value='<path>'}`), which
  narrows the window in which two devices both add the file. Otherwise create it with a multipart upload
  (`POST /upload/drive/v3/files?uploadType=multipart`, metadata
  `{name, parents: [folder], properties}`).
- **Update** (Dropbox's update with `strict_conflict`): fetch the file's
  `headRevisionId`; if it isn't the expected `rev`, or the file is gone,
  that is a conflict. Otherwise
  `PATCH /upload/drive/v3/files/<fileId>?uploadType=media`. Drive can't make
  the write itself conditional, so a write from another device can land in
  between and be overwritten. That loses nothing for long: the other device
  still has its registers, finds the `rev` it wrote gone at its next sync,
  and merges again.
- **Bundles** (Dropbox's `overwrite`) go up in a resumable upload: `POST`
  (a new file) or `PATCH` (the file already listed for that path)
  `/upload/drive/v3/files?uploadType=resumable` with the metadata, then the
  bytes to the session URI from `Location` in 8 MiB pieces
  (`Content-Range: bytes a-b/total`). Drive answers 308 until the last
  piece. After a dropped connection, `PUT` with `Content-Range: */total`
  says how much arrived (`Range`); a 404 means the session expired, and the
  upload starts over, once. A replaced bundle is renamed after its book's
  title in the same request, so a renamed book shows its new name.
- **Delete** moves the file to the trash (`PATCH {"trashed": true}`), so a
  user can get it back for 30 days. A file already gone is fine.
- **Check a download** against `sha256Checksum`, the plain SHA-256 of the
  whole file (not Dropbox's block hash). If Drive gives none, check the size
  alone.

### Errors from Drive

- 401 refreshes the access token once (the desktop from the refresh token,
  the phone from Play services).
- 403 with reason `rateLimitExceeded` or `userRateLimitExceeded`, 429, and
  500, 502, 503 and 504 back off and retry: 1, 2, then 4 seconds plus up to a
  second at random. If the request still fails, the sync stops and the next
  one tries again (on the phone, after `Retry-After`, or a minute).
- On the phone, a second 401 right after Play services gave a new token,
  or Play services asking for the user, means the same.
- `invalid_grant` when refreshing means the sign-in is gone (revoked, unused
  for six months, or a Testing project's 7 days are up): disconnect and ask
  the user to connect again, as for Dropbox.

## Arguments with non-ASCII text

`Dropbox-API-Arg` is a header, so the JSON in it escapes 0x7F and every
non-ASCII character as `\uXXXX` (UTF-16 code units, so characters beyond
the BMP become a surrogate pair).
