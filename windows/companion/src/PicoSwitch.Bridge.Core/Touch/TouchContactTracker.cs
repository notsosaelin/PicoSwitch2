namespace PicoSwitch.Bridge.Touch;

/// <summary>
/// Keeps a platform's contact reporting honest before it reaches the engine.
///
/// A platform adapter converts one native event into the contacts that event
/// describes and hands the batch here. Two things then happen that every adapter
/// would otherwise have to reimplement:
///
/// 1. contacts are forwarded in order, so Down / Move / Up stay deterministic;
/// 2. a contact that the platform simply STOPS mentioning, without ever ending
///    it, is cancelled.
///
/// The second one is the important one. A dropped contact is not a theoretical
/// failure — it is what a window losing its gesture, an interrupted event stream
/// or a host bug looks like from in here, and the consequence is a control held
/// down forever with no contact left to release it. Noticing it costs one set
/// difference per event.
///
/// **CONTRACT:** each batch must describe EVERY contact the platform currently
/// knows about, not only the one that changed. An adapter whose platform reports
/// a single changed contact per event must accumulate before calling this, or the
/// reconciliation below will cancel the contacts it did not mention. WinUI is
/// exactly such a platform — see <c>WindowsTouchPointerAdapter</c>.
/// </summary>
public sealed class TouchContactTracker(ITouchContactSink engine)
{
    private readonly HashSet<long> active = [];

    /// <summary>Released contacts that must lift before they may claim new geometry.</summary>
    private readonly HashSet<long> quarantined = [];

    /// <summary>Contacts currently believed to be down. Diagnostics only.</summary>
    public int ActiveCount => active.Count;

    public int QuarantinedCount => quarantined.Count;

    public void Dispatch(IReadOnlyList<TouchContact> batch)
    {
        foreach (var contact in batch)
        {
            if (quarantined.Contains(contact.Id))
            {
                if (contact.Phase is TouchPhase.Up or TouchPhase.Cancel)
                {
                    quarantined.Remove(contact.Id);
                }

                continue;
            }

            switch (contact.Phase)
            {
                case TouchPhase.Down:
                case TouchPhase.Move:
                    active.Add(contact.Id);
                    break;
                default:
                    active.Remove(contact.Id);
                    break;
            }

            engine.OnContact(contact);
        }

        var present = new HashSet<long>(batch.Select(contact => contact.Id));
        var vanished = active.Where(id => !present.Contains(id)).ToList();
        foreach (var id in vanished)
        {
            active.Remove(id);
            engine.OnContact(new TouchContact(id, TouchPhase.Cancel, 0f, 0f));
        }

        // A platform may lose the whole stream at the same boundary that caused
        // the release. Once an id is absent it cannot later be the same held
        // contact, so keeping it quarantined would only reject a legitimately
        // recycled identifier.
        quarantined.RemoveWhere(id => !present.Contains(id));
    }

    /// <summary>
    /// Forget every contact and release the engine.
    ///
    /// The adapter's boundary call: disposal, host inactivity, a caught fault. The
    /// reason is carried through so the diagnostic says which boundary fired.
    /// </summary>
    public void ReleaseAll(TouchReleaseReason reason)
    {
        quarantined.UnionWith(active);
        active.Clear();
        engine.ReleaseAll(reason);
    }
}
