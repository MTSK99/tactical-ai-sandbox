// ===========================================================================
//  src/engine/GameLoop.cpp
// ===========================================================================
#include "engine/GameLoop.h"

#include <algorithm>
#include <cstdio>
#include <memory>
#include <utility>

namespace TacticalAI::Engine {

namespace {
/// 慢动作档位。观察 AI 决策过程时，0.25x 通常最舒服。
constexpr double kSlowMotionScale = 0.25;
constexpr double kNormalSpeedScale = 1.0;
} // namespace

// ---------------------------------------------------------------------------
// LoopStats
// ---------------------------------------------------------------------------
std::string LoopStats::debugString() const {
    char buffer[256];
    std::snprintf(buffer, sizeof(buffer),
                  "frames=%llu steps=%llu real=%.1fs game=%.1fs\n"
                  "maxStepsPerFrame=%d starvation=%d overflow=%d",
                  static_cast<unsigned long long>(totalFrames),
                  static_cast<unsigned long long>(totalFixedSteps),
                  totalRealSeconds,
                  totalGameSeconds,
                  maxStepsInOneFrame,
                  stepStarvation,
                  stepOverflow);
    return std::string(buffer);
}

// ---------------------------------------------------------------------------
// 构造 / 析构
// ---------------------------------------------------------------------------
GameLoop::GameLoop(Config config, IGameLayer& layer)
    : config_(std::move(config)), layer_(layer) {}

GameLoop::~GameLoop() {
    // 析构顺序很重要：先让游戏层释放它持有的 GPU 资源，
    // 再销毁渲染器（CloseWindow）。反过来会导致资源在无上下文时释放而崩溃。
    //
    // 【修复】原先这里以 initialized_ 为条件。若游戏层在 onInitialize()
    // 内部已经分配了 GPU 资源之后才返回 false，initialized_ 仍是 false，
    // onShutdown() 就被跳过 —— 那些资源再也没有释放的机会。
    // 改为用独立的标志记录"是否调用过 onInitialize"。
    if (layerInitialized_) {
        layer_.onShutdown();
    }
}

// ---------------------------------------------------------------------------
// 初始化
// ---------------------------------------------------------------------------
bool GameLoop::initialize() {
    if (initialized_) {
        return true;
    }

    renderer_ = std::make_unique<Renderer>(config_.screenWidth, config_.screenHeight,
                                           config_.title);
    input_ = std::make_unique<Input>();
    time_  = std::make_unique<Time>();

    time_->setFixedDelta(config_.fixedDelta);
    renderer_->setTargetFPS(config_.targetFPS);
    renderer_->setWindowMinSize(640, 360);

    // 【修复】enableVsync 此前从未被读取：窗口创建时无条件开 VSync，
    // 命令行 --no-vsync 之类的选项完全无效且没有任何提示。
    renderer_->setVsync(config_.enableVsync);

    layerInitialized_ = true;
    if (!layer_.onInitialize()) {
        std::fprintf(stderr, "[GameLoop] 游戏层初始化失败，循环终止\n");
        // 让游戏层有机会清理「半初始化」期间已经拿到的资源
        layer_.onShutdown();
        layerInitialized_ = false;
        return false;
    }

    initialized_ = true;
    // 【修复】用 time_->fixedDelta() 而不是 config_.fixedDelta：
    // 后者是未经钳制的原始值，传 0 时会打印 "inf Hz"（实际跑的是 60Hz），
    // 排障时极具误导性。
    std::printf("[GameLoop] 启动完成 | 逻辑帧率 %.0f Hz | 窗口 %dx%d\n",
                1.0 / time_->fixedDelta(),
                config_.screenWidth,
                config_.screenHeight);
    return true;
}

// ---------------------------------------------------------------------------
// 主循环
// ---------------------------------------------------------------------------
void GameLoop::recordFrame(int stepsThisFrame) {
    if (stepsThisFrame == 0) {
        ++consecutiveZeroStepFrames_;
        // 连续大量帧没有推进逻辑，说明渲染帧率远高于逻辑帧率，
        // 或者逻辑被意外暂停。记录一次事件便于排查。
        if (consecutiveZeroStepFrames_ == 180) {
            stats_.stepStarvation = 180;
        }
    } else {
        consecutiveZeroStepFrames_ = 0;
    }

    if (time_ && stepsThisFrame > Time::kMaxStepsPerFrame - 1 &&
        time_->accumulator() >= time_->fixedDelta()) {
        ++stats_.stepOverflow;
    }

    stats_.maxStepsInOneFrame = std::max(stats_.maxStepsInOneFrame, stepsThisFrame);
    stats_.totalFixedSteps += static_cast<uint64_t>(stepsThisFrame);
    if (time_) {
        stats_.totalRealSeconds = time_->realTime();
        stats_.totalGameSeconds = time_->gameTime();
    }
    ++stats_.totalFrames;
}

int GameLoop::run() {
    if (!initialize()) {
        return 1;
    }

    while (!exitRequested_) {
        // --- 1. 采样输入（必须先于任何读取） ---
        input_->update();

        // --- 2. 时间推进：计算本帧 deltaTime 与待消费的逻辑步数 ---
        time_->beginFrame();

        // --- 3. 全局快捷键（暂停/单步/慢动作），在逻辑步之前处理 ---
        handleGlobalHotkeys();

        // --- 4. 固定步长逻辑循环 ---
        int stepsThisFrame = 0;
        while (time_->consumeFixedStep()) {
            stepFixed();
            ++stepsThisFrame;

            // 逻辑中途可能请求退出（例如玩家死亡），立即停止补步
            if (exitRequested_) {
                break;
            }
        }

        // --- 5. 统计与健康检查 ---
        recordFrame(stepsThisFrame);

        // --- 6. 可变更新（相机、UI 悬停等帧率相关逻辑） ---
        layer_.onVariableUpdate(time_->frame());

        // --- 7. 渲染 ---
        if (renderer_->beginFrame()) {
            layer_.onRender(time_->interpolationAlpha());
        } else {
            // 窗口关闭请求：结束本次循环
            exitRequested_ = true;
            break;
        }
        renderer_->endFrame();

        // --- 8. 时间收尾（结算帧率统计） ---
        time_->endFrame();

        // --- 9. 压测/无头模式：达到帧数上限后退出 ---
        if (config_.maxFrames > 0 &&
            stats_.totalFrames >= static_cast<uint64_t>(config_.maxFrames)) {
            std::printf("[GameLoop] 达到 --frames=%d 上限，正常退出\n", config_.maxFrames);
            exitRequested_ = true;
        }
    }

    std::printf("\n[GameLoop] 会话结束\n");
    std::printf("  渲染帧数      : %llu\n",
                static_cast<unsigned long long>(stats_.totalFrames));
    std::printf("  逻辑步数      : %llu\n",
                static_cast<unsigned long long>(stats_.totalFixedSteps));
    std::printf("  真实时长      : %.2f s\n", stats_.totalRealSeconds);
    std::printf("  游戏时长      : %.2f s\n", stats_.totalGameSeconds);
    std::printf("  单帧最多补步  : %d\n", stats_.maxStepsInOneFrame);

    return 0;
}

void GameLoop::stepFixed() {
    const double fixedDelta = time_->fixedDelta();

    // 逻辑层读取输入：此处使用 consume 语义，
    // 保证一次按键在多个逻辑步中只被处理一次。
    layer_.onFixedUpdate(fixedDelta);

    // idle 计时也按逻辑步推进，与逻辑帧率绑定
    input_->tickIdle(fixedDelta);
}

// ---------------------------------------------------------------------------
// 全局快捷键
// ---------------------------------------------------------------------------
void GameLoop::handleGlobalHotkeys() {
    // 暂停 / 恢复
    if (input_->consumeActionPressed(Action::TogglePause)) {
        time_->togglePaused();
        std::printf("[GameLoop] %s\n", time_->isPaused() ? "已暂停" : "已恢复");
    }

    // 暂停状态下单步
    if (time_->isPaused() && input_->consumeActionPressed(Action::StepFrame)) {
        time_->requestSingleStep();
    }

    // 慢动作切换
    if (input_->consumeActionPressed(Action::ToggleSlowMotion)) {
        const double next = (time_->timeScale() < kNormalSpeedScale)
                                ? kNormalSpeedScale
                                : kSlowMotionScale;
        time_->setTimeScale(next);
        std::printf("[GameLoop] 时间缩放 -> %.2fx\n", next);
    }

    // 全屏切换（沿用 raylib 约定，但玩家可能误触，故需要明确绑定）
    if (input_->consumeKeyPressed(KEY_F11)) {
        renderer_->toggleFullscreen();
    }
}

} // namespace TacticalAI::Engine
