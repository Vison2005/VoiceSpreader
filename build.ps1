[CmdletBinding()]
param(
    [string]$QtRoot = 'D:\Anaconda\Library',
    [string]$PackageDir = 'dist\VoiceSpreader'
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$projectDir = (Resolve-Path -LiteralPath (Split-Path -Parent $MyInvocation.MyCommand.Path)).Path
$buildDir = Join-Path $projectDir 'build'
$packageDir = if ([System.IO.Path]::IsPathRooted($PackageDir)) {
    $PackageDir
} else {
    Join-Path $projectDir $PackageDir
}

function Import-MsvcEnvironment {
    $vswhere = 'C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe'
    if (-not (Test-Path -LiteralPath $vswhere -PathType Leaf)) {
        throw 'vswhere.exe was not found. Install the Visual Studio 2022 C++ toolchain.'
    }

    $installPath = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if ($LASTEXITCODE -ne 0 -or -not $installPath) {
        throw 'A Visual Studio installation with the C++ toolchain was not found.'
    }

    $devCommand = Join-Path $installPath.Trim() 'Common7\Tools\VsDevCmd.bat'
    $environmentLines = & cmd.exe /d /c "`"$devCommand`" -no_logo -arch=x64 -host_arch=x64 && set"
    if ($LASTEXITCODE -ne 0) {
        throw 'Visual Studio developer environment initialization failed.'
    }

    foreach ($line in $environmentLines) {
        if ($line -match '^([^=]+)=(.*)$') {
            [Environment]::SetEnvironmentVariable($matches[1], $matches[2], 'Process')
        }
    }
}

$qtConfig = Join-Path $QtRoot 'lib\cmake\Qt5\Qt5Config.cmake'
$deployTool = Join-Path $QtRoot 'bin\windeployqt.exe'
if (-not (Test-Path -LiteralPath $qtConfig -PathType Leaf)) {
    throw "Qt 5 CMake configuration was not found: $qtConfig"
}
if (-not (Test-Path -LiteralPath $deployTool -PathType Leaf)) {
    throw "windeployqt.exe was not found: $deployTool"
}

Import-MsvcEnvironment

$cmake = (Get-Command cmake.exe -ErrorAction Stop).Source
$ninja = (Get-Command ninja.exe -ErrorAction Stop).Source
Write-Host '=== VoiceSpreader Build ===' -ForegroundColor Cyan
Write-Host "Project: $projectDir"
Write-Host "Qt: $QtRoot"
Write-Host "CMake: $cmake"
Write-Host "Ninja: $ninja"

& $cmake -S $projectDir -B $buildDir -G Ninja -DCMAKE_BUILD_TYPE=Release "-DCMAKE_PREFIX_PATH=$QtRoot"
if ($LASTEXITCODE -ne 0) {
    throw "CMake configuration failed with exit code: $LASTEXITCODE"
}

& $cmake --build $buildDir --config Release --parallel
if ($LASTEXITCODE -ne 0) {
    throw "C++ compilation failed with exit code: $LASTEXITCODE"
}

$builtExe = Join-Path $buildDir 'VoiceSpreader.exe'
if (-not (Test-Path -LiteralPath $builtExe -PathType Leaf)) {
    throw "The build did not produce the expected executable: $builtExe"
}

New-Item -ItemType Directory -Path $packageDir -Force | Out-Null
$packagedExe = Join-Path $packageDir 'VoiceSpreader.exe'
Copy-Item -LiteralPath $builtExe -Destination $packagedExe -Force

& $deployTool --release --force --compiler-runtime --no-translations --no-system-d3d-compiler --dir $packageDir $packagedExe
if ($LASTEXITCODE -ne 0) {
    throw "windeployqt failed with exit code: $LASTEXITCODE"
}

Copy-Item -LiteralPath (Join-Path $projectDir 'README.md') -Destination (Join-Path $packageDir 'README.md') -Force

$licenseDir = Join-Path $packageDir 'licenses'
New-Item -ItemType Directory -Path $licenseDir -Force | Out-Null
Copy-Item -LiteralPath (Join-Path $projectDir 'assets\fonts\HarmonyOS_Sans_LICENSE.txt') `
    -Destination (Join-Path $licenseDir 'HarmonyOS_Sans_LICENSE.txt') -Force

$exeInfo = Get-Item -LiteralPath $packagedExe
Write-Host ''
Write-Host 'Build succeeded.' -ForegroundColor Green
Write-Host "EXE: $($exeInfo.FullName)"
Write-Host ('EXE size: {0:N2} MB' -f ($exeInfo.Length / 1MB))
