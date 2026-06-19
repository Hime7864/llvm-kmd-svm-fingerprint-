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

BOOLEAN NmiCallback(
    _In_ PVOID CallbackContext,
    _In_ BOOLEAN IsHandled)
{
    auto coreid = KeGetCurrentProcessorNumberEx(nullptr);
    printf("RTC APIC ID: %p\n", io_apic_rtc());
    printf("tsc        : %p\n", __rdtsc());
    for (UINT64 i = 0; i < 0x5000000; i++)
        _mm_pause();
    printf("RTC APIC ID: %p\n", io_apic_rtc());
    printf("tsc        : %p\n", __rdtsc());
    return TRUE;
}

void IpiCallback(_In_ PVOID CallbackContext)
{
    auto coreid = KeGetCurrentProcessorNumberEx(nullptr);
    DbgPrintEx(0, 0, "%p\n", MSR::APERF());
    return;
}

NTSTATUS BroadcastNmi(PVOID ctx)
{
    auto affinity = (_KAFFINITY_EX*)ExAllocatePool(NonPagedPool, sizeof(_KAFFINITY_EX));
    if (!affinity)
        return STATUS_INSUFFICIENT_RESOURCES;
    auto handle = KeRegisterNmiCallback(NmiCallback, ctx);
    if (!handle)
    {
        ExFreePoolWithTag(affinity, NMI_CB_POOL_TAG);
        return STATUS_UNSUCCESSFUL;
    }

    memset(affinity, 0, sizeof(_KAFFINITY_EX));

    auto numCores = KeQueryActiveProcessorCount(0);
    KeInitializeAffinityEx(affinity);
    //for (int i = 0; i < numCores; i++)
        //KeAddProcessorAffinityEx(affinity, i);
    KeAddProcessorAffinityEx(affinity, 1);
    HalSendNMI(affinity);
    Sleep(1);

    KeDeregisterNmiCallback(handle);

    ExFreePoolWithTag(affinity, NMI_CB_POOL_TAG);

    return STATUS_SUCCESS;
}

void BroadcastIntr(PVOID ctx)
{
    KeIpiGenericCall(IpiCallback, ctx);
}

UINT32 NAKED tsc_io_raw()
{
    __asm {
        push rdx

        mov dx, 0xCD6
        mov ax, 0x500
        out dx, ax

        mov dx, 0xCD7
        in eax, dx

        pop rdx
        ret
    }
}

bool read_bit(UINT64 value, int bit)
{
    return (value >> bit) & 1;
}

struct L3RAPLPowerUnit0
{
    union
    {
        UINT64 AsUINT64;
        struct
        {
            UINT64 PowerUnits : 4;
            UINT64 Reserved0 : 4;
            UINT64 EnergyStatusUnits : 5;
            UINT64 Reserved1 : 3;
            UINT64 TimeUnits : 4;
            UINT64 Reserved2 : 44;
        };
    };
};

struct RAPLPowerUnit
{
    union
    {
        UINT64 AsUINT64;
        struct
        {
            UINT64 PowerUnits : 4;
            UINT64 Reserved0 : 4;
            UINT64 EnergyStatusUnits : 5;
            UINT64 Reserved1 : 3;
            UINT64 TimeUnits : 4;
            UINT64 Reserved2 : 44;
        };
    };
};


struct CppcCapability1
{
    union
    {
        UINT64 AsUINT64;
        struct
        {
            UINT64 LowestPerf : 8;
            UINT64 LowNonLinPerf : 8;
            UINT64 NominalPerf : 8;
            UINT64 HighestPerf : 8;
        };
    };
};

struct CppcCapability2
{
    union
    {
        UINT64 AsUINT64;
        struct
        {
            UINT64 MaxPerf : 8;
        };
    };
};

struct ExtPerfMonAndDbgEbx
{
    union
    {
        UINT32 AsUINT32;
        struct
        {
            UINT32 NumPerfCtrCore : 4;
            UINT32 LbrStackSz : 6;
            UINT32 NumPerfCtrNB : 6;
            UINT32 NumPerfCtrUmc : 8;
        };
    };
};

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
    return;
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

inline void __hlt()
{
    __asm {
        MWAIT
    }
}   

namespace SingleRTC
{
    void rdtsc()
    {
        ComputeUnitIdentifiers data;
        data.AsUINT32 = CPUID::query(0x8000001E).ebx;

        auto logic_core_number = CPUID::current_core_number();
        auto core_number = data.ComputeUnitId;

        //printf("Compute Unit ID: %i Cores per Compute Unit: %i\n", data.ComputeUnitId, data.CoresPerComputeUnit);

        auto P0 = MSR::PSTATE(0).get_frequency_mhz() * 1000;

        int cnt = 0;
        UINT64 watt = _mm_readmsr(0xC001029B);
        while (watt == _mm_readmsr(0xC001029B))
            _mm_pause();

        TSC_DATA start, end;
        read_tsc_data(&start);

        watt = _mm_readmsr(0xC001029B);
        while (watt == _mm_readmsr(0xC001029B))
        {
            MSR::EFER().svme;
            cnt++;
        }
        
        read_tsc_data(&end);

        TSC_DATA delta;
        delta.aperf = end.aperf - start.aperf;
        delta.mperf = end.mperf - start.mperf;
        delta.energy = end.energy - start.energy;
        delta.msr_tsc = end.msr_tsc - start.msr_tsc;
        delta.io_apicTimer = end.io_apicTimer - start.io_apicTimer;
        delta.rdtsc = end.rdtsc - start.rdtsc;

        UINT64 avg_delta = (delta.mperf + delta.msr_tsc + delta.rdtsc) / 3;
		//printf("P0: %i MHz\n", avg_delta - P0);
		double offset_ratio =  (double)P0 / (double)avg_delta;
        //delta.aperf = (UINT64)(delta.aperf * offset_ratio);
		//delta.mperf = (UINT64)(delta.mperf * offset_ratio);
		//delta.energy = (UINT64)(delta.energy * offset_ratio);
		//delta.msr_tsc = (UINT64)(delta.msr_tsc * offset_ratio);
		//delta.io_apicTimer = (UINT64)(delta.io_apicTimer * offset_ratio);
		//delta.rdtsc = (UINT64)(delta.rdtsc * offset_ratio);


        printf("%02i %02i )a %i m %i e %i mt %i io %i tsc %i cnt %i - %i\n",
            logic_core_number % 2, core_number, delta.aperf, delta.mperf, delta.energy, delta.msr_tsc, delta.io_apicTimer, delta.rdtsc, cnt, (UINT64)((((double)delta.mperf / (double)delta.aperf) * (double)cnt) / 1));

        return;
    }
};

bool svme_enabled()
{
    return MSR::EFER().svme;
}

bool dtc_pstate_shadowing()
{
    auto old = MSR::PSTATE_STATUS().CurPstate;

    int score = 0;
    for (int i = 0; i < 10; i++)
    {
        MSR_PSTATE_CONTROL cmd;
        cmd.PstateCmd = i % 2;
        MSR::PSTATE_CONTROL(cmd);
        if (MSR::PSTATE_STATUS().CurPstate == i % 2)
            score++;
    }

    auto cmd = MSR::PSTATE_CONTROL();
    cmd.PstateCmd = old;
    MSR::PSTATE_CONTROL(cmd);

    return score > 2;
}

struct DTC
{
    static bool PowerStateElevation()
    {
		auto new_state = !MSR::PSTATE_STATUS().CurPstate;
        MSR_PSTATE_CONTROL cmd{ 0 };
        cmd.PstateCmd = new_state;
        MSR::PSTATE_CONTROL(cmd);
        MSR::EFER().svme;
        return MSR::PSTATE_STATUS().CurPstate == new_state;
    }

    static bool PowerStateShadowing()
    {
        int counter = 0;
        for (int i = 0; i < 10; i++)
        {
            auto data = MSR::PSTATE_CONTROL();
            data.PstateCmd = !MSR::PSTATE_STATUS().CurPstate;
            MSR::PSTATE_CONTROL(data);
            if (MSR::PSTATE_STATUS().CurPstate == data.PstateCmd)
                counter++;
        }
        return counter >= 5;
	}

    static void ProbeCounters(TSC_DATA* out_delta, MSR_CPPC_REQUEST cppc)
    {
        MSR::CPPC_REQUEST(cppc);

        UINT64 unit = _mm_readmsr(0xC001029B);
        while (unit == _mm_readmsr(0xC001029B))
            _mm_pause();

        TSC_DATA start, end;
        read_tsc_data(&start);

        int cnt = 0;
        unit = _mm_readmsr(0xC001029B);
        while (unit == _mm_readmsr(0xC001029B))
        {
            _mm_readmsr(MSR::_MSR_EFER);
            cnt++;
        }

        read_tsc_data(&end);

        if (out_delta)
        {
            out_delta->aperf = end.aperf - start.aperf;
            out_delta->mperf = end.mperf - start.mperf;
            out_delta->energy = end.energy - start.energy;
            out_delta->msr_tsc = end.msr_tsc - start.msr_tsc;
            out_delta->io_apicTimer = end.io_apicTimer - start.io_apicTimer;
            out_delta->rdtsc = end.rdtsc - start.rdtsc;
            out_delta->rdtscp = end.rdtscp - start.rdtscp;
            out_delta->counter = cnt;
            out_delta->pstate = MSR::PSTATE_STATUS().CurPstate;
        }

        return;
    }

    static void ProbeCounters(TSC_DATA* out_delta)
    {
        UINT64 unit = _mm_readmsr(0xC001029B);
        while (unit == _mm_readmsr(0xC001029B))
            _mm_pause();

        TSC_DATA start, end;
        read_tsc_data(&start);

        int cnt = 0;
        unit = _mm_readmsr(0xC001029B);
        while (unit == _mm_readmsr(0xC001029B))
        {
            _mm_readmsr(MSR::_MSR_EFER);
            cnt++;
        }

        read_tsc_data(&end);

        if (out_delta)
        {
            out_delta->aperf = end.aperf - start.aperf;
            out_delta->mperf = end.mperf - start.mperf;
            out_delta->energy = end.energy - start.energy;
            out_delta->msr_tsc = end.msr_tsc - start.msr_tsc;
            out_delta->io_apicTimer = end.io_apicTimer - start.io_apicTimer;
            out_delta->rdtsc = end.rdtsc - start.rdtsc;
            out_delta->rdtscp = end.rdtscp - start.rdtscp;
            out_delta->counter = cnt;
            out_delta->pstate = MSR::PSTATE_STATUS().CurPstate;
        }

        return;
    }

    static void SanityCheckCounters()
    {


        // first get Min and Max performance values
        auto cppc_backup = MSR::CPPC_REQUEST();
        auto cppc_capability = MSR::CPPC_CAPABILITY_1();
        auto cppc_request = cppc_backup;
        cppc_request.MinPerf = cppc_capability.LowestPerf;
        cppc_request.MaxPerf = cppc_capability.HighestPerf;
        //power state 0 check
        cppc_request.DesPerf = cppc_request.MaxPerf;
        DTC::ProbeCounters(nullptr, cppc_request);
        TSC_DATA tsc_data[10];

        
        for (int i = 0; i < 10; i++)
        {
            if (i % 2)
                cppc_request.DesPerf = cppc_request.MinPerf;
            else
                cppc_request.DesPerf = cppc_request.MaxPerf;
            DTC::ProbeCounters(&tsc_data[i], cppc_request);
        }

        //power state 1 check
		//TSC_DATA p1_delta_min, p1_delta_max;
        //cppc_request.DesPerf = cppc_request.MinPerf;
        //DTC::ProbeCounters(&p1_delta_min, cppc_request, 1, true);
        //cppc_request.DesPerf = cppc_request.MaxPerf;
        //DTC::ProbeCounters(&p1_delta_max, cppc_request, 1, true);
        MSR::CPPC_REQUEST(cppc_backup);

        for (int i = 0; i < 10; i++)
        {
            printf("%i) -> a %i m %i e %i mt %i io %i tsc %i tscp %i cnt %i\n",
                i,
                tsc_data[i].aperf, tsc_data[i].mperf, tsc_data[i].energy, tsc_data[i].msr_tsc, tsc_data[i].io_apicTimer, tsc_data[i].rdtsc, tsc_data[i].rdtscp, tsc_data[i].counter);
        }


        return;

    }
};

void run_detection()
{
    auto irql = _mm_readcr8();
    _mm_writecr8(15);
    
    //if (DTC::PowerStateElevation())
    //    printf("[Flagged] - Power State Elevation\n");
    //if (DTC::PowerStateShadowing())
    //    printf("[Flagged] - Power State Shadowing\n");

    DTC::SanityCheckCounters();
    _mm_writecr8(irql);
    return;
}

void ipi_probe(TSC_DATA* output)
{
    ComputeUnitIdentifiers data;
    data.AsUINT32 = CPUID::query(0x8000001E).ebx;

    auto logic_core_number = CPUID::current_core_number();
    auto core_number = data.ComputeUnitId;

	auto coreid = KeGetCurrentProcessorNumberEx(nullptr);

    if (data.ComputeUnitId == 0)
    {
        int idx = coreid % 2;
        TSC_DATA tsc_data;
        DTC::ProbeCounters(&tsc_data);
		output[idx].aperf = tsc_data.aperf;
		output[idx].mperf = tsc_data.mperf;
		output[idx].energy = tsc_data.energy;
		output[idx].msr_tsc = tsc_data.msr_tsc;
		output[idx].io_apicTimer = tsc_data.io_apicTimer;
		output[idx].rdtsc = tsc_data.rdtsc;
		output[idx].rdtscp = tsc_data.rdtscp;
		output[idx].counter = tsc_data.counter;
		output[idx].pstate = tsc_data.pstate;
    }

    
    return;
}




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
    return;
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

    UINT64 CalculateGrossJitter()
    {
        auto result = abs64(abs64(logical_core_start[0].rdtsc - logical_core_start[1].rdtsc) - abs64(logical_core_end[0].rdtsc - logical_core_end[1].rdtsc));
		result += abs64(abs64(logical_core_start[0].rdtscp - logical_core_start[1].rdtscp) - abs64(logical_core_end[0].rdtscp - logical_core_end[1].rdtscp));
        result += abs64(abs64(logical_core_start[0].msr_tsc - logical_core_start[1].msr_tsc) - abs64(logical_core_end[0].msr_tsc - logical_core_end[1].msr_tsc));
		result += abs64(abs64(logical_core_start[0].aperf - logical_core_start[1].aperf) - abs64(logical_core_end[0].aperf - logical_core_end[1].aperf));
		result += abs64(abs64(logical_core_start[0].mperf - logical_core_start[1].mperf) - abs64(logical_core_end[0].mperf - logical_core_end[1].mperf));
		result += abs64(abs64(logical_core_start[0].energy - logical_core_start[1].energy) - abs64(logical_core_end[0].energy - logical_core_end[1].energy));
		result += abs64(abs64(logical_core_start[0].io_apicTimer - logical_core_start[1].io_apicTimer) - abs64(logical_core_end[0].io_apicTimer - logical_core_end[1].io_apicTimer));
		result += abs64(abs64(logical_core_start[0].counter - logical_core_start[1].counter) - abs64(logical_core_end[0].counter - logical_core_end[1].counter));
	    return result;
    }

    UINT64 CalculateGrossTscDelta()
    {
        auto result = abs64(abs64(logical_core_end[0].rdtsc - logical_core_start[0].rdtsc) - abs64(logical_core_end[1].rdtsc - logical_core_start[1].rdtsc));
		result += abs64(abs64(logical_core_end[0].rdtscp - logical_core_start[0].rdtscp) - abs64(logical_core_end[1].rdtscp - logical_core_start[1].rdtscp));
		result += abs64(abs64(logical_core_end[0].msr_tsc - logical_core_start[0].msr_tsc) - abs64(logical_core_end[1].msr_tsc - logical_core_start[1].msr_tsc));
		result += abs64(abs64(logical_core_end[0].aperf - logical_core_start[0].aperf) - abs64(logical_core_end[1].aperf - logical_core_start[1].aperf));
        return result;
	}

    double CalculateExecutionRatio()
    {
		auto aperf_delta_0 = logical_core_end[0].aperf - logical_core_start[0].aperf;
		auto aperf_delta_1 = logical_core_end[1].aperf - logical_core_start[1].aperf;
		auto aperf = (aperf_delta_0 > aperf_delta_1) ? aperf_delta_0 : aperf_delta_1;

		auto mperf_delta_0 = logical_core_end[0].mperf - logical_core_start[0].mperf;   
		auto mperf_delta_1 = logical_core_end[1].mperf - logical_core_start[1].mperf;
		auto mperf = (mperf_delta_0 > mperf_delta_1) ? mperf_delta_0 : mperf_delta_1;

        return ((double)aperf / (double)mperf);
    }

    UINT64 CalculateWork()
    {
		return logical_core_end[0].counter + logical_core_end[1].counter;
    }
};

void IpiCoreHandler(RTC_CPPC_DATA* output)
{
    if (!output)
        return;

	MSR_PSTATE_CONTROL cmd;
    cmd.PstateCmd = 0;
    MSR::PSTATE_CONTROL(cmd);
    ComputeUnitIdentifiers data;
    data.AsUINT32 = CPUID::query(0x8000001E).ebx;

    auto logic_core_number = CPUID::current_core_number();
    auto core_number = data.ComputeUnitId;

    auto coreid = KeGetCurrentProcessorNumberEx(nullptr);

    if (data.ComputeUnitId == output->target_core)
    {
		auto irql = _mm_readcr8();
		_mm_writecr8(15);
        auto old = MSR::CPPC_REQUEST();
        MSR::CPPC_REQUEST(output->cppc);
        int idx = coreid % 2;

        ProbeCounters(&output->logical_core_start[idx], &output->logical_core_end[idx]);

        MSR::CPPC_REQUEST(old);
		_mm_writecr8(irql);
    }
    return;
}

void ProbeCore(RTC_CPPC_DATA* output)
{
	KeIpiGenericCall(IpiCoreHandler, output);
    return;
}

void run_test()
{
    RTC_CPPC_DATA data{ 0 };
    auto cppc_capabilities = MSR::CPPC_CAPABILITY_1();
    data.cppc.MinPerf = cppc_capabilities.LowestPerf;
    data.cppc.MaxPerf = cppc_capabilities.HighestPerf;
    data.cppc.DesPerf = cppc_capabilities.NominalPerf;

    data.target_core = 0;
    ProbeCore(&data);

	auto mperf_delta = data.logical_core_end[0].mperf - data.logical_core_start[0].mperf;
	auto msr_tsc_delta = data.logical_core_end[0].msr_tsc - data.logical_core_start[0].msr_tsc;
	auto rdtsc_delta = data.logical_core_end[0].rdtsc - data.logical_core_start[0].rdtsc;
	auto rdtsp_delta = data.logical_core_end[0].rdtscp - data.logical_core_start[0].rdtscp;

    double sync_ratio = (double)mperf_delta / (double)msr_tsc_delta;
	sync_ratio += (double)mperf_delta / (double)rdtsc_delta;
	sync_ratio += (double)mperf_delta / (double)rdtsp_delta;
    int sync_ratio_total = abs64(100 - (int)((sync_ratio * 100.0) / 3.0));


	printf("TSC Desynchronization Threashold: 5%%\n");
    if (sync_ratio_total > 5)
    {
        printf("   [Flagged] - desync: %i%%\n", sync_ratio_total);
        printf("      * mperf rdtsc rdtscp msr_tsc : not synced\n");
    }
    else
    {
		printf("   [Normal] - desync: %i%%\n", sync_ratio_total);
    }

	auto io_apic_ratio = 920000.0 / (double)(data.logical_core_end[0].io_apicTimer - data.logical_core_start[0].io_apicTimer);
	auto reported_aperf_delta = data.logical_core_end[0].aperf - data.logical_core_start[0].aperf;
    auto reported_MHz_ratio = (double)reported_aperf_delta / (double)(data.logical_core_end[0].mperf - data.logical_core_start[0].mperf);
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

    printf("TEST %i\n", (int)(MHz_ratio * 100));

    auto MHz_ratio_total = 1.0 - (MHz_ratio / 4.0);
    auto reported_MHz_ratio_total = abs64((int)(MHz_ratio_total * 100.0));

	printf("P0 MHz Ratio Threashold: 5%%\n");
    if(reported_MHz_ratio_total > 5)
    {
        printf("   [Flagged] - desync: %i%%\n", reported_MHz_ratio_total);
    }
    else
    {
        printf("   [Normal] - desync: %i%%\n", reported_MHz_ratio_total);
    }

	auto reported_cycles = reported_aperf_delta_ajusted;
	auto missing_cycles = (UINT64)(reported_aperf_delta_ajusted * MHz_ratio_total);

    printf("Reported cycles per batch: %i\n", reported_cycles / counter_total_ajusted);
    printf("Expected cycles per batch: %i\n", ((reported_aperf_delta_ajusted + missing_cycles) / counter_total_ajusted));

	printf("cycles unaccounted for: %i\n", missing_cycles);

    
    return;
}

NTSTATUS DriverEntry()
{
    run_test();
    return STATUS_SUCCESS;
}
