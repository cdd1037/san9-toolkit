[CmdletBinding()]
param(
    [ValidateSet('Debug', 'Release')]
    [string]$Configuration = 'Release'
)

$ErrorActionPreference = 'Stop'
$toolkitRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
. (Join-Path $toolkitRoot 'scripts\build_environment.ps1')
$buildEnvironment = Get-San9BuildEnvironment
$testProject = Join-Path $toolkitRoot 'San9Toolkit.Tests.vcxproj'

& $buildEnvironment.MsBuild $testProject /m /t:Build "/p:Configuration=$Configuration" `
    /p:Platform=Win32 "/p:PlatformToolset=$($buildEnvironment.PlatformToolset)"
if ($LASTEXITCODE -ne 0) { throw '测试构建失败。' }

& (Join-Path $toolkitRoot "build\tests\$Configuration\San9Toolkit.Tests.exe")
if ($LASTEXITCODE -ne 0) { throw '测试失败。' }
Write-Host '测试通过。'
