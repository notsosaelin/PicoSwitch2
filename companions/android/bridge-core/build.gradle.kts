plugins {
    id("java-library")
    id("org.jetbrains.kotlin.jvm")
}

// NO Android dependency, deliberately. Bridge Core is the platform-neutral
// definition of the PicoSwitch Bridge; a future Windows or Linux backend compiles
// this same module. Adding an Android (or any other platform) dependency here
// would silently turn the definition back into "whatever Android happens to do".
// The `bridge core has no platform imports` guard test additionally scans the
// sources, so a stray `android.*` reference fails fast with a readable message.
dependencies {
    // `api`, not `implementation`: ControllerInputState and BridgeSession publish
    // StateFlow, so coroutines are part of this module's contract.
    api("org.jetbrains.kotlinx:kotlinx-coroutines-core:1.10.2")
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
