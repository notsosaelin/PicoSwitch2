package dev.picoswitch.bridge.session

/**
 * How far along the link to the adapter is.
 *
 * Deliberately named for what the BRIDGE is doing, not for how a platform does
 * it: the previous `AcquiringProfile` was Android's profile-proxy vocabulary
 * leaking into shared state. Every platform has a preparation step of some kind;
 * what that step consists of is transport detail and belongs in [message].
 */
enum class BridgeLinkPhase {
    /** Nothing running. */
    Idle,

    /** Obtaining whatever the host requires before it can act as a HID device. */
    Preparing,

    /** The descriptor has been submitted; waiting for the host to confirm. */
    Registering,

    /** Registered as a HID device and able to connect. */
    Ready,

    /** Connecting to the adapter. */
    Connecting,

    /** Reports are flowing. */
    Playing,

    /** This host cannot act as a HID device at all. */
    Unsupported,

    /** A step failed; [BridgeState.message] says which and what to do. */
    Failed,
}

/**
 * A paired adapter, as the host knows it.
 *
 * Opaque on purpose. The bridge compares, persists and displays hosts; it never
 * needs the platform's own device object, so that object stays behind the
 * transport instead of appearing in shared state and in the UI.
 */
interface BridgeHost {
    val address: String
    val name: String?
}

/**
 * The platform's HID-device transport.
 *
 * The split is: this interface owns TRANSPORT MECHANICS — registering a HID
 * report descriptor with the host OS, discovering paired adapters, opening and
 * closing the link, pushing report bytes, and the host-stack quirks around all of
 * that. It owns none of the PROTOCOL or SESSION semantics: report composition,
 * motion gating, rumble, battery polling, cadence, neutralization and report
 * accounting all live in [BridgeSession] and are identical on every platform.
 *
 * A transport reports what happened through [Listener] rather than returning it,
 * because on every host stack examined so far the synchronous return value of a
 * registration or connection call is a request acknowledgement, not an outcome.
 *
 * Implementations must be safe to call [stop] on repeatedly and from any thread.
 */
interface BridgeTransport {
    /** Attach the session before any other call. */
    fun attach(listener: Listener)

    /**
     * The listener this transport will actually deliver to, for wiring audits.
     *
     * Exists because "attach() was called" and "the object that receives HID
     * callbacks is the live session" are different claims, and only the second
     * one matters. A transport that stored the listener somewhere the callbacks
     * do not read would satisfy the first and fail the second.
     */
    fun attachedListener(): Listener?

    /** Adapters the host already knows about and could connect to. */
    fun knownHosts(): List<BridgeHost>

    /**
     * Register as a HID device and, if [preferredHost] is non-null, connect to it
     * as soon as registration is confirmed.
     */
    fun start(preferredHost: BridgeHost?)

    /** Connect to a specific adapter, registering first if necessary. */
    fun connect(host: BridgeHost)

    /** Push one input report. Returns false when the host rejected it. */
    fun send(reportId: Int, payload: ByteArray): Boolean

    /** Tear the link down and release any host-wide resources it holds. */
    fun stop()

    /** Permanent teardown; the transport is not reusable afterwards. */
    fun close()

    interface Listener {
        /** Link progress, including failures. */
        fun onPhase(
            phase: BridgeLinkPhase,
            hostName: String? = null,
            message: String? = null,
            registered: Boolean = false,
        )

        /** The interrupt link is up and reports may be sent. */
        fun onLinkUp(hostName: String?)

        /** The link went away, for any reason including a deliberate stop. */
        fun onLinkDown(message: String?)

        /** An output report arrived, in whatever framing the host stack used. */
        fun onOutputReport(data: ByteArray?, reportId: Int?)

        /**
         * The host polled for the current state over the control channel. Returns
         * a complete, freshly composed input report payload.
         */
        fun currentReport(): ByteArray
    }
}
