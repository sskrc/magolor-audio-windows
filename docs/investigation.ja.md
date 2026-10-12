# 調査メモ: Magolor(Jasper Lake)の内蔵スピーカーを Windows で鳴らすまで

## ハードウェア構成(ACPI とデバイスの調査から)

- ボード: Google "Magolor"(Acer Chromebook R753T、Celeron N4500 / Jasper Lake)、MrChromebox(coreboot)UEFI ファームウェア
- 音声コントローラ: Intel HDAS `PCI\VEN_8086&DEV_4DC8`(cAVS 2.0 DSP 内蔵、SOF / IPC3)
  - MMIO: HDA = BAR0(16 KB)、DSP = BAR4(1 MB)
- スピーカーアンプ: Realtek **RT1015P**(`ACPI\RTL1015`)。I2C 制御は不要で、SDB 端子を High にするだけで動く
  - SDB は ACPI の GpioIo 209。これは Linux pinctrl の **gpio_base 空間の番号**で、`GPP_D`(base 192、ピン 116〜)の 17 番 = **GPP_D17 = ピン 133 = コミュニティ1(`0xFD6D0000`)パッド 41**
- ヘッドホン: RT5682(`ACPI\10EC5682`、I2C4)。未対応
- 公式トポロジ `sof-jsl-rt5682-rt1015.tplg` では、スピーカーは **SSP1**(I2S、48 kHz、32bit スロット×2、BCLK 3.072 MHz、DSP がクロックを供給)。SSP0 はヘッドホン、2.4 MHz の設定は DMIC
- GPIO コミュニティの MMIO: `0xFD6E0000`(0)、`0xFD6D0000`(1)、`0xFD6A0000`(4)、`0xFD690000`(5)

## 経緯

1. **DSP のブート** — Linux SOF ドライバと同じ手順(コア電源投入 → ROM への IPC → コードローダー DMA で FW 転送)を KMDF ドライバで実装し、SOF v2.2(intel-signed)が `FW_READY` を返すところまで成功。
   ブート前に PCI 構成空間の PGCTL / CGCTL で DSP の電源・クロックゲーティングを無効にし、HDA コントローラをリセットしておく。
2. **パイプライン構築** — IPC3 で `pipe_new / comp_new(host) / buffer_new / comp_new(SSP1 DAI) / connect / dai_config / pipe_complete / pcm_params / trigger` を送り、すべて成功(err=0)。
3. **無音の原因調査** — FW の DMA トレースを `.ldc` 辞書で解読するツール(`tools/decode-trace.ps1`)を作って調べた。
   - トレース上、SSP1 の設定値は正常だった(sscr0=0xc1d00b77、M/N=24/25、SCR=12 で 3.072 MHz)。
   - **IPC trigger を先に送り、ホスト DMA を後から起動していたため、DSP 側が約4 ms でアンダーランしてパイプラインを停止していた。** Linux と同じく「ホスト DMA 起動 → IPC trigger」の順にすると、止まらず再生が続いた。
   - **アンプの SDB として別のピン(ピン 209 = ISH_GP_4)を操作していた。** 正しい GPP_D17 を High にすると、内蔵スピーカーから音が出た。
4. **Windows のサウンドデバイス化** — PortCls の WaveRT ドライバとして書き直した。
   - デバイスの停止時に DSP を止めないと、再起動時に動いたままの DSP を再ブートしようとして失敗する。
   - 起動直後はデータボリュームがまだ使えないことがあるため、FW はドライバパッケージに同梱してドライバストアから読む。
   - モダンスタンバイでは D3 で DSP の電源が切れるため、D0 で再ブートしてストリームを復元する。

## 気づいたこと・落とし穴

- **パッドの RXSTATE ではネイティブ機能(I2S)の出力は観測できない。** 実際に音が出ている状態でも、RXSTATE は変化しなかった。「クロックが出ていない」という判断はこの観測方法の限界による誤りだった。
- DSP 起動後は、ホストから SSP のレジスタ(BAR4 + 0x10000〜)が見えなくなる(0xFFFFFFFF)。状態の確認は FW トレースで行う。
- clang-cl + lld-link でカーネルドライバをリンクするときは、`.retplne` などの属性のないセクションを `/MERGE` しておかないと、ローダーに拒否される(`0xC000026C`)。
- 再生中に小さなホワイトノイズが聞こえる。無音時は聞こえず、アンプとスピーカーのアナログ特性と考えられる。
