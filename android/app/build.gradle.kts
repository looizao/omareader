import java.util.Properties

plugins {
    id("com.android.application")
}

// VERSION at the top of the repository is the version people see, the same
// for the desktop and the phone.
val appVersion = rootProject.file("../VERSION").readText().trim()

// Play wants a new, higher version code for every upload, including test
// builds between releases, so the code is the number of commits: each push
// to main raises it, and the same commit always builds the same code. A
// shallow clone counts too few, which is why CI fetches the whole history.
val appVersionCode = runCatching {
    providers.exec { commandLine("git", "rev-list", "--count", "HEAD") }
        .standardOutput.asText.get().trim().toInt()
}.getOrDefault(1)

// The Dropbox app key, public by design (PKCE needs no secret), lives in
// DROPBOX_APP_KEY beside VERSION so the desktop reads the same one. Empty
// means this build has no sync. A dropboxAppKey Gradle property overrides
// it, for trying the sync screen before the real app exists.
val dropboxAppKey = (providers.gradleProperty("dropboxAppKey").orNull
    ?: rootProject.file("../DROPBOX_APP_KEY").takeIf { it.exists() }?.readText())
    .orEmpty().trim()
require(dropboxAppKey.matches(Regex("[A-Za-z0-9]*"))) { "DROPBOX_APP_KEY holds an unexpected value" }

// A release is signed with the key named in keystore.properties when there is
// one (kept out of git), otherwise with the debug key, which is enough to
// install it on your own phone.
val keystoreFile = rootProject.file("keystore.properties")
val keystore = Properties().apply { if (keystoreFile.exists()) keystoreFile.inputStream().use { load(it) } }

android {
    namespace = "io.github.looizao.omareader"
    compileSdk = 36

    defaultConfig {
        applicationId = "io.github.looizao.omareader"
        minSdk = 26
        targetSdk = 36
        versionCode = appVersionCode
        versionName = appVersion
        testInstrumentationRunner = "androidx.test.runner.AndroidJUnitRunner"
        buildConfigField("String", "DROPBOX_APP_KEY", "\"$dropboxAppKey\"")
    }

    buildFeatures {
        buildConfig = true
    }

    // The device tests import the same real export the JVM tests read.
    sourceSets.getByName("androidTest").assets.directories.add("src/test/resources")

    signingConfigs {
        if (keystoreFile.exists()) {
            create("release") {
                storeFile = rootProject.file(keystore.getProperty("storeFile"))
                storePassword = keystore.getProperty("storePassword")
                keyAlias = keystore.getProperty("keyAlias")
                keyPassword = keystore.getProperty("keyPassword")
            }
        }
    }

    buildTypes {
        release {
            isMinifyEnabled = true
            isShrinkResources = true
            proguardFiles(getDefaultProguardFile("proguard-android-optimize.txt"), "proguard-rules.pro")
            signingConfig = signingConfigs.findByName("release") ?: signingConfigs.getByName("debug")
        }
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }

    packaging {
        resources.excludes += listOf("META-INF/*.version", "META-INF/**/LICENSE*", "kotlin/**", "DebugProbesKt.bin")
    }

    lint {
        abortOnError = true
        warningsAsErrors = false
    }
}

dependencies {
    // Only the player and the media session: lock screen, headphones,
    // Bluetooth and the notification come with them. No AppCompat, no
    // Compose, no Fragments; the screens are plain platform views.
    implementation("androidx.media3:media3-exoplayer:1.11.1")
    implementation("androidx.media3:media3-session:1.11.1")

    testImplementation("junit:junit:4.13.2")
    // Android's org.json is a stub in local unit tests; this is the real one.
    testImplementation("org.json:json:20260814")

    // Device tests (bin/android device-test) drive the media library the way
    // Android Auto does; they run on an emulator and never ship in the app.
    androidTestImplementation("androidx.test:runner:1.7.0")
    androidTestImplementation("androidx.test.ext:junit:1.3.0")
}
