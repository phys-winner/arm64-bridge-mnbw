[CmdletBinding()]
param(
    [string]$NdkRoot = $env:ANDROID_NDK_ROOT,
    [switch]$SkipGuestChecks
)

$ErrorActionPreference = 'Stop'
$Here = Split-Path -Parent $MyInvocation.MyCommand.Path
$Repo = (Resolve-Path (Join-Path $Here '..\..\..')).Path
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
$ClangAarch64 = Join-Path $ToolBin 'aarch64-linux-android21-clang++.cmd'
$Objcopy = Join-Path $ToolBin 'llvm-objcopy.exe'
foreach ($Tool in @($Clang, $ClangAarch64, $Objcopy)) {
    if (-not (Test-Path -LiteralPath $Tool)) { throw "Missing tool: $Tool" }
}

New-Item -ItemType Directory -Force -Path $Build | Out-Null
$EntryObject = Join-Path $Build 'guest_settings_entry.o'
$PayloadObject = Join-Path $Build 'guest_settings_tab.o'
$BridgeProtocolObject = Join-Path $Build 'bridge_protocol_compile_test.o'
$Elf = Join-Path $Build 'guest_settings_tab.elf'
$Bin = Join-Path $Build 'guest_settings_tab.bin'
$Map = Join-Path $Build 'guest_settings_tab.map'
$Manifest = Join-Path $Build 'manifest.json'
$Linker = Join-Path $Here 'payload.ld'

$Common = @(
    '-march=armv7-a', '-marm', '-mfloat-abi=softfp', '-mfpu=neon',
    '-Oz', '-ffreestanding', '-fno-builtin', '-fno-stack-protector',
    '-fno-unwind-tables', '-fno-asynchronous-unwind-tables',
    '-fno-exceptions', '-fno-pic', '-fno-pie', '-fno-jump-tables',
    '-fdata-sections', '-ffunction-sections'
)

& $Clang @Common -c (Join-Path $Here 'guest_settings_entry.S') -o $EntryObject
if ($LASTEXITCODE -ne 0) { throw 'Assembly compilation failed.' }
& $Clang @Common '-std=c11' '-Wall' '-Wextra' '-Werror' -c (Join-Path $Here 'guest_settings_tab.c') -o $PayloadObject
if ($LASTEXITCODE -ne 0) { throw 'C compilation failed.' }
& $ClangAarch64 '-std=c++20' '-Wall' '-Wextra' '-Werror' '-fno-exceptions' '-fno-rtti' `
    -c (Join-Path $Here 'bridge_protocol_compile_test.cpp') -o $BridgeProtocolObject
if ($LASTEXITCODE -ne 0) { throw 'AArch64 bridge protocol compilation failed.' }

$MapOption = "-Wl,-Map,$Map"
$LinkerOption = "-Wl,-T,$Linker"
& $Clang @Common '-nostdlib' '-Wl,-no-pie,--build-id=none,--gc-sections' $MapOption $LinkerOption $EntryObject $PayloadObject -o $Elf
if ($LASTEXITCODE -ne 0) { throw 'Payload link failed.' }

& $Objcopy '--only-section=.payload' '-O' 'binary' $Elf $Bin
if ($LASTEXITCODE -ne 0) { throw 'Payload extraction failed.' }

$VerifyArgs = @(
    (Join-Path $Here 'verify_payload.py'),
    '--elf', $Elf,
    '--bin', $Bin,
    '--manifest', $Manifest
)
if (-not $SkipGuestChecks) {
    $VerifyArgs += @(
        '--guest', (Join-Path $Repo 'Mount_and_Blade_Warband_v1.071\lib\armeabi-v7a\libMBExpMobile.so'),
        '--guest', (Join-Path $Repo 'work\ida\ui_batch_20260912\libMBExpMobile.so')
    )
}
& python @VerifyArgs
if ($LASTEXITCODE -ne 0) { throw 'Static payload verification failed.' }

if (-not $SkipGuestChecks) {
    # The production staged guest already contains this feature and is not a
    # valid hook/payload preimage.  Combined-feature composition is exercised
    # by patch_mbexpmobile.py --self-test.
    & python (Join-Path $Here 'test_integration.py') '--elf' $Elf '--bin' $Bin '--guest' (Join-Path $Repo 'Mount_and_Blade_Warband_v1.071\lib\armeabi-v7a\libMBExpMobile.so') '--guest' (Join-Path $Repo 'work\ida\ui_batch_20260912\libMBExpMobile.so')
    if ($LASTEXITCODE -ne 0) { throw 'In-memory integration proof failed.' }

    & python (Join-Path $Here 'test_report_text_scale.py') '--ndk' $NdkRoot
    if ($LASTEXITCODE -ne 0) { throw 'Combat report scale proof failed.' }
}

Write-Host "Built $Bin"
Write-Host "Manifest $Manifest"
