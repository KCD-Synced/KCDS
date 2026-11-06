using System;
using System.Collections.Generic;
using System.Reflection;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices.JavaScript;
using System.Threading.Tasks;

namespace Kcds.Runtime;

/// <summary>
/// C# delegates the host calls: event handlers, timer callbacks, export functions. Each delegate is
/// one JavaScript function while it lives (so <c>Events.Off(name, handler)</c> finds what
/// <c>Events.On(name, handler)</c> added); the host calls it through <see cref="Dispatch"/> with the
/// arguments converted to the delegate's own parameter types, as the resource that made it.
/// </summary>
public static partial class Callbacks
{
    private sealed record Entry(int Resource, Delegate Function);

    private static readonly Dictionary<int, Entry> ById = new();
    private static readonly ConditionalWeakTable<Delegate, JSObject> Functions = new();
    private static int nextId = 1;

    internal static JSObject ToFunction(Delegate function)
    {
        if (Functions.TryGetValue(function, out var known))
        {
            return known;
        }
        var id = nextId;
        nextId += 1;
        var resource = Current.Id;
        ById[id] = new Entry(resource, function);
        var returnsTask = typeof(Task).IsAssignableFrom(function.Method.ReturnType);
        var created = Bridge.Callback(resource, id, returnsTask);
        Functions.Add(function, created);
        return created;
    }

    /// <summary>Lets go of every delegate of a stopped resource.</summary>
    internal static void Forget(int resource)
    {
        var gone = new List<int>();
        foreach (var (id, entry) in ById)
        {
            if (entry.Resource == resource)
            {
                gone.Add(id);
            }
        }
        foreach (var id in gone)
        {
            ById.Remove(id);
        }
    }

    /// <summary>Runs delegate <paramref name="id"/> with the host's arguments; its result for JavaScript.</summary>
    [JSExport]
    [return: JSMarshalAs<JSType.Any>]
    internal static object? Dispatch(int id, [JSMarshalAs<JSType.Array<JSType.Any>>] object?[] args)
    {
        var entry = Find(id);
        var previous = Current.Id;
        Current.Id = entry.Resource;
        try
        {
            return Values.ToJs(Invoke(entry.Function, args));
        }
        finally
        {
            Current.Id = previous;
        }
    }

    /// <summary>As <see cref="Dispatch"/>, for a delegate that answers a task: the host gets a promise.</summary>
    [JSExport]
    [return: JSMarshalAs<JSType.Promise<JSType.Any>>]
    internal static async Task<object?> DispatchAsync(int id, [JSMarshalAs<JSType.Array<JSType.Any>>] object?[] args)
    {
        var entry = Find(id);
        Current.Id = entry.Resource;
        var task = (Task)Invoke(entry.Function, args)!;
        await task;
        var type = task.GetType();
        if (!type.IsGenericType)
        {
            return null;
        }
        var result = type.GetProperty("Result")!;
        return result.PropertyType.Name == "VoidTaskResult" ? null : Values.ToJs(result.GetValue(task));
    }

    private static Entry Find(int id)
    {
        if (!ById.TryGetValue(id, out var entry))
        {
            throw new InvalidOperationException("The C# function is gone (its resource stopped)");
        }
        return entry;
    }

    private static object? Invoke(Delegate function, object?[] args)
    {
        var parameters = function.Method.GetParameters();
        var converted = new object?[parameters.Length];
        for (var index = 0; index < parameters.Length; index++)
        {
            var raw = index < args.Length ? args[index] : null;
            converted[index] = Values.FromJs(raw, parameters[index].ParameterType);
        }
        try
        {
            return function.DynamicInvoke(converted);
        }
        catch (TargetInvocationException problem) when (problem.InnerException != null)
        {
            // The handler's own exception, not the reflection wrapper around it.
            System.Runtime.ExceptionServices.ExceptionDispatchInfo.Capture(problem.InnerException).Throw();
            throw;
        }
    }
}
