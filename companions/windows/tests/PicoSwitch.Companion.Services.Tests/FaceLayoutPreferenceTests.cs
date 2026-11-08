using PicoSwitch.Bridge.Core;
using PicoSwitch.Companion.Platform.Storage;
using Xunit;

namespace PicoSwitch.Companion.Services.Tests;

/// <summary>
/// The printed-legend preference both companions persist.
///
/// The stored vocabulary is pinned here because it is a file on users' disks:
/// renaming an enum member must not silently reset everyone to Auto.
/// </summary>
public sealed class FaceLayoutPreferenceTests : IDisposable
{
    private readonly string root = Path.Combine(
        Path.GetTempPath(), "ps2-face-" + Guid.NewGuid().ToString("N"));

    private DocumentStore Documents => new(root);

    public void Dispose()
    {
        if (Directory.Exists(root))
        {
            Directory.Delete(root, recursive: true);
        }
    }

    [Theory]
    [InlineData(ControllerFaceLayout.Nintendo, "nintendo")]
    [InlineData(ControllerFaceLayout.Xbox, "xbox")]
    [InlineData(ControllerFaceLayout.Auto, "auto")]
    public void TheStoredWordsAreStable(ControllerFaceLayout layout, string stored)
    {
        FaceLayoutPreference.Set(Documents, new ControllerInputSession(), layout);
        Assert.Equal(stored, Documents.Read(FaceLayoutPreference.DocumentName));
        Assert.Equal(layout, FaceLayoutPreference.Load(Documents));
    }

    [Theory]
    [InlineData(null)]
    [InlineData("")]
    [InlineData("Nintendo")]
    [InlineData("playstation")]
    public void AnythingUnrecognisedIsAuto(string? stored)
    {
        if (stored is not null)
        {
            Documents.Write(FaceLayoutPreference.DocumentName, stored);
        }

        Assert.Equal(ControllerFaceLayout.Auto, FaceLayoutPreference.Load(Documents));
    }

    [Fact]
    public void ARestoredChoiceReachesTheSession()
    {
        Documents.Write(FaceLayoutPreference.DocumentName, "xbox\n");
        var input = new ControllerInputSession();

        FaceLayoutPreference.Restore(Documents, input);

        Assert.Equal(ControllerFaceLayout.Xbox, input.FaceLayout);
    }
}
