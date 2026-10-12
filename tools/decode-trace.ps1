param([string]$Trace = "D:\jsl-audio\trace.bin",
      [string]$Ldc = "D:\sof-bin\sof-bin-main\v2.2.x\sof-v2.2\sof-jsl.ldc")
# SOF v2.2 DMA trace decoder: 20-byte header (uid, ids/core, u64 ts, entry addr) + params
$t = [IO.File]::ReadAllBytes($Trace)
$l = [IO.File]::ReadAllBytes($Ldc)
$base = [BitConverter]::ToUInt32($l, 4); $dlen = [BitConverter]::ToUInt32($l, 8); $doff = [BitConverter]::ToUInt32($l, 12)
function Entry([uint32]$addr) {
    $o = [int]($addr - $base + $doff)
    if ($o -lt $doff -or $o + 20 -gt $l.Length) { return $null }
    $lvl = [BitConverter]::ToUInt32($l, $o); $np = [BitConverter]::ToUInt32($l, $o + 8)
    $line = [BitConverter]::ToUInt32($l, $o + 12); $fnl = [BitConverter]::ToUInt32($l, $o + 16)
    if ($np -gt 4 -or $fnl -gt 200) { return $null }
    $tl = [BitConverter]::ToUInt32($l, $o + 20)
    if ($tl -gt 1000) { return $null }
    $fn = [Text.Encoding]::ASCII.GetString($l, $o + 24, $fnl).TrimEnd([char]0)
    $tx = [Text.Encoding]::ASCII.GetString($l, $o + 24 + $fnl, $tl).TrimEnd([char]0)
    [pscustomobject]@{ lvl = $lvl; np = $np; file = [IO.Path]::GetFileName($fn); line = $line; text = $tx }
}
$i = 0; $out = @()
while ($i + 20 -le $t.Length) {
    $addr = [BitConverter]::ToUInt32($t, $i + 16)
    if ($addr -ge $base -and $addr -lt $base + $dlen) {
        $e = Entry $addr
        if ($e) {
            $ts = [BitConverter]::ToUInt64($t, $i + 8)
            $script:pa = @(); for ($k = 0; $k -lt $e.np; $k++) { $script:pa += [BitConverter]::ToUInt32($t, $i + 20 + 4 * $k) }
            $msg = $e.text
            $script:ai = 0
            $msg = [regex]::Replace($msg, '%(-?\d*)(l*)([dDuxXps])', {
                param($mm)
                if ($script:ai -ge $script:pa.Count) { return $mm.Value }
                $v = $script:pa[$script:ai]; $script:ai++
                switch -CaseSensitive ($mm.Groups[3].Value) { 'd' { [string][int32]$v } 'u' { [string]$v } 'x' { '{0:x}' -f $v } 'X' { '{0:X}' -f $v } 'p' { '0x{0:x}' -f $v } default { '0x{0:x}' -f $v } }
            })
            $script:ai = 0
            $out += ('{0,12} {1}:{2} {3}' -f $ts, $e.file, $e.line, $msg)
            $i += 20 + 4 * $e.np; continue
        }
    }
    $i += 4
}
$out


