#include <intrinsics.hpp>

UINT64 NAKED io_apic_rtc()
{
    __asm {
        push rdx
        push r8
        push r9

        mov dx, 0x80C
        mov ax, 0x4
        out dx, ax
        mov dx, 0x80B
        in eax, dx
        mov r8, rax
        and r8, 0xFF
        shl r8, 32

        mov dx, 0x80A
        out dx, ax
        mov dx, 0x809
        in eax, dx
        mov r9, rax
        and r9, 0xFFFF
        shl r9, 16
        or r8, r9

        mov dx, 0x808
        out dx, ax
        mov dx, 0x807
        in eax, dx
        mov r9, rax
        and r9, 0xFF00
        or r8, r9

        mov dx, 0x806
        out dx, ax
        mov dx, 0x805
        in eax, dx
        shr eax, 24
        or r8, rax

        mov rax, r8

        pop r9
        pop r8
        pop rdx
        ret
    }
}

struct TSC_DATA
{
    UINT64 aperf;
    UINT64 mperf;
    UINT64 energy;
    UINT64 msr_tsc;
    UINT64 io_apicTimer;
    UINT64 rdtsc;
    UINT64 rdtscp;
    UINT64 counter;
    UINT64 pstate;
};

void read_tsc_data(TSC_DATA* data)
{
    data->aperf = MSR::APERF();
    data->mperf = MSR::MPERF();
    data->energy = _mm_readmsr(0xC001029A);
    data->msr_tsc = MSR::TSC();
    data->io_apicTimer = io_apic_rtc();
    data->rdtsc = __rdtsc();
    UINT32 aux = 0;
    data->rdtscp = _mm_rdtscp(&aux);
    _mm_lfence();
    _mm_mfence();
}

struct ComputeUnitIdentifiers
{
    union
    {
        UINT32 AsUINT32;
        struct
        {
            UINT32 ComputeUnitId : 8;
            UINT32 CoresPerComputeUnit : 2;
            UINT32 Reserved : 22;
        };
    };
};

void ProbeCounters(TSC_DATA* start, TSC_DATA* end)
{
    UINT64 unit = _mm_readmsr(0xC001029B);
    while (unit == _mm_readmsr(0xC001029B))
        _mm_pause();

    read_tsc_data(start);

    int cnt = 0;
    unit = _mm_readmsr(0xC001029B);
    while (unit == _mm_readmsr(0xC001029B))
    {
        _mm_readmsr(MSR::_MSR_EFER);
        cnt++;
    }
    read_tsc_data(end);
    end->counter = cnt;
    start->counter = cnt;
}

INT64 abs64(INT64 value)
{
    return (value < 0) ? -value : value;
}

struct RTC_CPPC_DATA
{
    int target_core;
    MSR_CPPC_REQUEST cppc;
    TSC_DATA logical_core_start[2];
    TSC_DATA logical_core_end[2];
};

void IpiCoreHandler(RTC_CPPC_DATA* output)
{
    if (!output)
        return;

    ComputeUnitIdentifiers data;
    data.AsUINT32 = CPUID::query(0x8000001E).ebx;

    if (data.ComputeUnitId != output->target_core)
        return;

    auto coreid = KeGetCurrentProcessorNumberEx(nullptr);
    auto irql = _mm_readcr8();
    _mm_writecr8(15);

    MSR_PSTATE_CONTROL cmd{ 0 };
    cmd.PstateCmd = 0;
    MSR::PSTATE_CONTROL(cmd);

    auto old = MSR::CPPC_REQUEST();
    MSR::CPPC_REQUEST(output->cppc);

    int idx = coreid % 2;
    ProbeCounters(&output->logical_core_start[idx], &output->logical_core_end[idx]);

    MSR::CPPC_REQUEST(old);
    _mm_writecr8(irql);
}

void ProbeCore(RTC_CPPC_DATA* output)
{
    KeIpiGenericCall(IpiCoreHandler, output);
}

struct TSC_SANITY_DATA
{
    double tsc_desync_ratio;
    double interval_desync_ratio;
    UINT64 rdtsc_delta_ajusted;
    UINT64 reported_cycles;
    UINT64 missing_cycles;
    UINT64 counter_total;

    bool is_tsc_desynced() const
    {
        return tsc_desync_ratio > 0.05;
    }

    bool is_interval_desynced() const
    {
        return interval_desync_ratio > 0.05;
    }

    bool is_reported_cycles_missing() const
    {
        auto batch_reported_cycles = reported_cycles / counter_total;
        auto batch_expected_cycles = (reported_cycles + missing_cycles) / counter_total;
        return abs64(batch_reported_cycles - batch_expected_cycles) > 20;
    }

    int interval_desync_percent() const
    {
        auto percent = (int)(interval_desync_ratio * 100.0);
        return (int)abs64(percent);
    }

    int tsc_desync_percent() const
    {
        auto percent = (int)(tsc_desync_ratio * 100.0);
        return (int)abs64(percent);
    }

    UINT64 rtc_missing_cycles() const
    {
        if (!reported_cycles)
            return 0;
        return (missing_cycles / reported_cycles) * rdtsc_delta_ajusted;
    }
};

static void log_banner()
{
    printf("\n");
    printf("========================================\n");
    printf("  SVM TSC Fingerprint Detection\n");
    printf("========================================\n");
}

static void log_check(const char* name, const char* threshold, bool flagged, const char* detail)
{
    printf("  %-30s %-9s  %s (limit: %s)\n",
        name,
        flagged ? "FLAGGED" : "OK",
        detail,
        threshold);
}

static void log_summary(bool interval_flagged, bool tsc_flagged, bool workload_flagged)
{
    int flagged_count = (interval_flagged ? 1 : 0) + (tsc_flagged ? 1 : 0) + (workload_flagged ? 1 : 0);

    printf("----------------------------------------\n");
    if (flagged_count == 0)
        printf("  Result: CLEAN  (0/3 checks flagged)\n");
    else
        printf("  Result: FLAGGED (%i/3 checks flagged)\n", flagged_count);
    printf("========================================\n\n");
}

void ProtoSanityCheckTsc(TSC_SANITY_DATA* output, int core_id)
{
    if (!output)
        return;

    RTC_CPPC_DATA data{ 0 };
    auto cppc_capabilities = MSR::CPPC_CAPABILITY_1();
    data.cppc.MinPerf = cppc_capabilities.LowestPerf;
    data.cppc.MaxPerf = cppc_capabilities.HighestPerf;
    data.cppc.DesPerf = cppc_capabilities.NominalPerf;
    data.target_core = core_id;

    ProbeCore(&data);

    auto mperf_delta = data.logical_core_end[0].mperf - data.logical_core_start[0].mperf;
    auto msr_tsc_delta = data.logical_core_end[0].msr_tsc - data.logical_core_start[0].msr_tsc;
    auto rdtsc_delta = data.logical_core_end[0].rdtsc - data.logical_core_start[0].rdtsc;
    auto rdtsp_delta = data.logical_core_end[0].rdtscp - data.logical_core_start[0].rdtscp;

    double sync_ratio = (double)mperf_delta / (double)msr_tsc_delta;
    sync_ratio += (double)mperf_delta / (double)rdtsc_delta;
    sync_ratio += (double)mperf_delta / (double)rdtsp_delta;
    output->tsc_desync_ratio = (sync_ratio / 3.0) - 1.0;

    auto io_apic_ratio = 920000.0 / (double)(data.logical_core_end[0].io_apicTimer - data.logical_core_start[0].io_apicTimer);
    auto reported_aperf_delta = data.logical_core_end[0].aperf - data.logical_core_start[0].aperf;
    auto counter_total = data.logical_core_end[0].counter + data.logical_core_end[1].counter;

    auto counter_total_ajusted = (UINT64)((double)counter_total * io_apic_ratio);
    auto mperf_delta_ajusted = (UINT64)((double)mperf_delta * io_apic_ratio);
    auto msr_tsc_delta_ajusted = (UINT64)((double)msr_tsc_delta * io_apic_ratio);
    auto rdtsc_delta_ajusted = (UINT64)((double)rdtsc_delta * io_apic_ratio);
    auto rdtsp_delta_ajusted = (UINT64)((double)rdtsp_delta * io_apic_ratio);
    auto reported_aperf_delta_ajusted = (UINT64)((double)reported_aperf_delta * io_apic_ratio);

    auto p0_MHz = MSR::PSTATE(0).get_frequency_mhz() * 1000;

    double MHz_ratio = (double)p0_MHz / (double)mperf_delta_ajusted;
    MHz_ratio += (double)p0_MHz / (double)msr_tsc_delta_ajusted;
    MHz_ratio += (double)p0_MHz / (double)rdtsc_delta_ajusted;
    MHz_ratio += (double)p0_MHz / (double)rdtsp_delta_ajusted;

    auto MHz_ratio_total = 1.0 - (MHz_ratio / 4.0);
    output->interval_desync_ratio = MHz_ratio_total;

    auto reported_cycles = reported_aperf_delta_ajusted;
    auto missing_cycles = abs64((UINT64)(reported_aperf_delta_ajusted * MHz_ratio_total));

    output->reported_cycles = reported_cycles;
    output->missing_cycles = missing_cycles;
    output->counter_total = counter_total_ajusted;
    output->rdtsc_delta_ajusted = rdtsc_delta_ajusted;
}

void SanityCheckTsc(TSC_SANITY_DATA* output, int core_id)
{
    TSC_SANITY_DATA sanity_data[3]{ 0 };
    for (int i = 0; i < 3; i++)
        ProtoSanityCheckTsc(&sanity_data[i], core_id);

    for (int i = 0; i < 3; i++)
    {
        if (sanity_data[i].missing_cycles < output->missing_cycles || output->missing_cycles == 0)
            *output = sanity_data[i];
    }
}


void RunTest(int core_id)
{
    log_banner();
    printf("  Running probes on core %i...\n\n", core_id + 1);

    TSC_SANITY_DATA tsc_sanity{ 0 };
    SanityCheckTsc(&tsc_sanity, core_id);

    char detail[128];

    auto interval_flagged = tsc_sanity.is_interval_desynced();
    sprintf(detail, "%i%% desync", tsc_sanity.interval_desync_percent());
    log_check("Interval desynchronization", "5%", interval_flagged, detail);

    auto tsc_flagged = tsc_sanity.is_tsc_desynced();
    sprintf(detail, "%i%% desync", tsc_sanity.tsc_desync_percent());
    log_check("TSC desynchronization", "5%", tsc_flagged, detail);

    auto workload_flagged = tsc_sanity.is_reported_cycles_missing();
    if (workload_flagged)
    {
        sprintf(detail, "%llu missing cycles, ~%llu RTC cycles",
            tsc_sanity.missing_cycles,
            tsc_sanity.rtc_missing_cycles());
    }
    else
    {
        auto batch_reported_cycles = tsc_sanity.reported_cycles / tsc_sanity.counter_total;
        auto batch_expected_cycles = (tsc_sanity.reported_cycles + tsc_sanity.missing_cycles) / tsc_sanity.counter_total;
        sprintf(detail, "%llu cycles unaccounted for", abs64(batch_reported_cycles - batch_expected_cycles));
    }
    log_check("Workload desynchronization", "20 cycles", workload_flagged, detail);

    log_summary(interval_flagged, tsc_flagged, workload_flagged);
    return;
}

NTSTATUS DriverEntry()
{
    RunTest(0);
    return STATUS_SUCCESS;
}
