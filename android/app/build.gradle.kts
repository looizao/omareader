import java.util.Properties

plugins {
    id("com.android.application")
}

// VERSION at the top of the repository is the one version for the desktop
// and the phone; 1.2.3 becomes version code 10203.
val appVersion = rootProject.file("../VERSION").readText().trim()
val appVersionCode = appVersion.split(".").map { it.toInt() }.let { (major, minor, patch) ->
    major * 10000 + minor * 100 + patch
}

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
