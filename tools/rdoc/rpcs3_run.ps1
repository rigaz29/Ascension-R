# A reference run of God of War: Ascension in RPCS3 (its settings unchanged): boots NPUA80918 with
# the "automation" keyboard input profile for this run only (--input-config), presses the menu's
# buttons by posting key messages to the game window (Square through the movie, then Cross at
# 90/125/150/185 s: profile, Continue Game), posts F12 every -ShotEvery s from -ShotFrom (RPCS3 then
# writes its own screenshot under screenshots\, copied to <Out>\t<s>.png), and with -RenderDoc
# loads RenderDoc's Vulkan layer into this RPCS3 process only (environment variables; nothing is
# registered) and captures -CapFrames frames at each second in -CapAt (trigger.py, captures in
# %TEMP%\RenderDoc, or <CapDir>\RenderDoc). Times count from the epilepsy warning, the first screen that holds 60 Hz. A
# memory guard closes RPCS3 when free RAM stays under -MinFreeMB.
param([Parameter(Mandatory = $true)][string]$Out, [int]$End = 110, [int]$ShotFrom = 5, [int]$ShotEvery = 5,
      [string]$CapAt = "", [int]$CapFrames = 1, [switch]$RenderDoc, [int]$WaitStart = 300,
      [int]$MinFreeMB = 700, [string]$Rpcs3 = "D:\Games\PS3\RPCS3", [string]$Title = "NPUA80918",
      [string]$CapDir = "")
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$rdHome = if ($env:RENDERDOC_HOME) { $env:RENDERDOC_HOME } else { "$env:USERPROFILE\tools\RenderDoc_1.47_64" }
New-Item -ItemType Directory -Force $Out | Out-Null
$shotRoot = "$Rpcs3\screenshots"
Add-Type @"
using System; using System.Text; using System.Runtime.InteropServices;
public static class RdocWin {
  public delegate bool EnumProc(IntPtr h, IntPtr l);
  [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc f, IntPtr l);
  [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
  [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetWindowText(IntPtr h, StringBuilder b, int n);
  [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h, uint m, IntPtr w, IntPtr l);
  [DllImport("user32.dll")] public static extern uint MapVirtualKey(uint c, uint t);
  [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out RECT r);
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
}
"@
$log = "$Out\run.txt"; Set-Content -Path $log -Value "" -Encoding utf8
function L($m) { Add-Content -Path $log -Value $m; $m }
function FreeMB { [int](Get-Counter '\Memory\Available MBytes').CounterSamples[0].CookedValue }
if ($RenderDoc) {
  $env:VK_ADD_IMPLICIT_LAYER_PATH = $rdHome
  $env:ENABLE_VULKAN_RENDERDOC_CAPTURE = "1"
  # RenderDoc writes captures under the process's temp folder: -CapDir moves it (a capture of
  # RPCS3 is 1-2 GB).
  if ($CapDir) {
    New-Item -ItemType Directory -Force $CapDir | Out-Null
    $env:TMP = $CapDir; $env:TEMP = $CapDir
  }
}
$eboot = "$Rpcs3\dev_hdd0\game\$Title\USRDIR\EBOOT.BIN"
$p = Start-Process -FilePath "$Rpcs3\rpcs3.exe" -ArgumentList @("--no-gui", "--input-config", "automation", "`"$eboot`"") -WorkingDirectory $Rpcs3 -PassThru
L "started rpcs3 pid $($p.Id) renderdoc=$RenderDoc free=$(FreeMB) MB"
$t0 = Get-Date
function T { ((Get-Date) - $t0).TotalSeconds }
function GameWin {
  $script:best = [IntPtr]::Zero; $script:bestArea = 0
  [RdocWin]::EnumWindows({ param($h, $l)
    $wp = 0; [void][RdocWin]::GetWindowThreadProcessId($h, [ref]$wp)
    if ($wp -eq $p.Id -and [RdocWin]::IsWindowVisible($h)) {
      $rc = New-Object RdocWin+RECT; [void][RdocWin]::GetClientRect($h, [ref]$rc)
      $a = ($rc.R - $rc.L) * ($rc.B - $rc.T)
      if ($a -gt $script:bestArea) { $script:bestArea = $a; $script:best = $h }
    }
    return $true }, [IntPtr]::Zero) | Out-Null
  return $script:best
}
function Title($h) { $b = New-Object System.Text.StringBuilder 512; [void][RdocWin]::GetWindowText($h, $b, 512); $b.ToString() }
function Key($h, $vk, $hold = 120) {
  $sc = [RdocWin]::MapVirtualKey($vk, 0)
  [void][RdocWin]::PostMessage($h, 0x0100, [IntPtr]$vk, [IntPtr](1 -bor ($sc -shl 16)))
  Start-Sleep -Milliseconds $hold
  [void][RdocWin]::PostMessage($h, 0x0101, [IntPtr]$vk, [IntPtr]((1 -bor ($sc -shl 16) -bor (1 -shl 30)) -bor 0x80000000))
}
$taken = @{}
function Shot($h, $k) {
  $before = Get-Date
  Key $h 0x7B 60
  Start-Sleep -Milliseconds 900
  $new = Get-ChildItem $shotRoot -Recurse -Filter *.png -ErrorAction SilentlyContinue |
         Where-Object { $_.LastWriteTime -ge $before.AddSeconds(-1) -and -not $taken.ContainsKey($_.FullName) } |
         Sort-Object LastWriteTime | Select-Object -Last 1
  if ($new) { $taken[$new.FullName] = 1; Copy-Item $new.FullName ("$Out\t{0:D3}.png" -f $k); L "+$k shot $($new.Name)" }
  else { L "+$k shot: no file" }
}
$low = 0
function Guard { $f = FreeMB; if ($f -lt $MinFreeMB) { $script:low++ } else { $script:low = 0 }; return $f }
$h = [IntPtr]::Zero; $start = -1; $ok = 0
for ($t = 1; $t -le $WaitStart -and -not $p.HasExited -and $start -lt 0; $t++) {
  while ((T) -lt $t) { Start-Sleep -Milliseconds 100 }
  $h = GameWin
  if ($h -eq [IntPtr]::Zero) { $ok = 0; continue }
  $ti = Title $h
  if (($t % 5) -eq 0) { L "t=$t $ti free=$(Guard)" }
  if ($ti -match "FPS: ([\d.]+)" -and [double]$matches[1] -ge 58 -and [double]$matches[1] -le 62) { $ok++ } else { $ok = 0 }
  if ($ok -ge 3) { $start = $t }
}
if ($start -lt 0) { L "the game did not start within $WaitStart s" } else { L "game started at t=$start" }
$plan = @{}
for ($k = 10; $k -le 60; $k += 5) { $plan[$k] = 0x5A }
foreach ($k in 90, 125, 150, 185) { $plan[$k] = 0x58 }
$caps = @{}
if ($CapAt) { foreach ($c in $CapAt.Split(",")) { $caps[[int]$c] = 1 } }
$tp = Join-Path $Out "trigger_params.json"
(@{ out = $Out; target = "rpcs3"; frames = $CapFrames } | ConvertTo-Json) | Set-Content -Path $tp -Encoding utf8
if ($start -ge 0) {
  for ($k = 1; $k -le $End -and -not $p.HasExited; $k++) {
    while ((T) -lt $start + $k) { Start-Sleep -Milliseconds 100 }
    if (($k % 5) -eq 0) {
      $h = GameWin
      $f = Guard
      L "+$k $(Title $h) free=$f"
      if ($low -ge 2) { L "+$k free RAM under $MinFreeMB MB twice: closing RPCS3"; break }
    }
    if ($h -eq [IntPtr]::Zero) { continue }
    if ($plan.ContainsKey($k)) { Key $h $plan[$k]; L "+$k key 0x$('{0:X2}' -f $plan[$k])" }
    if ($k -ge $ShotFrom -and (($k - $ShotFrom) % $ShotEvery) -eq 0) { Shot $h $k }
    if ($RenderDoc -and $caps.ContainsKey($k)) {
      L "+$k capture ($CapFrames frames), free=$(FreeMB)"
      & powershell -NoProfile -ExecutionPolicy Bypass -File "$here\rdoc.ps1" trigger $tp -Max 330 | ForEach-Object { L "  $_" }
    }
  }
}
if (-not $p.HasExited) {
  $h = GameWin
  if ($h -ne [IntPtr]::Zero) { [void][RdocWin]::PostMessage($h, 0x0010, [IntPtr]::Zero, [IntPtr]::Zero) }
  for ($i = 0; $i -lt 20 -and -not $p.HasExited; $i++) { Start-Sleep -Seconds 1 }
  if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force; L "rpcs3 stopped by force" } else { L "rpcs3 closed" }
}
L "done t=$([int](T))"
