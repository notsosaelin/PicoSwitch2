package dev.picoswitch.companion.data

/**
 * Decides when the app must re-read the adapter's console-slot controller identity.
 *
 * The Adapter page's Controller row is the adapter's canonical slot-0 identity (the `device`
 * command), which is the right source of truth: it is what the console is actually being driven
 * by, not what happens to be connected. The bug this policy fixes is that nothing ever re-read it.
 *
 * Hardware-confirmed 2026-08-21: with Controller Link the active source and delivering accepted
 * reports, the adapter reported `input status` vid/pid `0x001D/0x1200` and source name
 * "Controller Link", while the app's Adapter page still said "None paired". Every `device` command
 * in the app's entire diagnostic log carried `caller=Refreshing adapter` — the identity was read
 * only by the manual Refresh button. That is why it looked correct exactly once: whenever Refresh
 * happened to be pressed while the active source already owned the slot.
 *
 * Ownership handover deliberately neutralizes slot 0 and waits for the new owner's first fresh
 * report, so a read taken immediately after a switch legitimately returns no identity. The policy
 * therefore keeps re-reading for a bounded number of polls until the answer settles, rather than
 * reading once and believing a transitional value.
 *
 * It stores no controller identity of its own. The adapter remains the single source of truth;
 * this only decides *when* to ask.
 */
class ActiveControllerIdentityPolicy(
    private val maxConvergenceReads: Int = DEFAULT_CONVERGENCE_READS,
) {
    private var observedFor: Long? = null
    private var readsRemaining = 0
    private var settled = false

    /**
     * Called with each poll's active source id. True when the identity must be re-read.
     *
     * A change of active source always earns at least one read; after that, reads continue only
     * while the answer has not settled, and never more than [maxConvergenceReads] times, so an
     * adapter that legitimately publishes no identity cannot turn this into an endless poll.
     */
    fun shouldRefresh(activeSourceId: Long): Boolean {
        if (observedFor != activeSourceId) {
            observedFor = activeSourceId
            readsRemaining = maxConvergenceReads
            settled = false
        }
        return !settled && readsRemaining > 0
    }

    /** Called with whatever identity the adapter actually returned for that read. */
    fun identityRead(activeSourceId: Long, identityAttached: Boolean) {
        if (observedFor != activeSourceId) return
        if (readsRemaining > 0) readsRemaining--
        // An owned console has settled once the slot publishes an identity; an unowned console has
        // settled once it publishes none. Anything else is still the handover window.
        if (identityAttached == (activeSourceId != NO_ACTIVE_SOURCE)) settled = true
    }

    companion object {
        const val NO_ACTIVE_SOURCE = 0L

        /** ~30 s of convergence at the background poll interval; then it stops asking. */
        const val DEFAULT_CONVERGENCE_READS = 6
    }
}
