using System.Reflection;
using System.Runtime.InteropServices;
using Nullus;

internal static unsafe class Program
{
    private static int _mode = 1;
    private static int _calls;
    private static int _legacyCalls;
    private static string _message = string.Empty;
    private static string _stack = string.Empty;
    private static string _source = string.Empty;
    private static int _line;

    [UnmanagedCallersOnly]
    private static byte GetMode(byte severity) => (byte)_mode;

    [UnmanagedCallersOnly]
    private static void LogDetailed(byte severity, byte* message, byte* stack, byte* source, int line)
    {
        ++_calls;
        _message = Marshal.PtrToStringUTF8((IntPtr)message) ?? string.Empty;
        _stack = Marshal.PtrToStringUTF8((IntPtr)stack) ?? string.Empty;
        _source = Marshal.PtrToStringUTF8((IntPtr)source) ?? string.Empty;
        _line = line;
    }

    [UnmanagedCallersOnly]
    private static void LogLegacy(byte severity, byte* message) => ++_legacyCalls;

    private static void Configure(NativeApiTable table)
    {
        var configure = typeof(NativeBinding).GetMethod("Configure", BindingFlags.NonPublic | BindingFlags.Static)
            ?? throw new InvalidOperationException("NativeBinding.Configure is missing.");
        configure.Invoke(null, new object[] { table });
    }

    [System.Runtime.CompilerServices.MethodImpl(System.Runtime.CompilerServices.MethodImplOptions.NoInlining)]
    private static void EmitFromUserCode() => Nullus.Debug.Log("managed console smoke");

    private static void Require(bool condition, string message)
    {
        if (!condition) throw new InvalidOperationException(message);
    }

    private static int Main()
    {
        Require(sizeof(NativeApiTable) == 352, "Unexpected native ABI table size.");
        Require(sizeof(ScriptInputValue) == 56, "Unexpected input ABI value size.");
        Require(Marshal.OffsetOf<NativeApiTable>(nameof(NativeApiTable.LogDetailed)).ToInt32() == 312,
            "The appended logging fields changed the existing ABI layout.");
        var table = new NativeApiTable
        {
            Header = new ScriptAbiHeader { Size = (uint)sizeof(NativeApiTable), AbiVersion = 2 },
            Log = &LogLegacy,
            LogDetailed = &LogDetailed,
            GetLogStackMode = &GetMode
        };
        Configure(table);
        EmitFromUserCode();
        Require(_calls == 1 && _legacyCalls == 0 && _message == "managed console smoke", "Detailed log was not delivered once.");
        Require(_stack.Contains(nameof(EmitFromUserCode)) && _source.EndsWith("Program.cs") && _line > 0,
            "Managed stack or source location is missing.");

        _mode = 0;
        EmitFromUserCode();
        Require(_calls == 2 && _stack.Length == 0 && _source.Length == 0, "None must not capture a managed stack.");

        Configure(new NativeApiTable
        {
            Header = new ScriptAbiHeader { Size = 312, AbiVersion = 2 },
            Log = &LogLegacy
        });
        EmitFromUserCode();
        Require(_legacyCalls == 1, "Legacy logging callback did not remain available.");
        Console.WriteLine("Console managed logging smoke passed.");
        return 0;
    }
}
