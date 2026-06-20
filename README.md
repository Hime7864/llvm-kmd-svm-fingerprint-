# SVM TSC Spoofing Detection

This project is a research driver designed to detect VMMs that shadow SVME and spoof performance-monitoring timers in order to hide active SVM virtualization.
The detector combines a basic EFER read-overhead check with timing correlation checks that target the subtle missing time introduced by the compensation logic VMMs apply to guest-visible clocks.
It works by correlating two hard-to-spoof, highly consistent time sources:

- `CORE_ENERGY_STAT` — a read-only MSR that updates at a steady ~10-15 ms cadence.
- `I/O APIC timer` — an independent hardware timer driven by its own crystal oscillator.

These two sources serve as a ground truth for elapsed time. Using them, the expected cycle count can be calculated and compared against the P0-derived timings to quantify any desyncs introduced by time negation in the guest.

The detector currently reports five checks:

- `EFER read overhead`: reads EFER repeatedly and measures the average RDTSC delta around each read. It flags when the average EFER read cost is greater than 1000 cycles.
- `Power state elevation`: tracks whether the probe observed power-state behavior that suggests VM-exit-triggered elevation. It flags when any violations are reported.
- `TSC desynchronization`: compares MPERF against MSR::TSC, RDTSC, and RDTSCP. It flags when these P0-derived TSC sources do not advance consistently with the reference performance counter by more than 5%.
- `Interval desynchronization`: compares the measured probe interval against the interval expected from the P0 state after syncing it to the I/O APIC timer. It flags when the interval is more than 5% out of sync.
- `Workload desynchronization`: estimates whether the APERF-reported cycles match the amount of work completed in the MSR-read loop. It flags when more than 20 cycles per measured batch appear to be missing. This metric can also give a rough estimate of how much time the VMM is hiding.

The probe flow is:

1. `RunTest()` starts a TSC sanity check for the selected compute unit.
2. `SanityCheckTsc()` runs the probe three times and keeps the sample with the smallest missing-cycle estimate.
3. `ProtoSanityCheckTsc()` builds the probe configuration from MSR_CPPC_CAPABILITY_1, requests a high-performance CPPC/P-state target, and dispatches the probe across processors with `KeIpiGenericCall()`.
4. `IpiCoreHandler()` filters processors by AMD compute unit ID (CPUID Fn8000_001E_EBX Compute Unit Identifiers) so that both logical processors on the same physical core execute the probe code simultaneously. This prevents the core from being dirtied by unrelated threads. It then raises IRQL, applies the CPPC request, and runs the counter probe.
5. `ProbeCounters()` waits for CORE_ENERGY_STAT to change, snapshots all timing sources, then repeatedly reads EFER until CORE_ENERGY_STAT changes again. This gives the probe a roughly 10-15 ms measurement window and records how many intercepted-looking MSR reads completed inside that window.
6. `ProtoSanityCheckTsc()` normalizes the deltas with the I/O APIC timer, compares them against the expected P0-state interval, and calculates the final missing-time/desynchronization ratios.
7. `GetEferAverage()` raises IRQL, reads EFER 1000 times, and records the average RDTSC delta as a direct EFER read-overhead signal.

The important timing sources captured in TSC_DATA are:

- `APERF`: actual performance clock count.
- `MPERF`: max/reference performance clock count.
- `MSR_TSC`: TSC value read through the TSC MSR.
- `RDTSC`: direct timestamp counter instruction.
- `RDTSCP`: serialized timestamp counter instruction.
- `APIC Timer`: external timer source read through I/O ports.
- `counter`: number of EFER operations completed during the probe interval.

# Example output

```
00000001	0.00000000	 	
00000002	0.00000200	========================================	
00000003	0.00000290	  SVM SVME Spoofing Detectornator	
00000004	0.00000370	========================================	
00000005	0.00000460	  Running probes on core 0...	
00000006	0.00000470	 	
00000007	0.00573280	  EFER read overhead             OK         93 cycles (limit: 1000)	
00000008	0.00573390	  Power state elevation          OK         0 violations (limit: 1)	
00000009	0.00573480	  Interval desynchronization     OK         0% desync (limit: 5%)	
00000010	0.00573560	  TSC desynchronization          OK         0% desync (limit: 5%)	
00000011	0.00573670	  Workload desynchronization     OK         1 cycles unaccounted for (limit: 20 cycles)	
00000012	0.00573720	----------------------------------------	
00000013	0.00573800	  Result: CLEAN  (0/5 checks flagged)	
00000014	0.00573860	========================================	
00000015	0.00573870	 
```
