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
