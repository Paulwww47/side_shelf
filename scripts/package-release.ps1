<#
.SYNOPSIS
    生成 Windows x64 发布包：收集 Qt 运行库、压缩为 zip、输出 SHA256 校验和。

.DESCRIPTION
    发布包不进入版本库，只作为 GitHub Release 资产上传（见
    .github/workflows/release.yml）。在 CI 与本地使用同一份脚本，
    保证人工发布与自动发布产物一致。

.PARAMETER Exe
    已构建的 side_shelf.exe 路径。

.PARAMETER Version
    版本号，可带 v 前缀（v1.0.0 与 1.0.0 等价）。

.PARAMETER OutDir
    输出目录，默认为 <仓库根>\dist。

.PARAMETER QtBin
    Qt 的 bin 目录（含 windeployqt.exe）。默认先查 PATH，再回退到 C:\Qt 下
    版本号最高的 msvc*_64 套件。

.EXAMPLE
    pwsh -File scripts\package-release.ps1 -Exe side_shelf.exe -Version v1.0.0
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$Exe,
    [Parameter(Mandatory)][string]$Version,
    [string]$OutDir,
    [string]$QtBin
)

$ErrorActionPreference = 'Stop'

$RepoRoot = Split-Path -Parent $PSScriptRoot
if (-not $OutDir) { $OutDir = Join-Path $RepoRoot 'dist' }
if (-not (Test-Path $Exe)) { throw "找不到可执行文件：$Exe" }
$Exe = (Resolve-Path $Exe).Path

$ver = $Version.TrimStart('v', 'V')
$pkgName = "side-shelf-$ver-windows-x64"

function Find-WindeployQt {
    param([string]$Hint)
    if ($Hint) {
        $c = Join-Path $Hint 'windeployqt.exe'
        if (Test-Path $c) { return $c }
        throw "在 -QtBin 指定的目录中找不到 windeployqt.exe：$Hint"
    }
    $onPath = Get-Command windeployqt -ErrorAction SilentlyContinue
    if ($onPath) { return $onPath.Source }
    $cand = Get-ChildItem 'C:\Qt' -Directory -ErrorAction SilentlyContinue |
        Sort-Object Name -Descending |
        ForEach-Object { Get-ChildItem $_.FullName -Directory -Filter 'msvc*_64' -ErrorAction SilentlyContinue } |
        ForEach-Object { Join-Path $_.FullName 'bin\windeployqt.exe' } |
        Where-Object { Test-Path $_ } |
        Select-Object -First 1
    if ($cand) { return $cand }
    throw '找不到 windeployqt.exe，请用 -QtBin 指定 Qt 的 bin 目录。'
}

$wdq = Find-WindeployQt -Hint $QtBin
Write-Output "windeployqt : $wdq"
Write-Output "exe         : $Exe"
Write-Output "版本        : $ver"

# windeployqt 只在 VCINSTALLDIR 已设置时才收集 MSVC 运行时（C++ Redistributable）。
# 直接运行本脚本时该变量通常为空，若不补齐，未安装 VC++ Redistributable 的目标
# 机器上程序会因缺少 msvcp140.dll / vcruntime140.dll 而无法启动。
function Copy-VCRuntime {
    param([string]$Stage)

    $roots = @()
    if ($env:VCINSTALLDIR) { $roots += (Join-Path $env:VCINSTALLDIR 'Redist\MSVC') }
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (Test-Path $vswhere) {
        foreach ($inst in (& $vswhere -products * -property installationPath 2>$null)) {
            if ($inst) { $roots += (Join-Path $inst.Trim() 'VC\Redist\MSVC') }
        }
    }

    foreach ($root in $roots) {
        if (-not (Test-Path $root)) { continue }
        $crt = Get-ChildItem $root -Directory -ErrorAction SilentlyContinue |
            Where-Object { $_.Name -match '^\d+\.\d+' } |
            Sort-Object { [version]$_.Name } -Descending |
            ForEach-Object {
                Get-ChildItem (Join-Path $_.FullName 'x64') -Directory -Filter 'Microsoft.VC*.CRT' -ErrorAction SilentlyContinue
            } |
            Where-Object { $_ } |
            Select-Object -First 1
        if ($crt) {
            Copy-Item (Join-Path $crt.FullName '*.dll') $Stage -Force
            Write-Output "MSVC 运行时 : $($crt.FullName)"
            return $true
        }
    }
    return $false
}

if (Test-Path $OutDir) { Remove-Item $OutDir -Recurse -Force }
$stage = Join-Path $OutDir $pkgName
New-Item -ItemType Directory -Force -Path $stage | Out-Null

Copy-Item $Exe (Join-Path $stage 'side_shelf.exe') -Force

# 收集 Qt 运行库。不加 --no-opengl-sw：无可用 GPU 的环境需要软件光栅回退。
& $wdq --release --no-translations --dir $stage (Join-Path $stage 'side_shelf.exe')
if ($LASTEXITCODE -ne 0) { throw "windeployqt 失败（exit $LASTEXITCODE）" }

if (-not (Copy-VCRuntime -Stage $stage)) {
    Write-Warning '未找到 MSVC 运行时 redist 目录，发布包将依赖目标机器已安装 VC++ Redistributable。'
}

Copy-Item (Join-Path $RepoRoot 'LICENSE') (Join-Path $stage 'LICENSE') -Force

$zip = Join-Path $OutDir "$pkgName.zip"
Compress-Archive -Path $stage -DestinationPath $zip -CompressionLevel Optimal

$hash = (Get-FileHash $zip -Algorithm SHA256).Hash.ToLower()
$sumFile = Join-Path $OutDir 'SHA256SUMS.txt'
# 用 WriteAllText 而非 Set-Content：跨 Windows PowerShell 5.1 / pwsh 7 都不带 BOM
[System.IO.File]::WriteAllText($sumFile, "$hash  $pkgName.zip`n")

Write-Output ''
Write-Output ("包大小      : {0:N1} MB" -f ((Get-Item $zip).Length / 1MB))
Write-Output "SHA256      : $hash"
Write-Output "输出        : $zip"
