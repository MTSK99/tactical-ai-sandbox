# ---------------------------------------------------------------------------
#  一键获取依赖。
#
#  用法（在项目根目录）:
#     pwsh ./scripts/fetch_dependencies.ps1
#     pwsh ./scripts/fetch_dependencies.ps1 -Depth 1        # 只拉一级（更快）
#     pwsh ./scripts/fetch_dependencies.ps1 -Only raylib    # 只拉指定依赖
#
#  为什么不用 git submodule:
#    大多数使用者只是想快速跑起来。submodule 需要额外执行 init/update，
#    且一旦忘记就会得到「目录存在但为空」的困惑状态。
#    直接 clone 到 external/ 更符合「拿来即用」的预期；
#    CMake 会自动优先使用 external/ 下的源码，无需额外配置。
# ---------------------------------------------------------------------------

[CmdletBinding()]
param(
    [switch]$Force,
    [switch]$Depth1,
    [string[]]$Only = @()
)

$ErrorActionPreference = 'Stop'

$projectRoot = Split-Path -Parent $PSScriptRoot
$externalDir = Join-Path $projectRoot 'external'

Write-Host ""
Write-Host "===============================================" -ForegroundColor Cyan
Write-Host "  TacticalAI-Sandbox 依赖获取" -ForegroundColor Cyan
Write-Host "===============================================" -ForegroundColor Cyan
Write-Host "  目标目录: $externalDir"
Write-Host ""

if (-not (Test-Path $externalDir)) {
    New-Item -ItemType Directory -Path $externalDir -Force | Out-Null
    Write-Host "  已创建 external/ 目录" -ForegroundColor DarkGray
}

# 依赖清单：名称 / 仓库 / 标签 / 必要文件（用于校验是否已就绪）
$dependencies = @(
    @{
        Name    = 'raylib'
        Repo    = 'https://github.com/raysan5/raylib.git'
        Tag     = '5.5'
        Check   = 'CMakeLists.txt'
        Note    = '窗口 / 输入 / 2D 渲染后端'
    },
    @{
        Name    = 'imgui'
        Repo    = 'https://github.com/ocornut/imgui.git'
        Tag     = 'v1.90.9-docking'
        Check   = 'imgui.cpp'
        Note    = '运行时调参面板'
    },
    @{
        Name    = 'glm'
        Repo    = 'https://github.com/g-truc/glm.git'
        Tag     = '1.0.1'
        Check   = 'CMakeLists.txt'
        Note    = '向量 / 矩阵数学'
    },
    @{
        Name    = 'nlohmann-json'
        Repo    = 'https://github.com/nlohmann/json.git'
        Tag     = 'v3.11.3'
        Check   = 'CMakeLists.txt'
        Note    = '配置与场景序列化'
    },
    @{
        Name    = 'googletest'
        Repo    = 'https://github.com/google/googletest.git'
        Tag     = 'v1.14.0'
        Check   = 'CMakeLists.txt'
        Note    = '单元测试框架'
    }
)

# 检查 git 是否可用
try {
    $gitVersion = (git --version) 2>&1
    Write-Host "  使用 $gitVersion" -ForegroundColor DarkGray
} catch {
    Write-Host "  [错误] 未找到 git。请先安装 Git 并确保它在 PATH 中。" -ForegroundColor Red
    Write-Host "         下载地址: https://git-scm.com/downloads" -ForegroundColor Red
    exit 1
}

$cloneDepth = if ($Depth1) { @('--depth', '1') } else { @() }

$successCount = 0
$skipCount = 0
$failCount = 0

foreach ($dep in $dependencies) {
    $name = $dep.Name

    if ($Only.Count -gt 0 -and $Only -notcontains $name) {
        continue
    }

    $targetDir = Join-Path $externalDir $name
    $checkFile = Join-Path $targetDir $dep.Check

    Write-Host "[$name] $($dep.Note)" -ForegroundColor White
    Write-Host "  仓库: $($dep.Repo) @ $($dep.Tag)" -ForegroundColor DarkGray

    # 已存在且完整 -> 跳过（除非 -Force）
    if ((Test-Path $checkFile) -and -not $Force) {
        Write-Host "  状态: 已存在，跳过（用 -Force 强制重新拉取）" -ForegroundColor Yellow
        $skipCount++
        Write-Host ""
        continue
    }

    # 目录存在但内容不完整 -> 清理后重来，避免 git 报「目录非空」
    if (Test-Path $targetDir) {
        Write-Host "  状态: 目录不完整，清理后重新拉取" -ForegroundColor Yellow
        Remove-Item -Recurse -Force $targetDir -ErrorAction SilentlyContinue
    }

    Write-Host "  状态: 正在克隆..." -ForegroundColor Cyan

    $cloneArgs = @('clone', '--branch', $dep.Tag) + $cloneDepth + @($dep.Repo, $targetDir)

    try {
        # 把 git 的进度输出重定向掉，只保留错误
        $output = & git @cloneArgs 2>&1
        if ($LASTEXITCODE -ne 0) {
            throw "git clone 返回 $LASTEXITCODE`n$output"
        }

        if (Test-Path $checkFile) {
            Write-Host "  状态: 完成" -ForegroundColor Green
            $successCount++
        } else {
            Write-Host "  状态: 克隆成功但缺少 $($dep.Check)" -ForegroundColor Yellow
            Write-Host "        请手动检查该目录内容" -ForegroundColor Yellow
            $failCount++
        }
    } catch {
        Write-Host "  状态: 失败" -ForegroundColor Red
        Write-Host "  原因: $_" -ForegroundColor DarkRed
        Write-Host "  提示: 该依赖可以留空，CMake 会尝试通过 FetchContent 或系统包解析" -ForegroundColor DarkGray
        $failCount++

        # 清理可能留下的半成品目录
        if (Test-Path $targetDir) {
            Remove-Item -Recurse -Force $targetDir -ErrorAction SilentlyContinue
        }
    }

    Write-Host ""
}

# ---------------------------------------------------------------------------
#  校验 imgui 的 raylib 后端
# ---------------------------------------------------------------------------
$imguiDir = Join-Path $externalDir 'imgui'
if (Test-Path $imguiDir) {
    $backendFile = Join-Path $imguiDir 'backends/imgui_impl_raylib.cpp'
    if (Test-Path $backendFile) {
        Write-Host "[imgui 后端] 找到 imgui_impl_raylib，完整调参面板可用" -ForegroundColor Green
    } else {
        Write-Host "[imgui 后端] 未找到 imgui_impl_raylib.cpp" -ForegroundColor Yellow
        Write-Host "             调试 UI 会自动降级为 raylib 原生只读 HUD（仍可正常运行）" -ForegroundColor DarkGray
    }
} else {
    Write-Host "[imgui] 未安装，调试 UI 将使用原生只读 HUD" -ForegroundColor Yellow
}

# ---------------------------------------------------------------------------
#  汇总
# ---------------------------------------------------------------------------
Write-Host ""
Write-Host "===============================================" -ForegroundColor Cyan
Write-Host "  汇总: 成功 $successCount | 跳过 $skipCount | 失败 $failCount" -ForegroundColor Cyan
Write-Host "===============================================" -ForegroundColor Cyan
Write-Host ""

if ($failCount -gt 0) {
    Write-Host "  部分依赖获取失败。你仍然可以尝试配置构建——" -ForegroundColor Yellow
    Write-Host "  CMake 会依次尝试 external/ -> 系统包 -> FetchContent。" -ForegroundColor Yellow
    Write-Host ""
}

Write-Host "  下一步:" -ForegroundColor White
Write-Host "    cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release" -ForegroundColor Gray
Write-Host "    cmake --build build" -ForegroundColor Gray
Write-Host ""
Write-Host "  若网络受限，可关闭自动拉取:" -ForegroundColor DarkGray
Write-Host "    cmake -S . -B build -DTACTICALAI_USE_FETCHCONTENT=OFF" -ForegroundColor DarkGray
Write-Host ""

exit 0
