// ===========================================================================
//  src/engine/Time.h
//
//  时间管理。职责：把“不均匀的帧间隔”整理成游戏逻辑可用的时间量。
//
//  设计要点：
//   - 逻辑帧用固定步长 (fixedDelta)，渲染帧用可变步长 (deltaTime)。
//     这是确定性 AI/物理仿真的前提——同一个种子 + 同一串输入必须得到同样结果。
//   - 帧率无关的平滑通过 Scale() 提供，供 UI 动画等非逻辑用途使用。
//   - 支持时间缩放（慢动作观察 AI 决策）和单帧步进。
// ===========================================================================
#pragma once

#include <cstdint>
#include <string>

namespace TacticalAI::Engine {

/// 引擎单帧的时间快照。以值语义传递，不持有资源。
struct FrameTime {
    /// 本渲染帧真实经过的时间（秒），未受 timeScale 影响。
    double deltaTime = 0.0;

    /// 逻辑帧固定步长（秒）。默认 1/60。
    double fixedDelta = 1.0 / 60.0;

    /// 程序启动至今的真实时间（秒）。不受 pause / timeScale 影响。
    double realTime = 0.0;

    /// 游戏世界时间（秒），受 timeScale 影响，累加 fixedDelta。
    double gameTime = 0.0;

    /// 当前帧号，从 0 开始，每个渲染帧自增。
    /// 命名为 frameIndex 而非 frameCount，避免与 Time::frameCount()
    /// 访问器在 Time 类内部产生「字段与方法同名」的编译冲突。
    uint64_t frameIndex = 0;

    /// 本渲染帧内执行的逻辑步进次数（0 表示本帧未推进逻辑，
    /// 例如渲染帧率高于固定步长时；上限见 Time::MaxStepsPerFrame）。
    int stepsThisFrame = 0;

    /// 当前帧率（每秒渲染帧数），由滑动平均得出。
    float fps = 0.0f;

    /// 本帧的固定步长相关量，便于逻辑层直接使用，避免重复乘法
    [[nodiscard]] float fixedDeltaF() const noexcept { return static_cast<float>(fixedDelta); }
    [[nodiscard]] float deltaTimeF()  const noexcept { return static_cast<float>(deltaTime);  }
};

/// 时间管理器。
///
/// 典型用法（在 GameLoop 内部）：
/// @code
///   Time time;
///   time.beginFrame();                       // 每渲染帧开头调用一次
///   while (time.consumeFixedStep()) { ... }  // 循环消费逻辑步
/// @endcode
class Time {
public:
    /// 单渲染帧内允许的最大逻辑步数。
    /// 防止「死亡螺旋」：若某一帧卡顿过久（如断点调试后），
    /// 累积时间会很大，无限补步会让下一帧更卡，形成正反馈崩溃。
    static constexpr int kMaxStepsPerFrame = 8;

    Time() = default;

    // -- 帧生命周期 ----------------------------------------------------------

    /// 开始新的一帧：采样真实时间、计算 deltaTime、累积待消费的固定步。
    /// @note 本函数不推进 gameTime，gameTime 在 consumeFixedStep 中推进。
    void beginFrame() noexcept;

    /// 尝试取出一个待执行的固定步长。
    /// @return true 表示还有逻辑步需要执行，调用方应立即执行一次逻辑更新。
    [[nodiscard]] bool consumeFixedStep() noexcept;

    /// 结束本帧：结算帧率统计。必须在 logic 循环之后调用。
    void endFrame() noexcept;

    // -- 状态查询 ------------------------------------------------------------

    /// 最近一帧的时间快照。
    [[nodiscard]] const FrameTime& frame() const noexcept { return frame_; }

    [[nodiscard]] double deltaTime()  const noexcept { return frame_.deltaTime;  }
    [[nodiscard]] double fixedDelta() const noexcept { return frame_.fixedDelta; }
    [[nodiscard]] double realTime()   const noexcept { return frame_.realTime;   }
    [[nodiscard]] double gameTime()   const noexcept { return frame_.gameTime;   }
    [[nodiscard]] float  fps()        const noexcept { return frame_.fps;        }
    [[nodiscard]] uint64_t frameCount() const noexcept { return frame_.frameIndex; }

    /// 本帧尚未消费的累积时间（秒）。可用于渲染插值：
    ///   alpha = time.accumulator() / time.fixedDelta()
    [[nodiscard]] double accumulator() const noexcept { return accumulator_; }

    /// 渲染插值系数 [0,1)。用它在两个逻辑状态之间插值，可消除
    /// 逻辑帧率与渲染帧率不一致导致的位置抖动。
    [[nodiscard]] float interpolationAlpha() const noexcept;

    [[nodiscard]] bool isPaused() const noexcept { return paused_; }
    [[nodiscard]] bool isStepping() const noexcept { return singleStepPending_; }

    // -- 控制 ----------------------------------------------------------------

    /// 修改固定步长（秒）。必须在 beginFrame 之外调用。
    void setFixedDelta(double seconds) noexcept;

    /// 暂停/恢复逻辑推进。暂停时 gameTime 与逻辑步均停止，
    /// 但 realTime、frameCount、fps 仍继续更新。
    void setPaused(bool paused) noexcept { paused_ = paused; }
    void togglePaused() noexcept { paused_ = !paused_; }

    /// 请求单帧步进：在暂停状态下按一次「下一步」，只推进一个固定步。
    void requestSingleStep() noexcept { singleStepPending_ = true; }

    /// 时间缩放。1.0 为正常，0.5 为慢动作，2.0 为二倍速。
    /// 仅影响逻辑步的产生速率，不影响 realTime。
    void setTimeScale(double scale) noexcept;
    [[nodiscard]] double timeScale() const noexcept { return timeScale_; }

    /// 限制单帧 deltaTime 上限（秒），默认 0.25。
    /// 防止窗口拖动、断点调试后出现巨大的时间跳跃。
    void setMaxDeltaTime(double seconds) noexcept { maxDeltaTime_ = seconds; }

    /// 复位所有计时状态（不改变配置项）。
    void reset() noexcept;

    /// 生成一行可读的时间状态，供调试 HUD 使用。
    [[nodiscard]] std::string debugString() const;

private:
    /// 采样单调递增的秒级时钟。
    /// Windows 上用 QueryPerformanceCounter 保证高精度，
    /// 其他平台用 std::chrono::steady_clock。
    [[nodiscard]] static double now() noexcept;

    FrameTime frame_{};

    double lastFrameTime_  = 0.0;   ///< 上一帧的绝对时间戳
    double accumulator_    = 0.0;   ///< 累积的、待消费的逻辑时间
    double timeScale_      = 1.0;
    double maxDeltaTime_   = 0.25;

    bool   paused_            = false;
    bool   singleStepPending_ = false;
    bool   started_           = false;  ///< 首帧需特殊处理（无上一帧时间戳）

    // FPS 滑动平均
    static constexpr int kFpsSampleCount = 30;
    double fpsSamples_[kFpsSampleCount] = {};
    int    fpsSampleIndex_ = 0;
    int    fpsSampleFilled_ = 0;
    double fpsSampleAccum_ = 0.0;
};

} // namespace TacticalAI::Engine
