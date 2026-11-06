package dev.picoswitch.companion.data

/**
 * How the management bond was started. Every mechanism here is LE; TRANSPORT_AUTO is deliberately
 * absent because it is the confirmed cause of the "incorrect PIN or passkey" failure.
 */
enum class AdapterBondMechanism(val diagnosticName: String) {
    /**
     * `BluetoothDevice.createBond(TRANSPORT_LE)`. Public SDK from API 37; before that the same
     * method exists but is not in `android.jar`, so it is reached through the single reflective
     * compatibility seam in the caller.
     */
    LeCreateBond("le-create-bond"),

    /**
     * No explicit bond call was available, so the management GATT connection itself provokes LE
     * pairing: the firmware's RX/TX characteristics and the TX CCC are `ATT_SECURITY_ENCRYPTED`,
     * Android's stack answers `GATT_INSUFFICIENT_AUTHENTICATION` by encrypting the LE link, and
     * with no stored LTK that starts SMP bonding. Uses only public API.
     */
    LeGattInitiated("le-gatt-initiated"),

    /** No LE bonding could be started at all. */
    Unavailable("none"),
}

data class AdapterBondStartResult(
    val mechanism: AdapterBondMechanism,
    val detail: String,
) {
    /** True when an explicit bond call was accepted and Android owns the procedure from here. */
    val startedExplicitBond: Boolean get() = mechanism == AdapterBondMechanism.LeCreateBond

    /** True when the caller must open the management GATT link to provoke bonding instead. */
    val delegatesToGatt: Boolean get() = mechanism == AdapterBondMechanism.LeGattInitiated
}

/**
 * Starts the Android bond for the management relationship on the **LE** transport, always.
 *
 * PicoSwitch2 is a genuine dual-mode device: a Classic HID host for controllers and an LE
 * peripheral for management, both on the same public BD_ADDR. Once a phone has observed the
 * Classic identity its device record becomes `DEVICE_TYPE_DUAL` and stays that way across
 * "Forget" — the cached type is adapter-level, not bond-level. `BluetoothDevice.createBond()` is
 * `createBond(TRANSPORT_AUTO)`, and TRANSPORT_AUTO prefers BR/EDR on a dual-mode record, so the
 * phone runs SSP against the adapter's Classic admission gate instead of LE SMP against the
 * management service. That gate correctly refuses an unbonded Classic ACL, and Android reports the
 * refusal to the user as "Couldn't pair because of incorrect PIN or passkey".
 *
 * Confirmed on hardware (2026-08-21, Android 13 / bt_btif):
 *   btif_get_device_type: Device [<adapter>] type 3      (DEVICE_TYPE_DUAL)
 *   bt_btm_sec: transport=classic, btm_status=8
 *   bta_dm_authentication_complete_cback deleting <adapter> - result: 0x0e
 *   bondStateChangeCallback: Status: 9 ... newState: 0 hciReason: 14
 * and after forcing LE, on the same phone and adapter:
 *   bt_btm_sec: transport=le, btm_status=10
 * followed by a successful fresh pair inside the adapter's pairing window.
 *
 * A BR/EDR bond is not merely slower here — it is the wrong relationship. The management link is
 * GATT over `TRANSPORT_LE`, and the firmware authorizes it only when that LE link is bonded and
 * encrypted with a 16-byte key, so a Classic link key could never satisfy it even if SSP succeeded.
 * TRANSPORT_AUTO is therefore never used, not even as a fallback.
 *
 * API compatibility: `createBond(int transport)` is absent from `android.jar` through API 36 and
 * public from API 37, so on every currently shipping Android it needs the one reflective seam the
 * caller provides. Where that seam is unavailable this falls back to [AdapterBondMechanism
 * .LeGattInitiated], which reaches the same LE SMP procedure with public API only.
 */
class AdapterBondStarter(private val platform: Platform) {

    interface Platform {
        /**
         * Invokes `createBond(TRANSPORT_LE)`. Returns `null` when the entry point is not reachable
         * on this platform at all, which is the runtime feature detection for the seam.
         */
        fun createBondOnLe(): Boolean?

        /** Android's cached device type, for diagnostics only. */
        fun cachedDeviceTypeName(): String
    }

    fun start(): AdapterBondStartResult {
        val type = runCatching { platform.cachedDeviceTypeName() }.getOrDefault("unreadable")
        val le = runCatching { platform.createBondOnLe() }.getOrNull()
        return when (le) {
            true -> AdapterBondStartResult(AdapterBondMechanism.LeCreateBond, "type=$type")
            // Refused is a real "no" from a working entry point: the peer or the stack is busy, and
            // opening a GATT link on top of that would race Android's own state. Report it.
            false -> AdapterBondStartResult(AdapterBondMechanism.Unavailable, "type=$type le=refused")
            null -> AdapterBondStartResult(
                AdapterBondMechanism.LeGattInitiated,
                "type=$type le=unavailable; provoking SMP from the encrypted management GATT link",
            )
        }
    }
}
