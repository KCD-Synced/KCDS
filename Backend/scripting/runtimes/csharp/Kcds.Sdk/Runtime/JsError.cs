using System;

namespace Kcds;

/// <summary>
/// A JavaScript error the SDK threw, or a promise it rejected with, in C#: its message, its name
/// (TypeError, RangeError, AggregateError, ...) and the error itself for anything else on it
/// (<c>problem.Error["errors"]</c> of an AggregateError).
/// </summary>
public sealed class JsError : Exception
{
    /// <summary>The JavaScript error (or whatever was thrown).</summary>
    public JsValue Error { get; }

    /// <summary>The error's name, "Error" when it has none.</summary>
    public string Name { get; }

    public JsError(JsValue error)
        : base(MessageOf(error))
    {
        Error = error;
        var name = error["name"];
        Name = name.IsNull ? "Error" : name.ToString();
    }

    private static string MessageOf(JsValue error)
    {
        var message = error["message"];
        return message.IsNull ? error.ToString() : message.ToString();
    }

    public override string ToString() => $"{Name}: {Message}";
}
