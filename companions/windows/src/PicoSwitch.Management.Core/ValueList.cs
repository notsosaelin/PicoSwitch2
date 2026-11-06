using System.Collections;

namespace PicoSwitch.Management;

/// <summary>
/// An immutable list with STRUCTURAL equality.
///
/// Kotlin's <c>data class</c> gets this for free because <c>List</c> compares by
/// content. C# records compare a <c>List&lt;T&gt;</c> field by reference, so a
/// direct translation would silently make two identical snapshots unequal. That
/// is not a cosmetic difference: change detection, test assertions and
/// "unchanged reply" checks are all built on record equality, and every one of
/// them would be quietly wrong.
///
/// Kept deliberately small — construction, indexing, enumeration, equality — and
/// implicitly convertible from an array or list so call sites read the same as
/// the Kotlin they mirror.
/// </summary>
public sealed class ValueList<T> : IReadOnlyList<T>, IEquatable<ValueList<T>>
{
    private readonly T[] items;

    public ValueList(IEnumerable<T> items) => this.items = items as T[] ?? items.ToArray();

    public static ValueList<T> Empty { get; } = new([]);

    public T this[int index] => items[index];

    public int Count => items.Length;

    public static implicit operator ValueList<T>(T[] items) => new(items);

    public static implicit operator ValueList<T>(List<T> items) => new(items);

    public IEnumerator<T> GetEnumerator() => ((IEnumerable<T>)items).GetEnumerator();

    IEnumerator IEnumerable.GetEnumerator() => items.GetEnumerator();

    public bool Equals(ValueList<T>? other)
    {
        if (other is null)
        {
            return false;
        }

        if (ReferenceEquals(this, other))
        {
            return true;
        }

        if (items.Length != other.items.Length)
        {
            return false;
        }

        var comparer = EqualityComparer<T>.Default;
        for (var index = 0; index < items.Length; index++)
        {
            if (!comparer.Equals(items[index], other.items[index]))
            {
                return false;
            }
        }

        return true;
    }

    public override bool Equals(object? obj) => Equals(obj as ValueList<T>);

    public override int GetHashCode()
    {
        var hash = new HashCode();
        foreach (var item in items)
        {
            hash.Add(item);
        }

        return hash.ToHashCode();
    }

    public override string ToString() => $"[{string.Join(", ", items)}]";
}
