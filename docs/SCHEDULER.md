# Scheduler

## Model
- Up to 8 application tasks plus the kernel idle task; unique fixed priorities 1..7 (idle = 0).
- Periodic tasks: period T, constrained relative deadline D <= T, synchronous first release at `t0`
  (the critical instant, the worst case for fixed-priority analysis). Aperiodic tasks run when nothing
  else is ready (the benchmark's logger).
- A job ends with `kestrel_wait_period[_ts]()`. States: READY (includes running), BLOCKED (period, sleep,
  flags, mutex), FAULTED.

## Ready set and levels
`ready_levels` has one bit per level, `level = 2 * priority`; the next task is `level_owner[31 - CLZ(ready)]`,
O(1). Odd levels are used only by the priority-ceiling mutex (holder runs at `2 * ceiling + 1`), so a holder
outranks every user of the mutex without sharing a level with any task.

## Tickless release timing
One hardware compare (GPT320 compare A, 48 MHz, 32 bit, extended to 64 bit by the overflow interrupt) is
programmed for the earliest pending event: a release, a deadline, a timeout, or the supervisor tick. Each TCB
caches `next_event` (never later than its true next event); the timer ISR processes only due tasks and
computes the next compare in the same pass. If a compare would be programmed less than 2 us ahead, or the
counter passed it while writing, the interrupt is pended in software instead (no missed-compare window).

## Deadline and overrun policy
- The miss is detected at the deadline instant by the timer (not at job end), counted once per job, traced.
- The late job continues to completion (no abort).
- A release that finds the previous job still active is skipped and counted as an overrun; the release grid
  is kept (no backlog). This is a deliberate difference from FreeRTOS `vTaskDelayUntil`, which releases the
  late jobs back to back to catch up (docs/BENCHMARKS.md, overload).

## Measured quantities
Per job (12-byte job record, streamed): release latency (ideal release to timer processing), start latency
(ideal release to the task's own first timestamp, or to dispatch), execution time (CPU time while running,
including interrupts that hit it), response time (completion minus ideal release), miss/overrun flags.
Exact on-target maxima and counters are in every status frame.

## Schedulability analysis
`host/sched_analysis.py`: Liu & Layland and hyperbolic bounds (sufficient only; failing them proves nothing)
and exact response-time analysis with per-job kernel overhead, ISR interference and blocking. RTA is exact
for the model, so a disagreement with hardware means the model misses a term. Comparison with measured
behaviour around the failure boundary: docs/BENCHMARKS.md.

## Mutex protocol choice
`KMUTEX_PLAIN` exists to demonstrate unbounded inversion. The production protocol is the immediate priority
ceiling (IPCP) rather than priority inheritance: with a static task set the ceiling is known at build time;
IPCP needs no wait queue, bounds blocking to one critical section, prevents chained blocking and deadlock
among ceiling mutexes on a single core, and is simpler to analyse (one blocking term in RTA). The cost is
that the holder delays intermediate-priority tasks even when there is no contention.
