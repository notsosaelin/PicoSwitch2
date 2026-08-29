using PicoSwitch.Management;

namespace PicoSwitch.Companion.Services;

/// <summary>
/// One failure, rendered for a person.
///
/// Lives here rather than in a page because it is not a XAML concern: every front
/// end over this service should describe a failure the same way, and a private
/// helper in one code-behind is both untestable and easy to diverge from.
///
/// From the 2026-08-29 retest, where the recovery ladder's aggregate produced:
///
/// <code>
/// The adapter did not expose its management service. · The adapter did not expose its management service.
/// </code>
///
/// Both ladder branches failed the same way, so joining them verbatim said the
/// same sentence twice. The repetition reads as two distinct problems and makes
/// the message twice as long for no added information.
/// </summary>
public static class ManagementErrorText
{
    public static string Summarize(Exception error)
    {
        if (error is not AggregateException aggregate)
        {
            return error.Message;
        }

        // Distinct, in order. The ladder reports the fallback and the direct
        // failure together, and they are usually the same sentence -- but when
        // they differ, both matter and both are kept.
        var seen = new List<string>();
        foreach (var branch in aggregate.InnerExceptions)
        {
            var message = branch.Message;
            if (!string.IsNullOrWhiteSpace(message) &&
                !seen.Contains(message, StringComparer.Ordinal))
            {
                seen.Add(message);
            }
        }

        return seen.Count == 0 ? aggregate.Message : string.Join(" · ", seen);
    }
}

/// <summary>
/// A connect that failed because the adapter no longer holds our key.
///
/// A distinct type so a caller can present the ACTIONABLE message without having
/// to re-run the classification. On 2026-08-29 the relationship reached
/// <c>RepairRequired</c> correctly and the banner carried
/// "Repair pairing to continue", while the error surface -- the most prominent
/// thing on the page -- said "The adapter did not expose its management service",
/// twice. The diagnosis was right and the user was told something else.
///
/// The original tagged failure is retained as the inner exception, so the
/// diagnostic line keeps the stage, status, ATT byte and <c>HRESULT</c>.
/// </summary>
public sealed class AdapterBondMismatchException(string message, Exception innerException)
    : ManagementException(message, innerException);
