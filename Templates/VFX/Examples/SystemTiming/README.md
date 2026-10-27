# System warmup and fixed ticks

Copy this directory into the project's Assets directory and open
`WarmupFixedTick.vfx` in **Window > Particle Editor**.
The red CPU emitter and green GPU emitter use the same simulation and start
with half a second of prewarmed particles. The visible System age starts at
zero after warmup; particle ages and emitter clocks keep the simulated history.
The runtime consumes pending warmup on the first active Tick, including Tick(0),
after parameter overrides and collision providers have been bound. The candidate
preview performs that zero-time tick when compiling, resetting or starting a seek.

The System section in Details exposes **Warmup Time**, **Warmup Tick Count**,
**Warmup Tick Delta** and **Fixed Tick Delta Time**. Warmup time rounds down to
whole warmup ticks. Its count/delta controls appear when warmup is active.
The fixed delta input is disabled until fixed ticks are enabled.

This example uses a 0.03125 s fixed tick and eight warmup ticks of 0.0625 s.
Each warmup tick passes through the same fixed-step accumulator as a host frame.
Host frames shorter than a tick accumulate without advancing particles; longer
frames may simulate several ticks. The default Niagara catch-up limit of 100
ticks is used per host/warmup tick, and whole excess ticks are dropped while
the fractional tick is retained. Pausing freezes the accumulator. Reset clears
it, repeats warmup with current User parameter overrides, and starts age at zero.

Timeline seeks replay elapsed input time in bounded batches. A target between
fixed ticks displays the preceding completed simulation tick and finishes the
seek without spinning. Source timing edits participate in document undo and
semantic compilation; failed compilation keeps the previous program.

```text
NullusVfxCompile compile Templates/VFX/Examples/SystemTiming/WarmupFixedTick.vfx build/WarmupFixedTick.compiled.json --backend all
```

Timing uses System source/program format 5 and runtime settings version 3.
Older float/typed sources and programs stay readable and are never rewritten
automatically. The shader layout stays 11 and the native VFX importer is 17.
Source-free packages retain the same timing settings.

Candidate limits are fixed delta 0.0001-1 s, warmup delta 0-1 s, at most 1024
warmup ticks and 1024 resolved warmup simulation ticks. Unsupported values
produce diagnostics. This does not establish full Niagara world scheduling,
data-interface tick ordering, installed-editor parity or Linux/Metal acceptance.
