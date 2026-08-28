[CmdletBinding()]
param(
    [string]$Version = '1.2.1.1',
    [string]$Publisher = 'CN=Vison2005'
)

$ErrorActionPreference = 'Stop'

$repositoryRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..'))
$workspaceRoot = Split-Path $repositoryRoot -Parent
$buildOutput = Join-Path $repositoryRoot 'src\VoiceSpreader.App\bin\Release\net9.0-windows10.0.26100.0\win-x64'
$distBase = [IO.Path]::GetFullPath((Join-Path $repositoryRoot 'dist'))
$distRoot = [IO.Path]::GetFullPath((Join-Path $repositoryRoot 'dist\release'))
$stagingRoot = [IO.Path]::GetFullPath((Join-Path $repositoryRoot 'dist\msix-layout'))
$signingRoot = [IO.Path]::GetFullPath((Join-Path $workspaceRoot '.signing\VoiceSpreader\windows'))
$makeAppx = 'D:\Windows Kits\10\bin\10.0.26100.0\x64\makeappx.exe'
$signTool = 'D:\Windows Kits\10\bin\10.0.26100.0\x64\signtool.exe'
$packageVersion = [Version]$Version
$releaseVersion = "{0}.{1}.{2}" -f $packageVersion.Major, $packageVersion.Minor, $packageVersion.Build
$packagePath = Join-Path $distRoot "VoiceSpreader-v$releaseVersion-x64.msix"
$certificatePath = Join-Path $distRoot "VoiceSpreader-v$releaseVersion.cer"
$pfxPath = Join-Path $signingRoot 'VoiceSpreader-CodeSigning.pfx'
$passwordPath = Join-Path $signingRoot 'VoiceSpreader-CodeSigning.password'

if (-not (Test-Path -LiteralPath $buildOutput -PathType Container)) {
    throw "Release 输出不存在，请先执行 .\build-winui.ps1 -Configuration Release"
}
foreach ($tool in @($makeAppx, $signTool)) {
    if (-not (Test-Path -LiteralPath $tool -PathType Leaf)) {
        throw "缺少 Windows SDK 工具：$tool"
    }
}

$safeDistPrefix = $distBase.TrimEnd([IO.Path]::DirectorySeparatorChar) + [IO.Path]::DirectorySeparatorChar
if (-not $stagingRoot.StartsWith($safeDistPrefix, [StringComparison]::OrdinalIgnoreCase)) {
    throw "暂存目录不在预期的 dist 目录内：$stagingRoot"
}
if (Test-Path -LiteralPath $stagingRoot) {
    Remove-Item -LiteralPath $stagingRoot -Recurse -Force
}
New-Item -ItemType Directory -Path $stagingRoot, $distRoot, $signingRoot -Force | Out-Null

Copy-Item -Path (Join-Path $buildOutput '*') -Destination $stagingRoot -Recurse -Force
Get-ChildItem -LiteralPath $stagingRoot -Filter '*.pdb' -File -Recurse | Remove-Item -Force

$assetsDirectory = Join-Path $stagingRoot 'Assets'
New-Item -ItemType Directory -Path $assetsDirectory -Force | Out-Null
$sourceLogo = Join-Path $repositoryRoot 'assets\VoiceSpreader.png'

Add-Type -AssemblyName System.Drawing
function Export-SquareLogo([int]$size, [string]$destination) {
    $source = [Drawing.Image]::FromFile($sourceLogo)
    $bitmap = [Drawing.Bitmap]::new($size, $size)
    $graphics = [Drawing.Graphics]::FromImage($bitmap)
    try {
        $graphics.Clear([Drawing.Color]::Transparent)
        $graphics.InterpolationMode = [Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
        $graphics.DrawImage($source, 0, 0, $size, $size)
        $bitmap.Save($destination, [Drawing.Imaging.ImageFormat]::Png)
    }
    finally {
        $graphics.Dispose()
        $bitmap.Dispose()
        $source.Dispose()
    }
}

Export-SquareLogo 44 (Join-Path $assetsDirectory 'Square44x44Logo.png')
Export-SquareLogo 150 (Join-Path $assetsDirectory 'Square150x150Logo.png')
Export-SquareLogo 50 (Join-Path $assetsDirectory 'StoreLogo.png')

[xml]$manifest = Get-Content (Join-Path $PSScriptRoot 'AppxManifest.xml') -Raw -Encoding UTF8
$namespace = [Xml.XmlNamespaceManager]::new($manifest.NameTable)
$namespace.AddNamespace('f', 'http://schemas.microsoft.com/appx/manifest/foundation/windows10')
$identity = $manifest.SelectSingleNode('/f:Package/f:Identity', $namespace)
$identity.SetAttribute('Publisher', $Publisher)
$identity.SetAttribute('Version', $Version)
$manifest.Save((Join-Path $stagingRoot 'AppxManifest.xml'))

if (-not (Test-Path -LiteralPath $pfxPath)) {
    $passwordBytes = [byte[]]::new(32)
    $generator = [Security.Cryptography.RandomNumberGenerator]::Create()
    try {
        $generator.GetBytes($passwordBytes)
    }
    finally {
        $generator.Dispose()
    }
    $plainPassword = [Convert]::ToBase64String($passwordBytes)
    [IO.File]::WriteAllText($passwordPath, $plainPassword)
    $securePassword = ConvertTo-SecureString $plainPassword -AsPlainText -Force
    $certificate = New-SelfSignedCertificate `
        -Type Custom `
        -Subject $Publisher `
        -FriendlyName 'VoiceSpreader private release signing' `
        -CertStoreLocation 'Cert:\CurrentUser\My' `
        -KeyAlgorithm RSA `
        -KeyLength 4096 `
        -KeyExportPolicy Exportable `
        -KeyUsage DigitalSignature `
        -HashAlgorithm SHA256 `
        -NotAfter (Get-Date).AddYears(10) `
        -TextExtension @('2.5.29.37={text}1.3.6.1.5.5.7.3.3', '2.5.29.19={text}')
    Export-PfxCertificate -Cert $certificate -FilePath $pfxPath -Password $securePassword | Out-Null
    Export-Certificate -Cert $certificate -FilePath $certificatePath | Out-Null
    & icacls.exe $signingRoot '/inheritance:r' '/grant:r' "${env:USERNAME}:(OI)(CI)F" | Out-Null
}
else {
    $plainPassword = [IO.File]::ReadAllText($passwordPath).Trim()
    $securePassword = ConvertTo-SecureString $plainPassword -AsPlainText -Force
    $certificate = [Security.Cryptography.X509Certificates.X509Certificate2]::new(
        $pfxPath,
        $plainPassword,
        [Security.Cryptography.X509Certificates.X509KeyStorageFlags]::PersistKeySet)
    if (-not (Get-ChildItem 'Cert:\CurrentUser\My' | Where-Object Thumbprint -EQ $certificate.Thumbprint)) {
        Import-PfxCertificate -FilePath $pfxPath -Password $securePassword -CertStoreLocation 'Cert:\CurrentUser\My' | Out-Null
    }
    [IO.File]::WriteAllBytes(
        $certificatePath,
        $certificate.Export([Security.Cryptography.X509Certificates.X509ContentType]::Cert))
}

if (Test-Path -LiteralPath $packagePath) {
    Remove-Item -LiteralPath $packagePath -Force
}
& $makeAppx pack /d $stagingRoot /p $packagePath /o
if ($LASTEXITCODE -ne 0) {
    throw "MakeAppx 失败，退出码 $LASTEXITCODE"
}
& $signTool sign /fd SHA256 /sha1 $certificate.Thumbprint /s My $packagePath
if ($LASTEXITCODE -ne 0) {
    throw "SignTool 签名失败，退出码 $LASTEXITCODE"
}
$trustedCertificatePath = "Cert:\CurrentUser\Root\$($certificate.Thumbprint)"
$certificateWasTrusted = Test-Path -LiteralPath $trustedCertificatePath
if (-not $certificateWasTrusted) {
    Import-Certificate `
        -FilePath $certificatePath `
        -CertStoreLocation 'Cert:\CurrentUser\Root' | Out-Null
}
try {
    & $signTool verify /pa /v $packagePath
    if ($LASTEXITCODE -ne 0) {
        throw "SignTool 验证失败，退出码 $LASTEXITCODE"
    }
}
finally {
    if (-not $certificateWasTrusted -and (Test-Path -LiteralPath $trustedCertificatePath)) {
        Remove-Item -LiteralPath $trustedCertificatePath -Force
    }
}

$hash = (Get-FileHash -LiteralPath $packagePath -Algorithm SHA256).Hash.ToLowerInvariant()
[IO.File]::WriteAllText("$packagePath.sha256", "$hash  $([IO.Path]::GetFileName($packagePath))`n")
Copy-Item (Join-Path $PSScriptRoot 'INSTALL.md') (Join-Path $distRoot 'WINDOWS-INSTALL.md') -Force

Write-Host "Windows release package: $packagePath"
Write-Host "Public certificate: $certificatePath"
