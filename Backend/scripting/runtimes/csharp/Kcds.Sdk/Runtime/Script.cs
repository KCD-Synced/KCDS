using System;
using System.IO;
using System.Text;

namespace Kcds;

/// <summary>
/// A C# resource's entry: every public class of the resource's assembly that derives from Script
/// is made once when the resource starts, and its <see cref="OnStart"/> runs, in the order the
/// classes are declared. Handlers it adds and timers it sets go away with the resource.
/// </summary>
public abstract class Script
{
    /// <summary>Runs when the resource starts.</summary>
    public virtual void OnStart()
    {
    }
}

/// <summary>The resource's log: what a JavaScript resource writes with console.log.</summary>
public static class Log
{
    public static void Info(object? value) => Runtime.Bridge.Log("info", Text(value));

    public static void Warn(object? value) => Runtime.Bridge.Log("warn", Text(value));

    public static void Error(object? value) => Runtime.Bridge.Log("error", Text(value));

    private static string Text(object? value)
    {
        return value switch
        {
            null => "null",
            bool flag => flag ? "true" : "false",
            double number => number.ToString(System.Globalization.CultureInfo.InvariantCulture),
            float number => number.ToString(System.Globalization.CultureInfo.InvariantCulture),
            _ => value.ToString() ?? "",
        };
    }
}

/// <summary>Console.WriteLine of a C# resource, to its log.</summary>
internal sealed class LogWriter : TextWriter
{
    private readonly StringBuilder line = new();

    public override Encoding Encoding => Encoding.UTF8;

    public override void Write(char value)
    {
        if (value == '\n')
        {
            Runtime.Bridge.Log("info", line.ToString().TrimEnd('\r'));
            line.Clear();
            return;
        }
        line.Append(value);
    }

    public override void Flush()
    {
        if (line.Length > 0)
        {
            Runtime.Bridge.Log("info", line.ToString());
            line.Clear();
        }
    }
}
