<#
===============================================================================
  scripts/make_gif.ps1 —— 把 --capture 导出的 PNG 序列合成 MP4 / GIF

  【典型用法】
    # 1) 先录一段（窗口模式；--demo 让内置驾驶员自己打，不用人操作）
    ./build/bin/TacticalAI-Sandbox.exe --demo --capture build/capture `
        --capture-every 6 --width 1024 --height 576 --frames 820

    # 2) 合成（--capture 结束时会把这行连同建议帧率一起打印出来）
    powershell -File ./scripts/make_gif.ps1 -Frames build/capture -Fps 12

  【为什么分两步（先 MP4 再 GIF）】
    ffmpeg 的 GIF 调色板是两遍的：先统计这段画面的最优 N 色，再映射每一帧。
    直接从 PNG 出发等于跑两遍完整解码，更慢；而且中间留一个 MP4 也有用 ——
    演示视频站/PPT 用 MP4，README 首屏用 GIF。

  【体积经验值】（1024x576 素材、含 AI 可视化覆盖层）
    640 宽 / 10fps / 96 色 ≈ 3.6MB；560 宽 ≈ 3.3MB。
    想更小就降 -Width 或 -GifFps；画面里的候选点/网格点越密越难压。

  【两个 PowerShell 坑（都踩过）】
    1. 本文件以 UTF-8 **带 BOM** 保存：Windows PowerShell 5.1 对无 BOM 的
       脚本按本地代码页（中文环境是 GBK）解析，中文注释会变乱码并导致语法错误。
    2. 不用 $ErrorActionPreference = "Stop"：ffmpeg 把进度/警告写到 stderr，
       在 PowerShell 里那会被当成错误记录，配合 Stop 就会"有警告即中断"。
       原生命令的正确做法是显式检查 $LASTEXITCODE。
===============================================================================
#>
[CmdletBinding()]
param(
    [string] $Frames = "build/capture",     # --capture 的输出目录
    [int]    $Fps    = 12,                  # 素材帧率（--capture 结束时会把建议值打印出来）
    [int]    $GifFps = 10,                  # GIF 帧率（比素材低一档可明显减体积）
    [int]    $Width  = 640,                 # 输出宽度（高度按比例）
    [int]    $Colors = 96,                  # GIF 调色板颜色数
    [string] $Mp4    = "docs/images/demo.mp4",
    [string] $Gif    = "docs/images/demo.gif"
)

if (-not (Get-Command ffmpeg -ErrorAction SilentlyContinue)) {
    Write-Error "找不到 ffmpeg。安装：winget install Gyan.FFmpeg"
    exit 1
}

$pattern = Join-Path $Frames "frame_%04d.png"
$frames  = @(Get-ChildItem -Path $Frames -Filter "frame_*.png" -ErrorAction SilentlyContinue)
if ($frames.Count -eq 0) {
    Write-Error "目录 $Frames 里没有 frame_*.png。先用 --capture 录一段。"
    exit 1
}
# 括号不能省：$frames.Count 直接跟在 -f 后面会被解析成"先对 $frames 格式化"，
# 于是打印出来的是数组本身而不是元素个数。
Write-Host ("[make_gif] 输入 {0} 帧 @ {1}fps  ({2})" -f ($frames.Count), $Fps, $Frames)

# 输出目录（docs/images）可能还不存在
foreach ($out in @($Mp4, $Gif)) {
    $dir = Split-Path -Parent $out
    if ($dir -and -not (Test-Path $dir)) { New-Item -ItemType Directory -Force -Path $dir | Out-Null }
}

# --- 1) PNG 序列 -> MP4（H.264；高度取 -2 保证是偶数，否则 x264 会报错）---
$mp4Args = @("-y", "-loglevel", "warning", "-framerate", "$Fps", "-i", "$pattern",
             "-vf", "scale=${Width}:-2:flags=lanczos",
             "-c:v", "libx264", "-preset", "slow", "-crf", "18", "-pix_fmt", "yuv420p", "$Mp4")
& ffmpeg @mp4Args
if ($LASTEXITCODE -ne 0) { Write-Error "ffmpeg 生成 MP4 失败（退出码 $LASTEXITCODE）"; exit 1 }

# --- 2) 两遍调色板 -> GIF ---
#
# palettegen 只输出一张图，而 ffmpeg 默认把 png 输出当成"序列"，
# 会报 "does not contain an image sequence pattern" ⇒ 加 -frames:v 1 -update 1。
#
# 注意 ${Colors} 的花括号不能省：写成 $Colors:stats_mode 会被 PowerShell
# 解析成"名为 stats_mode 的驱动器变量"，而不是"变量 $Colors 后面接冒号"。
$palette  = Join-Path (Split-Path -Parent $Mp4) "palette.png"
$palArgs  = @("-y", "-loglevel", "warning", "-i", "$Mp4",
              "-vf", "fps=$GifFps,scale=${Width}:-1:flags=lanczos,palettegen=max_colors=${Colors}:stats_mode=diff",
              "-frames:v", "1", "-update", "1", "$palette")
& ffmpeg @palArgs
if ($LASTEXITCODE -ne 0) { Write-Error "ffmpeg 生成调色板失败（退出码 $LASTEXITCODE）"; exit 1 }

$gifArgs  = @("-y", "-loglevel", "warning", "-i", "$Mp4", "-i", "$palette",
              "-lavfi", "fps=$GifFps,scale=${Width}:-1:flags=lanczos[x];[x][1:v]paletteuse=dither=bayer:bayer_scale=5",
              "$Gif")
& ffmpeg @gifArgs
if ($LASTEXITCODE -ne 0) { Write-Error "ffmpeg 生成 GIF 失败（退出码 $LASTEXITCODE）"; exit 1 }
Remove-Item -Force $palette -ErrorAction SilentlyContinue

foreach ($f in @($Mp4, $Gif)) {
    if (Test-Path $f) {
        Write-Host ("[make_gif] {0}  ({1:N2} MB)" -f $f, ((Get-Item $f).Length / 1MB))
    }
}
if ((Test-Path $Gif) -and (Get-Item $Gif).Length -gt 5MB) {
    Write-Warning "GIF 超过 5MB，建议降低 -Width / -GifFps / -Colors。"
}
