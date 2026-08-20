using Nullus;

namespace Nullus.EngineScripts;

public sealed class TransformMover : Behaviour
{
    [SerializeField]
    public float Speed = 1.0f;

    [SerializeField]
    public int AwakeCount;

    [SerializeField]
    public int StartCount;

    [SerializeField]
    public int UpdateCount;

    [SerializeField]
    public bool ThrowOnUpdate;

    public int AutoSerializedCount;

    [System.NonSerialized]
    public int RuntimeOnlyCount;

    public void Awake() => AwakeCount++;

    public void Start() => StartCount++;

    public void Update()
    {
        UpdateCount++;
        if (ThrowOnUpdate)
            throw new InvalidOperationException("TransformMover debug exception");
        var position = transform.LocalPosition;
        position.X += Speed * Time.deltaTime;
        transform.LocalPosition = position;
    }
}
