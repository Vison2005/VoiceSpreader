@echo off
setlocal

set "ROOT=%~dp0"
set "PACKAGE=%ROOT%SoundSpreader-v{{VERSION}}-x64.msix"
set "HASH_FILE=%PACKAGE%.sha256"
set "CERT=%ROOT%SoundSpreader-v{{VERSION}}.cer"

if not exist "%PACKAGE%" (
  echo Missing package: %PACKAGE%
  exit /b 1
)
if not exist "%HASH_FILE%" (
  echo Missing SHA-256 file: %HASH_FILE%
  exit /b 1
)
if not exist "%CERT%" (
  echo Missing certificate: %CERT%
  exit /b 1
)

echo Verifying SHA-256...
powershell.exe -NoProfile -ExecutionPolicy Bypass -Command "$expected=(Get-Content -Raw -LiteralPath '%HASH_FILE%').Trim().Split()[0].ToLowerInvariant(); $actual=(Get-FileHash -Algorithm SHA256 -LiteralPath '%PACKAGE%').Hash.ToLowerInvariant(); if ($actual -ne $expected) { throw ('SHA-256 mismatch. Expected ' + $expected + ', got ' + $actual) }"
if errorlevel 1 (
  echo SHA-256 verification failed.
  exit /b 1
)

echo Importing the self-signed certificate and installing SoundSpreader...
powershell.exe -NoProfile -ExecutionPolicy Bypass -Command "Import-Certificate -FilePath '%CERT%' -CertStoreLocation 'Cert:\LocalMachine\TrustedPeople' | Out-Null; Add-AppxPackage -Path '%PACKAGE%'"
if errorlevel 1 (
  echo Installation failed. Please run this file as administrator.
  exit /b 1
)

echo SoundSpreader v{{VERSION}} installed successfully.
exit /b 0
