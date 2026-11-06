using System;
using System.Runtime.InteropServices.JavaScript;
using System.Threading.Tasks;

namespace Kcds.Runtime;

/// <summary>
/// The calls into the resource's JavaScript scope, which the host
/// (Backend/scripting/runtimes/js/languages/csharp_host.cjs) provides as the "kcds" module. Every
/// SDK member a C# resource uses is the very JavaScript member a JavaScript resource of the same
/// side uses, reached through these few calls. A JavaScript error in one of them is a
/// <see cref="Kcds.JsError"/> with that error in it.
/// </summary>
internal static partial class Bridge
{
    private const string Module = "kcds";

    /// <summary>The running resource's <c>kcds</c> object.</summary>
    internal static JSObject Root() => RootOf(Current.Id);

    /// <summary>One of the running resource's other globals (setTimeout, fetch, ...), or null.</summary>
    internal static object? Global(string name) => GlobalOf(Current.Id, name);

    /// <summary>Writes to the running resource's log.</summary>
    internal static void Log(string level, string text) => LogOf(Current.Id, level, text);

    internal static object? Get(object target, string name) => Guard(() => GetRaw(target, name));

    internal static void Set(object target, string name, object? value)
    {
        Guard(() =>
        {
            SetRaw(target, name, value);
            return null;
        });
    }

    internal static object? Invoke(object target, string name, object?[] args) => Guard(() => InvokeRaw(target, name, args));

    internal static object? Call(object function, object?[] args) => Guard(() => CallRaw(function, args));

    internal static object? Construct(object type, object?[] args) => Guard(() => ConstructRaw(type, args));

    /// <summary>
    /// A promise the host gave, which reaches C# as a task of its settled outcome: its value, or
    /// a <see cref="Kcds.JsError"/> when it was rejected.
    /// </summary>
    internal static async Task<object?> Settle(Task<object?> outcome)
    {
        var settled = await outcome;
        if (settled is JSObject wrapped && wrapped.HasProperty("kcdsError"))
        {
            throw new Kcds.JsError(new Kcds.JsValue(GetRaw(wrapped, "kcdsError")));
        }
        if (settled is JSObject value && value.HasProperty("kcdsValue"))
        {
            return GetRaw(value, "kcdsValue");
        }
        return settled;
    }

    private static object? Guard(Func<object?> call)
    {
        try
        {
            return call();
        }
        catch (JSException)
        {
            throw new Kcds.JsError(new Kcds.JsValue(LastError()));
        }
    }

    [JSImport("root", Module)]
    private static partial JSObject RootOf(int resource);

    [JSImport("global", Module)]
    [return: JSMarshalAs<JSType.Any>]
    private static partial object? GlobalOf(int resource, string name);

    [JSImport("log", Module)]
    private static partial void LogOf(int resource, string level, string text);

    [JSImport("get", Module)]
    [return: JSMarshalAs<JSType.Any>]
    private static partial object? GetRaw([JSMarshalAs<JSType.Any>] object target, string name);

    [JSImport("set", Module)]
    private static partial void SetRaw(
        [JSMarshalAs<JSType.Any>] object target,
        string name,
        [JSMarshalAs<JSType.Any>] object? value);

    [JSImport("invoke", Module)]
    [return: JSMarshalAs<JSType.Any>]
    private static partial object? InvokeRaw(
        [JSMarshalAs<JSType.Any>] object target,
        string name,
        [JSMarshalAs<JSType.Array<JSType.Any>>] object?[] args);

    [JSImport("call", Module)]
    [return: JSMarshalAs<JSType.Any>]
    private static partial object? CallRaw(
        [JSMarshalAs<JSType.Any>] object function,
        [JSMarshalAs<JSType.Array<JSType.Any>>] object?[] args);

    [JSImport("construct", Module)]
    [return: JSMarshalAs<JSType.Any>]
    private static partial object? ConstructRaw(
        [JSMarshalAs<JSType.Any>] object type,
        [JSMarshalAs<JSType.Array<JSType.Any>>] object?[] args);

    /// <summary>What the last bridge call that failed threw.</summary>
    [JSImport("lastError", Module)]
    [return: JSMarshalAs<JSType.Any>]
    private static partial object? LastError();

    /// <summary>A JavaScript function that runs the registered C# delegate <paramref name="id"/>.</summary>
    [JSImport("callback", Module)]
    internal static partial JSObject Callback(int resource, int id, bool returnsTask);

    [JSImport("newObject", Module)]
    internal static partial JSObject NewObject();

    [JSImport("newArray", Module)]
    internal static partial JSObject NewArray([JSMarshalAs<JSType.Array<JSType.Any>>] object?[] items);

    [JSImport("keys", Module)]
    internal static partial string[] Keys([JSMarshalAs<JSType.Any>] object target);

    [JSImport("isArray", Module)]
    internal static partial bool IsArray([JSMarshalAs<JSType.Any>] object? value);

    [JSImport("isPromise", Module)]
    internal static partial bool IsPromise([JSMarshalAs<JSType.Any>] object? value);

    [JSImport("length", Module)]
    internal static partial int Length([JSMarshalAs<JSType.Any>] object target);

    [JSImport("at", Module)]
    [return: JSMarshalAs<JSType.Any>]
    internal static partial object? At([JSMarshalAs<JSType.Any>] object target, int index);

    [JSImport("typeOf", Module)]
    internal static partial string TypeOf([JSMarshalAs<JSType.Any>] object? value);

    /// <summary>The name of the JavaScript class an object was made by ("" for none).</summary>
    [JSImport("className", Module)]
    internal static partial string ClassName([JSMarshalAs<JSType.Any>] object? value);

    [JSImport("text", Module)]
    internal static partial string Text([JSMarshalAs<JSType.Any>] object? value);

    [JSImport("same", Module)]
    internal static partial bool Same([JSMarshalAs<JSType.Any>] object? a, [JSMarshalAs<JSType.Any>] object? b);

    [JSImport("jsonParse", Module)]
    [return: JSMarshalAs<JSType.Any>]
    internal static partial object? JsonParse(string text);

    [JSImport("jsonStringify", Module)]
    internal static partial string JsonStringify([JSMarshalAs<JSType.Any>] object? value);
}
