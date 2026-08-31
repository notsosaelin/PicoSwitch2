plugins {
    id("java-library")
    id("org.jetbrains.kotlin.jvm")
}

// This module is the reusable reference implementation of the PicoSwitch2
// management contract. It deliberately has no Android plugin or Android SDK
// dependency; platform discovery, pairing, GATT lifecycle, and UI live in :app.
dependencies {
    implementation("org.jetbrains.kotlinx:kotlinx-coroutines-core:1.10.2")
    implementation("org.jetbrains.kotlinx:kotlinx-serialization-json:1.9.0")

    testImplementation("junit:junit:4.13.2")
    testImplementation("org.jetbrains.kotlinx:kotlinx-coroutines-test:1.10.2")
}

java {
    sourceCompatibility = JavaVersion.VERSION_17
    targetCompatibility = JavaVersion.VERSION_17
}

kotlin {
    compilerOptions.jvmTarget.set(org.jetbrains.kotlin.gradle.dsl.JvmTarget.JVM_17)
}

// The language-neutral vectors live at repository scope so a non-JVM client
// can consume the same inputs without importing Android or Gradle artifacts.
sourceSets.test {
    resources.srcDir("../../../tools/fixtures/management")
}

// The firmware's canonical KB/M default mappings, shipped in the artifact.
//
// A companion must be able to create and edit a profile with NO adapter
// connected: the library belongs to the user, not to a device. A local profile
// stores only sparse overrides, so drawing one needs the table those overrides
// are applied against — and that table is firmware data.
//
// Linked from tools/fixtures rather than copied, so there is one authority, and
// filtered to the single file: the rest of that directory is test vectors.
// KbmDefaultsTest asserts the shipped copy still matches what the firmware
// emits. This module has no other main resources.
sourceSets.main {
    resources.srcDir("../../../tools/fixtures/management")
    resources.include("kbm-default-mappings.json")
}
