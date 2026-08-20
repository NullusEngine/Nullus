using System.Runtime.InteropServices;
using System.Text;

namespace Nullus;

[StructLayout(LayoutKind.Sequential)]
public struct ScriptAbiHeader
{
    public uint Size;
    public uint AbiVersion;
    public ulong SchemaHash;
}

[StructLayout(LayoutKind.Sequential)]
public struct ScriptAbiResult
{
    public byte Code;
    private byte Reserved0;
    private byte Reserved1;
    private byte Reserved2;
    private byte Reserved3;
    private byte Reserved4;
    private byte Reserved5;
    private byte Reserved6;
    public IntPtr Message;
}

[StructLayout(LayoutKind.Sequential)]
public struct ScriptAbiDiagnostic
{
    public uint Size;
    public uint Severity;
    public int Line;
    public int Column;
    public IntPtr SourcePath;
    public IntPtr Message;
    public IntPtr StackTrace;
}

// Must stay byte-for-byte compatible with Native::Scripting::ScriptAbiValue.
// Pointers are borrowed for the duration of the ABI call.
[StructLayout(LayoutKind.Sequential)]
public struct ScriptAbiValue
{
    public uint Kind;
    public uint Reserved;
    public ulong TypeId;
    public long SignedValue;
    public ulong UnsignedValue;
    public double FloatingValue;
    public ulong ObjectValue;
    public IntPtr Utf8Data;
    public uint Utf8Size;
    public IntPtr Bytes;
    public uint ByteSize;
}

[StructLayout(LayoutKind.Sequential)]
public struct ScriptFrameContext
{
    public float DeltaTime;
    public float UnscaledDeltaTime;
    public double Time;
    public ulong FrameIndex;
    public float FixedDeltaTime;
    public float TimeScale;
    public double UnscaledTime;
    public ulong FixedFrameIndex;
}

[StructLayout(LayoutKind.Sequential)]
public unsafe struct NativeApiTable
{
    public ScriptAbiHeader Header;
    public delegate* unmanaged<byte, byte*, void> Log;
    public delegate* unmanaged<ulong, byte> IsAlive;
    public delegate* unmanaged<ulong, ulong, ulong*, ScriptAbiResult> GetObject;
    public delegate* unmanaged<ulong, ulong, float*, float*, float*, ScriptAbiResult> GetVector3;
    public delegate* unmanaged<ulong, ulong, float, float, float, ScriptAbiResult> SetVector3;
    public delegate* unmanaged<ulong, ulong, byte, ScriptAbiResult> SetBool;
    public delegate* unmanaged<ulong, ulong, float*, float*, float*, float*, ScriptAbiResult> GetQuaternion;
    public delegate* unmanaged<ulong, ulong, float, float, float, float, ScriptAbiResult> SetQuaternion;
    public delegate* unmanaged<ulong, byte*, ulong*, ScriptAbiResult> CreatePrimitive;
    public delegate* unmanaged<ulong, ulong, byte**, uint*, ScriptAbiResult> GetString;
    public delegate* unmanaged<ulong, ulong, byte*, ScriptAbiResult> SetString;
    public delegate* unmanaged<ulong, ulong, byte*, ScriptAbiResult> GetBool;
    public delegate* unmanaged<ulong, ulong, ulong*, ScriptAbiResult> GetComponent;
    public delegate* unmanaged<ulong, ulong, int*, ScriptAbiResult> GetInt32;
    public delegate* unmanaged<ulong, ulong, int, ScriptAbiResult> SetInt32;
    public delegate* unmanaged<ulong, ulong, float*, ScriptAbiResult> GetFloat;
    public delegate* unmanaged<ulong, ulong, float, ScriptAbiResult> SetFloat;
    public delegate* unmanaged<ulong, float, ScriptAbiResult> Destroy;
    public delegate* unmanaged<ulong, ulong*, ScriptAbiResult> Instantiate;
    public delegate* unmanaged<byte*, ulong*, ScriptAbiResult> Find;
    public delegate* unmanaged<byte*, ulong*, ScriptAbiResult> FindWithTag;
    public delegate* unmanaged<ulong, ulong, ulong*, ScriptAbiResult> AddComponent;
    public delegate* unmanaged<ulong, ulong, ulong, ScriptAbiResult> SetObject;
    public delegate* unmanaged<int, byte*, ScriptAbiResult> GetKey;
    public delegate* unmanaged<int, byte*, ScriptAbiResult> GetKeyDown;
    public delegate* unmanaged<int, byte*, ScriptAbiResult> GetKeyUp;
    public delegate* unmanaged<int, byte*, ScriptAbiResult> GetMouseButton;
    public delegate* unmanaged<int, byte*, ScriptAbiResult> GetMouseButtonDown;
    public delegate* unmanaged<int, byte*, ScriptAbiResult> GetMouseButtonUp;
    public delegate* unmanaged<float*, float*, ScriptAbiResult> GetMousePosition;
    public delegate* unmanaged<float*, float*, ScriptAbiResult> GetMouseScrollDelta;
    public delegate* unmanaged<float*, ScriptAbiResult> GetTimeScale;
    public delegate* unmanaged<float, ScriptAbiResult> SetTimeScale;
    public delegate* unmanaged<byte*, byte*, ulong*, ScriptAbiResult> CreateGameObject;
    public delegate* unmanaged<byte**, uint*, byte*, ScriptAbiResult> GetActiveScene;
    public delegate* unmanaged<byte*, ScriptAbiResult> LoadScene;
}

public static class ScriptValueConversion
{
    public static bool TryConvert(object? value, Type targetType, out object? converted)
    {
        converted = null;
        if (value is null)
        {
            if (!targetType.IsValueType || Nullable.GetUnderlyingType(targetType) is not null)
                return true;
            return false;
        }
        if (targetType.IsInstanceOfType(value))
        {
            converted = value;
            return true;
        }
        if (targetType.IsEnum)
        {
            try
            {
                converted = Enum.ToObject(targetType, value);
                return true;
            }
            catch (Exception)
            {
                return false;
            }
        }
        if (targetType == typeof(NativeObjectHandle))
        {
            if (value is ulong unsigned)
            {
                converted = new NativeObjectHandle(unsigned);
                return true;
            }
            if (value is long signed && signed >= 0)
            {
                converted = new NativeObjectHandle((ulong)signed);
                return true;
            }
        }
        if (targetType == typeof(string))
        {
            converted = Convert.ToString(value, System.Globalization.CultureInfo.InvariantCulture);
            return converted is not null;
        }
        try
        {
            converted = Convert.ChangeType(value, targetType, System.Globalization.CultureInfo.InvariantCulture);
            return converted is not null;
        }
        catch (Exception)
        {
            return false;
        }
    }
}

public static unsafe class ScriptAbiCodec
{
    private const uint Null = 0;
    private const uint Bool = 1;
    private const uint Int8 = 2;
    private const uint UInt8 = 3;
    private const uint Int16 = 4;
    private const uint UInt16 = 5;
    private const uint Int32 = 6;
    private const uint UInt32 = 7;
    private const uint Int64 = 8;
    private const uint UInt64 = 9;
    private const uint Float = 10;
    private const uint Double = 11;
    private const uint String = 12;
    private const uint Enum = 13;
    private const uint Vector2 = 14;
    private const uint Vector3 = 15;
    private const uint Vector4 = 16;
    private const uint Quaternion = 17;
    private const uint Color = 18;
    private const uint ObjectReference = 19;

    public static bool TryEncode(object? value, ScriptAbiValue* output, List<IntPtr> buffers)
    {
        *output = default;
        if (value is null)
        {
            output->Kind = Null;
            return true;
        }
        switch (value)
        {
            case bool boolean: output->Kind = Bool; output->UnsignedValue = boolean ? 1UL : 0UL; return true;
            case sbyte signedByte: output->Kind = Int8; output->SignedValue = signedByte; return true;
            case byte unsignedByte: output->Kind = UInt8; output->UnsignedValue = unsignedByte; return true;
            case short signedShort: output->Kind = Int16; output->SignedValue = signedShort; return true;
            case ushort unsignedShort: output->Kind = UInt16; output->UnsignedValue = unsignedShort; return true;
            case int signedInt: output->Kind = Int32; output->SignedValue = signedInt; return true;
            case uint unsignedInt: output->Kind = UInt32; output->UnsignedValue = unsignedInt; return true;
            case long signedLong: output->Kind = Int64; output->SignedValue = signedLong; return true;
            case ulong unsignedLong: output->Kind = UInt64; output->UnsignedValue = unsignedLong; return true;
            case float single: output->Kind = Float; output->FloatingValue = single; return true;
            case double number: output->Kind = Double; output->FloatingValue = number; return true;
            case string text:
                output->Kind = String;
                output->Utf8Data = Marshal.StringToCoTaskMemUTF8(text);
                output->Utf8Size = (uint)Encoding.UTF8.GetByteCount(text);
                buffers.Add(output->Utf8Data);
                return true;
            case NativeObjectHandle handle:
                output->Kind = ObjectReference;
                output->ObjectValue = handle.Value;
                return true;
            case Vector2 vector: return StoreStruct(Vector2, vector, output, buffers);
            case Vector3 vector: return StoreStruct(Vector3, vector, output, buffers);
            case Vector4 vector: return StoreStruct(Vector4, vector, output, buffers);
            case Quaternion quaternion: return StoreStruct(Quaternion, quaternion, output, buffers);
            case Color color: return StoreStruct(Color, color, output, buffers);
            default:
                if (value.GetType().IsEnum)
                {
                    output->Kind = Enum;
                    output->SignedValue = Convert.ToInt64(value, System.Globalization.CultureInfo.InvariantCulture);
                    return true;
                }
                return false;
        }
    }

    public static bool TryDecode(ScriptAbiValue input, out object? value)
    {
        value = null;
        switch (input.Kind)
        {
            case Null: return true;
            case Bool: value = input.UnsignedValue != 0; return true;
            case Int8: value = (sbyte)input.SignedValue; return true;
            case UInt8: value = (byte)input.UnsignedValue; return true;
            case Int16: value = (short)input.SignedValue; return true;
            case UInt16: value = (ushort)input.UnsignedValue; return true;
            case Int32: value = (int)input.SignedValue; return true;
            case UInt32: value = (uint)input.UnsignedValue; return true;
            case Int64: value = input.SignedValue; return true;
            case UInt64: value = input.UnsignedValue; return true;
            case Float: value = (float)input.FloatingValue; return true;
            case Double: value = input.FloatingValue; return true;
            case String:
                value = input.Utf8Data == IntPtr.Zero ? string.Empty : Marshal.PtrToStringUTF8(input.Utf8Data, (int)input.Utf8Size);
                return value is not null;
            case Enum: value = input.SignedValue; return true;
            case ObjectReference: value = new NativeObjectHandle(input.ObjectValue); return true;
            case Vector2: return TryReadStruct(input, out value, typeof(Vector2));
            case Vector3: return TryReadStruct(input, out value, typeof(Vector3));
            case Vector4: return TryReadStruct(input, out value, typeof(Vector4));
            case Quaternion: return TryReadStruct(input, out value, typeof(Quaternion));
            case Color: return TryReadStruct(input, out value, typeof(Color));
            default: return false;
        }
    }

    private static bool StoreStruct<T>(uint kind, T value, ScriptAbiValue* output, List<IntPtr> buffers)
        where T : struct
    {
        var pointer = Marshal.AllocCoTaskMem(Marshal.SizeOf<T>());
        Marshal.StructureToPtr(value, pointer, false);
        buffers.Add(pointer);
        output->Kind = kind;
        output->Bytes = pointer;
        output->ByteSize = (uint)Marshal.SizeOf<T>();
        return true;
    }

    private static bool TryReadStruct(ScriptAbiValue input, out object? value, Type type)
    {
        value = null;
        if (input.Bytes == IntPtr.Zero || input.ByteSize != (uint)Marshal.SizeOf(type))
            return false;
        value = Marshal.PtrToStructure(input.Bytes, type);
        return value is not null;
    }
}
