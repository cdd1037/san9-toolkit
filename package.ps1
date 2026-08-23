[CmdletBinding()]
param(
    [string]$CoreUiRoot = ''
)

$ErrorActionPreference = 'Stop'
$toolkitRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$version = (Get-Content -LiteralPath (Join-Path $toolkitRoot 'VERSION') -Raw).Trim()
if ($version -notmatch '^\d+\.\d+\.\d+$') { throw "无效版本号：$version" }

$buildArguments = @{ Configuration = 'Release' }
if (-not [string]::IsNullOrWhiteSpace($CoreUiRoot)) {
    $buildArguments.CoreUiRoot = $CoreUiRoot
}
& (Join-Path $toolkitRoot 'build.ps1') @buildArguments
if ($LASTEXITCODE -ne 0) { throw 'Release 构建失败。' }

$sourceRoot = Join-Path $toolkitRoot 'dist\Release'
$packageParent = [IO.Path]::GetFullPath((Join-Path $toolkitRoot 'build\package'))
$packageName = "San9Toolkit-v$version"
$packageRoot = [IO.Path]::GetFullPath((Join-Path $packageParent $packageName))
if ([IO.Directory]::GetParent($packageRoot).FullName -ne $packageParent) {
    throw "不安全的打包目录：$packageRoot"
}
if (Test-Path -LiteralPath $packageRoot) {
    [IO.Directory]::Delete($packageRoot, $true)
}

$packageX86 = Join-Path $packageRoot 'bin\x86'
$packageX64 = Join-Path $packageRoot 'bin\x64'
$packageUi = Join-Path $packageRoot 'ui'
$packageData = Join-Path $packageRoot 'data'
foreach ($directory in @($packageX86, $packageX64, $packageUi, $packageData)) {
    [IO.Directory]::CreateDirectory($directory) | Out-Null
}

Copy-Item -LiteralPath (Join-Path $sourceRoot 'San9Toolkit.exe') -Destination $packageRoot
Copy-Item -LiteralPath (Join-Path $sourceRoot 'bin\x86\San9Toolkit.Bootstrap.exe') -Destination $packageX86
Copy-Item -LiteralPath (Join-Path $sourceRoot 'bin\x86\San9Toolkit.Runtime.dll') -Destination $packageX86
Copy-Item -LiteralPath (Join-Path $sourceRoot 'bin\x64\core-ui.dll') -Destination $packageX64
Copy-Item -LiteralPath (Join-Path $sourceRoot 'ui\app.uix') -Destination $packageUi
Copy-Item -LiteralPath (Join-Path $toolkitRoot 'San9Toolkit.ini.example') -Destination $packageData
Copy-Item -LiteralPath (Join-Path $toolkitRoot 'README.md') -Destination $packageRoot
Copy-Item -LiteralPath (Join-Path $toolkitRoot 'THIRD_PARTY_NOTICES.md') -Destination $packageRoot

$archivePath = Join-Path $toolkitRoot "dist\San9Toolkit-v$version-windows.zip"
$checksumPath = "$archivePath.sha256"
foreach ($file in @($archivePath, $checksumPath)) {
    if (Test-Path -LiteralPath $file) { Remove-Item -LiteralPath $file -Force }
}
Compress-Archive -LiteralPath $packageRoot -DestinationPath $archivePath -CompressionLevel Optimal
$hash = (Get-FileHash -LiteralPath $archivePath -Algorithm SHA256).Hash.ToLowerInvariant()
[IO.File]::WriteAllText($checksumPath, "$hash  $([IO.Path]::GetFileName($archivePath))`n",
                        [Text.UTF8Encoding]::new($false))
Write-Host "发布包：$archivePath"
Write-Host "SHA-256：$hash"
