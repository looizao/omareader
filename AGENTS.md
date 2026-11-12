# omareader for agents

omareader is an offline ebook reader for Omarchy (C++17, Qt 6 Quick, qmake)
that reads books aloud with Kokoros and highlights each word as it's spoken.
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
   - When the change touches the window, test what is drawn, not just the
     logic. `AppWindow` in `tests/omareader_tests.cpp` loads the real
     `qrc:/Main.qml` offscreen with everything `main()` gives it; add a test
     that grabs it and checks the pixels (see
     `libraryRemoveButtonDrawsItsCrossCentred`, which measures where the
     cross is drawn inside its circle). Then look at it yourself:
     `OMAREADER_SCREENSHOTS=<folder> bin/test <test>` saves each shot.
     Crop and enlarge small controls (icons, badges, buttons) before
     judging them; at full size an off-centre glyph looks fine. When it touches
     narration, run it against the real `koko` with the audio sink muted
     (`wpctl set-mute @DEFAULT_AUDIO_SINK@ 1`, unmuted afterwards).
   - For changes to how books are parsed, check `omareader info` and
     `omareader text` against real books, such as the ones in
     `~/Documents/books`.
2. **Commit it** with a short imperative message ("Skip code listings in
   PDFs").
3. **Release it to main.** Every push to main is a release, so ship with
   `bin/release patch` for a fix or small change, or `bin/release minor`
   for a new feature (see Releases). Then wait for the full CI run on
   GitHub (`.github/workflows/ci.yml`) to finish green: every job,
   including the release, not just the first. If it fails, fix
   it, release again, and wait again.
4. **Install it locally** so the user can test it: run `bin/install`. It
   needs no sudo: it builds and copies the binary to `~/.local/bin` and the
   icon and launcher entry under `~/.local/share`. If it reports missing
   dependencies, give the user the `sudo pacman -S` command it prints rather
   than running it. Check that `omareader version` resolves to
   `~/.local/bin/omareader`.

Then tell the user what changed, how it was tested, and what to try.

## Releases

The version lives in `VERSION` (and `pkgbuild/PKGBUILD`), and each push to
main publishes it. Rebase onto `origin/main` first; `bin/release
patch|minor` then bumps both files from main's version, commits "Release
v<x.y.z>", and pushes `HEAD` to main, from main or any branch. CI then builds and tests,
tags `v<x.y.z>`, and publishes the GitHub release with the Arch package. A
push to main without a bump fails the release job.

You choose the bump: patch for fixes, polish, docs, and other small
changes; minor for anything a user would call a new feature. Leave major
alone unless the user asks. Commits pushed together share one release, so
release once per push, not once per commit.

## Conventions

- Follow the Omarchy app conventions: C++ owns logic, QML owns layout and
  keys, every visible string goes through `qsTr()`/`tr()` with a pt-BR entry
  in `src/i18n/pt-BR.json`, and the theme accent and `theme.radius` style
  everything.
- The app touches the network only when the user asks. Kokoros downloads a
  missing model on its own, so synthesis only starts once both model files
  exist (`KokoroSetup`). `omareader setup` downloads the engine. Adding a
  web page (`WebImport`) fetches that one page when the user presses Fetch,
  loads nothing it links to, runs none of its scripts and keeps no cookies;
  its text is then kept and never fetched again. The title
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
- Material's `Button` lays its `contentItem` out with paddings of its own,
  even with `padding: 0`, so a drawn icon there ends up off centre. Draw
  icons on the `background` instead, as the library's remove button does.
- Tests point `HOME` and `XDG_*` at temp directories so they never see the
  real library or the installed model; keep it that way.
