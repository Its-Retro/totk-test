<#
.SYNOPSIS
  Starts the game from a finished build. The builder copies this file into the output folder; run it there
  (double-click output\play.cmd), or from the repository with -OutputDir.

.PARAMETER TextureBudgetMB
  Texture memory budget in MB (default 4096). Lower it (for example 2048) on GPUs with less than 8 GB.
.PARAMETER NoShaderWatcher
  Do not translate shaders met for the first time while playing (effects stay missing until the next start).
.PARAMETER Diagnostics
  Keep crash breadcrumbs and the flicker log on (slower; useful when reporting a crash).
#>
param([string]$OutputDir = '', [int]$TextureBudgetMB = 4096, [switch]$NoShaderWatcher, [switch]$Diagnostics)
$ErrorActionPreference = 'Stop'
$root = if ($OutputDir) { [IO.Path]::GetFullPath($OutputDir) } elseif (Test-Path (Join-Path $PSScriptRoot 'startup\totk.exe')) { $PSScriptRoot } else { Join-Path $PSScriptRoot 'output' }
$exe = Join-Path $root 'startup\totk.exe'
$dll = Join-Path $root 'startup\totk-play.dll'
if (-not (Test-Path $exe) -or -not (Test-Path $dll)) { Write-Host "No finished build in $root (run build.cmd first)." -ForegroundColor Red; exit 1 }
if (Get-Process totk -ErrorAction SilentlyContinue) { Write-Host 'The game is already running.'; exit 1 }

# The tested play settings (the development boot script's performance profile).
Remove-Item Env:TOTK_NVN, Env:TOTK_SHADER_MANIFEST, Env:TOTK_TILE_BIND_TRACE, Env:TOTK_CHAIN_TRACE, Env:TOTK_DRAW_CENSUS, Env:TOTK_GPU_PROFILE -ErrorAction SilentlyContinue
$env:TOTK_THREADS = 'host'; $env:TOTK_FASTMEM = '1'; $env:TOTK_DIRECT_CALLS = '1'
$env:TOTK_TEX_BUDGET_MB = "$TextureBudgetMB"; $env:TOTK_TEX_VERIFY_MB = '16'
$env:TOTK_GUEST_CLOCK = 'wall'; $env:TOTK_PRESENT_HOLD = '0'; $env:TOTK_FRAME_LOCK = '0'; $env:TOTK_WAIT_GRAPH = '0'
$env:TOTK_SYNC_WAIT_FENCE = '1'; $env:TOTK_PRESENT_TRACE = '1'
$env:TOTK_PROVOKING_FIRST = '0'; $env:TOTK_NVN_WAIT_TRACE = '0'; $env:TOTK_RT_WINDOW_TRACE = '0'
if (-not $env:TOTK_MOVIE_PACE) { $env:TOTK_MOVIE_PACE = 'game' }
if (-not $env:TOTK_OVERLAP) { $env:TOTK_OVERLAP = '1' }
if (-not $env:TOTK_MT_CPUSETS) { $env:TOTK_MT_CPUSETS = 'p' }          # P-cores on hybrid CPUs; no effect on others
if (-not $env:TOTK_SLOT_VERSIONING) { $env:TOTK_SLOT_VERSIONING = '0' }
if ($Diagnostics) { $env:TOTK_DRED_MARKERS = '1'; $env:TOTK_FLICKER_LOG = '1' }
else { $env:TOTK_DRED_MARKERS = '0'; $env:TOTK_FLICKER_LOG = '0'; $env:TOTK_FRAME_DUMP_EVERY = '0' }

$logs = Join-Path $root 'logs'; New-Item -ItemType Directory -Force $logs | Out-Null
$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
Write-Host "Starting the game (logs: $logs\play-$stamp-*.log)"
$game = Start-Process -FilePath $exe -ArgumentList "`"$dll`"" -WorkingDirectory $root -PassThru `
    -RedirectStandardOutput (Join-Path $logs "play-$stamp-stdout.log") -RedirectStandardError (Join-Path $logs "play-$stamp-stderr.log")

if (-not $NoShaderWatcher) {
    try {
        $py = (Get-Content (Join-Path $root 'play-settings.json') -Raw | ConvertFrom-Json).python
        if (-not [IO.Path]::IsPathRooted($py)) { $py = Join-Path $root $py }
        $env:PYTHONNOUSERSITE = '1'
        Start-Process -FilePath $py -ArgumentList @('-B', "`"$(Join-Path $root 'tools\shader_watch.py')`"", "`"$root`"", '--pid', "$($game.Id)") -WindowStyle Hidden | Out-Null
        Write-Host 'Shader watcher running: shaders met for the first time are translated in the background.'
    } catch { Write-Host "Shader watcher not started ($($_.Exception.Message)); the game runs without it." }
}
$game.WaitForExit()
$stop = Join-Path $root 'totk-stop.txt'
if ($game.ExitCode -eq 0) { Write-Host 'The game window was closed.' }
else {
    Write-Host "The game stopped (exit code $($game.ExitCode))." -ForegroundColor Yellow
    if (Test-Path $stop) { Write-Host ('Last stop: ' + (Get-Content $stop -Tail 1)) }
    Write-Host "When reporting it, include $logs\play-$stamp-stderr.log and totk-stop.txt."
}
