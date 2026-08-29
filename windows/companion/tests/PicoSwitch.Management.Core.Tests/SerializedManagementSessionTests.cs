using Xunit;

namespace PicoSwitch.Management.Tests;

/// <summary>
/// Single-flight ownership.
///
/// Management replies carry no request identifier, so the ONLY thing keeping a
/// late reply from becoming the next request's reply is that the next request
/// cannot start. These tests pin that, including the awkward half: a caller who
/// gives up must not release the carrier before the exchange it started has
/// finished.
/// </summary>
public sealed class SerializedManagementSessionTests
{
    [Fact]
    public async Task ConcurrentExchangesAreSingleFlight()
    {
        var session = new SerializedManagementSession();
        var release = new TaskCompletionSource();
        var events = new List<string>();

        var first = session.ExchangeAsync<object?>(async () =>
        {
            lock (events)
            {
                events.Add("first-start");
            }

            await release.Task;
            lock (events)
            {
                events.Add("first-end");
            }

            return null;
        });

        await WaitFor(() => Snapshot(events).Contains("first-start"));

        var second = session.ExchangeAsync<object?>(() =>
        {
            lock (events)
            {
                events.Add("second");
            }

            return Task.FromResult<object?>(null);
        });

        // The second exchange must not have run: the first still owns the carrier.
        await Task.Delay(20);
        Assert.Equal(["first-start"], Snapshot(events));

        release.SetResult();
        await first;
        await second;
        Assert.Equal(["first-start", "first-end", "second"], Snapshot(events));
    }

    [Fact]
    public async Task CancellationAfterOwnershipDoesNotAbandonAReply()
    {
        var session = new SerializedManagementSession();
        var transmitted = new TaskCompletionSource();
        var reply = new TaskCompletionSource();
        var consumed = false;

        using var cancellation = new CancellationTokenSource();
        var first = session.ExchangeAsync<object?>(
            async () =>
            {
                transmitted.SetResult();
                await reply.Task;
                consumed = true;
                return null;
            },
            cancellation.Token);

        await transmitted.Task;
        await cancellation.CancelAsync();

        var secondStarted = false;
        var second = session.ExchangeAsync<object?>(() =>
        {
            secondStarted = true;
            return Task.FromResult<object?>(null);
        });

        await Task.Delay(20);
        Assert.False(secondStarted, "a cancelled caller must not hand the carrier over mid-exchange");

        reply.SetResult();
        await first;
        await second;
        Assert.True(consumed);
        Assert.True(secondStarted);
    }

    [Fact]
    public async Task CallerCancelledWhileQueuedNeverTransmits()
    {
        var session = new SerializedManagementSession();
        var release = new TaskCompletionSource();
        var first = session.ExchangeAsync<object?>(async () =>
        {
            await release.Task;
            return null;
        });

        using var cancellation = new CancellationTokenSource();
        var transmitted = false;
        var queued = session.ExchangeAsync<object?>(
            () =>
            {
                transmitted = true;
                return Task.FromResult<object?>(null);
            },
            cancellation.Token);

        await cancellation.CancelAsync();
        await Assert.ThrowsAnyAsync<OperationCanceledException>(() => queued);

        release.SetResult();
        await first;
        Assert.False(transmitted);
    }

    [Fact]
    public async Task SessionMutationWaitsForInFlightExchange()
    {
        var session = new SerializedManagementSession();
        var release = new TaskCompletionSource();
        var order = new List<string>();

        var exchange = session.ExchangeAsync<object?>(async () =>
        {
            lock (order)
            {
                order.Add("exchange");
            }

            await release.Task;
            return null;
        });

        await WaitFor(() => Snapshot(order).Contains("exchange"));

        var mutation = session.MutateAsync(() =>
        {
            lock (order)
            {
                order.Add("disconnect");
            }

            return Task.CompletedTask;
        });

        await Task.Delay(20);
        Assert.Equal(["exchange"], Snapshot(order));

        release.SetResult();
        await exchange;
        await mutation;
        Assert.Equal(["exchange", "disconnect"], Snapshot(order));
    }

    [Fact]
    public async Task CancellationAfterMutationOwnershipCannotExposePartialCleanup()
    {
        var session = new SerializedManagementSession();
        var started = new TaskCompletionSource();
        var release = new TaskCompletionSource();
        var cleanupCompleted = false;

        using var cancellation = new CancellationTokenSource();
        var mutation = session.MutateAsync(
            async () =>
            {
                started.SetResult();
                await release.Task;
                cleanupCompleted = true;
            },
            cancellation.Token);

        await started.Task;
        await cancellation.CancelAsync();

        var exchangeStarted = false;
        var exchange = session.ExchangeAsync<object?>(() =>
        {
            exchangeStarted = true;
            return Task.FromResult<object?>(null);
        });

        await Task.Delay(20);
        Assert.False(exchangeStarted);

        release.SetResult();
        await mutation;
        await exchange;
        Assert.True(cleanupCompleted);
        Assert.True(exchangeStarted);
    }

    private static List<string> Snapshot(List<string> events)
    {
        lock (events)
        {
            return [.. events];
        }
    }

    private static async Task WaitFor(Func<bool> condition)
    {
        for (var attempt = 0; attempt < 200 && !condition(); attempt++)
        {
            await Task.Delay(5);
        }

        Assert.True(condition(), "condition was never reached");
    }
}
