# A port run with RenderDoc loaded in-process (RENDERDOC_DLL): scripts/gowa.py bench with the
# given route, and at each of the -At seconds after it starts (a comma list) one capture of -Frames
# frames through trigger.py (the target is named after the exe). The captures land in build\rdoc
# (RENDERDOC_DIR); <Out> gets the bench's output and trigger.log names the captures.
#   port_run.ps1 -Out logs\rdoc-menu -Exe build\Ascension-R.exe -At 45 -BenchArgs "--route","200","--until","60"
param([Parameter(Mandatory = $true)][string]$Out, [string]$Exe = "build\Ascension-R.exe", [string]$At = "45",
      [int]$Frames = 1, [string[]]$BenchArgs = @("--route", "200", "--until", "60"), [string]$ExtraEnv = "")
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$repo = Split-Path -Parent (Split-Path -Parent $here)
$rdHome = if ($env:RENDERDOC_HOME) { $env:RENDERDOC_HOME } else { "$env:USERPROFILE\tools\RenderDoc_1.47_64" }
New-Item -ItemType Directory -Force $Out | Out-Null
$Out = (Resolve-Path $Out).Path
$envs = "RENDERDOC_DLL=$rdHome\renderdoc.dll"
if ($ExtraEnv) { $envs += ";" + $ExtraEnv }
$args = @("scripts/gowa.py", "bench", "--runs", "1", "--no-retry") + $BenchArgs +
        @("--env", "D3D12_VSYNC=0", "--variant", "`"r|$Exe|$envs`"", "--early-stop", "0", "--out", "`"$Out\bench`"")
$b = Start-Process -FilePath "py" -ArgumentList $args -WorkingDirectory $repo -PassThru -RedirectStandardOutput "$Out\bench.txt" -RedirectStandardError "$Out\bench.err"
$t0 = Get-Date
$tp = Join-Path $Out "trigger_params.json"
(@{ out = $Out; target = [System.IO.Path]::GetFileNameWithoutExtension($Exe).ToLower(); frames = $Frames } |
  ConvertTo-Json) | Set-Content -Path $tp -Encoding utf8
foreach ($s in ($At.Split(",") | ForEach-Object { [int]$_ } | Sort-Object)) {
  while (((Get-Date) - $t0).TotalSeconds -lt $s -and -not $b.HasExited) { Start-Sleep -Milliseconds 500 }
  if ($b.HasExited) { "the bench ended before $s s"; break }
  "capture at $s s"
  & powershell -NoProfile -ExecutionPolicy Bypass -File "$here\rdoc.ps1" trigger $tp -Max 330
}
if (-not $b.WaitForExit(900000)) { "the bench is still running after 900 s" }
