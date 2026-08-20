pluginManagement {
    repositories {
        google()
        mavenCentral()
        gradlePluginPortal()
    }
}

dependencyResolutionManagement {
    repositoriesMode.set(RepositoriesMode.FAIL_ON_PROJECT_REPOS)
    repositories {
        google()
        mavenCentral()
    }
}

rootProject.name = "PicoSwitch Companion"

// Bridge Core is a plain Kotlin/JVM library on purpose: the Android SDK is not on
// its compile classpath, so any Android type that leaks into the shared model is a
// build failure rather than a review finding. See docs/bridge/PLATFORM_BACKEND.md.
include(":bridge-core")
include(":management-core")
include(":app")
