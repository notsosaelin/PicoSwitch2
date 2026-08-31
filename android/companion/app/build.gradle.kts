import java.util.Properties

plugins {
    id("com.android.application")
    id("org.jetbrains.kotlin.android")
    id("org.jetbrains.kotlin.plugin.compose")
}

// Release signing credentials, loaded from OUTSIDE version control.
//
// Resolution order: `app/keystore.properties`, then environment variables. Both
// are external by construction -- no key, password or alias is ever stored in
// the repository.
//
// Absent credentials are NOT an error: the release variant still builds and
// produces an unsigned APK. That keeps CI and ordinary contributors working
// while making it obvious that a publishable artifact needs the real key.
val keystoreProperties = Properties().apply {
    val file = rootProject.file("app/keystore.properties")
    if (file.exists()) file.inputStream().use { load(it) }
}

fun signingValue(key: String, env: String): String? =
    keystoreProperties.getProperty(key) ?: System.getenv(env)

val releaseStoreFile = signingValue("storeFile", "PICOSWITCH_KEYSTORE")
val releaseStorePassword = signingValue("storePassword", "PICOSWITCH_KEYSTORE_PASSWORD")
val releaseKeyAlias = signingValue("keyAlias", "PICOSWITCH_KEY_ALIAS")
val releaseKeyPassword = signingValue("keyPassword", "PICOSWITCH_KEY_PASSWORD")
val hasReleaseSigning = listOf(
    releaseStoreFile, releaseStorePassword, releaseKeyAlias, releaseKeyPassword,
).all { !it.isNullOrBlank() } && File(releaseStoreFile!!).exists()

android {
    namespace = "dev.picoswitch.companion"
    compileSdk = 36

    defaultConfig {
        applicationId = "dev.picoswitch.companion"
        minSdk = 28
        targetSdk = 35
        // Ships alongside a PicoSwitch2 firmware release; the two must be updated
        // together (see BridgeContract). Product version only -- it is NOT the
        // bridge contract version, which changes only when the wire changes.
        //
        // versionCode scheme: major * 10000 + minor * 100 + patch.
        versionCode = 20000
        versionName = "2.0.0"

        testInstrumentationRunner = "androidx.test.runner.AndroidJUnitRunner"
        vectorDrawables.useSupportLibrary = true
        buildConfigField("String", "MGMT_PROTOCOL_VERSION", "\"1\"")
    }

    signingConfigs {
        if (hasReleaseSigning) {
            create("release") {
                storeFile = File(releaseStoreFile!!)
                storePassword = releaseStorePassword
                keyAlias = releaseKeyAlias
                keyPassword = releaseKeyPassword
                enableV1Signing = true
                enableV2Signing = true
                enableV3Signing = true
            }
        }
    }

    buildTypes {
        debug {
            // Suffixed so a debug build can sit alongside the published app. The
            // RELEASE variant deliberately keeps the bare applicationId, which is
            // what makes future in-place upgrades possible.
            applicationIdSuffix = ".debug"
            versionNameSuffix = "-debug"
        }
        release {
            isMinifyEnabled = false
            proguardFiles(
                getDefaultProguardFile("proguard-android-optimize.txt"),
                "proguard-rules.pro",
            )
            if (hasReleaseSigning) {
                signingConfig = signingConfigs.getByName("release")
            }
        }
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }
    kotlinOptions.jvmTarget = "17"
    buildFeatures {
        compose = true
        buildConfig = true
    }
    packaging.resources.excludes += "/META-INF/{AL2.0,LGPL2.1}"
    testOptions.unitTests.isIncludeAndroidResources = true
    // DiagnosticLog mirrors every event to android.util.Log so hardware bring-up is
    // observable over ADB. That class is covered by pure-JVM tests, where the android
    // stubs throw unless unmocked calls are allowed to return their default value.
    testOptions.unitTests.isReturnDefaultValues = true

    // The firmware-emitted KB/M vectors, at repository scope so every client
    // consumes the same inputs. The cross-platform profile tests live in this
    // module because they drive the app's own library and bank projection, and
    // they must be fed content the ADAPTER produced: hand-written fixtures are
    // how a pagination defect once passed every client test in two languages
    // while the firmware dropped a row per page.
    sourceSets.getByName("test").resources.srcDir("../../../tools/fixtures/management")
}

// Amiibo crypto fixture generation needs the user's own key_retail.bin, which is
// never in the repository and never committed. Gradle does not forward -D to the
// test JVM, so these two are passed through explicitly; declaring them as inputs
// also stops an up-to-date check from silently skipping a regeneration run.
//
// Absent them, the fixture generator assumes out and the keyless fixture check
// still runs, which is what keeps CI meaningful without any key material.
tasks.withType<Test>().configureEach {
    listOf("amiibo.retailKey", "amiibo.regenerate").forEach { name ->
        System.getProperty(name)?.let { systemProperty(name, it) }
    }
}

dependencies {
    // Bridge Core: the platform-neutral definition of the PicoSwitch Bridge. This
    // app is one backend for it, not the definition itself. `api` so the Compose
    // layer can observe the shared model directly.
    api(project(":bridge-core"))
    api(project(":management-core"))

    val composeBom = platform("androidx.compose:compose-bom:2025.08.01")
    implementation(composeBom)
    androidTestImplementation(composeBom)

    implementation("androidx.core:core-ktx:1.17.0")
    // The framework's ExifInterface only understands JPEG. The system photo
    // picker hands back HEIC on most modern phones, whose orientation tag the
    // framework class silently cannot read -- so a sideways background would be
    // stored sideways. See TouchBackgroundStore.
    implementation("androidx.exifinterface:exifinterface:1.4.1")
    implementation("androidx.activity:activity-compose:1.10.1")
    implementation("androidx.lifecycle:lifecycle-runtime-ktx:2.9.2")
    implementation("androidx.lifecycle:lifecycle-viewmodel-compose:2.9.2")
    implementation("androidx.compose.ui:ui")
    implementation("androidx.compose.ui:ui-tooling-preview")
    implementation("androidx.compose.foundation:foundation")
    implementation("androidx.compose.material3:material3")
    implementation("androidx.compose.material:material-icons-extended")
    implementation("org.jetbrains.kotlinx:kotlinx-coroutines-android:1.10.2")
    implementation("org.jetbrains.kotlinx:kotlinx-serialization-json:1.9.0")

    debugImplementation("androidx.compose.ui:ui-tooling")
    debugImplementation("androidx.compose.ui:ui-test-manifest")

    testImplementation("junit:junit:4.13.2")
    testImplementation("org.jetbrains.kotlinx:kotlinx-coroutines-test:1.10.2")
    androidTestImplementation("androidx.test.ext:junit:1.2.1")
    androidTestImplementation("androidx.test.espresso:espresso-core:3.6.1")
    androidTestImplementation("androidx.compose.ui:ui-test-junit4")
}
