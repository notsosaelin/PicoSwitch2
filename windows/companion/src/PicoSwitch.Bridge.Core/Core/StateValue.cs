namespace PicoSwitch.Bridge.Core;

/// <summary>
/// An observable current value. The read side.
///
/// The C# stand-in for Kotlin's <c>StateFlow</c>: there is always a current
/// value, and observers are notified only when it actually CHANGES. That second
/// half is load-bearing rather than an optimisation — the input path recomposes a
/// whole snapshot on every event at up to 125 Hz, and without conflation an
/// unchanged snapshot would wake every observer 125 times a second.
/// </summary>
public interface IReadOnlyStateValue<out T>
{
    T Value { get; }

    event Action Changed;
}

/// <summary>
/// The write side. Deliberately not an <c>IObservable&lt;T&gt;</c> pipeline: this
/// layer needs a value with change notification and nothing else, and a
/// dependency-free core is worth more here than operator composition.
/// </summary>
public sealed class StateValue<T>(T initial) : IReadOnlyStateValue<T>
{
    private readonly IEqualityComparer<T> comparer = EqualityComparer<T>.Default;

    public T Value { get; private set; } = initial;

    public event Action? Changed;

    event Action IReadOnlyStateValue<T>.Changed
    {
        add => Changed += value;
        remove => Changed -= value;
    }

    /// <summary>Returns true when the value actually changed and observers were notified.</summary>
    public bool Set(T next)
    {
        if (comparer.Equals(Value, next))
        {
            return false;
        }

        Value = next;
        Changed?.Invoke();
        return true;
    }
}
