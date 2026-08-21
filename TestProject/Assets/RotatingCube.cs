using Nullus;

namespace Nullus.GameScripts;

/// <summary>
/// Creates a child Cube when the component wakes and rotates it every frame.
/// Attach this script to any GameObject in a playing scene.
/// </summary>
public sealed class RotatingCube : Behaviour
{
    private GameObject? _cube;
    public float _angle;

    private void Awake()
    {
        _cube = gameObject.CreatePrimitive("Cube");
    }

    private void Update()
    {
        if (_cube is null)
            return;

        _angle = (_angle + Time.deltaTime * 90.0f) % 360.0f;
        _cube.transform.localRotation = Quaternion.AngleAxis(
            _angle,
            new Vector3(0.0f, 1.0f, 0.0f));
    }
}
