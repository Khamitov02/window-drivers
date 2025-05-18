#include <ntifs.h>
#include <ntstrsafe.h>
#include <ntddk.h>
#include <string.h>
#include "driver.h"
#include "internal.h"


static KQUEUE g_LogQueue;
static volatile LONG g_LoqQueueSize;
static volatile HANDLE g_LastReportedProcessID;


NTSTATUS DriverEntry(
    _In_ PDRIVER_OBJECT DriverObject,
    _In_ PUNICODE_STRING RegistryPath
) {
    UNREFERENCED_PARAMETER(RegistryPath);

    UNICODE_STRING ntUnicodeString = RTL_CONSTANT_STRING(NT_DEVICE_NAME);

    PDEVICE_OBJECT deviceObject = NULL;  // ptr to device object
    NTSTATUS ntStatus = IoCreateDevice(
        DriverObject,             // Our Driver Object
        0,                        // We don't use a device extension
        &ntUnicodeString,         // Device name "\Device\PROCSPY"
        FILE_DEVICE_UNKNOWN,      // Device type
        FILE_DEVICE_SECURE_OPEN,  // Device characteristics
        FALSE,                    // Not an exclusive device
        &deviceObject             // Returned ptr to Device Object
    );

    if (!NT_SUCCESS(ntStatus)) {
        PROCSPY_KDPRINT("Can't create dev obj\n");
        return ntStatus;
    }

    // Initialize driver's entry points.
    DriverObject->MajorFunction[IRP_MJ_CREATE] = ProcSpyCreateClose;
    DriverObject->MajorFunction[IRP_MJ_CLOSE] = ProcSpyCreateClose;
    DriverObject->MajorFunction[IRP_MJ_DEVICE_CONTROL] = ProcSpyDeviceControl;
    DriverObject->DriverUnload = ProcSpyUnloadDriver;
    UNICODE_STRING ntWin32NameString = RTL_CONSTANT_STRING(DOS_DEVICE_NAME);

    // Create link between our device and the Win32 name
    ntStatus = IoCreateSymbolicLink(
        &ntWin32NameString, &ntUnicodeString
    );

    if (!NT_SUCCESS(ntStatus)) {
        // Delete everything that this routine has allocated.
        PROCSPY_KDPRINT("Can't create symbolic link\n");
        IoDeleteDevice(deviceObject);
        return ntStatus;
    }

    KeInitializeQueue(&g_LogQueue, 0);
    InterlockedExchange(&g_LoqQueueSize, 0);
    InterlockedExchangePointer(&g_LastReportedProcessID, NULL);

    ntStatus = PsSetCreateProcessNotifyRoutineEx(ProcSpyCreateProc, FALSE);
    if (!NT_SUCCESS(ntStatus)) {
        // Delete everything that this routine has allocated.
        PROCSPY_KDPRINT("Can't create proc callback\n");
        IoDeleteSymbolicLink(&ntWin32NameString);
        IoDeleteDevice(deviceObject);
        return ntStatus;
    }

    return STATUS_SUCCESS;
}


VOID ProcSpyUnloadDriver(
    _In_ PDRIVER_OBJECT DriverObject
) {
    PDEVICE_OBJECT deviceObject = DriverObject->DeviceObject;
    PAGED_CODE();
    PsSetCreateProcessNotifyRoutineEx(ProcSpyCreateProc, TRUE);
    KeRundownQueue(&g_LogQueue);

    for (
        PLIST_ENTRY listEntry = MyKeRemoveQueue(&g_LogQueue);
        listEntry;
        listEntry = MyKeRemoveQueue(&g_LogQueue)
    ) {
        struct LOG_QUEUE_DATA *data = CONTAINING_RECORD(listEntry, struct LOG_QUEUE_DATA, ListEntry);
        ExFreePoolWithTag(data, MEMORY_TAG);
        InterlockedDecrement(&g_LoqQueueSize);
    }

    UNICODE_STRING uniWin32NameString = RTL_CONSTANT_STRING(DOS_DEVICE_NAME);
    IoDeleteSymbolicLink(&uniWin32NameString);

    if (deviceObject != NULL) {
        IoDeleteDevice(deviceObject);
    }
}


NTSTATUS ProcSpyDeviceControl(
    PDEVICE_OBJECT DeviceObject,
    PIRP Irp
) {
    NTSTATUS ntStatus = STATUS_SUCCESS;

    UNREFERENCED_PARAMETER(DeviceObject);
    PAGED_CODE();
    PIO_STACK_LOCATION irpSp = IoGetCurrentIrpStackLocation(Irp);

    ULONG inBufLength = irpSp->Parameters.DeviceIoControl.InputBufferLength;
    ULONG outBufLength = irpSp->Parameters.DeviceIoControl.OutputBufferLength;
    if (inBufLength < sizeof(struct PROCSPY_MESSAGE) || outBufLength < sizeof(struct PROCSPY_MESSAGE)) {
        ntStatus = STATUS_INVALID_PARAMETER;
        goto End;
    }

    if (irpSp->Parameters.DeviceIoControl.IoControlCode != IOCTL_PROCSPY_GET_SPAWNED_PROCESSES) {
        ntStatus = STATUS_INVALID_DEVICE_REQUEST;
        PROCSPY_KDPRINT("ERROR: unrecognized IOCTL %d\n", irpSp->Parameters.DeviceIoControl.IoControlCode);
        goto End;
    }

    struct PROCSPY_MESSAGE *message = Irp->AssociatedIrp.SystemBuffer;

    if (message->TerminateLast) {
        HANDLE ProcessID = InterlockedExchangePointer(&g_LastReportedProcessID, NULL);
        ntStatus = ProcSpyKillProc(ProcessID);
        message->TerminateLast = FALSE;
    }

    RtlZeroMemory(message, sizeof(*message));

    PLIST_ENTRY listEntry = MyKeRemoveQueue(&g_LogQueue);
    if (!listEntry) {
        message->NewProcName[0] = 0;
        message->MoreAvailable = FALSE;

        ntStatus = STATUS_NO_MORE_ENTRIES;
        goto End;
    }

    struct LOG_QUEUE_DATA *data = CONTAINING_RECORD(listEntry, struct LOG_QUEUE_DATA, ListEntry);

    RtlCopyMemory(message->NewProcName, data->NewProcName, sizeof(data->NewProcName));
    InterlockedExchangePointer(&g_LastReportedProcessID, data->ProcessID);

    ExFreePoolWithTag(data, MEMORY_TAG);

    LONG remainingItems = InterlockedDecrement(&g_LoqQueueSize);
    message->MoreAvailable = (remainingItems > 0);

    ntStatus = STATUS_SUCCESS;
    Irp->IoStatus.Information = sizeof(*message);

End:
    Irp->IoStatus.Status = ntStatus;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return ntStatus;
}


NTSTATUS ProcSpyCreateClose(
    PDEVICE_OBJECT DeviceObject,
    PIRP Irp
) {
    UNREFERENCED_PARAMETER(DeviceObject);

    PAGED_CODE();

    Irp->IoStatus.Status = STATUS_SUCCESS;
    Irp->IoStatus.Information = 0;

    IoCompleteRequest(Irp, IO_NO_INCREMENT);

    return STATUS_SUCCESS;
}


void ProcSpyCreateProc(
    PEPROCESS Process,
    HANDLE ProcessId,
    PPS_CREATE_NOTIFY_INFO CreateInfo
) {
    UNREFERENCED_PARAMETER(Process);

    if (!CreateInfo) {
        return;
    }

    if (!CreateInfo->ImageFileName) {
        return;
    }

    struct LOG_QUEUE_DATA *data = ExAllocatePool2(POOL_FLAG_PAGED, sizeof(struct LOG_QUEUE_DATA), MEMORY_TAG);
    // Note: we assume ascii process names, but if it isn't, we'll still handle it gracefully
    ANSI_STRING createdProcessName;
    NTSTATUS status = RtlUnicodeStringToAnsiString(&createdProcessName, CreateInfo->ImageFileName, TRUE);
    if (!NT_SUCCESS(status)) {
        PROCSPY_KDPRINT("Failed to convert process name to ANSI: %d", status);
        return;
    }

    status = RtlStringCchCopyNA(
        data->NewProcName,
        sizeof(data->NewProcName) - 1,
        createdProcessName.Buffer,
        createdProcessName.Length
    );
    RtlFreeAnsiString(&createdProcessName);
    if (!NT_SUCCESS(status)) {
        PROCSPY_KDPRINT("Failed to copy process name to buffer: %d", status);
        return;
    }
	
    data->ProcessID = ProcessId;
    KeInsertQueue(&g_LogQueue, &data->ListEntry);

    LONG queueSize = InterlockedIncrement(&g_LoqQueueSize);
    if (queueSize > LOG_QUEUE_MAX_SIZE) {
        // If the queue grows too big, pop the extra item
        PLIST_ENTRY listEntry = MyKeRemoveQueue(&g_LogQueue);
        if (listEntry) {
            struct LOG_QUEUE_DATA *head = CONTAINING_RECORD(listEntry, struct LOG_QUEUE_DATA, ListEntry);
            ExFreePoolWithTag(head, MEMORY_TAG);
            InterlockedDecrement(&g_LoqQueueSize);
        }
    }
}


PLIST_ENTRY MyKeRemoveQueue(PRKQUEUE Queue) {
    LARGE_INTEGER ZeroTimeout = {
        .QuadPart = 0
    };

    PLIST_ENTRY listEntry = KeRemoveQueue(Queue, KernelMode, &ZeroTimeout);

    ULONG_PTR status = (ULONG_PTR)listEntry;

    if (status == STATUS_TIMEOUT || status == STATUS_USER_APC || status == STATUS_ABANDONED) {
        return NULL;
    }

    return listEntry;
}


NTSTATUS ProcSpyKillProc(HANDLE ProcessID) {
    if (ProcessID == NULL) {
        return STATUS_INVALID_PARAMETER;
    }

    PEPROCESS Process;
    NTSTATUS status = PsLookupProcessByProcessId(ProcessID, &Process);
    if (status == STATUS_INVALID_CID) {
        // If a process is already dead, it's considered a success
        return STATUS_SUCCESS;
    }
    if (!NT_SUCCESS(status)) {
        return status;
    }

    HANDLE hProcess;
    status = ObOpenObjectByPointer(
        Process,
        OBJ_KERNEL_HANDLE,
        NULL,
        0x0001,  // PROCESS_TERMINATE
        *PsProcessType,
        KernelMode,
        &hProcess
    );
    if (!NT_SUCCESS(status)) {
        ObDereferenceObject(Process);
        return status;
    }

    status = ZwTerminateProcess(hProcess, STATUS_SUCCESS);
    
    ZwClose(hProcess);
    ObDereferenceObject(Process);

    return status;
}


VOID PrintIrpInfo(
    PIRP Irp
) {
    PIO_STACK_LOCATION irpSp;
    irpSp = IoGetCurrentIrpStackLocation(Irp);

    PAGED_CODE();

    PROCSPY_KDPRINT("\tIrp->AssociatedIrp.SystemBuffer = 0x%p\n", Irp->AssociatedIrp.SystemBuffer);
    PROCSPY_KDPRINT("\tIrp->UserBuffer = 0x%p\n", Irp->UserBuffer);
    PROCSPY_KDPRINT("\tirpSp->Parameters.DeviceIoControl.Type3InputBuffer = 0x%p\n", irpSp->Parameters.DeviceIoControl.Type3InputBuffer);
    PROCSPY_KDPRINT("\tirpSp->Parameters.DeviceIoControl.InputBufferLength = %d\n", irpSp->Parameters.DeviceIoControl.InputBufferLength);
    PROCSPY_KDPRINT("\tirpSp->Parameters.DeviceIoControl.OutputBufferLength = %d\n", irpSp->Parameters.DeviceIoControl.OutputBufferLength);
    
    return;
}
