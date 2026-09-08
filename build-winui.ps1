[CmdletBinding()]
param(
    [ValidateSet('Debug', 'Release')]
    [string]$Configuration = 'Debug',
    [string]$QtRoot = 'D:\Anaconda\Library',
    [switch]$Run
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$projectDir = (Resolve-Path -LiteralPath (Split-Path -Parent $MyInvocation.MyCommand.Path)).Path
$appProject = Join-Path $projectDir 'src\VoiceSpreader.App\VoiceSpreader.App.csproj'
$nativeBuildDir = Join-Path $projectDir 'build-msvc-v110'

& (Join-Path $projectDir 'build.ps1') -QtRoot $QtRoot -SkipPackage
if ($LASTEXITCODE -ne 0) {
    throw "Native build failed with exit code: $LASTEXITCODE"
}

& dotnet build $appProject -c $Configuration -r win-x64
if ($LASTEXITCODE -ne 0) {
    throw "WinUI build failed with exit code: $LASTEXITCODE"
}

$outputDir = Join-Path $projectDir "src\VoiceSpreader.App\bin\$Configuration\net9.0-windows10.0.26100.0\win-x64"
$nativeDll = Join-Path $nativeBuildDir 'VoiceSpreader.Native.dll'
if (-not (Test-Path -LiteralPath $nativeDll -PathType Leaf)) {
    throw "Native bridge was not produced: $nativeDll"
}

Copy-Item -LiteralPath $nativeDll -Destination (Join-Path $outputDir 'VoiceSpreader.Native.dll') -Force
Copy-Item -LiteralPath (Join-Path $QtRoot 'bin\Qt5Core_conda.dll') -Destination $outputDir -Force

& (Join-Path $projectDir 'scripts\copy_conda_deps.ps1') `
    -PackageDir $outputDir `
    -CondaBins @((Join-Path $QtRoot 'bin'), $QtRoot)
if ($LASTEXITCODE -ne 0) {
    throw "Native dependency deployment failed with exit code: $LASTEXITCODE"
}

$executable = Join-Path $outputDir 'SoundSpreader.exe'
Write-Host ''
Write-Host 'WinUI build succeeded.' -ForegroundColor Green
Write-Host "EXE: $executable"

if ($Run) {
    Start-Process -FilePath $executable -WorkingDirectory $outputDir
}
