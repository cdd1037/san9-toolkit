function Get-San9BuildEnvironment {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (-not (Test-Path -LiteralPath $vswhere)) {
        throw '未找到 Visual Studio Installer 的 vswhere.exe。'
    }

    $installationPath = & $vswhere -latest -products * `
        -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if (-not $installationPath) {
        throw '未找到带 x86/x64 C++ 工具链的 Visual Studio。'
    }

    $vcTargetsRoot = Join-Path $installationPath 'MSBuild\Microsoft\VC'
    $toolsetRoots = Get-ChildItem -LiteralPath $vcTargetsRoot -Directory -Filter 'v*' |
        ForEach-Object {
            $candidateRoot = Join-Path $_.FullName 'Platforms\Win32\PlatformToolsets'
            if (Test-Path -LiteralPath $candidateRoot -PathType Container) {
                Get-ChildItem -LiteralPath $candidateRoot -Directory
            }
        }
    $platformToolset = $toolsetRoots | Where-Object { $_.Name -match '^v\d+$' } |
        Sort-Object { [int]$_.Name.Substring(1) } -Descending |
        Select-Object -First 1 -ExpandProperty Name
    if (-not $platformToolset) {
        throw '未找到可用的 MSVC 平台工具集。'
    }

    [PSCustomObject]@{
        MsBuild = Join-Path $installationPath 'MSBuild\Current\Bin\MSBuild.exe'
        PlatformToolset = $platformToolset
    }
}
