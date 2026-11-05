using System;
using Nullus;

// Copy into the project's Assets. Add Player Input with an .inputactions asset,
// then select its Jump action in this component's Inspector field.
public sealed class JumpInput : Behaviour
{
    [SerializeField] private InputActionReference jump = new();
    private IDisposable? subscription;
    private void Start()
    {
        var player = GetComponent<PlayerInput>();
        var action = player is null ? jump.action : jump.Resolve(player.user);
        subscription = action?.BindAction(this, InputActionPhase.Performed,
            _ => Debug.Log("Jump"), mode: InputUpdateMode.Fixed);
    }
    private void OnDestroy() => subscription?.Dispose();
}
