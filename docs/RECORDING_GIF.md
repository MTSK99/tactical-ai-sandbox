# 录制 GIF / 视频（演示素材怎么来）

> README 首屏那段动图是**一条命令**录出来的，不是手打的。
> 本文说明整条链路：怎么录、录什么、多大合适、踩过哪些坑。

---

## 一、三步走

```powershell
# 1) 录：--demo 让内置驾驶员自己打（走位 / 瞄准 / 开火 / 翻滚），
#    并自动打开感知 · EQS 候选 · 路径三样可视化 —— 否则画面里只是"两个人在对射"，
#    看不出这是一个 AI 沙盒。
./build/bin/TacticalAI-Sandbox.exe --demo --capture build/capture `
    --capture-every 6 --width 1024 --height 576 --frames 820

# 2) 合成（窗口关闭时会把这行连同"建议素材帧率"一起打印出来，复制即可）
powershell -File ./scripts/make_gif.ps1 -Frames build/capture -Fps 12

# 3) 产物
#    docs/images/demo.mp4   （0.6MB 左右，给 PPT / 视频站）
#    docs/images/demo.gif   （3~4MB，给 README）
```

> 装了 PowerShell 7 的话把 `powershell` 换成 `pwsh` 也可以。
> `scripts/` 下的 `.ps1` 都带 UTF-8 BOM，两种 PowerShell 都能读。

---

## 二、几个参数怎么选

| 参数 | 作用 | 建议 |
|---|---|---|
| `--capture-every N` | 每 N 个渲染帧存一张 | 4~8。渲染 120FPS 时，4 ≈ 30fps 素材，8 ≈ 15fps |
| `--width/--height` | 录制分辨率 | 1024×576 足够；GIF 会再缩到 640 宽 |
| `--frames N` | 录多少渲染帧后自动退出 | 800~2400。`--capture` 不写 `--frames` 时默认 1800 |
| `--capture` 结束 | 打印导出张数、**建议素材帧率**、合成命令 | 素材帧率 = 导出张数 ÷ 游戏时长，与帧率高低无关 |
| `-GifFps` | GIF 帧率 | 10 比素材低一档可以明显减体积 |
| `-Width` / `-Colors` | GIF 宽度 / 调色板颜色数 | 640 / 96 是实测的平衡点（≈3.6MB） |

**为什么建议保持 vsync 开着录**：`--frames` 数的是**渲染帧**，关掉 vsync 后帧率可能飙到几百，
"录 1800 帧"可能只有 4 秒。开着 vsync（默认）时帧率稳定在显示器刷新率，时长可预期。
（即使真关掉了，合成的帧率建议值仍然是准的 —— 它按游戏时间算，不按渲染帧率算。）

---

## 三、录什么（三段式，每段 5~10 秒）

1. **发现与包抄**：`--demo`，按 `1`（视野锥/视线）、`3`（EQS 候选与评分）、`4`（路径）。
   看敌人从巡逻 → 发现 → 绕向玩家侧翼。
2. **掩体对射 + 翻滚躲弹**：保持默认可视化 + `7`（子弹与弹道拖尾）。
   重点是 HUD 上的**"闪避"计数**会跳 —— 那是"翻滚无敌帧真的生效"的唯一可见证据。
3. **单步观察**：按 `P` 暂停，再按 `.` 单步，看行为树状态与黑板数据逐帧变化。
   （这一段建议不要 `--demo`，因为它需要"停下来"的静态画面。）

> 录制时**别把窗口最小化或被完全遮挡**：抓帧读的是帧缓冲，被遮挡的内容不保证有效。

---

## 四、体积经验值

以 1024×576 素材、含 AI 可视化覆盖层为基准：

| 设置 | GIF 体积 |
|---|---|
| 720 宽 / 12fps / 默认调色板 | 5.2 MB（偏大） |
| 640 宽 / 10fps / 96 色 / 粗抖动 | **3.6 MB** ✅ |
| 560 宽 / 10fps / 96 色 / 粗抖动 | 3.3 MB |

**体积主要被高频细节顶上去**：候选点、网格点、弹道拖尾越密越难压。
如果还想更小，优先降 `-Width`，其次降 `-GifFps`，最后才动 `-Colors`。

---

## 五、踩过的坑（都是真的）

| 坑 | 现象 | 处理 |
|---|---|---|
| 用 raylib 的 `TakeScreenshot` | 显示缩放 150% 的 Windows 上产出带黑边的图；且会丢掉参数里的目录 | 自己按逻辑分辨率读像素：`captureScreenshot()`（见 `src/main.cpp`） |
| 2D 绘制是批处理的 | 抓帧时 HUD 还没提交，截图里只有世界层 | 读像素前先 `rlDrawRenderBatchActive()` 强制 flush |
| 抓帧时机放在 `endFrame()` 之后 | 抓到空帧/上一帧（SwapBuffers 已发生） | 放在 `endFrame()` **之前**，与 `--shot` 同一时机 |
| `.ps1` 没有 BOM | Windows PowerShell 5.1 按 GBK 解析，中文注释乱码 → 语法错误 | 脚本保存为 **UTF-8 带 BOM** |
| `$ErrorActionPreference = "Stop"` | ffmpeg 把警告写到 stderr，被当成错误 → 脚本"有警告就中断" | 原生命令改为显式检查 `$LASTEXITCODE` |
| `palettegen` 输出单张 png | 报 "does not contain an image sequence pattern" | 加 `-frames:v 1 -update 1` |
| PowerShell 里 `$Colors:stats_mode` | 被解析成"驱动器变量"，滤镜串坏掉 | 写成 `${Colors}:stats_mode` |

---

## 六、怎么嵌进 README

```markdown
![自动演示：内置驾驶员 + AI 可视化](docs/images/demo.gif)
```

GIF ≤ 5MB 比较友好（大 GIF 在 GitHub 上加载慢，也可能被压缩处理）。
视频站/PPT 用 `docs/images/demo.mp4`。

---

## 七、想更专业一点（可选）

- **固定演示脚本**：`--demo` 的驾驶员是"通用策略"，不是编排好的剧本。
  要做逐秒可控的镜头（"第 3 秒一定要发生一次翻滚"），需要一层输入脚本回放
  （见 `docs/SHOWCASE_GAP.md` 的 P2-4：确定性回放）。当前架构**支持**这件事 ——
  固定步长 + 确定性逻辑，只要记录每步输入即可。
- **多段拼接**：录三段分别合成，再用 `ffmpeg -f concat` 拼成一个 MP4，
  比一个长 GIF 更适合放到项目主页。
