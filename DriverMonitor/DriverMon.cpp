#include "pch.h"
#include "DriverMon.h"
#include "CyclicBuffer.h"

//
// prototypes
//

void DriverMonUnload(PDRIVER_OBJECT);
NTSTATUS DriverMonCreateClose(PDEVICE_OBJECT, PIRP);
NTSTATUS DriverMonDeviceControl(PDEVICE_OBJECT, PIRP);
NTSTATUS CompleteRequest(PIRP Irp, NTSTATUS status = STATUS_SUCCESS, ULONG_PTR Information = 0);
NTSTATUS AddDriver(PCWSTR driverName, PVOID* driverObject);
NTSTATUS RemoveDriver(PVOID DriverObject);
NTSTATUS RemoveDriver(int index);
NTSTATUS DriverMonGenericDispatch(PDEVICE_OBJECT, PIRP);
void RemoveAllDrivers();

DriverMonGlobals globals;

extern "C"
NTSTATUS DriverEntry(PDRIVER_OBJECT DriverObject, PUNICODE_STRING /* RegistryPath */) {
    DriverObject->DriverUnload = DriverMonUnload;
    DriverObject->MajorFunction[IRP_MJ_CREATE] = DriverObject->MajorFunction[IRP_MJ_CLOSE] = DriverMonCreateClose;
    DriverObject->MajorFunction[IRP_MJ_DEVICE_CONTROL] = DriverMonDeviceControl;

    UNICODE_STRING name, symLink;
    RtlInitUnicodeString(&name, DeviceName);
    RtlInitUnicodeString(&symLink, DeviceSymLink);

    globals.DataBuffer = new (NonPagedPool) CyclicBuffer;
    if (globals.DataBuffer == nullptr)
        return STATUS_INSUFFICIENT_RESOURCES;

    auto status = globals.DataBuffer->Init(1 << 16, NonPagedPool, DRIVER_TAG);
    if (!NT_SUCCESS(status)) {
        delete globals.DataBuffer;
        return status;
    }

    PDEVICE_OBJECT DeviceObject;
    status = IoCreateDevice(DriverObject, 0, &name, FILE_DEVICE_UNKNOWN, FILE_DEVICE_SECURE_OPEN, TRUE, &DeviceObject);
    if (!NT_SUCCESS(status)) {
        KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL, DRIVER_PREFIX "Error creating device object (0x%08X)\n", status));
        delete globals.DataBuffer;
        return status;
    }

    status = IoCreateSymbolicLink(&symLink, &name);
    if (!NT_SUCCESS(status)) {
        KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL, DRIVER_PREFIX "Error creating symbolic link (0x%08X)\n", status));
        IoDeleteDevice(DeviceObject);
        delete globals.DataBuffer;
        return status;
    }

    KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_INFO_LEVEL, DRIVER_PREFIX "DriverEntry completed successfully\n"));

    return status;
}

NTSTATUS CompleteRequest(PIRP Irp, NTSTATUS status, ULONG_PTR Information) {
    Irp->IoStatus.Status = status;
    Irp->IoStatus.Information = Information;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return status;
}

NTSTATUS DriverMonCreateClose(PDEVICE_OBJECT, PIRP Irp) {
    return CompleteRequest(Irp);
}

void DriverMonUnload(PDRIVER_OBJECT DriverObject) {
    RemoveAllDrivers();

    if (globals.NotifyEvent)
        ObDereferenceObject(globals.NotifyEvent);

    delete globals.DataBuffer;

    UNICODE_STRING symLink;
    RtlInitUnicodeString(&symLink, DeviceSymLink);
    IoDeleteSymbolicLink(&symLink);
    IoDeleteDevice(DriverObject->DeviceObject);
}

NTSTATUS DriverMonDeviceControl(PDEVICE_OBJECT, PIRP Irp) {
    auto stack = IoGetCurrentIrpStackLocation(Irp);
    auto status = STATUS_SUCCESS;
    ULONG_PTR information = 0;
    auto inputLen = stack->Parameters.DeviceIoControl.InputBufferLength;
    auto outputLen = stack->Parameters.DeviceIoControl.OutputBufferLength;

    switch (static_cast<DriverMonIoctls>(stack->Parameters.DeviceIoControl.IoControlCode)) {
    case DriverMonIoctls::StartMonitoring:
        globals.IsMonitoring = true;
        KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_TRACE_LEVEL, DRIVER_PREFIX "Monitoring started\n"));
        break;

    case DriverMonIoctls::StopMonitoring:
        globals.IsMonitoring = false;
        KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_TRACE_LEVEL, DRIVER_PREFIX "Monitoring stopped\n"));
        break; 

    case DriverMonIoctls::SetEventHandle: {
        if (inputLen < sizeof(HANDLE)) {
            status = STATUS_BUFFER_TOO_SMALL;
            break;
        }
        PKEVENT event;
        status = ObReferenceObjectByHandle(*(HANDLE*)Irp->AssociatedIrp.SystemBuffer, EVENT_ALL_ACCESS, *ExEventObjectType, KernelMode, (PVOID*)&event, nullptr);
        if (!NT_SUCCESS(status))
            break;

        auto oldEvent = InterlockedExchangePointer((PVOID*)&globals.NotifyEvent, event);

        if (oldEvent)
            ObDereferenceObject(oldEvent);
        break;
    }

    case DriverMonIoctls::AddDriver: {
        if (globals.Count >= MaxMonitoredDrivers) {
            status = STATUS_TOO_MANY_ADDRESSES;
            break;
        }

        if (inputLen < 1 || inputLen > 64 || outputLen < sizeof(PVOID)) {
            status = STATUS_INVALID_BUFFER_SIZE;
            break;
        }

        PCWSTR driverName = static_cast<PCWSTR>(Irp->AssociatedIrp.SystemBuffer);
        if (driverName[inputLen / sizeof(WCHAR) - 1] != L'\0') {
            status = STATUS_INVALID_PARAMETER;
            break;
        }
        status = AddDriver(driverName, (PVOID*)Irp->AssociatedIrp.SystemBuffer);
        if (NT_SUCCESS(status)) {
            information = sizeof(PVOID);
        }
        break;
    }

    case DriverMonIoctls::RemoveDriver:
        if (inputLen < sizeof(PVOID)) {
            status = STATUS_BUFFER_TOO_SMALL;
            break;
        }
        status = RemoveDriver(*(PVOID*)Irp->AssociatedIrp.SystemBuffer);
        break;

    case DriverMonIoctls::RemoveAll:
        RemoveAllDrivers();
        break;

    case DriverMonIoctls::GetData: {
        if (outputLen < sizeof(IrpArrivedInfo)) {
            status = STATUS_BUFFER_TOO_SMALL;
            break;
        }
        auto buffer = static_cast<PUCHAR>(MmGetSystemAddressForMdlSafe(Irp->MdlAddress, NormalPagePriority));
        if (buffer == nullptr) {
            status = STATUS_INSUFFICIENT_RESOURCES;
            break;
        }
        information = globals.DataBuffer->Read(buffer, outputLen);
        break;
    }

    default:
        status = STATUS_INVALID_DEVICE_REQUEST;
        break;
    }

    return CompleteRequest(Irp, status, information);
}

NTSTATUS AddDriver(PCWSTR driverName, PVOID* driverObject) {
    int index = -1;

    //
    // find first available slot, make sure driver is not already monitored
    //

    for (int i = 0; i < MaxMonitoredDrivers; ++i) {
        if (globals.Drivers[i].DriverObject == nullptr) {
            if (index < 0) {
                index = i;
            }
        }
        else {
            // existing driver, check if not already being monitored

            if (_wcsicmp(globals.Drivers[i].DriverName, driverName) == 0) {
                *driverObject = globals.Drivers[i].DriverObject;
                return STATUS_SUCCESS;
            }
        }
    }

    UNICODE_STRING name;
    RtlInitUnicodeString(&name, driverName);
    PDRIVER_OBJECT driver;
    auto status = ObReferenceObjectByName(&name, OBJ_CASE_INSENSITIVE, nullptr, 0, *IoDriverObjectType, KernelMode, nullptr, (PVOID*)&driver);
    if (!NT_SUCCESS(status))
        return status;

    ::wcscpy(globals.Drivers[index].DriverName, driverName);

    for (int i = 0; i <= IRP_MJ_MAXIMUM_FUNCTION; i++) {
        globals.Drivers[index].MajorFunction[i] = static_cast<PDRIVER_DISPATCH>(
            InterlockedExchangePointer((PVOID*)&driver->MajorFunction[i], DriverMonGenericDispatch));
    }

    globals.Drivers[index].DriverObject = driver;
    ++globals.Count;
    *driverObject = driver;

    return STATUS_SUCCESS;
}

NTSTATUS RemoveDriver(PVOID DriverObject) {
    for (int i = 0; i < MaxMonitoredDrivers; ++i) {
        auto& driver = globals.Drivers[i];
        if (driver.DriverObject == DriverObject) {
            return RemoveDriver(i);
        }
    }
    return STATUS_INVALID_PARAMETER;
}

NTSTATUS RemoveDriver(int i) {
    auto& driver = globals.Drivers[i];
    for (int j = 0; j <= IRP_MJ_MAXIMUM_FUNCTION; j++) {
        InterlockedExchangePointer((PVOID*)&driver.DriverObject->MajorFunction[j], driver.MajorFunction[j]);
    }
    globals.Count--;
    ObDereferenceObject(driver.DriverObject);
    driver.DriverObject = nullptr;

    return STATUS_SUCCESS;
}

NTSTATUS DriverMonGenericDispatch(PDEVICE_OBJECT DeviceObject, PIRP Irp) {
    auto driver = DeviceObject->DriverObject;
    auto stack = IoGetCurrentIrpStackLocation(Irp);

    for (int i = 0; i < MaxMonitoredDrivers; ++i) {
        if (globals.Drivers[i].DriverObject == driver) {
            if (globals.IsMonitoring && globals.NotifyEvent) {
                // report operation
                KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_TRACE_LEVEL, DRIVER_PREFIX "Driver 0x%p intercepted!\n", driver));

                IrpArrivedInfo info;
                info.Type = DataItemType::IrpArrived;
                KeQuerySystemTime((PLARGE_INTEGER)&info.Time);
                info.Size = sizeof(info);
                info.DeviceObject = DeviceObject;
                info.Irp = Irp;
                info.DriverObject = driver;
                info.MajorFunction = static_cast<IrpMajorCode>(stack->MajorFunction);
                info.MinorFunction = static_cast<IrpMinorCode>(stack->MinorFunction);
                info.ProcessId = HandleToULong(PsGetCurrentProcessId());
                info.ThreadId = HandleToULong(PsGetCurrentThreadId());
                info.Irql = KeGetCurrentIrql();

                switch (info.MajorFunction) {
                case IrpMajorCode::READ:
                case IrpMajorCode::WRITE:
                    info.Write.Length = stack->Parameters.Write.Length;
                    info.Write.Offset = stack->Parameters.Write.ByteOffset.QuadPart;
                    break;

                case IrpMajorCode::DEVICE_CONTROL:
                case IrpMajorCode::INTERNAL_DEVICE_CONTROL:
                    info.DeviceIoControl.IoControlCode = stack->Parameters.DeviceIoControl.IoControlCode;
                    info.DeviceIoControl.InputBufferLength = stack->Parameters.DeviceIoControl.InputBufferLength;
                    info.DeviceIoControl.OutputBufferLength = stack->Parameters.DeviceIoControl.OutputBufferLength;
                    break;
                }

                globals.DataBuffer->Write(&info, info.Size);
                if (globals.NotifyEvent)
                    KeSetEvent(globals.NotifyEvent, 2, FALSE);
            }

            return globals.Drivers[i].MajorFunction[stack->MajorFunction](DeviceObject, Irp);
        }
    }

    // should never get here!

    NT_ASSERT(0);
    return STATUS_UNSUCCESSFUL;
}

void RemoveAllDrivers() {
    for (int i = 0; i < MaxMonitoredDrivers; ++i) {
        if (globals.Drivers[i].DriverObject)
            RemoveDriver(i);
    }
    NT_ASSERT(globals.Count == 0);
}
