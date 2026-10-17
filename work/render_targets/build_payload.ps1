[CmdletBinding()]
param(
    [string]$NdkRoot = $env:ANDROID_NDK_ROOT
)

$ErrorActionPreference = 'Stop'
$Here = Split-Path -Parent $MyInvocation.MyCommand.Path
$Build = Join-Path $Here 'build'

if (-not $NdkRoot) {
    $NdkParent = Join-Path $env:LOCALAPPDATA 'Android\Sdk\ndk'
    if (Test-Path -LiteralPath $NdkParent) {
        $NdkRoot = Get-ChildItem -LiteralPath $NdkParent -Directory |
            Sort-Object { [version]$_.Name } -Descending |
            Select-Object -First 1 -ExpandProperty FullName
    }
}
if (-not $NdkRoot -or -not (Test-Path -LiteralPath $NdkRoot)) {
    throw 'Android NDK not found. Pass -NdkRoot or set ANDROID_NDK_ROOT.'
}

$ToolBin = Join-Path $NdkRoot 'toolchains\llvm\prebuilt\windows-x86_64\bin'
$Clang = Join-Path $ToolBin 'armv7a-linux-androideabi21-clang.cmd'
$Objcopy = Join-Path $ToolBin 'llvm-objcopy.exe'
foreach ($Tool in @($Clang, $Objcopy)) {
    if (-not (Test-Path -LiteralPath $Tool)) { throw "Missing tool: $Tool" }
}

New-Item -ItemType Directory -Force -Path $Build | Out-Null
$Object = Join-Path $Build 'guest_postfx_rt_stub.o'
$Elf = Join-Path $Build 'guest_postfx_rt_stub.elf'
$Bin = Join-Path $Build 'guest_postfx_rt_stub.bin'
$Map = Join-Path $Build 'guest_postfx_rt_stub.map'
$Linker = Join-Path $Here 'payload.ld'

$Common = @(
    '-march=armv7-a', '-marm', '-mfloat-abi=softfp', '-mfpu=neon',
    '-Oz', '-ffreestanding', '-fno-builtin', '-fno-stack-protector',
    '-fno-unwind-tables', '-fno-asynchronous-unwind-tables',
    '-fno-exceptions', '-fno-pic', '-fno-pie'
)

& $Clang @Common -c (Join-Path $Here 'guest_postfx_rt_stub.S') -o $Object
if ($LASTEXITCODE -ne 0) { throw 'PostFX stub assembly failed.' }
& $Clang @Common '-nostdlib' '-Wl,-no-pie,--build-id=none,--gc-sections' `
    "-Wl,-Map,$Map" "-Wl,-T,$Linker" $Object -o $Elf
if ($LASTEXITCODE -ne 0) { throw 'PostFX stub link failed.' }
& $Objcopy '--only-section=.payload' '-O' 'binary' $Elf $Bin
if ($LASTEXITCODE -ne 0) { throw 'PostFX stub extraction failed.' }
& $Objcopy '--only-section=.hook_float_formats' '-O' 'binary' $Elf (Join-Path $Build 'hook_float_formats.bin')
if ($LASTEXITCODE -ne 0) { throw 'PostFX float-format hook extraction failed.' }
& $Objcopy '--only-section=.hook_single_channel_float_formats' '-O' 'binary' $Elf (Join-Path $Build 'hook_single_channel_float_formats.bin')
if ($LASTEXITCODE -ne 0) { throw 'PostFX single-channel float-format hook extraction failed.' }
& $Objcopy '--only-section=.hook_argb16' '-O' 'binary' $Elf (Join-Path $Build 'hook_argb16.bin')
if ($LASTEXITCODE -ne 0) { throw 'PostFX A16B16G16R16 hook extraction failed.' }
& $Objcopy '--only-section=.hook_render_nodes' '-O' 'binary' $Elf (Join-Path $Build 'hook_render_nodes.bin')
if ($LASTEXITCODE -ne 0) { throw 'PostFX render-node hook extraction failed.' }
& $Objcopy '--only-section=.hook_publish_effect' '-O' 'binary' $Elf (Join-Path $Build 'hook_publish_effect.bin')
if ($LASTEXITCODE -ne 0) { throw 'PostFX early effect hook extraction failed.' }

& python (Join-Path $Here 'verify_payload.py') '--elf' $Elf '--bin' $Bin '--hook-dir' $Build
if ($LASTEXITCODE -ne 0) { throw 'PostFX stub verification failed.' }

Write-Host "Built $Bin"
