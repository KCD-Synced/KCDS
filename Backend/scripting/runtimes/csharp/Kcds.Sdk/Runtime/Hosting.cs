using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Reflection;
using System.Runtime.Loader;

namespace Kcds.Runtime;

/// <summary>
/// What the C# resource host (Kcds.Host) does with resources: each in its own collectible
/// AssemblyLoadContext on the side's one .NET runtime, its log, its scripts, its stop. Not for
/// resources themselves.
/// </summary>
public static class Hosting
{
    private static readonly Dictionary<int, AssemblyLoadContext> Contexts = new();
    private static bool installed;

    /// <summary>Console output goes to the running resource's log.</summary>
    public static void Install()
    {
        if (installed)
        {
            return;
        }
        installed = true;
        var writer = new LogWriter();
        Console.SetOut(writer);
        Console.SetError(writer);
    }

    /// <summary>Loads one of a resource's assemblies into its own context (made on first use).</summary>
    public static Assembly Load(int resource, byte[] bytes)
    {
        if (!Contexts.TryGetValue(resource, out var context))
        {
            // Kcds.Sdk and the framework resolve to the runtime's own (the default context).
            context = new AssemblyLoadContext("kcds-resource-" + resource, isCollectible: true);
            Contexts[resource] = context;
        }
        return context.LoadFromStream(new MemoryStream(bytes));
    }

    /// <summary>
    /// Makes each public <see cref="Script"/> of the resource's assemblies, in the order they were
    /// loaded and declared, and runs its OnStart.
    /// </summary>
    public static IReadOnlyList<string> StartScripts(int resource)
    {
        if (!Contexts.TryGetValue(resource, out var context))
        {
            throw new InvalidOperationException("The resource has no assemblies");
        }
        var scripts = context.Assemblies
            .SelectMany((assembly) => assembly.GetTypes().OrderBy((type) => type.MetadataToken))
            .Where((type) => type.IsPublic && !type.IsAbstract && typeof(Script).IsAssignableFrom(type))
            .ToList();
        if (scripts.Count == 0)
        {
            throw new InvalidOperationException("The resource's assemblies have no public class deriving from Kcds.Script");
        }
        var started = new List<string>();
        Current.Id = resource;
        foreach (var type in scripts)
        {
            Script script;
            try
            {
                script = (Script)Activator.CreateInstance(type)!;
            }
            catch (TargetInvocationException problem) when (problem.InnerException != null)
            {
                throw problem.InnerException;
            }
            script.OnStart();
            started.Add(type.FullName!);
        }
        return started;
    }

    /// <summary>
    /// A stopped resource: its delegates and paths are let go and its context unloaded. (The
    /// WebAssembly runtime keeps an unloaded context's few kilobytes; nothing of it runs again.)
    /// </summary>
    public static void Stop(int resource)
    {
        Callbacks.Forget(resource);
        Sdk.Forget(resource);
        if (Contexts.Remove(resource, out var context))
        {
            context.Unload();
        }
    }
}
