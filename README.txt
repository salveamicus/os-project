HYBRID CPU SCHEDULER
====================

CSCI Operating Systems, University of Colorado Denver.
Illiasse Mounjim, James Osborne, Braden Tillema.


WHAT THIS IS
------------

A discrete-event simulator that runs seven classical scheduling policies
(FCFS, SJF, SRTF, LJF, RR, Priority, and a rule-based Hybrid) over
fifteen designed workload scenarios, then aggregates the per-run output
into one results.csv and seven figures. The hybrid scheduler reads the
ready queue at every tick and picks one of FCFS, SJF, or RR depending
on the shape of the workload. The point of the project is to test
whether runtime-observable signals are enough to guide useful policy
switching, without predicting future jobs.


HOW TO RUN
----------

    cmake -B build
    cmake --build build
    python3 analyze.py

analyze.py runs every (policy, scenario, seed), writes
analysis/results.csv, and saves the seven figures into
analysis/figures/. Reruns are bit identical because the per-run RNG
seed is hashed from (scenario, replicate) and the simulator is
deterministic given the seed. If results.csv is already current
relative to the scenarios and the binary, the sweep is skipped and
only figures are redrawn.

Python deps: pandas, matplotlib. Install with:
    pip install pandas matplotlib


THE HYBRID RULE
---------------

  Condition                                   Mode
  ------------------------------------------  ----
  avg(percentIO) >= 15  over arrived jobs     RR
  CoV(remaining_burst) < 0.2                  FCFS
  CoV(remaining_burst) > 0.5                  SJF
  else                                        RR

CoV is the coefficient of variation of remaining burst lengths over
the running job plus the arrived ready queue. Including the running
job is what handles the cold-start case where one long job has the
cpu and shorts are waiting. The rule is re-evaluated every tick, but
a candidate change is only honored if at least hybStabilization ticks
have passed since the last accepted change. This is the proposal's
"preventing rapid switching" countermeasure. In SJF mode the running
job is preempted if a shorter eligible job is sitting in the ready
queue, which is what closes the SRTF gap on scenario13 and scenario15.

Defaults live in src/hybrid.cpp as:
    hybCovLow        = 0.2
    hybCovHigh       = 0.5
    hybIoCutoff      = 15
    hybStabilization = 250

They are not retuned post-hoc on the test set.


REPO LAYOUT
-----------

    CMakeLists.txt           cmake build, c++20, jsoncpp via submodule
    external/jsoncpp/        vendored json parser
    src/
      schedule.{cpp,h}       Job and Schedule classes
      policy.{cpp,h}         base Policy + Trace + the metrics rubric
      fcfs.{cpp,h}           policies, one file per policy
      sjf.{cpp,h}
      srtf.{cpp,h}
      ljf.{cpp,h}
      rr.{cpp,h}
      priority.{cpp,h}
      lottery.{cpp,h}
      hybrid.{cpp,h}         the workload adaptive scheduler
      main.cpp               cli dispatch, flags: -p -j -q -s --csv
    json/
      scenario1.json .. scenario15.json    designed workloads
      scenarios.md                          per-scenario notes
    analyze.py               one shot: build check, run sweep, write csv, draw figures

After running, analysis/ contains:

    analysis/results.csv                    one row per (policy, scenario, seed)
    analysis/figures/fig1_metric_heatmap.png
    analysis/figures/fig2_turnaround.png
    analysis/figures/fig3_response.png
    analysis/figures/fig4_fairness.png
    analysis/figures/fig5_starvation.png
    analysis/figures/fig6_ctx_switches.png
    analysis/figures/fig7_robustness.png    turnaround relative to per-scenario best static


WORKLOAD SCENARIOS
------------------

  ID  Jobs  Burst range   Arrival range  %I/O      Priority
  --  ----  -----------  -------------  --------  --------
   1    5     200, 200       0,  100      0         0
   2    5     100, 500       0,  100      0         0
   3    5     100, 500       0, 1000      0         0
   4    5     100, 500       0, 1000     25         0
   5    5     100, 500       0, 1000      0         0, 1
   6    5     100, 500       0, 1000     25         0, 1
   7   20     200, 200       0,  100      0         0
   8   20     100, 500       0,  100      0         0
   9   20     100, 500       0, 1000      0         0
  10   20     100, 500       0, 1000     25         0
  11   20     100, 500       0, 1000      0         0, 1
  12   20     100, 500       0, 1000     25         0, 1
  13    5     100, 800       0,  200      0         0
  14    8      80, 300       0,  150     30 to 50   0
  15   10      50, 1500      0,  100      0         0

%I/O is the per-tick blocking probability on jobs marked I/O capable.
Five scenarios actually exercise I/O randomness (4, 6, 10, 12, 14).
The other ten are deterministic by construction. The replicate count
in analyze.py reflects this: stochastic scenarios use 100 seeds,
deterministic scenarios use 1.


A FEW HONEST NOTES
------------------

- Lottery is implemented but excluded from the headline figures. It
  runs and produces a trace, but its win counts on the proposal
  metrics are dominated by chance and we did not stabilize the seed
  strategy across enough replicates to report a fair number. The code
  is in src/lottery.cpp for inspection.

- The simulator abstracts hardware. Per-tick context-switch cost is
  a constant. Cache state, TLB pressure, and pipeline depth are not
  modeled. Absolute switch overhead numbers are simulator-specific.
  The relative ordering across policies, which is what the comparison
  hinges on, holds under the constant-cost assumption.

- Ten of fifteen scenarios are deterministic. Their confidence
  intervals are degenerate by construction. Statistical claims rest
  on the five stochastic scenarios with 100 seeds each.

- Scheduler rule defaults were fixed before the sweep. hybCovLow=0.2,
  hybCovHigh=0.5, hybIoCutoff=15, hybStabilization=250. Not retuned
  post-hoc.

- HYBRID trails pure RR on s4 and s6. Both scenarios fall in the
  medium-CoV "default to RR" branch. Hybrid runs in RR mode there but
  the per-tick mode infrastructure adds preemption events pure RR
  does not trigger, so HYBRID lands at roughly 2x the turnaround of
  RR on these two scenarios. The stabilization window narrows the
  gap but does not close it. Across the suite HYBRID still wins 11 of
  15 turnaround scenarios and 12 of 15 waiting time scenarios.

- HYBRID trails non-preemptive policies on s14. s14 is 100% I/O
  capable. Hybrid correctly routes to RR mode for response time and
  pays the turnaround cost, the same cost RR alone pays. This is the
  design tradeoff the proposal anticipated.


REPRODUCIBILITY
---------------

    cmake -B build && cmake --build build && python3 analyze.py

That is the entire pipeline. Same machine, same git commit, same
results.csv and same figures, byte for byte. The randomness is
hashed from (scenario, replicate) inside analyze.py so the seeds are
stable across machines too.
