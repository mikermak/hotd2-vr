<#
  Netplay test on one PC: two copies of the Windows build play a game together over GGPO on
  localhost (host = player 1, guest = player 2), with the VR reprojection on, each driven by
  the UDP light gun (menus, then shooting and reloading at made-up spots, the guest joining
  with Start), window shots of both, and at the end their per-frame memory compared (main,
  video and sound RAM; HOTD2VR_SYNCLOG, ggpo.cpp): whether the two stay the same game. Also
  how much was rolled back and how fast it ran.

  .\hotd2-vr\netplay-test.ps1                              HOTD2 PAL, 150 s, a perfect network
  .\hotd2-vr\netplay-test.ps1 -Quest                       vr.FovScale 2 (online the game's
                                                           profile sets that on both sides anyway)
  .\hotd2-vr\netplay-test.ps1 -OwnSaves                    each copy its own flash and memory
                                                           card (as two headsets): must still match
  .\hotd2-vr\netplay-test.ps1 -NetDelay 80 -Loss 2         a bad connection: 80 ms each way
                                                           (with jitter), 2% of packets lost
  .\hotd2-vr\netplay-test.ps1 -Game us -GamePath X.chd     the US version
  .\hotd2-vr\netplay-test.ps1 -Game mok                    The Maze of the Kings (naomi\mok.zip)

  -Delay is GGPO's input delay in frames (fewer rollbacks, a later shot), -Oop the percentage of
  packets sent out of order, -Dump frames to write state + RAM of (e.g. '2,3860'), -Pages a hash
  of main RAM per 64 KB page every frame (sync.txt.pages), to find where a difference is.

  Each copy gets its own folder (run\netplay\host and guest, next to this repository or in
  $env:HOTD2VR_DATA) with flycast.exe and the same memory card and flash, so both start
  alike; HOTD2 PAL is looked for in "game" there, as run-test.ps1 does.
#>
param(
	[ValidateSet('pal', 'us', 'mok')][string]$Game = 'pal',
	[string]$GamePath = '',
	[switch]$Quest,
	[int]$Seconds = 150,
	[int]$Delay = 0,
	[int]$NetDelay = 0,
	[int]$Loss = 0,
	[int]$Oop = 0,
	[string]$Dump = '',
	[switch]$Pages,
	[switch]$OwnSaves,
	[switch]$RollbackCheck,
	[int]$StallGuest = 0,	# the guest's render loop stops this many ms every 20 s (as a headset's system overlay)	# compare the RAM after every rollback with a full copy (HOTD2VR_ROLLBACKCHECK)
	[string]$Region = '',	# 'offset,size,fromFrame,toFrame': that part of RAM every frame (sync.txt.region)
	[double[]]$Shots = @()
)
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
$data = if ($env:HOTD2VR_DATA) { $env:HOTD2VR_DATA } else { Split-Path -Parent $repo }
$build = Join-Path $repo 'build-win'
if (-not $GamePath) {
	$GamePath = switch ($Game) {
		'pal' { (Get-ChildItem (Join-Path $data 'game') -Include *.cue, *.gdi, *.chd -Recurse | Select-Object -First 1).FullName }
		'us'  { (Get-ChildItem (Join-Path $data 'game-us') -Include *.cue, *.gdi, *.chd -Recurse -ErrorAction SilentlyContinue | Select-Object -First 1).FullName }
		'mok' { Join-Path $data 'naomi\mok.zip' }
	}
}
if (-not $GamePath -or -not (Test-Path $GamePath)) { throw "No game for '$Game' (give -GamePath)" }
$root = Join-Path $data 'run\netplay'
$shotDir = Join-Path $data 'shots\netplay'
New-Item -ItemType Directory -Force $root, $shotDir | Out-Null
Get-ChildItem $shotDir -Filter *.png -ErrorAction SilentlyContinue | Remove-Item

# The route through each game, in seconds after the start: P1's Starts through the menus,
# B through HOTD2's story, when both start shooting, when P2 joins (three presses: on the
# Dreamcast a press while playing pauses, the next goes on), and when to take shots.
$route = switch ($Game) {
	'mok' { @{ Starts = @(36, 62, 2); Bs = @(); Fire = 64; GuestFire = 76; Join = @(80, 84, 88); Shots = @(40, 60, 75, 90, 105, 120, 140) } }
	default { @{ Starts = @(44, 58.5, 2); Bs = @(61, 63.5, 66, 68.5); Fire = 70; GuestFire = 82; Join = @(85, 89, 93); Shots = @(60, 75, 90, 105, 120, 140) } }
}
if (-not $Shots) { $Shots = $route.Shots | Where-Object { $_ -lt $Seconds } }

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
	# the same memory card and flash for both, or (-OwnSaves) the host's from the PC's solo
	# games and none for the guest: online the game must not care (a blank card and flash)
	if (-not $OwnSaves -or $peer.Name -eq 'host') {
		Get-ChildItem (Join-Path $build 'data') -File | Where-Object { $_.Name -match 'vmu_save|nvmem|eeprom' } |
			Copy-Item -Destination (Join-Path $dir 'data')
	}
	$peer | Add-Member Dir $dir
}

# OpenGL + native depth (the reprojection's path; each game's profile finds its camera),
# light guns in A and B (the Dreamcast; an arcade game has them on its I/O board)
$base = 'config:pvr.rend=0,config:rend.NativeDepthInterpolation=yes,config:aica.Volume=0,log:LogToFile=yes' +
	',config:vr.Reproject=yes,input:device1=7,input:device2=7,network:GGPO=yes,network:EnableUPnP=no' +
	",network:GGPODelay=$Delay"
if ($Quest) { $base += ',config:vr.FovScale=2,config:vr.WidenFov=yes' }
# GGPO's own network emulation (udp_proto.cpp), both ways
$netEnv = @{ GGPO_NETWORK_DELAY = $NetDelay; GGPO_PACKET_LOSS = $Loss; GGPO_OOP_PERCENT = $Oop }
foreach ($k in $netEnv.Keys) { if ($netEnv[$k]) { Set-Item "Env:$k" $netEnv[$k] } }
if ($Dump) { $env:HOTD2VR_SYNCDUMP = $Dump }
if ($Pages) { $env:HOTD2VR_SYNCPAGES = '1' }
if ($Region) { $env:HOTD2VR_SYNCREGION = $Region }
if ($RollbackCheck) { $env:HOTD2VR_ROLLBACKCHECK = '1' }
foreach ($peer in $peers) {
	$env:HOTD2VR_SYNCLOG = Join-Path $peer.Dir 'sync.txt'
	if ($StallGuest -and $peer.Name -eq 'guest') { $env:HOTD2VR_STALLRENDER = $StallGuest } else { Remove-Item Env:HOTD2VR_STALLRENDER -ErrorAction SilentlyContinue }
	$cfg = "$base,config:vr.UdpPort=$($peer.Port),$($peer.Net)"
	$peer | Add-Member Proc (Start-Process -FilePath (Join-Path $peer.Dir 'flycast.exe') `
		-ArgumentList @('-config', $cfg, "`"$GamePath`"") -WorkingDirectory $peer.Dir -PassThru)
	Start-Sleep -Seconds 1
}
foreach ($k in @('HOTD2VR_SYNCLOG', 'HOTD2VR_SYNCDUMP', 'HOTD2VR_SYNCPAGES', 'HOTD2VR_SYNCREGION', 'HOTD2VR_ROLLBACKCHECK', 'HOTD2VR_STALLRENDER') + @($netEnv.Keys)) { Remove-Item "Env:$k" -ErrorAction SilentlyContinue }

# What each gun does at time t (s). Buttons: 1 trigger, 2 reload, 4 Start, 128 B.
function Gun-State([string]$who, [double]$t) {
	$x = 5000; $y = 5000; $b = 0
	if ($who -eq 'host') {
		$from, $to, $every = $route.Starts
		if ($t -ge $from -and $t -le $to -and (($t - $from) % $every) -lt 0.2) { $b = 4 }
		foreach ($at in $route.Bs) { if ($t -ge $at -and $t -lt $at + 0.2) { $b = 128 } }
	} else {
		foreach ($at in $route.Join) { if ($t -ge $at -and $t -lt $at + 0.2) { $b = 4 } }
	}
	$fireFrom = if ($who -eq 'host') { $route.Fire } else { $route.GuestFire }
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
		}
		Start-Sleep -Milliseconds 40
	}
} finally {
	$udp.Close()
	foreach ($peer in $peers) { if (-not $peer.Proc.HasExited) { Stop-Process -Id $peer.Proc.Id -Force } }
}
Start-Sleep -Milliseconds 500

# Each side's log: per frame the last RAM hash (a rollback saves a frame again); rollbacks are
# where the frame number goes back, their depth the frames simulated again.
$logs = @{}
foreach ($peer in $peers) {
	$ram = @{}
	$lines = 0; $rollbacks = 0; $resim = 0; $deepest = 0; $prev = -1; $first = $null; $last = $null
	$file = Join-Path $peer.Dir 'sync.txt'
	if (Test-Path $file) {
		foreach ($line in [IO.File]::ReadLines($file)) {
			$f = $line.Split(' ')
			if ($f.Count -lt 3) { continue }
			$frame = [int]$f[0]
			$lines++
			if ($frame -le $prev) {
				$rollbacks++
				$depth = $prev - $frame + 1
				$resim += $depth
				if ($depth -gt $deepest) { $deepest = $depth }
			}
			$prev = $frame
			# main RAM, and video + sound RAM when logged
			$ram[$frame] = if ($f.Count -ge 5) { "$($f[2]) $($f[4])" } else { $f[2] }
			if ($f.Count -ge 4) {
				if ($null -eq $first) { $first = @($frame, [long]$f[3]) }
				$last = @($frame, [long]$f[3])
			}
		}
	}
	$logs[$peer.Name] = $ram
	$fps = if ($first -and $last -and $last[1] -gt $first[1]) { ($last[0] - $first[0]) * 1000.0 / ($last[1] - $first[1]) } else { 0 }
	Write-Output ("{0}: {1} frames, {2:0.0} fps; {3} rollbacks ({4:0.0} per second), on average {5:0.0} frames deep, at most {6}; {7:0.0}% of frames simulated again" -f `
		$peer.Name, $ram.Count, $fps, $rollbacks, ($rollbacks * 60.0 / [math]::Max(1, $ram.Count)),
		($resim / [math]::Max(1, $rollbacks)), $deepest, (100.0 * $resim / [math]::Max(1, $ram.Count)))
}
# The last frames were still predictions when the copies were closed (not yet corrected by
# the other side's input): only frames GGPO can no longer roll back count.
$confirmedUpTo = [math]::Min(($logs['host'].Keys | Measure-Object -Maximum).Maximum,
	($logs['guest'].Keys | Measure-Object -Maximum).Maximum) - 10
$common = $logs['host'].Keys | Where-Object { $_ -le $confirmedUpTo -and $logs['guest'].ContainsKey($_) } | Sort-Object
$ramDiff = @($common | Where-Object { $logs['host'][$_] -ne $logs['guest'][$_] })
Write-Output ("frames in both (confirmed): {0}, memory (main, video, sound RAM) different: {1}" -f $common.Count, $ramDiff.Count)
if ($ramDiff.Count -gt 0) {
	$a = $logs['host'][$ramDiff[0]].Split(' '); $b = $logs['guest'][$ramDiff[0]].Split(' ')
	$which = @(if ($a[0] -ne $b[0]) { 'main RAM' }; if ($a.Count -gt 1 -and $a[1] -ne $b[1]) { 'video/sound RAM' }) -join ' + '
	Write-Output ("first frame with different memory: {0} ({1:0.0} s), {2}" -f $ramDiff[0], ($ramDiff[0] / 60), $which)
} elseif ($common.Count -gt 0) {
	Write-Output 'the same game on both sides, every frame'
}
foreach ($peer in $peers) {
	$log = Join-Path $peer.Dir 'flycast.log'
	if (Test-Path $log) {
		$bad = @(Select-String -Path $log -Pattern 'Rollback check').Count
		if ($RollbackCheck) { "$($peer.Name): rollback check, $bad problem lines" }
		Select-String -Path $log -Pattern 'GGPO|stopped|Exception|disconnect|Rollback check|render loop stops' |
			Where-Object { $_.Line -notmatch 'GGPO verif' } | Select-Object -Last 4 |
			ForEach-Object { "$($peer.Name) log: " + ($_.Line -replace '^\S+ \S+ ', '') }
	}
}
