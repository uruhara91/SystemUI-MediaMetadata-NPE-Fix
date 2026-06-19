param(
    [string]$NdkPath = $env:ANDROID_NDK_HOME,
    [switch]$NoClean
)

$ErrorActionPreference = 'Stop'
$Root = Split-Path -Parent $MyInvocation.MyCommand.Path
$ZygiskDir = Join-Path $Root 'zygisk'
$ModuleDir = Join-Path $Root 'module'
$ModuleProp = Join-Path $ModuleDir 'module.prop'
$OutputDir = Join-Path $Root 'dist'
$ModuleZygiskDir = Join-Path $ModuleDir 'zygisk'

$VersionLine = Get-Content $ModuleProp | Where-Object { $_ -like 'version=*' } | Select-Object -First 1
if ([string]::IsNullOrWhiteSpace($VersionLine)) {
    throw "Could not read version from $ModuleProp"
}
$Version = $VersionLine.Substring('version='.Length).Trim()

if ([string]::IsNullOrWhiteSpace($NdkPath)) {
    $NdkPath = $env:ANDROID_NDK_ROOT
}
if ([string]::IsNullOrWhiteSpace($NdkPath)) {
    throw 'Set ANDROID_NDK_HOME or pass -NdkPath C:\path\to\ndk.'
}

$NdkBuild = Join-Path $NdkPath 'ndk-build.cmd'
if (-not (Test-Path $NdkBuild)) {
    $NdkBuild = Join-Path $NdkPath 'ndk-build'
}
if (-not (Test-Path $NdkBuild)) {
    throw "ndk-build was not found under $NdkPath"
}

if (-not $NoClean) {
    & $NdkBuild '-C' $ZygiskDir 'clean'
    if ($LASTEXITCODE -ne 0) { throw "ndk-build clean failed: $LASTEXITCODE" }
}

$Jobs = [Math]::Max(1, [Environment]::ProcessorCount)
& $NdkBuild '-C' $ZygiskDir "-j$Jobs"
if ($LASTEXITCODE -ne 0) { throw "ndk-build failed: $LASTEXITCODE" }

Remove-Item $ModuleZygiskDir -Recurse -Force -ErrorAction SilentlyContinue
New-Item $ModuleZygiskDir -ItemType Directory -Force | Out-Null

$Libraries = @{
    'arm64-v8a' = 'arm64-v8a.so'
    'armeabi-v7a' = 'armeabi-v7a.so'
}
foreach ($Abi in $Libraries.Keys) {
    $Source = Join-Path $ZygiskDir "libs\$Abi\libsystemui_media_fix.so"
    if (-not (Test-Path $Source)) { throw "Missing compiled library: $Source" }
    Copy-Item $Source (Join-Path $ModuleZygiskDir $Libraries[$Abi]) -Force
}

New-Item $OutputDir -ItemType Directory -Force | Out-Null
$ZipPath = Join-Path $OutputDir "SystemUI-Media-Fix-$Version.zip"
Remove-Item $ZipPath -Force -ErrorAction SilentlyContinue
Compress-Archive -Path (Join-Path $ModuleDir '*') -DestinationPath $ZipPath -CompressionLevel Optimal

Write-Host "Built module: $ZipPath"
