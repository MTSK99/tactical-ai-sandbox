// ===========================================================================
//  src/engine/GameLoop.h
//
//  固定时间步游戏循环。
//
//  采用经典的「固定逻辑步 + 可变渲染 + 插值」结构：
//
//    ┌────────────────────────────────────────────────┐
//    │ while (running)                                │
//    │   input.update()          <- 采样输入           │
//    │   while (time.consume())  <- 逻辑步循环         │
//    │       fixedUpdate(dt)     <- 确定性仿真         │
//    │   render(alpha)           <- 用 alpha 插值显示   │
//    └────────────────────────────────────────────────┘
//
//  为什么 AI 项目必须这样做：
//   行为树、EQS 评分、寻路都依赖「每一步的时间间隔完全相等」。
//   若直接拿渲染帧的 deltaTime 去驱动，60fps 与 144fps 下敌方的
//   决策频率会不同，调试时无法复现问题。固定步长把仿真与帧率解耦，
//   使「同一初始状态 + 同一输入序列」必然得到同一结果。
//
//  GameLoop 本身不关心游戏内容：它只负责时序，通过 IGameLayer 回调外部。
// ===========================================================================
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

#include "engine/Input.h"
#include "engine/Renderer.h"
#include "engine/Time.h"

namespace TacticalAI::Engine {

/// 帧级统计信息，供调试面板显示性能曲线。
struct LoopStats {
    uint64_t totalFrames        = 0;   ///< 已渲染帧数
    uint64_t totalFixedSteps    = 0;   ///< 已执行逻辑步数
    double   totalRealSeconds   = 0.0;
    double   totalGameSeconds   = 0.0;
    int      maxStepsInOneFrame = 0;   ///< 单帧最多补了几步（诊断卡顿用）
    int      stepStarvation     = 0;   ///< 连续 0 步的帧数（逻辑被渲染饿死）
    int      stepOverflow       = 0;   ///< 触发超步保护的次数

    [[nodiscard]] std::string debugString() const;
};

/// 游戏层需要实现的接口。GameLoop 通过它驱动业务逻辑，
/// 单向依赖让引擎层无需知道任何游戏类型。
class IGameLayer {
public:
    virtual ~IGameLayer() = default;

    /// 初始化（窗口与渲染器已就绪）。返回 false 表示初始化失败，循环不启动。
    virtual bool onInitialize() { return true; }

    /// 固定步长逻辑更新。必须保证幂等与确定性，禁止在此读取鼠标位置等
    /// 与帧率相关的量（这类量应在 onVariableUpdate 中处理）。
    /// @param fixedDelta 固定步长（秒），每步恒定。
    virtual void onFixedUpdate(double fixedDelta) = 0;

    /// 每渲染帧调用一次。处理与视觉相关的、非确定性的更新
    /// （相机跟随、鼠标拾取、UI 悬停等）。
    /// @param frameTime 本帧时间快照。
    virtual void onVariableUpdate(const FrameTime& frameTime) = 0;

    /// 渲染。此时已处于屏幕空间，世界空间绘制需自行 beginWorld/endWorld。
    /// @param alpha 逻辑帧间的插值系数 [0,1)，用于平滑显示。
    virtual void onRender(float alpha) = 0;

    /// 释放资源（渲染器仍然有效，可在其中卸载 GPU 资源）。
    virtual void onShutdown() {}
};

/// 固定时间步循环驱动器。
class GameLoop {
public:
    /// 构造参数集中在一个结构体里，避免位置参数过多导致的调用错误。
    struct Config {
        int         screenWidth  = 1280;
        int         screenHeight = 720;
        std::string title        = "Tactical AI Sandbox";
        double      fixedDelta   = 1.0 / 60.0;  ///< 逻辑帧率 60Hz
        int         targetFPS    = 0;           ///< 0 表示不限制（依赖 VSync）
        int         maxFrames    = 0;           ///< >0 时跑够帧数后自动退出（无头/压测用）
        bool        enableVsync  = true;
    };

    /// @param layer 游戏层。所有权不转移，调用方需保证其生命周期长于 GameLoop。
    explicit GameLoop(Config config, IGameLayer& layer);
    ~GameLoop();

    GameLoop(const GameLoop&)            = delete;
    GameLoop& operator=(const GameLoop&) = delete;

    /// 初始化窗口与渲染器。失败时返回 false。
    [[nodiscard]] bool initialize();

    /// 进入主循环，直到窗口关闭或达到 maxFrames 后返回。
    /// 返回进程退出码（0 表示正常结束）。
    int run();

    /// 请求退出循环。可从逻辑回调中调用（如游戏中按退出键）。
    void requestExit() noexcept { exitRequested_ = true; }

    /// 记录一个渲染帧的执行结果，用于维护 stats()。
    ///
    /// 【为什么需要它】run() 内部会调用它；而 main 为了在运行期更换
    /// IGameLayer，自己写了一份等价的主循环（GameLoop 的 layer 引用不可更换）。
    /// 那份循环如果不调用这个方法，stats() 就永远停在初始值——
    /// HUD 上的"逻辑步"会恒为 0，看着像逻辑根本没跑。
    /// @param stepsThisFrame 本帧实际执行的固定步数
    void recordFrame(int stepsThisFrame);

    [[nodiscard]] bool isRunning() const noexcept { return !exitRequested_; }

    // -- 运行期可访问的子系统 ------------------------------------------------

    [[nodiscard]] Renderer& renderer() noexcept { return *renderer_; }
    [[nodiscard]] Input&    input()    noexcept { return *input_;    }
    [[nodiscard]] Time&     time()     noexcept { return *time_;     }

    [[nodiscard]] const LoopStats& stats() const noexcept { return stats_; }

private:
    /// 处理全局快捷键（暂停、单步、慢动作、全屏），在每帧逻辑之前执行。
    void handleGlobalHotkeys();

    /// 推进一个逻辑步：按固定步长调用游戏层。
    void stepFixed();

    Config      config_;
    IGameLayer& layer_;

    // 用 unique_ptr 延后到 initialize() 中构造：
    // Renderer 的构造会创建真实窗口，而单元测试不会调用 initialize()。
    std::unique_ptr<Renderer> renderer_;
    std::unique_ptr<Input>    input_;
    std::unique_ptr<Time>     time_;

    LoopStats stats_{};
    bool exitRequested_ = false;
    bool initialized_   = false;
    /// 是否调用过 layer_.onInitialize()。与 initialized_ 分开记录：
    /// 初始化失败时也要保证 onShutdown() 被调用一次。
    bool layerInitialized_ = false;
    int  consecutiveZeroStepFrames_ = 0;
};

} // namespace TacticalAI::Engine
