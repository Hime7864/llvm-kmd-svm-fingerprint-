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




void ProbeCounters(TSC_DATA* out_delta)
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

struct RTC_CPPC_DATA
{
	int target_core;
    MSR_CPPC_REQUEST cppc;
	TSC_DATA logical_core[2];
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
        auto old = MSR::CPPC_REQUEST();
        MSR::CPPC_REQUEST(output->cppc);

        int idx = coreid % 2;
        TSC_DATA tsc_data;
        ProbeCounters(&tsc_data);
        output->logical_core[idx].aperf = tsc_data.aperf;
        output->logical_core[idx].mperf = tsc_data.mperf;
        output->logical_core[idx].energy = tsc_data.energy;
        output->logical_core[idx].msr_tsc = tsc_data.msr_tsc;
        output->logical_core[idx].io_apicTimer = tsc_data.io_apicTimer;
        output->logical_core[idx].rdtsc = tsc_data.rdtsc;
        output->logical_core[idx].rdtscp = tsc_data.rdtscp;
        output->logical_core[idx].counter = tsc_data.counter;
        output->logical_core[idx].pstate = tsc_data.pstate;

        MSR::CPPC_REQUEST(old);
    }
    return;
}

void ProbeCore(RTC_CPPC_DATA* output)
{
	KeIpiGenericCall(IpiCoreHandler, output);
    return;
}

NTSTATUS DriverEntry()
{
    RTC_CPPC_DATA data{ 0 };
    auto cppc_capabilities = MSR::CPPC_CAPABILITY_1();
	data.cppc.MinPerf = cppc_capabilities.LowestPerf;
	data.cppc.MaxPerf = cppc_capabilities.HighestPerf;
	data.cppc.DesPerf = cppc_capabilities.NominalPerf;
    data.target_core = 0;

	auto pstate_start = MSR::PSTATE_STATUS().CurPstate;
    ProbeCore(&data);
	printf("Pstate before:after : %i -> %i\n", pstate_start, MSR::PSTATE_STATUS().CurPstate);

    printf("Core %i) Pstate: %i -> a %i m %i e %i mt %i io %i tsc %i tscp %i cnt %i\n",
		data.target_core, data.logical_core[0].pstate, data.logical_core[0].aperf, data.logical_core[0].mperf, data.logical_core[0].energy, data.logical_core[0].msr_tsc, data.logical_core[0].io_apicTimer, data.logical_core[0].rdtsc, data.logical_core[0].rdtscp, data.logical_core[0].counter);
	printf("Core %i) Pstate: %i -> a %i m %i e %i mt %i io %i tsc %i tscp %i cnt %i\n",
		data.target_core, data.logical_core[1].pstate, data.logical_core[1].aperf, data.logical_core[1].mperf, data.logical_core[1].energy, data.logical_core[1].msr_tsc, data.logical_core[1].io_apicTimer, data.logical_core[1].rdtsc, data.logical_core[1].rdtscp, data.logical_core[1].counter);


    int core_with_highest_counter = 0;
    if(data.logical_core[1].counter > data.logical_core[0].counter)
        core_with_highest_counter = 1;

	//check if the core with the highest counter also show all counter are also larger than the other core, if not flag as suspicious

	printf("aperf delta: %i\n", data.logical_core[core_with_highest_counter].aperf - data.logical_core[!core_with_highest_counter].aperf);
	printf("mperf delta: %i\n", data.logical_core[core_with_highest_counter].mperf - data.logical_core[!core_with_highest_counter].mperf);
	printf("msr_tsc delta: %i\n", data.logical_core[core_with_highest_counter].msr_tsc - data.logical_core[!core_with_highest_counter].msr_tsc);
	printf("io_apicTimer delta: %i\n", data.logical_core[core_with_highest_counter].io_apicTimer - data.logical_core[!core_with_highest_counter].io_apicTimer);
	printf("rdtsc delta: %i\n", data.logical_core[core_with_highest_counter].rdtsc - data.logical_core[!core_with_highest_counter].rdtsc);
	printf("rdtscp delta: %i\n", data.logical_core[core_with_highest_counter].rdtscp - data.logical_core[!core_with_highest_counter].rdtscp);
	printf("counter delta: %i\n", data.logical_core[core_with_highest_counter].counter - data.logical_core[!core_with_highest_counter].counter);

    auto cnt_total = data.logical_core[core_with_highest_counter].counter + data.logical_core[!core_with_highest_counter].counter;
    auto aperf_val = data.logical_core[core_with_highest_counter].aperf / cnt_total;
    auto io_val = data.logical_core[core_with_highest_counter].io_apicTimer / cnt_total;
	auto tsc_val = data.logical_core[core_with_highest_counter].msr_tsc / cnt_total;
	printf("counter total %i\n", cnt_total);
	printf("aperf per count %i\n", aperf_val);
    printf("io_apicTimer per count %i\n", io_val);
    printf("tsc per count %i\n", tsc_val);
	printf("io to tsc %i\n", tsc_val / io_val);
    //printf("start");
    //run_detection();
	//printf("end");
	//printf("PM: %i\n", PM());
    //for (int i = 0; i < 10; i++)
    //{
    //    spin_100ms();
    //    printf("1\n");
    //}
    //for (int i = 0; i < 10; i++)
    //{
    //    spin_100ms();
    //    printf("2\n");
    //}
    //PauseTest();
    //auto cppc = MSR::CPPC_CAPABILITY_1();
	//printf("LowestPerf: %i\n", cppc.LowestPerf);
	//printf("LowNonLinPerf: %i\n", cppc.LowNonLinPerf);
	//printf("NominalPerf: %i\n", cppc.NominalPerf);
	//printf("HighestPerf: %i\n", cppc.HighestPerf);
	//auto cur_pstate = MSR::PSTATE_STATUS().CurPstate;
    //auto pstate = MSR::PSTATE(cur_pstate);
	//printf("Current Pstate: %i -> %i\n", pstate.get_frequency_mhz(), cur_pstate);
	//printf("TscFreqSel: %i\n", MSR::HWCR().TscFreqSel);
	//printf("LockTscToCurrentP0: %i\n", MSR::HWCR().LockTscToCurrentP0);
	//printf("P(C) FID: %i\n", pstate.CpuFid);
	//printf("P(C) VID: %i\n", pstate.CpuVid);
	//printf("P(C) IddValue: %i\n", pstate.IddValue);
	//printf("P(C) IddDiv: %i\n", pstate.IddDiv);
	//printf("P(C) PstateEn: %i\n", pstate.PstateEn);
    //printf("cppc Enabled: %i\n", MSR::CPPC_ENABLE().CPPC_En);
	//auto P0 = MSR::PSTATE(0).get_frequency_mhz() * 100;
	//printf("P0: %i MHz\n", P0);
    //MSR_CPPC_REQUEST start = MSR::CPPC_REQUEST();
    //printf("base min perf: %i\n", start.MinPerf);
    //printf("base max perf: %i\n", start.MaxPerf);
    //auto start_cpy = start;
    //start_cpy.MinPerf = cppc.LowestPerf;
	//start_cpy.MaxPerf = cppc.HighestPerf;
    //auto irql = _mm_readcr8();
    //_mm_writecr8(15);
    //
    //printf("p0 %i", MSR::PSTATE(0).get_frequency_mhz());
    //DTC::ChangePstate(0);
    //ipi_thing();//KeIpiGenericCall(ipi_thing, nullptr);
    //printf("p1 %i", MSR::PSTATE(1).get_frequency_mhz());
    //DTC::ChangePstate(1);
    //ipi_thing();
    //
    //_mm_writecr8(irql);
    //auto irql = _mm_readcr8();
    //_mm_writecr8(15);
    //MSR::CPPC_REQUEST(start_cpy);
    //printf("min perf: %i\n", start_cpy.MinPerf);
    //printf("max perf: %i\n", start_cpy.MaxPerf);
    //Pause();
    //
    //for (int i = cppc.LowestPerf; i < cppc.HighestPerf; i+=10)
    //{
    //    SetPerformance(i);
    //    SingleRTC::rdtsc();
    //}
    //
    //_mm_writecr8(irql);
    //MSR::CPPC_REQUEST(start);
    return STATUS_SUCCESS;
}
