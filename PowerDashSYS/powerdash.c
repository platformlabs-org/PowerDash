#include "powerdash.h"
#include "ntdef.h"
#include <wdm.h>
#include <wdmsec.h>


/*!     \file powerdash.c
*/

#define NT_DEVICE_NAME L"\\Device\\PowerDash"   /* devices belong in \Device\;
                                                   a name under \Driver\ would collide
                                                   (case-insensitively) with the driver
                                                   object of the INF service 'powerdash' */
#define DOS_DEVICE_NAME L"\\DosDevices\\POWERDASH"

struct DeviceExtension
{
    HANDLE devMemHandle;
    HANDLE counterSetHandle;
    FAST_MUTEX smnMutex;       /* serializes the SMN 0xB8-addr / 0xBC-data window
                                  pair in IO_CTL_SMN_READ/WRITE (AMD northbridge
                                  B0:D0:F0, CF8/CFC 原生端口 I/O —— Hal 总线数据
                                  接口经 pci.sys 写过滤,0xBC 数据口写实测被拒
                                  (Krackan Point);WinRing0 生态(ryzenAdj/LHM)
                                  同走 CF8/CFC 端口)。0x60/0x64 数据口写亦被拒,
                                  两窗独立闩锁不可混用 */
    PDEVICE_OBJECT lowerDO;     /* FDO only: device below us in the PnP stack */
};

/*
 *  Fn+Q notification injection (Lenovo EnergyDrv / AcpiVpc.sys).
 *
 *  Synthesizes the kernel-side effect of the Fn+Q hotkey so that
 *  FnHotkeyUtility pops the power-mode OSD for the *current* DYTC mode:
 *      - devext + 0xEC + i*4  : per-channel notify counters
 *      - devext + 0x58 + i*8  : named events (EnergyDrvEvent1/2/3)
 *      - driver image + 0x7C80: mode2 registered consumer array,
 *        ctx size 0xB0, PKEVENT at ctx+0x10, count at image+0xD484
 *  Offsets verified against AcpiVpc.sys 15.11.30.11
 *  (SHA256 F589BB88137DED8BFEA1F2F741B51EF7F0BD27A4BE51A48978B91D0C29E936FE).
 */
#define VPC_MUTEX_OFF        0xA0
#define VPC_COUNTER_OFF(i)   (0xEC + (i) * 4)
#define VPC_NAMEDEV_OFF(i)   (0x58 + (i) * 8)
#define VPC_MODE2_ARRAY_RVA  0x7C80
#define VPC_MODE2_COUNT_RVA  0xD484
#define VPC_CTX_SIZE         0xB0
#define VPC_CTX_EVENT_OFF    0x10
#define VPC_MAX_CONSUMERS    0x3F

NTSYSAPI
NTSTATUS
NTAPI
ObReferenceObjectByName(
    __in PUNICODE_STRING ObjectName,
    __in ULONG Attributes,
    __in_opt PACCESS_STATE AccessState,
    __in_opt ACCESS_MASK DesiredAccess,
    __in_opt POBJECT_TYPE ObjectType,
    __in KPROCESSOR_MODE AccessMode,
    __inout_opt PVOID ParseContext,
    __out PVOID *Object);

extern POBJECT_TYPE IoDeviceObjectType;

static VOID FnQInject(PULONG64 pResult)
{
    UNICODE_STRING name;
    PDEVICE_OBJECT vpcDevObj = NULL;
    PFILE_OBJECT fileObj = NULL;
    PUCHAR devext, drvBase;
    PKEVENT event;
    PVOID mutex;
    LONG cnt, i;
    ULONG signaled = 0;
    NTSTATUS status;

    *pResult = 0;

    RtlInitUnicodeString(&name, L"\\Device\\EnergyDrv");
    status = ObReferenceObjectByName(&name, OBJ_CASE_INSENSITIVE, NULL, 0,
                                     IoDeviceObjectType, KernelMode, NULL,
                                     (PVOID *)&vpcDevObj);
    if (!NT_SUCCESS(status))
    {
        /* fallback through the DOS symlink */
        RtlInitUnicodeString(&name, L"\\DosDevices\\EnergyDrv");
        status = IoGetDeviceObjectPointer(&name, 0, &fileObj, &vpcDevObj);
        if (!NT_SUCCESS(status))
        {
            DbgPrint("PowerDash: FnQInject cannot find EnergyDrv => %08X\n", status);
            *pResult = 0x80000000ULL | (ULONG64)(0x01000000 | (status & 0xFFFFFF));
            return;
        }
    }

    devext = (PUCHAR)vpcDevObj->DeviceExtension;
    drvBase = (PUCHAR)vpcDevObj->DriverObject->DriverStart;

    if (!devext || !drvBase || vpcDevObj->DriverObject->DriverSize < VPC_MODE2_COUNT_RVA + 4)
    {
        *pResult = 0x80000000ULL | (ULONG64)0x02000000;
        goto out;
    }

    /* sanity: registered consumer count must be 0..0x3F */
    cnt = *(volatile LONG *)(drvBase + VPC_MODE2_COUNT_RVA);
    if (cnt < 0 || cnt > VPC_MAX_CONSUMERS)
    {
        *pResult = 0x80000000ULL | (ULONG64)0x03000000;
        goto out;
    }

    mutex = (PVOID)(devext + VPC_MUTEX_OFF);
    status = KeWaitForSingleObject(mutex, Executive, KernelMode, FALSE, NULL);
    if (!NT_SUCCESS(status))
    {
        *pResult = 0x80000000ULL | (ULONG64)(0x04000000 | (status & 0xFFFFFF));
        goto out;
    }

    /* mode2 counter = 1, exactly what a real Fn+Q press leaves behind */
    InterlockedExchange((PLONG)(devext + VPC_COUNTER_OFF(1)), 1);

    event = *(PKEVENT *)(devext + VPC_NAMEDEV_OFF(1));
    if (event)
    {
        KeSetEvent(event, IO_NO_INCREMENT, FALSE);
        signaled++;
    }

    for (i = 0; i < cnt; i++)
    {
        event = *(PKEVENT *)(drvBase + VPC_MODE2_ARRAY_RVA +
                             (ULONG)i * VPC_CTX_SIZE + VPC_CTX_EVENT_OFF);
        if (event)
        {
            KeSetEvent(event, IO_NO_INCREMENT, FALSE);
            signaled++;
        }
    }

    KeReleaseMutex(mutex, FALSE);
    *pResult = signaled;
    DbgPrint("PowerDash: FnQInject signaled %u events\n", signaled);

out:
    if (fileObj)
        ObDereferenceObject(fileObj);
    else
        ObDereferenceObject(vpcDevObj);
}

DRIVER_INITIALIZE DriverEntry;
DRIVER_ADD_DEVICE PnpAddDevice;

__drv_dispatchType(IRP_MJ_CREATE)
__drv_dispatchType(IRP_MJ_CLOSE)
DRIVER_DISPATCH dummyFunction;

__drv_dispatchType(IRP_MJ_PNP)
DRIVER_DISPATCH pnpDispatch;

__drv_dispatchType(IRP_MJ_POWER)
DRIVER_DISPATCH powerDispatch;

__drv_dispatchType(IRP_MJ_SYSTEM_CONTROL)
DRIVER_DISPATCH wmiDispatch;

__drv_dispatchType(IRP_MJ_DEVICE_CONTROL)
DRIVER_DISPATCH deviceControl;

DRIVER_UNLOAD MSRUnload;

static BOOLEAN g_weOwnControlDevice = TRUE;   /* FALSE if another instance
                                                 already owns \Device\PowerDash */

#ifdef ALLOC_PRAGMA
#pragma alloc_text(INIT,DriverEntry)
#pragma alloc_text(PAGE,MSRUnload)
#pragma alloc_text(PAGE,dummyFunction)
#pragma alloc_text(PAGE,pnpDispatch)
#pragma alloc_text(PAGE,powerDispatch)
#pragma alloc_text(PAGE,wmiDispatch)
#pragma alloc_text(PAGE,PnpAddDevice)
#pragma alloc_text(PAGE,deviceControl)
#endif


NTSTATUS
DriverEntry(
    __in PDRIVER_OBJECT DriverObject,
    __in PUNICODE_STRING RegistryPath
    )
{
    NTSTATUS status = STATUS_SUCCESS;
    UNICODE_STRING UnicodeString;
    UNICODE_STRING dosDeviceName;
    PDEVICE_OBJECT MSRSystemDeviceObject = NULL;
    struct DeviceExtension * pExt = NULL;
    UNICODE_STRING devMemPath;
    OBJECT_ATTRIBUTES attr;
    PFILE_OBJECT existingFileObject = NULL;
    PDEVICE_OBJECT existingDeviceObject = NULL;

    UNREFERENCED_PARAMETER(RegistryPath);

    RtlInitUnicodeString(&UnicodeString, NT_DEVICE_NAME);
    RtlInitUnicodeString(&dosDeviceName, DOS_DEVICE_NAME);

    /*
     *  The control device name is global while this driver can be loaded
     *  twice (SCM service 'PowerDashSYS' by PowerDash.exe AND the INF
     *  service 'powerdash' via PnP). If another instance already owns
     *  \Device\PowerDash, reuse it instead of failing with a name
     *  collision; we then own neither device nor symlink and must not
     *  delete them on unload.
     */
    status = IoGetDeviceObjectPointer(&UnicodeString, FILE_ALL_ACCESS,
                                      &existingFileObject, &existingDeviceObject);
    if (NT_SUCCESS(status))
    {
        ObDereferenceObject(existingFileObject);
        g_weOwnControlDevice = FALSE;
        DbgPrint("PowerDash: control device already owned by another instance\n");

        DriverObject->DriverUnload = MSRUnload;
        DriverObject->DriverExtension->AddDevice = PnpAddDevice;
        DriverObject->MajorFunction[IRP_MJ_CLOSE] = dummyFunction;
        DriverObject->MajorFunction[IRP_MJ_CREATE] = dummyFunction;
        DriverObject->MajorFunction[IRP_MJ_PNP] = pnpDispatch;
        DriverObject->MajorFunction[IRP_MJ_POWER] = powerDispatch;
        DriverObject->MajorFunction[IRP_MJ_SYSTEM_CONTROL] = wmiDispatch;
        DriverObject->MajorFunction[IRP_MJ_DEVICE_CONTROL] = deviceControl;
        return STATUS_SUCCESS;
    }
    g_weOwnControlDevice = TRUE;

#if 1
    status = IoCreateDeviceSecure(DriverObject,
        sizeof(struct DeviceExtension),
        &UnicodeString,
        FILE_DEVICE_UNKNOWN,
        FILE_DEVICE_SECURE_OPEN,
        FALSE,
        &SDDL_DEVOBJ_SYS_ALL_ADM_ALL,
        NULL,
        &MSRSystemDeviceObject
    );
#else
    status = IoCreateDevice(DriverObject,
                            sizeof(struct DeviceExtension),
                            &UnicodeString,
                            FILE_DEVICE_UNKNOWN,
                            FILE_DEVICE_SECURE_OPEN,
                            FALSE,
                            &MSRSystemDeviceObject
                            );
#endif

    if (!NT_SUCCESS(status))
        return status;

    DriverObject->DriverUnload = MSRUnload;
    DriverObject->DriverExtension->AddDevice = PnpAddDevice;
    DriverObject->MajorFunction[IRP_MJ_CLOSE] = dummyFunction;
    DriverObject->MajorFunction[IRP_MJ_CREATE] = dummyFunction;
    DriverObject->MajorFunction[IRP_MJ_PNP] = pnpDispatch;
    DriverObject->MajorFunction[IRP_MJ_POWER] = powerDispatch;
    DriverObject->MajorFunction[IRP_MJ_SYSTEM_CONTROL] = wmiDispatch;
    DriverObject->MajorFunction[IRP_MJ_DEVICE_CONTROL] = deviceControl;

    pExt = DriverObject->DeviceObject->DeviceExtension;
    RtlInitUnicodeString(&devMemPath, L"\\Device\\PhysicalMemory");
    InitializeObjectAttributes(&attr, &devMemPath, OBJ_KERNEL_HANDLE, (HANDLE)NULL, (PSECURITY_DESCRIPTOR)NULL);
    status = ZwOpenSection(&pExt->devMemHandle, SECTION_MAP_READ | SECTION_MAP_WRITE, &attr);
    if (!NT_SUCCESS(status))
    {
        DbgPrint("Error: failed ZwOpenSection(devMemHandle) => %08X\n", status);
        return status;
    }
    pExt->counterSetHandle = NULL;
    ExInitializeFastMutex(&pExt->smnMutex);   /* DriverEntry runs at PASSIVE_LEVEL */

    IoCreateSymbolicLink(&dosDeviceName, &UnicodeString);

    return status;
}


NTSTATUS dummyFunction(PDEVICE_OBJECT DeviceObject, PIRP Irp)
{
    UNREFERENCED_PARAMETER(DeviceObject);

    PAGED_CODE();

    Irp->IoStatus.Status = STATUS_SUCCESS;
    Irp->IoStatus.Information = 0;


    IoCompleteRequest(Irp, IO_NO_INCREMENT);

    return STATUS_SUCCESS;
}


/*
 *  PnP support (INF installable).
 *
 *  The driver works in two modes:
 *    - SCM service mode: DriverEntry creates the named control device
 *      (\Device\PowerDash + \\.\POWERDASH) directly. Used by PowerDash.exe.
 *    - PnP mode (INF install via pnputil/devcon): DriverEntry runs first
 *      (control device already created), then AddDevice is called for the
 *      Root\PowerDash devnode. We create an unnamed FDO and attach it to
 *      the PDO; the control device keeps serving the IOCTL interface.
 */
NTSTATUS
PnpAddDevice(
    __in PDRIVER_OBJECT DriverObject,
    __in PDEVICE_OBJECT PhysicalDeviceObject
    )
{
    NTSTATUS status;
    PDEVICE_OBJECT fdo = NULL;
    struct DeviceExtension * pExt = NULL;

    UNREFERENCED_PARAMETER(DriverObject);

    PAGED_CODE();

    status = IoCreateDeviceSecure(DriverObject,
        sizeof(struct DeviceExtension),
        NULL,                       /* unnamed FDO */
        FILE_DEVICE_UNKNOWN,
        FILE_DEVICE_SECURE_OPEN,
        FALSE,
        &SDDL_DEVOBJ_SYS_ALL_ADM_ALL,
        NULL,
        &fdo
        );

    if (!NT_SUCCESS(status))
        return status;

    pExt = (struct DeviceExtension *)fdo->DeviceExtension;
    pExt->devMemHandle = NULL;
    pExt->counterSetHandle = NULL;
    ExInitializeFastMutex(&pExt->smnMutex);   /* AddDevice runs at PASSIVE_LEVEL */
    pExt->lowerDO = IoAttachDeviceToDeviceStack(fdo, PhysicalDeviceObject);

    if (!pExt->lowerDO)
    {
        IoDeleteDevice(fdo);
        return STATUS_DEVICE_REMOVED;
    }

    fdo->Flags &= ~DO_DEVICE_INITIALIZING;
    DbgPrint("PowerDash: AddDevice ok, FDO=%p attached to PDO=%p\n", fdo, PhysicalDeviceObject);

    return STATUS_SUCCESS;
}


NTSTATUS
pnpDispatch(
    __in PDEVICE_OBJECT DeviceObject,
    __inout PIRP Irp
    )
{
    struct DeviceExtension * pExt = (struct DeviceExtension *)DeviceObject->DeviceExtension;
    PIO_STACK_LOCATION irpStack = IoGetCurrentIrpStackLocation(Irp);
    NTSTATUS status;

    PAGED_CODE();

    if (!pExt->lowerDO)
    {
        /* not one of ours (should not happen) */
        Irp->IoStatus.Status = STATUS_INVALID_DEVICE_REQUEST;
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
        return STATUS_INVALID_DEVICE_REQUEST;
    }

    switch (irpStack->MinorFunction)
    {
    case IRP_MN_REMOVE_DEVICE:
        IoSkipCurrentIrpStackLocation(Irp);
        status = IoCallDriver(pExt->lowerDO, Irp);
        IoDetachDevice(pExt->lowerDO);
        IoDeleteDevice(DeviceObject);
        DbgPrint("PowerDash: RemoveDevice\n");
        return status;

    default:
        IoSkipCurrentIrpStackLocation(Irp);
        return IoCallDriver(pExt->lowerDO, Irp);
    }
}


/*
 *  Power / WMI IRPs. Without these dispatchers registered the I/O manager
 *  fails every IRP_MJ_POWER with STATUS_INVALID_DEVICE_REQUEST, which puts
 *  the devnode into problem-code-31 right after a successful start.
 *  FDO: pass down. Control device: complete with success.
 */
NTSTATUS
powerDispatch(
    __in PDEVICE_OBJECT DeviceObject,
    __inout PIRP Irp
    )
{
    struct DeviceExtension * pExt = (struct DeviceExtension *)DeviceObject->DeviceExtension;

    PAGED_CODE();

    if (pExt->lowerDO)
    {
        PoStartNextPowerIrp(Irp);
        IoSkipCurrentIrpStackLocation(Irp);
        return PoCallDriver(pExt->lowerDO, Irp);
    }

    PoStartNextPowerIrp(Irp);
    Irp->IoStatus.Status = STATUS_SUCCESS;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return STATUS_SUCCESS;
}


NTSTATUS
wmiDispatch(
    __in PDEVICE_OBJECT DeviceObject,
    __inout PIRP Irp
    )
{
    struct DeviceExtension * pExt = (struct DeviceExtension *)DeviceObject->DeviceExtension;

    PAGED_CODE();

    if (pExt->lowerDO)
    {
        IoSkipCurrentIrpStackLocation(Irp);
        return IoCallDriver(pExt->lowerDO, Irp);
    }

    Irp->IoStatus.Status = STATUS_SUCCESS;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return STATUS_SUCCESS;
}



VOID MSRUnload(PDRIVER_OBJECT DriverObject)
{
    PDEVICE_OBJECT deviceObject = DriverObject->DeviceObject;
    UNICODE_STRING nameString;

    PAGED_CODE();

    if (!g_weOwnControlDevice)
    {
        /* another instance owns the control device; our FDOs (if any) are
           deleted through IRP_MN_REMOVE, nothing else to do here */
        return;
    }

    RtlInitUnicodeString(&nameString, DOS_DEVICE_NAME);

    IoDeleteSymbolicLink(&nameString);

    if (deviceObject != NULL)
    {
        IoDeleteDevice(deviceObject);
    }
}


/* 原生 CF8/CFC 配置周期(内核端口 I/O)—— HalSetBusDataByOffset 经
   pci.sys 会被写过滤(B0:D0:F0 的 SMN 数据口 0xBC 写实测被拒,
   Krackan Point);WinRing0 生态(ryzenAdj/LHM)同样走 CF8/CFC 端口。
   仅支持 bus 0-15(legacy 机制覆盖范围),SMN 窗口在 B0:D0:F0,足够。 */
static NTSTATUS PciCfgDwordRead(ULONG reg, ULONG32* out)
{
    if (reg & 3) return STATUS_INVALID_PARAMETER;
    __outdword(0xCF8, 0x80000000u | (0u << 16) | (0u << 11) | (0u << 8) | reg);  /* B0:D0:F0 */
    *out = __indword(0xCFC);
    return STATUS_SUCCESS;
}
static NTSTATUS PciCfgDwordWrite(ULONG reg, ULONG32 value)
{
    if (reg & 3) return STATUS_INVALID_PARAMETER;
    __outdword(0xCF8, 0x80000000u | (0u << 16) | (0u << 11) | (0u << 8) | reg);
    __outdword(0xCFC, value);
    return STATUS_SUCCESS;
}

NTSTATUS deviceControl(PDEVICE_OBJECT DeviceObject, PIRP Irp)
{
    NTSTATUS status = STATUS_SUCCESS;
    PIO_STACK_LOCATION IrpStackLocation = NULL;
    struct MSR_Request * input_msr_req = NULL;
    struct PCICFG_Request * input_pcicfg_req = NULL;
    struct MMAP_Request* input_mmap_req = NULL;
    ULONG64 * output = NULL;
    GROUP_AFFINITY old_affinity, new_affinity;
    ULONG inputSize = 0;
    PCI_SLOT_NUMBER slot;
    unsigned size = 0;
    PROCESSOR_NUMBER ProcNumber;
    struct DeviceExtension* pExt = NULL;
    LARGE_INTEGER offset;
    SIZE_T mmapSize = 0;
    PVOID baseAddress = NULL;

    pExt = DeviceObject->DeviceExtension;

    PAGED_CODE();

    IrpStackLocation = IoGetCurrentIrpStackLocation(Irp);

    if (IrpStackLocation)
    {
        inputSize = IrpStackLocation->Parameters.DeviceIoControl.InputBufferLength;

        if (IrpStackLocation->Parameters.DeviceIoControl.OutputBufferLength >=
            sizeof(ULONG64))
        {
            input_msr_req = (struct MSR_Request *)Irp->AssociatedIrp.SystemBuffer;
            input_pcicfg_req = (struct PCICFG_Request *)Irp->AssociatedIrp.SystemBuffer;
            input_mmap_req = (struct MMAP_Request*)Irp->AssociatedIrp.SystemBuffer;
            output = (ULONG64 *)Irp->AssociatedIrp.SystemBuffer;

            RtlSecureZeroMemory(&ProcNumber, sizeof(PROCESSOR_NUMBER));

            switch (IrpStackLocation->Parameters.DeviceIoControl.IoControlCode)
            {
            case IO_CTL_MSR_WRITE:
                if (inputSize < sizeof(struct MSR_Request))
                {
                    status = STATUS_INVALID_PARAMETER;
                    break;
                }
                RtlSecureZeroMemory(&new_affinity, sizeof(GROUP_AFFINITY));
                RtlSecureZeroMemory(&old_affinity, sizeof(GROUP_AFFINITY));
                KeGetProcessorNumberFromIndex(input_msr_req->core_id, &ProcNumber);
                new_affinity.Group = ProcNumber.Group;
                new_affinity.Mask = 1ULL << (ProcNumber.Number);
                KeSetSystemGroupAffinityThread(&new_affinity, &old_affinity);
                __try
                {
                    __writemsr(input_msr_req->msr_address, input_msr_req->write_value);
                }
                __except (EXCEPTION_EXECUTE_HANDLER)
                {
                    status = GetExceptionCode();
                    DbgPrint("Error: exception with code 0x%X in IO_CTL_MSR_WRITE core 0x%X msr 0x%llX value 0x%llX\n",
                        status, input_msr_req->core_id, input_msr_req->msr_address, input_msr_req->write_value);
                }
                KeRevertToUserGroupAffinityThread(&old_affinity);
                Irp->IoStatus.Information = 0;                         // result size
                break;
            case IO_CTL_MSR_READ:
                if (inputSize < sizeof(struct MSR_Request))
                {
                    status = STATUS_INVALID_PARAMETER;
                    break;
                }
                RtlSecureZeroMemory(&new_affinity, sizeof(GROUP_AFFINITY));
                RtlSecureZeroMemory(&old_affinity, sizeof(GROUP_AFFINITY));
                KeGetProcessorNumberFromIndex(input_msr_req->core_id, &ProcNumber);
                new_affinity.Group = ProcNumber.Group;
                new_affinity.Mask = 1ULL << (ProcNumber.Number);
                KeSetSystemGroupAffinityThread(&new_affinity, &old_affinity);
                __try
                {
                    *output = __readmsr(input_msr_req->msr_address);
                }
                __except (EXCEPTION_EXECUTE_HANDLER)
                {
                    status = GetExceptionCode();
                    DbgPrint("Error: exception with code 0x%X in IO_CTL_MSR_READ core 0x%X msr 0x%llX\n",
                        status, input_msr_req->core_id, input_msr_req->msr_address);
                }
                KeRevertToUserGroupAffinityThread(&old_affinity);
                Irp->IoStatus.Information = sizeof(ULONG64);                         // result size
                break;
            case IO_CTL_MMAP_SUPPORT:
                *output = 1;
                Irp->IoStatus.Information = sizeof(ULONG64); // result size
                break;
            case IO_CTL_MMAP:
                offset = input_mmap_req->address;
                mmapSize = input_mmap_req->size;
                status = ZwMapViewOfSection(pExt->devMemHandle, ZwCurrentProcess(), &baseAddress, 0L, PAGE_SIZE, &offset, &mmapSize, ViewUnmap, 0, PAGE_READWRITE);
                if (status != STATUS_SUCCESS || baseAddress == NULL)
                {
                    DbgPrint("Error: ZwMapViewOfSection failed, %lld %lld (%ld).\n", offset.QuadPart, mmapSize, status);
                }
                else
                {
                    *output = (ULONG64)baseAddress;
                    Irp->IoStatus.Information = sizeof(PVOID); // result size
                }
                break;
            case IO_CTL_MUNMAP:
                status = ZwUnmapViewOfSection(ZwCurrentProcess(), (PVOID) input_mmap_req->address.QuadPart);
                break;
            case IO_CTL_PMU_ALLOC_SUPPORT:
                *output = 1;
                Irp->IoStatus.Information = sizeof(ULONG64); // result size
                break;
            case IO_CTL_PMU_ALLOC:
                if (pExt->counterSetHandle == NULL)
                {
                    status = HalAllocateHardwareCounters(NULL, 0, NULL, &(pExt->counterSetHandle));
                }
                *output = status;
                Irp->IoStatus.Information = sizeof(ULONG64); // result size
                break;
            case IO_CTL_PMU_FREE:
                if (pExt->counterSetHandle != NULL)
                {
                    status = HalFreeHardwareCounters(pExt->counterSetHandle);
                    if (status == STATUS_SUCCESS)
                    {
                        pExt->counterSetHandle = NULL;
                    }
                }
                *output = status;
                Irp->IoStatus.Information = sizeof(ULONG64); // result size
                break;
            case IO_CTL_PCICFG_WRITE:
                if (inputSize < sizeof(struct PCICFG_Request) || (input_pcicfg_req->bytes != 4 && input_pcicfg_req->bytes != 8))
                {
                    status = STATUS_INVALID_PARAMETER;
                    break;
                }
                slot.u.AsULONG = 0;
                slot.u.bits.DeviceNumber = input_pcicfg_req->dev;
                slot.u.bits.FunctionNumber = input_pcicfg_req->func;
#pragma warning(push)
#pragma warning(disable: 4996)
                __try
                {
                    size = HalSetBusDataByOffset(PCIConfiguration, input_pcicfg_req->bus, slot.u.AsULONG,
                        &(input_pcicfg_req->write_value), input_pcicfg_req->reg, input_pcicfg_req->bytes);
                }
                __except (EXCEPTION_EXECUTE_HANDLER)
                {
                    status = GetExceptionCode();
                    size = 0;
                    DbgPrint("Error: exception with code 0x%X in IO_CTL_PCICFG_WRITE b 0x%X d 0x%X f 0x%X reg 0x%X bytes 0x%X value 0x%llX\n",
                        status, input_pcicfg_req->bus, input_pcicfg_req->dev, input_pcicfg_req->func, input_pcicfg_req->reg, input_pcicfg_req->bytes,
                        input_pcicfg_req->write_value);
                }
#pragma warning(pop)
                if (size != input_pcicfg_req->bytes)
                {
                    status = STATUS_INVALID_PARAMETER;
                    break;
                }
                Irp->IoStatus.Information = 0;                                         // result size
                break;
            case IO_CTL_PCICFG_READ:
                if (inputSize < sizeof(struct PCICFG_Request) || (input_pcicfg_req->bytes != 4 && input_pcicfg_req->bytes != 8))
                {
                    status = STATUS_INVALID_PARAMETER;
                    break;
                }
                slot.u.AsULONG = 0;
                slot.u.bits.DeviceNumber = input_pcicfg_req->dev;
                slot.u.bits.FunctionNumber = input_pcicfg_req->func;
#pragma warning(push)
#pragma warning(disable: 4996)
                __try
                {
                    size = HalGetBusDataByOffset(PCIConfiguration, input_pcicfg_req->bus, slot.u.AsULONG,
                        output, input_pcicfg_req->reg, input_pcicfg_req->bytes);
                }
                __except (EXCEPTION_EXECUTE_HANDLER)
                {
                    status = GetExceptionCode();
                    size = 0;
                    DbgPrint("Error: exception with code 0x%X in IO_CTL_PCICFG_READ b 0x%X d 0x%X f 0x%X reg 0x%X bytes 0x%X\n",
                        status, input_pcicfg_req->bus, input_pcicfg_req->dev, input_pcicfg_req->func, input_pcicfg_req->reg, input_pcicfg_req->bytes);
                }
#pragma warning(pop)
                if (size != input_pcicfg_req->bytes)
                {
                    status = STATUS_INVALID_PARAMETER;
                    break;
                }
                Irp->IoStatus.Information = size;                                         // result size
                break;

            case IO_CTL_SMN_READ:
            {
                struct SMN_Request* req = (struct SMN_Request*)Irp->AssociatedIrp.SystemBuffer;
                if (inputSize < sizeof(struct SMN_Request))
                {
                    status = STATUS_INVALID_PARAMETER;
                    break;
                }
                ExAcquireFastMutex(&pExt->smnMutex);
                __try
                {
                    /* CF8/CFC 原生端口 I/O @ B0:D0:F0 0xB8(地址)/0xBC(数据),
                       缘由见 PciCfgDwordRead 头注释 */
                    /* 1) write the SMN address window 0xB8; sequential steps instead
                       of __leave so control always reaches the mutex release below */
                    status = PciCfgDwordWrite(0xB8, req->address);
                    /* 2) read the SMN data window 0xBC (same window as 0xB8 above;
                       the 0x60/0x64 window has a separate address latch) */
                    if (status == STATUS_SUCCESS)
                        status = PciCfgDwordRead(0xBC, &req->value);
                }
                __except (EXCEPTION_EXECUTE_HANDLER)
                {
                    status = GetExceptionCode();
                    DbgPrint("PowerDash: SMN read exception 0x%X addr 0x%X\n", status, req->address);
                }
                ExReleaseFastMutex(&pExt->smnMutex);         /* every path incl. exceptions */
                Irp->IoStatus.Information = sizeof(struct SMN_Request);   // METHOD_BUFFERED write-back
                break;
            }

            case IO_CTL_SMN_WRITE:
            {
                struct SMN_Request* req = (struct SMN_Request*)Irp->AssociatedIrp.SystemBuffer;
                if (inputSize < sizeof(struct SMN_Request))
                {
                    status = STATUS_INVALID_PARAMETER;
                    break;
                }
                ExAcquireFastMutex(&pExt->smnMutex);
                __try
                {
                    /* CF8/CFC 原生端口 I/O @ B0:D0:F0 0xB8(地址)/0xBC(数据),
                       缘由见 PciCfgDwordRead 头注释;same window as IO_CTL_SMN_READ:
                       the two SMN windows have separate address latches and the
                       0x64 data port rejects writes on Krackan */
                    status = PciCfgDwordWrite(0xB8, req->address);
                    if (status == STATUS_SUCCESS)
                        status = PciCfgDwordWrite(0xBC, req->value);
                }
                __except (EXCEPTION_EXECUTE_HANDLER)
                {
                    status = GetExceptionCode();
                    DbgPrint("PowerDash: SMN write exception 0x%X addr 0x%X\n", status, req->address);
                }
                ExReleaseFastMutex(&pExt->smnMutex);
                Irp->IoStatus.Information = 0;
                break;
            }

            case IO_CTL_FNQ_INJECT:
                FnQInject(output);          /* count of events, or 0x8ZZSSSS error code */
                Irp->IoStatus.Information = sizeof(ULONG64);
                break;

            default:
                status = STATUS_INVALID_DEVICE_REQUEST;
            }
        }
        else
            status = STATUS_INVALID_PARAMETER;
    }
    else
        status = STATUS_INVALID_DEVICE_REQUEST;


    Irp->IoStatus.Status = status;

    IoCompleteRequest(Irp, IO_NO_INCREMENT);

    return status;
}
