# Google Play internal testing

omareader for Android reaches beta testers through Play's internal testing
track: up to 100 people, invited by email, who install it from the Play Store
and get updates like any other app. Internal builds skip Play's review, so a
build is in testers' hands minutes after the upload.

## How a build gets there

- **CI.** The `play` job in `.github/workflows/ci.yml` runs when CI
  publishes a new release, and when the workflow is run by hand on main with
  **play** ticked (`gh workflow run ci.yml -f play=true`). It builds the
  bundle, signs it with the upload key, keeps it as the `android-bundle`
  artifact, and sends it to internal testing with `bin/android publish`.
- **Version codes.** Every upload needs a higher version code than the last,
  so the code is the number of commits on main (`android/app/build.gradle.kts`);
  the version testers see is still `VERSION`. Each run of the job is a new
  code, but two runs on the same commit would collide, and Play refuses the
  second.
- **Signing.** Play App Signing: Google keeps the key that signs what testers
  install, and we sign uploads with an upload key. If the upload key is lost
  or leaks, Play can replace it (Play Console, Test and release, App
  integrity), so nothing is locked out for good.

| Where | What |
|---|---|
| `~/.config/omareader/android-upload/upload.p12` | the upload key (PKCS12, alias `upload`), on the machine that made it |
| `~/.config/omareader/android-upload/keystore.properties` | its password; link it as `android/keystore.properties` to sign locally |
| GitHub environment `play`, secret `UPLOAD_KEYSTORE` | the key, base64 |
| GitHub environment `play`, secret `UPLOAD_KEYSTORE_PASSWORD` | its password |
| GitHub environment `play`, variables `PLAY_WIF_PROVIDER` and `PLAY_SERVICE_ACCOUNT` | how CI signs in to Google (below) |

Only main can use the `play` environment, so a pull request never sees the
key. Keep a copy of the upload key and its password in your password
manager.

## Setting it up, once

### 1. A Play developer account

Sign up at <https://play.google.com/console> as a personal account. It costs
US$25 once; Google checks your identity and asks you to confirm you have an
Android phone through the Play Console app, which can take a few days.

### 2. The app

In Play Console, **Create app**: name *omareader*, default language English
(United States), App, Free.

### 3. The first build, by hand

Play takes the first bundle of a new app only through the console.

1. Run CI on main with play ticked: `gh workflow run ci.yml -f play=true`.
   Without the Google sign-in from step 5 it stops after building, which is
   all this needs. Download its `android-bundle` artifact
   (`gh run download <run id> -n android-bundle`).
2. Test and release, Testing, **Internal testing**, **Testers**: create an
   email list with your testers' Google accounts, and save.
3. **Create new release**. When Play asks about app signing, keep the
   default, a key Google creates and keeps. Upload `app-release.aab`, then
   **Next** and **Save and publish**.

If Play asks for app content before it will roll out, the answers are:

- Privacy policy: <https://github.com/looizao/omareader/blob/main/docs/privacy.md>
- Ads: none
- Data safety: review this before the next release. Until Dropbox sync the
  answer was "no data collected or shared". Sync now sends, at the user's
  request and to the user's own Dropbox: books, reading positions and two
  settings, all over HTTPS. Check Play's current definitions of "collected"
  and "shared" against that, and keep the answer in step with
  [privacy.md](privacy.md)
- App access: everything works without an account
- Content rating questionnaire: a reference app with none of the listed content
- Target audience: 13 and over

### 4. Invite the testers

On the Testers tab, copy the **Join on the web** link and send it to them.
Each tester opens it signed in to the Google account on the list, accepts,
and installs omareader from the Play Store. A first install can take a few
minutes to appear.

### 5. Let CI send new builds

CI signs in to Google with workload identity federation: GitHub vouches for
the run, and Google lets that run, and nothing else, act as a service
account that may only release to testing tracks. No Google key exists to
leak.

Use a personal Google Cloud project and account for this, not a work one
(`gcloud config configurations create personal`, then `gcloud auth login`).
Billing is not needed.

```sh
PROJECT=omareader-play          # any free project ID
REPO=looizao/omareader
REPO_ID=1445076282              # gh api repos/looizao/omareader -q .id

gcloud projects create "$PROJECT"
gcloud config set project "$PROJECT"
gcloud services enable androidpublisher.googleapis.com iamcredentials.googleapis.com sts.googleapis.com
gcloud iam service-accounts create play-publisher --display-name "omareader CI"
gcloud iam workload-identity-pools create github --location global
# Only runs of this repository in the play environment, which only main can
# use, may sign in.
gcloud iam workload-identity-pools providers create-oidc omareader \
  --location global --workload-identity-pool github \
  --issuer-uri https://token.actions.githubusercontent.com \
  --attribute-mapping "google.subject=assertion.sub,attribute.repository_id=assertion.repository_id,attribute.environment=assertion.environment" \
  --attribute-condition "assertion.repository_id == '$REPO_ID' && assertion.environment == 'play'"

NUMBER="$(gcloud projects describe "$PROJECT" --format 'value(projectNumber)')"
POOL="projects/$NUMBER/locations/global/workloadIdentityPools/github"
gcloud iam service-accounts add-iam-policy-binding \
  "play-publisher@$PROJECT.iam.gserviceaccount.com" \
  --role roles/iam.workloadIdentityUser \
  --member "principalSet://iam.googleapis.com/$POOL/attribute.repository_id/$REPO_ID"

gh variable set PLAY_WIF_PROVIDER --env play --repo "$REPO" --body "$POOL/providers/omareader"
gh variable set PLAY_SERVICE_ACCOUNT --env play --repo "$REPO" \
  --body "play-publisher@$PROJECT.iam.gserviceaccount.com"
```

Then let the service account release: Play Console, **Users and
permissions**, **Invite new users**, the service account's email, **App
permissions**, add omareader with **View app information** and **Release
apps to testing tracks**, and nothing else.

Try it with `gh workflow run ci.yml -f play=true`: the run ends with "Sent
x.y.z (n) to internal testing", and testers get the update.

## In the car

Testers who install from internal testing see omareader in Android Auto
straight away: Android Auto trusts apps from Play, and internal testing
skips review. A build installed with `bin/android install` is not from Play,
so Android Auto hides it until its developer settings allow unknown sources
(Android Auto settings, tap the version ten times, the menu, Developer
settings, Unknown sources).

## When it goes wrong

`bin/android publish` prints Play's own explanation and leaves nothing half
done on Play. The usual ones:

- *Only releases with status draft may be created on draft app*: the first
  internal release has not been rolled out from the console yet (step 3).
- *APK specifies a version code that has already been used*: that commit
  was already sent; push a new one.
- *The caller does not have permission*: the service account is not invited
  in Play Console, or lacks **Release apps to testing tracks**.
- *Unable to acquire impersonated credentials* in **Sign in to Google**: the
  `PLAY_*` variables or the pool's condition do not match this repository
  and environment.
- *The bundle is signed with the debug key*: the `play` environment has lost
  its upload key secrets.

To check a build with Play without sending it to anyone, run `bin/android
bundle`, then `bin/android publish --dry-run` with `PLAY_ACCESS_TOKEN` set
to a token for the service account.
