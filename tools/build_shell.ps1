<#
.SYNOPSIS
    Build espfm_shell.py into a standalone executable using PyInstaller.

.DESCRIPTION
    Packages the ESPFM Interactive Shell with all dependencies (protobuf, rich,
    prompt_toolkit, zeroconf) and the companion espfm_pb2.py into a single .exe.

    Output: tools/dist/espfm_shell.exe

.PARAMETER Clean
    Remove build artifacts before building.

.EXAMPLE
    .\tools\build_shell.ps1
    .\tools\build_shell.ps1 -Clean
#>

& 'C:\Espressif\tools\Microsoft.v6.0.1.PowerShell_profile.ps1';

param(
    [switch]$Clean
)

$ErrorActionPreference = "Stop"

$ToolsDir = $PSScriptRoot
$ScriptPath = Join-Path $ToolsDir "espfm_shell.py"
$Pb2Path = Join-Path $ToolsDir "espfm_pb2.py"
$DistDir = Join-Path $ToolsDir "dist"
$BuildDir = Join-Path $ToolsDir "build"

# --- Verify prerequisites ---
if (-not (Test-Path $ScriptPath)) {
    Write-Error "espfm_shell.py not found at $ScriptPath"
    exit 1
}
if (-not (Test-Path $Pb2Path)) {
    Write-Error "espfm_pb2.py not found at $Pb2Path. Run tools/gen_proto.ps1 first."
    exit 1
}

# --- Install PyInstaller if missing ---
$pyinstaller = python -c "import PyInstaller; print(PyInstaller.__version__)" 2>$null
if (-not $pyinstaller) {
    Write-Host "Installing PyInstaller..." -ForegroundColor Yellow
    pip install pyinstaller
}

# --- Clean if requested ---
if ($Clean) {
    Write-Host "Cleaning build artifacts..." -ForegroundColor Yellow
    if (Test-Path $BuildDir) { Remove-Item -Recurse -Force $BuildDir }
    if (Test-Path $DistDir) { Remove-Item -Recurse -Force $DistDir }
    $specFile = Join-Path $ToolsDir "espfm_shell.spec"
    if (Test-Path $specFile) { Remove-Item -Force $specFile }
}

# --- Build ---
Write-Host "Building espfm_shell.exe..." -ForegroundColor Cyan

$pyinstallerArgs = @(
    "--onefile"
    "--name", "espfm_shell"
    "--distpath", $DistDir
    "--workpath", $BuildDir
    "--specpath", $ToolsDir
    "--paths", $ToolsDir          # add tools/ to sys.path so espfm_pb2 is found
    "--hidden-import", "espfm_pb2"
    "--collect-submodules", "rich"
    "--collect-submodules", "prompt_toolkit"
    "--collect-submodules", "zeroconf"
    "--console"                   # keep console window (it's a CLI tool)
    $ScriptPath
)

& python -m PyInstaller @pyinstallerArgs

if ($LASTEXITCODE -ne 0) {
    Write-Error "PyInstaller failed with exit code $LASTEXITCODE"
    exit $LASTEXITCODE
}

$exePath = Join-Path $DistDir "espfm_shell.exe"
if (Test-Path $exePath) {
    $size = (Get-Item $exePath).Length / 1MB
    Write-Host ""
    Write-Host "Build successful!" -ForegroundColor Green
    Write-Host "  Output: $exePath" -ForegroundColor Green
    Write-Host "  Size:   $([math]::Round($size, 1)) MB" -ForegroundColor Green
    Write-Host ""
    Write-Host "Usage: .\dist\espfm_shell.exe --host 192.168.0.22" -ForegroundColor Cyan
} else {
    Write-Error "Build completed but exe not found at $exePath"
    exit 1
}
