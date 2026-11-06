using System.Threading;

namespace Kcds.Runtime;

/// <summary>
/// Which resource the running C# code belongs to. All C# resources of a side share one .NET
/// runtime, each in its own AssemblyLoadContext; the host sets this when it starts a resource or
/// calls one of its handlers, and it flows through <c>await</c>, so every SDK call reaches that
/// resource's own scope (its <c>kcds</c>, its timers, its log).
/// </summary>
public static class Current
{
    private static readonly AsyncLocal<int> Resource = new();

    /// <summary>The resource's number on the host, 0 outside a resource.</summary>
    public static int Id
    {
        get => Resource.Value;
        internal set => Resource.Value = value;
    }
}
