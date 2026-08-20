using Nullus;

namespace Nullus.EngineScripts;

public sealed class PrivateLifecycle : Behaviour
{
    public int AwakeCount;
    public int StartCount;
    public int EnableCount;
    public int DisableCount;
    public int UpdateCount;
    public int FixedUpdateCount;
    public int LateUpdateCount;
    public int DestroyCount;

    private void Awake() => AwakeCount++;
    private void Start() => StartCount++;
    private void OnEnable() => EnableCount++;
    private void OnDisable() => DisableCount++;
    private void Update() => UpdateCount++;
    private void FixedUpdate() => FixedUpdateCount++;
    private void LateUpdate() => LateUpdateCount++;
    private void OnDestroy() => DestroyCount++;
}
