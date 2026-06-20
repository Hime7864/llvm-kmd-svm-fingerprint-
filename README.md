# SVM TSC Spoofing Detection

This project is an research driver used for detecting VMMs that shadow Core::X86::Msr::EFER.SVME and spoof performance timers to hide that SVM is active.I've seen many people just read an MSR 10k times and look at the average — this is the next level past that. It specifically targets the "missing time" caused by compensations made on the guest-visible clocks.
It works by correlating two hard-to-spoof, consistent time sources:

- `Core::X86::Msr::CORE_ENERGY_STAT` — a read-only MSR that updates at a steady ~10-15 ms cadence.
- The I/O APIC timer — an independent hardware timer driven by its own crystal oscillator.

These two sources serve as a ground truth for elapsed time. Using them, the expected cycle count can be calculated and compared against the P0-derived timings to quantify any desyncs introduced by time negation in the guest.

The detector currently reports three checks:

- `TSC desynchronization`: compares `MPERF` against `TSC`, `RDTSC`, and `RDTSCP`. It flags when these P0-derived TSC time does not advance consistently with the reference performance counter by more than 5%.
- `Interval desynchronization`: compares the measured probe interval against the interval expected from the P0 state after syncing it to the I/O APIC timer. It flags when the interval is more than `5%` out of sync.
- `Workload desynchronization`: estimates whether the APERF-reported cycles match the amount of work completed in the MSR-read loop. It flags when more than `20` cycles per measured batch appear to be missing. This metric can also give a rough estimate of how much time the VMM is hiding.

The probe flow is:

1. `RunTest()` starts a TSC sanity check for the selected compute unit.
2. `SanityCheckTsc()` runs the lower-level probe three times and keeps the sample with the smallest missing-cycle estimate.
3. `ProtoSanityCheckTsc()` builds the probe configuration from `MSR_CPPC_CAPABILITY_1`, requests a high-performance CPPC/P-state target, and dispatches the probe across processors with `KeIpiGenericCall()`.
4. `IpiCoreHandler()` filters processors by AMD compute unit ID (from `CPUID Fn8000_001E_EBX Compute Unit Identifiers`) so that both logical processors on the same physical core execute the probe code simultaneously. This prevents the core from being dirtied by unrelated threads. It then raises IRQL, forces P-state command 0, applies the CPPC request, and runs the counter probe.
5. `ProbeCounters()` waits for `Core::X86::Msr::CORE_ENERGY_STAT` to change, snapshots all timing sources, then repeatedly reads `Core::X86::Msr::EFER` until `Core::X86::Msr::CORE_ENERGY_STAT` changes again. This gives the probe a roughly 10-15 ms measurement window and records how many intercepted-looking MSR reads completed inside that window.
6. `ProtoSanityCheckTsc()` normalizes the deltas with the I/O APIC timer, compares them against the expected P0-state interval, and calculates the final missing-time/desynchronization ratios.

The important timing sources captured in `TSC_DATA` are:

- `APERF`: actual performance clock count.
- `MPERF`: max/reference performance clock count.
- `MSR_TSC`: TSC value read through the TSC MSR.
- `RDTSC`: direct timestamp counter instruction.
- `RDTSCP`: serialized timestamp counter instruction.
- `APIC Timer`: external timer source read through I/O ports.
- `counter`: number of `RDMSR(EFER)` operations completed during the probe interval.
