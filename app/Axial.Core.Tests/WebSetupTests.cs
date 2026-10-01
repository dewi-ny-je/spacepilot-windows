using Xunit;

namespace Axial.Tests;

public class WebSetupTests {
    [Fact]
    public void AppManagedSetupCancellationIdempotenceAndRenewal() => SingleThreadContext.Run(async () => {
        int retries = 0, restored = 0;
        Task Retry() { retries++; return Task.CompletedTask; }
        var disabled = new WebSetupCoordinator(new FakeWebSetup());
        await disabled.RefreshAsync(false, false, "", Retry);
        Assert.False(disabled.Ready); Assert.Equal(0, retries); Assert.Contains("disabled", disabled.Message);
        var operations = new FakeWebSetup(); var subject = new WebSetupCoordinator(operations);
        await subject.RefreshAsync(true, false, "", Retry);
        Assert.False(subject.Ready); Assert.False(subject.Busy); Assert.Contains("setup", subject.Message);
        Assert.Equal((1, 0, 0), (operations.Inspections, operations.Generations, operations.Approvals)); // checking never prompts
        await subject.RefreshAsync(true, false, "", Retry);
        Assert.Equal(1, operations.Inspections); // throttled
        operations.Cancelled = true;
        await subject.SetUpAsync(Retry, () => restored++);
        Assert.False(subject.Ready); Assert.False(subject.Busy); Assert.Contains("cancelled", subject.Message); Assert.Equal(0, retries);
        Assert.Equal(1, restored); // cancellation returns focus to settings
        operations.Cancelled = false;
        await subject.SetUpAsync(Retry, () => restored++);
        Assert.True(subject.Ready); Assert.False(subject.Busy); Assert.Equal(1, retries);
        Assert.Equal(3, restored); // after the approval and after completion
        Assert.Equal((1, 1), (operations.Generations, operations.Approvals));
        await subject.RefreshAsync(true, true, "", Retry, force: true);
        Assert.True(subject.Ready); Assert.Equal(1, retries);
        // Healthy setup does not repeat generation or approval on a manual retry.
        await subject.SetUpAsync(Retry);
        Assert.Equal((1, 1, 2), (operations.Generations, operations.Approvals, retries));
        // A removed root needs approval again but keeps the existing credentials.
        operations.Snapshot = operations.Snapshot with { Trusted = false };
        await subject.RefreshAsync(true, false, "", Retry, force: true);
        Assert.False(subject.Ready); Assert.Contains("approval", subject.Message);
        await subject.SetUpAsync(Retry);
        Assert.True(subject.Ready); Assert.Equal((1, 2), (operations.Generations, operations.Approvals));
        // Expiring credentials are detected and renewed with a new approval.
        operations.Snapshot = new WebSetupSnapshot(false, false);
        await subject.RefreshAsync(true, true, "", Retry, force: true);
        Assert.False(subject.Ready); Assert.Contains("renewal", subject.Message);
        await subject.SetUpAsync(Retry);
        Assert.True(subject.Ready); Assert.Equal((2, 3), (operations.Generations, operations.Approvals));
        // A failing tool reports its message and stays not ready.
        operations.Snapshot = new WebSetupSnapshot(false, false); operations.InstallFails = true;
        await subject.SetUpAsync(Retry);
        Assert.False(subject.Ready); Assert.Equal("Test repair failure", subject.Message);
    });
}
