# omareader for agents

omareader is an offline ebook reader for Omarchy (C++17, Qt 6 Quick, qmake)
that reads books aloud with Kokoros and highlights each word as it's spoken.
`android/` is its Android companion (Kotlin, platform views, Media3), which
plays the `.omabook` bundles the desktop exports (`docs/omabook.md`).
`README.md` covers what it does and how the pieces fit together.

## How every change ships

Every new feature, bugfix, or other change goes through all of these steps,
in order, before it counts as done:

1. **Test it locally.**
   - Add or update tests in `tests/omareader_tests.cpp` for the behavior you
     changed: a failing test first for a bugfix, and new cases for a feature.
   - Run `bin/build` and `bin/test`; both must pass with no failures.
   - Local runs are the quick check. The full CI (shell lint, the Arch
     container build and tests, the package, the release) runs on GitHub,
     and only a green GitHub run counts. Don't stand in for it by running
     the workflow locally (act, podman, docker).
   - When the change touches the window, look at it: drive the real QML
     offscreen (`QT_QPA_PLATFORM=offscreen`, load `qrc:/Main.qml`, save
     `window->grabWindow()`) and check the screenshot. When it touches
     narration, run it against the real `koko` with the audio sink muted
     (`wpctl set-mute @DEFAULT_AUDIO_SINK@ 1`, unmuted afterwards).
   - For changes to how books are parsed, check `omareader info` and
     `omareader text` against real books, such as the ones in
     `~/Documents/books`.
   - For the Android app, add tests in `android/app/src/test` and run
     `bin/android test` (unit tests and lint). When it touches a screen or
     playback, run it on the emulator (`~/Android/Sdk/emulator/emulator -avd
     <name> -no-window`, `bin/android install`, `adb exec-out screencap -p`)
     with a real export, and check the screenshots.
   - A change to the bundle format touches both sides: update
     `docs/omabook.md`, and regenerate `android/app/src/test/resources/
     fixture.omabook` with the real exporter so the contract test stays
     honest.
2. **Commit it** with a short imperative message ("Skip code listings in
   PDFs").
3. **Push it to main** and wait for the full CI run on GitHub
   (`.github/workflows/ci.yml`) to finish green: every job, not just the
   first. Fix anything it reports, push again, and wait again.
4. **Install it locally** so the user can test it: run `bin/install`. It
   needs no sudo: it builds and copies the binary to `~/.local/bin` and the
   icon and launcher entry under `~/.local/share`. If it reports missing
   dependencies, give the user the `sudo pacman -S` command it prints rather
   than running it. Check that `omareader version` resolves to
   `~/.local/bin/omareader`. For Android changes, also run
   `bin/android install` when `adb devices` lists the user's phone.

Then tell the user what changed, how it was tested, and what to try.

## Releases

`bin/release <x.y.z>` bumps `VERSION` and `pkgbuild/PKGBUILD` and pushes;
CI tags `v<x.y.z>` and publishes the GitHub release. Only release when the
user asks.

## Conventions

- Follow the Omarchy app conventions: C++ owns logic, QML owns layout and
  keys, every visible string goes through `qsTr()`/`tr()` with a pt-BR entry
  in `src/i18n/pt-BR.json`, and the theme accent and `theme.radius` style
  everything.
- The Android app keeps the same rules: no network permission, every visible
  string in `res/values/strings.xml` with a pt-BR entry in
  `res/values-pt-rBR`, and no libraries beyond Media3 without a reason.
- The app never touches the network. Kokoros downloads a missing model on
  its own, so synthesis only starts once both model files exist
  (`KokoroSetup`). `omareader setup` is the one networked step. The title
  agent for pasted text (`TitleAgent`) runs a coding agent that may use the
  network, so it stays off until the user turns it on in the dialog.
- Never modify the user's books; save app data atomically (`QSaveFile`).
- Comments explain why, in full sentences. No em dashes in prose.

## Traps found so far

- moc fails silently on XML raw-string literals in a file with `Q_OBJECT`;
  keep fixtures in `tests/fixtures.cpp`.
- Don't name QML files after Qt Quick types: `Palette.qml` broke every
  colour, hence `Colors.qml`.
- The offscreen renderer drops Material's elevated button backgrounds; draw
  prominent buttons yourself (`AccentButton.qml`).
- Tests point `HOME` and `XDG_*` at temp directories so they never see the
  real library or the installed model; keep it that way.
