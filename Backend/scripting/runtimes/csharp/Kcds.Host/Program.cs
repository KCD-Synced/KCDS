using System.Runtime.InteropServices.JavaScript;

namespace Kcds.Host;

/// <summary>
/// The entry points the JavaScript side calls on the side's one .NET runtime. Each resource is a
/// number the JavaScript side chose (csharp_host.cjs).
/// </summary>
public static partial class Entry
{
    /// <summary>Loads one of the resource's assemblies (its own, or a library it ships).</summary>
    [JSExport]
    internal static void Load(int resource, [JSMarshalAs<JSType.Array<JSType.Number>>] byte[] bytes)
    {
        Kcds.Runtime.Hosting.Install();
        Kcds.Runtime.Hosting.Load(resource, bytes);
    }

    /// <summary>Starts the scripts of the resource's assemblies; their names, one per line.</summary>
    [JSExport]
    internal static string Start(int resource)
    {
        return string.Join("\n", Kcds.Runtime.Hosting.StartScripts(resource));
    }

    /// <summary>Lets go of a stopped resource.</summary>
    [JSExport]
    internal static void Stop(int resource)
    {
        Kcds.Runtime.Hosting.Stop(resource);
    }

    public static void Main()
    {
    }
}
