package dev.picoswitch.companion.data

import dev.picoswitch.management.KbmLimits
import dev.picoswitch.management.KbmPositions
import dev.picoswitch.management.KbmProfile
import dev.picoswitch.management.KbmProfileInfo
import dev.picoswitch.management.KbmProfiles
import dev.picoswitch.management.KbmSource
import dev.picoswitch.management.KbmSwitchBinding

/**
 * Where a local profile stands relative to the adapter.
 *
 * THREE INDEPENDENT COMPARISONS, not one overloaded boolean:
 *
 * ```text
 * local vs resident    — is the adapter's copy the one I have?
 * resident vs runtime  — is the adapter RUNNING what it has stored?
 * local vs runtime     — derived from the two above.
 * ```
 *
 * A single `matchesSaved` flag could not express "I edited this locally, the
 * adapter still has the old copy, and it is running that old copy" — which is
 * the ordinary state after a local save and the exact thing the user needs told.
 */
enum class KbmLocalState {
    /** In the library only. Never sent to this adapter. */
    LocalOnly,

    /** Assigned to a bank position, and the copies agree. */
    OnAdapter,

    /**
     * Assigned, but edited locally since. The adapter still holds the older
     * content and is unaware of the edit.
     */
    AdapterCopyOutOfDate,

    /** Assigned, in agreement, and what the console is running now. */
    Active,

    /**
     * The resident copy was updated but the console has not picked it up.
     *
     * Deliberately reachable: assigning into an active position must not mutate
     * gameplay mid-session, so the realized snapshot is kept until the user
     * activates. This state is how the screen says so.
     */
    ResidentUpdatedNotActivated,
}

/** One row of the "on adapter" bank list. */
data class KbmBankSlot(
    val layout: KbmProfile,
    val position: Int,
    val resident: KbmProfileInfo?,
    val isRuntime: Boolean,
    val isBoot: Boolean,
    val switchKey: KbmSource?,
) {
    val empty: Boolean get() = resident == null

    val positionLabel: String get() = KbmPositions.label(position)

    val residentLabel: String get() = resident?.name ?: "Empty"

    /** Default is built-in: it cannot be assigned into or removed. */
    val isDefault: Boolean get() = position == KbmPositions.DEFAULT
}

/** One row of the library list, with its adapter relationship. */
data class KbmLibraryRow(
    val profile: KbmLocalProfile,
    val state: KbmLocalState,
    val assignedPosition: Int?,
) {
    val stateLabel: String
        get() = when (state) {
            KbmLocalState.LocalOnly -> "Local only"
            KbmLocalState.OnAdapter ->
                "On adapter · ${KbmPositions.label(assignedPosition ?: 0)}"
            KbmLocalState.AdapterCopyOutOfDate ->
                "${KbmPositions.label(assignedPosition ?: 0)} · adapter copy out of date"
            KbmLocalState.Active ->
                "${KbmPositions.label(assignedPosition ?: 0)} · active"
            KbmLocalState.ResidentUpdatedNotActivated ->
                "${KbmPositions.label(assignedPosition ?: 0)} · activate to use changes"
        }

    /** Offered only when there is something to send. */
    val canUpdateAdapterCopy: Boolean get() = state == KbmLocalState.AdapterCopyOutOfDate
}

/**
 * The bank and library projection.
 *
 * Pure and adapter-free: it is handed the two snapshots and returns rows, so the
 * relationship rules — which are the easy thing to get subtly wrong — are
 * covered by ordinary JVM tests rather than only by looking at the screen.
 */
object KbmBankView {
    /**
     * Every position of a layout's bank, empty ones included.
     *
     * Empty positions are rows rather than omissions: "Profile 3 — Empty" is what
     * tells a user they have somewhere to assign to, and a list that only showed
     * occupied positions would hide the capacity entirely.
     */
    fun bank(
        profiles: KbmProfiles,
        switches: List<KbmSwitchBinding>,
        layout: KbmProfile,
    ): List<KbmBankSlot> {
        val active = profiles.activeFor(layout)
        return (KbmPositions.DEFAULT..KbmLimits.POSITIONS_PER_LAYOUT).map { position ->
            KbmBankSlot(
                layout = layout,
                position = position,
                resident = if (position == KbmPositions.DEFAULT) {
                    profiles.forLayout(layout).firstOrNull { it.builtin }
                } else {
                    profiles.at(layout, position)
                },
                isRuntime = active?.runtimePosition == position,
                isBoot = active?.bootPosition == position,
                // The switch key is the same in both layouts by design, so the
                // row shows the key that selects THIS position anywhere.
                switchKey = switches.firstOrNull { it.position == position }?.source,
            )
        }
    }

    /** The library, each row carrying its relationship to the adapter. */
    fun library(
        library: KbmProfileLibrary,
        profiles: KbmProfiles,
        layout: KbmProfile,
    ): List<KbmLibraryRow> {
        val active = profiles.activeFor(layout)

        // A resident copy belongs to AT MOST ONE library row.
        //
        // Two local profiles can hold identical content — two untouched copies of
        // Default do — and without this both would claim the same resident and
        // each would be reported as "on adapter". Claiming consumes, and the
        // strongest evidence wins: name AND content, then name, then content.
        //
        // NAME OUTRANKS CONTENT ALONE, and the ordering is load-bearing. Editing
        // a profile locally is the common case and changes its content, so right
        // after a save the local profile no longer matches its own resident copy
        // — but it may coincidentally match a DIFFERENT one. Content-first made
        // an edited "Halo" claim the unrelated resident it now happened to equal
        // and report "on adapter", when the fact the user needed was that Halo's
        // own copy had gone stale. Content still decides when no name matches,
        // which is what keeps a locally renamed profile attached to the resident
        // the other companion wrote.
        val unclaimed = profiles.profiles.filter { it.layout == layout }.toMutableList()
        val rows = library.forLayout(layout)
        val matched = LinkedHashMap<String, KbmProfileInfo>()

        for (strength in 0..2) {
            for (profile in rows) {
                if (matched.containsKey(profile.id)) continue
                val resident = unclaimed.firstOrNull { candidate ->
                    when (strength) {
                        0 -> candidate.fingerprint == profile.fingerprint &&
                            same(candidate.name, profile.name)
                        1 -> same(candidate.name, profile.name)
                        else -> candidate.fingerprint == profile.fingerprint
                    }
                }
                if (resident != null) {
                    matched[profile.id] = resident
                    unclaimed.remove(resident)
                }
            }
        }

        return rows.map { profile ->
            val resident = matched[profile.id]
                ?: return@map KbmLibraryRow(profile, KbmLocalState.LocalOnly, null)

            val agrees = resident.fingerprint == profile.fingerprint
            val isRuntime = active?.runtimePosition == resident.position

            val state = when {
                !agrees -> KbmLocalState.AdapterCopyOutOfDate
                isRuntime && active?.matchesSaved == false ->
                    KbmLocalState.ResidentUpdatedNotActivated
                isRuntime -> KbmLocalState.Active
                else -> KbmLocalState.OnAdapter
            }

            KbmLibraryRow(profile, state, resident.position)
        }
    }

    /**
     * The four semantic switch actions and whatever key is bound to each.
     *
     * Rendered from the ACTIONS rather than from the bindings, so an unassigned
     * action still appears as a row the user can bind — a list built from the
     * bindings alone would hide the actions they have not set up yet.
     */
    fun switchActions(switches: List<KbmSwitchBinding>): List<Pair<Int, KbmSource?>> =
        KbmPositions.all.map { position ->
            position to switches.firstOrNull { it.position == position }?.source
        }

    private fun same(a: String, b: String): Boolean = a.equals(b, ignoreCase = true)
}
