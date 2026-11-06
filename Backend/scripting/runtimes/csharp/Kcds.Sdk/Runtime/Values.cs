using System;
using System.Collections;
using System.Collections.Generic;
using System.Reflection;
using System.Runtime.InteropServices.JavaScript;
using System.Threading.Tasks;

namespace Kcds.Runtime;

/// <summary>
/// Values between C# and the JavaScript SDK. To JavaScript: numbers, text and true/false as
/// themselves; null as null; an SDK object as the JavaScript object it stands for; a delegate as a
/// function; a dictionary, an anonymous object or a record as an object (its property names start
/// lower-case: <c>new { Author = "Server" }</c> is <c>{ author: "Server" }</c>); a list or array as an
/// array. From JavaScript: into the type the C# side asks for.
/// </summary>
public static class Values
{
    /// <summary>A C# value as the host takes it.</summary>
    public static object? ToJs(object? value)
    {
        switch (value)
        {
            case null:
                return null;
            case string or bool or double:
                return value;
            case JsObject wrapper:
                return wrapper.Handle;
            case JsValue raw:
                return raw.Raw;
            case JSObject handle:
                return handle;
            case Delegate function:
                return Callbacks.ToFunction(function);
            case Task task:
                // A promise of the task's result.
                return Outcome(task);
            case Enum named:
                return named.ToString();
            case IConvertible number when IsNumber(value):
                return number.ToDouble(null);
            case IDictionary dictionary:
                return DictionaryToJs(dictionary);
            case IEnumerable list:
                return ListToJs(list);
            default:
                return ObjectToJs(value);
        }
    }

    /// <summary>The first arguments followed by the rest (a params array).</summary>
    public static object?[] Concat(object?[] first, object?[] rest)
    {
        var all = new object?[first.Length + rest.Length];
        first.CopyTo(all, 0);
        rest.CopyTo(all, first.Length);
        return all;
    }

    /// <summary>Several C# values as call arguments.</summary>
    public static object?[] ToJsArguments(object?[] values)
    {
        var converted = new object?[values.Length];
        for (var index = 0; index < values.Length; index++)
        {
            converted[index] = ToJs(values[index]);
        }
        return converted;
    }

    /// <summary>A value from JavaScript as <paramref name="type"/>.</summary>
    public static object? FromJs(object? raw, Type type)
    {
        if (type == typeof(void))
        {
            return null;
        }
        var nullable = Nullable.GetUnderlyingType(type);
        if (nullable != null)
        {
            return raw == null ? null : FromJs(raw, nullable);
        }
        if (type == typeof(JsValue))
        {
            return new JsValue(raw);
        }
        if (type == typeof(object))
        {
            return raw is JSObject ? new JsValue(raw) : raw;
        }
        if (raw == null)
        {
            return type.IsValueType ? Activator.CreateInstance(type) : null;
        }
        if (type == typeof(string))
        {
            return raw as string ?? Bridge.Text(raw);
        }
        if (type == typeof(bool))
        {
            return raw is bool flag ? flag : Convert.ToBoolean(raw);
        }
        if (IsNumberType(type))
        {
            return Convert.ChangeType(raw, type);
        }
        if (type.IsEnum)
        {
            return Enum.Parse(type, Bridge.Text(raw), ignoreCase: true);
        }
        if (typeof(JsObject).IsAssignableFrom(type))
        {
            return raw is JSObject handle ? JsObject.Wrap(type, handle) : null;
        }
        if (type.IsArray)
        {
            return ArrayFromJs(raw, type.GetElementType()!);
        }
        if (type.IsGenericType && IsListType(type.GetGenericTypeDefinition()))
        {
            var element = type.GetGenericArguments()[0];
            var array = ArrayFromJs(raw, element);
            var list = (IList)Activator.CreateInstance(typeof(List<>).MakeGenericType(element))!;
            foreach (var item in array)
            {
                list.Add(item);
            }
            return list;
        }
        if (type.IsGenericType && type.GetGenericTypeDefinition() == typeof(Task<>))
        {
            return TaskFromJs(raw, type.GetGenericArguments()[0]);
        }
        if (type == typeof(Task))
        {
            return raw is Task<object?> outcome ? Bridge.Settle(outcome) : Task.CompletedTask;
        }
        if (raw is JSObject json && !type.IsPrimitive)
        {
            // A plain data shape (a record or class with properties): from its JSON.
            return System.Text.Json.JsonSerializer.Deserialize(Bridge.JsonStringify(json), type, JsonOptions);
        }
        return Convert.ChangeType(raw, type);
    }

    /// <summary>A value from JavaScript as <typeparamref name="T"/>.</summary>
    public static T FromJs<T>(object? raw)
    {
        return (T)FromJs(raw, typeof(T))!;
    }

    internal static readonly System.Text.Json.JsonSerializerOptions JsonOptions = new()
    {
        PropertyNameCaseInsensitive = true,
        PropertyNamingPolicy = System.Text.Json.JsonNamingPolicy.CamelCase,
    };

    private static bool IsNumber(object value)
    {
        return IsNumberType(value.GetType());
    }

    private static bool IsNumberType(Type type)
    {
        return type == typeof(int)
            || type == typeof(long)
            || type == typeof(short)
            || type == typeof(byte)
            || type == typeof(sbyte)
            || type == typeof(uint)
            || type == typeof(ulong)
            || type == typeof(ushort)
            || type == typeof(float)
            || type == typeof(double)
            || type == typeof(decimal);
    }

    private static bool IsListType(Type definition)
    {
        return definition == typeof(List<>)
            || definition == typeof(IList<>)
            || definition == typeof(IReadOnlyList<>)
            || definition == typeof(IEnumerable<>)
            || definition == typeof(ICollection<>)
            || definition == typeof(IReadOnlyCollection<>);
    }

    private static Array ArrayFromJs(object raw, Type element)
    {
        var length = Bridge.Length(raw);
        var array = Array.CreateInstance(element, length);
        for (var index = 0; index < length; index++)
        {
            array.SetValue(FromJs(Bridge.At(raw, index), element), index);
        }
        return array;
    }

    private static object TaskFromJs(object raw, Type result)
    {
        var method = typeof(Values).GetMethod(nameof(Await), BindingFlags.NonPublic | BindingFlags.Static)!;
        return method.MakeGenericMethod(result).Invoke(null, new[] { raw })!;
    }

    private static async Task<object?> Outcome(Task task)
    {
        await task;
        var type = task.GetType();
        if (!type.IsGenericType)
        {
            return null;
        }
        var result = type.GetProperty("Result")!;
        return result.PropertyType.Name == "VoidTaskResult" ? null : ToJs(result.GetValue(task));
    }

    private static async Task<T> Await<T>(object raw)
    {
        var value = raw is Task<object?> outcome ? await Bridge.Settle(outcome) : raw;
        return FromJs<T>(value);
    }

    private static JSObject DictionaryToJs(IDictionary dictionary)
    {
        var target = Bridge.NewObject();
        foreach (DictionaryEntry entry in dictionary)
        {
            Bridge.Set(target, Convert.ToString(entry.Key)!, ToJs(entry.Value));
        }
        return target;
    }

    private static JSObject ListToJs(IEnumerable list)
    {
        var items = new List<object?>();
        foreach (var item in list)
        {
            items.Add(ToJs(item));
        }
        return Bridge.NewArray(items.ToArray());
    }

    private static JSObject ObjectToJs(object value)
    {
        var target = Bridge.NewObject();
        foreach (var property in value.GetType().GetProperties(BindingFlags.Public | BindingFlags.Instance))
        {
            if (property.GetIndexParameters().Length > 0)
            {
                continue;
            }
            var propertyValue = property.GetValue(value);
            if (propertyValue == null)
            {
                // A property left null is not given, as an absent JavaScript property is.
                continue;
            }
            Bridge.Set(target, LowerFirst(property.Name), ToJs(propertyValue));
        }
        return target;
    }

    internal static string LowerFirst(string name)
    {
        if (name.Length == 0 || char.IsLower(name[0]))
        {
            return name;
        }
        return char.ToLowerInvariant(name[0]) + name.Substring(1);
    }
}
