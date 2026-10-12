# magolor-audio-windows

**日本語** | [English](README.en.md)

Windows で **Acer Chromebook R753T(ボード名 "Magolor"、Intel Jasper Lake)** の内蔵スピーカー、内蔵マイク、ヘッドホン端子を使うための、非公式な自作オーディオドライバです。

ヘッドホン端子(RT5682 コーデック)を使うには、別リポジトリの [sskrc/magolor-rt5682-windows](https://github.com/sskrc/magolor-rt5682-windows)(`jslcodec`、GPL-2.0)も必要です。

> **English summary** — An unofficial, experimental Windows PortCls (WaveRT) driver for the internal speakers, microphones and headphone jack of the Jasper Lake Chromebook "Magolor" (Acer R753T) running Windows via MrChromebox UEFI firmware. It boots the cAVS 2.0 audio DSP with Intel's signed Sound Open Firmware (SOF v2.2, IPC3), builds host → SSP1 (speaker, RT1015P amplifier), DMIC → host (microphone) and SSP0 (RT5682 headset codec, configured by the separate GPL-2.0 driver in sskrc/magolor-rt5682-windows) pipelines. Speakers, microphones, headphones, headset mic, volume, reboot and Modern Standby resume work. Use at your own risk. Full English README: [README.en.md](README.en.md).

## AI による作成について

このリポジトリのドライバ、スクリプト、ドキュメントは、**AI(Anthropic の Claude。Claude Code を使用)が作成**しました。作者は、方針の指示、実機でのインストール、動作確認(音が出るか、再起動・スリープ後の動作など)を担当しています。
コードは実機で動作を確認していますが、人間による網羅的なコードレビューは行っていません。カーネルドライバであることを踏まえて、利用や改変の際は十分に注意してください。

> **AI disclosure** — The driver, scripts and documentation in this repository were written by an AI (Anthropic's Claude, via Claude Code). The author directed the work and tested it on real hardware. The code has not been exhaustively reviewed by a human; treat it with the caution appropriate for a kernel-mode driver.

## 状態

| 機能 | 状態 |
|---|---|
| 内蔵スピーカー再生(48 kHz / 2ch、Windows 標準の「スピーカー」として表示) | ✅ |
| 音量調整(Windows のソフトウェア音量) | ✅ |
| 再起動後・スリープ(モダンスタンバイ)復帰後の再生 | ✅ |
| ヘッドホン出力(RT5682、Windows の「Headphones」。要 jslcodec) | ✅ |
| ヘッドセットマイク(4極、Windows の「Headset Microphone」。要 jslcodec) | ✅ |
| ジャック差し込み時の出力先の自動切り替え | ❌ 未対応(手動で切り替え) |
| 内蔵マイク(DMIC、2ch、Windows 標準の「マイク」として表示) | ✅ |

動作確認機: Acer Chromebook R753T(Magolor、Celeron N4500)、MrChromebox UEFI ファームウェア、Windows 11 Pro(build 26200)。
再生中にわずかなホワイトノイズが聞こえることがあります(無音時は聞こえません)。

## ⚠ 注意

- **非公式・実験的なカーネルドライバ**です。不具合があるとブルースクリーンやデータ損失の可能性があります。自己責任で使ってください。
- 自己署名(テスト署名)のため、Windows の **テスト署名モード** を有効にする必要があります。これはセキュリティを下げる設定です(内容は下記)。
- 対象は `PCI\VEN_8086&DEV_4DC8`(Jasper Lake の HD Audio / DSP)です。ピン配置(アンプの SDB = GPP_D17 など)は Magolor 固有のため、**他機種では動かないか、誤動作する可能性があります**。
- Intel、Google、Acer、Realtek とは無関係です。

## 必要なもの(ビルド)

- LLVM(clang-cl / lld-link)。19.1.7 で確認
- Windows Driver Kit のヘッダとライブラリ(10.0.26100)。NuGet パッケージ `Microsoft.Windows.WDK.x64` と `Microsoft.Windows.SDK.CPP` を展開したもの(またはインストール済みの WDK / SDK)で確認

Visual Studio は不要です。

## 使い方

すべて PowerShell で、リポジトリのルートから実行します。

1. **テスト署名の準備(初回のみ、管理者)** — 自己署名のコード署名証明書を作って PC に信頼させ、テスト署名モードを有効にします。終わったら再起動します。
   ```
   powershell -ExecutionPolicy Bypass -File scripts\setup-testsigning.ps1
   ```
2. **ビルド**(`pkg\` にドライバパッケージができます)
   ```
   powershell -ExecutionPolicy Bypass -File scripts\build.ps1 -LlvmBin <LLVMのbin> -WdkRoot <WDKのc> -SdkRoot <SDKのc>
   ```
3. **インストール(管理者)**
   ```
   powershell -ExecutionPolicy Bypass -File scripts\install.ps1
   ```
   「サウンド」設定に **Speakers (JSL Speaker (SOF DSP))** が出れば成功です。
4. **アンインストール(管理者)**
   ```
   powershell -ExecutionPolicy Bypass -File scripts\uninstall.ps1
   ```
   テスト署名モードを戻すには `bcdedit /set testsigning off` を実行して再起動し、証明書(`CN=JSL Audio Test`)を `certlm.msc` / `certmgr.msc` から削除します。

ドライバのログは `C:\Windows\Temp\jslaudio.log` に出ます(読むには管理者権限が必要です)。`install.ps1 -LogPath <ファイル>` を付けてインストールすると、指定したファイルに出力します。

## 仕組み

- PortCls の WaveRT ミニポート(Wave)とトポロジーミニポートの組を経路ごとに用意し、Windows に「スピーカー」と「マイク」を見せます。対応形式は 48 kHz / 2ch / 32bit PCM のみで、それ以外は Windows のオーディオエンジンが変換します。
- デバイス起動時に、HDA のコードローダー DMA で SOF ファームウェアを DSP に転送してブートし、IPC3 で次のパイプラインを組みます。
  - スピーカー: `host → buffer → SSP1 DAI`(I2S、48 kHz、32bit スロット×2、BCLK 3.072 MHz)
  - ヘッドホン: `host → buffer → SSP0 DAI`、ヘッドセットマイク: `SSP0 DAI → buffer → host`(I2S、48 kHz、25bit スロット×2、BCLK 2.4 MHz)
  - MCLK(24 MHz)は端子が1本しかなく SSP0 と SSP1 で共有するため、両方とも公式トポロジと同じ 24 MHz にしています
  - マイク: `DMIC0 DAI → buffer → host`(PDM0 の2マイク、PDM クロック 2.4〜4.8 MHz。ACPI の `mic-tplg` = `2ch-pdm0` に合わせた構成)
- 再生開始時は Linux SOF と同じく **ホスト DMA を先に起動してから IPC trigger** を送ります(逆順だと DSP 側がすぐアンダーランして停止します)。
- アンプ RT1015P の SDB は ACPI の GpioIo 209 です。これは pinctrl の gpio_base 空間の番号で、実際のピンは **GPP_D17(コミュニティ1 パッド41)** です。再生中だけ High にします。
- スリープ時(D3)は DSP を停止し、復帰時(D0)に再ブートしてストリームを復元します。ファームウェアはドライバパッケージに同梱し、初回起動時にメモリに読み込んで保持します。

調査の経緯は [docs/investigation.ja.md](docs/investigation.ja.md) にあります。

## リポジトリ構成

| パス | 内容 |
|---|---|
| `driver/` | ドライバ本体(`jslaudio.cpp`、`jslaudio.inf`) |
| `scripts/` | テスト署名の準備、ビルド、インストール、アンインストール |
| `fw/` | Intel 署名済み SOF ファームウェアとトポロジ(**別ライセンス**、[fw/README.md](fw/README.md)) |
| `tools/decode-trace.ps1` | SOF ファームウェアの DMA トレースを `.ldc` 辞書で解読するツール |
| `experiments/` | 開発途中の試作ドライバとスクリプト(記録用。パスはこの PC 固有) |
| `docs/` | 調査メモ |

## ライセンス

- このリポジトリの自作コード(`driver/`、`scripts/`、`tools/`、`experiments/`)は **MIT License** です([LICENSE](LICENSE))。
- `fw/` 以下のファームウェアとトポロジは Intel Corporation の著作物で、**MIT の対象外**です。[fw/LICENCE.Intel](fw/LICENCE.Intel)(BSD-3-Clause ほか)に従います。
- 参考にしたプロジェクトなどは [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md) にまとめています。
