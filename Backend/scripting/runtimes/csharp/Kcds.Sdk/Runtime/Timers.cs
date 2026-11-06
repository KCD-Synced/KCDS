using System;
using System.Threading.Tasks;

namespace Kcds;

/// <summary>
/// The resource's timers: the same setTimeout and setInterval a JavaScript resource has, so
/// stopping or restarting the resource cancels them. A handle is what the clear calls take.
/// </summary>
public static class Timers
{
    /// <summary>Runs <paramref name="callback"/> once after <paramref name="milliseconds"/>.</summary>
    public static JsValue SetTimeout(Delegate callback, double milliseconds) => Run("setTimeout", callback, milliseconds);

    /// <summary>Runs <paramref name="callback"/> every <paramref name="milliseconds"/>.</summary>
    public static JsValue SetInterval(Delegate callback, double milliseconds) => Run("setInterval", callback, milliseconds);

    /// <summary>Cancels a timeout.</summary>
    public static void ClearTimeout(JsValue handle) => Clear("clearTimeout", handle);

    /// <summary>Cancels an interval.</summary>
    public static void ClearInterval(JsValue handle) => Clear("clearInterval", handle);

    /// <summary>A task that completes after <paramref name="milliseconds"/>, on the resource's own timers.</summary>
    public static Task Delay(double milliseconds)
    {
        var done = new TaskCompletionSource();
        SetTimeout(new Action(() => done.TrySetResult()), milliseconds);
        return done.Task;
    }

    private static JsValue Run(string name, Delegate callback, double milliseconds)
    {
        var timer = Runtime.Bridge.Global(name) ?? throw new InvalidOperationException($"{name} is not on this side");
        return new JsValue(Runtime.Bridge.Call(timer, new[] { Runtime.Values.ToJs(callback), milliseconds }));
    }

    private static void Clear(string name, JsValue handle)
    {
        var clear = Runtime.Bridge.Global(name)!;
        Runtime.Bridge.Call(clear, new[] { handle.Raw });
    }
}

/// <summary>The web platform's calls a server resource has (fetch), as for JavaScript.</summary>
public static class Network
{
    /// <summary>
    /// fetch(url, options): the resource's own fetch, with its limits; the response as a value
    /// (<c>response["status"]</c>, <c>await Network.Json(response)</c>).
    /// </summary>
    public static Task<JsValue> Fetch(string url, object? options = null)
    {
        var fetch = Runtime.Bridge.Global("fetch") ?? throw new InvalidOperationException("fetch is not on this side");
        var promise = Runtime.Bridge.Call(fetch, new[] { url, options == null ? Sdk.Undefined : Runtime.Values.ToJs(options) });
        return Runtime.Values.FromJs<Task<JsValue>>(promise);
    }

    /// <summary>A response's body read as JSON.</summary>
    public static Task<JsValue> Json(JsValue response) => response.Call("json").As<Task<JsValue>>();

    /// <summary>A response's body read as text.</summary>
    public static Task<string> Text(JsValue response) => response.Call("text").As<Task<string>>();
}
