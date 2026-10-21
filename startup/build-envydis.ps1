# Offline disassembler only. No hardware access or emulator runtime is built.
$ErrorActionPreference = 'Stop'
$taskRoot = Split-Path -Parent $PSScriptRoot
$taskVendor = Join-Path $taskRoot 'tools/envytools'
$taskCommit = git -C $taskVendor rev-parse HEAD
if ($LASTEXITCODE -ne 0 -or $taskCommit -ne 'f102b82381f3f11cee113d16374c87091db039d9') { throw 'Unreviewed envytools revision' }
$taskDis = @('core','core-as','core-dis','g80','gf100','gk110','gm107','ctx','falcon','hwsq','xtensa','vuc','macro','vp1','vcomp','envydis') | ForEach-Object { Join-Path $taskVendor "envydis/$_.c" }
$taskUtil = @('mask','hash','symtab','colors','astr','aprintf','vardata','varinfo','varselect') | ForEach-Object { Join-Path $taskVendor "util/$_.c" }
$taskEasm = @('easm','easm_print','easm_xfrm') | ForEach-Object { Join-Path $taskVendor "easm/$_.c" }
& (Join-Path $taskRoot 'tools/zig-x86_64-windows-0.16.0/zig.exe') cc -target x86_64-windows-gnu -std=gnu11 -O2 -Wno-missing-braces "-I$taskVendor/include" "-I$taskVendor/envydis" @taskDis @taskUtil @taskEasm -o (Join-Path $taskRoot 'tools/envydis.exe')
if ($LASTEXITCODE -ne 0) { throw 'Offline disassembler build failed' }
