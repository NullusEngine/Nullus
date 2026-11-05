# Windows Debug physics baseline

Measured on 2026-10-29, AMD Ryzen 9 8945HX, 32 logical processors, Windows
10.0.26200, two TaskGraph workers. PhysX 5.6.1 and Box2D 3.2.0 run on CPU.
The checkout and host were not isolated. These are Debug diagnostic results;
Release performance acceptance, long-duration stress and editor performance
have not been measured.

The executable creates a static floor and 1k/5k/10k dynamic boxes in a grid,
warms up for 30 fixed steps and samples 120 steps at 1/60s. Box2D uses four
solver subdivisions. The metric measures the complete native Step call,
including 3D contact aggregation, excluding game-thread dispatch and rendering.
Each case also issues 1,000 actual closest-hit rays. All 1,000 rays hit geometry.

| Dimension | Dynamic boxes | Step p50 ms | p95 ms | p99 ms | 1,000 rays ms | Final active / asleep |
| --- | --- | --- | --- | --- | --- | --- |
| 3D | 1,000 | 8.099 | 8.519 | 8.671 | 8.160 | 0 / 1,000 |
| 2D | 1,000 | 0.0007 | 1.291 | 1.328 | 2.580 | 0 / 1,000 |
| 3D | 5,000 | 52.190 | 143.346 | 151.237 | 9.855 | 0 / 5,000 |
| 2D | 5,000 | 7.925 | 9.519 | 9.972 | 3.638 | 5,000 / 0 |
| 3D | 10,000 | 213.663 | 225.569 | 247.696 | 10.374 | 10,000 / 0 |
| 2D | 10,000 | 13.255 | 17.941 | 19.396 | 3.695 | 10,000 / 0 |

The sleep state and contact layouts differ across these cases; these figures
do not rank backend performance. The 3D Debug 10k case exceeds a 60 Hz budget.
Profiling, optimized-build measurements and performance work remain open.

[Raw results](Benchmarks/WindowsDebug-20261029.json) include exact counts,
configuration, host details and process working-set samples. Working set is
the entire benchmark process at sample completion, not exclusive physics
allocation or peak memory. Backend-owned memory is unavailable and recorded
as null. Object counts include the static floor; requested counts do not.

Build NullusPhysicsBenchmark and pass one output JSON filename. The target is
not registered in normal CTest because benchmark execution is opt-in. Joints,
triggers, tilemap churn, multiple worlds and sustained Play/Stop memory stress
remain unmeasured. Full examples A-E and packaged Player acceptance remain open.
