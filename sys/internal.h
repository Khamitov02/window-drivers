#pragma once

struct LOG_QUEUE_ITEM {
    LIST_ENTRY list_entry;
    PROC_NAME process_name;
};

#define NT_DEVICE_NAME  L"\\Device\\PROCSPY"
#define DOS_DEVICE_NAME L"\\DosDevices\\PROCSPY"
#define MEMORY_TAG 'DPRM'

#if DBG
#define PROCSPY_KDPRINT(...)   \
    DbgPrint("PROCSPY.SYS: "); \
    DbgPrint(__VA_ARGS__);
#else
#define PROCSPY_KDPRINT(...)
#endif


DRIVER_INITIALIZE DriverEntry;
_Dispatch_type_(IRP_MJ_CREATE)
_Dispatch_type_(IRP_MJ_CLOSE)
DRIVER_DISPATCH ProcSpyCreateClose;
_Dispatch_type_(IRP_MJ_DEVICE_CONTROL)
DRIVER_DISPATCH ProcSpyDeviceControl;
DRIVER_UNLOAD ProcSpyUnloadDriver;

void ProcSpyCreateProc(
    PEPROCESS Process,
    HANDLE ProcessId,
    PPS_CREATE_NOTIFY_INFO CreateInfo
);

PLIST_ENTRY MyKeRemoveQueue(PRKQUEUE Queue);
NTSTATUS ProcSpyKillProc(HANDLE ProcessID);
VOID PrintIrpInfo(PIRP Irp);

#ifdef ALLOC_PRAGMA
#pragma alloc_text(INIT, DriverEntry)
#pragma alloc_text(PAGE, ProcSpyCreateClose)
#pragma alloc_text(PAGE, ProcSpyDeviceControl)
#pragma alloc_text(PAGE, ProcSpyUnloadDriver)
#pragma alloc_text(PAGE, PrintIrpInfo)
#endif
struct LOG_QUEUE_DATA {
    LIST_ENTRY ListEntry;
    PROC_NAME NewProcName;
    HANDLE ProcessID;
};

#define LOG_QUEUE_MAX_SIZE  0x80
