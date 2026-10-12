<#
  Netplay with the headset: the Quest hosts (player 1, the player in the headset), this PC
  joins as player 2 driven by the UDP light gun (it joins with Start and keeps shooting), to
  see what netplay costs on the Quest: its log says every 5 s how many frames it ran and how
  many it had to simulate again (adb logcat -s flycast | findstr "GGPO stats").

  A Quest and a PC round floating point differently (ggpo.cpp), so they are normally refused;
  network:hotd2vr.TestAnyCpu lets them connect for this test, and their games drift apart
  after a while. The cost is what this measures, not the sync.

  .\hotd2-vr\netplay-quest.ps1 -QuestIp 192.168.1.155 [-Seconds 300] [-Delay 1]

  Start the headset side first (the script prints the adb command), then this.
#>
param(
	[Parameter(Mandatory)][string]$QuestIp,
	[int]$Seconds = 300,
	[int]$Delay = 0,
	[string]$GamePath = ''
)
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
$data = if ($env:HOTD2VR_DATA) { $env:HOTD2VR_DATA } else { Split-Path -Parent $repo }
if (-not $GamePath) { $GamePath = (Get-ChildItem (Join-Path $data 'game') -Include *.cue, *.gdi, *.chd -Recurse | Select-Object -First 1).FullName }
$dir = Join-Path $data 'run\netplay\pc-guest'
if (Test-Path $dir) { Remove-Item $dir -Recurse -Force }
New-Item -ItemType Directory -Force (Join-Path $dir 'data') | Out-Null
Copy-Item (Join-Path $repo 'build-win\flycast.exe') $dir

$port = 27041
$cfg = 'config:pvr.rend=0,config:rend.NativeDepthInterpolation=yes,config:aica.Volume=0,log:LogToFile=yes' +
	",input:device1=7,input:device2=7,config:vr.UdpPort=$port" +
	",network:GGPO=yes,network:ActAsServer=no,network:server=$QuestIp,network:EnableUPnP=no" +
	",network:GGPODelay=$Delay,network:hotd2vr.TestAnyCpu=yes" +
	',config:Dreamcast.Region=2,config:Dreamcast.Broadcast=1,config:Dreamcast.Language=1,config:Dreamcast.Cable=3'	# (as the headset's launch: the blank flash gets them)
Write-Output "Headset side (start this first):"
Write-Output "  adb shell am start -S -n com.flycast.emulator.vr/com.flycast.emulator.MainActivity --es hotd2vr.config `"network:GGPO=yes,network:ActAsServer=yes,network:EnableUPnP=no,network:GGPODelay=$Delay,network:hotd2vr.TestAnyCpu=yes,config:Dreamcast.Region=2,config:Dreamcast.Broadcast=1,config:Dreamcast.Language=1,config:Dreamcast.Cable=3`""
$p = Start-Process -FilePath (Join-Path $dir 'flycast.exe') -ArgumentList @('-config', $cfg, "`"$GamePath`"") -WorkingDirectory $dir -PassThru

# player 2 on this PC: joins with Start now and then (a press while it plays pauses, the next
# goes on), shoots at made-up spots and reloads every 6 s
$udp = New-Object System.Net.Sockets.UdpClient
$t0 = Get-Date
try {
	while (((Get-Date) - $t0).TotalSeconds -lt $Seconds -and -not $p.HasExited) {
		$t = ((Get-Date) - $t0).TotalSeconds
		$b = 0
		if ($t -gt 60 -and ($t % 30) -lt 0.2) { $b = 4 }
		$x = [int](5000 + 3500 * [math]::Sin($t * 0.9 + 1.7))
		$y = [int](5000 + 2500 * [math]::Sin($t * 1.3 + 3.4))
		if ($t -gt 60) {
			if ((($t * 1000) % 300) -lt 120) { $b = $b -bor 1 }
			if (($t % 6) -gt 5.7) { $b = 2 }
		}
		$msg = [Text.Encoding]::ASCII.GetBytes("LG 0 $x $y $b")
		[void]$udp.Send($msg, $msg.Length, '127.0.0.1', $port)
		Start-Sleep -Milliseconds 40
	}
} finally {
	$udp.Close()
	if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force }
}
Select-String -Path (Join-Path $dir 'flycast.log') -Pattern 'GGPO stats|GGPO verif|stopped|Disconnect' | Select-Object -Last 12 | ForEach-Object { $_.Line }
