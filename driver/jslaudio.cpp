// SPDX-License-Identifier: MIT
/*
 * jslaudio: PortCls WaveRT driver for the Jasper Lake cAVS 2.0 DSP (SOF firmware, IPC3)
 * driving the RT1015P speaker amp on SSP1 (Acer Chromebook R753T / Magolor).
 *
 * Hardware sequence is the one proven in boot2.c:
 *   - DSP boot via HDA code-loader stream, SOF v2.2 firmware
 *   - static pipelines (see g_paths):
 *       speaker: host(1) -> buffer(3) -> SSP1 DAI(5), pipeline 1, HDA output stream 8 / tag 2
 *       DMIC:    DMIC0 DAI(15) -> buffer(13) -> host(11), pipeline 2, HDA input stream 1 / tag 2
 *   - per stream: pcm_params, host DMA RUN *before* IPC trigger
 *   - amp SDB = GPP_D17 (community 1 pad 41) high while the speaker runs
 * Only 48 kHz / 2 ch / 32-bit PCM is exposed; the Windows audio engine converts.
 * Everything is polled; no interrupts. Log: %SystemRoot%\Temp\jslaudio.log (or service value LogPath)
 */
#define _NEW_DELETE_OPERATORS_
#include <initguid.h>
#include <ntddk.h>
#include <portcls.h>
#include <stdunk.h>
#include <ksmedia.h>
#include <wdmguid.h>
#include <ntstrsafe.h>

/* ------------------------------------------------------------------ C++ runtime bits */
void *__cdecl operator new(size_t n, POOL_FLAGS f, ULONG tag) { return ExAllocatePool2(f, n, tag); }
/* operator delete(void *) comes from stdunk.lib */
void __cdecl operator delete(void *p, size_t) { if (p) ExFreePool(p); }
extern "C" int __cdecl _purecall(void) { KeBugCheckEx(0x4A534C00, 0, 0, 0, 0); }

#define TAG 'auLJ'
#define FW_PATH   L"\\SystemRoot\\System32\\drivers\\sof-jsl.ri"   /* fallback only; normally next to jslaudio.sys */
#define LOG_PATH  L"\\SystemRoot\\Temp\\jslaudio.log"
#define EXTMAN_MAGIC 0x6E614D58u

/* ------------------------------------------------------------------ registers */
#define HDA_GCAP   0x00
#define HDA_GCTL   0x08
#define HDA_LLCH   0x14
#define HDA_LTRP   0x1048
#define HDA_SD(i)  (0x80 + (i) * 0x20)
#define SD_CTL     0x00
#define SD_STS     0x03
#define SD_LPIB    0x04
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

#define DSP_ADSPCS 0x04
#define DSP_HIPCTDR 0xC0
#define DSP_HIPCTDA 0xC4
#define DSP_HIPCIDR 0xD0
#define DSP_HIPCIDA 0xD4
#define DSP_ROM_STATUS 0x80000
#define HOSTBOX    0x82000
#define IPC_BUSY      (1u << 31)
#define IPC_DONE      (1u << 31)
#define ROM_IPC_CONTROL  0x01000000u
#define ROM_IPC_PURGE_FW 0x00004000u
#define FSR_STATE(x)  ((x) & 0x00FFFFFFu)
#define FSR_INIT_DONE 0x1
#define FSR_FW_ENTERED 0x5
#define DSP_SSP_BASE  0x10000
#define SSP_CBP_CFP   ((1u << 25) | (1u << 24))

#define GPIO_COMM1 0xFD6D0000ULL
#define SDB_PAD    41            /* GPP_D17 = ACPI GpioIo 209 (gpio_base space) */

#define R8(b, o)  READ_REGISTER_UCHAR((b) + (o))
#define R16(b, o) READ_REGISTER_USHORT((PUSHORT)((b) + (o)))
#define R32(b, o) READ_REGISTER_ULONG((PULONG)((b) + (o)))
#define W8(b, o, v)  WRITE_REGISTER_UCHAR((b) + (o), (UCHAR)(v))
#define W16(b, o, v) WRITE_REGISTER_USHORT((PUSHORT)((b) + (o)), (USHORT)(v))
#define W32(b, o, v) WRITE_REGISTER_ULONG((PULONG)((b) + (o)), (ULONG)(v))

typedef struct { ULONG lo, hi, size, ioc; } BDLE;

static PUCHAR g_hda, g_dsp;
static ULONG  g_hdaLen, g_dspLen, g_ppcap, g_spbcap, g_iss, g_oss;
static HANDLE g_log;
static KMUTEX g_ipcLock;
static BOOLEAN g_dspReady;
static BOOLEAN g_inPower;               /* inside a power transition: no file I/O (disk may still be asleep) */
static PVOID  g_fw; static ULONG g_fwSize;   /* firmware cached at first start, reused on resume */
static BUS_INTERFACE_STANDARD g_bus; static BOOLEAN g_busValid;
static DEVICE_POWER_STATE g_dstate = PowerDeviceD0;
static NTSTATUS g_resumeStatus = STATUS_SUCCESS; static ULONG g_resumeCount;

/* ------------------------------------------------------------------ logging (PASSIVE_LEVEL only) */
static VOID LogOpen(VOID);
static char g_pend[8192]; static ULONG g_pendLen;   /* lines logged before the log file can be opened (early boot) */

static VOID Log(const char *fmt, ...)
{
    char buf[300]; va_list ap; IO_STATUS_BLOCK iosb; size_t len;
    if (g_inPower || KeGetCurrentIrql() != PASSIVE_LEVEL) return;
    va_start(ap, fmt);
    RtlStringCbVPrintfA(buf, sizeof(buf) - 2, fmt, ap);
    va_end(ap);
    RtlStringCbLengthA(buf, sizeof(buf) - 2, &len);
    buf[len++] = '\r'; buf[len++] = '\n';
    if (!g_log) LogOpen();
    if (!g_log) {
        if (g_pendLen + len <= sizeof(g_pend)) { RtlCopyMemory(g_pend + g_pendLen, buf, len); g_pendLen += (ULONG)len; }
        return;
    }
    if (g_pendLen) {
        ZwWriteFile(g_log, NULL, NULL, NULL, &iosb, g_pend, g_pendLen, NULL, NULL);
        g_pendLen = 0;
    }
    ZwWriteFile(g_log, NULL, NULL, NULL, &iosb, buf, (ULONG)len, NULL, NULL);
}

/* optional REG_SZ "LogPath" (NT path, e.g. \??\D:\jslaudio.log) under the service key overrides LOG_PATH;
   read on every open attempt so it can be set after the driver is loaded (scripts\install.ps1 -LogPath) */
static WCHAR g_regPath[300];

static BOOLEAN RegReadString(PCWSTR value, PWCHAR out, ULONG cbOut)
{
    OBJECT_ATTRIBUTES oa; HANDLE k; UNICODE_STRING kn, vn; ULONG len = 0; BOOLEAN ok = FALSE;
    UCHAR buf[sizeof(KEY_VALUE_PARTIAL_INFORMATION) + 700];
    PKEY_VALUE_PARTIAL_INFORMATION kv = (PKEY_VALUE_PARTIAL_INFORMATION)buf;
    if (!g_regPath[0]) return FALSE;
    RtlInitUnicodeString(&kn, g_regPath);
    InitializeObjectAttributes(&oa, &kn, OBJ_KERNEL_HANDLE | OBJ_CASE_INSENSITIVE, NULL, NULL);
    if (!NT_SUCCESS(ZwOpenKey(&k, KEY_READ, &oa))) return FALSE;
    RtlInitUnicodeString(&vn, value);
    RtlZeroMemory(buf, sizeof(buf));
    if (NT_SUCCESS(ZwQueryValueKey(k, &vn, KeyValuePartialInformation, kv, sizeof(buf) - 2, &len)) &&
        (kv->Type == REG_SZ || kv->Type == REG_EXPAND_SZ) && kv->DataLength >= 4)
        ok = NT_SUCCESS(RtlStringCbCopyNW(out, cbOut, (PCWSTR)kv->Data, kv->DataLength));
    ZwClose(k);
    return ok;
}

static VOID LogOpen(VOID)
{
    UNICODE_STRING n; OBJECT_ATTRIBUTES oa; IO_STATUS_BLOCK iosb; WCHAR path[300];
    if (g_log) return;
    if (!RegReadString(L"LogPath", path, sizeof(path))) RtlStringCbCopyW(path, sizeof(path), LOG_PATH);
    RtlInitUnicodeString(&n, path);
    InitializeObjectAttributes(&oa, &n, OBJ_KERNEL_HANDLE | OBJ_CASE_INSENSITIVE, NULL, NULL);
    if (!NT_SUCCESS(ZwCreateFile(&g_log, FILE_APPEND_DATA | SYNCHRONIZE, &oa, &iosb, NULL,
        FILE_ATTRIBUTE_NORMAL, FILE_SHARE_READ | FILE_SHARE_WRITE, FILE_OPEN_IF,
        FILE_SYNCHRONOUS_IO_NONALERT | FILE_WRITE_THROUGH | FILE_NON_DIRECTORY_FILE, NULL, 0)))
        g_log = NULL;
}

static BOOLEAN Poll(PUCHAR b, ULONG off, ULONG mask, ULONG want, ULONG timeout_us)
{
    for (ULONG t = 0; t <= timeout_us; t += 100) {
        if ((R32(b, off) & mask) == want) return TRUE;
        KeStallExecutionProcessor(100);
    }
    return (R32(b, off) & mask) == want;
}

static VOID SleepMs(ULONG ms)
{
    LARGE_INTEGER d; d.QuadPart = -10000LL * ms;
    KeDelayExecutionThread(KernelMode, FALSE, &d);
}

/* ------------------------------------------------------------------ DMA memory */
typedef struct { PVOID va; ULONG64 pa; SIZE_T size; } DMABUF;

static BOOLEAN DmaAlloc(DMABUF *d, SIZE_T size)
{
    PHYSICAL_ADDRESS lo, hi, bound;
    lo.QuadPart = 0; hi.QuadPart = 0x0000FFFFFFFFFFFFLL; bound.QuadPart = 0;
    d->size = (size + PAGE_SIZE - 1) & ~(SIZE_T)(PAGE_SIZE - 1);
    d->va = MmAllocateContiguousMemorySpecifyCache(d->size, lo, hi, bound, MmNonCached);
    if (!d->va) return FALSE;
    RtlZeroMemory(d->va, d->size);
    d->pa = (ULONG64)MmGetPhysicalAddress(d->va).QuadPart;
    return TRUE;
}

static VOID DmaFree(DMABUF *d)
{
    if (d->va) MmFreeContiguousMemorySpecifyCache(d->va, d->size, MmNonCached);
    d->va = NULL;
}

/* ------------------------------------------------------------------ GPIO (amp SDB) */
static VOID AmpEnable(BOOLEAN on)
{
    PHYSICAL_ADDRESS pa; PUCHAR b; ULONG off, v;
    pa.QuadPart = (LONGLONG)GPIO_COMM1;
    b = (PUCHAR)MmMapIoSpaceEx(pa, 0x10000, PAGE_READWRITE | PAGE_NOCACHE);
    if (!b) return;
    off = (R32(b, 0x0C) & 0xFFFF) + SDB_PAD * 16;
    v = R32(b, off);
    if (((v >> 10) & 7) == 0 && !((v >> 8) & 1))          /* GPIO mode, TX enabled */
        W32(b, off, (v & ~1u) | (on ? 1u : 0u));
    MmUnmapIoSpace(b, 0x10000);
}

/* ------------------------------------------------------------------ DSP core control */
static VOID AdspcsRmw(ULONG clr, ULONG set) { W32(g_dsp, DSP_ADSPCS, (R32(g_dsp, DSP_ADSPCS) & ~clr) | set); }
static BOOLEAN CorePowerUp(ULONG m) { AdspcsRmw(0, m << 16); return Poll(g_dsp, DSP_ADSPCS, m << 24, m << 24, 50000); }
static BOOLEAN CoreResetEnter(ULONG m) { AdspcsRmw(0, m); return Poll(g_dsp, DSP_ADSPCS, m, m, 50000); }
static BOOLEAN CoreResetLeave(ULONG m) { AdspcsRmw(m, 0); return Poll(g_dsp, DSP_ADSPCS, m, 0, 50000); }
static BOOLEAN CoreStallReset(ULONG m) { AdspcsRmw(0, m << 8); return CoreResetEnter(m); }
static BOOLEAN CoreEnabled(ULONG m)
{
    ULONG v = R32(g_dsp, DSP_ADSPCS);
    return ((v & (m << 24)) == (m << 24)) && ((v & (m << 16)) == (m << 16)) && !(v & m) && !(v & (m << 8));
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

/* ------------------------------------------------------------------ HDA stream helpers */
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
    for (ULONG t = 0; t < 3000; t++) {
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

/* program a playback stream over a physically contiguous buffer, 2 BDL entries */
static BOOLEAN StreamSetup(ULONG idx, ULONG tag, DMABUF *bdlMem, ULONG64 bufPa, ULONG size, USHORT fmt)
{
    ULONG o = HDA_SD(idx), half = size / 2;
    BDLE *bdl = (BDLE *)bdlMem->va;
    if (g_ppcap) W32(g_hda, g_ppcap + PP_PPCTL, R32(g_hda, g_ppcap + PP_PPCTL) | (1u << idx));
    W8(g_hda, o + SD_CTL, R8(g_hda, o + SD_CTL) & ~(SD_RUN | SD_INTEN));
    if (!WaitRun(o, FALSE)) return FALSE;
    W8(g_hda, o + SD_STS, SD_INTEN);
    if (!StreamReset(idx)) return FALSE;
    W32(g_hda, o + SD_BDLPL, 0); W32(g_hda, o + SD_BDLPU, 0);
    bdl[0].lo = (ULONG)bufPa;          bdl[0].hi = (ULONG)(bufPa >> 32);          bdl[0].size = half;        bdl[0].ioc = 0;
    bdl[1].lo = (ULONG)(bufPa + half); bdl[1].hi = (ULONG)((bufPa + half) >> 32); bdl[1].size = size - half; bdl[1].ioc = 0;
    KeMemoryBarrier();
    W8(g_hda, o + SD_CTL + 2, (R8(g_hda, o + SD_CTL + 2) & 0x0F) | (tag << 4));
    W32(g_hda, o + SD_CBL, size);
    W16(g_hda, o + SD_FMT, fmt);
    W16(g_hda, o + SD_LVI, 1);
    W32(g_hda, o + SD_BDLPL, (ULONG)bdlMem->pa);
    W32(g_hda, o + SD_BDLPU, (ULONG)(bdlMem->pa >> 32));
    return TRUE;
}

static VOID StreamClear(ULONG idx)
{
    ULONG o = HDA_SD(idx);
    StreamStop(idx);
    W32(g_hda, o + SD_BDLPL, 0); W32(g_hda, o + SD_BDLPU, 0);
    W32(g_hda, o, 0);
}

/* ------------------------------------------------------------------ IPC3 */
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
#define STREAM_PCM_FREE    0x00030000u
#define STREAM_TRIG_START  0x00040000u
#define STREAM_TRIG_STOP   0x00050000u
#define STREAM_TRIG_PAUSE  0x00060000u
#define STREAM_TRIG_RELEASE 0x00070000u
#define COMP_HOST 1
#define COMP_DAI  2
#define COMP_BUFFER 12
#define FRAME_S24_4LE 1
#define FRAME_S32_LE  2
#define DAI_SSP  1
#define DAI_DMIC 2
#define DIR_PLAYBACK 0
#define DIR_CAPTURE  1

/* One audio path = one static SOF pipeline + one HDA host stream + one Windows endpoint.
   Component ids: host = base+1, buffer = base+3, dai = base+5, pipeline (sched) comp = base+6. */
typedef struct {
    const char *name;
    ULONG   dir;          /* DIR_PLAYBACK / DIR_CAPTURE */
    ULONG   base;         /* component id base, also used as pipeline id / 10 */
    ULONG   daiType, daiIndex, daiFrameFmt;
    ULONG   tag;          /* HDA stream tag (per direction) */
    BOOLEAN amp;          /* drive the RT1015P SDB while running */
    ULONG   hdaIdx;       /* HDA stream descriptor index, set at HwStart */
    BOOLEAN ready;        /* pipeline built in the current DSP boot */
} AUDIOPATH;

enum { PATH_SPK = 0, PATH_DMIC = 1, PATH_HP = 2, NPATHS };
static AUDIOPATH g_paths[NPATHS] = {
    { "speaker",   DIR_PLAYBACK,  0, DAI_SSP,  1, FRAME_S24_4LE, 2, TRUE  },
    { "dmic",      DIR_CAPTURE,  10, DAI_DMIC, 0, FRAME_S32_LE,  2, FALSE },
    { "headphone", DIR_PLAYBACK, 20, DAI_SSP,  0, FRAME_S24_4LE, 3, FALSE },   /* RT5682 via SSP0 (codec set up by jslcodec) */
};
#define HOST_ID(p) ((p)->base + 1)
#define BUF_ID(p)  ((p)->base + 3)
#define DAI_ID(p)  ((p)->base + 5)
#define PIPE_ID(p) ((p)->base + 6)

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
    ULONG s_reserved0; USHORT s_reserved1, mclk_id; ULONG mclk_rate, fsync_rate, bclk_rate;
    ULONG tdm_slots, rx_slots, tx_slots, sample_valid_bits; USHORT tdm_slot_width, s_reserved2;
    ULONG mclk_direction; USHORT frame_pulse_width, tdm_per_slot_padding_flag;
    ULONG clks_control, quirks, bclk_delay;
} SSP_PARAMS;                                   /* sof_ipc_dai_ssp_params, 60 bytes */
typedef struct {
    ULONG reserved0; USHORT id, enable_mic_a, enable_mic_b, polarity_mic_a, polarity_mic_b, clk_edge, skew, reserved[3];
} DMIC_PDM;                                     /* sof_ipc_dai_dmic_pdm_ctrl, 24 bytes */
typedef struct {
    ULONG reserved0, driver_ipc_version, pdmclk_min, pdmclk_max, fifo_fs, reserved_1;
    USHORT fifo_bits, fifo_bits_b, duty_min, duty_max;
    ULONG num_pdm_active, wake_up_time, min_clock_on_time, unmute_ramp_time, reserved[5];
    DMIC_PDM pdm[4];
} DMIC_PARAMS;                                  /* sof_ipc_dai_dmic_params, 164 bytes */
typedef struct {
    CMDHDR hdr; ULONG type, dai_index; USHORT format; UCHAR group_id, flags; ULONG reserved[8];
    union { SSP_PARAMS ssp; DMIC_PARAMS dmic; };
} IPC_DAI_CONFIG;                               /* sof_ipc_dai_config, 216 bytes (Linux sends the full union) */
C_ASSERT(sizeof(SSP_PARAMS) == 60);
C_ASSERT(sizeof(DMIC_PARAMS) == 164);
C_ASSERT(sizeof(IPC_DAI_CONFIG) == 216);
typedef struct {
    CMDHDR hdr; ULONG comp_id, flags, reserved[2];
    ULONG p_size;
    ULONG b_size, phy_addr, pages, b_bytes, b_res[3];
    ULONG direction, frame_fmt, buffer_fmt, rate;
    USHORT stream_tag, channels, sample_valid_bytes, sample_container_bytes;
    ULONG host_period_bytes;
    USHORT no_stream_position; UCHAR cont_update_posn, reserved0; USHORT ext_data_length;
    UCHAR reserved2[2]; USHORT chmap[8];
} IPC_PCM_PARAMS;
typedef struct { CMDHDR hdr; ULONG comp_id; } IPC_STREAM;
#pragma pack(pop)

typedef union { IPC_HOST h; IPC_DAI d; IPC_BUFFER b; IPC_PIPE_NEW pn; IPC_PIPE_READY pr; IPC_CONNECT c; IPC_DAI_CONFIG dc; IPC_PCM_PARAMS pp; IPC_STREAM s; } IPCMSG;

static VOID IpcAckDspMessages(VOID)
{
    if (R32(g_dsp, DSP_HIPCTDR) & IPC_BUSY) {
        W32(g_dsp, DSP_HIPCTDR, IPC_BUSY);
        W32(g_dsp, DSP_HIPCTDA, IPC_DONE);
    }
}

/* caller holds g_ipcLock (or is single-threaded start-up) */
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
    rsize = R32(g_dsp, HOSTBOX); rcmd = R32(g_dsp, HOSTBOX + 4); err = R32(g_dsp, HOSTBOX + 8);
    W32(g_dsp, DSP_HIPCIDA, IPC_DONE);
    if (err) Log("IPC %s: cmd=%08X -> reply size=%u cmd=%08X err=%d", what, p[1], rsize, rcmd, (LONG)err);
    return err ? STATUS_UNSUCCESSFUL : STATUS_SUCCESS;
}

static NTSTATUS IpcStream(AUDIOPATH *p, const char *what, ULONG cmd)
{
    IPCMSG m; RtlZeroMemory(&m, sizeof(m));
    m.s.hdr.size = sizeof(m.s); m.s.hdr.cmd = GLB_STREAM | cmd; m.s.comp_id = HOST_ID(p);
    return IpcTx(what, &m, sizeof(m.s));
}

static VOID CompCfg(COMPCFG *c, ULONG sink, ULONG src, ULONG fmt)
{
    RtlZeroMemory(c, sizeof(*c));
    c->hdr.size = sizeof(*c);
    c->periods_sink = sink; c->periods_source = src; c->frame_fmt = fmt;
}

/* DAI configuration for each path (values from sof-jsl-rt5682-rt1015.tplg; DMIC as "2ch-pdm0" per ACPI) */
static VOID FillDaiConfig(AUDIOPATH *p, IPC_DAI_CONFIG *dc)
{
    dc->hdr.size = sizeof(*dc); dc->hdr.cmd = GLB_DAI | DAI_CONFIG;
    dc->type = p->daiType; dc->dai_index = p->daiIndex;
    dc->flags = 1;                                   /* HW_PARAMS */
    if (p->daiType == DAI_SSP) {
        /* both SSPs share the single I2S_MCLK pin: 24 MHz as in the topology (RT1015P ignores MCLK) */
        dc->format = 0x0001 | 0x4000;                /* I2S, NB_NF, codec bclk+fsync consumer */
        dc->ssp.mclk_rate = 24000000; dc->ssp.fsync_rate = 48000;
        dc->ssp.tdm_slots = 2; dc->ssp.rx_slots = 3; dc->ssp.tx_slots = 3;
        dc->ssp.sample_valid_bits = 24;
        if (p->daiIndex == 0) { dc->ssp.bclk_rate = 2400000; dc->ssp.tdm_slot_width = 25; }   /* RT5682: 50 fs */
        else                  { dc->ssp.bclk_rate = 3072000; dc->ssp.tdm_slot_width = 32; }   /* RT1015P: 64 fs */
        dc->ssp.clks_control = (1u << 6) | (1u << 7);   /* MCLK/BCLK early start: codecs lock before data */
    } else {
        dc->format = 6;                              /* SOF_DAI_FMT_PDM */
        dc->dmic.driver_ipc_version = 1;
        dc->dmic.pdmclk_min = 2400000; dc->dmic.pdmclk_max = 4800000;
        dc->dmic.duty_min = 40; dc->dmic.duty_max = 60;
        dc->dmic.fifo_fs = 48000; dc->dmic.fifo_bits = 32;
        dc->dmic.unmute_ramp_time = 200;
        dc->dmic.num_pdm_active = 1;
        dc->dmic.pdm[0].id = 0; dc->dmic.pdm[0].enable_mic_a = 1; dc->dmic.pdm[0].enable_mic_b = 1;
    }
}

/* static topology per path. playback: host -> buffer -> dai (scheduled on the dai)
                            capture:  dai  -> buffer -> host (scheduled on the dai) */
static NTSTATUS BuildPipeline(AUDIOPATH *p)
{
    IPCMSG m; NTSTATUS st;
    BOOLEAN cap = (p->dir == DIR_CAPTURE);

    RtlZeroMemory(&m, sizeof(m));
    m.pn.hdr.size = sizeof(m.pn); m.pn.hdr.cmd = GLB_TPLG | TPLG_PIPE_NEW;
    m.pn.comp_id = PIPE_ID(p); m.pn.pipeline_id = p->base / 10 + 1; m.pn.sched_id = DAI_ID(p); m.pn.core = 0;
    m.pn.period = 1000; m.pn.priority = 0; m.pn.period_mips = 5000; m.pn.time_domain = 1;
    if (!NT_SUCCESS(st = IpcTx("pipe_new", &m, sizeof(m.pn)))) return st;

    RtlZeroMemory(&m, sizeof(m));
    m.h.comp.hdr.size = sizeof(m.h); m.h.comp.hdr.cmd = GLB_TPLG | TPLG_COMP_NEW;
    m.h.comp.id = HOST_ID(p); m.h.comp.type = COMP_HOST; m.h.comp.pipeline_id = p->base / 10 + 1;
    CompCfg(&m.h.config, cap ? 0 : 2, cap ? 2 : 0, FRAME_S32_LE);
    m.h.direction = p->dir;
    if (!NT_SUCCESS(st = IpcTx("host", &m, sizeof(m.h)))) return st;

    RtlZeroMemory(&m, sizeof(m));
    m.b.comp.hdr.size = sizeof(m.b); m.b.comp.hdr.cmd = GLB_TPLG | TPLG_BUFFER_NEW;
    m.b.comp.id = BUF_ID(p); m.b.comp.type = COMP_BUFFER; m.b.comp.pipeline_id = p->base / 10 + 1;
    m.b.size = 3072; m.b.caps = 0x31;
    if (!NT_SUCCESS(st = IpcTx("buffer", &m, sizeof(m.b)))) return st;

    RtlZeroMemory(&m, sizeof(m));
    m.d.comp.hdr.size = sizeof(m.d); m.d.comp.hdr.cmd = GLB_TPLG | TPLG_COMP_NEW;
    m.d.comp.id = DAI_ID(p); m.d.comp.type = COMP_DAI; m.d.comp.pipeline_id = p->base / 10 + 1;
    CompCfg(&m.d.config, cap ? 2 : 0, cap ? 0 : 2, p->daiFrameFmt);
    m.d.direction = p->dir; m.d.dai_index = p->daiIndex; m.d.type = p->daiType;
    if (!NT_SUCCESS(st = IpcTx("dai", &m, sizeof(m.d)))) return st;

    RtlZeroMemory(&m, sizeof(m));
    m.c.hdr.size = sizeof(m.c); m.c.hdr.cmd = GLB_TPLG | TPLG_COMP_CONNECT;
    m.c.source_id = cap ? DAI_ID(p) : HOST_ID(p); m.c.sink_id = BUF_ID(p);
    if (!NT_SUCCESS(st = IpcTx("connect 1", &m, sizeof(m.c)))) return st;
    m.c.source_id = BUF_ID(p); m.c.sink_id = cap ? HOST_ID(p) : DAI_ID(p);
    if (!NT_SUCCESS(st = IpcTx("connect 2", &m, sizeof(m.c)))) return st;

    RtlZeroMemory(&m, sizeof(m));
    FillDaiConfig(p, &m.dc);
    if (!NT_SUCCESS(st = IpcTx("dai_config", &m, sizeof(m.dc)))) return st;

    RtlZeroMemory(&m, sizeof(m));
    m.pr.hdr.size = sizeof(m.pr); m.pr.hdr.cmd = GLB_TPLG | TPLG_PIPE_COMPLETE; m.pr.comp_id = PIPE_ID(p);
    return IpcTx("pipe_complete", &m, sizeof(m.pr));
}

/* ------------------------------------------------------------------ firmware boot */
/* firmware ships in the driver package (DIRID 13 = driver store, next to jslaudio.sys): data volumes may
   not be mounted yet when the device starts during boot. Path is derived from the service ImagePath. */
static WCHAR g_fwPath[400];

static VOID InitFwPath(PUNICODE_STRING reg)
{
    OBJECT_ATTRIBUTES oa; HANDLE k; UNICODE_STRING vn; ULONG len = 0, n, i, cut = 0;
    UCHAR buf[sizeof(KEY_VALUE_PARTIAL_INFORMATION) + 700];
    PKEY_VALUE_PARTIAL_INFORMATION kv = (PKEY_VALUE_PARTIAL_INFORMATION)buf;
    RtlStringCbCopyW(g_fwPath, sizeof(g_fwPath), FW_PATH);           /* fallback */
    InitializeObjectAttributes(&oa, reg, OBJ_KERNEL_HANDLE | OBJ_CASE_INSENSITIVE, NULL, NULL);
    if (!NT_SUCCESS(ZwOpenKey(&k, KEY_READ, &oa))) return;
    RtlInitUnicodeString(&vn, L"ImagePath");
    if (NT_SUCCESS(ZwQueryValueKey(k, &vn, KeyValuePartialInformation, kv, sizeof(buf) - 2, &len)) &&
        (kv->Type == REG_SZ || kv->Type == REG_EXPAND_SZ)) {
        PWCHAR p = (PWCHAR)kv->Data;
        n = kv->DataLength / sizeof(WCHAR);
        while (n && p[n - 1] == 0) n--;
        p[n] = 0;
        for (i = 0; i < n; i++) if (p[i] == L'\\') cut = i + 1;     /* keep the trailing backslash */
        p[cut] = 0;
        if (cut) {
            g_fwPath[0] = 0;
            if (p[0] != L'\\') RtlStringCbCatW(g_fwPath, sizeof(g_fwPath), L"\\SystemRoot\\");
            RtlStringCbCatW(g_fwPath, sizeof(g_fwPath), p);
            RtlStringCbCatW(g_fwPath, sizeof(g_fwPath), L"sof-jsl.ri");
        }
    }
    ZwClose(k);
}

static NTSTATUS ReadFw(PVOID *data, ULONG *size)
{
    UNICODE_STRING n; OBJECT_ATTRIBUTES oa; IO_STATUS_BLOCK iosb; HANDLE h;
    FILE_STANDARD_INFORMATION fi; NTSTATUS st; PVOID buf;
    RtlInitUnicodeString(&n, g_fwPath);
    InitializeObjectAttributes(&oa, &n, OBJ_KERNEL_HANDLE | OBJ_CASE_INSENSITIVE, NULL, NULL);
    st = ZwCreateFile(&h, GENERIC_READ | SYNCHRONIZE, &oa, &iosb, NULL, FILE_ATTRIBUTE_NORMAL,
                      FILE_SHARE_READ, FILE_OPEN, FILE_SYNCHRONOUS_IO_NONALERT | FILE_NON_DIRECTORY_FILE, NULL, 0);
    if (!NT_SUCCESS(st)) return st;
    st = ZwQueryInformationFile(h, &iosb, &fi, sizeof(fi), FileStandardInformation);
    if (!NT_SUCCESS(st) || fi.EndOfFile.QuadPart > 0x400000) { ZwClose(h); return STATUS_UNSUCCESSFUL; }
    buf = ExAllocatePool2(POOL_FLAG_NON_PAGED, (SIZE_T)fi.EndOfFile.QuadPart, TAG);
    if (!buf) { ZwClose(h); return STATUS_INSUFFICIENT_RESOURCES; }
    st = ZwReadFile(h, NULL, NULL, NULL, &iosb, buf, (ULONG)fi.EndOfFile.QuadPart, NULL, NULL);
    ZwClose(h);
    if (!NT_SUCCESS(st)) { ExFreePool(buf); return st; }
    *data = buf; *size = (ULONG)fi.EndOfFile.QuadPart;
    return STATUS_SUCCESS;
}

static NTSTATUS BootDsp(VOID)
{
    DMABUF cl = {0}, clBdl = {0}, ic = {0}, icBdl = {0};
    PVOID fw; ULONG fwSize, payloadOff, payloadSize, clIdx, icIdx = 0, tag = 1, v, t;
    NTSTATUS st;

    if (!g_fw) {
        st = ReadFw(&g_fw, &g_fwSize);
        if (!NT_SUCCESS(st)) { Log("fw read failed %08X (%S)", st, g_fwPath); g_fw = NULL; return st; }
        if (*(PULONG)g_fw != EXTMAN_MAGIC) { ExFreePool(g_fw); g_fw = NULL; return STATUS_INVALID_IMAGE_FORMAT; }
    }
    fw = g_fw; fwSize = g_fwSize;
    payloadOff = ((PULONG)fw)[1];
    payloadSize = fwSize - payloadOff;

    if (!DmaAlloc(&cl, payloadSize) || !DmaAlloc(&clBdl, 4096) || !DmaAlloc(&ic, 4096) || !DmaAlloc(&icBdl, 4096)) {
        st = STATUS_INSUFFICIENT_RESOURCES; goto out;
    }
    RtlCopyMemory(cl.va, (PUCHAR)fw + payloadOff, payloadSize);
    clIdx = g_iss;
    if (g_ppcap) W32(g_hda, g_ppcap + PP_PPCTL, R32(g_hda, g_ppcap + PP_PPCTL) | PP_PPCTL_GPROCEN);

    /* ICCMAX capture stream (hda_dsp_cl_boot_firmware_iccmax) */
    {
        ULONG o = HDA_SD(icIdx); BDLE *b = (BDLE *)icBdl.va;
        W32(g_hda, o + SD_BDLPL, 0); W32(g_hda, o + SD_BDLPU, 0);
        b[0].lo = (ULONG)ic.pa; b[0].hi = (ULONG)(ic.pa >> 32); b[0].size = 2048; b[0].ioc = 0;
        b[1].lo = (ULONG)(ic.pa + 2048); b[1].hi = (ULONG)((ic.pa + 2048) >> 32); b[1].size = 2048; b[1].ioc = 0;
        W32(g_hda, o + SD_BDLPL, (ULONG)icBdl.pa); W32(g_hda, o + SD_BDLPU, (ULONG)(icBdl.pa >> 32));
        W32(g_hda, o + SD_CBL, 4096);
        W16(g_hda, o + SD_LVI, 1);
        if (g_ppcap) W32(g_hda, g_ppcap + PP_PPCTL, R32(g_hda, g_ppcap + PP_PPCTL) | (1u << icIdx));
        W8(g_hda, HDA_LTRP, (R8(g_hda, HDA_LTRP) & ~0x3F) | (95 & 0x3F));
        W8(g_hda, o + SD_CTL, R8(g_hda, o + SD_CTL) | SD_RUN);
    }

    if (!StreamSetup(clIdx, tag, &clBdl, cl.pa, payloadSize, CL_FORMAT)) { st = STATUS_UNSUCCESSFUL; goto cleanup; }
    W8(g_hda, HDA_SD(clIdx) + SD_CTL, R8(g_hda, HDA_SD(clIdx) + SD_CTL) | SD_INTEN);
    if (g_spbcap) {
        W32(g_hda, g_spbcap + SPB_SPBFCCTL, R32(g_hda, g_spbcap + SPB_SPBFCCTL) | (1u << clIdx));
        W32(g_hda, g_spbcap + SPB_SPIB(clIdx), payloadSize);
    }

    if (!CorePowerUp(3)) { Log("core power-up failed ADSPCS=%08X", R32(g_dsp, DSP_ADSPCS)); st = STATUS_UNSUCCESSFUL; goto cleanup; }
    for (t = 0; t < 6; t++) {
        ULONG off = DSP_SSP_BASE + t * 0x1000 + 4;
        if (off + 4 <= g_dspLen) W32(g_dsp, off, R32(g_dsp, off) | SSP_CBP_CFP);
    }
    W32(g_dsp, DSP_HIPCIDR, IPC_BUSY | ROM_IPC_CONTROL | ROM_IPC_PURGE_FW | ((tag - 1) << 9));
    if (!CoreRun(1)) { st = STATUS_UNSUCCESSFUL; goto cleanup; }
    if (!Poll(g_dsp, DSP_HIPCIDA, IPC_DONE, IPC_DONE, 500000)) { st = STATUS_IO_TIMEOUT; goto cleanup; }
    W32(g_dsp, DSP_HIPCIDA, IPC_DONE);
    CoreResetPowerDown(2);

    for (t = 0; t < 300000; t += 500) {
        if (FSR_STATE(R32(g_dsp, DSP_ROM_STATUS)) == FSR_INIT_DONE) break;
        KeStallExecutionProcessor(500);
    }
    if (FSR_STATE(R32(g_dsp, DSP_ROM_STATUS)) != FSR_INIT_DONE) { Log("ROM init timeout %08X", R32(g_dsp, DSP_ROM_STATUS)); st = STATUS_IO_TIMEOUT; goto cleanup; }

    StreamStart(clIdx);
    for (t = 0; t < 3000000; t += 500) {
        if (FSR_STATE(R32(g_dsp, DSP_ROM_STATUS)) == FSR_FW_ENTERED) break;
        KeStallExecutionProcessor(500);
    }
    v = R32(g_dsp, DSP_ROM_STATUS);
    StreamStop(clIdx);
    if (FSR_STATE(v) != FSR_FW_ENTERED) { Log("FW load failed rom=%08X", v); st = STATUS_IO_TIMEOUT; goto cleanup; }

    /* wait for FW_READY and ack it */
    for (t = 0; t < 5000000; t += 1000) {
        if (R32(g_dsp, DSP_HIPCTDR) & IPC_BUSY) break;
        KeStallExecutionProcessor(1000);
    }
    if (!(R32(g_dsp, DSP_HIPCTDR) & IPC_BUSY)) { Log("no FW_READY"); st = STATUS_IO_TIMEOUT; goto cleanup; }
    W32(g_dsp, DSP_HIPCTDR, IPC_BUSY);
    W32(g_dsp, DSP_HIPCTDA, IPC_DONE);
    st = STATUS_SUCCESS;

cleanup:
    if (g_spbcap) {
        W32(g_hda, g_spbcap + SPB_SPIB(clIdx), 0);
        W32(g_hda, g_spbcap + SPB_SPBFCCTL, R32(g_hda, g_spbcap + SPB_SPBFCCTL) & ~(1u << clIdx));
    }
    StreamClear(clIdx);
    W8(g_hda, HDA_SD(icIdx) + SD_CTL, R8(g_hda, HDA_SD(icIdx) + SD_CTL) & ~SD_RUN);
    W32(g_hda, HDA_SD(icIdx) + SD_BDLPL, 0); W32(g_hda, HDA_SD(icIdx) + SD_BDLPU, 0); W32(g_hda, HDA_SD(icIdx), 0);
    W8(g_hda, HDA_LTRP, R8(g_hda, HDA_LTRP) & ~0x3F);
    if (!NT_SUCCESS(st)) CoreResetPowerDown(3);
out:
    DmaFree(&cl); DmaFree(&clBdl); DmaFree(&ic); DmaFree(&icBdl);
    return st;
}

/* ------------------------------------------------------------------ PCI config (pre_fw_run) */
static NTSTATUS GetBusInterface(PDEVICE_OBJECT pdo, PBUS_INTERFACE_STANDARD bus)
{
    KEVENT ev; IO_STATUS_BLOCK iosb; PIRP irp; PIO_STACK_LOCATION sp; NTSTATUS st;
    PDEVICE_OBJECT top = IoGetAttachedDeviceReference(pdo);
    KeInitializeEvent(&ev, NotificationEvent, FALSE);
    irp = IoBuildSynchronousFsdRequest(IRP_MJ_PNP, top, NULL, 0, NULL, &ev, &iosb);
    if (!irp) { ObDereferenceObject(top); return STATUS_INSUFFICIENT_RESOURCES; }
    irp->IoStatus.Status = STATUS_NOT_SUPPORTED;
    sp = IoGetNextIrpStackLocation(irp);
    sp->MinorFunction = IRP_MN_QUERY_INTERFACE;
    sp->Parameters.QueryInterface.InterfaceType = &GUID_BUS_INTERFACE_STANDARD;
    sp->Parameters.QueryInterface.Size = sizeof(*bus);
    sp->Parameters.QueryInterface.Version = 1;
    sp->Parameters.QueryInterface.Interface = (PINTERFACE)bus;
    sp->Parameters.QueryInterface.InterfaceSpecificData = NULL;
    st = IoCallDriver(top, irp);
    if (st == STATUS_PENDING) { KeWaitForSingleObject(&ev, Executive, KernelMode, FALSE, NULL); st = iosb.Status; }
    ObDereferenceObject(top);
    return st;
}

/* acquired once at start (a PnP query is not safe from the power path), released in HwStop */
static VOID AcquireBus(PDEVICE_OBJECT fdo)
{
    PDEVICE_OBJECT pdo = NULL;
    if (g_busValid) return;
    if (!NT_SUCCESS(PcGetPhysicalDeviceObject(fdo, &pdo)) || !pdo) return;
    g_busValid = NT_SUCCESS(GetBusInterface(pdo, &g_bus));
    if (!g_busValid) Log("no bus interface");
}

static VOID PciPreFwRun(VOID)
{
    PBUS_INTERFACE_STANDARD b = &g_bus;
    if (!g_busValid) return;
#define bus (*b)
    USHORT cmd = 0; ULONG pg = 0, cg = 0;
    bus.GetBusData(bus.Context, PCI_WHICHSPACE_CONFIG, &cmd, 4, 2);
    if ((cmd & 6) != 6) { cmd |= 6; bus.SetBusData(bus.Context, PCI_WHICHSPACE_CONFIG, &cmd, 4, 2); }
    bus.GetBusData(bus.Context, PCI_WHICHSPACE_CONFIG, &pg, 0x44, 4);
    bus.GetBusData(bus.Context, PCI_WHICHSPACE_CONFIG, &cg, 0x48, 4);
    pg |= (1u << 2); cg &= ~(1u << 1);      /* disable DSP power/clock gating */
    bus.SetBusData(bus.Context, PCI_WHICHSPACE_CONFIG, &pg, 0x44, 4);
    bus.SetBusData(bus.Context, PCI_WHICHSPACE_CONFIG, &cg, 0x48, 4);
#undef bus
    W32(g_hda, 0x1030, R32(g_hda, 0x1030) & ~(1u << 13));   /* EM2.L1SEN off */
}

/* full bring-up: used at StartDevice and again on every D3 -> D0 */
static NTSTATUS HwStart(VOID)
{
    ULONG t, off, gcap; NTSTATUS st;
    g_dspReady = FALSE;
    PciPreFwRun();

    W32(g_hda, HDA_GCTL, R32(g_hda, HDA_GCTL) & ~1u);
    for (t = 0; t < 1000 && (R32(g_hda, HDA_GCTL) & 1); t++) KeStallExecutionProcessor(100);
    KeStallExecutionProcessor(200);
    W32(g_hda, HDA_GCTL, R32(g_hda, HDA_GCTL) | 1u);
    for (t = 0; t < 1000 && !(R32(g_hda, HDA_GCTL) & 1); t++) KeStallExecutionProcessor(100);
    KeStallExecutionProcessor(1000);

    gcap = R16(g_hda, HDA_GCAP);
    g_oss = (gcap >> 12) & 0xF; g_iss = (gcap >> 8) & 0xF;
    g_ppcap = g_spbcap = 0;
    off = R32(g_hda, HDA_LLCH) & 0xFFFF;
    for (t = 0; t < 10 && off && off + 4 <= g_hdaLen; t++) {
        ULONG cap = R32(g_hda, off), id = (cap >> 16) & 0xFFF;
        if (id == 3) g_ppcap = off; else if (id == 4) g_spbcap = off;
        off = cap & 0xFFFF;
    }
    Log("GCAP=%04X iss=%u oss=%u ppcap=%X spbcap=%X ADSPCS=%08X", gcap, g_iss, g_oss, g_ppcap, g_spbcap, R32(g_dsp, DSP_ADSPCS));
    if (R32(g_dsp, DSP_ADSPCS) & 0x0F000000) {      /* cores still powered (previous instance / restart) */
        CoreResetPowerDown(0xF);
        Log("powered down leftover DSP cores: ADSPCS=%08X", R32(g_dsp, DSP_ADSPCS));
    }

    g_paths[PATH_SPK].hdaIdx  = g_iss + 1;      /* 2nd output stream (1st is the code loader) */
    g_paths[PATH_DMIC].hdaIdx = 1;              /* 2nd input stream (1st is used by the boot ICCMAX stream) */
    g_paths[PATH_HP].hdaIdx   = g_iss + 2;      /* 3rd output stream */
    for (t = 0; t < NPATHS; t++) g_paths[t].ready = FALSE;

    st = BootDsp();
    Log("BootDsp -> %08X", st);
    if (!NT_SUCCESS(st)) return st;
    st = BuildPipeline(&g_paths[PATH_SPK]);     /* the speaker is mandatory ... */
    Log("BuildPipeline speaker -> %08X", st);
    if (!NT_SUCCESS(st)) return st;
    g_paths[PATH_SPK].ready = TRUE;
    for (t = PATH_SPK + 1; t < NPATHS; t++) {   /* ... the other paths are optional */
        NTSTATUS ps = BuildPipeline(&g_paths[t]);
        g_paths[t].ready = NT_SUCCESS(ps);
        Log("BuildPipeline %s -> %08X", g_paths[t].name, ps);
    }
    g_dspReady = TRUE;
    return STATUS_SUCCESS;
}

/* ------------------------------------------------------------------ formats / descriptors */
#define RATE 48000
#define CHANNELS 2
#define BITS 32
#define FRAME_BYTES (CHANNELS * BITS / 8)
#define HOST_STREAM_FMT 0x0041          /* HDA SDnFMT: 48 kHz, 32-bit, 2 ch */

static KSDATARANGE_AUDIO g_pinRange = {
    { sizeof(KSDATARANGE_AUDIO), 0, 0, 0,
      STATICGUIDOF(KSDATAFORMAT_TYPE_AUDIO), STATICGUIDOF(KSDATAFORMAT_SUBTYPE_PCM), STATICGUIDOF(KSDATAFORMAT_SPECIFIER_WAVEFORMATEX) },
    CHANNELS, BITS, BITS, RATE, RATE
};
static PKSDATARANGE g_pinRanges[] = { (PKSDATARANGE)&g_pinRange };

static KSDATARANGE g_bridgeRange = {
    sizeof(KSDATARANGE), 0, 0, 0,
    STATICGUIDOF(KSDATAFORMAT_TYPE_AUDIO), STATICGUIDOF(KSDATAFORMAT_SUBTYPE_ANALOG), STATICGUIDOF(KSDATAFORMAT_SPECIFIER_NONE)
};
static PKSDATARANGE g_bridgeRanges[] = { &g_bridgeRange };

static PCPIN_DESCRIPTOR g_wavePins[] = {
    { 1, 1, 0, NULL, { 0, NULL, 0, NULL, 1, g_pinRanges, KSPIN_DATAFLOW_IN, KSPIN_COMMUNICATION_SINK, &KSCATEGORY_AUDIO, NULL, 0 } },
    { 0, 0, 0, NULL, { 0, NULL, 0, NULL, 1, g_bridgeRanges, KSPIN_DATAFLOW_OUT, KSPIN_COMMUNICATION_NONE, &KSCATEGORY_AUDIO, NULL, 0 } },
};
static PCCONNECTION_DESCRIPTOR g_waveConn[] = { { PCFILTER_NODE, 0, PCFILTER_NODE, 1 } };
static PCFILTER_DESCRIPTOR g_waveFilter = {
    0, NULL, sizeof(PCPIN_DESCRIPTOR), 2, g_wavePins, sizeof(PCNODE_DESCRIPTOR), 0, NULL, 1, g_waveConn, 0, NULL
};

static PCPIN_DESCRIPTOR g_topoPins[] = {
    { 0, 0, 0, NULL, { 0, NULL, 0, NULL, 1, g_bridgeRanges, KSPIN_DATAFLOW_IN, KSPIN_COMMUNICATION_NONE, &KSCATEGORY_AUDIO, NULL, 0 } },
    { 0, 0, 0, NULL, { 0, NULL, 0, NULL, 1, g_bridgeRanges, KSPIN_DATAFLOW_OUT, KSPIN_COMMUNICATION_NONE, &KSNODETYPE_SPEAKER, NULL, 0 } },
};
static PCCONNECTION_DESCRIPTOR g_topoConn[] = { { PCFILTER_NODE, 0, PCFILTER_NODE, 1 } };
static PCFILTER_DESCRIPTOR g_topoFilter = {
    0, NULL, sizeof(PCPIN_DESCRIPTOR), 2, g_topoPins, sizeof(PCNODE_DESCRIPTOR), 0, NULL, 1, g_topoConn, 0, NULL
};

/* capture (internal DMIC): topology  mic pin 0 (in) -> bridge pin 1 (out)  ==>  wave bridge pin 0 (in) -> streaming pin 1 (out) */
static PCPIN_DESCRIPTOR g_waveCapPins[] = {
    { 0, 0, 0, NULL, { 0, NULL, 0, NULL, 1, g_bridgeRanges, KSPIN_DATAFLOW_IN, KSPIN_COMMUNICATION_NONE, &KSCATEGORY_AUDIO, NULL, 0 } },
    { 1, 1, 0, NULL, { 0, NULL, 0, NULL, 1, g_pinRanges, KSPIN_DATAFLOW_OUT, KSPIN_COMMUNICATION_SINK, &KSCATEGORY_AUDIO, NULL, 0 } },
};
static PCFILTER_DESCRIPTOR g_waveCapFilter = {
    0, NULL, sizeof(PCPIN_DESCRIPTOR), 2, g_waveCapPins, sizeof(PCNODE_DESCRIPTOR), 0, NULL, 1, g_topoConn, 0, NULL
};
static PCPIN_DESCRIPTOR g_topoMicPins[] = {
    { 0, 0, 0, NULL, { 0, NULL, 0, NULL, 1, g_bridgeRanges, KSPIN_DATAFLOW_IN, KSPIN_COMMUNICATION_NONE, &KSNODETYPE_MICROPHONE, NULL, 0 } },
    { 0, 0, 0, NULL, { 0, NULL, 0, NULL, 1, g_bridgeRanges, KSPIN_DATAFLOW_OUT, KSPIN_COMMUNICATION_NONE, &KSCATEGORY_AUDIO, NULL, 0 } },
};
static PCFILTER_DESCRIPTOR g_topoMicFilter = {
    0, NULL, sizeof(PCPIN_DESCRIPTOR), 2, g_topoMicPins, sizeof(PCNODE_DESCRIPTOR), 0, NULL, 1, g_topoConn, 0, NULL
};
/* headphones: same shape as the speaker topology, endpoint pin category = headphones */
static PCPIN_DESCRIPTOR g_topoHpPins[] = {
    { 0, 0, 0, NULL, { 0, NULL, 0, NULL, 1, g_bridgeRanges, KSPIN_DATAFLOW_IN, KSPIN_COMMUNICATION_NONE, &KSCATEGORY_AUDIO, NULL, 0 } },
    { 0, 0, 0, NULL, { 0, NULL, 0, NULL, 1, g_bridgeRanges, KSPIN_DATAFLOW_OUT, KSPIN_COMMUNICATION_NONE, &KSNODETYPE_HEADPHONES, NULL, 0 } },
};
static PCFILTER_DESCRIPTOR g_topoHpFilter = {
    0, NULL, sizeof(PCPIN_DESCRIPTOR), 2, g_topoHpPins, sizeof(PCNODE_DESCRIPTOR), 0, NULL, 1, g_topoConn, 0, NULL
};
#define STREAM_PIN(path) (g_paths[path].dir == DIR_CAPTURE ? 1u : 0u)

static BOOLEAN FormatSupported(PKSDATAFORMAT f)
{
    if (!IsEqualGUIDAligned(f->MajorFormat, KSDATAFORMAT_TYPE_AUDIO) ||
        !IsEqualGUIDAligned(f->Specifier, KSDATAFORMAT_SPECIFIER_WAVEFORMATEX) ||
        f->FormatSize < sizeof(KSDATAFORMAT_WAVEFORMATEX)) return FALSE;
    PWAVEFORMATEX w = &((PKSDATAFORMAT_WAVEFORMATEX)f)->WaveFormatEx;
    if (w->nChannels != CHANNELS || w->nSamplesPerSec != RATE || w->wBitsPerSample != BITS) return FALSE;
    if (w->wFormatTag == WAVE_FORMAT_PCM) return TRUE;
    if (w->wFormatTag == WAVE_FORMAT_EXTENSIBLE && f->FormatSize >= sizeof(KSDATAFORMAT_WAVEFORMATEXTENSIBLE)) {
        PWAVEFORMATEXTENSIBLE x = (PWAVEFORMATEXTENSIBLE)w;
        return IsEqualGUIDAligned(x->SubFormat, KSDATAFORMAT_SUBTYPE_PCM) && x->Samples.wValidBitsPerSample >= 24;
    }
    return FALSE;
}

/* ------------------------------------------------------------------ WaveRT stream */
class CStream : public IMiniportWaveRTStream, public CUnknown
{
public:
    DECLARE_STD_UNKNOWN();
    DEFINE_STD_CONSTRUCTOR(CStream);
    ~CStream();
    IMP_IMiniportWaveRTStream;

    NTSTATUS Init(PPORTWAVERTSTREAM port, ULONG path)
    {
        m_path = &g_paths[path]; m_pathIdx = path;
        m_port = port; m_port->AddRef();
        return DmaAlloc(&m_bdl, 4096) ? STATUS_SUCCESS : STATUS_INSUFFICIENT_RESOURCES;
    }
    VOID     PowerDown();          /* g_ipcLock held: DSP is about to lose power */
    VOID     PowerUp();            /* g_ipcLock held: DSP freshly rebooted, replay m_state */

private:
    AUDIOPATH *m_path;
    ULONG    m_pathIdx;
    PPORTWAVERTSTREAM m_port;
    PMDL     m_mdl;
    ULONG    m_size;
    ULONG64  m_pa;
    DMABUF   m_bdl;
    KSSTATE  m_state;              /* state requested by PortCls (kept across D3) */
    BOOLEAN  m_prepared, m_started, m_paused;   /* what the DSP/HDA currently has */
    VOID     Teardown();
    NTSTATUS Prepare();
    NTSTATUS Start();
};

static CStream *g_active[NPATHS];   /* one stream per path (pin max instances = 1), under g_ipcLock */

static VOID IpcLock(VOID)   { KeWaitForSingleObject(&g_ipcLock, Executive, KernelMode, FALSE, NULL); }
static VOID IpcUnlock(VOID) { KeReleaseMutex(&g_ipcLock, FALSE); }

STDMETHODIMP CStream::NonDelegatingQueryInterface(REFIID riid, PVOID *obj)
{
    if (IsEqualGUIDAligned(riid, IID_IUnknown)) *obj = PVOID(PUNKNOWN(PMINIPORTWAVERTSTREAM(this)));
    else if (IsEqualGUIDAligned(riid, IID_IMiniportWaveRTStream)) *obj = PVOID(PMINIPORTWAVERTSTREAM(this));
    else { *obj = NULL; return STATUS_INVALID_PARAMETER; }
    PUNKNOWN(*obj)->AddRef();
    return STATUS_SUCCESS;
}

VOID CStream::Teardown()
{
    IpcLock();
    if (m_started && g_dspReady) {           /* Linux STOP: IPC first, then host DMA */
        IpcStream(m_path, "trigger stop", STREAM_TRIG_STOP);
        StreamStop(m_path->hdaIdx);
        if (m_path->amp) AmpEnable(FALSE);
    }
    if (m_prepared && g_dspReady) {
        IpcStream(m_path, "pcm_free", STREAM_PCM_FREE);
        StreamClear(m_path->hdaIdx);
    }
    m_started = m_paused = m_prepared = FALSE;
    IpcUnlock();
}

CStream::~CStream()
{
    Teardown();
    IpcLock(); if (m_path && g_active[m_pathIdx] == this) g_active[m_pathIdx] = NULL; IpcUnlock();
    DmaFree(&m_bdl);
    if (m_port) m_port->Release();
}

/* STOP -> ACQUIRE: host stream + pcm_params (lock held) */
NTSTATUS CStream::Prepare()
{
    IPCMSG m; NTSTATUS st;
    if (m_prepared || !m_mdl) return STATUS_SUCCESS;
    if (!m_path->ready) return STATUS_DEVICE_NOT_READY;
    if (!StreamSetup(m_path->hdaIdx, m_path->tag, &m_bdl, m_pa, m_size, HOST_STREAM_FMT)) return STATUS_UNSUCCESSFUL;
    RtlZeroMemory(&m, sizeof(m));
    m.pp.hdr.size = sizeof(m.pp); m.pp.hdr.cmd = GLB_STREAM | STREAM_PCM_PARAMS;
    m.pp.comp_id = HOST_ID(m_path);
    m.pp.p_size = 84;
    m.pp.b_size = 28; m.pp.phy_addr = (ULONG)m_pa; m.pp.pages = (m_size + PAGE_SIZE - 1) / PAGE_SIZE; m.pp.b_bytes = m_size;
    m.pp.direction = m_path->dir; m.pp.frame_fmt = FRAME_S32_LE; m.pp.rate = RATE;
    m.pp.stream_tag = (USHORT)m_path->tag; m.pp.channels = CHANNELS; m.pp.sample_valid_bytes = 4; m.pp.sample_container_bytes = 4;
    m.pp.host_period_bytes = m_size / 2; m.pp.no_stream_position = 1;
    m.pp.chmap[0] = 0; m.pp.chmap[1] = 1;
    st = IpcTx("pcm_params", &m, sizeof(m.pp));
    if (!NT_SUCCESS(st)) { StreamClear(m_path->hdaIdx); return st; }
    m_prepared = TRUE;
    return STATUS_SUCCESS;
}

/* -> RUN (lock held): host DMA first, then IPC (sof_pcm_trigger order) */
NTSTATUS CStream::Start()
{
    NTSTATUS st;
    if (!m_prepared || (m_started && !m_paused)) return STATUS_SUCCESS;
    if (m_path->amp) AmpEnable(TRUE);
    StreamStart(m_path->hdaIdx);
    st = IpcStream(m_path, m_started ? "trigger release" : "trigger start", m_started ? STREAM_TRIG_RELEASE : STREAM_TRIG_START);
    if (NT_SUCCESS(st)) { m_started = TRUE; m_paused = FALSE; }
    else { StreamStop(m_path->hdaIdx); if (m_path->amp) AmpEnable(FALSE); }
    return st;
}

VOID CStream::PowerDown()
{
    if (m_started && !m_paused) StreamStop(m_path->hdaIdx);   /* stop bus mastering; DSP state is lost anyway */
    if (m_path->amp) AmpEnable(FALSE);
    m_started = m_paused = m_prepared = FALSE;
}

VOID CStream::PowerUp()
{
    if (m_state == KSSTATE_STOP) return;
    if (NT_SUCCESS(Prepare()) && m_state == KSSTATE_RUN) Start();   /* a paused stream resumes later via TRIG_START */
}

STDMETHODIMP_(NTSTATUS) CStream::SetFormat(PKSDATAFORMAT f)
{
    return FormatSupported(f) ? STATUS_SUCCESS : STATUS_INVALID_PARAMETER;   /* only one format exists */
}

STDMETHODIMP_(NTSTATUS) CStream::AllocateAudioBuffer(ULONG req, PMDL *mdl, ULONG *actual, ULONG *offset, MEMORY_CACHING_TYPE *cache)
{
    PHYSICAL_ADDRESS lo, hi;
    ULONG size = (req + 255) & ~255u;                    /* two BDL halves, each a multiple of 128 bytes */
    if (size < 512) size = 512;
    lo.QuadPart = 0; hi.QuadPart = 0x0000FFFFFFFFFFFFLL;
    PMDL p = m_port->AllocateContiguousPagesForMdl(lo, hi, size);
    if (!p) return STATUS_INSUFFICIENT_RESOURCES;
    m_mdl = p; m_size = size;
    m_pa = (ULONG64)m_port->GetPhysicalPageAddress(p, 0).QuadPart;
    /* HDA host DMA snoops (boot2 played correctly from cached common buffers), so cached is safe
       and avoids mixing cache attributes on the same pages */
    *mdl = p; *actual = size; *offset = 0; *cache = MmCached;
    Log("AllocateAudioBuffer req=%u -> %u pa=%llX", req, size, m_pa);
    return STATUS_SUCCESS;
}

STDMETHODIMP_(VOID) CStream::FreeAudioBuffer(PMDL mdl, ULONG)
{
    Teardown();
    if (mdl) m_port->FreePagesFromMdl(mdl);
    m_mdl = NULL; m_size = 0;
}

STDMETHODIMP_(VOID) CStream::GetHWLatency(KSRTAUDIO_HWLATENCY *l)
{
    l->FifoSize = 3072;      /* DSP buffer component */
    l->ChipsetDelay = 0;
    l->CodecDelay = 0;
}

STDMETHODIMP_(NTSTATUS) CStream::GetPositionRegister(KSRTAUDIO_HWREGISTER *) { return STATUS_NOT_SUPPORTED; }
STDMETHODIMP_(NTSTATUS) CStream::GetClockRegister(KSRTAUDIO_HWREGISTER *) { return STATUS_NOT_SUPPORTED; }

STDMETHODIMP_(NTSTATUS) CStream::GetPosition(PKSAUDIO_POSITION pos)
{
    ULONG lpib = (m_prepared && g_dspReady && g_hda) ? R32(g_hda, HDA_SD(m_path->hdaIdx) + SD_LPIB) : 0;
    if (m_size && lpib >= m_size) lpib %= m_size;
    pos->PlayOffset = lpib;
    pos->WriteOffset = lpib;
    return STATUS_SUCCESS;
}

STDMETHODIMP_(NTSTATUS) CStream::SetState(KSSTATE s)
{
    NTSTATUS st = STATUS_SUCCESS;

    if (g_resumeCount) { Log("resumed %u time(s), last HwStart -> %08X", g_resumeCount, g_resumeStatus); g_resumeCount = 0; }
    if (s == KSSTATE_STOP) { Teardown(); m_state = s; Log("state STOP"); return STATUS_SUCCESS; }

    IpcLock();
    if (!g_dspReady) {                              /* device in D3: remember, PowerUp() replays it */
        m_state = s;
        IpcUnlock();
        return STATUS_SUCCESS;
    }
    st = Prepare();
    if (NT_SUCCESS(st) && s == KSSTATE_RUN) st = Start();
    else if (NT_SUCCESS(st) && s == KSSTATE_PAUSE && m_started && !m_paused) {
        st = IpcStream(m_path, "trigger pause", STREAM_TRIG_PAUSE);   /* PAUSE_PUSH: IPC first, then DMA */
        StreamStop(m_path->hdaIdx);
        m_paused = TRUE;
    }
    if (NT_SUCCESS(st)) m_state = s;
    IpcUnlock();
    if (s != KSSTATE_ACQUIRE || !NT_SUCCESS(st))
        Log("state -> %u: %08X (size=%u)", s, st, m_size);
    return st;
}

/* ------------------------------------------------------------------ WaveRT miniport */
class CMiniportWave : public IMiniportWaveRT, public CUnknown
{
public:
    DECLARE_STD_UNKNOWN();
    DEFINE_STD_CONSTRUCTOR(CMiniportWave);
    ~CMiniportWave() { if (m_port) m_port->Release(); }
    IMP_IMiniportWaveRT;
    ULONG m_pathIdx;               /* which AUDIOPATH this filter exposes */
private:
    PPORTWAVERT m_port;
};

STDMETHODIMP CMiniportWave::NonDelegatingQueryInterface(REFIID riid, PVOID *obj)
{
    if (IsEqualGUIDAligned(riid, IID_IUnknown)) *obj = PVOID(PUNKNOWN(PMINIPORTWAVERT(this)));
    else if (IsEqualGUIDAligned(riid, IID_IMiniport)) *obj = PVOID(PMINIPORT(this));
    else if (IsEqualGUIDAligned(riid, IID_IMiniportWaveRT)) *obj = PVOID(PMINIPORTWAVERT(this));
    else { *obj = NULL; return STATUS_INVALID_PARAMETER; }
    PUNKNOWN(*obj)->AddRef();
    return STATUS_SUCCESS;
}

STDMETHODIMP_(NTSTATUS) CMiniportWave::Init(PUNKNOWN, PRESOURCELIST, PPORTWAVERT port)
{
    m_port = port; m_port->AddRef();
    return STATUS_SUCCESS;
}

STDMETHODIMP_(NTSTATUS) CMiniportWave::GetDescription(PPCFILTER_DESCRIPTOR *d)
{
    *d = (g_paths[m_pathIdx].dir == DIR_CAPTURE) ? &g_waveCapFilter : &g_waveFilter;
    return STATUS_SUCCESS;
}

STDMETHODIMP_(NTSTATUS) CMiniportWave::DataRangeIntersection(ULONG pin, PKSDATARANGE range, PKSDATARANGE, ULONG outLen, PVOID result, PULONG resultLen)
{
    if (pin != STREAM_PIN(m_pathIdx)) return STATUS_NOT_IMPLEMENTED;
    if (!IsEqualGUIDAligned(range->MajorFormat, KSDATAFORMAT_TYPE_AUDIO) && !IsEqualGUIDAligned(range->MajorFormat, KSDATAFORMAT_TYPE_WILDCARD))
        return STATUS_NO_MATCH;
    if (range->FormatSize >= sizeof(KSDATARANGE_AUDIO) && IsEqualGUIDAligned(range->Specifier, KSDATAFORMAT_SPECIFIER_WAVEFORMATEX)) {
        PKSDATARANGE_AUDIO a = (PKSDATARANGE_AUDIO)range;
        if (a->MaximumChannels < CHANNELS || a->MinimumBitsPerSample > BITS || a->MaximumBitsPerSample < BITS ||
            a->MinimumSampleFrequency > RATE || a->MaximumSampleFrequency < RATE) return STATUS_NO_MATCH;
    }
    *resultLen = sizeof(KSDATAFORMAT_WAVEFORMATEXTENSIBLE);
    if (outLen == 0) return STATUS_BUFFER_OVERFLOW;
    if (outLen < sizeof(KSDATAFORMAT_WAVEFORMATEXTENSIBLE)) return STATUS_BUFFER_TOO_SMALL;
    PKSDATAFORMAT_WAVEFORMATEXTENSIBLE r = (PKSDATAFORMAT_WAVEFORMATEXTENSIBLE)result;
    RtlZeroMemory(r, sizeof(*r));
    r->DataFormat.FormatSize = sizeof(*r);
    r->DataFormat.SampleSize = FRAME_BYTES;
    r->DataFormat.MajorFormat = KSDATAFORMAT_TYPE_AUDIO;
    r->DataFormat.SubFormat = KSDATAFORMAT_SUBTYPE_PCM;
    r->DataFormat.Specifier = KSDATAFORMAT_SPECIFIER_WAVEFORMATEX;
    r->WaveFormatExt.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    r->WaveFormatExt.Format.nChannels = CHANNELS;
    r->WaveFormatExt.Format.nSamplesPerSec = RATE;
    r->WaveFormatExt.Format.wBitsPerSample = BITS;
    r->WaveFormatExt.Format.nBlockAlign = FRAME_BYTES;
    r->WaveFormatExt.Format.nAvgBytesPerSec = RATE * FRAME_BYTES;
    r->WaveFormatExt.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
    r->WaveFormatExt.Samples.wValidBitsPerSample = BITS;
    r->WaveFormatExt.dwChannelMask = KSAUDIO_SPEAKER_STEREO;
    r->WaveFormatExt.SubFormat = KSDATAFORMAT_SUBTYPE_PCM;
    return STATUS_SUCCESS;
}

STDMETHODIMP_(NTSTATUS) CMiniportWave::GetDeviceDescription(PDEVICE_DESCRIPTION d)
{
    RtlZeroMemory(d, sizeof(*d));
    d->Version = DEVICE_DESCRIPTION_VERSION;
    d->Master = TRUE;
    d->ScatterGather = TRUE;
    d->Dma64BitAddresses = TRUE;
    d->InterfaceType = PCIBus;
    d->MaximumLength = 0xFFFFFFFF;
    return STATUS_SUCCESS;
}

STDMETHODIMP_(NTSTATUS) CMiniportWave::NewStream(PMINIPORTWAVERTSTREAM *out, PPORTWAVERTSTREAM portStream, ULONG pin, BOOLEAN capture, PKSDATAFORMAT fmt)
{
    AUDIOPATH *p = &g_paths[m_pathIdx];
    if (pin != STREAM_PIN(m_pathIdx) || capture != (p->dir == DIR_CAPTURE)) return STATUS_INVALID_PARAMETER;
    if (!FormatSupported(fmt)) { Log("NewStream %s: unsupported format", p->name); return STATUS_INVALID_PARAMETER; }
    if (!g_dspReady || !p->ready) return STATUS_DEVICE_NOT_READY;
    CStream *s = new (POOL_FLAG_NON_PAGED, TAG) CStream(NULL);
    if (!s) return STATUS_INSUFFICIENT_RESOURCES;      /* ExAllocatePool2 zero-fills, so members start at 0 */
    s->AddRef();
    NTSTATUS st = s->Init(portStream, m_pathIdx);
    if (!NT_SUCCESS(st)) { s->Release(); return st; }
    *out = PMINIPORTWAVERTSTREAM(s);
    IpcLock(); g_active[m_pathIdx] = s; IpcUnlock();
    Log("NewStream %s ok", p->name);
    return STATUS_SUCCESS;
}

/* ------------------------------------------------------------------ topology miniport */
class CMiniportTopo : public IMiniportTopology, public CUnknown
{
public:
    DECLARE_STD_UNKNOWN();
    DEFINE_STD_CONSTRUCTOR(CMiniportTopo);
    IMP_IMiniportTopology;
    ULONG m_pathIdx;
};

STDMETHODIMP CMiniportTopo::NonDelegatingQueryInterface(REFIID riid, PVOID *obj)
{
    if (IsEqualGUIDAligned(riid, IID_IUnknown)) *obj = PVOID(PUNKNOWN(PMINIPORTTOPOLOGY(this)));
    else if (IsEqualGUIDAligned(riid, IID_IMiniport)) *obj = PVOID(PMINIPORT(this));
    else if (IsEqualGUIDAligned(riid, IID_IMiniportTopology)) *obj = PVOID(PMINIPORTTOPOLOGY(this));
    else { *obj = NULL; return STATUS_INVALID_PARAMETER; }
    PUNKNOWN(*obj)->AddRef();
    return STATUS_SUCCESS;
}
STDMETHODIMP_(NTSTATUS) CMiniportTopo::Init(PUNKNOWN, PRESOURCELIST, PPORTTOPOLOGY) { return STATUS_SUCCESS; }
STDMETHODIMP_(NTSTATUS) CMiniportTopo::GetDescription(PPCFILTER_DESCRIPTOR *d)
{
    *d = (g_paths[m_pathIdx].dir == DIR_CAPTURE) ? &g_topoMicFilter : (m_pathIdx == PATH_HP) ? &g_topoHpFilter : &g_topoFilter;
    return STATUS_SUCCESS;
}
STDMETHODIMP_(NTSTATUS) CMiniportTopo::DataRangeIntersection(ULONG, PKSDATARANGE, PKSDATARANGE, ULONG, PVOID, PULONG) { return STATUS_NOT_IMPLEMENTED; }

/* ------------------------------------------------------------------ power management */
/* D3: stop host DMA, amp off, DSP cores off.  D0: full re-bring-up (cached FW) and replay the stream state.
   No file logging in here (g_inPower); results are logged from the next SetState. */
class CAdapterPower : public IAdapterPowerManagement, public CUnknown
{
public:
    DECLARE_STD_UNKNOWN();
    DEFINE_STD_CONSTRUCTOR(CAdapterPower);
    IMP_IAdapterPowerManagement;
};

STDMETHODIMP CAdapterPower::NonDelegatingQueryInterface(REFIID riid, PVOID *obj)
{
    if (IsEqualGUIDAligned(riid, IID_IUnknown)) *obj = PVOID(PUNKNOWN(PADAPTERPOWERMANAGEMENT(this)));
    else if (IsEqualGUIDAligned(riid, IID_IAdapterPowerManagement)) *obj = PVOID(PADAPTERPOWERMANAGEMENT(this));
    else { *obj = NULL; return STATUS_INVALID_PARAMETER; }
    PUNKNOWN(*obj)->AddRef();
    return STATUS_SUCCESS;
}

STDMETHODIMP_(void) CAdapterPower::PowerChangeState(POWER_STATE ns)
{
    DEVICE_POWER_STATE d = ns.DeviceState;
    if (d == g_dstate || !g_dsp || !g_hda) return;
    IpcLock();
    g_inPower = TRUE;
    if (d == PowerDeviceD0) {
        g_resumeStatus = HwStart();
        g_resumeCount++;
        if (NT_SUCCESS(g_resumeStatus))
            for (ULONG i = 0; i < NPATHS; i++) if (g_active[i]) g_active[i]->PowerUp();
    } else if (g_dstate == PowerDeviceD0) {
        for (ULONG i = 0; i < NPATHS; i++) if (g_active[i]) g_active[i]->PowerDown();
        AmpEnable(FALSE);
        g_dspReady = FALSE;
        CoreResetPowerDown(0xF);
    }
    g_dstate = d;
    g_inPower = FALSE;
    IpcUnlock();
}

STDMETHODIMP_(NTSTATUS) CAdapterPower::QueryPowerChangeState(POWER_STATE) { return STATUS_SUCCESS; }
STDMETHODIMP_(NTSTATUS) CAdapterPower::QueryDeviceCapabilities(PDEVICE_CAPABILITIES) { return STATUS_SUCCESS; }

/* ------------------------------------------------------------------ adapter */
static NTSTATUS InstallSubdevice(PDEVICE_OBJECT dev, PIRP irp, PWSTR name, REFGUID portClass, PUNKNOWN miniport, PRESOURCELIST res, PUNKNOWN *outPort)
{
    PPORT port = NULL;
    NTSTATUS st = PcNewPort(&port, portClass);
    if (NT_SUCCESS(st)) st = port->Init(dev, irp, miniport, NULL, res);
    if (NT_SUCCESS(st)) st = PcRegisterSubdevice(dev, name, port);
    if (NT_SUCCESS(st)) { *outPort = port; port->AddRef(); }
    if (port) port->Release();
    Log("InstallSubdevice %S -> %08X", name, st);
    return st;
}

static NTSTATUS StartDevice(PDEVICE_OBJECT dev, PIRP irp, PRESOURCELIST res)
{
    NTSTATUS st; ULONG i, mem = 0;

    LogOpen();
    Log("=== jslaudio start ===");
    KeInitializeMutex(&g_ipcLock, 0);
    for (i = 0; i < res->NumberOfEntries(); i++) {
        PCM_PARTIAL_RESOURCE_DESCRIPTOR d = res->FindTranslatedEntry(CmResourceTypeMemory, i);
        if (!d) break;
        if (mem == 0) { g_hdaLen = d->u.Memory.Length; g_hda = (PUCHAR)MmMapIoSpaceEx(d->u.Memory.Start, g_hdaLen, PAGE_READWRITE | PAGE_NOCACHE); }
        else if (mem == 1) { g_dspLen = d->u.Memory.Length; g_dsp = (PUCHAR)MmMapIoSpaceEx(d->u.Memory.Start, g_dspLen, PAGE_READWRITE | PAGE_NOCACHE); }
        mem++;
    }
    if (!g_hda || !g_dsp) { Log("BAR map failed"); return STATUS_DEVICE_CONFIGURATION_ERROR; }

    AcquireBus(dev);
    g_dstate = PowerDeviceD0;
    st = HwStart();
    if (!NT_SUCCESS(st)) return st;

    {
        CAdapterPower *pm = new (POOL_FLAG_NON_PAGED, TAG) CAdapterPower(NULL);
        if (pm) {
            pm->AddRef();
            Log("PcRegisterAdapterPowerManagement -> %08X", PcRegisterAdapterPowerManagement(PUNKNOWN(PADAPTERPOWERMANAGEMENT(pm)), dev));
            pm->Release();          /* PortCls keeps its own reference */
        }
    }

    /* one Wave + Topology filter pair per path; names must match the INF (KSNAME_*) */
    static const struct { ULONG path; PCWSTR wave, topo; } ep[] = {
        { PATH_SPK,  L"Wave",    L"Topology" },
        { PATH_DMIC, L"WaveMic", L"TopologyMic" },
        { PATH_HP,   L"WaveHP",  L"TopologyHP" },
    };
    for (i = 0; i < ARRAYSIZE(ep); i++) {
        PUNKNOWN wavePort = NULL, topoPort = NULL;
        NTSTATUS es;
        CMiniportWave *w = new (POOL_FLAG_NON_PAGED, TAG) CMiniportWave(NULL);
        CMiniportTopo *t = new (POOL_FLAG_NON_PAGED, TAG) CMiniportTopo(NULL);
        if (!w || !t) { if (w) delete w; if (t) delete t; es = STATUS_INSUFFICIENT_RESOURCES; goto done; }
        w->m_pathIdx = t->m_pathIdx = ep[i].path;
        w->AddRef(); t->AddRef();
        es = InstallSubdevice(dev, irp, (PWSTR)ep[i].topo, CLSID_PortTopology, PUNKNOWN(PMINIPORTTOPOLOGY(t)), res, &topoPort);
        if (NT_SUCCESS(es)) es = InstallSubdevice(dev, irp, (PWSTR)ep[i].wave, CLSID_PortWaveRT, PUNKNOWN(PMINIPORTWAVERT(w)), res, &wavePort);
        if (NT_SUCCESS(es)) {
            if (g_paths[ep[i].path].dir == DIR_CAPTURE) es = PcRegisterPhysicalConnection(dev, topoPort, 1, wavePort, 0);
            else                                        es = PcRegisterPhysicalConnection(dev, wavePort, 1, topoPort, 0);
        }
        w->Release(); t->Release();
        if (wavePort) wavePort->Release();
        if (topoPort) topoPort->Release();
    done:
        Log("endpoint %s -> %08X", g_paths[ep[i].path].name, es);
        if (i == 0) st = es;                 /* only the speaker endpoint is fatal */
    }
    Log("StartDevice -> %08X", st);
    return st;
}

static NTSTATUS AddDevice(PDRIVER_OBJECT drv, PDEVICE_OBJECT pdo)
{
    return PcAddAdapterDevice(drv, pdo, (PCPFNSTARTDEVICE)StartDevice, 8, 0);
}

/* hardware teardown after PortCls has stopped/removed the subdevices (streams are torn down by then) */
static VOID HwStop(const char *why)
{
    g_dspReady = FALSE;
    AmpEnable(FALSE);
    if (g_dsp) {
        CoreResetPowerDown(0xF);
        Log("%s: DSP powered down ADSPCS=%08X", why, R32(g_dsp, DSP_ADSPCS));
        MmUnmapIoSpace(g_dsp, g_dspLen); g_dsp = NULL;
    }
    if (g_hda) { MmUnmapIoSpace(g_hda, g_hdaLen); g_hda = NULL; }
    if (g_busValid) { g_bus.InterfaceDereference(g_bus.Context); g_busValid = FALSE; }
    if (g_log) { ZwClose(g_log); g_log = NULL; }
}

static PDRIVER_DISPATCH g_pcPnp;

static NTSTATUS PnpDispatch(PDEVICE_OBJECT dev, PIRP irp)
{
    UCHAR minor = IoGetCurrentIrpStackLocation(irp)->MinorFunction;
    NTSTATUS st = g_pcPnp(dev, irp);            /* irp is gone after this */
    if (minor == IRP_MN_STOP_DEVICE) HwStop("stop");
    else if (minor == IRP_MN_SURPRISE_REMOVAL) HwStop("surprise remove");
    else if (minor == IRP_MN_REMOVE_DEVICE) {
        HwStop("remove");
        if (g_fw) { ExFreePool(g_fw); g_fw = NULL; }
    }
    return st;
}

extern "C" DRIVER_INITIALIZE DriverEntry;
extern "C" NTSTATUS DriverEntry(PDRIVER_OBJECT drv, PUNICODE_STRING reg)
{
    RtlStringCbCopyNW(g_regPath, sizeof(g_regPath), reg->Buffer, reg->Length);
    InitFwPath(reg);
    NTSTATUS st = PcInitializeAdapterDriver(drv, reg, (PDRIVER_ADD_DEVICE)AddDevice);
    if (NT_SUCCESS(st)) {
        g_pcPnp = drv->MajorFunction[IRP_MJ_PNP];
        drv->MajorFunction[IRP_MJ_PNP] = PnpDispatch;
    }
    return st;
}
