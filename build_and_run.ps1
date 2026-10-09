<#
.SYNOPSIS
    Builds RPCS3 (Release|x64) and launches it.

.PARAMETER NoRun
    Build only, don't launch rpcs3.exe afterwards.

.PARAMETER NoBuild
    Skip the build, just launch the existing rpcs3.exe.
#>
param(
    [switch]$NoRun,
    [switch]$NoBuild
)

$ErrorActionPreference = "Stop"

$RepoRoot = $PSScriptRoot
$MSBuild  = "C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe"
$Sln      = Join-Path $RepoRoot "rpcs3.sln"
$Exe      = Join-Path $RepoRoot "bin\rpcs3.exe"
$LogFile  = Join-Path $RepoRoot "build_release.log"

$env:QTDIR       = "C:\Qt\6.10.2\msvc2022_64"
$env:VULKAN_SDK  = "S:\DEV\vcpkg-tmp\installed\x64-windows"

if (-not $NoBuild) {
    # rpcs3.exe locks its own file while running, which breaks the link step - make sure it's not running
    Get-Process rpcs3 -ErrorAction SilentlyContinue | Stop-Process -Force

    if (Test-Path $LogFile) {
        Remove-Item $LogFile -Force
    }

    Write-Host "Building $Sln (Release|x64)..." -ForegroundColor Cyan

    & $MSBuild $Sln /t:rpcs3 /p:Configuration=Release /p:Platform=x64 /m /v:minimal /fl "/flp:logfile=$LogFile;verbosity=normal"

    if ($LASTEXITCODE -ne 0) {
        Write-Host "Build FAILED (exit code $LASTEXITCODE). See $LogFile" -ForegroundColor Red
        Write-Host "--- Errors ---" -ForegroundColor Red
        Select-String -Path $LogFile -Pattern ': error' | ForEach-Object { Write-Host $_.Line -ForegroundColor Red }
        exit $LASTEXITCODE
    }

    Write-Host "Build succeeded." -ForegroundColor Green
}

if (-not $NoRun) {
    if (-not (Test-Path $Exe)) {
        Write-Host "rpcs3.exe not found at $Exe" -ForegroundColor Red
        exit 1
    }

    Write-Host "Launching rpcs3.exe..." -ForegroundColor Cyan
    Start-Process -FilePath $Exe -WorkingDirectory (Join-Path $RepoRoot "bin")
}
