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
    }

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
}
