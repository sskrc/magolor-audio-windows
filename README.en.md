# magolor-audio-windows

[日本語](README.md) | **English**

An unofficial, home-made audio driver that makes the internal speakers, microphones and headphone jack of the **Acer Chromebook R753T (board "Magolor", Intel Jasper Lake)** work on Windows.

The headphone jack (RT5682 codec) additionally needs the separate GPL-2.0 driver [sskrc/magolor-rt5682-windows](https://github.com/sskrc/magolor-rt5682-windows) (`jslcodec`).

## About AI authorship

The driver, scripts and documentation in this repository were **written by an AI (Anthropic's Claude, via Claude Code)**. The author directed the work, installed it on real hardware and tested it (playback, reboot, sleep/resume, etc.).
The code has been verified to work on real hardware, but it has **not been exhaustively reviewed by a human**. It is a kernel-mode driver, so use and modify it with appropriate caution.

## Status

| Feature | Status |
|---|---|
| Internal speaker playback (48 kHz / 2 ch, shown as a regular Windows "Speakers" device) | ✅ |
| Volume control (Windows software volume) | ✅ |
| Playback after reboot and after sleep (Modern Standby) resume | ✅ |
| Headphone output (RT5682, Windows "Headphones"; needs jslcodec) | ✅ |
| Headset microphone (4-pole, Windows "Headset Microphone"; needs jslcodec) | ✅ |
| Automatic output switching on jack insertion | ❌ not yet (switch manually) |
| Internal microphones (DMIC, 2 ch, shown as a regular Windows "Microphone" device) | ✅ |

Tested on: Acer Chromebook R753T (Magolor, Celeron N4500), MrChromebox UEFI firmware, Windows 11 Pro (build 26200).
A slight white noise may be audible during playback (it is silent when nothing is playing).

## ⚠ Warnings

- This is an **unofficial, experimental kernel-mode driver**. Bugs can cause blue screens or data loss. Use it at your own risk.
- The driver is self-signed (test-signed), so Windows **test-signing mode** must be enabled. This lowers the security of the system (details below).
- It binds to `PCI\VEN_8086&DEV_4DC8` (Jasper Lake HD Audio / DSP). The pin assignments (e.g. amplifier SDB = GPP_D17) are specific to Magolor, so **on other devices it may not work, or may misbehave**.
- This project is not affiliated with Intel, Google, Acer or Realtek.

## Build requirements

- LLVM (clang-cl / lld-link), tested with 19.1.7
- Windows Driver Kit headers and libraries (10.0.26100). Tested with the extracted NuGet packages `Microsoft.Windows.WDK.x64` and `Microsoft.Windows.SDK.CPP` (an installed WDK / SDK should also work)

Visual Studio is not required.

## Usage

Run everything from PowerShell in the repository root.

1. **Prepare test signing (once, as Administrator)** — creates a self-signed code-signing certificate, makes this PC trust it, and enables test-signing mode. Reboot afterwards.
   ```
   powershell -ExecutionPolicy Bypass -File scripts\setup-testsigning.ps1
   ```
2. **Build** (the driver package is written to `pkg\`)
   ```
   powershell -ExecutionPolicy Bypass -File scripts\build.ps1 -LlvmBin <LLVM bin dir> -WdkRoot <WDK "c" dir> -SdkRoot <SDK "c" dir>
   ```
3. **Install (as Administrator)**
   ```
   powershell -ExecutionPolicy Bypass -File scripts\install.ps1
   ```
   It worked if **Speakers (JSL Speaker (SOF DSP))** appears in the Sound settings.
4. **Uninstall (as Administrator)**
   ```
   powershell -ExecutionPolicy Bypass -File scripts\uninstall.ps1
   ```
   To turn test-signing mode off again, run `bcdedit /set testsigning off`, reboot, and delete the certificate (`CN=JSL Audio Test`) with `certlm.msc` / `certmgr.msc`.

The driver writes its log to `C:\Windows\Temp\jslaudio.log` (reading it requires Administrator rights). Install with `install.ps1 -LogPath <file>` to write it to a file of your choice instead.

## How it works

- One PortCls WaveRT miniport (Wave) + topology miniport pair per path exposes "Speakers" and "Microphone" endpoints to Windows. The only supported format is 48 kHz / 2 ch / 32-bit PCM; the Windows audio engine converts everything else.
- When the device starts, the SOF firmware is transferred to the DSP over the HDA code-loader DMA stream and booted, then these pipelines are built over IPC3:
  - speaker: `host → buffer → SSP1 DAI` (I2S, 48 kHz, 2 × 32-bit slots, BCLK 3.072 MHz)
  - headphones: `host → buffer → SSP0 DAI`, headset mic: `SSP0 DAI → buffer → host` (I2S, 48 kHz, 2 × 25-bit slots, BCLK 2.4 MHz)
  - there is a single MCLK pin shared by SSP0 and SSP1, so both use the topology's 24 MHz
  - microphone: `DMIC0 DAI → buffer → host` (two mics on PDM0, PDM clock 2.4–4.8 MHz, matching the ACPI hint `mic-tplg` = `2ch-pdm0`)
- On playback start, as in Linux SOF, **the host DMA is started before the IPC trigger** is sent (in the reverse order the DSP underruns almost immediately and stops the pipeline).
- The RT1015P amplifier's SDB pin is ACPI GpioIo 209. That number is in the pinctrl gpio_base space; the actual pad is **GPP_D17 (community 1, pad 41)**. It is driven high only while playing.
- On sleep (D3) the DSP is powered down; on resume (D0) it is rebooted and the stream is restored. The firmware ships inside the driver package and is cached in memory at first start.

The investigation notes are in [docs/investigation.ja.md](docs/investigation.ja.md) (Japanese).

## Repository layout

| Path | Contents |
|---|---|
| `driver/` | The driver (`jslaudio.cpp`, `jslaudio.inf`) |
| `scripts/` | Test-signing setup, build, install, uninstall |
| `fw/` | Intel-signed SOF firmware and topology (**separate license**, see [fw/README.md](fw/README.md)) |
| `tools/decode-trace.ps1` | Decodes the SOF firmware DMA trace using the `.ldc` dictionary |
| `experiments/` | Earlier prototype drivers and scripts (kept for the record; paths are specific to the development PC) |
| `docs/` | Investigation notes |

## License

- The original code in this repository (`driver/`, `scripts/`, `tools/`, `experiments/`) is under the **MIT License** ([LICENSE](LICENSE)).
- The firmware and topology under `fw/` are copyrighted by Intel Corporation and are **not covered by the MIT License**. They are governed by [fw/LICENCE.Intel](fw/LICENCE.Intel) (BSD-3-Clause and others).
- Referenced projects and notices are listed in [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
