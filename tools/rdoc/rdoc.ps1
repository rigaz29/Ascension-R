# Run one of the RenderDoc scripts in this folder inside qrenderdoc (--ui-python: no window):
#   rdoc.ps1 <script> <params.json> [-Max <s>]
# The script reads its parameters from the JSON file named by RDOC_PARAMS, writes its results into
# the folder the parameters' "out" names, and logs to <out>\<script>.log, which is printed here.
# RenderDoc is $env:RENDERDOC_HOME, else %USERPROFILE%\tools\RenderDoc_1.47_64 (the portable zip).
# -MinFreeMB: stop qrenderdoc when available RAM (free and standby) stays under that for two
# seconds (a replay of a big capture holds gigabytes).
param([Parameter(Mandatory = $true)][string]$Script, [Parameter(Mandatory = $true)][string]$Params,
      [int]$Max = 900, [int]$MinFreeMB = 600)
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$rd = if ($env:RENDERDOC_HOME) { $env:RENDERDOC_HOME } else { "$env:USERPROFILE\tools\RenderDoc_1.47_64" }
$py = Join-Path $here ($Script + ".py")
if (-not (Test-Path $py)) { "no script $py"; exit 1 }
$env:RDOC_PARAMS = (Resolve-Path $Params).Path
$p = Get-Content $Params -Raw | ConvertFrom-Json
New-Item -ItemType Directory -Force $p.out | Out-Null
$log = Join-Path $p.out ($Script + ".log")
Remove-Item $log -ErrorAction SilentlyContinue
$q = Start-Process -FilePath "$rd\qrenderdoc.exe" -ArgumentList @("--ui-python", "`"$py`"") -PassThru
$t0 = Get-Date; $low = 0
while (-not $q.WaitForExit(1000)) {
  $free = [int](Get-Counter '\Memory\Available MBytes').CounterSamples[0].CookedValue
  if ($free -lt $MinFreeMB) { $low++ } else { $low = 0 }
  if ($low -ge 2) { Stop-Process -Id $q.Id -Force; "qrenderdoc stopped: free RAM $free MB"; break }
  if (((Get-Date) - $t0).TotalSeconds -gt $Max) { Stop-Process -Id $q.Id -Force; "qrenderdoc stopped after $Max s"; break }
}
Get-Content $log -ErrorAction SilentlyContinue
