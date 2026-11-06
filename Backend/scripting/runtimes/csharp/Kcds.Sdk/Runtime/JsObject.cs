using System;
using System.Collections.Generic;
using System.Reflection;
using System.Runtime.InteropServices.JavaScript;
using System.Threading.Tasks;

namespace Kcds;

/// <summary>
/// An SDK object (a player, a door, a Vector3): the C# face of the JavaScript object the SDK gave.
/// Its properties and methods read and call that object; two wrappers of one object are equal.
/// </summary>
public abstract class JsObject
{
    private static readonly Dictionary<Type, ConstructorInfo> Constructors = new();

    /// <summary>The JavaScript object this stands for.</summary>
    public JSObject Handle { get; }

    protected JsObject(JSObject handle)
    {
        Handle = handle;
    }

    // The SDK's classes by their JavaScript class name, for the most derived wrapper of an object.
    private static Dictionary<string, Type>? classes;

    private static Dictionary<string, Type> Classes()
    {
        if (classes == null)
        {
            classes = new Dictionary<string, Type>();
            foreach (var type in typeof(JsObject).Assembly.GetTypes())
            {
                if (type.IsSubclassOf(typeof(JsObject)) && !type.IsAbstract)
                {
                    classes[type.Name] = type;
                }
            }
        }
        return classes;
    }

    /// <summary>
    /// A wrapper for a JavaScript object, as <paramref name="type"/> or the SDK class below it that
    /// made the object: an Entity that is a player is a <c>Player</c> (<c>entity is Player</c>).
    /// </summary>
    public static object Wrap(Type type, JSObject handle)
    {
        if (Classes().TryGetValue(Runtime.Bridge.ClassName(handle), out var made) && made != type
            && type.IsAssignableFrom(made))
        {
            type = made;
        }
        if (!Constructors.TryGetValue(type, out var constructor))
        {
            constructor = type.GetConstructor(
                BindingFlags.Instance | BindingFlags.Public | BindingFlags.NonPublic,
                new[] { typeof(JSObject) })
                ?? throw new InvalidOperationException($"{type.Name} cannot wrap a JavaScript object");
            Constructors[type] = constructor;
        }
        return constructor.Invoke(new object[] { handle });
    }

    /// <summary>A wrapper of <typeparamref name="T"/> for a JavaScript object.</summary>
    public static T Wrap<T>(JSObject handle)
        where T : JsObject
    {
        return (T)Wrap(typeof(T), handle);
    }

    /// <summary>Reads a property of the JavaScript object by its JavaScript name.</summary>
    protected T GetMember<T>(string name)
    {
        return Runtime.Values.FromJs<T>(Runtime.Bridge.Get(Handle, name));
    }

    /// <summary>Writes a property of the JavaScript object by its JavaScript name.</summary>
    protected void SetMember(string name, object? value)
    {
        Runtime.Bridge.Set(Handle, name, Runtime.Values.ToJs(value));
    }

    /// <summary>Calls a method by its JavaScript name and answers its result as <typeparamref name="T"/>.</summary>
    protected T CallMember<T>(string name, params object?[] args)
    {
        return Runtime.Values.FromJs<T>(Runtime.Bridge.Invoke(Handle, name, Runtime.Values.ToJsArguments(args)));
    }

    /// <summary>Calls a method by its JavaScript name.</summary>
    protected void CallMember(string name, params object?[] args)
    {
        Runtime.Bridge.Invoke(Handle, name, Runtime.Values.ToJsArguments(args));
    }

    /// <summary>This object as a dynamic value, for members the typed API does not name.</summary>
    public JsValue AsValue()
    {
        return new JsValue(Handle);
    }

    public override bool Equals(object? other)
    {
        return other is JsObject wrapper && Runtime.Bridge.Same(Handle, wrapper.Handle);
    }

    public override int GetHashCode()
    {
        return GetType().GetHashCode();
    }

    /// <summary>The object's own text (its toString).</summary>
    public override string ToString()
    {
        return Runtime.Bridge.Text(Handle);
    }

    public static bool operator ==(JsObject? a, JsObject? b)
    {
        if (a is null || b is null)
        {
            return a is null && b is null;
        }
        return a.Equals(b);
    }

    public static bool operator !=(JsObject? a, JsObject? b)
    {
        return !(a == b);
    }
}

/// <summary>
/// A static part of the SDK (a namespace such as Events or Chat, or a class's statics), read from
/// the resource's <c>kcds</c> object by its path.
/// </summary>
public static class Sdk
{
    private static readonly Dictionary<string, object> Paths = new();

    /// <summary>Forgets the paths a stopped resource read.</summary>
    internal static void Forget(int resource)
    {
        var prefix = resource + ":";
        var gone = new List<string>();
        foreach (var key in Paths.Keys)
        {
            if (key.StartsWith(prefix, StringComparison.Ordinal))
            {
                gone.Add(key);
            }
        }
        foreach (var key in gone)
        {
            Paths.Remove(key);
        }
    }

    /// <summary>The JavaScript value at <c>kcds.&lt;domain&gt;.&lt;name&gt;</c> of the running resource.</summary>
    public static object At(string domain, string name)
    {
        var key = Runtime.Current.Id + ":" + domain + "." + name;
        if (!Paths.TryGetValue(key, out var value))
        {
            var domainObject = Runtime.Bridge.Get(Runtime.Bridge.Root(), domain)
                ?? throw new InvalidOperationException($"kcds.{domain} is not on this side");
            value = Runtime.Bridge.Get(domainObject, name)
                ?? throw new InvalidOperationException($"kcds.{domain}.{name} is not on this side");
            Paths[key] = value;
        }
        return value;
    }

    /// <summary>Calls <c>kcds.&lt;domain&gt;.&lt;name&gt;.&lt;member&gt;(...)</c>.</summary>
    public static T Call<T>(string domain, string name, string member, params object?[] args)
    {
        var raw = Runtime.Bridge.Invoke(At(domain, name), member, Runtime.Values.ToJsArguments(args));
        return Runtime.Values.FromJs<T>(raw);
    }

    /// <summary>Calls a member that answers nothing.</summary>
    public static void Call(string domain, string name, string member, params object?[] args)
    {
        Runtime.Bridge.Invoke(At(domain, name), member, Runtime.Values.ToJsArguments(args));
    }

    /// <summary>Reads <c>kcds.&lt;domain&gt;.&lt;name&gt;.&lt;member&gt;</c>.</summary>
    public static T Get<T>(string domain, string name, string member)
    {
        return Runtime.Values.FromJs<T>(Runtime.Bridge.Get(At(domain, name), member));
    }

    /// <summary>Writes <c>kcds.&lt;domain&gt;.&lt;name&gt;.&lt;member&gt;</c>.</summary>
    public static void Set(string domain, string name, string member, object? value)
    {
        Runtime.Bridge.Set(At(domain, name), member, Runtime.Values.ToJs(value));
    }

    /// <summary><c>new kcds.&lt;domain&gt;.&lt;name&gt;(...)</c>.</summary>
    public static JSObject Construct(string domain, string name, params object?[] args)
    {
        var made = Runtime.Bridge.Construct(At(domain, name), Runtime.Values.ToJsArguments(args));
        return (JSObject)made!;
    }

    /// <summary>A live value at <c>kcds.&lt;domain&gt;.&lt;name&gt;</c> (LocalPlayer), read each time.</summary>
    public static T Live<T>(string domain, string name)
    {
        var domainObject = Runtime.Bridge.Get(Runtime.Bridge.Root(), domain)!;
        return Runtime.Values.FromJs<T>(Runtime.Bridge.Get(domainObject, name));
    }

    /// <summary>An optional argument not given: JavaScript's undefined, so the member's default applies.</summary>
    public static object? Optional(object? value)
    {
        return value ?? Undefined;
    }

    /// <summary>JavaScript's undefined, as an argument.</summary>
    public static object Undefined => undefinedValue ??= Runtime.Bridge.Global("__kcds_undefined")!;

    /// <summary>
    /// JavaScript's null, as an argument. A C# null for an optional parameter means "not given", so
    /// the member's default applies; Sdk.Null is null itself there too, for a member that asks for it
    /// (<c>state.OnChange(Sdk.Null, handler)</c> watches every key).
    /// </summary>
    public static object Null { get; } = default(JsValue);

    private static object? undefinedValue;
}
