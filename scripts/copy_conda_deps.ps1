# Recursively copy conda-build Qt third-party DLL dependencies into a package dir.
# windeployqt does not know conda's layout and misses zlib/zstd/openssl/libpng etc,
# which makes the packaged app fail to start on other machines with
# "The code execution cannot proceed because xxx.dll was not found."
# This script resolves the real imports of every DLL/exe via dumpbin and copies
# missing dependencies from the conda directories, looping until nothing is new.
param(
    [string]$PackageDir = 'dist\VoiceSpreader-phone-control',
    [string[]]$CondaBins = @('D:\Anaconda\Library\bin', 'D:\Anaconda\Library', 'D:\Anaconda\DLLs')
)

$ErrorActionPreference = 'Stop'

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

Import-MsvcEnvironment
$dumpbin = (Get-Command dumpbin.exe -ErrorAction Stop).Source

$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$projectDir = Split-Path -Parent $scriptDir
$packageDir = if ([System.IO.Path]::IsPathRooted($PackageDir)) { $PackageDir } else {
    Join-Path $projectDir $PackageDir
}

# System DLLs never need packaging.
$sysdll = @('api-ms-win','ext-ms-win','advapi32','avicap32','avrt','bcrypt','cabinet','cfgmgr32','clbcatq','comctl32','comdlg32','crypt32','cryptbase','cryptnet','d3d','d3dcompiler','dbghelp','dcomp','dhcpcsvc','dinput8','dnsapi','dpapi','dsound','dwrite','dwmapi','dxcore','dxgi','gamingtcui','gdi32','gdiplus','glu32','hid','imagehlp','imm32','iphlpapi','iprop','kernel32','kernelbase','libEGL','libGLESv2','mfreadwrite','mfplat','mfuuid','msvcrt','msvfw32','mpr','msi','msiexec','msimg32','netapi32','normaliz','ntdll','ntmarta','odbc32','odbcbcp','ole32','oleacc','oleaut32','oledlg','opengl32','opengl32sw','pathcch','powrprof','propsys','psapi','quartz','rasapi32','rpcrt4','sechost','secur32','setupapi','shell32','shfolder','shlwapi','sspicli','strmbase','twinapi','ucrtbase','urlmon','user32','userenv','usp10','uxtheme','vcruntime','version','wbem','wbemcomn','wldap32','windows.storage','windowsapp','winhttp','wininet','winmm','winspool','wintrust','winusb','ws2_32','wsock32','wtsapi32','xinput','xolehlp','mf','quartz','amstream','d3d9','dwmcore')

$existing = @{}
Get-ChildItem -Path $packageDir -Recurse -File | ForEach-Object { $existing[$_.Name.ToLower()] = $true }

$queue = New-Object System.Collections.Generic.Queue[object]
Get-ChildItem -Path $packageDir -Recurse -File | Where-Object { $_.Extension -match '\.(dll|exe)$' } | ForEach-Object { $queue.Enqueue($_) }
$processed = @{}
$copiedCount = 0

while ($queue.Count -gt 0) {
    $file = $queue.Dequeue()
    $key = $file.FullName.ToLower()
    if ($processed.ContainsKey($key)) { continue }
    $processed[$key] = $true

    $depLines = & $dumpbin /dependents $file.FullName 2>$null |
        Select-String '\.dll$' |
        Where-Object { $_.Line.Trim() -notmatch '^Dump of file' } |
        ForEach-Object { $_.Line.Trim() }
    foreach ($dep in $depLines) {
        $depName = $dep.ToLower()
        if ($depName -eq $file.Name.ToLower()) { continue }
        if ($existing.ContainsKey($depName)) { continue }
        $skip = $false
        foreach ($s in $sysdll) { if ($depName.StartsWith($s)) { $skip = $true; break } }
        if ($skip) { continue }

        $found = $null
        foreach ($cb in $CondaBins) {
            $cand = Join-Path $cb $dep
            if (Test-Path -LiteralPath $cand -PathType Leaf) { $found = $cand; break }
        }
        if ($found) {
            Copy-Item -LiteralPath $found -Destination (Join-Path $packageDir $dep) -Force
            $existing[$depName] = $true
            $newItem = Get-Item (Join-Path $packageDir $dep)
            $queue.Enqueue($newItem)
            $copiedCount++
            Write-Host ('  + ' + $dep)
        } else {
            Write-Host ('  !! NOT FOUND: ' + $dep + ' (needed by ' + $file.Name + ')')
        }
    }
}
Write-Host ('--- Done. Copied ' + $copiedCount + ' DLLs into ' + $packageDir + ' ---')