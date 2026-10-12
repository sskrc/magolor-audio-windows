/*
 * RT1015P speaker amplifier enable: drives the SDB GPIO (ACPI GpioIo resource of ACPI\RTL1015) high.
 */
#include <ntddk.h>
#include <wdf.h>
#include <ntstrsafe.h>
#include <gpio.h>
#include <reshub.h>

static HANDLE g_log;
static VOID Log(const char *fmt, ...)
{
    char buf[256]; va_list ap; IO_STATUS_BLOCK iosb; size_t len;
    if (!g_log) return;
    va_start(ap, fmt); RtlStringCbVPrintfA(buf, sizeof(buf) - 2, fmt, ap); va_end(ap);
    RtlStringCbLengthA(buf, sizeof(buf) - 2, &len); buf[len++] = '\r'; buf[len++] = '\n';
    ZwWriteFile(g_log, NULL, NULL, NULL, &iosb, buf, (ULONG)len, NULL, NULL);
}
static VOID LogOpen(VOID)
{
    UNICODE_STRING n; OBJECT_ATTRIBUTES oa; IO_STATUS_BLOCK iosb;
    RtlInitUnicodeString(&n, L"\\??\\D:\\jsl-audio\\amp.log");
    InitializeObjectAttributes(&oa, &n, OBJ_KERNEL_HANDLE | OBJ_CASE_INSENSITIVE, NULL, NULL);
    if (!NT_SUCCESS(ZwCreateFile(&g_log, FILE_APPEND_DATA | SYNCHRONIZE, &oa, &iosb, NULL, FILE_ATTRIBUTE_NORMAL,
        FILE_SHARE_READ, FILE_OPEN_IF, FILE_SYNCHRONOUS_IO_NONALERT | FILE_WRITE_THROUGH | FILE_NON_DIRECTORY_FILE, NULL, 0)))
        g_log = NULL;
}

static WDFIOTARGET g_target;

static NTSTATUS PinWrite(UCHAR v)
{
    WDF_MEMORY_DESCRIPTOR in, out; UCHAR buf = v; UCHAR obuf = v; ULONG_PTR bytes = 0;
    WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(&in, &buf, sizeof(buf));
    WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(&out, &obuf, sizeof(obuf));
    return WdfIoTargetSendIoctlSynchronously(g_target, NULL, IOCTL_GPIO_WRITE_PINS, &in, &out, NULL, &bytes);
}

EVT_WDF_DRIVER_DEVICE_ADD EvtDeviceAdd;
EVT_WDF_DEVICE_PREPARE_HARDWARE EvtPrepareHardware;
EVT_WDF_DEVICE_RELEASE_HARDWARE EvtReleaseHardware;

NTSTATUS EvtPrepareHardware(WDFDEVICE dev, WDFCMRESLIST raw, WDFCMRESLIST xlat)
{
    ULONG n = WdfCmResourceListGetCount(xlat), i;
    UNREFERENCED_PARAMETER(raw);
    LogOpen();
    Log("=== jslamp start, %u resources ===", n);
    for (i = 0; i < n; i++) {
        PCM_PARTIAL_RESOURCE_DESCRIPTOR d = WdfCmResourceListGetDescriptor(xlat, i);
        Log("res %u: type=%u", i, d->Type);
        if (d->Type == CmResourceTypeConnection && d->u.Connection.Class == CM_RESOURCE_CONNECTION_CLASS_GPIO &&
            d->u.Connection.Type == CM_RESOURCE_CONNECTION_TYPE_GPIO_IO) {
            WCHAR pathBuf[64]; UNICODE_STRING path;
            WDF_IO_TARGET_OPEN_PARAMS op; NTSTATUS st;
            st = RtlStringCchPrintfW(pathBuf, 64, L"\\??\\RESOURCE_HUB\\%016I64x", ((ULONG64)d->u.Connection.IdHighPart << 32) | d->u.Connection.IdLowPart); RtlInitUnicodeString(&path, pathBuf);
            Log("gpio conn id=%08X:%08X path st=%08X", d->u.Connection.IdHighPart, d->u.Connection.IdLowPart, st);
            if (!NT_SUCCESS(st)) continue;
            st = WdfIoTargetCreate(dev, WDF_NO_OBJECT_ATTRIBUTES, &g_target);
            if (!NT_SUCCESS(st)) { Log("target create %08X", st); continue; }
            WDF_IO_TARGET_OPEN_PARAMS_INIT_OPEN_BY_NAME(&op, &path, GENERIC_READ | GENERIC_WRITE);
            op.ShareAccess = 0;
            st = WdfIoTargetOpen(g_target, &op);
            Log("open gpio target: %08X", st);
            if (!NT_SUCCESS(st)) continue;
            st = PinWrite(1);
            Log("write pin HIGH: %08X", st);
        }
    }
    return STATUS_SUCCESS;
}

NTSTATUS EvtReleaseHardware(WDFDEVICE dev, WDFCMRESLIST xlat)
{
    UNREFERENCED_PARAMETER(dev); UNREFERENCED_PARAMETER(xlat);
    if (g_target) { NTSTATUS st = PinWrite(0); Log("write pin LOW: %08X", st); WdfIoTargetClose(g_target); g_target = NULL; }
    if (g_log) { ZwClose(g_log); g_log = NULL; }
    return STATUS_SUCCESS;
}

NTSTATUS EvtDeviceAdd(WDFDRIVER drv, PWDFDEVICE_INIT init)
{
    WDF_PNPPOWER_EVENT_CALLBACKS cb; WDFDEVICE dev;
    UNREFERENCED_PARAMETER(drv);
    WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&cb);
    cb.EvtDevicePrepareHardware = EvtPrepareHardware;
    cb.EvtDeviceReleaseHardware = EvtReleaseHardware;
    WdfDeviceInitSetPnpPowerEventCallbacks(init, &cb);
    return WdfDeviceCreate(&init, WDF_NO_OBJECT_ATTRIBUTES, &dev);
}

DRIVER_INITIALIZE DriverEntry;
NTSTATUS DriverEntry(PDRIVER_OBJECT drv, PUNICODE_STRING reg)
{
    WDF_DRIVER_CONFIG cfg;
    WDF_DRIVER_CONFIG_INIT(&cfg, EvtDeviceAdd);
    return WdfDriverCreate(drv, reg, WDF_NO_OBJECT_ATTRIBUTES, &cfg, WDF_NO_HANDLE);
}

