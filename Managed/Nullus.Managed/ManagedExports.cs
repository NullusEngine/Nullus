using System.Collections.Generic;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using System.Buffers.Binary;
using System.Diagnostics;
using System.Security.Cryptography;
using System.Text;

namespace Nullus;

// The native host obtains this table through hostfxr.  All entry points use
// unmanaged blittable arguments and convert failures into ScriptAbiResult;
// managed exceptions never cross the scripting ABI.
[DebuggerNonUserCode]
public static unsafe class ManagedExports
{
    private sealed class Instance
    {
        public required Behaviour Behaviour { get; init; }
        public required ulong Owner { get; init; }
        public required string AssetPath { get; init; }
        public required ManagedScriptRegistry.Registration Registration { get; init; }
    }

    private static readonly object Gate = new();
    private static readonly Dictionary<ulong, string> LoadedAssets = new();
    private static readonly Dictionary<ulong, Instance> Instances = new();
    private static readonly List<IntPtr> AbiBuffers = new();
    private static ScriptAbiDiagnostic _lastDiagnostic;
    private static IntPtr _lastDiagnosticSourcePath;
    private static IntPtr _lastDiagnosticMessage;
    private static IntPtr _lastDiagnosticStackTrace;
    private static IntPtr _behaviourManifest;
    private static ulong _nextToken = 1;
    private static bool _initialized;
    private static int _assemblyVersion;
    private static ScriptAssemblyLoadContext? _projectLoadContext;
    private static readonly List<WeakReference<ScriptAssemblyLoadContext>> ProjectLoadContexts = new();
    private static ManagedApiTable _apiTable;

    static ManagedExports()
    {
        _apiTable = new ManagedApiTable
        {
            Header = new ScriptAbiHeader { Size = (uint)sizeof(ManagedApiTable), AbiVersion = 2 },
            Initialize = &Initialize,
            Shutdown = &Shutdown,
            LoadScript = &LoadScript,
            CreateInstance = &CreateInstance,
            DestroyInstance = &DestroyInstance,
            Invoke = &Invoke,
            Reload = &Reload,
            ReloadAssembly = &ReloadAssembly,
            GetField = &GetField,
            SetField = &SetField,
            InvokeBatch = &InvokeBatch,
            GetLastDiagnostic = &GetLastDiagnostic,
            GetBehaviourManifest = &GetBehaviourManifest
        };
    }

    [UnmanagedCallersOnly]
    public static IntPtr GetApiTable()
        => GetApiTableManaged();

    // Used by the small forwarding export in GameScripts.dll.  Keeping the
    // table storage in Nullus.Managed means the native ABI remains stable
    // while loading the project assembly still runs its module initializer.
    public static IntPtr GetApiTableManaged()
    {
        fixed (ManagedApiTable* table = &_apiTable)
            return (IntPtr)table;
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct ManagedApiTable
    {
        public ScriptAbiHeader Header;
         public delegate* unmanaged<ScriptAbiHeader*, byte*, NativeApiTable*, ScriptAbiResult> Initialize;
        public delegate* unmanaged<ScriptAbiResult> Shutdown;
        public delegate* unmanaged<ulong, byte*, byte*, ulong*, ScriptAbiResult> LoadScript;
        public delegate* unmanaged<ulong, ulong, ulong*, ScriptAbiResult> CreateInstance;
        public delegate* unmanaged<ulong, ScriptAbiResult> DestroyInstance;
        public delegate* unmanaged<ulong, ushort, ScriptFrameContext*, ulong, ScriptAbiResult> Invoke;
        public delegate* unmanaged<ulong, byte*, byte*, ScriptAbiResult> Reload;
        public delegate* unmanaged<ulong, byte*, byte*, ScriptAbiResult> ReloadAssembly;
        public delegate* unmanaged<ulong, ulong, ScriptAbiValue*, ScriptAbiResult> GetField;
        public delegate* unmanaged<ulong, ulong, ScriptAbiValue*, ScriptAbiResult> SetField;
        public delegate* unmanaged<ushort, ulong*, ulong*, uint, ScriptFrameContext*, ScriptAbiResult> InvokeBatch;
        public delegate* unmanaged<ScriptAbiDiagnostic*> GetLastDiagnostic;
        public delegate* unmanaged<byte**, uint*, ScriptAbiResult> GetBehaviourManifest;
    }

    private static ScriptAbiResult Ok() => new() { Code = 0 };

    private static void ReleaseLastDiagnostic()
    {
        if (_lastDiagnosticSourcePath != IntPtr.Zero)
            Marshal.FreeCoTaskMem(_lastDiagnosticSourcePath);
        if (_lastDiagnosticMessage != IntPtr.Zero)
            Marshal.FreeCoTaskMem(_lastDiagnosticMessage);
        if (_lastDiagnosticStackTrace != IntPtr.Zero)
            Marshal.FreeCoTaskMem(_lastDiagnosticStackTrace);
        _lastDiagnosticSourcePath = IntPtr.Zero;
        _lastDiagnosticMessage = IntPtr.Zero;
        _lastDiagnosticStackTrace = IntPtr.Zero;
        _lastDiagnostic = default;
    }

    private static void ReleaseBehaviourManifest()
    {
        if (_behaviourManifest != IntPtr.Zero)
            Marshal.FreeCoTaskMem(_behaviourManifest);
        _behaviourManifest = IntPtr.Zero;
    }

    private static ScriptAbiResult Error(
        byte code,
        string message,
        Exception? exception = null,
        string? assetPath = null)
    {
        lock (Gate)
        {
            ReleaseLastDiagnostic();
            ReleaseBehaviourManifest();
            var frames = exception is null ? null : new StackTrace(exception, true).GetFrames();
            StackFrame? userFrame = null;
            if (frames is not null)
            {
                foreach (var frame in frames)
                {
                    if (!string.IsNullOrWhiteSpace(frame.GetFileName()))
                    {
                        userFrame = frame;
                        break;
                    }
                }
            }
            var sourcePath = !string.IsNullOrWhiteSpace(assetPath)
                ? assetPath
                : userFrame?.GetFileName() ?? string.Empty;
            var line = userFrame?.GetFileLineNumber() ?? 0;
            var column = userFrame?.GetFileColumnNumber() ?? 0;
            _lastDiagnosticSourcePath = string.IsNullOrEmpty(sourcePath)
                ? IntPtr.Zero
                : Marshal.StringToCoTaskMemUTF8(sourcePath);
            _lastDiagnosticMessage = Marshal.StringToCoTaskMemUTF8(message);
            var stack = exception?.ToString() ?? string.Empty;
            _lastDiagnosticStackTrace = string.IsNullOrEmpty(stack)
                ? IntPtr.Zero
                : Marshal.StringToCoTaskMemUTF8(stack);
            _lastDiagnostic = new ScriptAbiDiagnostic
            {
                Size = (uint)sizeof(ScriptAbiDiagnostic),
                Severity = 2,
                Line = line,
                Column = column,
                SourcePath = _lastDiagnosticSourcePath,
                Message = _lastDiagnosticMessage,
                StackTrace = _lastDiagnosticStackTrace
            };
            return new ScriptAbiResult { Code = code, Message = _lastDiagnosticMessage };
        }
    }

    [UnmanagedCallersOnly]
    private static ScriptAbiDiagnostic* GetLastDiagnostic()
    {
        fixed (ScriptAbiDiagnostic* diagnostic = &_lastDiagnostic)
            return diagnostic;
    }

    [UnmanagedCallersOnly]
    private static ScriptAbiResult GetBehaviourManifest(byte** data, uint* size)
    {
        try
        {
            if (data == null || size == null)
                return Error(1, "Behaviour manifest output pointers are null.");
            lock (Gate)
            {
                ReleaseBehaviourManifest();
                var json = ManagedScriptRegistry.GetManifest();
                var bytes = Encoding.UTF8.GetBytes(json);
                _behaviourManifest = Marshal.AllocCoTaskMem(bytes.Length + 1);
                Marshal.Copy(bytes, 0, _behaviourManifest, bytes.Length);
                Marshal.WriteByte(_behaviourManifest + bytes.Length, 0);
                *data = (byte*)_behaviourManifest;
                *size = (uint)bytes.Length;
            }
            return Ok();
        }
        catch (Exception exception)
        {
            return Error(16, exception.ToString(), exception);
        }
    }

    private static string ReadUtf8(byte* value)
        => value == null ? string.Empty : Marshal.PtrToStringUTF8((IntPtr)value) ?? string.Empty;

    private static bool TryParseHash(string value, out ulong hash)
    {
        hash = 0;
        if (value.Length < 16)
            return false;
        return ulong.TryParse(value[..16], System.Globalization.NumberStyles.HexNumber, null, out hash);
    }

    [UnmanagedCallersOnly]
    private static ScriptAbiResult Initialize(ScriptAbiHeader* header, byte* schemaHashHex, NativeApiTable* nativeApi)
    {
        try
        {
            if (header == null || header->Size < (uint)sizeof(ScriptAbiHeader) || header->AbiVersion != 2)
                return Error(10, "Managed scripting ABI header is unsupported.");
            if (nativeApi != null
                && (nativeApi->Header.Size < (uint)sizeof(NativeApiTable)
                    || nativeApi->Header.AbiVersion != 2))
                return Error(
                    10,
                    $"Native scripting ABI table is unsupported (received size={nativeApi->Header.Size}, " +
                    $"version={nativeApi->Header.AbiVersion}, expected size>={sizeof(NativeApiTable)}, version=2).");
            var schema = ReadUtf8(schemaHashHex);
            if (!TryParseHash(ScriptApiManifest.SchemaHash, out var managedHash)
                || !TryParseHash(schema, out var nativeHash)
                || managedHash != nativeHash)
                return Error(10, "Managed and Native Script API schema hashes do not match.");
            if (nativeApi != null && nativeApi->Header.SchemaHash != nativeHash)
                return Error(10, "Native scripting ABI schema hash does not match the managed manifest.");
            header->SchemaHash = managedHash;
            if (nativeApi != null)
                NativeBinding.Configure(*nativeApi);
            lock (Gate)
                _initialized = true;
            return Ok();
        }
        catch (Exception exception)
        {
            return Error(16, exception.Message, exception);
        }
    }

    [UnmanagedCallersOnly]
    private static ScriptAbiResult Shutdown()
    {
        lock (Gate)
        {
            Instances.Clear();
            LoadedAssets.Clear();
            ManagedScriptRegistry.Reset();
            ReleaseBehaviourManifest();
            _projectLoadContext?.Unload();
            _projectLoadContext = null;
            ProjectLoadContexts.Clear();
            _assemblyVersion = 0;
            _initialized = false;
            ReleaseLastDiagnostic();
            foreach (var pointer in AbiBuffers)
                Marshal.FreeCoTaskMem(pointer);
            AbiBuffers.Clear();
        }
        return Ok();
    }

    [UnmanagedCallersOnly]
    private static ScriptAbiResult LoadScript(ulong assetId, byte* assetPath, byte* source, ulong* contentHash)
    {
        if (!_initialized)
            return Error(6, "Managed scripting runtime is not initialized.");
        if (assetId == 0 || contentHash == null)
            return Error(1, "Managed script loading requires a valid asset id and content hash pointer.");
        var path = ReadUtf8(assetPath);
        var sourceText = ReadUtf8(source);
        if (string.IsNullOrWhiteSpace(path) || string.IsNullOrWhiteSpace(sourceText))
            return Error(14, "Managed ScriptAsset source is empty.");
        var digest = SHA256.HashData(Encoding.UTF8.GetBytes(sourceText));
        *contentHash = BinaryPrimitives.ReadUInt64BigEndian(digest);
        lock (Gate)
            LoadedAssets[assetId] = path;
        return Ok();
    }

    [UnmanagedCallersOnly]
    private static ScriptAbiResult CreateInstance(ulong assetId, ulong owner, ulong* token)
    {
        if (!_initialized)
            return Error(6, "Managed scripting runtime is not initialized.");
        if (token == null)
            return Error(1, "Managed instance token output is null.");
        lock (Gate)
        {
            if (!LoadedAssets.TryGetValue(assetId, out var path))
                return Error(7, "Managed ScriptAsset has not been loaded.");
            if (!ManagedScriptRegistry.TryCreate(
                    path,
                    new NativeObjectHandle(owner),
                    out var behaviour,
                    out var registration)
                || behaviour is null)
            {
                var factoryType = ManagedScriptRegistry.Current.Factory.Method.DeclaringType?.AssemblyQualifiedName ?? "<unknown>";
                return Error(7, $"No generated Behaviour factory entry matches '{path}' (factory={factoryType}).");
            }
            behaviour = NativeObject.RegisterBoundInstance(behaviour);
            var id = _nextToken++;
            Instances[id] = new Instance
            {
                Behaviour = behaviour,
                Owner = owner,
                AssetPath = path,
                Registration = registration
            };
            *token = id;
        }
        return Ok();
    }

    [UnmanagedCallersOnly]
    private static ScriptAbiResult DestroyInstance(ulong token)
    {
        lock (Gate)
            return Instances.Remove(token) ? Ok() : Error(2, "Managed instance token is invalid.");
    }

    [UnmanagedCallersOnly]
    private static ScriptAbiResult Invoke(ulong token, ushort callback, ScriptFrameContext* frame, ulong owner)
    {
        string? assetPath = null;
        try
        {
            lock (Gate)
            {
                if (!Instances.TryGetValue(token, out var instance))
                    return Error(2, "Managed instance token is invalid.");
                assetPath = instance.AssetPath;
                if (frame != null)
                    Time.ApplyFrame(*frame);
                var deltaTime = frame == null ? 0.0f : frame->DeltaTime;
                if (!instance.Registration.Invoke(instance.Behaviour, callback, deltaTime))
                    return Error(9, $"Generated Behaviour does not implement callback {callback}.");
            }
            return Ok();
        }
        catch (Exception exception)
        {
            return Error(16, exception.ToString(), exception, assetPath);
        }
    }

    [UnmanagedCallersOnly]
    private static ScriptAbiResult InvokeBatch(
        ushort callback,
        ulong* tokens,
        ulong* owners,
        uint count,
        ScriptFrameContext* frame)
    {
        try
        {
            if (count != 0 && tokens == null)
                return Error(1, "Managed batch invocation requires an instance token array.");
            _ = owners;
            lock (Gate)
            {
                var deltaTime = frame == null ? 0.0f : frame->DeltaTime;
                if (frame != null)
                    Time.ApplyFrame(*frame);
                var firstError = Ok();
                var failed = false;
                for (uint index = 0; index < count; ++index)
                {
                    if (!Instances.TryGetValue(tokens[index], out var instance))
                    {
                        if (!failed)
                        {
                            firstError = Error(2, "Managed instance token is invalid.");
                            failed = true;
                        }
                        continue;
                    }

                    try
                    {
                        if (!instance.Registration.Invoke(instance.Behaviour, callback, deltaTime) && !failed)
                        {
                            firstError = Error(9, $"Generated Behaviour does not implement callback {callback}.");
                            failed = true;
                        }
                    }
                    catch (Exception exception)
                    {
                        // Keep the first diagnostic buffer intact. The ABI
                        // exposes borrowed pointers, so calling Error again
                        // here would free the first error before Native reads
                        // getLastDiagnostic(). Later instances still run.
                        if (!failed)
                        {
                            firstError = Error(16, exception.ToString(), exception, instance.AssetPath);
                            failed = true;
                        }
                    }
                }
                return failed ? firstError : Ok();
            }
        }
        catch (Exception exception)
        {
            return Error(16, exception.ToString(), exception);
        }
    }

    [UnmanagedCallersOnly]
    private static ScriptAbiResult Reload(ulong assetId, byte* source, byte* schemaHashHex)
        => Error(15, "Managed hot reload requires a freshly compiled GameScripts assembly.");

    [UnmanagedCallersOnly]
    private static ScriptAbiResult ReloadAssembly(ulong assetId, byte* assemblyPath, byte* schemaHashHex)
    {
        try
        {
            if (!_initialized)
                return Error(6, "Managed scripting runtime is not initialized.");
            if (assemblyPath == null || schemaHashHex == null)
                return Error(1, "Managed hot reload requires an assembly path and schema hash.");
            var path = ReadUtf8(assemblyPath);
            var schema = ReadUtf8(schemaHashHex);
            if (!File.Exists(path))
                return Error(7, $"Managed replacement assembly was not found: {path}");
            if (!TryParseHash(ScriptApiManifest.SchemaHash, out var managedHash)
                || !TryParseHash(schema, out var nativeHash)
                || managedHash != nativeHash)
                return Error(10, "Managed and Native Script API schema hashes do not match during hot reload.");

            lock (Gate)
            {
                if (assetId != 0 && !LoadedAssets.ContainsKey(assetId))
                    return Error(7, "Managed hot reload requires the script asset to be loaded first.");

                var previousRegistration = ManagedScriptRegistry.Current;
                var replacementContext = new ScriptAssemblyLoadContext($"Nullus.GameScripts.{++_assemblyVersion}");
                try
                {
                    _ = replacementContext.LoadProject(path);
                    var replacements = new Dictionary<ulong, Instance>();
                    foreach (var (token, previous) in Instances)
                    {
                        if (!ManagedScriptRegistry.TryCreate(
                                previous.AssetPath,
                                new NativeObjectHandle(previous.Owner),
                                out var behaviour,
                                out var replacementRegistration)
                            || behaviour is null)
                            throw new InvalidOperationException($"Replacement assembly has no generated Behaviour factory entry for '{previous.AssetPath}'.");
                        MigrateFields(previous, behaviour, replacementRegistration);
                        replacements[token] = new Instance
                        {
                            Behaviour = behaviour,
                            Owner = previous.Owner,
                            AssetPath = previous.AssetPath,
                            Registration = replacementRegistration
                        };
                    }

                    var previousContext = _projectLoadContext;
                    Instances.Clear();
                    foreach (var replacement in replacements)
                        Instances.Add(replacement.Key, replacement.Value);
                    _projectLoadContext = replacementContext;
                    ProjectLoadContexts.Add(new WeakReference<ScriptAssemblyLoadContext>(replacementContext));
                    previousContext?.Unload();
                    GC.KeepAlive(previousRegistration);
                    return Ok();
                }
                catch (Exception exception)
                {
                    ManagedScriptRegistry.Restore(previousRegistration);
                    replacementContext.Unload();
                    return Error(15, $"Managed hot reload was rolled back: {exception.Message}", exception);
                }
            }
        }
        catch (Exception exception)
        {
            return Error(16, exception.ToString(), exception);
        }
    }

    // Test/diagnostic entry point.  It is intentionally outside the Native ABI
    // table so adding diagnostics cannot change the runtime handshake layout.
    // A successful replacement should leave only the current collectible
    // project context alive after the old instance graph is released.
    [UnmanagedCallersOnly]
    public static int CollectAndGetLiveProjectLoadContextCount()
        => CollectAndGetLiveProjectLoadContextCountManaged();

    public static int CollectAndGetLiveProjectLoadContextCountManaged()
    {
        lock (Gate)
        {
            GC.Collect();
            GC.WaitForPendingFinalizers();
            GC.Collect();
            var live = 0;
            for (var index = ProjectLoadContexts.Count - 1; index >= 0; --index)
            {
                if (ProjectLoadContexts[index].TryGetTarget(out _))
                    ++live;
                else
                    ProjectLoadContexts.RemoveAt(index);
            }
            return live;
        }
    }

    private static void MigrateFields(
        Instance previous,
        Behaviour replacement,
        ManagedScriptRegistry.Registration replacementRegistration)
    {
        var oldTypeName = "global::" + previous.Behaviour.GetType().FullName;
        var newTypeName = "global::" + replacement.GetType().FullName;
        var oldFields = previous.Registration.GetFields(oldTypeName);
        var newFields = replacementRegistration.GetFields(newTypeName);
        foreach (var oldField in oldFields)
        {
            if (!previous.Registration.TryGetField(previous.Behaviour, oldField.Id, out var value))
                continue;
            var target = newFields.FirstOrDefault(field => field.Id == oldField.Id
                || string.Equals(field.Name, oldField.Name, StringComparison.Ordinal)
                || field.Aliases.Contains(oldField.Name, StringComparer.Ordinal)
                || oldField.Aliases.Contains(field.Name, StringComparer.Ordinal));
            if (target is not null && !replacementRegistration.TrySetField(replacement, target.Id, value))
                throw new InvalidOperationException($"Replacement assembly could not restore serialized field '{oldField.Name}'.");
        }
    }

    [UnmanagedCallersOnly]
    private static ScriptAbiResult GetField(ulong token, ulong field, ScriptAbiValue* output)
    {
        try
        {
            if (output == null)
                return Error(1, "Managed field output is null.");
            lock (Gate)
            {
                if (!Instances.TryGetValue(token, out var instance))
                    return Error(2, "Managed instance token is invalid.");
                if (!instance.Registration.TryGetField(instance.Behaviour, field, out var value)
                    || !ScriptAbiCodec.TryEncode(value, output, AbiBuffers))
                    return Error(8, "Generated managed field accessor could not read the requested field.");
            }
            return Ok();
        }
        catch (Exception exception)
        {
            return Error(16, exception.ToString(), exception);
        }
    }

    [UnmanagedCallersOnly]
    private static ScriptAbiResult SetField(ulong token, ulong field, ScriptAbiValue* value)
    {
        try
        {
            if (value == null)
                return Error(1, "Managed field input is null.");
            lock (Gate)
            {
                if (!Instances.TryGetValue(token, out var instance))
                    return Error(2, "Managed instance token is invalid.");
                if (!ScriptAbiCodec.TryDecode(*value, out var managedValue)
                    || !instance.Registration.TrySetField(instance.Behaviour, field, managedValue))
                    return Error(8, "Generated managed field accessor could not write the requested field.");
            }
            return Ok();
        }
        catch (Exception exception)
        {
            return Error(16, exception.ToString(), exception);
        }
    }
}
