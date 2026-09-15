using System.Collections;
using System.Collections.Concurrent;
using System.Diagnostics;
using System.Runtime.InteropServices;
using System.Text;

namespace Nullus.Editor;

// the established editor baseline-compatible values. Keep the numeric values in sync with the
// native Editor/Serialization core; extensions use the exact type/numeric
// metadata instead of adding entries to this enum.
public enum SerializedPropertyType
{
    Generic = -1,
    Integer = 0,
    Boolean = 1,
    Float = 2,
    String = 3,
    Color = 4,
    ObjectReference = 5,
    LayerMask = 6,
    Enum = 7,
    Vector2 = 8,
    Vector3 = 9,
    Vector4 = 10,
    Rect = 11,
    ArraySize = 12,
    Character = 13,
    AnimationCurve = 14,
    Bounds = 15,
    Gradient = 16,
    Quaternion = 17,
    ExposedReference = 18,
    FixedBufferSize = 19,
    Vector2Int = 20,
    Vector3Int = 21,
    RectInt = 22,
    BoundsInt = 23
}

public enum SerializedNumericType
{
    Unknown,
    Int8,
    UInt8,
    Int16,
    UInt16,
    Int32,
    UInt32,
    Int64,
    UInt64,
    Float,
    Double,
    Char
}

public enum SerializedInspectorMode
{
    Normal,
    Debug,
    DebugInternal
}

public enum SerializedNativeResult
{
    Success,
    InvalidArgument,
    Stale,
    ReadOnly,
    TypeMismatch,
    Failed
}

// The editor host installs one versioned table when the native Editor module
// starts. Keeping the table managed-side makes domain reload and shutdown
// deterministic without putting editor entry points in the Player ABI.
public sealed class EditorNativeApiTable
{
    public uint AbiVersion { get; init; } = 1;
    public ulong SchemaHash { get; init; }
    public Func<nint, bool>? IsValid { get; init; }
    public Action<nint>? ReleaseObject { get; init; }
    public Action<nint>? Update { get; init; }
    public Func<nint, bool>? UpdateIfRequiredOrScript { get; init; }
    public Func<nint, int>? GetTargetCount { get; init; }
    public Func<nint, SerializedInspectorMode>? GetInspectorMode { get; init; }
    public Action<nint, SerializedInspectorMode>? SetInspectorMode { get; init; }
    public Func<nint, int>? GetMaxArraySizeForMultiEditing { get; init; }
    public Action<nint, int>? SetMaxArraySizeForMultiEditing { get; init; }
    public Action<nint>? SetIsDifferentCacheDirty { get; init; }
    public Func<nint, string, nint>? FindProperty { get; init; }
    public Func<nint, bool>? HasModifiedProperties { get; init; }
    public Func<nint, bool, bool>? ApplyModifiedProperties { get; init; }
    public Func<nint, string, string>? GetPropertyString { get; init; }
    public Func<nint, string, bool>? GetPropertyBool { get; init; }
    public Func<nint, string, long>? GetPropertyLong { get; init; }
    public Func<nint, string, ulong>? GetPropertyUnsignedLong { get; init; }
    public Func<nint, string, double>? GetPropertyDouble { get; init; }
    public Func<nint, string, SerializedPropertyType>? GetPropertyType { get; init; }
    public Func<nint, string, SerializedNumericType>? GetPropertyNumericType { get; init; }
    public Func<nint, string, string, SerializedNativeResult>? SetPropertyString { get; init; }
    public Func<nint, string, bool, SerializedNativeResult>? SetPropertyBool { get; init; }
    public Func<nint, string, long, SerializedNativeResult>? SetPropertyLong { get; init; }
    public Func<nint, string, ulong, SerializedNativeResult>? SetPropertyUnsignedLong { get; init; }
    public Func<nint, string, double, SerializedNativeResult>? SetPropertyDouble { get; init; }
    public Func<nint, string, global::Nullus.Vector2>? GetPropertyVector2 { get; init; }
    public Func<nint, string, global::Nullus.Vector3>? GetPropertyVector3 { get; init; }
    public Func<nint, string, global::Nullus.Vector4>? GetPropertyVector4 { get; init; }
    public Func<nint, string, global::Nullus.Quaternion>? GetPropertyQuaternion { get; init; }
    public Func<nint, string, global::Nullus.Color>? GetPropertyColor { get; init; }
    public Func<nint, string, global::Nullus.Rect>? GetPropertyRect { get; init; }
    public Func<nint, string, global::Nullus.Vector2, SerializedNativeResult>? SetPropertyVector2 { get; init; }
    public Func<nint, string, global::Nullus.Vector3, SerializedNativeResult>? SetPropertyVector3 { get; init; }
    public Func<nint, string, global::Nullus.Vector4, SerializedNativeResult>? SetPropertyVector4 { get; init; }
    public Func<nint, string, global::Nullus.Quaternion, SerializedNativeResult>? SetPropertyQuaternion { get; init; }
    public Func<nint, string, global::Nullus.Color, SerializedNativeResult>? SetPropertyColor { get; init; }
    public Func<nint, string, global::Nullus.Rect, SerializedNativeResult>? SetPropertyRect { get; init; }
    public Func<nint, bool>? GetPropertyIsArray { get; init; }
    public Func<nint, int>? GetPropertyArraySize { get; init; }
    public Func<nint, int>? GetPropertyMinArraySize { get; init; }
    public Func<nint, int>? GetPropertyFixedBufferSize { get; init; }
    public Func<nint, int, SerializedNativeResult>? SetPropertyArraySize { get; init; }
    public Func<nint, int, SerializedNativeResult>? InsertPropertyArrayElement { get; init; }
    public Func<nint, int, SerializedNativeResult>? DeletePropertyArrayElement { get; init; }
    public Func<nint, int, int, SerializedNativeResult>? MovePropertyArrayElement { get; init; }
    public Func<nint, SerializedNativeResult>? ClearPropertyArray { get; init; }
    public Func<nint, int>? GetPropertyObjectReferenceInstanceId { get; init; }
    public Func<nint, int, SerializedNativeResult>? SetPropertyObjectReferenceInstanceId { get; init; }
    public Func<nint, string, string>? GetPropertyName { get; init; }
    public Func<nint, string, string>? GetPropertyDisplayName { get; init; }
    public Func<nint, string, string>? GetPropertyTooltip { get; init; }
    public Func<nint, int>? GetPropertyDepth { get; init; }
    public Func<nint, bool>? GetPropertyHasChildren { get; init; }
    public Func<nint, bool>? GetPropertyHasVisibleChildren { get; init; }
    public Func<nint, bool>? GetPropertyEditable { get; init; }
    public Func<nint, bool>? GetPropertyIsExpanded { get; init; }
    public Action<nint, bool>? SetPropertyIsExpanded { get; init; }
    public Func<nint, bool>? GetPropertyHasMultipleDifferentValues { get; init; }
    public Func<nint, bool>? GetPropertyPrefabOverride { get; init; }
    public Func<nint, bool>? GetPropertyDefaultOverride { get; init; }
    public Func<nint, bool>? GetPropertyInstantiatedPrefab { get; init; }
    public Func<nint, nint>? GetIterator { get; init; }
    public Func<nint, bool, bool, nint>? NextProperty { get; init; }
    public Func<nint, bool, nint>? GetEndProperty { get; init; }
    public Func<nint, nint>? CopyProperty { get; init; }
    public Func<nint, nint, bool>? EqualPropertyContents { get; init; }
    public Func<nint, string, string>? GetPropertyPathNative { get; init; }
    public Func<nint, nint, SerializedNativeResult>? CopyFromSerializedProperty { get; init; }
    public Func<nint, string, global::Nullus.Vector2Int>? GetPropertyVector2Int { get; init; }
    public Func<nint, string, global::Nullus.Vector3Int>? GetPropertyVector3Int { get; init; }
    public Func<nint, string, global::Nullus.RectInt>? GetPropertyRectInt { get; init; }
    public Func<nint, string, global::Nullus.Bounds>? GetPropertyBounds { get; init; }
    public Func<nint, string, global::Nullus.BoundsInt>? GetPropertyBoundsInt { get; init; }
    public Func<nint, string, global::Nullus.Vector2Int, SerializedNativeResult>? SetPropertyVector2Int { get; init; }
    public Func<nint, string, global::Nullus.Vector3Int, SerializedNativeResult>? SetPropertyVector3Int { get; init; }
    public Func<nint, string, global::Nullus.RectInt, SerializedNativeResult>? SetPropertyRectInt { get; init; }
    public Func<nint, string, global::Nullus.Bounds, SerializedNativeResult>? SetPropertyBounds { get; init; }
    public Func<nint, string, global::Nullus.BoundsInt, SerializedNativeResult>? SetPropertyBoundsInt { get; init; }
    public Func<nint, string, string>? GetPropertyEnumDisplayNames { get; init; }
    public Func<nint, string, byte[]>? GetPropertyAnimationCurveBytes { get; init; }
    public Func<nint, string, byte[], SerializedNativeResult>? SetPropertyAnimationCurveBytes { get; init; }
    public Func<nint, string, byte[]>? GetPropertyGradientBytes { get; init; }
    public Func<nint, string, byte[], SerializedNativeResult>? SetPropertyGradientBytes { get; init; }
    public Func<nint, string, byte[]>? GetPropertyExposedReferenceBytes { get; init; }
    public Func<nint, string, byte[], SerializedNativeResult>? SetPropertyExposedReferenceBytes { get; init; }
    public Func<nint, nint, SerializedNativeResult>? CopyFromSerializedPropertyIfDifferent { get; init; }
    public Func<nint, string, string>? GetPropertyTypeName { get; init; }
    public Func<nint, ulong>? GetPropertyExactTypeId { get; init; }
    public Func<nint, bool, int>? GetPropertyCountInProperty { get; init; }
    public Func<nint, int>? GetPropertyCountRemaining { get; init; }
    public Func<nint, bool>? GetPropertyReadOnly { get; init; }
    public Func<nint, int, int>? GetTargetObjectInstanceId { get; init; }
    public Func<nint, string>? GetLastError { get; init; }
    public Func<int[], nint>? CreateObjectFromInstanceIds { get; init; }
    public Action<nint>? DiscardModifiedProperties { get; init; }
    public Func<nint, int>? GetContextObjectInstanceId { get; init; }
    public Func<nint, int>? GetResolvedExposedReferenceInstanceId { get; init; }
    public Func<int[], int, nint>? CreateObjectFromInstanceIdsWithContext { get; init; }
    public Action<nint>? ResetProperty { get; init; }
    public Func<nint, nint, bool>? DataEquals { get; init; }
    public Func<nint, SerializedNativeResult>? DeletePropertyCommand { get; init; }
    public Func<nint, SerializedNativeResult>? DuplicatePropertyCommand { get; init; }
    public Func<nint, string>? GetPropertyArrayElementType { get; init; }
    public Func<nint, int, SerializedNativeResult>? SetResolvedExposedReferenceInstanceId { get; init; }
    public Func<nint, bool>? GetPropertyIsAnimated { get; init; }
    public Action<nint>? ReleaseProperty { get; init; }
    public Func<nint, bool>? IsPropertyValid { get; init; }

    [StructLayout(LayoutKind.Sequential)]
    private struct NativeHeader
    {
        public uint Size;
        public uint AbiVersion;
        public ulong SchemaHash;
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct NativeTable
    {
        public NativeHeader Header;
        public nint IsObjectValid;
        public nint ReleaseObject;
        public nint Update;
        public nint UpdateIfRequiredOrScript;
        public nint HasModifiedProperties;
        public nint ApplyModifiedProperties;
        public nint FindProperty;
        public nint IsPropertyValid;
        public nint ReleaseProperty;
        public nint GetPropertyType;
        public nint GetPropertyBool;
        public nint GetPropertyLong;
        public nint GetPropertyUnsignedLong;
        public nint GetPropertyDouble;
        public nint GetPropertyString;
        public nint SetPropertyBool;
        public nint SetPropertyLong;
        public nint SetPropertyUnsignedLong;
        public nint SetPropertyDouble;
        public nint SetPropertyString;
        public nint GetPropertyVector2;
        public nint GetPropertyVector3;
        public nint GetPropertyVector4;
        public nint GetPropertyQuaternion;
        public nint GetPropertyColor;
        public nint GetPropertyRect;
        public nint SetPropertyVector2;
        public nint SetPropertyVector3;
        public nint SetPropertyVector4;
        public nint SetPropertyQuaternion;
        public nint SetPropertyColor;
        public nint SetPropertyRect;
        public nint IsPropertyArray;
        public nint GetPropertyArraySize;
        public nint GetPropertyMinArraySize;
        public nint GetPropertyFixedBufferSize;
        public nint SetPropertyArraySize;
        public nint InsertPropertyArrayElement;
        public nint DeletePropertyArrayElement;
        public nint MovePropertyArrayElement;
        public nint ClearPropertyArray;
        public nint GetPropertyObjectReferenceInstanceId;
        public nint SetPropertyObjectReferenceInstanceId;
        public nint GetPropertyNumericType;
        public nint GetPropertyName;
        public nint GetPropertyDisplayName;
        public nint GetPropertyTooltip;
        public nint GetPropertyDepth;
        public nint GetPropertyHasChildren;
        public nint GetPropertyHasVisibleChildren;
        public nint GetPropertyEditable;
        public nint GetPropertyIsExpanded;
        public nint SetPropertyIsExpanded;
        public nint GetPropertyHasMultipleDifferentValues;
        public nint GetPropertyPrefabOverride;
        public nint GetPropertyDefaultOverride;
        public nint GetPropertyInstantiatedPrefab;
        public nint GetIterator;
        public nint NextProperty;
        public nint GetEndProperty;
        public nint CopyProperty;
        public nint EqualPropertyContents;
        public nint GetPropertyPath;
        public nint GetTargetCount;
        public nint GetInspectorMode;
        public nint SetInspectorMode;
        public nint GetMaxArraySizeForMultiEditing;
        public nint SetMaxArraySizeForMultiEditing;
        public nint SetIsDifferentCacheDirty;
        public nint CopyFromSerializedProperty;
        public nint GetPropertyVector2Int;
        public nint GetPropertyVector3Int;
        public nint GetPropertyRectInt;
        public nint GetPropertyBounds;
        public nint GetPropertyBoundsInt;
        public nint SetPropertyVector2Int;
        public nint SetPropertyVector3Int;
        public nint SetPropertyRectInt;
        public nint SetPropertyBounds;
        public nint SetPropertyBoundsInt;
        public nint GetPropertyEnumDisplayNames;
        public nint GetPropertyAnimationCurve;
        public nint SetPropertyAnimationCurve;
        public nint GetPropertyGradient;
        public nint SetPropertyGradient;
        public nint GetPropertyExposedReference;
        public nint SetPropertyExposedReference;
        public nint CopyFromSerializedPropertyIfDifferent;
        public nint GetPropertyTypeName;
        public nint GetPropertyExactTypeId;
        public nint GetPropertyCountInProperty;
        public nint GetPropertyCountRemaining;
        public nint GetPropertyReadOnly;
        public nint GetTargetObjectInstanceId;
        public nint GetObjectLastError;
        public nint CreateObjectFromInstanceIds;
        public nint DiscardModifiedProperties;
        public nint GetContextObjectInstanceId;
        public nint GetResolvedExposedReferenceInstanceId;
        public nint CreateObjectFromInstanceIdsWithContext;
        public nint ResetProperty;
        public nint DataEquals;
        public nint DeletePropertyCommand;
        public nint DuplicatePropertyCommand;
        public nint GetPropertyArrayElementType;
        public nint SetResolvedExposedReferenceInstanceId;
        public nint GetPropertyIsAnimated;
    }

    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    private delegate byte IsObjectValidFn(ulong handle);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    private delegate void ReleaseObjectFn(ulong handle);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    private delegate void UpdateFn(ulong handle);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    private delegate byte UpdateIfRequiredFn(ulong handle);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    private delegate byte HasModifiedFn(ulong handle);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    private delegate byte ApplyFn(ulong handle, byte withoutUndo);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    private delegate ulong FindPropertyFn(ulong handle, [MarshalAs(UnmanagedType.LPUTF8Str)] string path);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    private delegate byte IsPropertyValidFn(ulong handle);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    private delegate int GetPropertyTypeFn(ulong handle);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    private delegate byte GetPropertyBoolFn(ulong handle);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    private delegate long GetPropertyLongFn(ulong handle);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    private delegate ulong GetPropertyUnsignedLongFn(ulong handle);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    private delegate double GetPropertyDoubleFn(ulong handle);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    private delegate uint GetPropertyStringFn(ulong handle, nint output, uint capacity);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    private delegate byte SetPropertyBoolFn(ulong handle, byte value);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    private delegate byte SetPropertyLongFn(ulong handle, long value);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    private delegate byte SetPropertyUnsignedLongFn(ulong handle, ulong value);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    private delegate byte SetPropertyDoubleFn(ulong handle, double value);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    private delegate byte SetPropertyStringFn(ulong handle, [MarshalAs(UnmanagedType.LPUTF8Str)] string value);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    private unsafe delegate byte GetPropertyVector2Fn(ulong handle, float* x, float* y);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    private unsafe delegate byte GetPropertyVector3Fn(ulong handle, float* x, float* y, float* z);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    private unsafe delegate byte GetPropertyVector4Fn(ulong handle, float* x, float* y, float* z, float* w);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    private unsafe delegate byte GetPropertyColorFn(ulong handle, float* r, float* g, float* b, float* a);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    private unsafe delegate byte GetPropertyRectFn(ulong handle, float* x, float* y, float* width, float* height);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    private delegate byte SetPropertyVector2Fn(ulong handle, float x, float y);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    private delegate byte SetPropertyVector3Fn(ulong handle, float x, float y, float z);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    private delegate byte SetPropertyVector4Fn(ulong handle, float x, float y, float z, float w);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    private delegate byte SetPropertyColorFn(ulong handle, float r, float g, float b, float a);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    private delegate byte SetPropertyRectFn(ulong handle, float x, float y, float width, float height);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    private delegate byte IsPropertyArrayFn(ulong handle);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    private delegate uint GetPropertyArraySizeFn(ulong handle);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    private delegate byte SetPropertyArraySizeFn(ulong handle, uint value);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    private delegate byte InsertPropertyArrayElementFn(ulong handle, uint index);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    private delegate byte DeletePropertyArrayElementFn(ulong handle, uint index);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    private delegate byte MovePropertyArrayElementFn(ulong handle, uint sourceIndex, uint destinationIndex);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    private delegate byte ClearPropertyArrayFn(ulong handle);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    private delegate int GetPropertyObjectReferenceInstanceIdFn(ulong handle);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    private delegate byte SetPropertyObjectReferenceInstanceIdFn(ulong handle, int instanceId);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    private delegate byte GetPropertyNumericTypeFn(ulong handle);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    private delegate uint GetPropertyTextFn(ulong handle, nint output, uint capacity);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    private delegate uint GetPropertyDepthFn(ulong handle);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    private delegate byte GetPropertyBoolFlagFn(ulong handle);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    private delegate byte SetPropertyExpandedFn(ulong handle, byte expanded);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    private delegate ulong GetIteratorFn(ulong objectHandle);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    private delegate ulong NextPropertyFn(ulong propertyHandle, byte enterChildren, byte visible);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    private delegate ulong GetEndPropertyFn(ulong propertyHandle, byte includeChildren);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    private delegate ulong CopyPropertyFn(ulong propertyHandle);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    private delegate byte EqualPropertyContentsFn(ulong left, ulong right);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    private delegate uint GetTargetCountFn(ulong objectHandle);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    private delegate byte GetInspectorModeFn(ulong objectHandle);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    private delegate void SetInspectorModeFn(ulong objectHandle, byte mode);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    private delegate uint GetMaxArraySizeForMultiEditingFn(ulong objectHandle);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    private delegate byte SetMaxArraySizeForMultiEditingFn(ulong objectHandle, uint value);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    private delegate void SetIsDifferentCacheDirtyFn(ulong objectHandle);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    private delegate byte CopyFromSerializedPropertyFn(ulong objectHandle, ulong propertyHandle);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    private unsafe delegate byte GetPropertyVector2IntFn(ulong handle, int* x, int* y);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    private unsafe delegate byte GetPropertyVector3IntFn(ulong handle, int* x, int* y, int* z);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    private unsafe delegate byte GetPropertyRectIntFn(ulong handle, int* x, int* y, int* width, int* height);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    private unsafe delegate byte GetPropertyBoundsFn(ulong handle, float* centerX, float* centerY, float* centerZ, float* sizeX, float* sizeY, float* sizeZ);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    private unsafe delegate byte GetPropertyBoundsIntFn(ulong handle, int* positionX, int* positionY, int* positionZ, int* sizeX, int* sizeY, int* sizeZ);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    private delegate byte SetPropertyVector2IntFn(ulong handle, int x, int y);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    private delegate byte SetPropertyVector3IntFn(ulong handle, int x, int y, int z);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    private delegate byte SetPropertyRectIntFn(ulong handle, int x, int y, int width, int height);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    private delegate byte SetPropertyBoundsFn(ulong handle, float centerX, float centerY, float centerZ, float sizeX, float sizeY, float sizeZ);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    private delegate byte SetPropertyBoundsIntFn(ulong handle, int positionX, int positionY, int positionZ, int sizeX, int sizeY, int sizeZ);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    private delegate uint GetPropertyEnumDisplayNamesFn(ulong handle, nint output, uint capacity);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    private delegate uint GetPropertyBytesFn(ulong handle, nint output, uint capacity);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    private delegate byte SetPropertyBytesFn(ulong handle, nint input, uint size);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    private delegate byte CopyFromSerializedPropertyIfDifferentFn(ulong objectHandle, ulong propertyHandle);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    private delegate ulong GetPropertyExactTypeIdFn(ulong handle);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    private delegate uint GetPropertyCountInPropertyFn(ulong handle, byte enterChildren);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    private delegate uint GetPropertyCountRemainingFn(ulong handle);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    private delegate byte GetPropertyReadOnlyFn(ulong handle);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    private delegate int GetTargetObjectInstanceIdFn(ulong objectHandle, uint index);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    private delegate uint GetObjectLastErrorFn(ulong objectHandle, nint output, uint capacity);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    private unsafe delegate ulong CreateObjectFromInstanceIdsFn(int* instanceIds, uint count);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    private delegate void DiscardModifiedPropertiesFn(ulong objectHandle);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    private delegate int GetContextObjectInstanceIdFn(ulong objectHandle);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    private delegate int GetResolvedExposedReferenceInstanceIdFn(ulong propertyHandle);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    private unsafe delegate ulong CreateObjectFromInstanceIdsWithContextFn(int* instanceIds, uint count, int contextInstanceId);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    private delegate void ResetPropertyFn(ulong propertyHandle);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    private delegate byte DataEqualsFn(ulong leftPropertyHandle, ulong rightPropertyHandle);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    private delegate byte PropertyCommandFn(ulong propertyHandle);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    private delegate byte SetResolvedExposedReferenceInstanceIdFn(ulong propertyHandle, int instanceId);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    private delegate byte GetPropertyIsAnimatedFn(ulong propertyHandle);

    private static T? ReadDelegate<T>(nint pointer) where T : Delegate
        => pointer == 0 ? null : Marshal.GetDelegateForFunctionPointer<T>(pointer);

    public static unsafe EditorNativeApiTable FromNativeTable(nint nativeTable)
    {
        if (nativeTable == 0)
            throw new ArgumentException("The native Editor serialization table is null.", nameof(nativeTable));
        var header = Marshal.PtrToStructure<NativeHeader>(nativeTable);
        var expectedSize = Marshal.SizeOf<NativeTable>();
        var minimumSize = Marshal.SizeOf<NativeHeader>();
        if (header.AbiVersion != 1 || header.Size < minimumSize)
        {
            throw new InvalidOperationException(
                $"Unsupported Editor serialization ABI (size={header.Size}, version={header.AbiVersion}).");
        }

        // The table is append-only. Copy only the bytes advertised by the
        // producer into a zero-filled current-layout buffer so older native
        // hosts remain usable and newly added function pointers stay null.
        var nativeStorage = new byte[expectedSize];
        var copySize = checked((int)Math.Min((ulong)header.Size, (ulong)expectedSize));
        Marshal.Copy(nativeTable, nativeStorage, 0, copySize);
        fixed (byte* storage = nativeStorage)
        {
            var native = Marshal.PtrToStructure<NativeTable>((nint)storage);

        var objectValid = ReadDelegate<IsObjectValidFn>(native.IsObjectValid);
        var releaseObject = ReadDelegate<ReleaseObjectFn>(native.ReleaseObject);
        var update = ReadDelegate<UpdateFn>(native.Update);
        var updateIfRequired = ReadDelegate<UpdateIfRequiredFn>(native.UpdateIfRequiredOrScript);
        var hasModified = ReadDelegate<HasModifiedFn>(native.HasModifiedProperties);
        var apply = ReadDelegate<ApplyFn>(native.ApplyModifiedProperties);
        var findProperty = ReadDelegate<FindPropertyFn>(native.FindProperty);
        var isPropertyValid = ReadDelegate<IsPropertyValidFn>(native.IsPropertyValid);
        var releaseProperty = ReadDelegate<ReleaseObjectFn>(native.ReleaseProperty);
        var getPropertyType = ReadDelegate<GetPropertyTypeFn>(native.GetPropertyType);
        var getBool = ReadDelegate<GetPropertyBoolFn>(native.GetPropertyBool);
        var getLong = ReadDelegate<GetPropertyLongFn>(native.GetPropertyLong);
        var getUnsignedLong = ReadDelegate<GetPropertyUnsignedLongFn>(native.GetPropertyUnsignedLong);
        var getDouble = ReadDelegate<GetPropertyDoubleFn>(native.GetPropertyDouble);
        var getString = ReadDelegate<GetPropertyStringFn>(native.GetPropertyString);
        var setBool = ReadDelegate<SetPropertyBoolFn>(native.SetPropertyBool);
        var setLong = ReadDelegate<SetPropertyLongFn>(native.SetPropertyLong);
        var setUnsignedLong = ReadDelegate<SetPropertyUnsignedLongFn>(native.SetPropertyUnsignedLong);
        var setDouble = ReadDelegate<SetPropertyDoubleFn>(native.SetPropertyDouble);
        var setString = ReadDelegate<SetPropertyStringFn>(native.SetPropertyString);
        var getVector2 = ReadDelegate<GetPropertyVector2Fn>(native.GetPropertyVector2);
        var getVector3 = ReadDelegate<GetPropertyVector3Fn>(native.GetPropertyVector3);
        var getVector4 = ReadDelegate<GetPropertyVector4Fn>(native.GetPropertyVector4);
        var getQuaternion = ReadDelegate<GetPropertyVector4Fn>(native.GetPropertyQuaternion);
        var getColor = ReadDelegate<GetPropertyColorFn>(native.GetPropertyColor);
        var getRect = ReadDelegate<GetPropertyRectFn>(native.GetPropertyRect);
        var setVector2 = ReadDelegate<SetPropertyVector2Fn>(native.SetPropertyVector2);
        var setVector3 = ReadDelegate<SetPropertyVector3Fn>(native.SetPropertyVector3);
        var setVector4 = ReadDelegate<SetPropertyVector4Fn>(native.SetPropertyVector4);
        var setQuaternion = ReadDelegate<SetPropertyVector4Fn>(native.SetPropertyQuaternion);
        var setColor = ReadDelegate<SetPropertyColorFn>(native.SetPropertyColor);
        var setRect = ReadDelegate<SetPropertyRectFn>(native.SetPropertyRect);
        var isArray = ReadDelegate<IsPropertyArrayFn>(native.IsPropertyArray);
        var getArraySize = ReadDelegate<GetPropertyArraySizeFn>(native.GetPropertyArraySize);
        var getMinArraySize = ReadDelegate<GetPropertyArraySizeFn>(native.GetPropertyMinArraySize);
        var getFixedBufferSize = ReadDelegate<GetPropertyArraySizeFn>(native.GetPropertyFixedBufferSize);
        var setArraySize = ReadDelegate<SetPropertyArraySizeFn>(native.SetPropertyArraySize);
        var insertArrayElement = ReadDelegate<InsertPropertyArrayElementFn>(native.InsertPropertyArrayElement);
        var deleteArrayElement = ReadDelegate<DeletePropertyArrayElementFn>(native.DeletePropertyArrayElement);
        var moveArrayElement = ReadDelegate<MovePropertyArrayElementFn>(native.MovePropertyArrayElement);
        var clearArray = ReadDelegate<ClearPropertyArrayFn>(native.ClearPropertyArray);
        var getObjectReference = ReadDelegate<GetPropertyObjectReferenceInstanceIdFn>(native.GetPropertyObjectReferenceInstanceId);
        var setObjectReference = ReadDelegate<SetPropertyObjectReferenceInstanceIdFn>(native.SetPropertyObjectReferenceInstanceId);
        var getNumericType = ReadDelegate<GetPropertyNumericTypeFn>(native.GetPropertyNumericType);
        var getName = ReadDelegate<GetPropertyTextFn>(native.GetPropertyName);
        var getDisplayName = ReadDelegate<GetPropertyTextFn>(native.GetPropertyDisplayName);
        var getTooltip = ReadDelegate<GetPropertyTextFn>(native.GetPropertyTooltip);
        var getDepth = ReadDelegate<GetPropertyDepthFn>(native.GetPropertyDepth);
        var getHasChildren = ReadDelegate<GetPropertyBoolFlagFn>(native.GetPropertyHasChildren);
        var getHasVisibleChildren = ReadDelegate<GetPropertyBoolFlagFn>(native.GetPropertyHasVisibleChildren);
        var getEditable = ReadDelegate<GetPropertyBoolFlagFn>(native.GetPropertyEditable);
        var getExpanded = ReadDelegate<GetPropertyBoolFlagFn>(native.GetPropertyIsExpanded);
        var setExpanded = ReadDelegate<SetPropertyExpandedFn>(native.SetPropertyIsExpanded);
        var getMultipleDifferent = ReadDelegate<GetPropertyBoolFlagFn>(native.GetPropertyHasMultipleDifferentValues);
        var getPrefabOverride = ReadDelegate<GetPropertyBoolFlagFn>(native.GetPropertyPrefabOverride);
        var getDefaultOverride = ReadDelegate<GetPropertyBoolFlagFn>(native.GetPropertyDefaultOverride);
        var getInstantiatedPrefab = ReadDelegate<GetPropertyBoolFlagFn>(native.GetPropertyInstantiatedPrefab);
        var getIterator = ReadDelegate<GetIteratorFn>(native.GetIterator);
        var nextProperty = ReadDelegate<NextPropertyFn>(native.NextProperty);
        var getEndProperty = ReadDelegate<GetEndPropertyFn>(native.GetEndProperty);
        var copyProperty = ReadDelegate<CopyPropertyFn>(native.CopyProperty);
        var equalPropertyContents = ReadDelegate<EqualPropertyContentsFn>(native.EqualPropertyContents);
        var getPropertyPath = ReadDelegate<GetPropertyTextFn>(native.GetPropertyPath);
        var getTargetCount = ReadDelegate<GetTargetCountFn>(native.GetTargetCount);
        var getInspectorMode = ReadDelegate<GetInspectorModeFn>(native.GetInspectorMode);
        var setInspectorMode = ReadDelegate<SetInspectorModeFn>(native.SetInspectorMode);
        var getMaxArraySizeForMultiEditing = ReadDelegate<GetMaxArraySizeForMultiEditingFn>(native.GetMaxArraySizeForMultiEditing);
        var setMaxArraySizeForMultiEditing = ReadDelegate<SetMaxArraySizeForMultiEditingFn>(native.SetMaxArraySizeForMultiEditing);
        var setIsDifferentCacheDirty = ReadDelegate<SetIsDifferentCacheDirtyFn>(native.SetIsDifferentCacheDirty);
        var copyFromSerializedProperty = ReadDelegate<CopyFromSerializedPropertyFn>(native.CopyFromSerializedProperty);
        var getVector2Int = ReadDelegate<GetPropertyVector2IntFn>(native.GetPropertyVector2Int);
        var getVector3Int = ReadDelegate<GetPropertyVector3IntFn>(native.GetPropertyVector3Int);
        var getRectInt = ReadDelegate<GetPropertyRectIntFn>(native.GetPropertyRectInt);
        var getBounds = ReadDelegate<GetPropertyBoundsFn>(native.GetPropertyBounds);
        var getBoundsInt = ReadDelegate<GetPropertyBoundsIntFn>(native.GetPropertyBoundsInt);
        var setVector2Int = ReadDelegate<SetPropertyVector2IntFn>(native.SetPropertyVector2Int);
        var setVector3Int = ReadDelegate<SetPropertyVector3IntFn>(native.SetPropertyVector3Int);
        var setRectInt = ReadDelegate<SetPropertyRectIntFn>(native.SetPropertyRectInt);
        var setBounds = ReadDelegate<SetPropertyBoundsFn>(native.SetPropertyBounds);
        var setBoundsInt = ReadDelegate<SetPropertyBoundsIntFn>(native.SetPropertyBoundsInt);
        var getEnumDisplayNames = ReadDelegate<GetPropertyEnumDisplayNamesFn>(native.GetPropertyEnumDisplayNames);
        var getAnimationCurve = ReadDelegate<GetPropertyBytesFn>(native.GetPropertyAnimationCurve);
        var setAnimationCurve = ReadDelegate<SetPropertyBytesFn>(native.SetPropertyAnimationCurve);
        var getGradient = ReadDelegate<GetPropertyBytesFn>(native.GetPropertyGradient);
        var setGradient = ReadDelegate<SetPropertyBytesFn>(native.SetPropertyGradient);
        var getExposedReference = ReadDelegate<GetPropertyBytesFn>(native.GetPropertyExposedReference);
        var setExposedReference = ReadDelegate<SetPropertyBytesFn>(native.SetPropertyExposedReference);
        var copyFromSerializedPropertyIfDifferent = ReadDelegate<CopyFromSerializedPropertyIfDifferentFn>(native.CopyFromSerializedPropertyIfDifferent);
        var getPropertyTypeName = ReadDelegate<GetPropertyTextFn>(native.GetPropertyTypeName);
        var getPropertyExactTypeId = ReadDelegate<GetPropertyExactTypeIdFn>(native.GetPropertyExactTypeId);
        var getPropertyCountInProperty = ReadDelegate<GetPropertyCountInPropertyFn>(native.GetPropertyCountInProperty);
        var getPropertyCountRemaining = ReadDelegate<GetPropertyCountRemainingFn>(native.GetPropertyCountRemaining);
        var getPropertyReadOnly = ReadDelegate<GetPropertyReadOnlyFn>(native.GetPropertyReadOnly);
        var getTargetObjectInstanceId = ReadDelegate<GetTargetObjectInstanceIdFn>(native.GetTargetObjectInstanceId);
        var getObjectLastError = ReadDelegate<GetObjectLastErrorFn>(native.GetObjectLastError);
        var createObjectFromInstanceIds = ReadDelegate<CreateObjectFromInstanceIdsFn>(native.CreateObjectFromInstanceIds);
        var discardModifiedProperties = ReadDelegate<DiscardModifiedPropertiesFn>(native.DiscardModifiedProperties);
        var getContextObjectInstanceId = ReadDelegate<GetContextObjectInstanceIdFn>(native.GetContextObjectInstanceId);
        var getResolvedExposedReferenceInstanceId = ReadDelegate<GetResolvedExposedReferenceInstanceIdFn>(native.GetResolvedExposedReferenceInstanceId);
        var createObjectFromInstanceIdsWithContext = ReadDelegate<CreateObjectFromInstanceIdsWithContextFn>(native.CreateObjectFromInstanceIdsWithContext);
        var resetProperty = ReadDelegate<ResetPropertyFn>(native.ResetProperty);
        var dataEquals = ReadDelegate<DataEqualsFn>(native.DataEquals);
        var deletePropertyCommand = ReadDelegate<PropertyCommandFn>(native.DeletePropertyCommand);
        var duplicatePropertyCommand = ReadDelegate<PropertyCommandFn>(native.DuplicatePropertyCommand);
        var getPropertyArrayElementType = ReadDelegate<GetPropertyTextFn>(native.GetPropertyArrayElementType);
        var setResolvedExposedReference = ReadDelegate<SetResolvedExposedReferenceInstanceIdFn>(native.SetResolvedExposedReferenceInstanceId);
        var getPropertyIsAnimated = ReadDelegate<GetPropertyIsAnimatedFn>(native.GetPropertyIsAnimated);

        string ReadPropertyText(GetPropertyTextFn? getter, ulong handle, string path)
        {
            if (getter is null)
                return string.Empty;
            var size = getter(handle, 0, 0);
            if (size == 0)
                return string.Empty;
            var buffer = Marshal.AllocCoTaskMem(checked((int)size + 1));
            try
            {
                getter(handle, buffer, size + 1);
                return Marshal.PtrToStringUTF8(buffer) ?? string.Empty;
            }
            finally
            {
                Marshal.FreeCoTaskMem(buffer);
            }
        }

        byte[] ReadPropertyBytes(GetPropertyBytesFn getter, ulong handle, string path)
        {
            var size = getter(handle, 0, 0);
            if (size == 0)
                return Array.Empty<byte>();
            if (size > int.MaxValue)
                throw new InvalidOperationException($"Native serialized payload for '{path}' is too large.");
            var buffer = Marshal.AllocCoTaskMem(checked((int)size));
            try
            {
                var copied = getter(handle, buffer, size);
                if (copied > size)
                    throw new InvalidOperationException($"Native serialized payload for '{path}' changed size while reading.");
                var result = new byte[checked((int)copied)];
                if (copied != 0)
                    Marshal.Copy(buffer, result, 0, checked((int)copied));
                return result;
            }
            finally
            {
                Marshal.FreeCoTaskMem(buffer);
            }
        }

        SerializedNativeResult WritePropertyBytes(SetPropertyBytesFn setter, ulong handle, string path, byte[] bytes)
        {
            ArgumentNullException.ThrowIfNull(bytes);
            if (bytes.Length == 0)
                return setter(handle, 0, 0) != 0 ? SerializedNativeResult.Success : SerializedNativeResult.Failed;
            var buffer = Marshal.AllocCoTaskMem(bytes.Length);
            try
            {
                Marshal.Copy(bytes, 0, buffer, bytes.Length);
                return setter(handle, buffer, checked((uint)bytes.Length)) != 0
                    ? SerializedNativeResult.Success
                    : SerializedNativeResult.Failed;
            }
            catch (OverflowException)
            {
                throw new ArgumentException($"Native serialized payload for '{path}' is too large.", nameof(bytes));
            }
            finally
            {
                Marshal.FreeCoTaskMem(buffer);
            }
        }

        return new EditorNativeApiTable
        {
            AbiVersion = native.Header.AbiVersion,
            SchemaHash = native.Header.SchemaHash,
            IsValid = objectValid is null ? null : handle => objectValid((ulong)handle) != 0,
            ReleaseObject = releaseObject is null ? null : handle => releaseObject((ulong)handle),
            Update = update is null ? null : handle => update((ulong)handle),
            UpdateIfRequiredOrScript = updateIfRequired is null ? null : handle => updateIfRequired((ulong)handle) != 0,
            HasModifiedProperties = hasModified is null ? null : handle => hasModified((ulong)handle) != 0,
            ApplyModifiedProperties = apply is null ? null : (handle, withoutUndo) => apply((ulong)handle, withoutUndo ? (byte)1 : (byte)0) != 0,
            FindProperty = findProperty is null ? null : (handle, path) => (nint)findProperty((ulong)handle, path),
            GetPropertyType = getPropertyType is null ? null : (handle, _) => (SerializedPropertyType)getPropertyType((ulong)handle),
            GetPropertyBool = getBool is null ? null : (handle, _) => getBool((ulong)handle) != 0,
            GetPropertyLong = getLong is null ? null : (handle, _) => getLong((ulong)handle),
            GetPropertyUnsignedLong = getUnsignedLong is null ? null : (handle, _) => getUnsignedLong((ulong)handle),
            GetPropertyDouble = getDouble is null ? null : (handle, _) => getDouble((ulong)handle),
            GetPropertyString = getString is null ? null : (handle, path) =>
            {
                var size = getString((ulong)handle, 0, 0);
                if (size == 0)
                    return string.Empty;
                var buffer = Marshal.AllocCoTaskMem(checked((int)size + 1));
                try
                {
                    getString((ulong)handle, buffer, size + 1);
                    return Marshal.PtrToStringUTF8(buffer) ?? string.Empty;
                }
                finally
                {
                    Marshal.FreeCoTaskMem(buffer);
                }
            },
            SetPropertyBool = setBool is null ? null : (handle, path, value) =>
                setBool((ulong)handle, value ? (byte)1 : (byte)0) != 0 ? SerializedNativeResult.Success : SerializedNativeResult.Failed,
            SetPropertyLong = setLong is null ? null : (handle, path, value) =>
                setLong((ulong)handle, value) != 0 ? SerializedNativeResult.Success : SerializedNativeResult.Failed,
            SetPropertyUnsignedLong = setUnsignedLong is null ? null : (handle, path, value) =>
                setUnsignedLong((ulong)handle, value) != 0 ? SerializedNativeResult.Success : SerializedNativeResult.Failed,
            SetPropertyDouble = setDouble is null ? null : (handle, path, value) =>
                setDouble((ulong)handle, value) != 0 ? SerializedNativeResult.Success : SerializedNativeResult.Failed,
            SetPropertyString = setString is null ? null : (handle, path, value) =>
                setString((ulong)handle, value) != 0 ? SerializedNativeResult.Success : SerializedNativeResult.Failed,
            GetPropertyVector2 = getVector2 is null ? null : (handle, path) =>
            {
                float x = 0.0f, y = 0.0f;
                if (getVector2((ulong)handle, &x, &y) == 0)
                    throw new InvalidOperationException($"Native Vector2 getter failed for '{path}'.");
                return new global::Nullus.Vector2(x, y);
            },
            GetPropertyVector3 = getVector3 is null ? null : (handle, path) =>
            {
                float x = 0.0f, y = 0.0f, z = 0.0f;
                if (getVector3((ulong)handle, &x, &y, &z) == 0)
                    throw new InvalidOperationException($"Native Vector3 getter failed for '{path}'.");
                return new global::Nullus.Vector3(x, y, z);
            },
            GetPropertyVector4 = getVector4 is null ? null : (handle, path) =>
            {
                float x = 0.0f, y = 0.0f, z = 0.0f, w = 0.0f;
                if (getVector4((ulong)handle, &x, &y, &z, &w) == 0)
                    throw new InvalidOperationException($"Native Vector4 getter failed for '{path}'.");
                return new global::Nullus.Vector4(x, y, z, w);
            },
            GetPropertyQuaternion = getQuaternion is null ? null : (handle, path) =>
            {
                float x = 0.0f, y = 0.0f, z = 0.0f, w = 1.0f;
                if (getQuaternion((ulong)handle, &x, &y, &z, &w) == 0)
                    throw new InvalidOperationException($"Native Quaternion getter failed for '{path}'.");
                return new global::Nullus.Quaternion(x, y, z, w);
            },
            GetPropertyColor = getColor is null ? null : (handle, path) =>
            {
                float r = 0.0f, g = 0.0f, b = 0.0f, a = 1.0f;
                if (getColor((ulong)handle, &r, &g, &b, &a) == 0)
                    throw new InvalidOperationException($"Native Color getter failed for '{path}'.");
                return new global::Nullus.Color(r, g, b, a);
            },
            GetPropertyRect = getRect is null ? null : (handle, path) =>
            {
                float x = 0.0f, y = 0.0f, width = 0.0f, height = 0.0f;
                if (getRect((ulong)handle, &x, &y, &width, &height) == 0)
                    throw new InvalidOperationException($"Native Rect getter failed for '{path}'.");
                return new global::Nullus.Rect(x, y, width, height);
            },
            SetPropertyVector2 = setVector2 is null ? null : (handle, path, value) =>
                setVector2((ulong)handle, value.X, value.Y) != 0 ? SerializedNativeResult.Success : SerializedNativeResult.Failed,
            SetPropertyVector3 = setVector3 is null ? null : (handle, path, value) =>
                setVector3((ulong)handle, value.X, value.Y, value.Z) != 0 ? SerializedNativeResult.Success : SerializedNativeResult.Failed,
            SetPropertyVector4 = setVector4 is null ? null : (handle, path, value) =>
                setVector4((ulong)handle, value.X, value.Y, value.Z, value.W) != 0 ? SerializedNativeResult.Success : SerializedNativeResult.Failed,
            SetPropertyQuaternion = setQuaternion is null ? null : (handle, path, value) =>
                setQuaternion((ulong)handle, value.X, value.Y, value.Z, value.W) != 0 ? SerializedNativeResult.Success : SerializedNativeResult.Failed,
            SetPropertyColor = setColor is null ? null : (handle, path, value) =>
                setColor((ulong)handle, value.R, value.G, value.B, value.A) != 0 ? SerializedNativeResult.Success : SerializedNativeResult.Failed,
            SetPropertyRect = setRect is null ? null : (handle, path, value) =>
                setRect((ulong)handle, value.X, value.Y, value.Width, value.Height) != 0 ? SerializedNativeResult.Success : SerializedNativeResult.Failed,
            GetPropertyIsArray = isArray is null ? null : handle => isArray((ulong)handle) != 0,
            GetPropertyArraySize = getArraySize is null ? null : handle => checked((int)getArraySize((ulong)handle)),
            GetPropertyMinArraySize = getMinArraySize is null ? null : handle => checked((int)getMinArraySize((ulong)handle)),
            GetPropertyFixedBufferSize = getFixedBufferSize is null ? null : handle => checked((int)getFixedBufferSize((ulong)handle)),
            SetPropertyArraySize = setArraySize is null ? null : (handle, value) =>
                setArraySize((ulong)handle, checked((uint)value)) != 0 ? SerializedNativeResult.Success : SerializedNativeResult.Failed,
            InsertPropertyArrayElement = insertArrayElement is null ? null : (handle, index) =>
                insertArrayElement((ulong)handle, checked((uint)index)) != 0 ? SerializedNativeResult.Success : SerializedNativeResult.Failed,
            DeletePropertyArrayElement = deleteArrayElement is null ? null : (handle, index) =>
                deleteArrayElement((ulong)handle, checked((uint)index)) != 0 ? SerializedNativeResult.Success : SerializedNativeResult.Failed,
            MovePropertyArrayElement = moveArrayElement is null ? null : (handle, sourceIndex, destinationIndex) =>
                moveArrayElement((ulong)handle, checked((uint)sourceIndex), checked((uint)destinationIndex)) != 0 ? SerializedNativeResult.Success : SerializedNativeResult.Failed,
            ClearPropertyArray = clearArray is null ? null : handle =>
                clearArray((ulong)handle) != 0 ? SerializedNativeResult.Success : SerializedNativeResult.Failed,
            GetPropertyObjectReferenceInstanceId = getObjectReference is null ? null : handle => getObjectReference((ulong)handle),
            SetPropertyObjectReferenceInstanceId = setObjectReference is null ? null : (handle, instanceId) =>
                setObjectReference((ulong)handle, instanceId) != 0 ? SerializedNativeResult.Success : SerializedNativeResult.Failed,
            GetPropertyNumericType = getNumericType is null ? null : (handle, path) =>
                (SerializedNumericType)getNumericType((ulong)handle),
            GetPropertyName = getName is null ? null : (handle, path) => ReadPropertyText(getName, (ulong)handle, path),
            GetPropertyDisplayName = getDisplayName is null ? null : (handle, path) => ReadPropertyText(getDisplayName, (ulong)handle, path),
            GetPropertyTooltip = getTooltip is null ? null : (handle, path) => ReadPropertyText(getTooltip, (ulong)handle, path),
            GetPropertyDepth = getDepth is null ? null : handle => checked((int)getDepth((ulong)handle)),
            GetPropertyHasChildren = getHasChildren is null ? null : handle => getHasChildren((ulong)handle) != 0,
            GetPropertyHasVisibleChildren = getHasVisibleChildren is null ? null : handle => getHasVisibleChildren((ulong)handle) != 0,
            GetPropertyEditable = getEditable is null ? null : handle => getEditable((ulong)handle) != 0,
            GetPropertyIsExpanded = getExpanded is null ? null : handle => getExpanded((ulong)handle) != 0,
            SetPropertyIsExpanded = setExpanded is null ? null : (handle, value) =>
                _ = setExpanded((ulong)handle, value ? (byte)1 : (byte)0),
            GetPropertyHasMultipleDifferentValues = getMultipleDifferent is null ? null : handle => getMultipleDifferent((ulong)handle) != 0,
            GetPropertyPrefabOverride = getPrefabOverride is null ? null : handle => getPrefabOverride((ulong)handle) != 0,
            GetPropertyDefaultOverride = getDefaultOverride is null ? null : handle => getDefaultOverride((ulong)handle) != 0,
            GetPropertyInstantiatedPrefab = getInstantiatedPrefab is null ? null : handle => getInstantiatedPrefab((ulong)handle) != 0,
            GetIterator = getIterator is null ? null : handle => (nint)getIterator((ulong)handle),
            NextProperty = nextProperty is null ? null : (handle, enterChildren, visible) =>
                (nint)nextProperty((ulong)handle, enterChildren ? (byte)1 : (byte)0, visible ? (byte)1 : (byte)0),
            GetEndProperty = getEndProperty is null ? null : (handle, includeInvisible) =>
                (nint)getEndProperty((ulong)handle, includeInvisible ? (byte)1 : (byte)0),
            CopyProperty = copyProperty is null ? null : handle => (nint)copyProperty((ulong)handle),
            EqualPropertyContents = equalPropertyContents is null ? null : (left, right) =>
                equalPropertyContents((ulong)left, (ulong)right) != 0,
            GetPropertyPathNative = getPropertyPath is null ? null : (handle, path) =>
                ReadPropertyText(getPropertyPath, (ulong)handle, path),
            GetTargetCount = getTargetCount is null ? null : handle => checked((int)getTargetCount((ulong)handle)),
            GetInspectorMode = getInspectorMode is null ? null : handle =>
                (SerializedInspectorMode)getInspectorMode((ulong)handle),
            SetInspectorMode = setInspectorMode is null ? null : (handle, mode) =>
                setInspectorMode((ulong)handle, (byte)mode),
            GetMaxArraySizeForMultiEditing = getMaxArraySizeForMultiEditing is null ? null :
                handle => checked((int)getMaxArraySizeForMultiEditing((ulong)handle)),
            SetMaxArraySizeForMultiEditing = setMaxArraySizeForMultiEditing is null ? null : (handle, value) =>
            {
                _ = setMaxArraySizeForMultiEditing((ulong)handle, checked((uint)value));
            },
            SetIsDifferentCacheDirty = setIsDifferentCacheDirty is null ? null : handle =>
                setIsDifferentCacheDirty((ulong)handle),
            CopyFromSerializedProperty = copyFromSerializedProperty is null ? null : (handle, property) =>
                copyFromSerializedProperty((ulong)handle, (ulong)property) != 0
                    ? SerializedNativeResult.Success
                    : SerializedNativeResult.Failed,
            GetPropertyVector2Int = getVector2Int is null ? null : (handle, path) =>
            {
                int x = 0, y = 0;
                if (getVector2Int((ulong)handle, &x, &y) == 0)
                    throw new InvalidOperationException($"Native Vector2Int getter failed for '{path}'.");
                return new global::Nullus.Vector2Int(x, y);
            },
            GetPropertyVector3Int = getVector3Int is null ? null : (handle, path) =>
            {
                int x = 0, y = 0, z = 0;
                if (getVector3Int((ulong)handle, &x, &y, &z) == 0)
                    throw new InvalidOperationException($"Native Vector3Int getter failed for '{path}'.");
                return new global::Nullus.Vector3Int(x, y, z);
            },
            GetPropertyRectInt = getRectInt is null ? null : (handle, path) =>
            {
                int x = 0, y = 0, width = 0, height = 0;
                if (getRectInt((ulong)handle, &x, &y, &width, &height) == 0)
                    throw new InvalidOperationException($"Native RectInt getter failed for '{path}'.");
                return new global::Nullus.RectInt(x, y, width, height);
            },
            GetPropertyBounds = getBounds is null ? null : (handle, path) =>
            {
                float centerX = 0, centerY = 0, centerZ = 0, sizeX = 0, sizeY = 0, sizeZ = 0;
                if (getBounds((ulong)handle, &centerX, &centerY, &centerZ, &sizeX, &sizeY, &sizeZ) == 0)
                    throw new InvalidOperationException($"Native Bounds getter failed for '{path}'.");
                return new global::Nullus.Bounds(new global::Nullus.Vector3(centerX, centerY, centerZ), new global::Nullus.Vector3(sizeX, sizeY, sizeZ));
            },
            GetPropertyBoundsInt = getBoundsInt is null ? null : (handle, path) =>
            {
                int positionX = 0, positionY = 0, positionZ = 0, sizeX = 0, sizeY = 0, sizeZ = 0;
                if (getBoundsInt((ulong)handle, &positionX, &positionY, &positionZ, &sizeX, &sizeY, &sizeZ) == 0)
                    throw new InvalidOperationException($"Native BoundsInt getter failed for '{path}'.");
                return new global::Nullus.BoundsInt(new global::Nullus.Vector3Int(positionX, positionY, positionZ), new global::Nullus.Vector3Int(sizeX, sizeY, sizeZ));
            },
            SetPropertyVector2Int = setVector2Int is null ? null : (handle, path, value) =>
                setVector2Int((ulong)handle, value.X, value.Y) != 0 ? SerializedNativeResult.Success : SerializedNativeResult.Failed,
            SetPropertyVector3Int = setVector3Int is null ? null : (handle, path, value) =>
                setVector3Int((ulong)handle, value.X, value.Y, value.Z) != 0 ? SerializedNativeResult.Success : SerializedNativeResult.Failed,
            SetPropertyRectInt = setRectInt is null ? null : (handle, path, value) =>
                setRectInt((ulong)handle, value.X, value.Y, value.Width, value.Height) != 0 ? SerializedNativeResult.Success : SerializedNativeResult.Failed,
            SetPropertyBounds = setBounds is null ? null : (handle, path, value) =>
                setBounds((ulong)handle, value.Center.X, value.Center.Y, value.Center.Z, value.Size.X, value.Size.Y, value.Size.Z) != 0 ? SerializedNativeResult.Success : SerializedNativeResult.Failed,
            SetPropertyBoundsInt = setBoundsInt is null ? null : (handle, path, value) =>
                setBoundsInt((ulong)handle, value.Position.X, value.Position.Y, value.Position.Z, value.Size.X, value.Size.Y, value.Size.Z) != 0 ? SerializedNativeResult.Success : SerializedNativeResult.Failed,
            GetPropertyEnumDisplayNames = getEnumDisplayNames is null ? null : (handle, path) =>
            {
                var size = getEnumDisplayNames((ulong)handle, 0, 0);
                if (size == 0)
                    return string.Empty;
                var buffer = Marshal.AllocCoTaskMem(checked((int)size + 1));
                try
                {
                    getEnumDisplayNames((ulong)handle, buffer, size + 1);
                    return Marshal.PtrToStringUTF8(buffer) ?? string.Empty;
                }
                finally
                {
                    Marshal.FreeCoTaskMem(buffer);
                }
            },
            GetPropertyAnimationCurveBytes = getAnimationCurve is null ? null : (handle, path) => ReadPropertyBytes(getAnimationCurve, (ulong)handle, path),
            SetPropertyAnimationCurveBytes = setAnimationCurve is null ? null : (handle, path, bytes) =>
                WritePropertyBytes(setAnimationCurve, (ulong)handle, path, bytes),
            GetPropertyGradientBytes = getGradient is null ? null : (handle, path) => ReadPropertyBytes(getGradient, (ulong)handle, path),
            SetPropertyGradientBytes = setGradient is null ? null : (handle, path, bytes) =>
                WritePropertyBytes(setGradient, (ulong)handle, path, bytes),
            GetPropertyExposedReferenceBytes = getExposedReference is null ? null : (handle, path) => ReadPropertyBytes(getExposedReference, (ulong)handle, path),
            SetPropertyExposedReferenceBytes = setExposedReference is null ? null : (handle, path, bytes) =>
                WritePropertyBytes(setExposedReference, (ulong)handle, path, bytes),
            CopyFromSerializedPropertyIfDifferent = copyFromSerializedPropertyIfDifferent is null ? null : (handle, property) =>
                copyFromSerializedPropertyIfDifferent((ulong)handle, (ulong)property) != 0
                    ? SerializedNativeResult.Success
                    : SerializedNativeResult.Failed,
            GetPropertyTypeName = getPropertyTypeName is null ? null : (handle, path) =>
                ReadPropertyText(getPropertyTypeName, (ulong)handle, path),
            GetPropertyExactTypeId = getPropertyExactTypeId is null ? null : handle =>
                getPropertyExactTypeId((ulong)handle),
            GetPropertyCountInProperty = getPropertyCountInProperty is null ? null : (handle, enterChildren) =>
                checked((int)getPropertyCountInProperty((ulong)handle, enterChildren ? (byte)1 : (byte)0)),
            GetPropertyCountRemaining = getPropertyCountRemaining is null ? null : handle =>
                checked((int)getPropertyCountRemaining((ulong)handle)),
            GetPropertyReadOnly = getPropertyReadOnly is null ? null : handle =>
                getPropertyReadOnly((ulong)handle) != 0,
            GetTargetObjectInstanceId = getTargetObjectInstanceId is null ? null : (handle, index) =>
                getTargetObjectInstanceId((ulong)handle, checked((uint)index)),
            GetLastError = getObjectLastError is null ? null : handle =>
            {
                var size = getObjectLastError((ulong)handle, 0, 0);
                if (size == 0)
                    return string.Empty;
                var buffer = Marshal.AllocCoTaskMem(checked((int)size + 1));
                try
                {
                    getObjectLastError((ulong)handle, buffer, size + 1);
                    return Marshal.PtrToStringUTF8(buffer) ?? string.Empty;
                }
                finally
                {
                    Marshal.FreeCoTaskMem(buffer);
                }
            },
            CreateObjectFromInstanceIds = createObjectFromInstanceIds is null ? null : instanceIds =>
            {
                ArgumentNullException.ThrowIfNull(instanceIds);
                if (instanceIds.Length == 0)
                    return 0;
                unsafe
                {
                    fixed (int* pointer = instanceIds)
                        return (nint)createObjectFromInstanceIds(pointer, checked((uint)instanceIds.Length));
                }
            },
            DiscardModifiedProperties = discardModifiedProperties is null ? null : handle =>
                discardModifiedProperties((ulong)handle),
            GetContextObjectInstanceId = getContextObjectInstanceId is null ? null : handle =>
                getContextObjectInstanceId((ulong)handle),
            GetResolvedExposedReferenceInstanceId = getResolvedExposedReferenceInstanceId is null ? null : handle =>
                getResolvedExposedReferenceInstanceId((ulong)handle),
            CreateObjectFromInstanceIdsWithContext = createObjectFromInstanceIdsWithContext is null ? null : (instanceIds, contextInstanceId) =>
            {
                ArgumentNullException.ThrowIfNull(instanceIds);
                if (instanceIds.Length == 0)
                    return 0;
                unsafe
                {
                    fixed (int* pointer = instanceIds)
                        return (nint)createObjectFromInstanceIdsWithContext(
                            pointer,
                            checked((uint)instanceIds.Length),
                            contextInstanceId);
                }
            },
            ResetProperty = resetProperty is null ? null : handle => resetProperty((ulong)handle),
            DataEquals = dataEquals is null ? null : (left, right) =>
                dataEquals((ulong)left, (ulong)right) != 0,
            DeletePropertyCommand = deletePropertyCommand is null ? null : handle =>
                deletePropertyCommand((ulong)handle) != 0 ? SerializedNativeResult.Success : SerializedNativeResult.Failed,
            DuplicatePropertyCommand = duplicatePropertyCommand is null ? null : handle =>
                duplicatePropertyCommand((ulong)handle) != 0 ? SerializedNativeResult.Success : SerializedNativeResult.Failed,
            GetPropertyArrayElementType = getPropertyArrayElementType is null ? null : handle =>
                ReadPropertyText(getPropertyArrayElementType, (ulong)handle, string.Empty),
            SetResolvedExposedReferenceInstanceId = setResolvedExposedReference is null ? null : (handle, instanceId) =>
                setResolvedExposedReference((ulong)handle, instanceId) != 0
                    ? SerializedNativeResult.Success
                    : SerializedNativeResult.Failed,
            GetPropertyIsAnimated = getPropertyIsAnimated is null ? null : handle =>
                getPropertyIsAnimated((ulong)handle) != 0,
            ReleaseProperty = releaseProperty is null ? null : handle => releaseProperty((ulong)handle),
            IsPropertyValid = isPropertyValid is null ? null : handle => isPropertyValid((ulong)handle) != 0
        };
        }
    }

    internal bool QueryIsValid(nint handle) => IsValid?.Invoke(handle) ?? handle != 0;
    internal bool QueryPropertyIsValid(nint handle) => IsPropertyValid?.Invoke(handle) ?? handle != 0;
    internal void QueryRelease(nint handle) => ReleaseObject?.Invoke(handle);
    internal void QueryUpdate(nint handle) => Update?.Invoke(handle);
    internal bool QueryUpdateIfRequired(nint handle) => UpdateIfRequiredOrScript?.Invoke(handle) ?? false;
    internal nint QueryFindProperty(nint handle, string path) => FindProperty?.Invoke(handle, path) ?? 0;
    internal bool QueryHasModified(nint handle) => HasModifiedProperties?.Invoke(handle) ?? false;
    internal bool QueryApply(nint handle, bool withoutUndo) => ApplyModifiedProperties?.Invoke(handle, withoutUndo) ?? false;
}

[DebuggerDisplay("{PropertyPath}")]
public sealed class SerializedObject : IDisposable
{
    private static readonly EditorNativeApiTable EmptyApi = new();
    private readonly EditorNativeApiTable _api;
    private readonly ConcurrentDictionary<nint, WeakReference<SerializedProperty>> _properties = new();
    private nint _nativeHandle;
    private int _disposed;

    internal SerializedObject(nint nativeHandle, EditorNativeApiTable? api = null)
    {
        _nativeHandle = nativeHandle;
        _api = ResolveApi(api);
    }

    // An explicit table always wins. Otherwise use the table the editor host
    // installed for this domain so `new SerializedObject(target)` works without
    // the caller threading the ABI through. Without an installed host the empty
    // table keeps the existing "handle is stale" failure mode.
    private static EditorNativeApiTable ResolveApi(EditorNativeApiTable? api)
        => api ?? (EditorSerializationHost.IsInstalled ? EditorSerializationHost.Api : EmptyApi);

    public SerializedObject(global::Nullus.Object target, EditorNativeApiTable? api = null)
        : this(CreateNativeHandle(new[] { target }, api), api)
    {
        ArgumentNullException.ThrowIfNull(target);
    }

    public SerializedObject(global::Nullus.Object[] targets, EditorNativeApiTable? api = null)
        : this(CreateNativeHandle(targets, api), api)
    {
        ArgumentNullException.ThrowIfNull(targets);
    }

    public SerializedObject(
        global::Nullus.Object target,
        global::Nullus.Object context,
        EditorNativeApiTable? api = null)
        : this(CreateNativeHandle(new[] { target }, context, api), api)
    {
        ArgumentNullException.ThrowIfNull(target);
        ArgumentNullException.ThrowIfNull(context);
    }

    public SerializedObject(
        global::Nullus.Object[] targets,
        global::Nullus.Object context,
        EditorNativeApiTable? api = null)
        : this(CreateNativeHandle(targets, context, api), api)
    {
        ArgumentNullException.ThrowIfNull(targets);
        ArgumentNullException.ThrowIfNull(context);
    }

    // Native hosts use this factory after allocating a slot+generation handle.
    // A zero handle is allowed so callers can create an invalid wrapper and
    // receive the same exception behavior as a released native object.
    public static SerializedObject FromNativeHandle(nint nativeHandle, EditorNativeApiTable? api = null)
        => new(nativeHandle, api);

    public bool IsValid => Volatile.Read(ref _disposed) == 0 && _api.QueryIsValid(_nativeHandle);
    public bool isValid => IsValid;
    public string LastError
    {
        get
        {
            EnsureValid();
            return _api.GetLastError?.Invoke(_nativeHandle) ?? string.Empty;
        }
    }
    public string lastError => LastError;
    public bool IsEditingMultipleObjects
    {
        get
        {
            EnsureValid();
            return TargetCount > 1;
        }
    }
    public bool isEditingMultipleObjects => IsEditingMultipleObjects;
    public int TargetCount
    {
        get
        {
            EnsureValid();
            return _api.GetTargetCount?.Invoke(_nativeHandle) ?? 1;
        }
    }
    public int targetCount => TargetCount;
    public global::Nullus.Object? TargetObject => ResolveTargetObject(0);
    public global::Nullus.Object? targetObject => TargetObject;
    public global::Nullus.Object[] TargetObjects
    {
        get
        {
            EnsureValid();
            var result = new List<global::Nullus.Object>();
            for (var index = 0; index < TargetCount; ++index)
            {
                var target = ResolveTargetObject(index);
                if (target is not null)
                    result.Add(target);
            }
            return result.ToArray();
        }
    }
    public global::Nullus.Object[] targetObjects => TargetObjects;
    public global::Nullus.Object? Context
    {
        get
        {
            EnsureValid();
            return ResolveObjectFromInstanceId(_api.GetContextObjectInstanceId?.Invoke(_nativeHandle) ?? 0);
        }
    }
    public global::Nullus.Object? context => Context;
    public bool HasModifiedProperties
    {
        get
        {
            EnsureValid();
            return _api.QueryHasModified(_nativeHandle);
        }
    }
    public bool hasModifiedProperties => HasModifiedProperties;

    public SerializedInspectorMode InspectorMode
    {
        get
        {
            EnsureValid();
            return _api.GetInspectorMode?.Invoke(_nativeHandle) ?? SerializedInspectorMode.Normal;
        }
        set
        {
            EnsureValid();
            _api.SetInspectorMode?.Invoke(_nativeHandle, value);
        }
    }

    public SerializedInspectorMode inspectorMode
    {
        get => InspectorMode;
        set => InspectorMode = value;
    }

    public int MaxArraySizeForMultiEditing
    {
        get
        {
            EnsureValid();
            return _api.GetMaxArraySizeForMultiEditing?.Invoke(_nativeHandle) ?? 64;
        }
        set
        {
            EnsureValid();
            if (value < 0)
                throw new ArgumentOutOfRangeException(nameof(value));
            _api.SetMaxArraySizeForMultiEditing?.Invoke(_nativeHandle, value);
        }
    }

    public int maxArraySizeForMultiEditing
    {
        get => MaxArraySizeForMultiEditing;
        set => MaxArraySizeForMultiEditing = value;
    }

    public void Update()
    {
        EnsureValid();
        _api.QueryUpdate(_nativeHandle);
    }

    public bool UpdateIfRequiredOrScript()
    {
        EnsureValid();
        return _api.QueryUpdateIfRequired(_nativeHandle);
    }

    [Obsolete("UpdateIfDirtyOrScript has been deprecated. Use UpdateIfRequiredOrScript instead.", false)]
    public bool UpdateIfDirtyOrScript() => UpdateIfRequiredOrScript();

    public bool ApplyModifiedProperties()
    {
        EnsureValid();
        return _api.QueryApply(_nativeHandle, withoutUndo: false);
    }

    public bool ApplyModifiedPropertiesWithoutUndo()
    {
        EnsureValid();
        return _api.QueryApply(_nativeHandle, withoutUndo: true);
    }

    public void DiscardModifiedProperties()
    {
        EnsureValid();
        if (_api.DiscardModifiedProperties is null)
        {
            Update();
            return;
        }
        _api.DiscardModifiedProperties(_nativeHandle);
    }

    public void discardModifiedProperties() => DiscardModifiedProperties();

    public void SetIsDifferentCacheDirty()
    {
        EnsureValid();
        _api.SetIsDifferentCacheDirty?.Invoke(_nativeHandle);
    }

    public void setIsDifferentCacheDirty() => SetIsDifferentCacheDirty();

    public SerializedProperty? FindProperty(string propertyPath)
    {
        EnsureValid();
        ArgumentException.ThrowIfNullOrEmpty(propertyPath);
        var propertyHandle = _api.QueryFindProperty(_nativeHandle, propertyPath);
        if (propertyHandle == 0)
            return null;
        if (_properties.TryGetValue(propertyHandle, out var weak) && weak.TryGetTarget(out var existing))
            return existing;
        var created = new SerializedProperty(this, propertyHandle, propertyPath);
        _properties[propertyHandle] = new WeakReference<SerializedProperty>(created);
        return created;
    }

    public SerializedProperty GetIterator()
    {
        EnsureValid();
        var handle = _api.GetIterator?.Invoke(_nativeHandle) ?? 0;
        return new SerializedProperty(this, handle, string.Empty);
    }

    public SerializedProperty getIterator() => GetIterator();

    public void CopyFromSerializedProperty(SerializedProperty source)
    {
        if (!TryCopyFromSerializedProperty(source))
            throw new InvalidOperationException("The native SerializedProperty copy failed.");
    }

    public bool TryCopyFromSerializedProperty(SerializedProperty source)
    {
        ArgumentNullException.ThrowIfNull(source);
        EnsureValid();
        source.EnsureValidForOwner(source.SerializedObject);
        var result = _api.CopyFromSerializedProperty?.Invoke(_nativeHandle, source.NativeHandle);
        if (result is null)
            throw new NotSupportedException("The native property-copy operation is not available.");
        return result is SerializedNativeResult.Success;
    }

    public bool CopyFromSerializedPropertyIfDifferent(SerializedProperty source)
    {
        ArgumentNullException.ThrowIfNull(source);
        EnsureValid();
        source.EnsureValidForOwner(source.SerializedObject);
        var result = _api.CopyFromSerializedPropertyIfDifferent?.Invoke(_nativeHandle, source.NativeHandle);
        if (result is null)
        {
            // Older native tables only expose the unconditional copy. The
            // fallback cannot compare contents, so it reports whether the copy
            // itself was staged.
            return TryCopyFromSerializedProperty(source);
        }
        return result is SerializedNativeResult.Success;
    }

    public bool tryCopyFromSerializedProperty(SerializedProperty source)
        => TryCopyFromSerializedProperty(source);

    public bool copyFromSerializedPropertyIfDifferent(SerializedProperty source)
        => CopyFromSerializedPropertyIfDifferent(source);

    public SerializedEditSession BeginEditSession(string propertyPath)
    {
        EnsureValid();
        ArgumentException.ThrowIfNullOrEmpty(propertyPath);
        var property = FindProperty(propertyPath)
            ?? throw new ArgumentException($"SerializedProperty '{propertyPath}' was not found.", nameof(propertyPath));
        return new SerializedEditSession(this, property);
    }

    public SerializedEditSession beginEditSession(string propertyPath)
        => BeginEditSession(propertyPath);

    internal EditorNativeApiTable Api => _api;
    internal nint NativeHandle => _nativeHandle;

    internal void EnsureValid()
    {
        if (Volatile.Read(ref _disposed) != 0)
            throw new ObjectDisposedException(nameof(SerializedObject));
        if (!_api.QueryIsValid(_nativeHandle))
            throw new InvalidOperationException("The native SerializedObject handle is stale.");
    }

    private static nint CreateNativeHandle(
        global::Nullus.Object[] targets,
        EditorNativeApiTable? api)
        => CreateNativeHandle(targets, null, api);

    private static nint CreateNativeHandle(
        global::Nullus.Object[] targets,
        global::Nullus.Object? context,
        EditorNativeApiTable? api)
    {
        ArgumentNullException.ThrowIfNull(targets);
        if (targets.Length == 0)
            throw new ArgumentException("At least one target is required.", nameof(targets));
        var nativeApi = ResolveApi(api);
        var ids = new int[targets.Length];
        for (var index = 0; index < targets.Length; ++index)
        {
            ArgumentNullException.ThrowIfNull(targets[index], nameof(targets));
            ids[index] = checked((int)targets[index].GetInstanceID());
        }
        if (context is not null && nativeApi.CreateObjectFromInstanceIdsWithContext is { } createWithContext)
            return createWithContext(ids, checked((int)context.GetInstanceID()));
        var create = nativeApi.CreateObjectFromInstanceIds;
        return create is null ? 0 : create(ids);
    }

    private global::Nullus.Object? ResolveTargetObject(int index)
    {
        EnsureValid();
        if (index < 0 || index >= TargetCount)
            return null;
        var instanceId = _api.GetTargetObjectInstanceId?.Invoke(_nativeHandle, index) ?? 0;
        return ResolveObjectFromInstanceId(instanceId);
    }

    internal static global::Nullus.Object? ResolveObjectFromInstanceId(int instanceId)
    {
        if (instanceId == 0)
            return null;
        try
        {
            return global::Nullus.NativeObject.FromHandle<global::Nullus.Object>(
                new global::Nullus.NativeObjectHandle(unchecked((ulong)(uint)instanceId)));
        }
        catch
        {
            return null;
        }
    }

    public void Dispose()
    {
        if (Interlocked.Exchange(ref _disposed, 1) != 0)
            return;
        var handle = Interlocked.Exchange(ref _nativeHandle, 0);
        _properties.Clear();
        if (handle != 0)
        {
            try
            {
                _api.QueryRelease(handle);
            }
            catch
            {
                // Native editor shutdown may retire the ABI table before a
                // finalizer runs; released wrappers must never surface an
                // exception from the GC thread.
            }
        }
        GC.SuppressFinalize(this);
    }

    ~SerializedObject() => Dispose();
}

public sealed class SerializedEditSession : IDisposable
{
    private readonly SerializedObject _owner;
    private readonly SerializedProperty _property;
    private int _active = 1;

    internal SerializedEditSession(SerializedObject owner, SerializedProperty property)
    {
        _owner = owner;
        _property = property;
    }

    public bool IsActive => Volatile.Read(ref _active) != 0 && _property.IsValid;
    public bool isActive => IsActive;
    public SerializedProperty Property => _property;
    public SerializedProperty property => Property;

    public bool Preview(Action<SerializedProperty> edit)
    {
        ArgumentNullException.ThrowIfNull(edit);
        if (!IsActive)
            return false;
        edit(_property);
        return true;
    }

    public bool Commit()
    {
        if (Interlocked.Exchange(ref _active, 0) == 0)
            return false;
        return _owner.ApplyModifiedProperties();
    }

    public bool Cancel()
    {
        if (Interlocked.Exchange(ref _active, 0) == 0)
            return false;
        var changed = _owner.HasModifiedProperties;
        _owner.DiscardModifiedProperties();
        return changed;
    }

    public void Dispose()
    {
        if (IsActive)
            Cancel();
        GC.SuppressFinalize(this);
    }
}

[DebuggerDisplay("{PropertyPath}")]
public sealed class SerializedProperty : IDisposable, IEnumerable
{
    private readonly SerializedObject _owner;
    private nint _nativeHandle;
    private string _propertyPath;

    internal SerializedProperty(SerializedObject owner, nint nativeHandle, string propertyPath)
    {
        _owner = owner;
        _nativeHandle = nativeHandle;
        _propertyPath = propertyPath;
    }

    public SerializedObject SerializedObject => _owner;
    public SerializedObject serializedObject => _owner;
    public string PropertyPath => _propertyPath;
    internal nint NativeHandle => _nativeHandle;
    public string Name
    {
        get
        {
            EnsureValid();
            return _owner.Api.GetPropertyName?.Invoke(_nativeHandle, _propertyPath)
                ?? LastPathSegment(_propertyPath);
        }
    }
    public string DisplayName
    {
        get
        {
            EnsureValid();
            return _owner.Api.GetPropertyDisplayName?.Invoke(_nativeHandle, _propertyPath)
                ?? Name;
        }
    }
    public string Tooltip
    {
        get
        {
            EnsureValid();
            return _owner.Api.GetPropertyTooltip?.Invoke(_nativeHandle, _propertyPath) ?? string.Empty;
        }
    }
    public int Depth
    {
        get
        {
            EnsureValid();
            return _owner.Api.GetPropertyDepth?.Invoke(_nativeHandle) ?? CountDepth(_propertyPath);
        }
    }
    public bool HasChildren
    {
        get
        {
            EnsureValid();
            return _owner.Api.GetPropertyHasChildren?.Invoke(_nativeHandle) ?? IsArray;
        }
    }
    public bool HasVisibleChildren
    {
        get
        {
            EnsureValid();
            return _owner.Api.GetPropertyHasVisibleChildren?.Invoke(_nativeHandle) ?? HasChildren;
        }
    }
    public bool Editable
    {
        get
        {
            EnsureValid();
            return _owner.Api.GetPropertyEditable?.Invoke(_nativeHandle) ?? true;
        }
    }

    public bool IsReadOnly
    {
        get
        {
            EnsureValid();
            return _owner.Api.GetPropertyReadOnly?.Invoke(_nativeHandle) ?? !Editable;
        }
    }

    public bool isReadOnly => IsReadOnly;
    public bool IsAnimated
    {
        get
        {
            EnsureValid();
            return _owner.Api.GetPropertyIsAnimated?.Invoke(_nativeHandle) ?? false;
        }
    }

    public bool isAnimated => IsAnimated;
    public bool IsExpanded
    {
        get
        {
            EnsureValid();
            return _owner.Api.GetPropertyIsExpanded?.Invoke(_nativeHandle) ?? false;
        }
        set
        {
            EnsureValid();
            if (_owner.Api.SetPropertyIsExpanded is null)
                throw new NotSupportedException("The native expansion setter is not available.");
            _owner.Api.SetPropertyIsExpanded(_nativeHandle, value);
        }
    }
    public bool HasMultipleDifferentValues
    {
        get
        {
            EnsureValid();
            return _owner.Api.GetPropertyHasMultipleDifferentValues?.Invoke(_nativeHandle) ?? false;
        }
    }
    public bool PrefabOverride
    {
        get
        {
            EnsureValid();
            return _owner.Api.GetPropertyPrefabOverride?.Invoke(_nativeHandle) ?? false;
        }
    }
    public bool IsDefaultOverride
    {
        get
        {
            EnsureValid();
            return _owner.Api.GetPropertyDefaultOverride?.Invoke(_nativeHandle) ?? false;
        }
    }
    public bool IsInstantiatedPrefab
    {
        get
        {
            EnsureValid();
            return _owner.Api.GetPropertyInstantiatedPrefab?.Invoke(_nativeHandle) ?? false;
        }
    }
    public bool IsValid
    {
        get
        {
            try
            {
                _owner.EnsureValid();
                return _owner.Api.QueryPropertyIsValid(_nativeHandle);
            }
            catch (ObjectDisposedException)
            {
                return false;
            }
            catch (InvalidOperationException)
            {
                return false;
            }
        }
    }
    public bool isValid => IsValid;

    public SerializedPropertyType PropertyType
    {
        get
        {
            EnsureValid();
            return _owner.Api.GetPropertyType?.Invoke(_nativeHandle, _propertyPath)
                ?? SerializedPropertyType.Generic;
        }
    }

    public string Type
    {
        get
        {
            EnsureValid();
            return _owner.Api.GetPropertyTypeName?.Invoke(_nativeHandle, _propertyPath)
                ?? PropertyType.ToString();
        }
    }

    public ulong ExactTypeId
    {
        get
        {
            EnsureValid();
            return _owner.Api.GetPropertyExactTypeId?.Invoke(_nativeHandle) ?? 0UL;
        }
    }

    public SerializedNumericType NumericType
    {
        get
        {
            EnsureValid();
            return _owner.Api.GetPropertyNumericType?.Invoke(_nativeHandle, _propertyPath)
                ?? SerializedNumericType.Unknown;
        }
    }

    public bool BoolValue
    {
        get
        {
            EnsureValid();
            return _owner.Api.GetPropertyBool?.Invoke(_nativeHandle, _propertyPath) ?? false;
        }
        set => SetResult(_owner.Api.SetPropertyBool?.Invoke(_nativeHandle, _propertyPath, value));
    }

    public long LongValue
    {
        get
        {
            EnsureValid();
            return _owner.Api.GetPropertyLong?.Invoke(_nativeHandle, _propertyPath) ?? 0;
        }
        set => SetResult(_owner.Api.SetPropertyLong?.Invoke(_nativeHandle, _propertyPath, value));
    }

    public ulong UnsignedLongValue
    {
        get
        {
            EnsureValid();
            return _owner.Api.GetPropertyUnsignedLong?.Invoke(_nativeHandle, _propertyPath) ?? 0;
        }
        set => SetResult(_owner.Api.SetPropertyUnsignedLong?.Invoke(_nativeHandle, _propertyPath, value));
    }

    public double DoubleValue
    {
        get
        {
            EnsureValid();
            return _owner.Api.GetPropertyDouble?.Invoke(_nativeHandle, _propertyPath) ?? 0;
        }
        set => SetResult(_owner.Api.SetPropertyDouble?.Invoke(_nativeHandle, _propertyPath, value));
    }

    public string StringValue
    {
        get
        {
            EnsureValid();
            return _owner.Api.GetPropertyString?.Invoke(_nativeHandle, _propertyPath) ?? string.Empty;
        }
        set
        {
            EnsureValid();
            SetResult(_owner.Api.SetPropertyString?.Invoke(_nativeHandle, _propertyPath, value));
        }
    }

    public global::Nullus.Vector2 Vector2Value
    {
        get
        {
            EnsureValid();
            return _owner.Api.GetPropertyVector2?.Invoke(_nativeHandle, _propertyPath)
                ?? throw new NotSupportedException("The native Vector2 getter is not available.");
        }
        set => SetResult(_owner.Api.SetPropertyVector2?.Invoke(_nativeHandle, _propertyPath, value));
    }

    public global::Nullus.Vector3 Vector3Value
    {
        get
        {
            EnsureValid();
            return _owner.Api.GetPropertyVector3?.Invoke(_nativeHandle, _propertyPath)
                ?? throw new NotSupportedException("The native Vector3 getter is not available.");
        }
        set => SetResult(_owner.Api.SetPropertyVector3?.Invoke(_nativeHandle, _propertyPath, value));
    }

    public global::Nullus.Vector4 Vector4Value
    {
        get
        {
            EnsureValid();
            return _owner.Api.GetPropertyVector4?.Invoke(_nativeHandle, _propertyPath)
                ?? throw new NotSupportedException("The native Vector4 getter is not available.");
        }
        set => SetResult(_owner.Api.SetPropertyVector4?.Invoke(_nativeHandle, _propertyPath, value));
    }

    public global::Nullus.Quaternion QuaternionValue
    {
        get
        {
            EnsureValid();
            return _owner.Api.GetPropertyQuaternion?.Invoke(_nativeHandle, _propertyPath)
                ?? throw new NotSupportedException("The native Quaternion getter is not available.");
        }
        set => SetResult(_owner.Api.SetPropertyQuaternion?.Invoke(_nativeHandle, _propertyPath, value));
    }

    public global::Nullus.Color ColorValue
    {
        get
        {
            EnsureValid();
            return _owner.Api.GetPropertyColor?.Invoke(_nativeHandle, _propertyPath)
                ?? throw new NotSupportedException("The native Color getter is not available.");
        }
        set => SetResult(_owner.Api.SetPropertyColor?.Invoke(_nativeHandle, _propertyPath, value));
    }

    public global::Nullus.Rect RectValue
    {
        get
        {
            EnsureValid();
            return _owner.Api.GetPropertyRect?.Invoke(_nativeHandle, _propertyPath)
                ?? throw new NotSupportedException("The native Rect getter is not available.");
        }
        set => SetResult(_owner.Api.SetPropertyRect?.Invoke(_nativeHandle, _propertyPath, value));
    }

    public global::Nullus.Vector2Int Vector2IntValue
    {
        get
        {
            EnsureValid();
            return _owner.Api.GetPropertyVector2Int?.Invoke(_nativeHandle, _propertyPath)
                ?? throw new NotSupportedException("The native Vector2Int getter is not available.");
        }
        set => SetResult(_owner.Api.SetPropertyVector2Int?.Invoke(_nativeHandle, _propertyPath, value));
    }

    public global::Nullus.Vector3Int Vector3IntValue
    {
        get
        {
            EnsureValid();
            return _owner.Api.GetPropertyVector3Int?.Invoke(_nativeHandle, _propertyPath)
                ?? throw new NotSupportedException("The native Vector3Int getter is not available.");
        }
        set => SetResult(_owner.Api.SetPropertyVector3Int?.Invoke(_nativeHandle, _propertyPath, value));
    }

    public global::Nullus.RectInt RectIntValue
    {
        get
        {
            EnsureValid();
            return _owner.Api.GetPropertyRectInt?.Invoke(_nativeHandle, _propertyPath)
                ?? throw new NotSupportedException("The native RectInt getter is not available.");
        }
        set => SetResult(_owner.Api.SetPropertyRectInt?.Invoke(_nativeHandle, _propertyPath, value));
    }

    public global::Nullus.Bounds BoundsValue
    {
        get
        {
            EnsureValid();
            return _owner.Api.GetPropertyBounds?.Invoke(_nativeHandle, _propertyPath)
                ?? throw new NotSupportedException("The native Bounds getter is not available.");
        }
        set => SetResult(_owner.Api.SetPropertyBounds?.Invoke(_nativeHandle, _propertyPath, value));
    }

    public global::Nullus.BoundsInt BoundsIntValue
    {
        get
        {
            EnsureValid();
            return _owner.Api.GetPropertyBoundsInt?.Invoke(_nativeHandle, _propertyPath)
                ?? throw new NotSupportedException("The native BoundsInt getter is not available.");
        }
        set => SetResult(_owner.Api.SetPropertyBoundsInt?.Invoke(_nativeHandle, _propertyPath, value));
    }

    public global::Nullus.AnimationCurve AnimationCurveValue
    {
        get
        {
            EnsureValid();
            var getter = _owner.Api.GetPropertyAnimationCurveBytes
                ?? throw new NotSupportedException("The native AnimationCurve getter is not available.");
            var bytes = getter(_nativeHandle, _propertyPath);
            if (!SerializedSpecialValueCodec.TryDecodeAnimationCurve(bytes, out var value))
                throw new InvalidOperationException($"Native AnimationCurve payload is invalid for '{_propertyPath}'.");
            return value;
        }
        set
        {
            EnsureValid();
            var setter = _owner.Api.SetPropertyAnimationCurveBytes
                ?? throw new NotSupportedException("The native AnimationCurve setter is not available.");
            SetResult(setter(_nativeHandle, _propertyPath, SerializedSpecialValueCodec.Encode(value)));
        }
    }

    public global::Nullus.Gradient GradientValue
    {
        get
        {
            EnsureValid();
            var getter = _owner.Api.GetPropertyGradientBytes
                ?? throw new NotSupportedException("The native Gradient getter is not available.");
            var bytes = getter(_nativeHandle, _propertyPath);
            if (!SerializedSpecialValueCodec.TryDecodeGradient(bytes, out var value))
                throw new InvalidOperationException($"Native Gradient payload is invalid for '{_propertyPath}'.");
            return value;
        }
        set
        {
            EnsureValid();
            var setter = _owner.Api.SetPropertyGradientBytes
                ?? throw new NotSupportedException("The native Gradient setter is not available.");
            SetResult(setter(_nativeHandle, _propertyPath, SerializedSpecialValueCodec.Encode(value)));
        }
    }

    // Raw serialized carrier. This is a Nullus extension for tooling that
    // deliberately edits the ExposedReference name/default pair. the external editor's
    // exposedReferenceValue API below instead exposes the context-resolved
    // Object value.
    public global::Nullus.ExposedReference<global::Nullus.GameObject> RawExposedReferenceValue
    {
        get
        {
            EnsureValid();
            var getter = _owner.Api.GetPropertyExposedReferenceBytes
                ?? throw new NotSupportedException("The native ExposedReference getter is not available.");
            var bytes = getter(_nativeHandle, _propertyPath);
            if (!SerializedSpecialValueCodec.TryDecodeExposedReference<global::Nullus.GameObject>(bytes, out var value))
                throw new InvalidOperationException($"Native ExposedReference payload is invalid for '{_propertyPath}'.");
            return value;
        }
        set
        {
            EnsureValid();
            var setter = _owner.Api.SetPropertyExposedReferenceBytes
                ?? throw new NotSupportedException("The native ExposedReference setter is not available.");
            SetResult(setter(
                _nativeHandle,
                _propertyPath,
                SerializedSpecialValueCodec.Encode(value)));
        }
    }

    // editor-compatible resolver-facing API. With a SerializedObject context
    // native writes the IExposedPropertyTable equivalent; without one it stages
    // the serialized default reference.
    public global::Nullus.Object? ExposedReferenceValue
    {
        get
        {
            EnsureValid();
            if (PropertyType != SerializedPropertyType.ExposedReference)
                return null;
            var resolved = _owner.Api.GetResolvedExposedReferenceInstanceId?.Invoke(_nativeHandle) ?? 0;
            if (resolved != 0)
                return SerializedObject.ResolveObjectFromInstanceId(resolved);
            return RawExposedReferenceValue.DefaultValue;
        }
        set
        {
            EnsureValid();
            if (PropertyType != SerializedPropertyType.ExposedReference)
                throw new InvalidOperationException($"SerializedProperty '{_propertyPath}' is not an ExposedReference.");
            if (value is not null && value is not global::Nullus.GameObject)
            {
                throw new ArgumentException(
                    "Nullus ExposedReference currently requires a GameObject value.",
                    nameof(value));
            }
            var setter = _owner.Api.SetResolvedExposedReferenceInstanceId;
            if (setter is not null)
            {
                SetResult(setter(_nativeHandle, value is null ? 0 : checked((int)value.GetInstanceID())));
                return;
            }

            // Old native hosts cannot write a resolver table. Their safest
            // fallback is the serialized default, which still matches external editor
            // when SerializedObject.context is null.
            var raw = RawExposedReferenceValue;
            raw.DefaultValue = (global::Nullus.GameObject?)value;
            RawExposedReferenceValue = raw;
        }
    }

    public global::Nullus.Object? ResolvedExposedReferenceValue => ExposedReferenceValue;

    public int EnumValueIndex
    {
        get => checked((int)LongValue);
        set => LongValue = value;
    }

    public string[] EnumDisplayNames
    {
        get
        {
            EnsureValid();
            var value = _owner.Api.GetPropertyEnumDisplayNames?.Invoke(_nativeHandle, _propertyPath);
            return string.IsNullOrEmpty(value)
                ? Array.Empty<string>()
                : value.Split('\n', StringSplitOptions.None);
        }
    }

    public string[] EnumNames => EnumDisplayNames;

    public bool IsArray
    {
        get
        {
            EnsureValid();
            return _owner.Api.GetPropertyIsArray?.Invoke(_nativeHandle) ?? false;
        }
    }

    public int ArraySize
    {
        get
        {
            EnsureValid();
            return _owner.Api.GetPropertyArraySize?.Invoke(_nativeHandle) ?? 0;
        }
        set
        {
            EnsureValid();
            if (value < 0)
                throw new ArgumentOutOfRangeException(nameof(value));
            SetResult(_owner.Api.SetPropertyArraySize?.Invoke(_nativeHandle, value));
        }
    }

    public int MinArraySize
    {
        get
        {
            EnsureValid();
            return _owner.Api.GetPropertyMinArraySize?.Invoke(_nativeHandle) ?? 0;
        }
    }

    public int FixedBufferSize
    {
        get
        {
            EnsureValid();
            return _owner.Api.GetPropertyFixedBufferSize?.Invoke(_nativeHandle) ?? 0;
        }
    }

    public bool IsFixedBuffer => FixedBufferSize > 0;
    public string ArrayElementType
    {
        get
        {
            EnsureValid();
            if (!IsArray)
                return string.Empty;
            var schemaElementType = _owner.Api.GetPropertyArrayElementType?.Invoke(_nativeHandle);
            if (!string.IsNullOrEmpty(schemaElementType))
                return schemaElementType;
            var first = GetArrayElementAtIndex(0);
            return first?.Type ?? string.Empty;
        }
    }

    public int ObjectReferenceInstanceId
    {
        get
        {
            EnsureValid();
            return _owner.Api.GetPropertyObjectReferenceInstanceId?.Invoke(_nativeHandle) ?? 0;
        }
        set => SetResult(_owner.Api.SetPropertyObjectReferenceInstanceId?.Invoke(_nativeHandle, value));
    }

    // The editor ABI exposes a stable InstanceID bridge. The returned Object is
    // a managed wrapper resolved through the same NativeObject cache used by
    // runtime bindings; a zero id represents a cleared reference.
    public global::Nullus.Object? ObjectReferenceValue
    {
        get
        {
            var instanceId = ObjectReferenceInstanceId;
            return instanceId == 0
                ? null
                : global::Nullus.NativeObject.FromHandle<global::Nullus.Object>(
                    new global::Nullus.NativeObjectHandle(unchecked((ulong)(uint)instanceId)));
        }
        set => ObjectReferenceInstanceId = value is null
            ? 0
            : checked((int)value.GetInstanceID());
    }

    // editor-compatible lower-case spellings used by managed custom editors.
    public bool boolValue { get => BoolValue; set => BoolValue = value; }
    public int intValue
    {
        get => checked((int)LongValue);
        set => LongValue = value;
    }
    public sbyte sbyteValue
    {
        get => checked((sbyte)LongValue);
        set => LongValue = value;
    }
    public byte byteValue
    {
        get => checked((byte)UnsignedLongValue);
        set => UnsignedLongValue = value;
    }
    public short shortValue
    {
        get => checked((short)LongValue);
        set => LongValue = value;
    }
    public ushort ushortValue
    {
        get => checked((ushort)UnsignedLongValue);
        set => UnsignedLongValue = value;
    }
    public uint uintValue
    {
        get => checked((uint)UnsignedLongValue);
        set => UnsignedLongValue = value;
    }
    public ulong ulongValue { get => UnsignedLongValue; set => UnsignedLongValue = value; }
    public long longValue { get => LongValue; set => LongValue = value; }
    public float floatValue
    {
        get => checked((float)DoubleValue);
        set => DoubleValue = value;
    }
    public double doubleValue { get => DoubleValue; set => DoubleValue = value; }
    public char charValue
    {
        get => checked((char)LongValue);
        set => LongValue = value;
    }
    public string stringValue { get => StringValue; set => StringValue = value; }
    public global::Nullus.Vector2 vector2Value { get => Vector2Value; set => Vector2Value = value; }
    public global::Nullus.Vector3 vector3Value { get => Vector3Value; set => Vector3Value = value; }
    public global::Nullus.Vector4 vector4Value { get => Vector4Value; set => Vector4Value = value; }
    public global::Nullus.Quaternion quaternionValue { get => QuaternionValue; set => QuaternionValue = value; }
    public global::Nullus.Color colorValue { get => ColorValue; set => ColorValue = value; }
    public global::Nullus.Rect rectValue { get => RectValue; set => RectValue = value; }
    public global::Nullus.Vector2Int vector2IntValue { get => Vector2IntValue; set => Vector2IntValue = value; }
    public global::Nullus.Vector3Int vector3IntValue { get => Vector3IntValue; set => Vector3IntValue = value; }
    public global::Nullus.RectInt rectIntValue { get => RectIntValue; set => RectIntValue = value; }
    public global::Nullus.Bounds boundsValue { get => BoundsValue; set => BoundsValue = value; }
    public global::Nullus.BoundsInt boundsIntValue { get => BoundsIntValue; set => BoundsIntValue = value; }
    public global::Nullus.AnimationCurve animationCurveValue { get => AnimationCurveValue; set => AnimationCurveValue = value; }
    public global::Nullus.Gradient gradientValue { get => GradientValue; set => GradientValue = value; }
    public global::Nullus.Object? exposedReferenceValue
    {
        get => ExposedReferenceValue;
        set => ExposedReferenceValue = value;
    }
    public global::Nullus.ExposedReference<global::Nullus.GameObject> rawExposedReferenceValue
    {
        get => RawExposedReferenceValue;
        set => RawExposedReferenceValue = value;
    }
    public global::Nullus.Object? resolvedExposedReferenceValue => ResolvedExposedReferenceValue;
    public int enumValueIndex { get => EnumValueIndex; set => EnumValueIndex = value; }
    public string[] enumDisplayNames => EnumDisplayNames;
    public string[] enumNames => EnumNames;
    public bool isArray => IsArray;
    public int arraySize { get => ArraySize; set => ArraySize = value; }
    public int minArraySize => MinArraySize;
    public int fixedBufferSize => FixedBufferSize;
    public bool isFixedBuffer => IsFixedBuffer;
    public string arrayElementType => ArrayElementType;
    public int objectReferenceInstanceIDValue
    {
        get => ObjectReferenceInstanceId;
        set => ObjectReferenceInstanceId = value;
    }
    public global::Nullus.Object? objectReferenceValue
    {
        get => ObjectReferenceValue;
        set => ObjectReferenceValue = value;
    }
    public SerializedPropertyType propertyType => PropertyType;
    public SerializedNumericType numericType => NumericType;
    public string propertyPath => PropertyPath;
    public string name => Name;
    public string displayName => DisplayName;
    public string tooltip => Tooltip;
    public int depth => Depth;
    public bool hasChildren => HasChildren;
    public bool hasVisibleChildren => HasVisibleChildren;
    public bool editable => Editable;
    public bool isExpanded { get => IsExpanded; set => IsExpanded = value; }
    public bool hasMultipleDifferentValues => HasMultipleDifferentValues;
    public bool prefabOverride => PrefabOverride;
    public bool isDefaultOverride => IsDefaultOverride;
    public bool isInstantiatedPrefab => IsInstantiatedPrefab;
    public string type => Type;
    public ulong exactTypeId => ExactTypeId;

    public SerializedProperty? FindPropertyRelative(string relativePath)
    {
        EnsureValid();
        ArgumentException.ThrowIfNullOrEmpty(relativePath);
        return _owner.FindProperty(string.IsNullOrEmpty(_propertyPath)
            ? relativePath
            : $"{_propertyPath}.{relativePath}");
    }

    public SerializedProperty? GetArrayElementAtIndex(int index)
    {
        if (index < 0)
            throw new ArgumentOutOfRangeException(nameof(index));
        EnsureValid();
        return _owner.FindProperty($"{_propertyPath}.Array.data[{index}]");
    }

    public SerializedProperty? GetFixedBufferElementAtIndex(int index)
    {
        if (!IsFixedBuffer)
            throw new InvalidOperationException($"SerializedProperty '{_propertyPath}' is not a fixed buffer.");
        return GetArrayElementAtIndex(index);
    }

    public void InsertArrayElementAtIndex(int index)
    {
        if (index < 0)
            throw new ArgumentOutOfRangeException(nameof(index));
        EnsureValid();
        SetResult(_owner.Api.InsertPropertyArrayElement?.Invoke(_nativeHandle, index));
    }

    public void DeleteArrayElementAtIndex(int index)
    {
        if (index < 0)
            throw new ArgumentOutOfRangeException(nameof(index));
        EnsureValid();
        SetResult(_owner.Api.DeletePropertyArrayElement?.Invoke(_nativeHandle, index));
    }

    public void MoveArrayElement(int sourceIndex, int destinationIndex)
    {
        if (sourceIndex < 0)
            throw new ArgumentOutOfRangeException(nameof(sourceIndex));
        if (destinationIndex < 0)
            throw new ArgumentOutOfRangeException(nameof(destinationIndex));
        EnsureValid();
        SetResult(_owner.Api.MovePropertyArrayElement?.Invoke(_nativeHandle, sourceIndex, destinationIndex));
    }

    public void ClearArray()
    {
        EnsureValid();
        SetResult(_owner.Api.ClearPropertyArray?.Invoke(_nativeHandle));
    }

    public SerializedProperty Copy()
    {
        EnsureValid();
        var handle = _owner.Api.CopyProperty?.Invoke(_nativeHandle) ?? 0;
        return handle == 0
            ? new SerializedProperty(_owner, _nativeHandle, _propertyPath)
            : new SerializedProperty(_owner, handle, ReadNativePath(handle));
    }

    public void Reset()
    {
        EnsureValid();
        var reset = _owner.Api.ResetProperty
            ?? throw new NotSupportedException("The native SerializedProperty reset operation is not available.");
        reset(_nativeHandle);
        _propertyPath = ReadNativePath(_nativeHandle);
    }

    public bool DeleteCommand()
    {
        EnsureValid();
        var command = _owner.Api.DeletePropertyCommand
            ?? throw new NotSupportedException("The native SerializedProperty delete command is not available.");
        var result = command(_nativeHandle);
        return result is SerializedNativeResult.Success;
    }

    public bool DuplicateCommand()
    {
        EnsureValid();
        var command = _owner.Api.DuplicatePropertyCommand
            ?? throw new NotSupportedException("The native SerializedProperty duplicate command is not available.");
        var result = command(_nativeHandle);
        return result is SerializedNativeResult.Success;
    }

    // the established editor baseline counts this visible property and its visible descendants.
    // It intentionally exposes no enter-children policy to callers.
    public int CountInProperty()
    {
        EnsureValid();
        return _owner.Api.GetPropertyCountInProperty?.Invoke(_nativeHandle, true) ?? 0;
    }

    public int CountRemaining()
    {
        EnsureValid();
        return _owner.Api.GetPropertyCountRemaining?.Invoke(_nativeHandle) ?? 0;
    }

    public int countInProperty => CountInProperty();
    public int countRemaining => CountRemaining();

    public bool Next(bool enterChildren = true) => Advance(enterChildren, visible: false);
    public bool NextVisible(bool enterChildren = true) => Advance(enterChildren, visible: true);

    public SerializedProperty GetEndProperty(bool includeInvisible = false)
    {
        EnsureValid();
        var handle = _owner.Api.GetEndProperty?.Invoke(_nativeHandle, includeInvisible) ?? 0;
        if (handle == 0)
            return new SerializedProperty(_owner, 0, string.Empty);
        return new SerializedProperty(_owner, handle, ReadNativePath(handle));
    }

    public static bool EqualContents(SerializedProperty left, SerializedProperty right)
    {
        ArgumentNullException.ThrowIfNull(left);
        ArgumentNullException.ThrowIfNull(right);
        if (!ReferenceEquals(left._owner, right._owner))
            return false;
        if (left._nativeHandle == 0 && right._nativeHandle == 0)
            return true;
        return left._owner.Api.EqualPropertyContents?.Invoke(left._nativeHandle, right._nativeHandle)
            ?? left._propertyPath == right._propertyPath;
    }

    public bool DataEquals(SerializedProperty other)
    {
        ArgumentNullException.ThrowIfNull(other);
        EnsureValid();
        other.EnsureValid();
        return _owner.Api.DataEquals?.Invoke(_nativeHandle, other._nativeHandle)
            ?? EqualContents(this, other);
    }

    public static bool DataEquals(SerializedProperty left, SerializedProperty right)
    {
        ArgumentNullException.ThrowIfNull(left);
        return left.DataEquals(right);
    }

    public IEnumerator GetEnumerator()
    {
        EnsureValid();
        if (!IsArray)
            yield break;
        for (var index = 0; index < ArraySize; ++index)
        {
            var element = GetArrayElementAtIndex(index);
            if (element is not null)
                yield return element;
        }
    }

    public void Dispose()
    {
        var handle = Interlocked.Exchange(ref _nativeHandle, 0);
        if (handle == 0)
            return;
        try
        {
            _owner.Api.ReleaseProperty?.Invoke(handle);
        }
        catch
        {
            // The native table may already be retired during editor shutdown.
        }
        GC.SuppressFinalize(this);
    }

    ~SerializedProperty() => Dispose();

    private void EnsureValid()
    {
        _owner.EnsureValid();
        if (!_owner.Api.QueryPropertyIsValid(_nativeHandle))
            throw new InvalidOperationException($"SerializedProperty '{_propertyPath}' is stale.");
    }

    internal void EnsureValidForOwner(SerializedObject owner)
    {
        if (!ReferenceEquals(_owner, owner))
            throw new ArgumentException("The SerializedProperty belongs to a different SerializedObject.", nameof(owner));
        EnsureValid();
    }

    private bool Advance(bool enterChildren, bool visible)
    {
        EnsureValid();
        var previous = _nativeHandle;
        var next = _owner.Api.NextProperty?.Invoke(_nativeHandle, enterChildren, visible) ?? 0;
        if (previous != 0 && next != previous)
        {
            try
            {
                _owner.Api.ReleaseProperty?.Invoke(previous);
            }
            catch
            {
                // The native registry may already have invalidated the cursor.
            }
        }
        if (next == 0)
        {
            _nativeHandle = 0;
            _propertyPath = string.Empty;
            return false;
        }
        _nativeHandle = next;
        _propertyPath = ReadNativePath(next);
        return true;
    }

    private string ReadNativePath(nint handle)
        => _owner.Api.GetPropertyPathNative?.Invoke(handle, _propertyPath) ?? _propertyPath;

    private static string LastPathSegment(string path)
    {
        var separator = path.LastIndexOf('.');
        return separator >= 0 && separator + 1 < path.Length ? path[(separator + 1)..] : path;
    }

    private static int CountDepth(string path)
    {
        if (string.IsNullOrEmpty(path))
            return 0;
        var depth = 0;
        foreach (var character in path)
        {
            if (character == '.')
                depth++;
        }
        return depth;
    }

    private void SetResult(SerializedNativeResult? result)
    {
        EnsureValid();
        if (result is null)
            throw new NotSupportedException("The native SerializedProperty setter is not available.");
        if (result is SerializedNativeResult.Success)
            return;
        throw result switch
        {
            SerializedNativeResult.InvalidArgument => new ArgumentException($"Invalid value for '{_propertyPath}'."),
            SerializedNativeResult.TypeMismatch => new ArgumentException($"Value type does not match '{_propertyPath}'."),
            SerializedNativeResult.ReadOnly => new InvalidOperationException($"SerializedProperty '{_propertyPath}' is read-only."),
            SerializedNativeResult.Stale => new InvalidOperationException($"SerializedProperty '{_propertyPath}' is stale."),
            _ => new InvalidOperationException($"Failed to set SerializedProperty '{_propertyPath}'.")
        };
    }
}
