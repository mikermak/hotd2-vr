<#
  Netplay test on one PC: two copies of the Windows build play HOTD2 together over GGPO on
  localhost (host = player 1, guest = player 2), with the VR reprojection on, each driven by
  the UDP light gun (menus, then shooting and reloading at made-up spots, the guest joining
  with Start), window shots of both, and at the end their per-frame state hashes compared
  (HOTD2VR_SYNCLOG, ggpo.cpp): whether the two stay the same game.

  .\hotd2-vr\netplay-test.ps1                  150 s, no input delay
  .\hotd2-vr\netplay-test.ps1 -Seconds 200 -Delay 2

  Each copy gets its own folder (run\netplay\host and guest, next to this repository or in
  $env:HOTD2VR_DATA) with flycast.exe and the same memory card and flash, so both start
  alike; the game is looked for in "game" there, as run-test.ps1 does.
#>
param(
	[int]$Seconds = 150,
	[int]$Delay = 0,
	[string]$Dump = '',		# frames to dump state + RAM of, e.g. '2,3860' (HOTD2VR_SYNCDUMP)
	[double[]]$Shots = @(30, 50, 65, 80, 95, 110, 125, 140)
)
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
$data = if ($env:HOTD2VR_DATA) { $env:HOTD2VR_DATA } else { Split-Path -Parent $repo }
$build = Join-Path $repo 'build-win'
$cue = Get-ChildItem (Join-Path $data 'game') -Include *.cue, *.gdi, *.chd -Recurse | Select-Object -First 1
if (-not $cue) { throw "No game found in $(Join-Path $data 'game')" }
$root = Join-Path $data 'run\netplay'
$shotDir = Join-Path $data 'shots\netplay'
New-Item -ItemType Directory -Force $root, $shotDir | Out-Null
Get-ChildItem $shotDir -Filter *.png -ErrorAction SilentlyContinue | Remove-Item

Add-Type -AssemblyName System.Drawing
Add-Type @'
using System;
using System.Runtime.InteropServices;
public static class NpWin {
	[StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
	[DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out RECT r);
	[DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr hdc, uint flags);
	[DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
	[DllImport("user32.dll")] public static extern bool IsIconic(IntPtr h);
	[DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int cmd);
}
'@
[void][NpWin]::SetProcessDPIAware()

function Save-Shot($proc, $file) {
	$proc.Refresh()
	$h = $proc.MainWindowHandle
	if ($h -eq [IntPtr]::Zero) { return }
	if ([NpWin]::IsIconic($h)) { [void][NpWin]::ShowWindow($h, 4); Start-Sleep -Milliseconds 300 }
	$r = New-Object NpWin+RECT
	[void][NpWin]::GetClientRect($h, [ref]$r)
	if ($r.R -le 0 -or $r.B -le 0) { return }
	$bmp = New-Object System.Drawing.Bitmap $r.R, $r.B
	$g = [System.Drawing.Graphics]::FromImage($bmp)
	$hdc = $g.GetHdc()
	[void][NpWin]::PrintWindow($h, $hdc, 3)	# client area, with GPU content
	$g.ReleaseHdc($hdc); $g.Dispose()
	$bmp.Save($file, [System.Drawing.Imaging.ImageFormat]::Png); $bmp.Dispose()
}

# both copies: the same build, memory card and flash (no savestates)
$peers = @(
	[pscustomobject]@{ Name = 'host';  Port = 27031; Net = 'network:ActAsServer=yes' },
	[pscustomobject]@{ Name = 'guest'; Port = 27032; Net = 'network:ActAsServer=no,network:server=127.0.0.1' }
)
foreach ($peer in $peers) {
	$dir = Join-Path $root $peer.Name
	if (Test-Path $dir) { Remove-Item $dir -Recurse -Force }
	New-Item -ItemType Directory -Force (Join-Path $dir 'data') | Out-Null
	Copy-Item (Join-Path $build 'flycast.exe') $dir
	Get-ChildItem $build -Filter *.dll -ErrorAction SilentlyContinue | Copy-Item -Destination $dir
	Get-ChildItem (Join-Path $build 'data') -File | Where-Object { $_.Name -match 'vmu_save|nvmem' } |
		Copy-Item -Destination (Join-Path $dir 'data')
	$peer | Add-Member Dir $dir
}

# OpenGL + native depth (the reprojection's path), HOTD2 PAL camera, light guns in A and B
$base = 'config:pvr.rend=0,config:rend.NativeDepthInterpolation=yes,config:aica.Volume=0,log:LogToFile=yes' +
	',config:vr.Reproject=yes,config:vr.ProjAddr=5006816,config:vr.ViewportAddr=5007112' +
	',input:device1=7,input:device2=7,network:GGPO=yes,network:EnableUPnP=no' +
	",network:GGPODelay=$Delay"
if ($Dump) { $env:HOTD2VR_SYNCDUMP = $Dump }
foreach ($peer in $peers) {
	$env:HOTD2VR_SYNCLOG = Join-Path $peer.Dir 'sync.txt'
	$cfg = "$base,config:vr.UdpPort=$($peer.Port),$($peer.Net)"
	$peer | Add-Member Proc (Start-Process -FilePath (Join-Path $peer.Dir 'flycast.exe') `
		-ArgumentList @('-config', $cfg, "`"$($cue.FullName)`"") -WorkingDirectory $peer.Dir -PassThru)
	Start-Sleep -Seconds 1
}
Remove-Item Env:HOTD2VR_SYNCLOG
if ($Dump) { Remove-Item Env:HOTD2VR_SYNCDUMP }

# What each gun does at time t (s): Start through the menus (host), B through the story,
# then shooting; the guest joins with Start. Buttons: 1 trigger, 2 reload, 4 Start, 128 B.
function Gun-State([string]$who, [double]$t) {
	$pulse = { param($from, $to, $every) $t -ge $from -and $t -le $to -and (($t - $from) % $every) -lt 0.2 }
	$x = 5000; $y = 5000; $b = 0
	if ($who -eq 'host') {
		if (& $pulse 44 58.5 2) { $b = 4 }
		foreach ($at in 61, 63.5, 66, 68.5) { if ($t -ge $at -and $t -lt $at + 0.2) { $b = 128 } }
	} else {
		# join: three times, as a press while it already plays pauses (and the next goes on)
		foreach ($at in 85, 89, 93) { if ($t -ge $at -and $t -lt $at + 0.2) { $b = 4 } }
	}
	$fireFrom = if ($who -eq 'host') { 70 } else { 82 }
	if ($t -ge $fireFrom) {
		$phase = if ($who -eq 'host') { 0 } else { 1.7 }
		$x = [int](5000 + 3500 * [math]::Sin($t * 0.9 + $phase))
		$y = [int](5000 + 2500 * [math]::Sin($t * 1.3 + $phase * 2))
		if ((($t * 1000) % 300) -lt 120) { $b = $b -bor 1 }
		if ((($t - $fireFrom) % 6) -gt 5.7) { $b = 2 }		# every 6 s: reload (a shot off the screen)
	}
	return "LG 0 $x $y $b"
}

$udp = New-Object System.Net.Sockets.UdpClient
$t0 = Get-Date
$shotQueue = [System.Collections.Generic.Queue[double]]::new()
foreach ($s in ($Shots | Sort-Object)) { $shotQueue.Enqueue($s) }
try {
	while ($true) {
		$t = ((Get-Date) - $t0).TotalSeconds
		if ($t -ge $Seconds) { break }
		foreach ($peer in $peers) {
			if ($peer.Proc.HasExited) { throw "$($peer.Name) closed early (code $($peer.Proc.ExitCode))" }
			$msg = [Text.Encoding]::ASCII.GetBytes((Gun-State $peer.Name $t))
			[void]$udp.Send($msg, $msg.Length, '127.0.0.1', $peer.Port)
		}
		if ($shotQueue.Count -gt 0 -and $t -ge $shotQueue.Peek()) {
			$at = $shotQueue.Dequeue()
			foreach ($peer in $peers) { Save-Shot $peer.Proc (Join-Path $shotDir ("{0}_{1:000}.png" -f $peer.Name, $at)) }
			Write-Output ("shots at {0}s" -f $at)
		}
		Start-Sleep -Milliseconds 40
	}
} finally {
	$udp.Close()
	foreach ($peer in $peers) { if (-not $peer.Proc.HasExited) { Stop-Process -Id $peer.Proc.Id -Force } }
}
Start-Sleep -Milliseconds 500

# Compare: per frame the last line of each (a rollback saves a frame again)
$logs = @{}
foreach ($peer in $peers) {
	$map = @{}
	$file = Join-Path $peer.Dir 'sync.txt'
	if (Test-Path $file) {
		foreach ($line in [IO.File]::ReadLines($file)) {
			$f = $line.Split(' ')
			if ($f.Count -eq 3) { $map[[int]$f[0]] = "$($f[1]) $($f[2])" }
		}
	}
	$logs[$peer.Name] = $map
	Write-Output ("{0}: {1} frames logged" -f $peer.Name, $map.Count)
}
# RAM decides (the rest of a rollback state holds a host pointer, different on each side)
$common = $logs['host'].Keys | Where-Object { $logs['guest'].ContainsKey($_) } | Sort-Object
$ramDiff = @($common | Where-Object { $logs['host'][$_].Split(' ')[1] -ne $logs['guest'][$_].Split(' ')[1] })
Write-Output ("frames in both: {0}, RAM different: {1}" -f $common.Count, $ramDiff.Count)
if ($ramDiff.Count -gt 0) {
	$first = $ramDiff[0]
	Write-Output ("first frame with different RAM: {0} ({1:0.0} s)" -f $first, ($first / 60))
} else {
	Write-Output 'the same game on both sides, every frame'
}
foreach ($peer in $peers) {
	$log = Join-Path $peer.Dir 'flycast.log'
	if (Test-Path $log) {
		Write-Output "--- $($peer.Name) log (network):"
		Select-String -Path $log -Pattern 'GGPO|NETWORK|desync|light gun' | Select-Object -Last 8 | ForEach-Object { $_.Line }
	}
}
