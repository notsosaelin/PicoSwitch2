package dev.picoswitch.companion.ui

/** Immediate single-flight admission for top-level management workflows. */
class OperationAdmissionGate {
    private var active = false

    @Synchronized
    fun tryAcquire(): Boolean {
        if (active) return false
        active = true
        return true
    }

    @Synchronized
    fun release() {
        active = false
    }
}
