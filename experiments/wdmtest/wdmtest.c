#include <ntddk.h>
static VOID Unload(PDRIVER_OBJECT d){ UNREFERENCED_PARAMETER(d); }
NTSTATUS DriverEntry(PDRIVER_OBJECT d, PUNICODE_STRING r){ UNREFERENCED_PARAMETER(r); d->DriverUnload=Unload; DbgPrint("jslwdm loaded\n"); return STATUS_SUCCESS; }
