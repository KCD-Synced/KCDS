using System.Collections;
using System.Collections.Generic;
using System.Runtime.InteropServices.JavaScript;

namespace Kcds;

/// <summary>
/// A JavaScript value the typed API gives no type for: an event payload, an options object, a
/// list. Read it with an indexer (<c>payload["text"]</c>, <c>list[0]</c>) and convert it with
/// <see cref="As{T}"/> or the As methods. Lists index from 0, as in JavaScript.
/// </summary>
public readonly struct JsValue : IEnumerable<JsValue>
{
    /// <summary>The value as the host gave it: null, a bool, a double, a string or a JavaScript object.</summary>
    public object? Raw { get; }

    public JsValue(object? raw)
    {
        Raw = raw is JsValue nested ? nested.Raw : raw;
    }

    /// <summary>Whether it is JavaScript's null or undefined.</summary>
    public bool IsNull => Raw == null;

    /// <summary>Whether it is a JavaScript array.</summary>
    public bool IsArray => Runtime.Bridge.IsArray(Raw);

    /// <summary>JavaScript's typeof of it ("object", "string", "number", ...).</summary>
    public string Type => Runtime.Bridge.TypeOf(Raw);

    /// <summary>
    /// A property, or nothing (a null value) when it is not an object. Setting it writes the
    /// JavaScript object's property (<c>fall["direction"] = new Vector3(0, 1, 0)</c>).
    /// </summary>
    public JsValue this[string name]
    {
        get => Raw is JSObject handle ? new JsValue(Runtime.Bridge.Get(handle, name)) : default;
        set => Runtime.Bridge.Set(Object(), name, Runtime.Values.ToJs(value));
    }

    /// <summary>An element of an array, from 0; setting it writes that element.</summary>
    public JsValue this[int index]
    {
        get => Raw is JSObject handle ? new JsValue(Runtime.Bridge.At(handle, index)) : default;
        set => Runtime.Bridge.Set(Object(), index.ToString(System.Globalization.CultureInfo.InvariantCulture), Runtime.Values.ToJs(value));
    }

    /// <summary>Writes a property to any value C# has (<c>row.Set("amount", 2)</c>).</summary>
    public void Set(string name, object? value)
    {
        Runtime.Bridge.Set(Object(), name, Runtime.Values.ToJs(value));
    }

    private JSObject Object()
    {
        return Raw as JSObject ?? throw new System.InvalidOperationException("Not an object");
    }

    /// <summary>The length of an array.</summary>
    public int Length => Raw is JSObject handle ? Runtime.Bridge.Length(handle) : 0;

    /// <summary>The names of an object's own properties.</summary>
    public IReadOnlyList<string> Keys => Raw is JSObject handle ? Runtime.Bridge.Keys(handle) : System.Array.Empty<string>();

    /// <summary>Calls a method of it.</summary>
    public JsValue Call(string method, params object?[] args)
    {
        var handle = Raw as JSObject ?? throw new System.InvalidOperationException("Not an object");
        return new JsValue(Runtime.Bridge.Invoke(handle, method, Runtime.Values.ToJsArguments(args)));
    }

    /// <summary>Whether it is the very same JavaScript value as <paramref name="other"/> (===).</summary>
    public bool Is(JsValue other) => Runtime.Bridge.Same(Raw, other.Raw);

    /// <summary>Calls it, when it is a function (the stop function Events.On or OnChange answers).</summary>
    public JsValue Invoke(params object?[] args)
    {
        var function = Raw ?? throw new System.InvalidOperationException("Not a function");
        return new JsValue(Runtime.Bridge.Call(function, Runtime.Values.ToJsArguments(args)));
    }

    /// <summary>It as <typeparamref name="T"/> (a number, text, an SDK type, a list, a record).</summary>
    public T As<T>()
    {
        return Runtime.Values.FromJs<T>(Raw);
    }

    public string? AsString() => IsNull ? null : As<string>();

    public double AsNumber() => As<double>();

    public int AsInt() => As<int>();

    public bool AsBool() => As<bool>();

    /// <summary>It as JSON text.</summary>
    public string ToJson() => Runtime.Bridge.JsonStringify(Raw);

    /// <summary>A value from JSON text.</summary>
    public static JsValue FromJson(string json) => new(Runtime.Bridge.JsonParse(json));

    public override string ToString() => IsNull ? "null" : Runtime.Bridge.Text(Raw);

    public IEnumerator<JsValue> GetEnumerator()
    {
        var length = Length;
        for (var index = 0; index < length; index++)
        {
            yield return this[index];
        }
    }

    IEnumerator IEnumerable.GetEnumerator() => GetEnumerator();

    public static implicit operator double(JsValue value) => value.AsNumber();

    public static implicit operator bool(JsValue value) => !value.IsNull && value.AsBool();
}
