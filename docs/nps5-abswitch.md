# In-run A/B switching (AbSwitch)

Compare an optimisation against its baseline inside one run, so machine noise, thermal state and scene differences cancel. Idea from rps5e's `RPS5E_AB_PERIOD`; the code here is our own.

## Running

```
APS5_AB_PERIOD=150 APS5_AB_FLAGS=spin_cheap_clock <your usual title run>
python3 tools/ab_summary.py <stderr or profile log>
```

- `APS5_AB_PERIOD=<frames>`: one `[ab]` line per N flips.
- `APS5_AB_FLAGS=<name>[,<name>...]`: flags toggled in mode B. Mode A is the baseline (every flag as its owner configured it); mode B inverts each listed flag. Flags not listed never change. The names of the older `APS5_AB` switches (`APS5_NO_BARRIER_BATCH`, `APS5_NO_GUARD_SPANS`, `APS5_NO_OWNED_DRAW_INPUTS`, `APS5_PIPELINE_DISPATCH_MISSES`, `APS5_NO_EXTENDED_FAST_PROOFS`) also work here.
- `APS5_AB_MODE_PERIODS=<n>` (default 2): periods per mode visit. The first period after a switch (`since_switch=0`) is warm-up, so keep this at 2 or more.

Line format (printed every period):

```
[ab] mode=B period=5 since_switch=1 frames=150 ft_p50=33.10 ft_p90=39.80 ft_mean=34.20 commit_ms=19.90 qworker_ms=18.50
```

`ft_*` are flip-to-flip frame times (ms). `commit_ms` and `qworker_ms` are CPU ms per frame summed over the draw-commit thread and all `qworker` threads (Mach thread CPU time, user plus system, not wall time).

`tools/ab_summary.py` drops every `since_switch=0` line, weights the rest by frames, and prints A, B and the B-A delta.

## Hooking a flag (for other agents)

```cpp
#include "prx/libSceAgcDriver/Graphics/include/AbSwitch.hpp"

static AgcDriver::Graphics::AbSwitch::Flag fast("my_fast_path", /*baseline=*/std::getenv("APS5_NO_MY_FAST") == nullptr);
...
if (fast.on()) { /* optimised path */ }
```

`Flag::on()` is one relaxed atomic load and a xor. The macro `APS5_AB_ENABLED("name", baseline)` caches a Flag per call site. `AbSwitch.hpp` is header-only for this part, so it works from any header in the lib. The flag must be safe to change between two calls: no state that the other path leaves half-built. Flags are read when a path is taken, so they switch within a frame or two.

Existing hooks: `spin_cheap_clock` (HandoffSpin.hpp, baseline on; mode B polls steady_clock).

## Caveats

- The mode changes at flip packet number N, on the thread that executes the flip; threads see it a moment later.
- Frame time p90 over one period of 100-200 frames is noisy; compare several periods (the summary averages them).
- A flag whose effect is a lasting state change (cache contents, pipeline compilation) is not suited: the second mode inherits the first's state.
