using System;
using Nullus;

namespace Nullus.EngineScripts;

public sealed class InputActionProbe : Behaviour
{
    private readonly InputAction action = new(Guid.Parse("8ec000fd-2903-4d97-9a0a-68c276495e25"));
    public int DynamicCount;
    public int FixedCount;
    public int DeviceCount;
    public int KeyboardConnected;
    private void Awake()
    {
        var devices = InputDevice.All; DeviceCount = devices.Count;
        foreach (var device in devices) if (device.Kind == InputDeviceKind.Keyboard && device.IsConnected && device.IsMapped) KeyboardConnected = 1;
        action.BindAction(this, InputActionPhase.Performed, _ => ++DynamicCount);
        action.BindAction(this, InputActionPhase.Performed, _ => ++FixedCount, mode: InputUpdateMode.Fixed);
    }
}
