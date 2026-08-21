package dev.picoswitch.companion.data

/**
 * Reduces repeated reads of one retained BluetoothDevice's authoritative bond state.
 *
 * BOND_NONE immediately after createBond() is not a rejection: Android can take a short interval
 * to enter BOND_BONDING. Once bonding has been observed, returning to NONE is authoritative
 * rejection. A bounded caller supplies the final timeout so a missing broadcast cannot leave the
 * product in Bonding forever.
 */
class AdapterBondWaitPolicy(initialState: AndroidBondState) {
    private var sawBonding = initialState == AndroidBondState.Bonding

    fun observe(state: AndroidBondState): AdapterBondWaitOutcome = when (state) {
        AndroidBondState.Bonded -> AdapterBondWaitOutcome.Bonded
        AndroidBondState.Bonding -> {
            sawBonding = true
            AdapterBondWaitOutcome.Continue
        }
        AndroidBondState.None -> if (sawBonding) {
            AdapterBondWaitOutcome.Rejected
        } else {
            AdapterBondWaitOutcome.Continue
        }
        AndroidBondState.Unknown -> AdapterBondWaitOutcome.Continue
    }
}

enum class AdapterBondWaitOutcome { Continue, Bonded, Rejected }
