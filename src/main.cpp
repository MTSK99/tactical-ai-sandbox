// ===========================================================================
//  src/main.cpp
//
//  程序入口。
//
//  职责（严格限定，避免 main 膨胀成上帝函数）：
//    1. 解析命令行参数
//    2. 把各层组装起来：World / Renderer / Input / DebugUI -> GameLoop
//    3. 实现 IGameLayer 接口，把引擎回调分发给游戏层与调试层
//
//  所有实际逻辑都在 SandboxLayer 里，main() 只做组装与启动。
//
//  命令行参数：
//    --width <int>        窗口宽度（默认 1280）
//    --height <int>       窗口高度（默认 720）
//    --enemies <int>      初始敌人数量（默认 3）
//    --fixed-delta <ms>   逻辑步长毫秒（默认 16.667，即 60Hz）
//    --frames <int>       运行指定帧数后退出（用于无头验证/CI）
//    --headless           不创建窗口，只跑逻辑（CI 冒烟测试用）
//    --no-eqs             禁用敌人的 EQS 选位（对比行为差异）
//    --vsync / --no-vsync
//    --help
// ===========================================================================
#include <algorithm>
#include <clocale>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>

#include "debug/AIDebugDraw.h"
#include "debug/DebugUI.h"
#include "engine/GameLoop.h"
#include "engine/Input.h"
#include "engine/Renderer.h"
#include "engine/Time.h"
#include "game/AutoPilot.h"
#include "game/World.h"
#include "rlgl.h"   // rlReadScreenPixels：自己抓帧，见下方 captureScreenshot

#if defined(_WIN32)
// 【为什么不用 #include <windows.h>】
// windows.h 会引入 Rectangle / CloseWindow / ShowCursor / PlaySound 等
// 与 raylib 同名的符号，且无法通过宏完全屏蔽，导致编译期大量重定义错误。
//
// 这里只需要两个 Win32 函数，因此直接声明它们的原型（extern "C"），
// 由 kernel32.dll 在链接期提供。这是最干净的隔离方式：
// 既不把 Windows 的宏污染带进整个翻译单元，也不改变任何构建配置。
extern "C" {
__declspec(dllimport) int __stdcall SetConsoleOutputCP(unsigned int codePage);
__declspec(dllimport) int __stdcall SetConsoleCP(unsigned int codePage);
}
#  ifndef CP_UTF8
#    define CP_UTF8 65001u   // UTF-8 代码页常量，来自 winnls.h
#  endif
#endif

namespace {

// ===========================================================================
//  控制台编码
// ===========================================================================
/// 把 Windows 控制台切成 UTF-8 模式。
///
/// 【为什么需要】
/// 源码里的中文字面量经 MSVC/GCC 编译后，在可执行文件里是 UTF-8 字节。
/// 但 Windows 控制台默认用本地代码页（中文系统为 GBK / CP936）解释这些字节，
/// 于是「行为树」被当成 GBK 去解 UTF-8，屏幕上就是一堆乱码。
///
/// 从 Windows 10 1903 起，控制台支持 CP65001（UTF-8），
/// 所以只要显式声明一次，此后所有 std::printf 的中文都能正确显示。
///
/// 非 Windows 平台（Linux / macOS 终端）默认就是 UTF-8，无需处理。
void setupConsoleEncoding() {
#if defined(_WIN32)
    // 输出代码页 -> UTF-8
    SetConsoleOutputCP(CP_UTF8);
    // 输入代码页也一并设置，避免 scanf / getchar 读中文时出现乱码
    SetConsoleCP(CP_UTF8);

    // 有些终端（尤其旧版 cmd）即使设了 CP_UTF8，字体仍不含中文字形，
    // 表现为方块。这种情况无法从程序侧修复，只能提示用户换字体。
    // 这里不主动提示，避免打扰——终端字体问题属于环境而非程序缺陷。
#endif

    // C 运行时的区域设置：让宽字符与多字节转换按 UTF-8 处理。
    // 注意：不能用 setlocale(LC_ALL, "")，那会跟随系统区域（中文系统 = GBK），
    // 反而把我们想要的 UTF-8 覆盖掉。
    std::setlocale(LC_ALL, ".UTF-8");
}

// ===========================================================================
//  命令行参数
// ===========================================================================
struct CommandLineOptions {
    int    width        = 1280;
    int    height       = 720;
    int    enemies      = 3;
    double fixedDeltaMs = 1000.0 / 60.0;
    int    maxFrames    = 0;      ///< 0 = 不限
    bool   headless     = false;
    bool   useEQS       = true;
    bool   dumpWorld    = false;   ///< --dump：无头模式下打印完整世界状态
    bool   vsync        = true;
    bool   showHelp     = false;

    /// --shot <文件>：运行到指定帧时把窗口内容存成 PNG 后退出。
    ///
    /// 【为什么需要这个开关】
    /// "渲染是否正确"是唯一无法靠单元测试回答的问题——逻辑测试能证明
    /// 子弹存在、会飞、会命中，但证明不了它**画出来了**、也没有画错位置。
    /// 上一轮的教训很直接：中文显示问题修了一次没修干净，用户回来说
    /// "窗口左上角还是乱码"。那次如果有截图，一轮就能看出来。
    std::string shotPath;

    /// --shot-frame <帧号>：在第几帧截图（默认取最后一帧）。
    /// 截图时机很重要：想拍到"子弹在空中"，就得在交火正酣时截，
    /// 而不是等敌人全死了之后。
    int shotFrame = -1;

    /// --auto-roll <秒>：每隔这么多秒自动翻滚一次（0 = 关闭）。
    ///
    /// 【为什么需要一个"自动按键"的开关】
    /// 翻滚的观感全在视觉上：残影、无敌护盾环、进度条。这些绘制代码
    /// 单元测试一行都覆盖不到（测试断言不了像素）。而无头/脚本化运行里
    /// 没人去按空格，翻滚就永远不会发生，那几段绘制代码等于从未被执行过。
    /// 有了这个开关，截图就能拍到翻滚的中间帧，把"画出来了吗、画对了吗"
    /// 变成一眼可查的事。
    ///
    /// 顺带也是个好用的观察模式：让玩家持续翻滚，看 AI 的预判射击
    /// 会不会被反复闪掉（HUD 上的"闪避"计数会跳）。
    double autoRollInterval = 0.0;

    /// --capture <目录>：把渲染帧**连续**导出为 PNG 序列（录制 GIF/视频用）。
    ///
    /// 【为什么需要它，--shot 不够吗】
    /// --shot 只能拍一张静态图，而"AI 在动"这件事恰恰是静态图证明不了的：
    /// 巡逻 → 发现 → 包抄 → 对射 → 撤退是**时序**行为。有了帧序列，
    /// 一条 ffmpeg 命令就能把它变成 GIF/MP4，README 首屏也就有了能动的东西。
    std::string captureDir;

    /// --capture-every <n>：每 n 个渲染帧导出一张（默认 4）。
    /// 120FPS 渲染 + 每 4 帧取一张 ≈ 30fps 素材，正好是 GIF 的常用帧率；
    /// 取值越大文件越小、动作越跳。
    int captureEvery = 4;

    /// --demo：自动演示模式（内置驾驶员操作玩家角色）。
    ///
    /// 录制与现场展示时不必有人坐在键盘前：它按**确定性**策略走位、瞄准、
    /// 开火，发现来袭子弹就翻滚。因为不依赖随机数，同一条命令能复现同一段
    /// 演示（这也是"固定步长 + 确定性逻辑"这个设计的一次实际兑现）。
    bool demo = false;
};

void printUsage(std::string_view programName) {
    std::printf(
        "Tactical AI Sandbox v0.1.0\n"
        "行为树 / 黑板 / 感知 / EQS / 寻路 的可交互实验沙盒\n"
        "\n"
        "用法: %s [选项]\n"
        "\n"
        "选项:\n"
        "  --width <int>          窗口宽度 (默认 1280)\n"
        "  --height <int>         窗口高度 (默认 720)\n"
        "  --enemies <int>        初始敌人数量 (默认 3)\n"
        "  --fixed-delta <ms>     逻辑步长毫秒 (默认 16.667 = 60Hz)\n"
        "  --frames <int>         运行指定帧数后退出 (无头验证用)\n"
        "  --headless             不创建窗口，只跑逻辑\n"
        "  --no-eqs               禁用 EQS 选位 (对比行为差异)\n"
        "  --no-vsync             关闭垂直同步\n"
        "  --shot <文件>          在第 N 帧把画面存成 PNG 后退出\n"
        "  --shot-frame <int>     指定截图帧号 (给了 --frames 时默认取其最后一帧,\n"
        "                         否则默认第 300 帧 ≈ 5 秒, 此时敌人已开始交火)\n"
        "  --auto-roll <秒>       每隔 N 秒自动翻滚一次 (观察/截图用)\n"
        "  --capture <目录>       连续导出渲染帧为 PNG 序列 (录制 GIF/视频用)\n"
        "  --capture-every <int>  每 N 帧导出一张 (默认 4; 120FPS 下约等于 30fps 素材)\n"
        "  --demo                 自动演示模式 (内置驾驶员操作玩家, 录制/展示用)\n"
        "  --dump                 退出时打印世界状态快照 (无头模式排障用)\n"
        "  --help                 显示本帮助\n"
        "\n"
        "运行时按键:\n"
        "  WASD / 方向键  移动\n"
        "  Shift          冲刺\n"
        "  空格           翻滚 (Dodge Roll)\n"
        "  Ctrl / 鼠标右键 瞄准\n"
        "  F              开火\n"
        "  鼠标           瞄准方向\n"
        "  P              暂停/恢复\n"
        "  .              暂停时单步\n"
        "  Tab            慢动作切换\n"
        "  N              生成敌人\n"
        "  R              全部重新寻路\n"
        "  1-7            切换调试可视化\n"
        "  F1             显示/隐藏调参面板\n"
        "  F11            全屏\n"
        "  Escape / 关窗  退出\n"
        "\n",
        std::string(programName).c_str());
}

/// 解析命令行。未知参数会打印警告但继续运行——
/// 沙盒工具不该因为一个拼错的参数就直接退出。
CommandLineOptions parseCommandLine(int argc, char** argv) {
    CommandLineOptions options;

    auto requireValue = [argc, argv](int& index, const char* flag) -> const char* {
        if (index + 1 >= argc) {
            std::fprintf(stderr, "[警告] %s 缺少参数值，忽略\n", flag);
            return nullptr;
        }
        return argv[++index];
    };

    for (int i = 1; i < argc; ++i) {
        const std::string_view argument(argv[i]);

        if (argument == "--help" || argument == "-h") {
            options.showHelp = true;
        } else if (argument == "--width") {
            if (const char* value = requireValue(i, "--width")) {
                options.width = std::atoi(value);
            }
        } else if (argument == "--height") {
            if (const char* value = requireValue(i, "--height")) {
                options.height = std::atoi(value);
            }
        } else if (argument == "--enemies") {
            if (const char* value = requireValue(i, "--enemies")) {
                options.enemies = std::atoi(value);
            }
        } else if (argument == "--fixed-delta") {
            if (const char* value = requireValue(i, "--fixed-delta")) {
                options.fixedDeltaMs = std::atof(value);
            }
        } else if (argument == "--frames") {
            if (const char* value = requireValue(i, "--frames")) {
                options.maxFrames = std::atoi(value);
            }
        } else if (argument == "--shot") {
            if (const char* value = requireValue(i, "--shot")) {
                options.shotPath = value;
            }
        } else if (argument == "--shot-frame") {
            if (const char* value = requireValue(i, "--shot-frame")) {
                options.shotFrame = std::atoi(value);
            }
        } else if (argument == "--auto-roll") {
            if (const char* value = requireValue(i, "--auto-roll")) {
                options.autoRollInterval = std::atof(value);
            }
        } else if (argument == "--capture") {
            if (const char* value = requireValue(i, "--capture")) {
                options.captureDir = value;
            }
        } else if (argument == "--capture-every") {
            if (const char* value = requireValue(i, "--capture-every")) {
                options.captureEvery = std::atoi(value);
            }
        } else if (argument == "--demo") {
            options.demo = true;
        } else if (argument == "--headless") {
            options.headless = true;
        } else if (argument == "--dump") {
            options.dumpWorld = true;
        } else if (argument == "--no-eqs") {
            options.useEQS = false;
        } else if (argument == "--no-vsync") {
            options.vsync = false;
        } else if (argument == "--vsync") {
            options.vsync = true;
        } else {
            std::fprintf(stderr, "[警告] 未知参数: %.*s（使用 --help 查看可用参数）\n",
                         static_cast<int>(argument.size()), argument.data());
        }
    }

    // 参数合法化：防止 0 或负数导致的除零/死循环
    options.width  = std::clamp(options.width, 320, 7680);
    options.height = std::clamp(options.height, 240, 4320);
    options.enemies = std::clamp(options.enemies, 0, 64);
    options.fixedDeltaMs = std::clamp(options.fixedDeltaMs, 1.0, 100.0);
    options.maxFrames = std::max(options.maxFrames, 0);
    options.enemies = std::max(options.enemies, 0);
    options.captureEvery = std::clamp(options.captureEvery, 1, 240);

    // 抓帧要读帧缓冲，必须有窗口 —— 与 --headless 互斥。
    if (!options.captureDir.empty() && options.headless) {
        std::fprintf(stderr,
                     "[警告] --capture 需要窗口（要读帧缓冲），与 --headless 冲突，"
                     "已忽略 --capture\n");
        options.captureDir.clear();
    }
    // 录帧没给时长就会一直跑到手动关窗（脚本化录制会挂住）：给一个默认时长。
    if (!options.captureDir.empty() && options.maxFrames <= 0) {
        options.maxFrames = 1800;
        std::printf("[main] --capture 未指定 --frames，默认录 %d 帧"
                    "（120FPS 下约 %d 秒）\n",
                    options.maxFrames, options.maxFrames / 120);
    }

    return options;
}

// ---------------------------------------------------------------------------
//  抓帧
//
//  【为什么不用 raylib 的 TakeScreenshot】
//  raylib 5.5 的实现是：
//      Vector2 scale = GetWindowScaleDPI();
//      rlReadScreenPixels(render.width * scale.x, render.height * scale.y);
//  它把「逻辑分辨率」乘上 DPI 缩放再去读像素。但窗口并不是按 DPI 缩放的
//  —— 本工程没有开 FLAG_WINDOW_HIGHDPI，帧缓冲实际就是 1280x720。
//  在显示缩放 150% 的 Windows 上这会让它去读 1920x1080，超出帧缓冲的部分
//  由 glReadPixels 留空，于是产出一张 1920x1080 的图：真正的画面只占左下角
//  1280x720，右边 640px、上边 360px 全是黑边。
//
//  这个假象曾经浪费过一轮排查——"截图里 HUD 不见了、顶部全黑"其实只是
//  截图被撑大了，屏幕上的显示一直是对的。诊断工具本身不可信比没有工具更糟，
//  所以这里按逻辑分辨率自己读，得到一张与窗口逐像素对应的图。
//
//  顺带修掉第二个坑：TakeScreenshot 用 CORE.Storage.basePath + GetFileName()，
//  会静默丢掉参数里的目录（--shot build/a.png 会写成 a.png）。这里用传入路径。
//
//  【第三个坑，也是真正让"截图里没有 HUD"的那个】
//  raylib 的 2D 绘制是批处理的：DrawRectangle / DrawTextEx 只是往顶点批里塞
//  数据，真正提交给 GPU 要等 EndDrawing()。本循环里 renderer_.endWorld() 会
//  flush 一次世界层，但之后画的屏幕空间内容（状态栏、玩家状态、覆盖层）全部
//  还留在批里。此时直接 glReadPixels，读到的自然只有世界层——于是"截图里
//  找不到状态栏"，看起来像是 HUD 没渲染，其实是抓帧早于提交。
//
//  所以读像素前必须先 rlDrawRenderBatchActive() 强制 flush。
//  （不能简单地把截图挪到 endFrame() 之后：那时 SwapBuffers 已经发生，
//    后缓冲内容不再保证有效，抓到的可能是空帧或上一帧。）
// ---------------------------------------------------------------------------
bool captureScreenshot(const std::string& path) {
    const int width  = ::GetScreenWidth();
    const int height = ::GetScreenHeight();
    if (width <= 0 || height <= 0) {
        return false;
    }

    ::rlDrawRenderBatchActive();   // 把屏幕空间的待提交图元推给 GPU，否则拍不到 HUD

    unsigned char* pixels = ::rlReadScreenPixels(width, height);
    if (pixels == nullptr) {
        return false;
    }

    ::Image image{};
    image.data     = pixels;
    image.width    = width;
    image.height   = height;
    image.mipmaps  = 1;
    image.format   = ::PIXELFORMAT_UNCOMPRESSED_R8G8B8A8;

    ::ExportImage(image, path.c_str());
    ::UnloadImage(image);   // 释放 rlReadScreenPixels 分配的内存

    return ::FileExists(path.c_str());
}

// ===========================================================================
//  沙盒游戏层
//
//  实现 IGameLayer，把引擎的三个回调分发到 World 与调试层。
//  这是「引擎」与「游戏」之间的唯一接缝。
// ===========================================================================
class SandboxLayer final : public TacticalAI::Engine::IGameLayer {
public:
    SandboxLayer(const CommandLineOptions& options,
                 TacticalAI::Engine::Renderer& renderer,
                 TacticalAI::Engine::Input& input,
                 TacticalAI::Engine::Time& time)
        : renderer_(renderer), input_(input), time_(time),
          shotPath_(options.shotPath) {
        worldConfig_.initialEnemyCount = options.enemies;
        worldConfig_.enemiesUseEQS = options.useEQS;
        autoRollInterval_ = options.autoRollInterval;

        captureDir_   = options.captureDir;
        captureEvery_ = std::max(options.captureEvery, 1);
        demo_         = options.demo;

        if (!captureDir_.empty()) {
            std::error_code error;
            std::filesystem::create_directories(captureDir_, error);
            if (error) {
                std::fprintf(stderr, "[main] 无法创建抓帧目录 %s：%s\n",
                             captureDir_.c_str(), error.message().c_str());
                captureDir_.clear();
            } else {
                std::printf("[main] 连续抓帧 -> %s（每 %d 帧一张；结束后会打印合成命令）\n",
                            captureDir_.c_str(), captureEvery_);
            }
        }
        if (demo_) {
            // 演示模式默认打开三样"能看出 AI 在思考"的可视化：
            //   视野锥/视线、EQS 候选与评分、寻路路径。
            // 否则录出来的画面只是"两个人在对射"，看不出这是 AI 沙盒 ——
            // 而这些覆盖层恰恰是这个项目最值得展示的部分。
            auto& flags = debugDraw_.flags();
            flags.showPerception    = true;
            flags.showEQSCandidates = true;
            flags.showPaths         = true;

            std::printf("[main] 自动演示模式：内置驾驶员接管玩家（无需人操作）；"
                        "已打开感知/EQS/路径可视化\n");
        }

        // 截图帧号：未显式指定时取"最后一帧"。
        // 取最后一帧而不是第一帧，是因为开场时敌人还没接近，
        // 画面上不会有任何交火——那样的截图证明不了子弹画出来了。
        //
        // 【修复】原实现是 max(maxFrames - 1, 0)：只给 --shot 而不给
        // --frames 时（maxFrames = 0）算出 0，而帧计数从 0 开始 ——
        // 结果在第 0 帧（画面几乎空白）就截图，与"取最后一帧"完全相反。
        // 现在：有 --frames 就用它的最后一帧；没有则用一个能看出交火的
        // 默认帧（5 秒 = 300 帧），并打印实际使用的帧号。
        if (options.shotFrame >= 0) {
            shotFrame_ = options.shotFrame;
        } else if (options.maxFrames > 0) {
            shotFrame_ = options.maxFrames - 1;
        } else {
            shotFrame_ = kDefaultShotFrame;   // 300 帧 ≈ 5 秒
            std::printf("[main] 未指定 --shot-frame/--frames，截图取第 %d 帧（约 %.1f 秒）\n",
                        shotFrame_, static_cast<double>(shotFrame_) / 60.0);
        }
    }

    /// 是否需要结束主循环（由 --shot 截图完成触发）。
    [[nodiscard]] bool exitRequested() const noexcept { return exitRequested_; }

    // -----------------------------------------------------------------------
    //  初始化
    // -----------------------------------------------------------------------
    bool onInitialize() override {
        world_ = std::make_unique<TacticalAI::Game::World>(worldConfig_);
        world_->initialize();

        // 初始化调试 UI。失败（无 imgui 后端）不是错误——
        // 会自动降级为原生 HUD。
        debugUI_.initialize();

        // 相机初始对准玩家
        if (const auto* player = world_->player()) {
            renderer_.camera().setTarget(player->position());
        }
        renderer_.camera().setZoom(0.85f);

        std::printf("\n");
        std::printf("  操作提示：\n");
        std::printf("    WASD 移动 / 鼠标瞄准 / F 开火 / Shift 冲刺 / 空格 翻滚 / P 暂停 / Tab 慢动作\n");
        std::printf("    N 增兵 / R 全部重新寻路 / 回车 重开\n");
        std::printf("    1-7 切换 AI 可视化 (7 = 子弹与弹道) / F1 打开调参面板\n");
        std::printf("\n");

        return true;
    }

    // -----------------------------------------------------------------------
    //  固定步长逻辑
    // -----------------------------------------------------------------------
    void onFixedUpdate(double fixedDelta) override {
        // 冻结开关：让开发者可以「暂停观察」AI 的世界而不停止渲染。
        // 与 Time::setPaused 的区别是它只冻结 AI，玩家仍可移动——
        // 便于逐帧观察某个敌人对玩家动作的反应。
        //
        // 【修复】原实现在这里直接 return，于是玩家输入、相机跟随、
        // 整个世界的推进（连子弹飞行）全部停住 —— F2 实际是第二个全局
        // 暂停键，与上面这段注释描述的语义正好相反。
        // 现在改为只关闭敌人的 AI，世界其余部分照常推进。
        world_->setAIEnabled(!brainFrozen_);
        if (brainFrozen_) {
            ++frozenFrames_;
        }

        // 重开放在这里而不是 onVariableUpdate：它必须与逻辑同帧生效，
        // 否则按下回车的那一帧世界还会照常推进（可能刚好把玩家打死，
        // 于是"重开"完又立刻显示失败）。
        if (input_.consumeActionPressed(TacticalAI::Engine::Action::Restart)) {
            restartRun();
        }

        handleGameplayInput(fixedDelta);

        world_->update(fixedDelta, time_.gameTime());

        // 在 update 之后采样：本帧的伤害已经结算完，
        // 否则"最后一击打死最后一个敌人"要到下一帧才显示胜利。
        lastOutcome_ = world_->outcome();

        updateCamera(fixedDelta);
    }

    // -----------------------------------------------------------------------
    //  可变更新（每渲染帧）
    // -----------------------------------------------------------------------
    void onVariableUpdate(const TacticalAI::Engine::FrameTime& frameTime) override {
        (void)frameTime;

        // 相机操作（中键平移 / 滚轮缩放）必须按渲染帧处理，理由见函数注释。
        updateCameraControls();

        // 调试开关的快捷键在可变更新里处理，因为它们与逻辑无关，
        // 且需要「按一次切换一次」的即时反馈。
        auto& flags = debugDraw_.flags();

        if (input_.consumeActionPressed(TacticalAI::Engine::Action::TogglePerceptionDebug)) {
            flags.showPerception = !flags.showPerception;
        }
        if (input_.consumeActionPressed(TacticalAI::Engine::Action::ToggleBehaviorTreeDebug)) {
            flags.showBehaviorTree = !flags.showBehaviorTree;
            // 行为树开关同时控制面板的「实体检视」页，保持一致
            debugUI_.setVisible(true);
        }
        if (input_.consumeActionPressed(TacticalAI::Engine::Action::ToggleEQSGridDebug)) {
            flags.showEQSCandidates = !flags.showEQSCandidates;
        }
        if (input_.consumeActionPressed(TacticalAI::Engine::Action::TogglePathDebug)) {
            flags.showPaths = !flags.showPaths;
        }

        // 数字键 5/6/7 没有对应的 Action，直接查键
        if (input_.consumeKeyPressed(KEY_FIVE)) {
            flags.showNavGridBlocked = !flags.showNavGridBlocked;
        }
        if (input_.consumeKeyPressed(KEY_SIX)) {
            flags.showLabels = !flags.showLabels;
        }
        // 7 = 子弹与弹着残影。关掉之后子弹本身也不画了——
        // 物理照常运行（子弹仍然会飞、会撞墙、会命中），只是不可见。
        // 调试"打击感来自视觉还是来自机制"时很有用。
        if (input_.consumeKeyPressed(KEY_SEVEN)) {
            flags.showShotTracers = !flags.showShotTracers;
        }

        // F1 切换面板
        if (input_.consumeActionPressed(TacticalAI::Engine::Action::ToggleDebugPanel)) {
            debugUI_.toggleVisible();
        }

        // F2 冻结/解冻 AI 决策
        if (input_.consumeKeyPressed(KEY_F2)) {
            brainFrozen_ = !brainFrozen_;
            std::printf("[Sandbox] AI 决策 %s\n", brainFrozen_ ? "已冻结" : "已恢复");
        }

        // F3 打印世界状态
        if (input_.consumeKeyPressed(KEY_F3)) {
            std::printf("%s\n", world_->dump().c_str());
        }
    }

    // -----------------------------------------------------------------------
    //  渲染
    // -----------------------------------------------------------------------
    void onRender(float alpha) override {
        (void)alpha;

        // --- 世界空间 ---
        renderer_.beginWorld();

        debugDraw_.draw(renderer_, *world_);

        renderer_.endWorld();

        // --- 屏幕空间 ---
        debugDraw_.drawOverlay(renderer_, *world_);

        // 顶部状态条：不依赖 imgui 的关键信息
        drawStatusBar();

        // 结算横幅：仅在分出胜负时出现（画在状态条之后，
        // 压在世界层之上、调试面板之下）
        drawOutcomeBanner();

        // --- 调试面板 ---
        debugUI_.beginFrame();
        debugUI_.draw(makeUIBindings(), makeUIActions(), loopStats());
        debugUI_.endFrame();

        // --- 连续抓帧（--capture）---
        //
        // 与 --shot 同一个时机、同一个理由：必须在 endFrame 之前抓，
        // 否则 SwapBuffers 已经发生，读到的是空帧或上一帧。
        // 区别只是它每 N 帧抓一次、并且**不**结束主循环。
        if (!captureDir_.empty() && captureEvery_ > 0 &&
            (time_.frameCount() % static_cast<uint64_t>(captureEvery_)) == 0) {
            char path[512];
            std::snprintf(path, sizeof(path), "%s/frame_%04d.png",
                          captureDir_.c_str(), captureIndex_);
            if (captureScreenshot(path)) {
                ++captureIndex_;
            } else {
                std::fprintf(stderr, "[main] 抓帧失败: %s\n", path);
            }
        }

        // --- 截图 ---
        // 必须在 endFrame 之前：抓的是当前仍处于绘制状态的后缓冲，
        // 放到 endFrame 之后就会抓到已经翻页（可能是空）的缓冲区。
        if (!shotPath_.empty() && !shotTaken_ &&
            static_cast<int>(time_.frameCount()) >= shotFrame_) {
            shotTaken_ = true;
            const bool ok = captureScreenshot(shotPath_);

            if (ok) {
                ::Image probe = ::LoadImage(shotPath_.c_str());
                std::printf("[main] 已截图: %s (第 %llu 帧, 空中子弹 %zu)\n",
                            shotPath_.c_str(),
                            static_cast<unsigned long long>(time_.frameCount()),
                            world_->projectileCount());
                std::printf("[main]   截图 %dx%d, 窗口 %dx%d, DPI 缩放 %.2f\n",
                            probe.width, probe.height,
                            ::GetScreenWidth(), ::GetScreenHeight(),
                            static_cast<double>(::GetWindowScaleDPI().x));
                ::UnloadImage(probe);
            } else {
                std::printf("[main] 截图失败: %s\n", shotPath_.c_str());
            }

            // 【修复】--help 里写的是"截图后退出"，但原实现只做"只截一次"，
            // 不给 --frames 时会一直跑到手动关窗 —— 脚本化截图会挂住。
            // 现在真的请求退出（main 的主循环会读到它），与文档一致。
            exitRequested_ = true;
        }
    }

    void onShutdown() override {
        // 关闭 imgui 必须在窗口销毁之前
        debugUI_.shutdown();

        // 抓完帧之后把合成命令直接打出来：录制流程的下一步是 ffmpeg，
        // 让用户不必回来翻文档，也避免手抄帧率/帧号写错。
        if (!captureDir_.empty()) {
            std::printf("\n[main] 共导出 %d 帧到 %s/\n", captureIndex_, captureDir_.c_str());
            if (captureIndex_ > 0) {
                // 【素材帧率怎么算才对】
                // 不用"渲染帧率 / captureEvery"：渲染帧率受 vsync 与机器性能影响，
                // 关掉 vsync 后可能跑到几百帧，那样算出来的帧率毫无意义。
                // 用"导出张数 ÷ 游戏时长"最稳 —— 它只取决于逻辑时间，与帧率无关。
                const double gameSeconds = std::max(time_.gameTime(), 1e-3);
                const int fps = std::clamp(
                    static_cast<int>(static_cast<double>(captureIndex_) / gameSeconds + 0.5),
                    5, 60);
                std::printf("[main] 素材帧率约 %d fps。合成：\n", fps);
                std::printf("  ffmpeg -y -framerate %d -i %s/frame_%%04d.png "
                            "-vf \"scale=960:-2\" -c:v libx264 -pix_fmt yuv420p docs/images/demo.mp4\n",
                            fps, captureDir_.c_str());
                std::printf("  pwsh ./scripts/make_gif.ps1 -Frames %s -Fps %d\n",
                            captureDir_.c_str(), fps);
            }
        }

        std::printf("\n%s\n", world_->dump().c_str());
    }

    /// 引擎循环统计注入点（由 main 在构造后设置）。
    void setLoopStatsProvider(std::function<TacticalAI::Engine::LoopStats()> provider) {
        loopStatsProvider_ = std::move(provider);
    }

private:
    // -----------------------------------------------------------------------
    //  顶部状态条的几何常量
    //
    //  这两个数有两个消费者：drawStatusBar() 自己，以及 makeUIBindings()——
    //  后者要把"顶部被占了多少"告诉调试层，好让调试 HUD 从下方开始画。
    //  所以必须是同一份常量：两边各写一个魔数，改了一处就会悄悄文字压文字。
    // -----------------------------------------------------------------------
    static constexpr float kStatusBarHeight    = 44.0f;                       ///< 状态条底板高度
    static constexpr float kStatusBarPlayerTop = kStatusBarHeight + 4.0f;     ///< 玩家状态行顶边

    /// --shot 未指定帧号、也没有 --frames 时使用的默认截图帧。
    /// 300 帧 ≈ 5 秒：此时敌人已经接近并开始交火，截图才说明得了问题。
    static constexpr int kDefaultShotFrame = 300;

    /// 顶部需要给状态条让出的总高度（含玩家状态行与其下留白）。
    static constexpr float kStatusBarReserved =
        kStatusBarPlayerTop + 12.0f + 6.0f;

    // -----------------------------------------------------------------------
    //  输入 -> 游戏意图
    // -----------------------------------------------------------------------

    /// 起手一次翻滚并打日志。
    ///
    /// 键盘与 --auto-roll 共用同一个出口。分成两份实现的坏处很具体：
    /// 将来改了触发条件（比如加上"必须有体力"），只改到一边，
    /// 就会变成"手按能滚、自动滚不了"这类只在特定运行方式下出现的怪问题。
    void requestRoll(const char* source) {
        auto* player = world_->player();
        if (player == nullptr) {
            return;
        }

        if (!player->tryStartRoll(time_.gameTime())) {
            return;   // 翻滚中或冷却未过，静默拒绝
        }

        lastAutoRollTime_ = time_.gameTime();

        std::printf("[Sandbox] 翻滚[%s] dir=(%.2f, %.2f)\n",
                    source,
                    static_cast<double>(player->rollDirection().x),
                    static_cast<double>(player->rollDirection().y));
    }

    /// 重开一局：清空世界重建，并把相机与统计归位。
    ///
    /// 【为什么单独一个函数，而不是就地写三行】
    /// 重开有一处**容易漏**的副作用：相机还指着上一局的玩家位置。
    /// 而玩家在 reset 后回到 playerStart，两者通常相距很远，于是重开后
    /// 画面会长时间对着空地，直到玩家动一下才回到角色身上。
    /// 把它和"重置统计"一起收在这里，漏哪一处都更容易发现。
    void restartRun() {
        world_->reset();

        brainFrozen_ = false;
        world_->setAIEnabled(true);
        lastOutcome_ = TacticalAI::Game::MatchOutcome::Running;
        lastAutoRollTime_ = -1e9;

        if (const auto* player = world_->player()) {
            renderer_.camera().setTarget(player->position());
        }

        std::printf("[Sandbox] 已重开一局\n");
    }

    void handleGameplayInput(double fixedDelta) {
        (void)fixedDelta;

        auto* player = world_->player();
        if (!player || player->isDead()) {
            return;
        }

        // 分出胜负后冻结玩家操作。
        //
        // 【为什么只冻结玩家，不冻结世界】
        // 失败时敌人会失去感知目标、退回巡逻（见 World::collectPerceptionTargets
        // 的说明），如果这时把世界也停掉，就会复现旧版"打完没结果"的那个
        // 问题：不仅没有反馈，连现场都观察不到。保持世界推进，
        // 玩家就能看着敌人走开、子弹落地、尸体消失——这本身就是结算画面。
        //
        // 沙盒里允许胜利后按 N 增兵继续打，所以这里不硬拦"胜利"，
        // 只拦"失败"（玩家已死，操作本就无意义）。
        if (lastOutcome_ == TacticalAI::Game::MatchOutcome::Defeat) {
            return;
        }

        // --- 自动演示（--demo）---
        //
        // 只把"玩家意图"的来源从 Input 换成 AutoPilot，其余链路（Player 的
        // 消费、World 的结算、渲染）完全不变 —— 所以演示里看到的行为
        // 与真人操作是同一条路径，不会出现"演示特供逻辑"。
        if (demo_) {
            autoPilot_.update(*world_, *player, time_.gameTime(), fixedDelta);
            return;
        }

        // 移动意图：来自键盘的归一化方向
        const Vector2 axis = input_.moveAxis();
        player->setMoveIntent(glm::vec2{axis.x, axis.y});
        player->setSprinting(input_.isActionDown(TacticalAI::Engine::Action::Sprint));

        // 瞄准：左 Ctrl 或鼠标右键。
        // 空格原本也是瞄准键，现在让给了翻滚；鼠标右键是这类俯视射击里
        // "按住放慢脚步"最自然的手感，所以两个都接受。
        player->setAiming(
            input_.isActionDown(TacticalAI::Engine::Action::Aim) ||
            input_.isMouseDown(TacticalAI::Engine::MouseButton::Right));

        // 瞄准：把鼠标屏幕坐标转换成世界坐标。
        // 相机可能在移动，因此必须每步重算。
        const Vector2 mouseScreen = input_.mousePosition();
        const glm::vec2 mouseWorld = renderer_.camera().screenToWorld(
            glm::vec2{mouseScreen.x, mouseScreen.y});

        player->setLookAtPoint(mouseWorld);

        // 翻滚。
        //
        // 【为什么必须边沿触发】
        // 消费式边沿触发保证一个按键事件只被读走一次。如果这里写成
        // isActionDown，玩家按住空格不放，只要冷却一过就会自动接上下一次
        // 翻滚，"按住就一直滚"，冷却也就形同虚设了。
        //
        // 方向由 Player 自己解析（移动输入 > 鼠标方向 > 当前朝向），
        // 因为这三样输入都已经通过 setMoveIntent / setLookAtPoint 交进去了——
        // 让 Player 独占这条决策，测试里才能用脚本化输入完整驱动它。
        if (input_.consumeActionPressed(TacticalAI::Engine::Action::Dodge)) {
            requestRoll("按键");
        }

        // 自动翻滚：只在 --auto-roll 打开时生效。
        // 与按键走同一个 requestRoll，所以不会出现"自动触发的规则和手按的不一样"。
        //
        // 【修复】时间戳原先只在 tryStartRoll **成功**后才更新，于是冷却
        // 期间每一步都会重试，冷却一过立刻翻滚 —— 实际间隔由翻滚冷却
        // （0.8s）决定，命令行传的 --auto-roll 5.0 完全不起作用。
        if (autoRollInterval_ > 0.0 &&
            time_.gameTime() - lastAutoRollTime_ >= autoRollInterval_) {
            // 无论本次是否成功都打时间戳，间隔才真正由参数决定
            lastAutoRollTime_ = time_.gameTime();
            requestRoll("自动");
        }

        // 开火
        if (input_.consumeActionPressed(TacticalAI::Engine::Action::Fire)) {
            player->requestFire();
        }

        // 生成敌人
        if (input_.consumeActionPressed(TacticalAI::Engine::Action::SpawnEnemy)) {
            if (auto* enemy = world_->spawnEnemy()) {
                std::printf("[Sandbox] 生成敌人 %s @ (%.0f, %.0f)\n",
                            enemy->name().c_str(),
                            static_cast<double>(enemy->position().x),
                            static_cast<double>(enemy->position().y));
            }
        }

        // 全部重新寻路：用于观察 AI 换路的过程
        if (input_.consumeActionPressed(TacticalAI::Engine::Action::RerouteAgents)) {
            for (auto* enemy : world_->enemies()) {
                enemy->requestRepath();
            }
            std::printf("[Sandbox] 已请求全部敌人重新寻路\n");
        }
    }

    // -----------------------------------------------------------------------
    //  相机
    // -----------------------------------------------------------------------
    void updateCamera(double fixedDelta) {
        const auto* player = world_->player();

        auto& camera = renderer_.camera();

        if (player && !player->isDead()) {
            // 平滑跟随：用带阻尼的插值而非硬吸附，
            // 否则玩家移动时画面会跟着抖动，长时间看很累。
            const glm::vec2 target = player->position();
            const glm::vec2 current = camera.target();
            const float smoothing = static_cast<float>(std::min(1.0, fixedDelta * 8.0));
            camera.setTarget(current + (target - current) * smoothing);
        }

    }

    /// 中键拖拽平移 + 滚轮缩放。
    ///
    /// 【为什么放在渲染帧（onVariableUpdate）而不是固定步里】
    /// 1. 它读的是"本帧鼠标位移"（input_.mouseDelta 每渲染帧更新一次），
    ///    放在固定步里会在慢动作时丢帧、补步时重复施加，平移速度随帧率漂移；
    /// 2. 更关键的是暂停：暂停后不再产生固定步，若这段逻辑在固定步里，
    ///    玩家就**完全无法平移/缩放**去观察战场——而"暂停下来看清局面"
    ///    正是暂停键存在的意义。
    void updateCameraControls() {
        auto& camera = renderer_.camera();

        if (input_.isMouseDown(TacticalAI::Engine::MouseButton::Middle)) {
            const Vector2 delta = input_.mouseDelta();
            const float zoom = camera.zoom();
            if (zoom > 1e-4f) {
                camera.moveBy(glm::vec2{-delta.x / zoom, -delta.y / zoom});
            }
        }

        const float wheel = input_.mouseWheel();
        if (std::fabs(wheel) > 0.001f) {
            camera.zoomBy(1.0f + wheel * 0.12f);
        }
    }

    // -----------------------------------------------------------------------
    //  HUD
    // -----------------------------------------------------------------------
    /// 对局结算横幅。分出胜负时画在屏幕中央，失败时额外给出重开提示。
    ///
    /// 【为什么只占屏幕中间一小块，而不是全屏遮罩】
    /// 失败后世界仍在推进（敌人退巡逻、子弹落地、尸体消失）。
    /// 盖一层全屏遮罩会把这些全挡住，而它们恰恰是结算的一部分——
    /// "我最后是怎么死的"往往就写在那几颗还在飞的子弹上。
    /// 所以这里只压一个半透明底板，位置偏上，正文照样看得见。
    void drawOutcomeBanner() {
        if (lastOutcome_ == TacticalAI::Game::MatchOutcome::Running) {
            return;
        }

        const glm::vec2 screen = renderer_.screenSize();

        const bool victory = lastOutcome_ == TacticalAI::Game::MatchOutcome::Victory;
        const auto color = victory
                               ? TacticalAI::Engine::Colors::kSuccess
                               : TacticalAI::Engine::Colors::kError;

        // 标题文案
        char title[64];
        std::snprintf(title, sizeof(title), "%s",
                      victory ? "胜利" : "失败");

        // 副标题：给出本局的关键数字 + 下一步该按什么。
        // 失败时必须提示重开键——否则玩家看到"失败"却不知道怎么继续，
        // 这正是旧版"打完没结果"的半个问题。
        char subtitle[192];
        if (victory) {
            std::snprintf(subtitle, sizeof(subtitle),
                          "用时 %.1fs · 击杀 %d · 命中率 %.0f%% · 回车重开 / N 增兵继续",
                          world_->settledElapsedTime(),
                          world_->enemiesKilled(),
                          world_->totalShotsFired() > 0
                              ? 100.0 * static_cast<double>(world_->totalProjectileHits()) /
                                    static_cast<double>(world_->totalShotsFired())
                              : 0.0);
        } else {
            std::snprintf(subtitle, sizeof(subtitle),
                          "坚持 %.1fs · 击杀 %d · 承受伤害 %.0f · 回车重开",
                          world_->settledElapsedTime(),
                          world_->enemiesKilled(),
                          static_cast<double>(world_->totalDamageDealt()));
        }

        // 标题用 34 号、副标题 14 号，据此算出底板尺寸。
        const int titleSize = 34;
        const int subSize   = 14;
        const int titleW = TacticalAI::Engine::Renderer::measureText(title, titleSize);
        const int subW   = TacticalAI::Engine::Renderer::measureText(subtitle, subSize);

        const float paddingX = 28.0f;
        const float paddingY = 18.0f;
        const float gap      = 10.0f;

        const float panelW = static_cast<float>(std::max(titleW, subW)) + paddingX * 2.0f;
        const float panelH = static_cast<float>(titleSize + subSize) + paddingY * 2.0f + gap;

        // 偏上（约屏幕高度的 30% 处）：正中央会被玩家角色和 HUD 挡住。
        const glm::vec2 panelPos{
            (screen.x - panelW) * 0.5f,
            screen.y * 0.30f};

        renderer_.drawPanel({panelPos.x, panelPos.y, panelW, panelH},
                            TacticalAI::Engine::Colors::kPanelBg,
                            color);

        renderer_.drawScreenText(
            title,
            {screen.x * 0.5f, panelPos.y + paddingY},
            titleSize, color, TacticalAI::Engine::TextAlign::Center);

        renderer_.drawScreenText(
            subtitle,
            {screen.x * 0.5f, panelPos.y + paddingY + titleSize + gap},
            subSize, TacticalAI::Engine::Colors::kTextMuted,
            TacticalAI::Engine::TextAlign::Center);
    }

    void drawStatusBar() {
        const glm::vec2 screen = renderer_.screenSize();

        // 顶部左侧：循环状态
        const TacticalAI::Engine::LoopStats stats = loopStats();

        char line1[256];
        std::snprintf(line1, sizeof(line1),
                      "%.0f FPS | 帧 %llu | 逻辑步 %llu | 敌人 %zu/%zu",
                      static_cast<double>(time_.fps()),
                      static_cast<unsigned long long>(time_.frameCount()),
                      static_cast<unsigned long long>(stats.totalFixedSteps),
                      world_->aliveEnemyCount(), world_->enemyCount());

        // 子弹数放在状态条上：它是"攻击是一个实体"最直观的证据——
        // 交火时这个数字会一直跳动，也能量化"空中的弹幕有多密"。
        // 闪避数同理：它是"无敌帧真的在跑"唯一能当场看到的证据。
        char line2[256];
        std::snprintf(line2, sizeof(line2),
                      "AI %.3f ms | 子弹 %zu 空中 | 命中 %zu/%zu | 闪避 %zu | 击杀 %d | 伤害 %.0f | %s%s",
                      world_->averageAITimeMs(),
                      world_->projectileCount(),
                      world_->totalProjectileHits(), world_->totalShotsFired(),
                      world_->totalProjectilesDodged(),
                      world_->enemiesKilled(),
                      static_cast<double>(world_->totalDamageDealt()),
                      time_.isPaused() ? "已暂停 " : "",
                      brainFrozen_ ? "AI已冻结" : "");

        renderer_.drawPanel({0.0f, 0.0f, screen.x, kStatusBarHeight},
                            TacticalAI::Engine::Colors::kPanelBg,
                            TacticalAI::Engine::Colors::kPanelBorder);

        renderer_.drawScreenText(line1, {10.0f, 6.0f}, 13,
                                 TacticalAI::Engine::Colors::kTextPrimary);
        renderer_.drawScreenText(line2, {10.0f, 24.0f}, 12,
                                 TacticalAI::Engine::Colors::kTextMuted);

        // 顶部右侧：面板后端提示
        const char* backendHint = debugUI_.usingImGui()
                                      ? "F1 调参面板 (imgui)"
                                      : "F1 只读 HUD (无 imgui 后端)";
        renderer_.drawScreenText(backendHint, {screen.x - 10.0f, 24.0f}, 12,
                                 TacticalAI::Engine::Colors::kTextMuted,
                                 TacticalAI::Engine::TextAlign::Right);

        // 玩家状态
        if (const auto* player = world_->player()) {
            // 冷却状态显示成"就绪 / 0.08s"，让"为什么这一枪没打出去"有据可查。
            const double cooldown = player->fireCooldownRemaining(time_.gameTime());
            char readyText[32];
            if (cooldown <= 0.0) {
                std::snprintf(readyText, sizeof(readyText), "就绪");
            } else {
                std::snprintf(readyText, sizeof(readyText), "冷却 %.2fs", cooldown);
            }

            // 翻滚状态分四种，必须分开显示：
            //   翻滚中[无敌] 0.18s  —— 动作进行中，且还在无敌窗口里
            //   翻滚中[破绽] 0.06s  —— 动作还没结束，但无敌已经过了
            //   翻滚 0.45s          —— 动作结束，冷却还在走
            //   翻滚就绪             —— 随时可用
            //
            // 把"翻滚中"和"无敌中"合成一个状态，会丢掉最有用的那条信息：
            // 玩家真正想知道的是"现在滚一下躲不躲得掉"。翻滚有 0.35s，
            // 无敌只有前 0.25s，最后 0.1s 是实打实的破绽——
            // 这个区别决定了"滚早了照样吃子弹"，必须能看见。
            char rollText[48];
            if (player->isRolling()) {
                const double remaining =
                    (1.0 - static_cast<double>(player->rollProgress())) *
                    static_cast<double>(player->config().rollDuration);
                std::snprintf(rollText, sizeof(rollText), "翻滚中[%s] %.2fs",
                              player->isInvulnerable() ? "无敌" : "破绽",
                              remaining);
            } else {
                const double rollCooldown = player->rollCooldownRemaining(time_.gameTime());
                if (rollCooldown <= 0.0) {
                    std::snprintf(rollText, sizeof(rollText), "翻滚就绪");
                } else {
                    std::snprintf(rollText, sizeof(rollText), "翻滚 %.2fs", rollCooldown);
                }
            }

            char playerLine[320];
            std::snprintf(playerLine, sizeof(playerLine),
                          "玩家 HP %.0f/%.0f | 射击 %d | 造成伤害 %d | 武器[%s] | %s",
                          static_cast<double>(player->health()),
                          static_cast<double>(player->maxHealth()),
                          player->shotsFired(),
                          player->damageDealt(),
                          readyText,
                          rollText);

            renderer_.drawScreenText(playerLine, {10.0f, kStatusBarPlayerTop}, 12,
                                     player->isDead()
                                         ? TacticalAI::Engine::Colors::kError
                                         : TacticalAI::Engine::Colors::kPlayer);
        }

        // 无 imgui 时，额外的只读 HUD 画在玩家状态下方
        if (!debugUI_.usingImGui() && debugUI_.isVisible()) {
            const TacticalAI::Engine::Renderer& constRenderer = renderer_;
            (void)constRenderer;
        }
    }

    // -----------------------------------------------------------------------
    //  调试 UI 绑定
    // -----------------------------------------------------------------------
    TacticalAI::Debug::DebugUIBindings makeUIBindings() {
        TacticalAI::Debug::DebugUIBindings bindings;
        bindings.time        = &time_;
        bindings.world       = world_.get();
        bindings.drawFlags   = &debugDraw_.flags();
        bindings.input       = &input_;
        bindings.brainFrozen = &brainFrozen_;
        bindings.topReserved = kStatusBarReserved;   // 调试 HUD 从状态条下方开始
        return bindings;
    }

    TacticalAI::Debug::DebugUIActions makeUIActions() {
        TacticalAI::Debug::DebugUIActions actions;

        actions.togglePause = [this]() { time_.togglePaused(); };
        actions.stepFrame = [this]() {
            if (time_.isPaused()) {
                time_.requestSingleStep();
            }
        };
        actions.resetWorld = [this]() {
            // 【修复】面板上的"重置世界"原来只调 world_->reset()，而回车重开
            // 走的是 restartRun()。两条"重开"路径的副作用因此不同：面板这条路
            // 不会清结算横幅、不会把相机拉回角色、也不会解除 AI 冻结 ——
            // 同一个操作在两个入口表现不一致，是典型的"同一事实多处维护"。
            // 现在统一走 restartRun()。
            restartRun();
        };
        actions.spawnEnemy = [this]() {
            (void)world_->spawnEnemy();
        };
        actions.clearEnemies = [this]() {
            for (auto* enemy : world_->enemies()) {
                world_->removeEntity(enemy->id());
            }
            std::printf("[Sandbox] 已清空所有敌人\n");
        };
        actions.killAllEnemies = [this]() {
            for (auto* enemy : world_->enemies()) {
                enemy->kill();
            }
        };
        actions.toggleSlowMotion = [this]() {
            const double next = (time_.timeScale() < 1.0) ? 1.0 : 0.25;
            time_.setTimeScale(next);
        };
        actions.requestRepath = [this]() {
            for (auto* enemy : world_->enemies()) {
                enemy->requestRepath();
            }
        };
        actions.dumpWorldState = [this]() {
            std::printf("%s\n", world_->dump().c_str());
        };
        actions.damagePlayer = [this]() {
            if (auto* player = world_->player()) {
                world_->applyDamage(player->id(), 20.0f, 0, player->position(),
                                    time_.gameTime());
                std::printf("[Sandbox] 对玩家造成 20 伤害 (当前 %.0f)\n",
                            static_cast<double>(player->health()));
            }
        };

        return actions;
    }

    TacticalAI::Engine::LoopStats loopStats() const {
        return loopStatsProvider_ ? loopStatsProvider_() : TacticalAI::Engine::LoopStats{};
    }

    // -- 引用（生命周期由 main 保证） --
    TacticalAI::Engine::Renderer& renderer_;
    TacticalAI::Engine::Input&    input_;
    TacticalAI::Engine::Time&     time_;

    // -- 拥有的对象 --
    std::unique_ptr<TacticalAI::Game::World> world_;
    TacticalAI::Game::WorldConfig worldConfig_{};
    TacticalAI::Debug::AIDebugDraw debugDraw_;
    TacticalAI::Debug::DebugUI     debugUI_;

    bool brainFrozen_ = false;
    uint64_t frozenFrames_ = 0;

    /// 沙盒层请求结束整个进程（目前只有 --shot 截图完成后会置位）。
    /// GameLoop 的 layer 引用不可更换，因此退出请求由 main 的主循环读取。
    bool exitRequested_ = false;

    // -- 截图（--shot） --
    std::string shotPath_;
    int  shotFrame_ = 0;
    bool shotTaken_ = false;

    // -- 连续抓帧（--capture） --
    std::string captureDir_;         ///< 空 = 不抓帧
    int  captureEvery_ = 4;          ///< 每 N 个渲染帧一张
    int  captureIndex_ = 0;          ///< 已导出的张数（也是文件名序号）

    // -- 自动演示（--demo） --
    bool demo_ = false;
    TacticalAI::Game::AutoPilot autoPilot_;

    // -- 自动翻滚（--auto-roll） --
    double autoRollInterval_  = 0.0;
    double lastAutoRollTime_  = -1e9;

    /// 上一帧的对局结果。由 onFixedUpdate 从 World 读取，
    /// 供 handleGameplayInput（冻结输入）与 drawOutcomeBanner（结算显示）使用。
    ///
    /// 缓存成成员而不是每次直接问 World::outcome()：outcome() 会遍历实体，
    /// 而这里每帧要问两次（输入 + 绘制）。更关键的是**一致性**——
    /// 两处读到同一个值，不会出现"输入已冻结但画面还没显示"的半帧错位。
    TacticalAI::Game::MatchOutcome lastOutcome_ =
        TacticalAI::Game::MatchOutcome::Running;

    std::function<TacticalAI::Engine::LoopStats()> loopStatsProvider_;
};

} // namespace

// ===========================================================================
//  main
// ===========================================================================
int main(int argc, char** argv) {
    // 必须在任何 printf 之前：否则第一行中文就已经是乱码了。
    setupConsoleEncoding();

    const CommandLineOptions options = parseCommandLine(argc, argv);

    if (options.showHelp) {
        printUsage(argc > 0 ? argv[0] : "TacticalAI-Sandbox");
        return 0;
    }

    std::printf("================================================\n");
    std::printf("  Tactical AI Sandbox v0.1.0\n");
    std::printf("  行为树 / 黑板 / 感知 / EQS / 寻路\n");
    std::printf("================================================\n");

    if (options.headless) {
        // 无头模式：不创建窗口，仅跑逻辑。
        // 用途：CI 冒烟测试、性能基准、自动化行为验证。
        std::printf("[main] 无头模式：不创建窗口\n");

        // 无头分支只跑 World 的推进，不经过 SandboxLayer——也就是说
        // 玩家的输入、相机、截图、--auto-roll 在这里全都不存在。
        // 静默忽略一个显式给出的选项最容易让人以为"功能坏了"，所以明确提示。
        if (options.autoRollInterval > 0.0) {
            std::printf("[警告] --auto-roll 只在窗口模式有效（无头模式没有玩家输入）\n");
        }
        if (!options.shotPath.empty()) {
            std::printf("[警告] --shot 需要窗口，无头模式下不会截图\n");
        }

        TacticalAI::Game::WorldConfig worldConfig;
        worldConfig.initialEnemyCount = options.enemies;
        worldConfig.enemiesUseEQS = options.useEQS;

        TacticalAI::Game::World world(worldConfig);
        world.initialize();

        const int totalFrames = (options.maxFrames > 0) ? options.maxFrames : 600;
        const double fixedDelta = options.fixedDeltaMs / 1000.0;

        double gameTime = 0.0;
        for (int frame = 0; frame < totalFrames; ++frame) {
            world.update(fixedDelta, gameTime);
            gameTime += fixedDelta;
        }

        std::printf("\n[main] 无头运行完成: %d 逻辑步, 游戏时间 %.2f s\n",
                    totalFrames, gameTime);
        std::printf("%s\n", world.debugString().c_str());
        if (options.dumpWorld) {
            std::printf("%s\n", world.dump().c_str());
        }
        return 0;
    }

    // --- 构建循环配置 ---
    TacticalAI::Engine::GameLoop::Config loopConfig;
    loopConfig.screenWidth  = options.width;
    loopConfig.screenHeight = options.height;
    // 窗口标题用中文，与界面其余文案保持一致。
    // 标题栏由 raylib 交给操作系统绘制，中文是否显示取决于系统是否能
    // 从窗口标题里解析 UTF-8——Windows 10+ 支持良好。
    loopConfig.title        = "战术 AI 沙盒";
    loopConfig.fixedDelta   = options.fixedDeltaMs / 1000.0;
    loopConfig.maxFrames    = options.maxFrames;
    loopConfig.enableVsync  = options.vsync;
    // targetFPS = 0 且开启 vsync 时，由显示器刷新率决定上限。
    // 关闭 vsync 时限制到 240，避免无意义的 GPU 满载。
    loopConfig.targetFPS    = options.vsync ? 0 : 240;

    // --- 组装 ---
    // 注意构造顺序：GameLoop 需要一个 IGameLayer，而 SandboxLayer
    // 需要 GameLoop 内部的 Renderer/Input/Time。
    // 用一个「两阶段构造」解决循环依赖：
    //   1. 先用占位 layer 建 GameLoop（此时还没初始化窗口）
    //   2. initialize() 之后再真正构造 SandboxLayer 并注入
    //
    // 更简洁的做法是让 GameLoop 暴露子系统创建回调，
    // 但那会让引擎层接口复杂化。这里的做法保持了接口简单。

    // 阶段 1：用一个空实现占位，只为拿到 GameLoop 的子系统
    class PlaceholderLayer final : public TacticalAI::Engine::IGameLayer {
    public:
        void onFixedUpdate(double) override {}
        void onVariableUpdate(const TacticalAI::Engine::FrameTime&) override {}
        void onRender(float) override {}
    };

    PlaceholderLayer placeholder;
    TacticalAI::Engine::GameLoop loop(loopConfig, placeholder);

    if (!loop.initialize()) {
        std::fprintf(stderr, "[main] 引擎初始化失败\n");
        return 1;
    }

    // 阶段 2：用真正的沙盒层替换。
    // GameLoop 持有 layer 的引用，因此必须让真正的 layer 活得更久——
    // 声明在同一个作用域内即可保证析构顺序正确（后声明的先析构）。
    SandboxLayer sandbox(options, loop.renderer(), loop.input(), loop.time());
    sandbox.setLoopStatsProvider([&loop]() -> TacticalAI::Engine::LoopStats {
        return loop.stats();
    });

    // 重新初始化：这次会带上真正的游戏层
    if (!sandbox.onInitialize()) {
        std::fprintf(stderr, "[main] 沙盒初始化失败\n");
        return 1;
    }

    // 字体加载结果：这是「窗口里中文变方块」最直接的诊断信息。
    // 成功时打印路径与字形数；失败时说明回退到了默认字体（不含中文）。
    std::printf("[main] %s\n", loop.renderer().fontNotice().c_str());
    if (!loop.renderer().cjkFontLoaded()) {
        std::fprintf(stderr,
                     "[main] 提示: 未加载到中文字体，窗口内中文将显示为方块。\n"
                     "       可设置环境变量 TACTICALAI_FONT 指向一个 .ttf/.otf/.ttc 中文字体。\n");
    }

    // 把中文字体注入原生 HUD。
    // 原生 HUD 的绘制路径不经过 Renderer（为保持 debug 层的独立性），
    // 因此必须显式把字体交给它，否则它只能用不含中文的默认字体，
    // 表现为「左上角信息面板的中文全是方块」。
    if (loop.renderer().cjkFontLoaded()) {
        TacticalAI::Debug::DebugUI::setFallbackFont(loop.renderer().defaultFont());
    }

    std::printf("[main] 进入主循环（关闭窗口退出）\n");

    // --- 真正的循环 ---
    // 由于 GameLoop 的 layer 引用不可更换，这里手动跑循环。
    // 与 GameLoop::run 逻辑一致，只是 layer 换成了 sandbox。
    int exitCode = 0;
    {
        using namespace TacticalAI::Engine;

        auto& renderer = loop.renderer();
        auto& input    = loop.input();
        auto& time     = loop.time();

        bool running = true;
        uint64_t frames = 0;

        while (running) {
            input.update();
            time.beginFrame();

            // 全局快捷键（暂停/单步/慢动作/全屏）
            if (input.consumeActionPressed(Action::TogglePause)) {
                time.togglePaused();
                std::printf("[main] %s\n", time.isPaused() ? "已暂停" : "已恢复");
            }
            if (time.isPaused() && input.consumeActionPressed(Action::StepFrame)) {
                time.requestSingleStep();
            }
            if (input.consumeActionPressed(Action::ToggleSlowMotion)) {
                const double next = (time.timeScale() < 1.0) ? 1.0 : 0.25;
                time.setTimeScale(next);
                std::printf("[main] 时间缩放 -> %.2fx\n", next);
            }
            if (input.consumeKeyPressed(KEY_F11)) {
                renderer.toggleFullscreen();
            }

            // 固定步长逻辑
            int stepsThisFrame = 0;
            while (time.consumeFixedStep()) {
                sandbox.onFixedUpdate(time.fixedDelta());
                ++stepsThisFrame;
            }

            // 【必须记一笔】把本帧的步数交给 GameLoop 的统计。
            // 这份主循环是手写的（GameLoop 的 layer 引用不可更换），如果不
            // 通知它，stats() 就永远是初始值 —— HUD 与调试面板上的
            // "逻辑步"会恒为 0，看起来像逻辑根本没跑。
            loop.recordFrame(stepsThisFrame);

            // 可变更新
            sandbox.onVariableUpdate(time.frame());

            // 渲染
            if (!renderer.beginFrame()) {
                running = false;
                break;
            }
            sandbox.onRender(time.interpolationAlpha());
            renderer.endFrame();

            time.endFrame();
            ++frames;

            if (options.maxFrames > 0 && frames >= static_cast<uint64_t>(options.maxFrames)) {
                std::printf("[main] 达到 --frames=%d 上限，正常退出\n", options.maxFrames);
                running = false;
            }

            // --shot 完成截图后由沙盒层请求退出（见其 onRender 里的说明）
            if (sandbox.exitRequested()) {
                std::printf("[main] 截图完成，退出\n");
                running = false;
            }
        }
    }

    // 先关闭调试 UI（imgui 必须在窗口销毁前 shutdown），
    // 再让沙盒层释放其资源。
    sandbox.onShutdown();

    std::printf("\n[main] 正常退出\n");
    return exitCode;
}
