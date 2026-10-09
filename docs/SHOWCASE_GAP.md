# 距离"能展示的作品"还差什么

> **本文是一份「当时」的差距清单（快照）**，保留原貌以便回看演进过程：
> 写它时仓库**一个 commit 都没有**、README 里**一张图都没有**。其中的大部分条目
> 后来已经解决（提交与推送、首屏动图、CI 说明、无头宏修复等）。
> 因此下文"现状"一栏里的数字（如 279 个用例）是**当时的**数字，
> 最新数字见 [README](../README.md)。
>
> 结论先给：**技术内容已经够了，差的几乎全是"包装与可验证性"。**
>
> 现状里最稀缺的三样资产你都已经有了 —— ① 清晰的四层架构与真实的设计取舍，
> ② 279 个用例 + 无头可回归的行为测试，③ 三个"从现象追到根因"的实测故事
> （输入丢帧 / 空枪 / 贴墙隐身）。评审真正会被打动的是②③，而不是功能数量。
>
> 所以下面这份清单的重点不是"再加功能"，而是**让别人能看见、能跑起来、能验证**。

---

## 一、体检表（按"展示作品"的八个维度）

| # | 维度 | 现状 | 差距 |
|---|---|---|---|
| 1 | **能被看到** | ❌ 仓库**一个 commit 都没有**（`main` 无提交记录，91 项未跟踪），无远端 | 最致命也最便宜：提交 + 推送 |
| 2 | **第一印象** | ⚠️ README 283 行、结构完整，但**一张图都没有**（`screenshot_*.png` 三个文件已经存在，却从未被引用） | 首屏加截图/GIF |
| 3 | **能跑起来** | ⚠️ 源码构建四步走通（含依赖四级回退），但**没有 `install()`/CPack**，没有可下载的包 | 打包 zip，"下载即运行" |
| 4 | **可验证** | ⚠️ 279 个测试全绿，但**没有 CI**（`.github/workflows` 不存在），只有作者本地跑过 | 加 Actions：构建 + 测试 + 产物 |
| 5 | **看得懂** | ✅ 很强：README + `docs/` 六篇（ROADMAP / REVIEW / PLAYABLE_FIXES / PROJECT_WALKTHROUGH / CODE_WALKTHROUGH / DESIGN_NOTES） | 仅需补"首屏导航" |
| 6 | **看得爽** | ❌ 只有静态 PNG，且未被引用；没有 GIF/视频，没有自动演示模式 | 30 秒动图 + `--demo` |
| 7 | **有深度** | ✅ 很强：行为树语义、感知不变量、EQS 打分、加权 A*、固定步长确定性、无头测试策略、几何一致性 | 只需要"讲出来"（已有两份讲解稿） |
| 8 | **可扩展** | ❌ 掩体/敌人硬编码在 `WorldConfig` + `generateDemoObstacles`；参数不可持久化；无回放 | JSON 关卡 / 回放 / 指标导出 |

**一句话**：1、2、6 是"看起来像不像作品"，3、4 是"能不能被验证"，8 是"能不能被继续用"。
5、7 你已经领先大多数个人项目。

---

## 二、P0：半天内做完，性质就从"代码存档"变成"作品"

### P0-2 README 首屏 ✅ **已完成** · P0-3 动图与录制工具链 ✅ **已完成**

- README 首屏已加：演示 GIF + 三张截图 + 能力标签行 + "从这里开始"导航表。
- 截图已从根目录收进 `docs/images/`（`gameplay.png` / `bullet_zoom.png` / `dodge_roll.png`）。
- 新增录制能力：`--capture <目录>`（连续导出渲染帧）、`--capture-every <n>`、
  `--demo`（内置驾驶员，并把感知/EQS/路径可视化打开）+
  `scripts/make_gif.ps1`（PNG 序列 → MP4 → GIF）+ `docs/RECORDING_GIF.md`。
- README 首屏那段动图就是这条链路产出的：**3.64MB**（640 宽 / 10fps / 96 色）。

### P0-1 首次提交 + 推送（约 20 分钟）

```bash
# 1) 先把散在根目录的截图收进 docs/images/（顺手统一命名）
#    ✅ 已完成（docs/images/gameplay.png / bullet_zoom.png / dodge_roll.png）

# 2) 清空暂存区：build_verify/ 之前被误暂存（.gitignore 里写的是 build-*/，带连字符），
#    现在已补上 build_verify/ 规则。注意本仓库还没有任何提交，
#    所以 `git reset` 会因为 "HEAD 无法解析" 而失败，要用 --cached 显式移除：
git rm -r --cached . -q

# 3) 重新暂存并检查：应当看不到 build/ build_verify/ .cache/ 里的任何文件
git add -A
git status --short

# 4) 提交（建议拆两个：项目本体 / 文档与讲解稿）
git commit -m "feat: tactical AI sandbox (behavior tree / perception / EQS / A*)"
git commit --allow-empty -m "docs: walkthroughs, showcase gap analysis, recording guide"  # 可选
git remote add origin <你的仓库地址>
git push -u origin main
```

**提交前建议再确认一次**（这三条都应该没有输出）：
`git status --short | Select-String "build/|build_verify/|\.cache/"`

**关于 `.workbuddy/`**：它现在也在暂存区里（5 个条目）。
那是工具/会话过程记录，不是项目源码 —— 建议也加进 `.gitignore`，
除非你希望把这些过程笔记一起公开（作为"我是怎么迭代的"的证据，也算一种素材）。

**为什么这是第一位**：没有提交记录，别人无法判断"这是不是你写的""改了多少轮"。
而且 `docs/PLAYABLE_FIXES_*.md`、`docs/REVIEW_*.md` 这些"修复史"只有配上提交历史才有说服力。

**顺手补**：加一个 `LICENSE`（MIT 最省事）。没有许可证，别人不能合法使用/参考你的代码 ——
面试官看到"没有 LICENSE"会当作"随手写的"。

### P0-2 README 首屏（约 1 小时）

现在的首屏是 ASCII 架构图（很好，保留），但在它**上面**要加三样东西：

```markdown
# Tactical AI Sandbox

一句话：**行为树 + 黑板 + 感知 + EQS + A* 的战术 AI 实验场；AI 不开挂，且它的每个决策都能在屏幕上看到。**

![对射与掩体](docs/images/gameplay.png)
![翻滚躲子弹](docs/images/dodge_roll.png)

[![CI](https://github.com/<你>/<仓库>/actions/workflows/ci.yml/badge.svg)](...)
`C++17` · `raylib` · `279 tests` · `60Hz 固定步长` · `无头可回归`

- 想直接跑：`pwsh ./scripts/fetch_dependencies.ps1 && cmake -S . -B build -G Ninja && cmake --build build`
- 想理解设计：`docs/PROJECT_WALKTHROUGH.md`（架构与取舍）
- 想看实现：`docs/CODE_WALKTHROUGH.md`（10 个关键机制逐段解释）
- 想知道踩过什么坑：`docs/PLAYABLE_FIXES_2026-10-09.md`
```

**要点**：首屏要让人在 **5 秒内**知道"这是什么 + 长什么样 + 怎么跑 + 从哪看深"。
README 里现有的东西不要删，往下沉即可。

### P0-3 30 秒 GIF（约 1.5 小时）

静态截图对"行为"类项目几乎无用 —— 你要展示的是**AI 在动**。当前工具链差一小步：

```bash
# 方案 A（零改动）：用现成的 --shot 逐帧导出，再合成
#   需要给 --shot 加一个"连续导出"开关（约 20 行代码）：
#   --capture <目录> --capture-every 2 --frames 1800
#   然后：
ffmpeg -framerate 30 -i build/capture/frame_%04d.png -vf "scale=960:-1" \
       -c:v libx264 -pix_fmt yuv420p docs/images/demo.mp4
#   GIF（README 里用 GIF 更省事，控制在 5MB 内）：
ffmpeg -i docs/images/demo.mp4 -vf "fps=15,scale=720:-1" docs/images/demo.gif
```

录什么（一次录三段，每段 8~10 秒）：
1. **巡逻 → 发现 → 包抄**（按 `1` 显示视野锥，`3` 显示 EQS 候选与评分）；
2. **掩体对射 + 翻滚躲子弹**（HUD 上"闪避"计数跳动）；
3. **F2 冻结 AI 后单步**（`P` + `.`）看行为树状态切换。

### P0-4 修掉"无头宏"这个构建缺陷（约 1 小时）✅ **已完成**

**这是唯一一个"会被追问到尴尬"的技术债**：`TACTICALAI_HEADLESS_TEST` 只加在测试目标上，
而 `tacticalai_engine`/`ai`/`game` 仍按正式配置编译 —— 也就是说引擎里那些
`#if !defined(TACTICALAI_HEADLESS_TEST)` 守卫在测试构建里**是失效的**。
当时没炸只是因为"没有测试构造 `Renderer`"。

**实际采用的方案**：把"要不要窗口"从**构建期宏**改成**运行期参数**
（`Renderer(int, int, string_view, bool createWindow = true)`），
并把引擎里所有窗口调用的守卫统一改成 `IsWindowReady()` 运行时判断：

- 传 `createWindow = false` 时，构造函数**不碰任何 GL/GLFW**（不 InitWindow、
  不 LoadFontEx、不 LoadRenderTexture），`screenWidth()/screenHeight()` 返回构造时声明的尺寸，
  析构也只关"自己创建的窗口"（新增 `ownsWindow_`）。
- 于是"能否无头运行"不再依赖构建配置，而成为代码结构的属性。

**证据**（对照实验）：`tests/test_renderer_headless.cpp` 里传 `createWindow = false` 时
断言 `cjkFontLoaded() == false`、`RenderTarget::isValid() == false`（即完全没做 GL 工作）；
把参数改成 `true` 复现旧行为，实测 `fontLoaded=1 renderTargetValid=1`、耗时 **262ms**
—— 也就是说修复前在单元测试里构造 `Renderer` 会真的开窗、烘字体、建纹理，
在没有显示设备的 CI 上就是 UB。

同时把 `tests/CMakeLists.txt` 里那句"定义此宏让 engine 层跳过窗口相关初始化"
改成了准确的说明（宏只作用于测试自己的翻译单元，**不能**指望它保护引擎）。

---

## 三、P1：2~3 天，让评审能**自己**验证

| # | 事项 | 最小可行做法 | 工作量 |
|---|---|---|---|
| P1-1 | **CI** | `.github/workflows/ci.yml`：`windows-latest` + `ubuntu-latest` 矩阵 → 装 Ninja/Python → `scripts/fetch_dependencies.ps1` → `cmake --build` → `TacticalAI-Tests` → 上传测试日志为 artifact | 半天 |
| P1-2 | **打包** | 顶层 CMake 加 `install(TARGETS TacticalAI-Sandbox RUNTIME DESTINATION bin)` + `include(CPack)` → 出一个 zip，让"下载即运行"成立（已核实：当前 `CMakeLists.txt` / `src/CMakeLists.txt` / `cmake/` / `scripts/` 里**没有任何** `install()` 或 CPack 规则） | 半天 |
| P1-3 | **跨平台验证** | 至少跑通 Ubuntu：CJK 字体候选路径（`CjkFont::candidateFontPaths`）在 Linux 上要能找到 `Noto Sans CJK`；raylib 的 Linux 依赖（X11/GL）写进文档 | 半天 |
| P1-4 | **代码规范** | `.clang-format`（Google/LLVM 风格二选一）+ 全量格式化一次 + `.clang-tidy` 开少量检查（`bugprone-*`、`performance-*`）；README 里注明 | 半天 |
| P1-5 | **`--demo` 自动演示模式** | 一段脚本化输入：巡逻 → 遭遇 → 包抄 → 对射 → 低血撤退 →（`--auto-roll` 已有）。展示时不用手按键盘，也不会翻车 | 1 天 |
| P1-6 | **性能数据点** | `--benchmark --enemies 50 --frames 3600` 输出帧时间/`aiAvg` 曲线（现有 `LoopStats` 已统计 `aiAvg`/`maxStepsInOneFrame`，只差导出 CSV） | 半天 |

**P1-1 的价值被低估**：一个绿徽章等于"我构建过、我测过"，而且 CI 日志本身就是"可复现"的证据。
对你这种"测试写得比代码还细"的项目，没有 CI 是纯浪费。

---

## 四、P2：1~2 周，从"能展示"到"能打动人"

| # | 事项 | 为什么它值这个时间 |
|---|---|---|
| P2-1 | **JSON 关卡** | `nlohmann_json` 已在依赖里却只用于配置：抽出 `WorldConfig` + 掩体列表 → `levels/*.json`，就能做**同一套 AI、换场景对比**（走廊 / 开阔地 / 掩体迷宫）。这是"可扩展性"最直观的证明 |
| P2-2 | **决策日志/时间线** | 现在"AI 在想什么"只有图形。加一个滚动日志（`失去视线 → 前往最后已知位置 → 观察 0.8s → 放弃 → 巡逻`）+ 可导出，讲解时能拿它当"AI 的心电图" |
| P2-3 | **量化指标 + 图表** | 输出 CSV：首次发现时间、命中率、掩体使用率、平均交战距离、决策耗时分布。把三个 bug 的**修复前后数据**画成图贴进 README —— 一张对比图胜过十段文字 |
| P2-4 | **确定性回放** | 固定步长 + 确定性逻辑 ⇒ 只要记录"每步输入"，就能逐帧复现整局。录制/回放既是功能，也是"我的 AI 是可复现的"最强证据（顺带给 P2-3 提供数据源） |
| P2-5 | **英文 README** | 面向国际团队/外企必备；也顺带逼自己把"一句话定位"打磨到能被翻译 |

---

## 五、**别做**清单（边际收益极低）

| 别做 | 理由 |
|---|---|
| 音频/音效 | 对"AI 架构"毫无证明力，纯工期 |
| 更多敌人类型、武器系统、技能树 | 在做"游戏"的加法，而你的价值主张是"AI 与可验证性" |
| 联机/多人 | 与主题无关，且会拖垮确定性 |
| 追求 1000 敌人 60FPS | 当前 `aiAvg` 0.003~0.004ms 已经足够说明"有余量"；把引擎换成多线程反而是新的风险面 |
| 把渲染升级成 Vulkan/抽象图形后端 | 面试官关心的是决策链路，不是渲染后端 |
| 现在重写黑板（换成类型安全句柄） | 这是"更好地讲"的点，不是"必须做"的事 —— 放在问答里当"如果重做我会怎么改"更有加分 |

---

## 六、只有一天 / 三天 / 一周，怎么选

**只有一天（发布日）**：P0-1 提交推送 → P0-2 README 首屏（现成截图）→ P0-3 录一段 30 秒 GIF → 加 LICENSE。
（这四步做完，项目对外就已经是"作品"而不是"存档"。）

**三天**：上面 + P1-1 CI + P1-2 打包 + P0-4 无头宏修复。
（评审可以自己 clone → 跑 CI 看到 279 绿 → 下载 zip 直接玩。）

**一周**：上面 + P1-5 `--demo` + P2-1 JSON 关卡 + P2-3 指标图表。
（讲的时候能说："换三个场景，同一套 AI，这是量化对比。"）

---

## 七、如果只让我给一句话的优先级

> **先让它"被看见"（提交 + 首屏 + 动图），再让它"被验证"（CI + 打包），
> 最后才考虑加功能。**
>
> 你目前最大的问题不是"缺什么功能"，而是 279 个测试、3 个真实 bug 故事、
> 6 篇设计文档全部只存在于你自己这台机器上。
