#define ZERO_DATA 0
#define PLAY_SOUND 1
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

/* ---------------- IPC3 ---------------- */
#define HOSTBOX 0x82000     /* window 1 (inbox / host-initiated mailbox) */

#define GLB_TPLG   0x30000000u
#define GLB_STREAM 0x60000000u
#define GLB_DAI    0x80000000u
#define TPLG_COMP_NEW      0x00010000u
#define TPLG_COMP_CONNECT  0x00030000u
#define TPLG_PIPE_NEW      0x00100000u
#define TPLG_PIPE_COMPLETE 0x00130000u
#define TPLG_BUFFER_NEW    0x00200000u
#define DAI_CONFIG         0x00010000u
#define STREAM_PCM_PARAMS  0x00010000u
#define STREAM_TRIG_START  0x00040000u
#define STREAM_TRIG_STOP   0x00050000u
#define STREAM_PCM_FREE    0x00030000u

#define COMP_HOST 1
#define COMP_DAI  2
#define COMP_BUFFER 12
#define FRAME_S24_4LE 1
#define FRAME_S32_LE  2
#define DAI_SSP 1
#ifndef DAI_IDX
#define DAI_IDX 1
#endif

#pragma pack(push, 1)
typedef struct { ULONG size, cmd; } CMDHDR;
typedef struct { CMDHDR hdr; ULONG id, type, pipeline_id, core, ext_data_length; } COMPHDR;
typedef struct { CMDHDR hdr; ULONG periods_sink, periods_source, reserved1, frame_fmt, xrun_action, reserved[2]; } COMPCFG;
typedef struct { COMPHDR comp; COMPCFG config; ULONG direction, no_irq, dmac_config; } IPC_HOST;
typedef struct { COMPHDR comp; COMPCFG config; ULONG direction, dai_index, type, reserved; } IPC_DAI;
typedef struct { COMPHDR comp; ULONG size, caps, flags, reserved; } IPC_BUFFER;
typedef struct { CMDHDR hdr; ULONG comp_id, pipeline_id, sched_id, core, period, priority, period_mips, frames_per_sched, xrun_limit_usecs, time_domain; } IPC_PIPE_NEW;
typedef struct { CMDHDR hdr; ULONG comp_id; } IPC_PIPE_READY;
typedef struct { CMDHDR hdr; ULONG source_id, sink_id; } IPC_CONNECT;
typedef struct {
    CMDHDR hdr; ULONG type, dai_index; USHORT format; UCHAR group_id, flags; ULONG reserved[8];
    /* ssp params (60 bytes) at offset 52 */
    ULONG s_reserved0; USHORT s_reserved1, mclk_id; ULONG mclk_rate, fsync_rate, bclk_rate;
    ULONG tdm_slots, rx_slots, tx_slots, sample_valid_bits; USHORT tdm_slot_width, s_reserved2;
    ULONG mclk_direction; USHORT frame_pulse_width, tdm_per_slot_padding_flag;
    ULONG clks_control, quirks, bclk_delay;
    UCHAR pad[216 - 52 - 60];
} IPC_DAI_CONFIG;
typedef struct {
    CMDHDR hdr; ULONG comp_id, flags, reserved[2];
    ULONG p_size;                                   /* params.hdr.size */
    ULONG b_size, phy_addr, pages, b_bytes, b_res[3]; /* params.buffer */
    ULONG direction, frame_fmt, buffer_fmt, rate;
    USHORT stream_tag, channels, sample_valid_bytes, sample_container_bytes;
    ULONG host_period_bytes;
    USHORT no_stream_position; UCHAR cont_update_posn, reserved0; USHORT ext_data_length;
    UCHAR reserved2[2]; USHORT chmap[8];
} IPC_PCM_PARAMS;
typedef struct { CMDHDR hdr; ULONG comp_id; } IPC_STREAM;
typedef struct { CMDHDR hdr; ULONG b_size, b_phy, b_pages, b_bytes, b_res[3]; ULONG stream_tag; ULONG64 timestamp_ns; ULONG reserved[8]; } IPC_TRACE_PARAMS;
#pragma pack(pop)

static ULONG g_ipcCount, g_reply[8], g_posnOffset;
static PVOID g_trV; static BOOLEAN g_traceOn;

static VOID IpcAckDspMessages(VOID)
{
    if (R32(g_dsp, DSP_HIPCTDR) & IPC_BUSY) {
        Log("  dsp-initiated msg TDR=%08X", R32(g_dsp, DSP_HIPCTDR));
        W32(g_dsp, DSP_HIPCTDR, IPC_BUSY);
        W32(g_dsp, DSP_HIPCTDA, IPC_DONE);
    }
}

static NTSTATUS IpcTx(const char *what, PVOID msg, ULONG size)
{
    PULONG p = (PULONG)msg;
    ULONG i, err, rsize, rcmd;
    IpcAckDspMessages();
    for (i = 0; i < (size + 3) / 4; i++) W32(g_dsp, HOSTBOX + i * 4, p[i]);
    W32(g_dsp, DSP_HIPCIDR, IPC_BUSY);
    if (!Poll(g_dsp, DSP_HIPCIDA, IPC_DONE, IPC_DONE, 2000000)) {
        Log("IPC %s: TIMEOUT (cmd=%08X) HIPCIDR=%08X ROM=%08X", what, p[1], R32(g_dsp, DSP_HIPCIDR), R32(g_dsp, DSP_ROM_STATUS));
        return STATUS_IO_TIMEOUT;
    }
    rsize = R32(g_dsp, HOSTBOX); rcmd = R32(g_dsp, HOSTBOX + 4); err = R32(g_dsp, HOSTBOX + 8); for (i = 0; i < 8; i++) g_reply[i] = R32(g_dsp, HOSTBOX + i * 4);
    W32(g_dsp, DSP_HIPCIDA, IPC_DONE);
    g_ipcCount++;
    Log("IPC %s: cmd=%08X size=%u -> reply size=%u cmd=%08X err=%d   [ssp1 %08X %08X, ssp0 %08X, ADSPCS %08X]", what, p[1], size, rsize, rcmd, (LONG)err, R32(g_dsp, 0x11000), R32(g_dsp, 0x11004), R32(g_dsp, 0x10004), R32(g_dsp, DSP_ADSPCS));
    return err ? STATUS_UNSUCCESSFUL : STATUS_SUCCESS;
}

static VOID CompCfg(COMPCFG *c, ULONG sink, ULONG src, ULONG fmt)
{
    RtlZeroMemory(c, sizeof(*c));
    c->hdr.size = sizeof(*c);
    c->periods_sink = sink; c->periods_source = src; c->frame_fmt = fmt;
}

static VOID FillTone(PLONG buf, ULONG frames)
{
    LONGLONG c = 535959061LL;           /* 2*cos(w) in Q28, w = 2*pi*76/8192 */
    LONGLONG y0 = 0, y1 = 7819292LL, t;  /* amplitude 2^27 (-24 dBFS) */
    ULONG i;
    for (i = 0; i < frames; i++) {
        buf[2 * i] = buf[2 * i + 1] = ZERO_DATA ? 0 : (LONG)y0;
        t = ((c * y1) >> 28) - y0;
        y0 = y1; y1 = t;
    }
}

static VOID DumpFile(PCWSTR path, PVOID data, ULONG len)
{
    UNICODE_STRING n; OBJECT_ATTRIBUTES oa; IO_STATUS_BLOCK iosb; HANDLE h;
    RtlInitUnicodeString(&n, path);
    InitializeObjectAttributes(&oa, &n, OBJ_KERNEL_HANDLE | OBJ_CASE_INSENSITIVE, NULL, NULL);
    if (NT_SUCCESS(ZwCreateFile(&h, FILE_WRITE_DATA | SYNCHRONIZE, &oa, &iosb, NULL, FILE_ATTRIBUTE_NORMAL, FILE_SHARE_READ,
        FILE_OVERWRITE_IF, FILE_SYNCHRONOUS_IO_NONALERT | FILE_WRITE_THROUGH | FILE_NON_DIRECTORY_FILE, NULL, 0))) {
        ZwWriteFile(h, NULL, NULL, NULL, &iosb, data, len, NULL, NULL);
        ZwClose(h);
    }
}

#define PCM_FRAMES 8192
#define PCM_BYTES  (PCM_FRAMES * 8)

static VOID PadDump(ULONG64 comm, const char *pn, ULONG pad)
{
    PHYSICAL_ADDRESS pa; PUCHAR b; ULONG padbar, off;
    pa.QuadPart = (LONGLONG)comm;
    b = (PUCHAR)MmMapIoSpaceEx(pa, 0x10000, PAGE_READONLY | PAGE_NOCACHE);
    if (!b) { Log("pad map failed %llX", comm); return; }
    padbar = R32(b, 0x0C) & 0xFFFF;
    off = padbar + pad * 16;
    Log("PAD %s (comm %llX pad %u): PADBAR=%X DW0=%08X DW1=%08X DW2=%08X  PMODE=%u RXDIS=%u TXDIS=%u TXSTATE=%u RXSTATE=%u",
        pn, comm, pad, padbar, R32(b, off), R32(b, off + 4), R32(b, off + 8),
        (R32(b, off) >> 10) & 7, (R32(b, off) >> 9) & 1, (R32(b, off) >> 8) & 1, R32(b, off) & 1, (R32(b, off) >> 1) & 1);
    MmUnmapIoSpace(b, 0x10000);
}

static VOID PadScan(ULONG64 comm, ULONG npads)
{
    PHYSICAL_ADDRESS pa; PUCHAR b; ULONG padbar, p;
    pa.QuadPart = (LONGLONG)comm;
    b = (PUCHAR)MmMapIoSpaceEx(pa, 0x10000, PAGE_READONLY | PAGE_NOCACHE);
    if (!b) return;
    padbar = R32(b, 0x0C) & 0xFFFF;
    for (p = 0; p < npads; p++) {
        ULONG d0 = R32(b, padbar + p * 16);
        if (((d0 >> 10) & 7) == 0 && !((d0 >> 8) & 1) && (d0 & 1))
            Log("PADSCAN comm %llX pad %u: output HIGH DW0=%08X DW1=%08X", comm, p, d0, R32(b, padbar + p * 16 + 4));
    }
    MmUnmapIoSpace(b, 0x10000);
}

static VOID PadWrite(ULONG64 comm, ULONG pad, ULONG val)
{
    PHYSICAL_ADDRESS pa; PUCHAR b; ULONG padbar, off, before, after;
    pa.QuadPart = (LONGLONG)comm;
    b = (PUCHAR)MmMapIoSpaceEx(pa, 0x10000, PAGE_READWRITE | PAGE_NOCACHE);
    if (!b) { Log("padwrite map failed"); return; }
    padbar = R32(b, 0x0C) & 0xFFFF; off = padbar + pad * 16;
    before = R32(b, off);
    W32(b, off, val);
    after = R32(b, off);
    Log("PADWRITE comm %llX pad %u: %08X -> wrote %08X -> read %08X", comm, pad, before, val, after);
    MmUnmapIoSpace(b, 0x10000);
}

/* GPIO-mode output pad: set/clear TXSTATE only (refuses if the pad is not a GPIO output) */
static VOID PadSetTx(ULONG64 comm, ULONG pad, ULONG level, const char *name)
{
    PHYSICAL_ADDRESS pa; PUCHAR b; ULONG off, before, after;
    pa.QuadPart = (LONGLONG)comm;
    b = (PUCHAR)MmMapIoSpaceEx(pa, 0x10000, PAGE_READWRITE | PAGE_NOCACHE);
    if (!b) { Log("PadSetTx map failed"); return; }
    off = (R32(b, 0x0C) & 0xFFFF) + pad * 16;
    before = R32(b, off);
    if (((before >> 10) & 7) == 0 && !((before >> 8) & 1))
        W32(b, off, (before & ~1u) | (level & 1));
    after = R32(b, off);
    Log("PADTX %s (comm %llX pad %u): %08X -> %08X%s", name, comm, pad, before, after,
        (((before >> 10) & 7) || ((before >> 8) & 1)) ? "  (NOT a GPIO output, untouched)" : "");
    MmUnmapIoSpace(b, 0x10000);
}

static VOID PadSample(ULONG64 comm, ULONG pad, const char *name)
{
    PHYSICAL_ADDRESS pa; PUCHAR b; ULONG padbar, off, i, ones = 0, trans = 0, last = 2, v;
    pa.QuadPart = (LONGLONG)comm;
    b = (PUCHAR)MmMapIoSpaceEx(pa, 0x10000, PAGE_READONLY | PAGE_NOCACHE);
    if (!b) return;
    padbar = R32(b, 0x0C) & 0xFFFF; off = padbar + pad * 16;
    for (i = 0; i < 20000; i++) { v = (R32(b, off) >> 1) & 1; ones += v; if (last != 2 && v != last) trans++; last = v; }
    Log("SAMPLE %s: ones=%u/20000 transitions=%u", name, ones, trans);
    MmUnmapIoSpace(b, 0x10000);
}

/* sample all candidate audio pins interleaved (comm0 pads 84..91, comm1 I2S1_SCLK/I2S2/I2S_MCLK), log afterwards */
static VOID MultiSample(const char *tag)
{
    static const struct { UCHAR c; UCHAR pad; const char *name; } P[] = {
        {0, 84, "HDA_BCLK(R0)"}, {0, 85, "HDA_SYNC(R1)"}, {0, 86, "HDA_SDO(R2)"}, {0, 87, "HDA_SDI0(R3)"},
        {0, 88, "HDA_RSTB(R4)"}, {0, 89, "HDA_SDI1(R5)"}, {0, 90, "I2S1_SFRM(R6)"}, {0, 91, "I2S1_TXD(R7)"},
        {1, 11, "I2S2_SCLK"}, {1, 12, "I2S2_SFRM"}, {1, 13, "I2S2_TXD"}, {1, 15, "I2S1_SCLK(H15)"}, {1, 42, "I2S_MCLK"},
    };
    enum { NP = sizeof(P) / sizeof(P[0]) };
    PHYSICAL_ADDRESS pa; PUCHAR b[2]; ULONG pb[2], ones[NP] = {0}, tr[NP] = {0}, last[NP], i, k, v;
    pa.QuadPart = 0xFD6E0000LL; b[0] = (PUCHAR)MmMapIoSpaceEx(pa, 0x10000, PAGE_READONLY | PAGE_NOCACHE);
    pa.QuadPart = 0xFD6D0000LL; b[1] = (PUCHAR)MmMapIoSpaceEx(pa, 0x10000, PAGE_READONLY | PAGE_NOCACHE);
    if (!b[0] || !b[1]) { Log("MultiSample map failed"); goto out; }
    pb[0] = R32(b[0], 0x0C) & 0xFFFF; pb[1] = R32(b[1], 0x0C) & 0xFFFF;
    for (k = 0; k < NP; k++) last[k] = 2;
    for (i = 0; i < 4000; i++)
        for (k = 0; k < NP; k++) {
            v = (R32(b[P[k].c], pb[P[k].c] + P[k].pad * 16) >> 1) & 1;
            ones[k] += v; if (last[k] != 2 && v != last[k]) tr[k]++; last[k] = v;
        }
    for (k = 0; k < NP; k++) {
        ULONG d0 = R32(b[P[k].c], pb[P[k].c] + P[k].pad * 16);
        Log("MS[%s] %-15s DW0=%08X PMODE=%u RXDIS=%u TXDIS=%u ones=%u/4000 trans=%u", tag, P[k].name, d0,
            (d0 >> 10) & 7, (d0 >> 9) & 1, (d0 >> 8) & 1, ones[k], tr[k]);
    }
out:
    if (b[0]) MmUnmapIoSpace(b[0], 0x10000);
    if (b[1]) MmUnmapIoSpace(b[1], 0x10000);
}

static VOID Ssp1Snap(const char *tag)
{
    Log("SSP1 [%s]: SSCR0=%08X SSCR1=%08X SSSR=%08X SSITR=%08X SSPSP=%08X SSTSA=%08X SSRSA=%08X SSCR2=%08X SSCR3=%08X SSIOC=%08X", tag,
        R32(g_dsp, 0x11000), R32(g_dsp, 0x11004), R32(g_dsp, 0x11008), R32(g_dsp, 0x1100C), R32(g_dsp, 0x1102C), R32(g_dsp, 0x11030), R32(g_dsp, 0x11034),
        R32(g_dsp, 0x11040), R32(g_dsp, 0x11048), R32(g_dsp, 0x1104C));
}

static NTSTATUS PlayTone(WDFDMAENABLER enabler)
{
    WDFCOMMONBUFFER pcmBuf = NULL, pcmBdl = NULL;
    NTSTATUS st;
    union { IPC_HOST h; IPC_DAI d; IPC_BUFFER b; IPC_PIPE_NEW pn; IPC_PIPE_READY pr; IPC_CONNECT c; IPC_DAI_CONFIG dc; IPC_PCM_PARAMS pp; IPC_STREAM s; IPC_TRACE_PARAMS tp; } m;
    ULONG idx = g_iss + 1, tag = 2, o = HDA_SD(g_iss + 1), i;
    ULONG64 bufA, bdlA; BDLE *bdl; PVOID bufV;

    /* amp SDB: ACPI GpioIo 209 is in the gpio_base space -> GPP_D base 192 + 17 = GPP_D17 = pin 133 = comm1 pad 41
       (earlier builds wrongly drove pin 209 = ISH_GP_4) */
    PadSetTx(0xFD6D0000ULL, 41, 1, "SDB GPP_D17");
    PadScan(0xFD6E0000ULL, 100); PadScan(0xFD6D0000ULL, 110); PadScan(0xFD6A0000ULL, 40); PadScan(0xFD690000ULL, 12);
    PadDump(0xFD6E0000ULL, "HDA_BCLK/I2S0_SCLK(84)", 84); PadDump(0xFD6E0000ULL, "HDA_SYNC(85)", 85); PadDump(0xFD6E0000ULL, "HDA_SDO(86)", 86); PadDump(0xFD6E0000ULL, "HDA_SDI_0(87)", 87); PadDump(0xFD6E0000ULL, "HDA_SDI_1(89)", 89);
    PadDump(0xFD6D0000ULL, "I2S2_SCLK(103)", 11); PadDump(0xFD6D0000ULL, "I2S2_SFRM(104)", 12); PadDump(0xFD6D0000ULL, "I2S2_TXD(105)", 13); PadDump(0xFD6D0000ULL, "I2S2_RXD(106)", 14);
    PadDump(0xFD6E0000ULL, "I2S1_SFRM(90)", 90);
    PadDump(0xFD6E0000ULL, "I2S1_TXD(91)", 91);
    PadDump(0xFD6D0000ULL, "I2S1_SCLK(107)", 15);
    PadDump(0xFD6D0000ULL, "I2S_MCLK(134)", 42);
    PadDump(0xFD6A0000ULL, "ISH_GP_4(209)=SDB", 14);
    PadDump(0xFD690000ULL, "comm5 pad 14", 14);
    PadDump(0xFD6D0000ULL, "comm1 pad 117 (209-92)", 117);
    PadDump(0xFD6A0000ULL, "comm4 pad 0..3 sample", 0);
    PadDump(0xFD6A0000ULL, "comm4 pad 20", 20);
    PadDump(0xFD690000ULL, "comm5 pad 0", 0);
    DumpDwords("ML_after_boot", g_hda, 0xC00, 48);
    Log("SSP1 before FW_READY ack: %08X %08X", R32(g_dsp, 0x11000), R32(g_dsp, 0x11004));
    DumpDwords("ML_after_boot", g_hda, 0xC00, 48);
    Log("SSP1 before FW_READY ack: %08X %08X", R32(g_dsp, 0x11000), R32(g_dsp, 0x11004));
    /* ack FW_READY */
    W32(g_dsp, DSP_HIPCTDR, IPC_BUSY);
    W32(g_dsp, DSP_HIPCTDA, IPC_DONE);
    Log("SSP1 after FW_READY ack: %08X %08X", R32(g_dsp, 0x11000), R32(g_dsp, 0x11004));

    st = WdfCommonBufferCreate(enabler, PCM_BYTES, WDF_NO_OBJECT_ATTRIBUTES, &pcmBuf);
    if (NT_SUCCESS(st)) st = WdfCommonBufferCreate(enabler, 4096, WDF_NO_OBJECT_ATTRIBUTES, &pcmBdl);
    if (!NT_SUCCESS(st)) { Log("pcm buffers failed %08X", st); return st; }
    bufV = WdfCommonBufferGetAlignedVirtualAddress(pcmBuf);
    bufA = (ULONG64)WdfCommonBufferGetAlignedLogicalAddress(pcmBuf).QuadPart;
    bdl  = (BDLE *)WdfCommonBufferGetAlignedVirtualAddress(pcmBdl);
    bdlA = (ULONG64)WdfCommonBufferGetAlignedLogicalAddress(pcmBdl).QuadPart;
    FillTone((PLONG)bufV, PCM_FRAMES);

    /* ---- firmware trace over a capture DMA stream (index 0, tag 1) ---- */
    {
        WDFCOMMONBUFFER trBuf = NULL, trBdl = NULL; NTSTATUS tst;
        tst = WdfCommonBufferCreate(enabler, 65536, WDF_NO_OBJECT_ATTRIBUTES, &trBuf);
        if (NT_SUCCESS(tst)) tst = WdfCommonBufferCreate(enabler, 4096, WDF_NO_OBJECT_ATTRIBUTES, &trBdl);
        if (NT_SUCCESS(tst)) {
            ULONG to = HDA_SD(0), j;
            ULONG64 tbA = (ULONG64)WdfCommonBufferGetAlignedLogicalAddress(trBuf).QuadPart;
            ULONG64 tdA = (ULONG64)WdfCommonBufferGetAlignedLogicalAddress(trBdl).QuadPart;
            BDLE *tb = (BDLE *)WdfCommonBufferGetAlignedVirtualAddress(trBdl);
            g_trV = WdfCommonBufferGetAlignedVirtualAddress(trBuf);
            RtlZeroMemory(g_trV, 65536);
            if (g_ppcap) W32(g_hda, g_ppcap + PP_PPCTL, R32(g_hda, g_ppcap + PP_PPCTL) | 1u);
            W8(g_hda, to + SD_CTL, R8(g_hda, to + SD_CTL) & ~(SD_RUN | SD_INTEN));
            WaitRun(to, FALSE); W8(g_hda, to + SD_STS, SD_INTEN);
            StreamReset(0);
            W32(g_hda, to + SD_BDLPL, 0); W32(g_hda, to + SD_BDLPU, 0);
            for (j = 0; j < 4; j++) {
                ULONG64 a = tbA + j * 16384;
                tb[j].lo = (ULONG)a; tb[j].hi = (ULONG)(a >> 32); tb[j].size = 16384; tb[j].ioc = 0;
            }
            W8(g_hda, to + SD_CTL + 2, (R8(g_hda, to + SD_CTL + 2) & 0x0F) | (1 << 4));
            W32(g_hda, to + SD_CBL, 65536);
            W16(g_hda, to + SD_FMT, 0x0041);
            W16(g_hda, to + SD_LVI, 3);
            W32(g_hda, to + SD_BDLPL, (ULONG)tdA); W32(g_hda, to + SD_BDLPU, (ULONG)(tdA >> 32));
            RtlZeroMemory(&m, sizeof(m));
            m.tp.hdr.size = sizeof(m.tp); m.tp.hdr.cmd = 0x90000000u | 0x00030000u;
            m.tp.b_size = 28; m.tp.b_phy = (ULONG)tbA; m.tp.b_pages = 16; m.tp.b_bytes = 65536; m.tp.stream_tag = 1;
            tst = IpcTx("trace_dma_params", &m, sizeof(m.tp));
            if (NT_SUCCESS(tst)) { StreamStart(0); Log("trace stream started"); g_traceOn = TRUE; }
        } else Log("trace buffers failed %08X", tst);
    }

    /* ---- topology: host -> buffer -> SSP1 DAI ---- */
    RtlZeroMemory(&m, sizeof(m));
    m.pn.hdr.size = sizeof(m.pn); m.pn.hdr.cmd = GLB_TPLG | TPLG_PIPE_NEW;
    m.pn.comp_id = 6; m.pn.pipeline_id = 1; m.pn.sched_id = 5; m.pn.core = 0;
    m.pn.period = 1000; m.pn.priority = 0; m.pn.period_mips = 5000; m.pn.frames_per_sched = 0;
    m.pn.xrun_limit_usecs = 0; m.pn.time_domain = 1; /* timer */
    if (!NT_SUCCESS(st = IpcTx("pipe_new", &m, sizeof(m.pn)))) return st;

    RtlZeroMemory(&m, sizeof(m));
    m.h.comp.hdr.size = sizeof(m.h); m.h.comp.hdr.cmd = GLB_TPLG | TPLG_COMP_NEW;
    m.h.comp.id = 1; m.h.comp.type = COMP_HOST; m.h.comp.pipeline_id = 1; m.h.comp.core = 0;
    CompCfg(&m.h.config, 2, 0, FRAME_S32_LE);
    m.h.direction = 0; m.h.no_irq = 0; m.h.dmac_config = 0;
    if (!NT_SUCCESS(st = IpcTx("host", &m, sizeof(m.h)))) return st;

    RtlZeroMemory(&m, sizeof(m));
    m.b.comp.hdr.size = sizeof(m.b); m.b.comp.hdr.cmd = GLB_TPLG | TPLG_BUFFER_NEW;
    m.b.comp.id = 3; m.b.comp.type = COMP_BUFFER; m.b.comp.pipeline_id = 1; m.b.comp.core = 0;
    m.b.size = 3072; m.b.caps = 0x31; m.b.flags = 0;
    if (!NT_SUCCESS(st = IpcTx("buffer", &m, sizeof(m.b)))) return st;

    RtlZeroMemory(&m, sizeof(m));
    m.d.comp.hdr.size = sizeof(m.d); m.d.comp.hdr.cmd = GLB_TPLG | TPLG_COMP_NEW;
    m.d.comp.id = 5; m.d.comp.type = COMP_DAI; m.d.comp.pipeline_id = 1; m.d.comp.core = 0;
    CompCfg(&m.d.config, 0, 2, FRAME_S24_4LE);
    m.d.direction = 0; m.d.dai_index = DAI_IDX; m.d.type = DAI_SSP;
    if (!NT_SUCCESS(st = IpcTx("dai", &m, sizeof(m.d)))) return st;

    RtlZeroMemory(&m, sizeof(m));
    m.c.hdr.size = sizeof(m.c); m.c.hdr.cmd = GLB_TPLG | TPLG_COMP_CONNECT;
    m.c.source_id = 1; m.c.sink_id = 3;
    if (!NT_SUCCESS(st = IpcTx("connect host->buf", &m, sizeof(m.c)))) return st;
    m.c.source_id = 3; m.c.sink_id = 5;
    if (!NT_SUCCESS(st = IpcTx("connect buf->dai", &m, sizeof(m.c)))) return st;

    RtlZeroMemory(&m, sizeof(m));
    m.dc.hdr.size = sizeof(m.dc); m.dc.hdr.cmd = GLB_DAI | DAI_CONFIG;
    m.dc.type = DAI_SSP; m.dc.dai_index = DAI_IDX;
    m.dc.format = 0x0001 | 0x4000;           /* I2S, gated, NB_NF, codec bclk+fsync consumer */
    m.dc.flags = 1;                          /* HW_PARAMS */
    m.dc.mclk_rate = 19200000; m.dc.fsync_rate = 48000; m.dc.bclk_rate = 3072000;
    m.dc.tdm_slots = 2; m.dc.rx_slots = 3; m.dc.tx_slots = 3;
    m.dc.sample_valid_bits = 24; m.dc.tdm_slot_width = 32;
    m.dc.clks_control = (1u << 6) | (1u << 7);   /* MCLK_ES | BCLK_ES: start clocks at config time */
    if (!NT_SUCCESS(st = IpcTx("dai_config", &m, sizeof(m.dc)))) return st;

    RtlZeroMemory(&m, sizeof(m));
    m.pr.hdr.size = sizeof(m.pr); m.pr.hdr.cmd = GLB_TPLG | TPLG_PIPE_COMPLETE; m.pr.comp_id = 6;
    if (!NT_SUCCESS(st = IpcTx("pipe_complete", &m, sizeof(m.pr)))) return st;
    Ssp1Snap("after pipe_complete");
    DumpDwords("ML_after_cfg", g_hda, 0xC00, 48);
    MultiSample("after cfg");
#ifndef PLAY_SOUND
    Log("silent mode: stopping before PCM start");
    return STATUS_SUCCESS;
#endif

    /* ---- host DMA stream (HDA output stream #1) ---- */
    if (g_ppcap) W32(g_hda, g_ppcap + PP_PPCTL, R32(g_hda, g_ppcap + PP_PPCTL) | (1u << idx));
    W8(g_hda, o + SD_CTL, R8(g_hda, o + SD_CTL) & ~(SD_RUN | SD_INTEN));
    WaitRun(o, FALSE);
    W8(g_hda, o + SD_STS, SD_INTEN);
    StreamReset(idx);
    W32(g_hda, o + SD_BDLPL, 0); W32(g_hda, o + SD_BDLPU, 0);
    for (i = 0; i < 4; i++) {
        ULONG64 a = bufA + i * (PCM_BYTES / 4);
        bdl[i].lo = (ULONG)a; bdl[i].hi = (ULONG)(a >> 32); bdl[i].size = PCM_BYTES / 4; bdl[i].ioc = 0;
    }
    W8(g_hda, o + SD_CTL + 2, (R8(g_hda, o + SD_CTL + 2) & 0x0F) | (tag << 4));
    W32(g_hda, o + SD_CBL, PCM_BYTES);
    W16(g_hda, o + SD_FMT, 0x0041);          /* 48 kHz, 32-bit, 2 ch */
    W16(g_hda, o + SD_LVI, 3);
    W32(g_hda, o + SD_BDLPL, (ULONG)bdlA); W32(g_hda, o + SD_BDLPU, (ULONG)(bdlA >> 32));
    Log("host stream %u prepared tag=%u fifos=%u", idx, tag, R16(g_hda, o + SD_FIFOS));

    RtlZeroMemory(&m, sizeof(m));
    m.pp.hdr.size = sizeof(m.pp); m.pp.hdr.cmd = GLB_STREAM | STREAM_PCM_PARAMS;
    m.pp.comp_id = 1; m.pp.flags = 0;
    m.pp.p_size = 84;
    m.pp.b_size = 28; m.pp.phy_addr = (ULONG)bufA; m.pp.pages = PCM_BYTES / 4096; m.pp.b_bytes = PCM_BYTES;
    m.pp.direction = 0; m.pp.frame_fmt = FRAME_S32_LE; m.pp.buffer_fmt = 0; m.pp.rate = 48000;
    m.pp.stream_tag = (USHORT)tag; m.pp.channels = 2; m.pp.sample_valid_bytes = 4; m.pp.sample_container_bytes = 4;
    m.pp.host_period_bytes = PCM_BYTES / 4; m.pp.no_stream_position = 0; m.pp.cont_update_posn = 1;
    m.pp.chmap[0] = 0; m.pp.chmap[1] = 1;
    if (!NT_SUCCESS(st = IpcTx("pcm_params", &m, sizeof(m.pp)))) return st;
    g_posnOffset = g_reply[4]; Log("pcm_params reply: %08X %08X %08X %08X %08X (posn_offset=%X)", g_reply[0], g_reply[1], g_reply[2], g_reply[3], g_reply[4], g_posnOffset);

    /* Linux order (sof_pcm_trigger START): host DMA RUN first, then the IPC trigger.
       No Log() between them: write-through logging delays the start and the DAI underruns. */
    StreamStart(idx);
    RtlZeroMemory(&m, sizeof(m));
    m.s.hdr.size = sizeof(m.s); m.s.hdr.cmd = GLB_STREAM | STREAM_TRIG_START; m.s.comp_id = 1;
    if (!NT_SUCCESS(st = IpcTx("trigger start", &m, sizeof(m.s)))) return st;
    MultiSample("running");
    Log("host DMA started; playing tone ~10 s  LPIB=%u", R32(g_hda, o + 4));

    {
        LARGE_INTEGER d; ULONG k;
        d.QuadPart = -10 * 1000 * 500;       /* 0.5 s */
        for (k = 1; k <= 20; k++) {          /* 10 s */
            KeDelayExecutionThread(KernelMode, FALSE, &d);
            if (R32(g_dsp, DSP_HIPCTDR) & IPC_BUSY) {
                Log("  dsp msg: TDR=%08X TDD=%08X mbox: %08X %08X %08X %08X", R32(g_dsp, DSP_HIPCTDR), R32(g_dsp, 0xC8),
                    R32(g_dsp, DSP_MBOX_UP), R32(g_dsp, DSP_MBOX_UP + 4), R32(g_dsp, DSP_MBOX_UP + 8), R32(g_dsp, DSP_MBOX_UP + 12));
                IpcAckDspMessages();
            }
            if ((k % 8) == 0) {
                ULONG pb = 0xC1000 + g_posnOffset;
                Log("posn@%X: %08X %08X %08X %08X %08X %08X %08X %08X %08X %08X %08X %08X", pb, R32(g_dsp, pb), R32(g_dsp, pb+4), R32(g_dsp, pb+8), R32(g_dsp, pb+12), R32(g_dsp, pb+16), R32(g_dsp, pb+20), R32(g_dsp, pb+24), R32(g_dsp, pb+28), R32(g_dsp, pb+32), R32(g_dsp, pb+36), R32(g_dsp, pb+40), R32(g_dsp, pb+44));
            }
            if (k == 8) {
                ULONG s;
                MultiSample("t=4s");
                for (s = 0; s < 3; s++) {
                    ULONG b = DSP_SSP_BASE + s * 0x1000;
                    Log("SSP%u: SSCR0=%08X SSCR1=%08X SSSR=%08X SSITR=%08X SSTO=%08X SSPSP=%08X SSTSA=%08X SSRSA=%08X",
                        s, R32(g_dsp, b), R32(g_dsp, b + 4), R32(g_dsp, b + 8), R32(g_dsp, b + 0xC), R32(g_dsp, b + 0x28),
                        R32(g_dsp, b + 0x2C), R32(g_dsp, b + 0x30), R32(g_dsp, b + 0x34));
                    Log("SSP%u: SSCR2=%08X SSPSP2=%08X SSCR3=%08X SSIOC=%08X", s, R32(g_dsp, b + 0x40), R32(g_dsp, b + 0x44), R32(g_dsp, b + 0x48), R32(g_dsp, b + 0x4C));
                }
            }
            if ((k % 4) == 0) Log("t=%us LPIB=%u STS=%02X CTL=%02X FIFOS=%u ADSPCS=%08X fwst=%08X", k / 2, R32(g_hda, o + 4),
                R8(g_hda, o + SD_STS), R8(g_hda, o + SD_CTL), R16(g_hda, o + SD_FIFOS), R32(g_dsp, DSP_ADSPCS), R32(g_dsp, DSP_ROM_STATUS));
        }
    }

    if (g_traceOn) { DumpFile(L"\\??\\D:\\jsl-audio\\trace.bin", g_trV, 65536); StreamStop(0); Log("trace saved"); }
    DumpDwords("trace", g_dsp, 0xE0000, 16);
    DumpDwords("dbg", g_dsp, 0xC0000, 32);
    DumpDwords("exc", g_dsp, 0xC0800, 16);
    RtlZeroMemory(&m, sizeof(m));
    m.s.hdr.size = sizeof(m.s); m.s.hdr.cmd = GLB_STREAM | STREAM_TRIG_STOP; m.s.comp_id = 1;
    IpcTx("trigger stop", &m, sizeof(m.s));
    StreamStop(idx);
    RtlZeroMemory(&m, sizeof(m));
    m.s.hdr.size = sizeof(m.s); m.s.hdr.cmd = GLB_STREAM | STREAM_PCM_FREE; m.s.comp_id = 1;
    IpcTx("pcm_free", &m, sizeof(m.s));
    PadSetTx(0xFD6D0000ULL, 41, 0, "SDB GPP_D17 off");
    W32(g_hda, o + SD_BDLPL, 0); W32(g_hda, o + SD_BDLPU, 0); W32(g_hda, o, 0);
    return STATUS_SUCCESS;
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

    Log("SSP regs after power-up: SSP0 %08X %08X %08X  SSP1 %08X %08X %08X  SSP2 %08X %08X", R32(g_dsp, 0x10000), R32(g_dsp, 0x10004), R32(g_dsp, 0x10008), R32(g_dsp, 0x11000), R32(g_dsp, 0x11004), R32(g_dsp, 0x11008), R32(g_dsp, 0x12000), R32(g_dsp, 0x12004));
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
    {   /* ICL-style HPRO: power up pseudo core 3 and keep it stalled (icl_dsp_post_fw_run) */
        BOOLEAN hp = CorePowerUp(8);
        AdspcsRmw(0, 8u << 8);
        Log("HPRO core3 power-up: %d ADSPCS=%08X", hp, R32(g_dsp, DSP_ADSPCS));
    }
    Log("SSP regs after fw: SSP0 %08X %08X %08X  SSP1 %08X %08X %08X", R32(g_dsp, 0x10000), R32(g_dsp, 0x10004), R32(g_dsp, 0x10008), R32(g_dsp, 0x11000), R32(g_dsp, 0x11004), R32(g_dsp, 0x11008));

    /* wait for the FW_READY IPC from the DSP */
    for (t = 0; t < 5000000; t += 1000) {
        if (R32(g_dsp, DSP_HIPCTDR) & IPC_BUSY) break;
        KeStallExecutionProcessor(1000);
    }
    Log("HIPCTDR=%08X HIPCTDA=%08X ADSPCS=%08X", R32(g_dsp, DSP_HIPCTDR), R32(g_dsp, DSP_HIPCTDA), R32(g_dsp, DSP_ADSPCS));
    DumpDwords("win0", g_dsp, DSP_ROM_STATUS, 16);
    DumpDwords("mbox", g_dsp, DSP_MBOX_UP, 48);
    {
        NTSTATUS pst = PlayTone(enabler);
        Log("PlayTone -> %08X", pst);
    }

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
            {   /* disable DSP power/clock gating before booting (hda_dsp_pre_fw_run) */
                ULONG pg = 0, cg = 0;
                bus.GetBusData(bus.Context, PCI_WHICHSPACE_CONFIG, &pg, 0x44, 4);
                bus.GetBusData(bus.Context, PCI_WHICHSPACE_CONFIG, &cg, 0x48, 4);
                Log("PGCTL=%08X CGCTL=%08X (before)", pg, cg);
                pg |= (1u << 2); cg &= ~(1u << 1);
                bus.SetBusData(bus.Context, PCI_WHICHSPACE_CONFIG, &pg, 0x44, 4);
                bus.SetBusData(bus.Context, PCI_WHICHSPACE_CONFIG, &cg, 0x48, 4);
                pg = cg = 0;
                bus.GetBusData(bus.Context, PCI_WHICHSPACE_CONFIG, &pg, 0x44, 4);
                bus.GetBusData(bus.Context, PCI_WHICHSPACE_CONFIG, &cg, 0x48, 4);
                Log("PGCTL=%08X CGCTL=%08X (after)", pg, cg);
                W32(g_hda, 0x1030, R32(g_hda, 0x1030) & ~(1u << 13));   /* EM2.L1SEN off */
            }
            bus.InterfaceDereference(bus.Context);
        } else Log("no bus interface");
    }

    /* full HDA controller reset so no state is carried over from a previous run */
    {
        ULONG t;
        W32(g_hda, HDA_GCTL, R32(g_hda, HDA_GCTL) & ~1u);
        for (t = 0; t < 1000 && (R32(g_hda, HDA_GCTL) & 1); t++) KeStallExecutionProcessor(100);
        KeStallExecutionProcessor(200);
        W32(g_hda, HDA_GCTL, R32(g_hda, HDA_GCTL) | 1u);
        for (t = 0; t < 1000 && !(R32(g_hda, HDA_GCTL) & 1); t++) KeStallExecutionProcessor(100);
        KeStallExecutionProcessor(1000);
        Log("controller reset done GCTL=%08X PPCTL?=n/a", R32(g_hda, HDA_GCTL));
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






















