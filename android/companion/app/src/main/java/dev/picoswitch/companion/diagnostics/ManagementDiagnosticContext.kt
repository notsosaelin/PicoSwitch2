package dev.picoswitch.companion.diagnostics

import dev.picoswitch.bridge.session.BridgeLinkPhase
import kotlinx.coroutines.asContextElement
import kotlinx.coroutines.withContext

/** Lightweight correlation context for the serialized management carrier. */
object ManagementDiagnosticContext {
    private val workflow = ThreadLocal<String?>()
    @Volatile private var bridgePhase = BridgeLinkPhase.Idle.name
    @Volatile private var personalityPhase = "idle"

    suspend fun <T> withWorkflow(name: String, action: suspend () -> T): T =
        withContext(workflow.asContextElement(name)) { action() }

    fun workflow(): String = workflow.get() ?: "unscoped"
    fun bridgePhase(): String = bridgePhase
    fun personalityPhase(): String = personalityPhase
    fun setBridgePhase(value: BridgeLinkPhase) { bridgePhase = value.name }
    fun setPersonalityPhase(value: String) { personalityPhase = value }
}
