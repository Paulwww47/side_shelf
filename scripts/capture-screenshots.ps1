<#
.SYNOPSIS
    生成 README 所需的截图（docs/）。

.DESCRIPTION
    全部截图由程序自身的 --render-preview 离屏渲染产出，不抓屏、不注入鼠标
    输入，因此不依赖桌面内容、不会泄露壁纸，结果完全可复现。

    --render-preview <out.png> 除目标文件外还会派生同一基名的整套面板图：

        <基名>.png              把手（药丸形侧边条）
        <基名>.panel.png        预览面板 · 瀑布流
        <基名>.panel.zoom.png   预览面板 · 单项放大
        <基名>.panel.zoomfile.png  预览面板 · 放大文件项
        <基名>.panel.undo.png   预览面板 · 5 秒撤销条

    因此这里只渲染一次（preview-count 3），把派生的面板图整理为 docs/panel*.png；
    空态与 99+ 两次渲染的面板图与常规态完全相同，直接丢弃。

    开发模式使用独立单实例互斥体与 %TEMP% 下的状态目录，可在正式实例正在
    运行时执行。

.PARAMETER Exe
    side_shelf.exe 路径。默认依次尝试 <仓库根>\side_shelf.exe、
    <仓库根>\cpp\build\side_shelf.exe。

.PARAMETER OutDir
    输出目录，默认为 <仓库根>\docs。

.EXAMPLE
    pwsh -File scripts\capture-screenshots.ps1
#>
[CmdletBinding()]
param(
    [string]$Exe,
    [string]$OutDir
)

$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing

$RepoRoot = Split-Path -Parent $PSScriptRoot
if (-not $OutDir) { $OutDir = Join-Path $RepoRoot 'docs' }
if (-not $Exe) {
    foreach ($cand in @((Join-Path $RepoRoot 'side_shelf.exe'),
                        (Join-Path $RepoRoot 'cpp\build\side_shelf.exe'))) {
        if (Test-Path $cand) { $Exe = $cand; break }
    }
}
if (-not $Exe -or -not (Test-Path $Exe)) {
    throw '找不到 side_shelf.exe，请用 -Exe 指定路径。'
}
$Exe = (Resolve-Path $Exe).Path
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null

# 在 %TEMP% 下以离屏模式渲染一张把手图，返回其路径
function Invoke-Render([int]$Count, [string]$BaseName) {
    $tmp = Join-Path $env:TEMP ('ss-render-' + [guid]::NewGuid().ToString('n'))
    New-Item -ItemType Directory -Force $tmp | Out-Null
    $out = Join-Path $tmp ($BaseName + '.png')
    Push-Location $tmp          # 开发模式回退的 cpp_stdout/stderr.txt 落在临时目录
    try   { & $Exe --render-preview $out --preview-count $Count | Out-Null }
    finally { Pop-Location }
    if (-not (Test-Path $out)) { throw "渲染失败：$BaseName（preview-count $Count）" }
    return $out
}

function Show-Info([string]$Path, [string]$Label) {
    $img = [System.Drawing.Image]::FromFile($Path)
    Write-Output ("  {0,-34} {1,5}x{2}" -f $Label, $img.Width, $img.Height)
    $img.Dispose()
}

# ---- 常规态：同时产出把手图与整套面板图 ----
Write-Output '渲染中（离屏）:'
$base = Invoke-Render 3 'shelf'
Copy-Item $base (Join-Path $OutDir 'shelf.png') -Force
Show-Info (Join-Path $OutDir 'shelf.png') 'shelf.png'

$tmpDir = Split-Path $base -Parent
foreach ($m in @(@{ s = 'panel.png';          k = 'panel.png'          },
                 @{ s = 'panel-zoom.png';     k = 'panel.zoom.png'     },
                 @{ s = 'panel-zoom-file.png';k = 'panel.zoomfile.png' },
                 @{ s = 'panel-undo.png';     k = 'panel.undo.png'     })) {
    $src = Join-Path $tmpDir ('shelf.' + $m.k)
    if (-not (Test-Path $src)) { throw "缺少派生图：shelf.$($m.k)" }
    Copy-Item $src (Join-Path $OutDir $m.s) -Force
    Show-Info (Join-Path $OutDir $m.s) $m.s
}
Remove-Item $tmpDir -Recurse -Force

# ---- 空态 / 99+：只保留把手图，面板图与常规态一致，直接丢弃 ----
foreach ($c in @(@{ n = 'shelf-empty.png';  c = 0   },
                 @{ n = 'shelf-99plus.png'; c = 120 })) {
    $tmp = Invoke-Render $c.c ([IO.Path]::GetFileNameWithoutExtension($c.n))
    Copy-Item $tmp (Join-Path $OutDir $c.n) -Force
    Show-Info (Join-Path $OutDir $c.n) $c.n
    Remove-Item (Split-Path $tmp -Parent) -Recurse -Force
}

Write-Output "完成 -> $OutDir"
