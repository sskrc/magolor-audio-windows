#include <ntddk.h>
#include <wdf.h>
#include <ntstrsafe.h>

#define BAR_MAX 6

typedef struct _DEVCTX { ULONG dummy; } DEVCTX, *PDEVCTX;
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(DEVCTX, GetDevCtx)

static VOID PutU64(WDFKEY key, PCWSTR name, ULONG64 v)
{
    UNICODE_STRING n;
    RtlInitUnicodeString(&n, name);
    WdfRegistryAssignValue(key, &n, REG_QWORD, sizeof(v), &v);
}

EVT_WDF_DRIVER_DEVICE_ADD EvtDeviceAdd;
EVT_WDF_DEVICE_PREPARE_HARDWARE EvtPrepareHardware;

NTSTATUS EvtPrepareHardware(WDFDEVICE dev, WDFCMRESLIST raw, WDFCMRESLIST xlat)
{
    UNREFERENCED_PARAMETER(raw);
    WDFKEY key;
    NTSTATUS st = WdfDeviceOpenRegistryKey(dev, PLUGPLAY_REGKEY_DEVICE, KEY_SET_VALUE, WDF_NO_OBJECT_ATTRIBUTES, &key);
    if (!NT_SUCCESS(st)) return STATUS_SUCCESS;

    ULONG n = WdfCmResourceListGetCount(xlat), memIdx = 0;
    PutU64(key, L"ResCount", n);
    for (ULONG i = 0; i < n; i++) {
        PCM_PARTIAL_RESOURCE_DESCRIPTOR d = WdfCmResourceListGetDescriptor(xlat, i);
        WCHAR nm[40];
        if (d->Type == CmResourceTypeMemory) {
            ULONG len = d->u.Memory.Length;
            RtlStringCchPrintfW(nm, 40, L"Mem%u_Phys", memIdx); PutU64(key, nm, (ULONG64)d->u.Memory.Start.QuadPart);
            RtlStringCchPrintfW(nm, 40, L"Mem%u_Len", memIdx);  PutU64(key, nm, len);
            volatile ULONG *p = (volatile ULONG *)MmMapIoSpaceEx(d->u.Memory.Start, len, PAGE_READWRITE | PAGE_NOCACHE);
            if (p) {
                /* dump first 0x20 dwords of every BAR (read only) */
                for (ULONG k = 0; k < 0x20 && (k * 4) < len; k++) {
                    RtlStringCchPrintfW(nm, 40, L"Mem%u_R%02X", memIdx, k * 4);
                    PutU64(key, nm, p[k]);
                }
                if (len >= 0x1000) {
                    /* HDA: PPCAP area / DSP regs */
                    for (ULONG k = 0; k < 0x10; k++) {
                        RtlStringCchPrintfW(nm, 40, L"Mem%u_R%03X", memIdx, 0x800 + k * 4);
                        if (0x800 + k * 4 < len) PutU64(key, nm, p[(0x800 / 4) + k]);
                    }
                }
                MmUnmapIoSpace((PVOID)p, len);
            }
            memIdx++;
        } else if (d->Type == CmResourceTypeInterrupt) {
            RtlStringCchPrintfW(nm, 40, L"Int%u_Vec", i); PutU64(key, nm, d->u.Interrupt.Vector);
        }
    }
    WdfRegistryClose(key);
    return STATUS_SUCCESS;
}

NTSTATUS EvtDeviceAdd(WDFDRIVER drv, PWDFDEVICE_INIT init)
{
    UNREFERENCED_PARAMETER(drv);
    WDF_PNPPOWER_EVENT_CALLBACKS cb;
    WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&cb);
    cb.EvtDevicePrepareHardware = EvtPrepareHardware;
    WdfDeviceInitSetPnpPowerEventCallbacks(init, &cb);
    WDF_OBJECT_ATTRIBUTES a;
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&a, DEVCTX);
    WDFDEVICE dev;
    return WdfDeviceCreate(&init, &a, &dev);
}

DRIVER_INITIALIZE DriverEntry;
NTSTATUS DriverEntry(PDRIVER_OBJECT drv, PUNICODE_STRING reg)
{
    WDF_DRIVER_CONFIG cfg;
    WDF_DRIVER_CONFIG_INIT(&cfg, EvtDeviceAdd);
    return WdfDriverCreate(drv, reg, WDF_NO_OBJECT_ATTRIBUTES, &cfg, WDF_NO_HANDLE);
}


