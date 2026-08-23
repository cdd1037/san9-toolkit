[CmdletBinding()]
param(
    [ValidateSet('Debug', 'Release')]
    [string]$Configuration = 'Release',
    [string]$CoreUiRoot = ''
)

$ErrorActionPreference = 'Stop'
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if (-not (Test-Path -LiteralPath $vswhere)) { throw '未找到 Visual Studio Installer 的 vswhere.exe。' }
$installationPath = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $installationPath) { throw '未找到带 x86/x64 C++ 工具链的 Visual Studio。' }
$msbuild = Join-Path $installationPath 'MSBuild\Current\Bin\MSBuild.exe'
$vcTargetsRoot = Join-Path $installationPath 'MSBuild\Microsoft\VC'
$toolsetRoots = Get-ChildItem -LiteralPath $vcTargetsRoot -Directory -Filter 'v*' |
    ForEach-Object {
        $candidateRoot = Join-Path $_.FullName 'Platforms\Win32\PlatformToolsets'
        if (Test-Path -LiteralPath $candidateRoot -PathType Container) {
            Get-ChildItem -LiteralPath $candidateRoot -Directory
        }
    }
$platformToolset = $toolsetRoots | Where-Object { $_.Name -match '^v\d+$' } |
    Sort-Object { [int]$_.Name.Substring(1) } -Descending | Select-Object -First 1 -ExpandProperty Name
if (-not $platformToolset) { throw '未找到可用的 MSVC 平台工具集。' }
$toolkitRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$coreUiRootPath = if ([string]::IsNullOrWhiteSpace($CoreUiRoot)) {
    [IO.Path]::GetFullPath((Join-Path $toolkitRoot '..\core-ui-v1.7.0'))
} else {
    [IO.Path]::GetFullPath($CoreUiRoot)
}
$coreUiHeader = Join-Path $coreUiRootPath 'include\ui_core.h'
$coreUiLibrary = Join-Path $coreUiRootPath 'lib\dynamic\core-ui.lib'
$coreUiRuntime = Join-Path $coreUiRootPath 'lib\dynamic\core-ui.dll'
foreach ($dependency in @($coreUiHeader, $coreUiLibrary, $coreUiRuntime)) {
    if (-not (Test-Path -LiteralPath $dependency -PathType Leaf)) {
        throw "Core UI SDK 不完整，缺少：$dependency"
    }
}
$stagingX86 = Join-Path $toolkitRoot "build\staging\$Configuration\x86"
$stagingX64 = Join-Path $toolkitRoot "build\staging\$Configuration\x64"
$outputRoot = Join-Path $toolkitRoot "dist\$Configuration"

& $msbuild (Join-Path $toolkitRoot 'San9Toolkit.Tests.vcxproj') /m /t:Build "/p:Configuration=$Configuration" /p:Platform=Win32 "/p:PlatformToolset=$platformToolset"
if ($LASTEXITCODE -ne 0) { throw '测试构建失败。' }
& (Join-Path $toolkitRoot "build\tests\$Configuration\San9Toolkit.Tests.exe")
if ($LASTEXITCODE -ne 0) { throw '播放器状态与时序测试失败。' }
& $msbuild (Join-Path $toolkitRoot 'San9Toolkit.Runtime.vcxproj') /m /t:Build "/p:Configuration=$Configuration" /p:Platform=Win32 "/p:OutDir=$stagingX86\" "/p:PlatformToolset=$platformToolset"
if ($LASTEXITCODE -ne 0) { throw 'Runtime 构建失败。' }
& $msbuild (Join-Path $toolkitRoot 'San9Toolkit.Launcher.vcxproj') /m /t:Build "/p:Configuration=$Configuration" /p:Platform=Win32 "/p:OutDir=$stagingX86\" "/p:PlatformToolset=$platformToolset"
if ($LASTEXITCODE -ne 0) { throw 'Bootstrap 构建失败。' }
& $msbuild (Join-Path $toolkitRoot 'San9Toolkit.App.vcxproj') /m /t:Build "/p:Configuration=$Configuration" /p:Platform=x64 "/p:OutDir=$stagingX64\" "/p:CoreUiRoot=$coreUiRootPath" "/p:PlatformToolset=$platformToolset"
if ($LASTEXITCODE -ne 0) { throw 'GUI 构建失败。' }

$binX86 = Join-Path $outputRoot 'bin\x86'
$binX64 = Join-Path $outputRoot 'bin\x64'
$uiRoot = Join-Path $outputRoot 'ui'
[IO.Directory]::CreateDirectory($binX86) | Out-Null
[IO.Directory]::CreateDirectory($binX64) | Out-Null
[IO.Directory]::CreateDirectory($uiRoot) | Out-Null
Copy-Item -LiteralPath (Join-Path $stagingX64 'San9Toolkit.exe') -Destination (Join-Path $outputRoot 'San9Toolkit.exe') -Force
Copy-Item -LiteralPath (Join-Path $stagingX86 'San9Toolkit.Bootstrap.exe') -Destination $binX86 -Force
Copy-Item -LiteralPath (Join-Path $stagingX86 'San9Toolkit.Runtime.dll') -Destination $binX86 -Force
Copy-Item -LiteralPath $coreUiRuntime -Destination $binX64 -Force
Copy-Item -LiteralPath (Join-Path $toolkitRoot 'src\ui\app.uix') -Destination $uiRoot -Force
Write-Host "构建完成：$outputRoot（根目录仅保留 San9Toolkit.exe）"
