using System.Buffers.Binary;
using System.Runtime.InteropServices;
using System.Text;

namespace Nullus;

[StructLayout(LayoutKind.Sequential)]
public struct AnimationCurveKeyframe(
    float time,
    float value,
    float inTangent,
    float outTangent,
    float inWeight = 1.0f,
    float outWeight = 1.0f,
    int weightedMode = 0)
{
    public float Time = time;
    public float Value = value;
    public float InTangent = inTangent;
    public float OutTangent = outTangent;
    public float InWeight = inWeight;
    public float OutWeight = outWeight;
    public int WeightedMode = weightedMode;

    public float time { readonly get => Time; set => Time = value; }
    public float value { readonly get => Value; set => Value = value; }
    public float inTangent { readonly get => InTangent; set => InTangent = value; }
    public float outTangent { readonly get => OutTangent; set => OutTangent = value; }
    public float inWeight { readonly get => InWeight; set => InWeight = value; }
    public float outWeight { readonly get => OutWeight; set => OutWeight = value; }
    public int weightedMode { readonly get => WeightedMode; set => WeightedMode = value; }
}

public sealed class AnimationCurve
{
    public AnimationCurveKeyframe[] Keys { get; set; } = Array.Empty<AnimationCurveKeyframe>();
    public int PreWrapMode { get; set; }
    public int PostWrapMode { get; set; }

    public AnimationCurve() { }

    public AnimationCurve(IEnumerable<AnimationCurveKeyframe> keys, int preWrapMode = 0, int postWrapMode = 0)
    {
        Keys = keys?.ToArray() ?? throw new ArgumentNullException(nameof(keys));
        PreWrapMode = preWrapMode;
        PostWrapMode = postWrapMode;
    }

    public AnimationCurveKeyframe[] keys { get => Keys; set => Keys = value ?? Array.Empty<AnimationCurveKeyframe>(); }
    public int preWrapMode { get => PreWrapMode; set => PreWrapMode = value; }
    public int postWrapMode { get => PostWrapMode; set => PostWrapMode = value; }
}

[StructLayout(LayoutKind.Sequential)]
public struct GradientColorKey(Color color, float time)
{
    public Color Color = color;
    public float Time = time;
    public Color color { readonly get => Color; set => Color = value; }
    public float time { readonly get => Time; set => Time = value; }
}

[StructLayout(LayoutKind.Sequential)]
public struct GradientAlphaKey(float alpha, float time)
{
    public float Alpha = alpha;
    public float Time = time;
    public float alpha { readonly get => Alpha; set => Alpha = value; }
    public float time { readonly get => Time; set => Time = value; }
}

public sealed class Gradient
{
    public GradientColorKey[] ColorKeys { get; set; } = Array.Empty<GradientColorKey>();
    public GradientAlphaKey[] AlphaKeys { get; set; } = Array.Empty<GradientAlphaKey>();
    public int Mode { get; set; }

    public Gradient() { }

    public Gradient(
        IEnumerable<GradientColorKey> colorKeys,
        IEnumerable<GradientAlphaKey> alphaKeys,
        int mode = 0)
    {
        ColorKeys = colorKeys?.ToArray() ?? throw new ArgumentNullException(nameof(colorKeys));
        AlphaKeys = alphaKeys?.ToArray() ?? throw new ArgumentNullException(nameof(alphaKeys));
        Mode = mode;
    }

    public GradientColorKey[] colorKeys { get => ColorKeys; set => ColorKeys = value ?? Array.Empty<GradientColorKey>(); }
    public GradientAlphaKey[] alphaKeys { get => AlphaKeys; set => AlphaKeys = value ?? Array.Empty<GradientAlphaKey>(); }
    public int mode { get => Mode; set => Mode = value; }
}

public sealed class ExposedReference<T> where T : NativeObject
{
    public T? DefaultValue { get; set; }
    public string ExposedName { get; set; } = string.Empty;

    public T? defaultValue { get => DefaultValue; set => DefaultValue = value; }
    public string exposedName { get => ExposedName; set => ExposedName = value ?? string.Empty; }
}

// Public because Nullus.Editor is a separate editor-only assembly that wraps
// the managed value types while keeping the native byte ABI opaque.
public static class SerializedSpecialValueCodec
{
    private const int MaxElementCount = 1_000_000;

    public static byte[] Encode(AnimationCurve value)
    {
        ArgumentNullException.ThrowIfNull(value);
        var keys = value.Keys ?? Array.Empty<AnimationCurveKeyframe>();
        var bytes = new byte[checked(4 + keys.Length * 28 + 8)];
        var offset = 0;
        BinaryPrimitives.WriteUInt32LittleEndian(bytes.AsSpan(offset, 4), checked((uint)keys.Length));
        offset += 4;
        foreach (var key in keys)
        {
            WriteSingle(bytes, ref offset, key.Time);
            WriteSingle(bytes, ref offset, key.Value);
            WriteSingle(bytes, ref offset, key.InTangent);
            WriteSingle(bytes, ref offset, key.OutTangent);
            WriteSingle(bytes, ref offset, key.InWeight);
            WriteSingle(bytes, ref offset, key.OutWeight);
            BinaryPrimitives.WriteInt32LittleEndian(bytes.AsSpan(offset, 4), key.WeightedMode);
            offset += 4;
        }
        BinaryPrimitives.WriteInt32LittleEndian(bytes.AsSpan(offset, 4), value.PreWrapMode);
        offset += 4;
        BinaryPrimitives.WriteInt32LittleEndian(bytes.AsSpan(offset, 4), value.PostWrapMode);
        return bytes;
    }

    public static bool TryDecodeAnimationCurve(ReadOnlySpan<byte> bytes, out AnimationCurve value)
    {
        value = new AnimationCurve();
        var offset = 0;
        if (!TryReadCount(bytes, ref offset, out var count) || count > MaxElementCount)
            return false;
        if (count > (bytes.Length - offset - 8) / 28)
            return false;
        var keys = new AnimationCurveKeyframe[count];
        for (var index = 0; index < count; ++index)
        {
            if (!TryReadSingle(bytes, ref offset, out var time) ||
                !TryReadSingle(bytes, ref offset, out var keyValue) ||
                !TryReadSingle(bytes, ref offset, out var inTangent) ||
                !TryReadSingle(bytes, ref offset, out var outTangent) ||
                !TryReadSingle(bytes, ref offset, out var inWeight) ||
                !TryReadSingle(bytes, ref offset, out var outWeight) ||
                !TryReadInt32(bytes, ref offset, out var weightedMode))
                return false;
            keys[index] = new AnimationCurveKeyframe(time, keyValue, inTangent, outTangent, inWeight, outWeight, weightedMode);
        }
        if (!TryReadInt32(bytes, ref offset, out var preWrapMode) ||
            !TryReadInt32(bytes, ref offset, out var postWrapMode) ||
            offset != bytes.Length)
            return false;
        value = new AnimationCurve(keys, preWrapMode, postWrapMode);
        return true;
    }

    public static byte[] Encode(Gradient value)
    {
        ArgumentNullException.ThrowIfNull(value);
        var colors = value.ColorKeys ?? Array.Empty<GradientColorKey>();
        var alphas = value.AlphaKeys ?? Array.Empty<GradientAlphaKey>();
        var bytes = new byte[checked(4 + colors.Length * 20 + 4 + alphas.Length * 8 + 4)];
        var offset = 0;
        BinaryPrimitives.WriteUInt32LittleEndian(bytes.AsSpan(offset, 4), checked((uint)colors.Length));
        offset += 4;
        foreach (var key in colors)
        {
            WriteSingle(bytes, ref offset, key.Color.R);
            WriteSingle(bytes, ref offset, key.Color.G);
            WriteSingle(bytes, ref offset, key.Color.B);
            WriteSingle(bytes, ref offset, key.Color.A);
            WriteSingle(bytes, ref offset, key.Time);
        }
        BinaryPrimitives.WriteUInt32LittleEndian(bytes.AsSpan(offset, 4), checked((uint)alphas.Length));
        offset += 4;
        foreach (var key in alphas)
        {
            WriteSingle(bytes, ref offset, key.Alpha);
            WriteSingle(bytes, ref offset, key.Time);
        }
        BinaryPrimitives.WriteInt32LittleEndian(bytes.AsSpan(offset, 4), value.Mode);
        return bytes;
    }

    public static bool TryDecodeGradient(ReadOnlySpan<byte> bytes, out Gradient value)
    {
        value = new Gradient();
        var offset = 0;
        if (!TryReadCount(bytes, ref offset, out var colorCount) || colorCount > MaxElementCount ||
            colorCount > (bytes.Length - offset - 8) / 20)
            return false;
        var colors = new GradientColorKey[colorCount];
        for (var index = 0; index < colorCount; ++index)
        {
            if (!TryReadSingle(bytes, ref offset, out var r) ||
                !TryReadSingle(bytes, ref offset, out var g) ||
                !TryReadSingle(bytes, ref offset, out var b) ||
                !TryReadSingle(bytes, ref offset, out var a) ||
                !TryReadSingle(bytes, ref offset, out var time))
                return false;
            colors[index] = new GradientColorKey(new Color(r, g, b, a), time);
        }
        if (!TryReadCount(bytes, ref offset, out var alphaCount) || alphaCount > MaxElementCount ||
            alphaCount > (bytes.Length - offset - 4) / 8)
            return false;
        var alphas = new GradientAlphaKey[alphaCount];
        for (var index = 0; index < alphaCount; ++index)
        {
            if (!TryReadSingle(bytes, ref offset, out var alpha) ||
                !TryReadSingle(bytes, ref offset, out var time))
                return false;
            alphas[index] = new GradientAlphaKey(alpha, time);
        }
        if (!TryReadInt32(bytes, ref offset, out var mode) || offset != bytes.Length)
            return false;
        value = new Gradient(colors, alphas, mode);
        return true;
    }

    public static byte[] Encode<T>(ExposedReference<T> value) where T : NativeObject
    {
        ArgumentNullException.ThrowIfNull(value);
        var nameBytes = Encoding.UTF8.GetBytes(value.ExposedName ?? string.Empty);
        var alignedLength = checked((nameBytes.Length + 4 + 3) & ~3);
        var bytes = new byte[checked(4 + alignedLength)];
        // Editor serialized references use the stable native handle bridge.  A
        // managed NativeObject intentionally does not expose a runtime-only
        // GetInstanceID method; its Handle is the canonical value shared with
        // the native editor ABI.
        var instanceId = value.DefaultValue switch
        {
            null => 0,
            global::Nullus.Object objectValue => unchecked((int)objectValue.GetInstanceID()),
            _ => unchecked((int)(value.DefaultValue.Handle.Value & 0xffffffffUL))
        };
        BinaryPrimitives.WriteInt32LittleEndian(bytes.AsSpan(0, 4), instanceId);
        BinaryPrimitives.WriteUInt32LittleEndian(bytes.AsSpan(4, 4), checked((uint)nameBytes.Length));
        nameBytes.CopyTo(bytes.AsSpan(8));
        return bytes;
    }

    public static bool TryDecodeExposedReference<T>(ReadOnlySpan<byte> bytes, out ExposedReference<T> value) where T : NativeObject
    {
        value = new ExposedReference<T>();
        if (bytes.Length < 8)
            return false;
        var instanceId = BinaryPrimitives.ReadInt32LittleEndian(bytes[..4]);
        var nameLength = BinaryPrimitives.ReadUInt32LittleEndian(bytes.Slice(4, 4));
        if (nameLength > (uint)int.MaxValue || nameLength > (uint)(bytes.Length - 8))
            return false;
        var end = checked(8 + (int)nameLength);
        var alignedEnd = (end + 3) & ~3;
        if (alignedEnd != bytes.Length)
            return false;
        T? reference = null;
        if (instanceId != 0)
        {
            try
            {
                reference = NativeObject.FromHandle<T>(new NativeObjectHandle(unchecked((ulong)(uint)instanceId)));
            }
            catch
            {
                return false;
            }
        }
        value.DefaultValue = reference;
        value.ExposedName = Encoding.UTF8.GetString(bytes.Slice(8, (int)nameLength));
        return true;
    }

    private static void WriteSingle(byte[] bytes, ref int offset, float value)
    {
        BinaryPrimitives.WriteInt32LittleEndian(bytes.AsSpan(offset, 4), BitConverter.SingleToInt32Bits(value));
        offset += 4;
    }

    private static bool TryReadSingle(ReadOnlySpan<byte> bytes, ref int offset, out float value)
    {
        value = 0.0f;
        if (offset < 0 || offset > bytes.Length - 4)
            return false;
        value = BitConverter.Int32BitsToSingle(BinaryPrimitives.ReadInt32LittleEndian(bytes.Slice(offset, 4)));
        offset += 4;
        return true;
    }

    private static bool TryReadInt32(ReadOnlySpan<byte> bytes, ref int offset, out int value)
    {
        value = 0;
        if (offset < 0 || offset > bytes.Length - 4)
            return false;
        value = BinaryPrimitives.ReadInt32LittleEndian(bytes.Slice(offset, 4));
        offset += 4;
        return true;
    }

    private static bool TryReadCount(ReadOnlySpan<byte> bytes, ref int offset, out int count)
    {
        count = 0;
        if (!TryReadInt32(bytes, ref offset, out var raw) || raw < 0)
            return false;
        count = raw;
        return true;
    }
}
