# Run one of the RenderDoc scripts in this folder inside qrenderdoc (--ui-python: no window):
#   rdoc.ps1 <script> <params.json> [-Max <s>]
# The script reads its parameters from the JSON file named by RDOC_PARAMS, writes its results into
# the folder the parameters' "out" names, and logs to <out>\<script>.log, which is printed here.
# RenderDoc is $env:RENDERDOC_HOME, else %USERPROFILE%\tools\RenderDoc_1.47_64 (the portable zip).
param([Parameter(Mandatory = $true)][string]$Script, [Parameter(Mandatory = $true)][string]$Params,
      [int]$Max = 900)
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
if (-not $q.WaitForExit($Max * 1000)) { Stop-Process -Id $q.Id -Force; "qrenderdoc stopped after $Max s" }
Get-Content $log -ErrorAction SilentlyContinue
