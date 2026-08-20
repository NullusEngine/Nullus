using Nullus;

public sealed class ExampleBehaviour : Behaviour
{
    private void Update()
    {
        transform.Rotate(Vector3.up * (30.0f * Time.deltaTime));
    }
}
