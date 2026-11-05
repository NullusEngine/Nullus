using System;
using Nullus;

namespace Nullus.EngineScripts;

// Host integration fixture: static state and subscriptions intentionally survive object destruction.
public sealed class PlaySessionProbe : Behaviour
{
    private static int _serial;
    private static event Action? Signal;
    public int AwakeSerial;
    public int SignalsReceived;
    public int CollisionEnterCount;
    public int CollisionStayCount;
    public int TriggerEnterCount;
    public int TriggerExitCount;
    public int PauseCount;
    public int WorkerApiRejected;
    private void Awake()
    {
        AwakeSerial=++_serial;
        Signal+=Receive;
        var body=GetComponent<Rigidbody>();
        if(body!=null)body.mass=2;
        WorkerApiRejected=System.Threading.Tasks.Task.Run(()=>
        {
            try { _=new GameObject("Stale worker must not create this");return 0; }
            catch(InvalidOperationException) {return 1;}
        }).GetAwaiter().GetResult();
    }
    private void Receive()=>++SignalsReceived;
    private void Update()=>Signal?.Invoke();
    private void OnCollisionEnter(Collision collision) { if(collision.collider!=null&&collision.gameObject!=null)++CollisionEnterCount; }
    private void OnCollisionStay(Collision collision) { if(collision.collider!=null)++CollisionStayCount; }
    private void OnTriggerEnter(Collider other) { if(other!=null)++TriggerEnterCount; }
    private void OnTriggerExit(Collider other) { if(other!=null)++TriggerExitCount; }
    private void OnApplicationPause(bool paused) { if(paused)++PauseCount; }
}
