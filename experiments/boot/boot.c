/*
 * JSL DSP boot experiment: bring up the Intel cAVS 2.0 DSP on Jasper Lake and
 * load SOF firmware through the HDA code-loader DMA stream.
 * Sequence ported from the Linux SOF driver (hda-loader.c / hda-dsp.c / hda-stream.c).
 * Progress is logged (write-through) to D:\jsl-audio\boot.log.
 * No interrupts are used; everything is polled.
 */
#include <ntddk.h>
#include <wdf.h>
#include <ntstrsafe.h>
#include <initguid.h>
#include <wdmguid.h>

#define FW_PATH   L"\\??\\D:\\jsl-audio\\fw\\sof-jsl.ri"
#define LOG_PATH  L"\\??\\D:\\jsl-audio\\boot.log"
#define EXTMAN_MAGIC 0x6E614D58u

/* HDA BAR0 */
#define HDA_GCAP   0x00
#define HDA_GCTL   0x08
#define HDA_LLCH   0x14
#define HDA_LTRP   0x1048
#define HDA_SD(i)  (0x80 + (i) * 0x20)
#define SD_CTL     0x00
#define SD_STS     0x03
#define SD_CBL     0x08
#define SD_LVI     0x0C
#define SD_FIFOS   0x10
#define SD_FMT     0x12
#define SD_BDLPL   0x18
#define SD_BDLPU   0x1C
#define SD_RUN     0x02
#define SD_INTEN   0x1C
#define CL_FORMAT  0x40
#define PP_PPCTL   0x04
#define PP_PPCTL_GPROCEN (1u << 30)
#define SPB_SPBFCCTL 0x04
#define SPB_SPIB(i)  (0x08 + 0x08 * (i))

/* DSP BAR4 */
#define DSP_ADSPCS 0x04
#define DSP_HIPCTDR 0xC0
#define DSP_HIPCTDA 0xC4
#define DSP_HIPCIDR 0xD0
#define DSP_HIPCIDA 0xD4
#define DSP_ROM_STATUS 0x80000
#define DSP_MBOX_UP   0x81000
#define DSP_SSP_BASE  0x10000
#define DSP_SSP_COUNT 6
#define SSP_CBP_CFP   ((1u << 25) | (1u << 24))
#define IPC_BUSY      (1u << 31)
#define IPC_DONE      (1u << 31)
#define ROM_IPC_CONTROL  0x01000000u
#define ROM_IPC_PURGE_FW 0x00004000u
#define FSR_STATE(x)  ((x) & 0x00FFFFFFu)
#define FSR_INIT_DONE 0x1
#define FSR_FW_ENTERED 0x5

typedef struct { ULONG lo, hi, size, ioc; } BDLE;

static PUCHAR g_hda, g_dsp;
static ULONG  g_hdaLen, g_dspLen;
static ULONG  g_ppcap, g_spbcap;       /* offsets into BAR0, 0 if absent */
static ULONG  g_iss, g_oss;
static HANDLE g_log;

#define R8(b, o)  READ_REGISTER_UCHAR((b) + (o))
#define R16(b, o) READ_REGISTER_USHORT((PUSHORT)((b) + (o)))
#define R32(b, o) READ_REGISTER_ULONG((PULONG)((b) + (o)))
#define W8(b, o, v)  WRITE_REGISTER_UCHAR((b) + (o), (UCHAR)(v))
#define W16(b, o, v) WRITE_REGISTER_USHORT((PUSHORT)((b) + (o)), (USHORT)(v))
#define W32(b, o, v) WRITE_REGISTER_ULONG((PULONG)((b) + (o)), (ULONG)(v))

static VOID Log(const char *fmt, ...)
{
    char buf[300];
    va_list ap;
    IO_STATUS_BLOCK iosb;
    size_t len;
    if (!g_log) return;
    va_start(ap, fmt);
    RtlStringCbVPrintfA(buf, sizeof(buf) - 2, fmt, ap);
    va_end(ap);
    RtlStringCbLengthA(buf, sizeof(buf) - 2, &len);
    buf[len++] = '\r'; buf[len++] = '\n';
    ZwWriteFile(g_log, NULL, NULL, NULL, &iosb, buf, (ULONG)len, NULL, NULL);
}

static VOID LogOpen(VOID)
{
    UNICODE_STRING n; OBJECT_ATTRIBUTES oa; IO_STATUS_BLOCK iosb;
    RtlInitUnicodeString(&n, LOG_PATH);
    InitializeObjectAttributes(&oa, &n, OBJ_KERNEL_HANDLE | OBJ_CASE_INSENSITIVE, NULL, NULL);
    if (!NT_SUCCESS(ZwCreateFile(&g_log, FILE_APPEND_DATA | SYNCHRONIZE, &oa, &iosb, NULL,
        FILE_ATTRIBUTE_NORMAL, FILE_SHARE_READ, FILE_OPEN_IF,
        FILE_SYNCHRONOUS_IO_NONALERT | FILE_WRITE_THROUGH | FILE_NON_DIRECTORY_FILE, NULL, 0)))
        g_log = NULL;
}

static VOID LogClose(VOID) { if (g_log) { ZwClose(g_log); g_log = NULL; } }

static BOOLEAN Poll(PUCHAR b, ULONG off, ULONG mask, ULONG want, ULONG timeout_us)
{
    ULONG t;
    for (t = 0; t <= timeout_us; t += 100) {
        if ((R32(b, off) & mask) == want) return TRUE;
        KeStallExecutionProcessor(100);
    }
    return (R32(b, off) & mask) == want;
}

/* ---------------- DSP core control ---------------- */
static VOID AdspcsRmw(ULONG clr, ULONG set)
{
    W32(g_dsp, DSP_ADSPCS, (R32(g_dsp, DSP_ADSPCS) & ~clr) | set);
}

static BOOLEAN CorePowerUp(ULONG m)
{
    AdspcsRmw(0, m << 16);
    return Poll(g_dsp, DSP_ADSPCS, m << 24, m << 24, 50000);
}

static BOOLEAN CoreResetEnter(ULONG m)
{
    AdspcsRmw(0, m);
    return Poll(g_dsp, DSP_ADSPCS, m, m, 50000);
}

static BOOLEAN CoreResetLeave(ULONG m)
{
    AdspcsRmw(m, 0);
    return Poll(g_dsp, DSP_ADSPCS, m, 0, 50000);
}

static BOOLEAN CoreStallReset(ULONG m)
{
    AdspcsRmw(0, m << 8);
    return CoreResetEnter(m);
}

static BOOLEAN CoreEnabled(ULONG m)
{
    ULONG v = R32(g_dsp, DSP_ADSPCS);
    return ((v & (m << 24)) == (m << 24)) && ((v & (m << 16)) == (m << 16)) &&
           !(v & m) && !(v & (m << 8));
}

static BOOLEAN CoreRun(ULONG m)
{
    if (!CoreResetLeave(m)) return FALSE;
    AdspcsRmw(m << 8, 0);
    if (!CoreEnabled(m)) { CoreStallReset(m); return FALSE; }
    return TRUE;
}

static BOOLEAN CoreResetPowerDown(ULONG m)
{
    BOOLEAN ok = CoreStallReset(m);
    AdspcsRmw(m << 16, 0);
    ok &= Poll(g_dsp, DSP_ADSPCS, m << 24, 0, 50000);
    return ok && !CoreEnabled(m);
}

/* ---------------- HDA stream helpers ---------------- */
static BOOLEAN StreamReset(ULONG idx)
{
    ULONG o = HDA_SD(idx), t;
    W8(g_hda, o + SD_CTL, R8(g_hda, o + SD_CTL) | 1);
    for (t = 0; t < 3000 && !(R8(g_hda, o + SD_CTL) & 1); t++) KeStallExecutionProcessor(10);
    if (!(R8(g_hda, o + SD_CTL) & 1)) return FALSE;
    W8(g_hda, o + SD_CTL, R8(g_hda, o + SD_CTL) & ~1);
    KeStallExecutionProcessor(3);
    for (t = 0; t < 3000 && (R8(g_hda, o + SD_CTL) & 1); t++) KeStallExecutionProcessor(10);
    return !(R8(g_hda, o + SD_CTL) & 1);
}

static BOOLEAN WaitRun(ULONG o, BOOLEAN want)
{
    ULONG t;
    for (t = 0; t < 3000; t++) {
        if (((R8(g_hda, o + SD_CTL) & SD_RUN) != 0) == want) return TRUE;
        KeStallExecutionProcessor(10);
    }
    return FALSE;
}

static VOID StreamStop(ULONG idx)
{
    ULONG o = HDA_SD(idx);
    W8(g_hda, o + SD_CTL, R8(g_hda, o + SD_CTL) & ~(SD_RUN | SD_INTEN));
    WaitRun(o, FALSE);
    W8(g_hda, o + SD_STS, SD_INTEN);
}

static VOID StreamStart(ULONG idx)
{
    ULONG o = HDA_SD(idx);
    W8(g_hda, o + SD_CTL, R8(g_hda, o + SD_CTL) | SD_RUN | SD_INTEN);
}

/* two-entry BDL covering a physically contiguous buffer */
static ULONG BdlSetup(BDLE *bdl, ULONG64 addr, ULONG size)
{
    ULONG half = size / 2;
    bdl[0].lo = (ULONG)addr;            bdl[0].hi = (ULONG)(addr >> 32);
    bdl[0].size = half;                 bdl[0].ioc = 0;
    bdl[1].lo = (ULONG)(addr + half);   bdl[1].hi = (ULONG)((addr + half) >> 32);
    bdl[1].size = size - half;          bdl[1].ioc = 0;
    return 2;
}

/* code-loader (playback) stream: mirrors hda_dsp_stream_hw_params + spib config */
static BOOLEAN ClPrepare(ULONG idx, ULONG tag, ULONG64 bdlAddr, BDLE *bdl, ULONG64 bufAddr, ULONG size)
{
    ULONG o = HDA_SD(idx), frags;
    if (g_ppcap) W32(g_hda, g_ppcap + PP_PPCTL, R32(g_hda, g_ppcap + PP_PPCTL) | (1u << idx));
    W8(g_hda, o + SD_CTL, R8(g_hda, o + SD_CTL) & ~(SD_RUN | SD_INTEN));
    if (!WaitRun(o, FALSE)) return FALSE;
    W8(g_hda, o + SD_STS, SD_INTEN);
    if (!StreamReset(idx)) return FALSE;
    W32(g_hda, o + SD_BDLPL, 0); W32(g_hda, o + SD_BDLPU, 0);
    W8(g_hda, o + SD_CTL, R8(g_hda, o + SD_CTL) & ~(SD_RUN | SD_INTEN));
    if (!WaitRun(o, FALSE)) return FALSE;
    W8(g_hda, o + SD_STS, SD_INTEN);
    frags = BdlSetup(bdl, bufAddr, size);
    W8(g_hda, o + SD_CTL + 2, (R8(g_hda, o + SD_CTL + 2) & 0x0F) | (tag << 4));
    W32(g_hda, o + SD_CBL, size);
    W16(g_hda, o + SD_FMT, CL_FORMAT);
    W16(g_hda, o + SD_LVI, frags - 1);
    W32(g_hda, o + SD_BDLPL, (ULONG)bdlAddr);
    W32(g_hda, o + SD_BDLPU, (ULONG)(bdlAddr >> 32));
    W8(g_hda, o + SD_CTL, R8(g_hda, o + SD_CTL) | SD_INTEN);
    if (g_spbcap) {
        W32(g_hda, g_spbcap + SPB_SPBFCCTL, R32(g_hda, g_spbcap + SPB_SPBFCCTL) | (1u << idx));
        W32(g_hda, g_spbcap + SPB_SPIB(idx), size);
    }
    return TRUE;
}

static VOID ClCleanup(ULONG idx)
{
    ULONG o = HDA_SD(idx);
    if (g_spbcap) {
        W32(g_hda, g_spbcap + SPB_SPIB(idx), 0);
        W32(g_hda, g_spbcap + SPB_SPBFCCTL, R32(g_hda, g_spbcap + SPB_SPBFCCTL) & ~(1u << idx));
    }
    W32(g_hda, o + SD_BDLPL, 0); W32(g_hda, o + SD_BDLPU, 0);
    W32(g_hda, o, 0);
}

/* ICCMAX capture stream (hda_dsp_iccmax_stream_hw_params): programs and starts DMA */
static VOID IccmaxStart(ULONG idx, ULONG64 bdlAddr, BDLE *bdl, ULONG64 bufAddr, ULONG size)
{
    ULONG o = HDA_SD(idx), frags;
    W32(g_hda, o + SD_BDLPL, 0); W32(g_hda, o + SD_BDLPU, 0);
    frags = BdlSetup(bdl, bufAddr, size);
    W32(g_hda, o + SD_BDLPL, (ULONG)bdlAddr);
    W32(g_hda, o + SD_BDLPU, (ULONG)(bdlAddr >> 32));
    W32(g_hda, o + SD_CBL, size);
    W16(g_hda, o + SD_LVI, frags - 1);
    if (g_ppcap) W32(g_hda, g_ppcap + PP_PPCTL, R32(g_hda, g_ppcap + PP_PPCTL) | (1u << idx));
    W8(g_hda, HDA_LTRP, (R8(g_hda, HDA_LTRP) & ~0x3F) | (95 & 0x3F));
    W8(g_hda, o + SD_CTL, R8(g_hda, o + SD_CTL) | SD_RUN);
}

static VOID IccmaxCleanup(ULONG idx)
{
    ULONG o = HDA_SD(idx);
    W8(g_hda, o + SD_CTL, R8(g_hda, o + SD_CTL) & ~SD_RUN);
    W32(g_hda, o + SD_BDLPL, 0); W32(g_hda, o + SD_BDLPU, 0);
    W32(g_hda, o, 0);
}

/* ---------------- firmware file ---------------- */
static NTSTATUS ReadFw(PVOID *data, ULONG *size)
{
    UNICODE_STRING n; OBJECT_ATTRIBUTES oa; IO_STATUS_BLOCK iosb; HANDLE h;
    FILE_STANDARD_INFORMATION fi; NTSTATUS st; PVOID buf;
    RtlInitUnicodeString(&n, FW_PATH);
    InitializeObjectAttributes(&oa, &n, OBJ_KERNEL_HANDLE | OBJ_CASE_INSENSITIVE, NULL, NULL);
    st = ZwCreateFile(&h, GENERIC_READ | SYNCHRONIZE, &oa, &iosb, NULL, FILE_ATTRIBUTE_NORMAL,
                      FILE_SHARE_READ, FILE_OPEN, FILE_SYNCHRONOUS_IO_NONALERT | FILE_NON_DIRECTORY_FILE, NULL, 0);
    if (!NT_SUCCESS(st)) return st;
    st = ZwQueryInformationFile(h, &iosb, &fi, sizeof(fi), FileStandardInformation);
    if (!NT_SUCCESS(st) || fi.EndOfFile.QuadPart > 0x400000) { ZwClose(h); return STATUS_UNSUCCESSFUL; }
    buf = ExAllocatePool2(POOL_FLAG_NON_PAGED, (SIZE_T)fi.EndOfFile.QuadPart, 'wfLJ');
    if (!buf) { ZwClose(h); return STATUS_INSUFFICIENT_RESOURCES; }
    st = ZwReadFile(h, NULL, NULL, NULL, &iosb, buf, (ULONG)fi.EndOfFile.QuadPart, NULL, NULL);
    ZwClose(h);
    if (!NT_SUCCESS(st)) { ExFreePool(buf); return st; }
    *data = buf; *size = (ULONG)fi.EndOfFile.QuadPart;
    return STATUS_SUCCESS;
}

static VOID DumpDwords(const char *tag, PUCHAR base, ULONG off, ULONG count)
{
    ULONG i;
    for (i = 0; i < count; i += 4)
        Log("%s+%05X: %08X %08X %08X %08X", tag, off + i * 4,
            R32(base, off + i * 4), R32(base, off + i * 4 + 4),
            R32(base, off + i * 4 + 8), R32(base, off + i * 4 + 12));
}

/* ---------------- the boot sequence ---------------- */
static NTSTATUS BootDsp(WDFDEVICE dev)
{
    WDFDMAENABLER enabler;
    WDFCOMMONBUFFER clBuf = NULL, clBdl = NULL, icBuf = NULL, icBdl = NULL;
    WDF_DMA_ENABLER_CONFIG dc;
    PVOID fw = NULL; ULONG fwSize = 0, payloadOff, payloadSize;
    NTSTATUS st;
    ULONG clIdx, icIdx = 0, tag = 1, v, t;
    BOOLEAN ok;
    ULONG64 clBufA, clBdlA, icBufA, icBdlA;

    st = ReadFw(&fw, &fwSize);
    Log("fw read: status=%08X size=%u", st, fwSize);
    if (!NT_SUCCESS(st)) return st;
    if (*(PULONG)fw != EXTMAN_MAGIC) { Log("bad ext manifest magic %08X", *(PULONG)fw); ExFreePool(fw); return STATUS_INVALID_IMAGE_FORMAT; }
    payloadOff = ((PULONG)fw)[1];
    payloadSize = fwSize - payloadOff;
    Log("ext_man full_size=%u payload=%u (aligned4k=%d)", payloadOff, payloadSize, (payloadSize & 0xFFF) == 0);

    WDF_DMA_ENABLER_CONFIG_INIT(&dc, WdfDmaProfileScatterGather64, 0x200000);
    st = WdfDmaEnablerCreate(dev, &dc, WDF_NO_OBJECT_ATTRIBUTES, &enabler);
    Log("dma enabler: %08X", st);
    if (!NT_SUCCESS(st)) goto out;

    st = WdfCommonBufferCreate(enabler, payloadSize, WDF_NO_OBJECT_ATTRIBUTES, &clBuf);
    if (NT_SUCCESS(st)) st = WdfCommonBufferCreate(enabler, 4096, WDF_NO_OBJECT_ATTRIBUTES, &clBdl);
    if (NT_SUCCESS(st)) st = WdfCommonBufferCreate(enabler, 4096, WDF_NO_OBJECT_ATTRIBUTES, &icBuf);
    if (NT_SUCCESS(st)) st = WdfCommonBufferCreate(enabler, 4096, WDF_NO_OBJECT_ATTRIBUTES, &icBdl);
    Log("common buffers: %08X", st);
    if (!NT_SUCCESS(st)) goto out;

    clBufA = (ULONG64)WdfCommonBufferGetAlignedLogicalAddress(clBuf).QuadPart;
    clBdlA = (ULONG64)WdfCommonBufferGetAlignedLogicalAddress(clBdl).QuadPart;
    icBufA = (ULONG64)WdfCommonBufferGetAlignedLogicalAddress(icBuf).QuadPart;
    icBdlA = (ULONG64)WdfCommonBufferGetAlignedLogicalAddress(icBdl).QuadPart;
    Log("dma addrs: cl=%llX clbdl=%llX ic=%llX icbdl=%llX", clBufA, clBdlA, icBufA, icBdlA);
    RtlCopyMemory(WdfCommonBufferGetAlignedVirtualAddress(clBuf), (PUCHAR)fw + payloadOff, payloadSize);
    RtlZeroMemory(WdfCommonBufferGetAlignedVirtualAddress(icBuf), 4096);

    clIdx = g_iss;                       /* first output stream */
    Log("streams: iss=%u oss=%u clIdx=%u ppcap=%X spbcap=%X", g_iss, g_oss, clIdx, g_ppcap, g_spbcap);
    if (g_ppcap) W32(g_hda, g_ppcap + PP_PPCTL, R32(g_hda, g_ppcap + PP_PPCTL) | PP_PPCTL_GPROCEN);

    /* --- ICCMAX capture stream first (hda_dsp_cl_boot_firmware_iccmax) --- */
    IccmaxStart(icIdx, icBdlA, (BDLE *)WdfCommonBufferGetAlignedVirtualAddress(icBdl), icBufA, 4096);
    Log("iccmax stream started, SD ctl=%08X", R32(g_hda, HDA_SD(icIdx)));

    /* --- code loader stream prepare --- */
    ok = ClPrepare(clIdx, tag, clBdlA, (BDLE *)WdfCommonBufferGetAlignedVirtualAddress(clBdl), clBufA, payloadSize);
    Log("cl prepare: %d  SD ctl=%08X cbl=%u lvi=%u", ok, R32(g_hda, HDA_SD(clIdx)), R32(g_hda, HDA_SD(clIdx) + SD_CBL), R16(g_hda, HDA_SD(clIdx) + SD_LVI));
    if (!ok) { st = STATUS_UNSUCCESSFUL; goto cleanup; }

    /* --- cl_dsp_init --- */
    Log("ADSPCS before: %08X rom=%08X", R32(g_dsp, DSP_ADSPCS), R32(g_dsp, DSP_ROM_STATUS));
    ok = CorePowerUp(3);
    Log("power up cores 0,1: %d ADSPCS=%08X", ok, R32(g_dsp, DSP_ADSPCS));
    if (!ok) { st = STATUS_UNSUCCESSFUL; goto cleanup; }

    for (t = 0; t < DSP_SSP_COUNT; t++) {
        ULONG off = DSP_SSP_BASE + t * 0x1000 + 4;
        if (off + 4 <= g_dspLen) W32(g_dsp, off, R32(g_dsp, off) | SSP_CBP_CFP);
    }

    v = IPC_BUSY | ROM_IPC_CONTROL | ROM_IPC_PURGE_FW | ((tag - 1) << 9);
    W32(g_dsp, DSP_HIPCIDR, v);
    Log("ROM_CONTROL sent: %08X", v);

    ok = CoreRun(1);
    Log("core0 run: %d ADSPCS=%08X rom=%08X", ok, R32(g_dsp, DSP_ADSPCS), R32(g_dsp, DSP_ROM_STATUS));
    if (!ok) { st = STATUS_UNSUCCESSFUL; goto cleanup; }

    ok = Poll(g_dsp, DSP_HIPCIDA, IPC_DONE, IPC_DONE, 500000);
    Log("wait HIPCIDA done: %d HIPCIDA=%08X rom=%08X", ok, R32(g_dsp, DSP_HIPCIDA), R32(g_dsp, DSP_ROM_STATUS));
    if (!ok) { st = STATUS_IO_TIMEOUT; goto cleanup; }
    W32(g_dsp, DSP_HIPCIDA, IPC_DONE);

    ok = CoreResetPowerDown(2);
    Log("power down core1: %d ADSPCS=%08X", ok, R32(g_dsp, DSP_ADSPCS));

    /* wait ROM_INIT_DONE */
    for (t = 0; t < 300000; t += 500) {
        if (FSR_STATE(R32(g_dsp, DSP_ROM_STATUS)) == FSR_INIT_DONE) break;
        KeStallExecutionProcessor(500);
    }
    v = R32(g_dsp, DSP_ROM_STATUS);
    Log("rom status after init wait: %08X (state=%X)", v, FSR_STATE(v));
    if (FSR_STATE(v) != FSR_INIT_DONE) { st = STATUS_IO_TIMEOUT; goto cleanup; }

    /* --- cl copy fw --- */
    StreamStart(clIdx);
    Log("cl DMA started, SD ctl=%08X", R32(g_hda, HDA_SD(clIdx)));
    for (t = 0; t < 3000000; t += 500) {
        if (FSR_STATE(R32(g_dsp, DSP_ROM_STATUS)) == FSR_FW_ENTERED) break;
        KeStallExecutionProcessor(500);
    }
    v = R32(g_dsp, DSP_ROM_STATUS);
    Log("rom status after fw load: %08X (state=%X) err=%08X LPIB=%u", v, FSR_STATE(v),
        R32(g_dsp, DSP_ROM_STATUS + 4), R32(g_hda, HDA_SD(clIdx) + 4));
    StreamStop(clIdx);
    if (FSR_STATE(v) != FSR_FW_ENTERED) { st = STATUS_IO_TIMEOUT; goto cleanup; }
    st = STATUS_SUCCESS;
    Log("*** FIRMWARE ENTERED ***");

    /* wait for the FW_READY IPC from the DSP */
    for (t = 0; t < 5000000; t += 1000) {
        if (R32(g_dsp, DSP_HIPCTDR) & IPC_BUSY) break;
        KeStallExecutionProcessor(1000);
    }
    Log("HIPCTDR=%08X HIPCTDA=%08X ADSPCS=%08X", R32(g_dsp, DSP_HIPCTDR), R32(g_dsp, DSP_HIPCTDA), R32(g_dsp, DSP_ADSPCS));
    DumpDwords("win0", g_dsp, DSP_ROM_STATUS, 16);
    DumpDwords("mbox", g_dsp, DSP_MBOX_UP, 48);

cleanup:
    ClCleanup(clIdx);
    IccmaxCleanup(icIdx);
    W8(g_hda, HDA_LTRP, R8(g_hda, HDA_LTRP) & ~0x3F);
    if (!NT_SUCCESS(st)) {
        Log("FAILED st=%08X; ADSPCS=%08X rom=%08X err=%08X", st, R32(g_dsp, DSP_ADSPCS), R32(g_dsp, DSP_ROM_STATUS), R32(g_dsp, DSP_ROM_STATUS + 4));
        CoreResetPowerDown(3);
        Log("powered down: ADSPCS=%08X", R32(g_dsp, DSP_ADSPCS));
    }
out:
    if (fw) ExFreePool(fw);
    return st;
}

/* ---------------- KMDF plumbing ---------------- */
EVT_WDF_DRIVER_DEVICE_ADD EvtDeviceAdd;
EVT_WDF_DEVICE_PREPARE_HARDWARE EvtPrepareHardware;
EVT_WDF_DEVICE_RELEASE_HARDWARE EvtReleaseHardware;

static VOID WalkCaps(VOID)
{
    ULONG off = R32(g_hda, HDA_LLCH) & 0xFFFF, i;
    g_ppcap = g_spbcap = 0;
    for (i = 0; i < 10 && off && off + 4 <= g_hdaLen; i++) {
        ULONG cap = R32(g_hda, off), id = (cap >> 16) & 0xFFF;
        Log("cap @%X = %08X (id %X)", off, cap, id);
        if (id == 3) g_ppcap = off;
        else if (id == 4) g_spbcap = off;
        off = cap & 0xFFFF;
    }
}

NTSTATUS EvtPrepareHardware(WDFDEVICE dev, WDFCMRESLIST raw, WDFCMRESLIST xlat)
{
    ULONG n = WdfCmResourceListGetCount(xlat), i, mem = 0;
    ULONG gcap;
    UNREFERENCED_PARAMETER(raw);

    LogOpen();
    Log("=== jslboot start ===");
    for (i = 0; i < n; i++) {
        PCM_PARTIAL_RESOURCE_DESCRIPTOR d = WdfCmResourceListGetDescriptor(xlat, i);
        if (d->Type != CmResourceTypeMemory) continue;
        if (mem == 0) { g_hdaLen = d->u.Memory.Length; g_hda = MmMapIoSpaceEx(d->u.Memory.Start, g_hdaLen, PAGE_READWRITE | PAGE_NOCACHE); }
        else if (mem == 1) { g_dspLen = d->u.Memory.Length; g_dsp = MmMapIoSpaceEx(d->u.Memory.Start, g_dspLen, PAGE_READWRITE | PAGE_NOCACHE); }
        mem++;
    }
    if (!g_hda || !g_dsp) { Log("map failed"); LogClose(); return STATUS_SUCCESS; }

    /* make sure bus mastering is on */
    {
        BUS_INTERFACE_STANDARD bus;
        if (NT_SUCCESS(WdfFdoQueryForInterface(dev, &GUID_BUS_INTERFACE_STANDARD, (PINTERFACE)&bus, sizeof(bus), 1, NULL))) {
            USHORT cmd = 0;
            bus.GetBusData(bus.Context, PCI_WHICHSPACE_CONFIG, &cmd, 4, 2);
            Log("PCI command=%04X", cmd);
            if ((cmd & 6) != 6) {
                cmd |= 6;
                bus.SetBusData(bus.Context, PCI_WHICHSPACE_CONFIG, &cmd, 4, 2);
                Log("set command -> %04X", cmd);
            }
            bus.InterfaceDereference(bus.Context);
        } else Log("no bus interface");
    }

    gcap = R16(g_hda, HDA_GCAP);
    g_oss = (gcap >> 12) & 0xF;
    g_iss = (gcap >> 8) & 0xF;
    Log("GCAP=%04X GCTL=%08X ADSPCS=%08X", gcap, R32(g_hda, HDA_GCTL), R32(g_dsp, DSP_ADSPCS));
    WalkCaps();

    {
        NTSTATUS st = BootDsp(dev);
        Log("BootDsp -> %08X", st);
    }
    return STATUS_SUCCESS;     /* never fail start: keep the device alive for inspection */
}

NTSTATUS EvtReleaseHardware(WDFDEVICE dev, WDFCMRESLIST xlat)
{
    UNREFERENCED_PARAMETER(dev); UNREFERENCED_PARAMETER(xlat);
    if (g_dsp) {
        Log("release: ADSPCS=%08X", R32(g_dsp, DSP_ADSPCS));
        CoreResetPowerDown(3);
        Log("release: powered down ADSPCS=%08X", R32(g_dsp, DSP_ADSPCS));
        MmUnmapIoSpace(g_dsp, g_dspLen); g_dsp = NULL;
    }
    if (g_hda) { MmUnmapIoSpace(g_hda, g_hdaLen); g_hda = NULL; }
    LogClose();
    return STATUS_SUCCESS;
}

NTSTATUS EvtDeviceAdd(WDFDRIVER drv, PWDFDEVICE_INIT init)
{
    WDF_PNPPOWER_EVENT_CALLBACKS cb;
    WDFDEVICE dev;
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
