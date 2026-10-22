<#
.SYNOPSIS
  Builds the totk-test native Windows port from your own extracted game files, in one command.

.DESCRIPTION
  Finds Python 3.12 (or downloads a private copy into the output folder), installs the pinned Python packages
  there, then runs builder\build.py, which does everything else: checks your files, gets the compilers and
  libraries, decompresses and translates the game code, translates the shaders, and compiles the game.
  Nothing is installed system-wide. Everything generated goes into the output folder.

.EXAMPLE
  .\build.cmd -ExeFS D:\dumps\totk\exefs -RomFS D:\dumps\totk\romfs

.EXAMPLE
  .\build.cmd -ExeFS D:\dumps\totk\exefs -RomFS D:\dumps\totk\romfs -CheckInputs
  Only checks that your files are the right version (takes a few minutes).
#>
param(
    [Parameter(Mandatory = $true)][string]$ExeFS,
    [Parameter(Mandatory = $true)][string]$RomFS,
    [string]$OutputDir = '',
    [int]$Workers = 0,
    [string]$From = '',
    [string]$Only = '',
    [switch]$CheckInputs,
    [switch]$AllowRomfsMismatch,
    [switch]$DryRun
)
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'      # the PowerShell 5 progress bar makes downloads very slow
$repo = $PSScriptRoot
if (-not $OutputDir) { $OutputDir = Join-Path $repo 'output' }
$OutputDir = [IO.Path]::GetFullPath($OutputDir)
$tools = Join-Path $OutputDir 'tools'
New-Item -ItemType Directory -Force $tools | Out-Null
[Net.ServicePointManager]::SecurityProtocol = [Net.ServicePointManager]::SecurityProtocol -bor [Net.SecurityProtocolType]::Tls12
$env:PYTHONNOUSERSITE = '1'                   # ignore packages installed in the user's own Python folder

# The private Python is python.org's NuGet package (a plain folder, no installer, no admin rights).
$pyVersion = '3.12.10'
$pyUrl = "https://api.nuget.org/v3-flatcontainer/python/$pyVersion/python.$pyVersion.nupkg"
$pySha512 = 'u9pNz2iKlCEbYtUJaKkbOPMF0LjR7NkCafdKhvigpPzrt8oWKgdTpHaR6z3wyWQAm9PYGUxv0Zr66NX9AeHMDw=='
$privatePy = Join-Path $tools "python-$pyVersion\python.exe"
$venvPy = Join-Path $tools 'venv\Scripts\python.exe'

function Say([string]$text) { Write-Host $text }
function Stop-Build([string]$text) { Write-Host ''; Write-Host "STOPPED: $text" -ForegroundColor Red; exit 1 }

# Runs a program with its error output discarded and returns @{Code; Out}. (Windows PowerShell 5.1 turns a
# redirected native error stream into script errors, so the preference is relaxed while it runs.)
function Invoke-Quiet([string]$exe, [string[]]$argv) {
    $old = $ErrorActionPreference; $ErrorActionPreference = 'Continue'
    try { $out = & $exe @argv 2>$null; return @{ Code = $LASTEXITCODE; Out = "$out".Trim() } }
    catch { return @{ Code = 1; Out = '' } }
    finally { $ErrorActionPreference = $old }
}

function Test-Python312([string]$exe, [string[]]$pre = @()) {
    $r = Invoke-Quiet $exe ($pre + @('-c', "import sys,struct;print('%d.%d-%d' % (sys.version_info[0], sys.version_info[1], struct.calcsize('P')*8))"))
    return ($r.Code -eq 0 -and $r.Out -eq '3.12-64')
}

function Get-PrivatePython {
    $downloads = Join-Path $OutputDir 'downloads'
    New-Item -ItemType Directory -Force $downloads | Out-Null
    $pkg = Join-Path $downloads "python.$pyVersion.nupkg"
    $vendorPkg = Join-Path $repo "vendor\python.$pyVersion.nupkg"
    if (Test-Path $vendorPkg) { Say "  Using the bundled Python $pyVersion (vendor folder)"; Copy-Item $vendorPkg $pkg -Force }
    else {
    Say "  Downloading Python $pyVersion (python.org NuGet package, about 15 MB)"
    try { Invoke-WebRequest -Uri $pyUrl -OutFile $pkg -UseBasicParsing }
    catch { Stop-Build "Could not download Python from $pyUrl ($($_.Exception.Message)). Check the internet connection, or install Python 3.12 (64-bit) from https://www.python.org/downloads/windows/ and run this again." }
    }
    $sha = [Security.Cryptography.SHA512]::Create()
    $fs = [IO.File]::OpenRead($pkg)
    try { $got = [Convert]::ToBase64String($sha.ComputeHash($fs)) } finally { $fs.Close() }
    if ($got -ne $pySha512) { Remove-Item $pkg; Stop-Build "The Python download does not have the expected SHA-512 (got $got). Run the command again." }
    $tmp = Join-Path $downloads 'python-unpack'
    if (Test-Path $tmp) { Remove-Item -Recurse -Force $tmp }
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    [IO.Compression.ZipFile]::ExtractToDirectory($pkg, $tmp)
    $target = Join-Path $tools "python-$pyVersion"
    if (Test-Path $target) { Remove-Item -Recurse -Force $target }
    Move-Item (Join-Path $tmp 'tools') $target
    Remove-Item -Recurse -Force $tmp
    if (-not (Test-Python312 $privatePy)) { Stop-Build "The downloaded Python does not run ($privatePy)." }
}

Say 'totk-test: getting Python 3.12 ready'
$py = $null
if (Test-Path $privatePy) { if (Test-Python312 $privatePy) { $py = $privatePy } }
if (-not $py -and (Test-Path $venvPy)) { if (Test-Python312 $venvPy) { $py = $venvPy } }
if (-not $py) {
    # A Python 3.12 that is already installed: make a private virtual environment from it.
    $base = $null
    if (Get-Command py -ErrorAction SilentlyContinue) {
        if (Test-Python312 'py' @('-3.12')) { $base = (Invoke-Quiet 'py' @('-3.12', '-c', 'import sys;print(sys.executable)')).Out }
    }
    if (-not $base) {
        $cmd = Get-Command python -ErrorAction SilentlyContinue
        if ($cmd -and $cmd.Source -notlike '*\WindowsApps\*' -and (Test-Python312 $cmd.Source)) { $base = $cmd.Source }
    }
    if ($base) {
        Say "  Using the installed Python 3.12 ($base) for a private environment in $tools\venv"
        & $base -m venv (Join-Path $tools 'venv')
        if ($LASTEXITCODE -eq 0 -and (Test-Python312 $venvPy)) { $py = $venvPy }
    }
}
if (-not $py) { Get-PrivatePython; $py = $privatePy }
Say "  Python: $py"

# Pinned packages (version and wheel hash): capstone, zstandard, numpy.
$probe = Invoke-Quiet $py @('-c', "import capstone,zstandard,numpy,sys; sys.exit(0 if (capstone.__version__,zstandard.__version__,numpy.__version__)==('5.0.7','0.25.0','2.3.5') else 1)")
if ($probe.Code -ne 0) {
    Say '  Installing the pinned Python packages (capstone 5.0.7, zstandard 0.25.0, numpy 2.3.5)'
    if ((Invoke-Quiet $py @('-m', 'pip', '--version')).Code -ne 0) { Invoke-Quiet $py @('-m', 'ensurepip', '--default-pip') | Out-Null }
    $wheels = Join-Path $repo 'vendor\wheels'
    $offline = if (Test-Path $wheels) { @('--no-index', '--find-links', $wheels) } else { @() }   # bundled wheels: no internet
    & $py -m pip install --disable-pip-version-check --no-input --require-hashes --only-binary=:all: @offline -r (Join-Path $repo 'builder\requirements.txt')
    if ($LASTEXITCODE -ne 0) { Stop-Build 'Installing the Python packages failed (see above). Check the internet connection and run the command again.' }
}

$buildArgs = @('-B', (Join-Path $repo 'builder\build.py'), '--exefs', $ExeFS, '--romfs', $RomFS, '--out', $OutputDir)
if ($Workers -gt 0) { $buildArgs += @('--workers', "$Workers") }
if ($From) { $buildArgs += @('--from', $From) }
if ($Only) { $buildArgs += @('--only', $Only) }
if ($CheckInputs) { $buildArgs += '--check-inputs' }
if ($AllowRomfsMismatch) { $buildArgs += '--allow-romfs-mismatch' }
if ($DryRun) { $buildArgs += '--dry-run' }
Say ''
& $py @buildArgs
exit $LASTEXITCODE
