[CmdletBinding()]
param(
    [ValidateSet('Debug', 'Release')]
    [string]$Configuration = 'Release',
    [switch]$Loose
)

$ErrorActionPreference = 'Stop'

$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if (-not (Test-Path -LiteralPath $vswhere)) {
    throw '未找到 Visual Studio Installer 的 vswhere.exe。'
}

$installationPath = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $installationPath) {
    throw '未找到带 x86/x64 C++ 工具链的 Visual Studio。'
}

$msbuild = Join-Path $installationPath 'MSBuild\Current\Bin\MSBuild.exe'
if (-not (Test-Path -LiteralPath $msbuild)) {
    throw "未找到 MSBuild：$msbuild"
}

$toolkitRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$stagingRoot = Join-Path $toolkitRoot "build\staging\$Configuration"
$outputVariant = if ($Loose) { "$Configuration-loose" } else { $Configuration }
$outputRoot = Join-Path $toolkitRoot "dist\$outputVariant"

& $msbuild (Join-Path $toolkitRoot 'San9Toolkit.sln') /m /t:Build "/p:Configuration=$Configuration" /p:Platform=Win32 "/p:OutDir=$stagingRoot\"
if ($LASTEXITCODE -ne 0) {
    throw "构建失败，MSBuild 退出码：$LASTEXITCODE"
}

$launcherPath = Join-Path $stagingRoot 'San9Toolkit.Launcher.exe'
$dllPath = Join-Path $stagingRoot 'San9Toolkit.Runtime.dll'
if (-not (Test-Path -LiteralPath $launcherPath) -or -not (Test-Path -LiteralPath $dllPath)) {
    throw '构建成功，但没有找到待打包的启动器或 DLL。'
}

[IO.Directory]::CreateDirectory($outputRoot) | Out-Null
$singleFilePath = Join-Path $outputRoot 'San9Toolkit.exe'
if ($Loose) {
    Copy-Item -LiteralPath $launcherPath -Destination $singleFilePath -Force
    Copy-Item -LiteralPath $dllPath -Destination (Join-Path $outputRoot 'San9Toolkit.Runtime.dll') -Force
    Write-Host "非单文件构建完成：$outputRoot"
    return
}

$launcherBytes = [IO.File]::ReadAllBytes($launcherPath)
$dllBytes = [IO.File]::ReadAllBytes($dllPath)
$magic = [Text.Encoding]::ASCII.GetBytes('SAN9TOOLKITDLL1')
$payloadSize = [BitConverter]::GetBytes([uint32]$dllBytes.Length)
$payloadHash = [Security.Cryptography.SHA256]::HashData($dllBytes)

$stream = [IO.File]::Open($singleFilePath, [IO.FileMode]::Create, [IO.FileAccess]::Write, [IO.FileShare]::Read)
try {
    $stream.Write($launcherBytes)
    $stream.Write($dllBytes)
    $stream.Write($magic)
    $stream.WriteByte(0)
    $stream.Write($payloadSize)
    $stream.Write($payloadHash)
} finally {
    $stream.Dispose()
}

Write-Host "单文件构建完成：$singleFilePath"
