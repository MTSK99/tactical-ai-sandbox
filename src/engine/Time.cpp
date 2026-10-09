// ===========================================================================
//  src/engine/Time.cpp
// ===========================================================================
#include "engine/Time.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

#if defined(_WIN32)
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#else
#  include <chrono>
#endif

namespace TacticalAI::Engine {

// ---------------------------------------------------------------------------
// 高精度时钟
// ---------------------------------------------------------------------------
double Time::now() noexcept {
#if defined(_WIN32)
    // QueryPerformanceCounter 是 Windows 上精度最高的可用时钟（通常 ~100ns），
    // 且单调递增，适合测量帧间隔。
    static LARGE_INTEGER frequency = [] {
        LARGE_INTEGER f{};
        QueryPerformanceFrequency(&f);
        return f;
    }();
    static const double inverseFrequency =
        (frequency.QuadPart > 0) ? 1.0 / static_cast<double>(frequency.QuadPart) : 0.0;

    LARGE_INTEGER counter{};
    QueryPerformanceCounter(&counter);
    return static_cast<double>(counter.QuadPart) * inverseFrequency;
#else
    using clock = std::chrono::steady_clock;
    static const auto origin = clock::now();
    const auto elapsed = clock::now() - origin;
    return std::chrono::duration<double>(elapsed).count();
#endif
}

// ---------------------------------------------------------------------------
// 帧生命周期
// ---------------------------------------------------------------------------
void Time::beginFrame() noexcept {
    const double current = now();

    if (!started_) {
        // 首帧没有上一帧参考点，把 deltaTime 视为 0，
        // 否则会得到一个“从进程启动到窗口出现”的巨大时间差。
        lastFrameTime_ = current;
        started_ = true;
        frame_.deltaTime = 0.0;
    } else {
        double delta = current - lastFrameTime_;
        lastFrameTime_ = current;

        // 夹紧异常值：负数（时钟回拨）或超长卡顿
        if (!(delta > 0.0)) {
            delta = 0.0;
        }
        frame_.deltaTime = std::min(delta, maxDeltaTime_);
    }

    frame_.realTime = current;

    // 暂停时不累积逻辑时间；但单步请求要伪造一个小累积量，让下方循环能产生一步。
    if (paused_) {
        if (singleStepPending_) {
            accumulator_ += frame_.fixedDelta;
            singleStepPending_ = false;
        }
    } else {
        accumulator_ += frame_.deltaTime * timeScale_;
    }

    frame_.stepsThisFrame = 0;
}

bool Time::consumeFixedStep() noexcept {
    if (accumulator_ < frame_.fixedDelta) {
        return false;
    }

    // 超步保护：丢弃掉无法在预算内消化的时间，保持真实感但避免死亡螺旋。
    // 注意判定用的是「还能执行多少步」，而不是直接清空 accumulator，
    // 这样被丢弃的时间不会凭空变成逻辑时间。
    if (frame_.stepsThisFrame >= kMaxStepsPerFrame) {
        const double maxConsumable = frame_.fixedDelta * static_cast<double>(kMaxStepsPerFrame);
        accumulator_ = std::min(accumulator_, maxConsumable);
        return false;
    }

    accumulator_ -= frame_.fixedDelta;
    ++frame_.stepsThisFrame;
    frame_.gameTime += frame_.fixedDelta;
    return true;
}

void Time::endFrame() noexcept {
    // 帧率 = 最近 N 帧平均帧时长的倒数，比瞬时值稳定得多
    if (frame_.deltaTime > 0.0) {
        fpsSampleAccum_ -= fpsSamples_[fpsSampleIndex_];
        fpsSamples_[fpsSampleIndex_] = frame_.deltaTime;
        fpsSampleAccum_ += frame_.deltaTime;

        fpsSampleIndex_ = (fpsSampleIndex_ + 1) % kFpsSampleCount;
        if (fpsSampleFilled_ < kFpsSampleCount) {
            ++fpsSampleFilled_;
        }
    }

    if (fpsSampleFilled_ > 0 && fpsSampleAccum_ > 0.0) {
        const double average = fpsSampleAccum_ / static_cast<double>(fpsSampleFilled_);
        frame_.fps = static_cast<float>(1.0 / average);
    }

    ++frame_.frameIndex;
}

// ---------------------------------------------------------------------------
// 配置
// ---------------------------------------------------------------------------
void Time::setFixedDelta(double seconds) noexcept {
    // 下限 1ms / 上限 1s，避免调用方传入 0 或负数导致死循环（accumulator 永不减少）
    if (!(seconds > 0.0)) {
        return;
    }
    frame_.fixedDelta = std::clamp(seconds, 0.001, 1.0);
}

void Time::setTimeScale(double scale) noexcept {
    if (!(scale >= 0.0)) {
        return;
    }
    timeScale_ = std::clamp(scale, 0.0, 16.0);
}

float Time::interpolationAlpha() const noexcept {
    if (frame_.fixedDelta <= 0.0) {
        return 0.0f;
    }
    const double alpha = accumulator_ / frame_.fixedDelta;
    return static_cast<float>(std::clamp(alpha, 0.0, 1.0));
}

void Time::reset() noexcept {
    // fixedDelta 属于「配置」而非「运行时状态」，但它是存在 frame_ 里的。
    // 若直接整体重置 frame_，调用方之前 setFixedDelta(1/30) 设的值会被
    // 悄悄打回默认的 1/60。因此先保存再恢复，保证 reset 只清运行时。
    const double preservedFixedDelta = frame_.fixedDelta;

    frame_ = FrameTime{};
    frame_.fixedDelta = preservedFixedDelta;

    lastFrameTime_ = 0.0;
    accumulator_ = 0.0;
    started_ = false;
    paused_ = false;
    singleStepPending_ = false;

    for (double& sample : fpsSamples_) {
        sample = 0.0;
    }
    fpsSampleIndex_ = 0;
    fpsSampleFilled_ = 0;
    fpsSampleAccum_ = 0.0;
}

std::string Time::debugString() const {
    char buffer[192];
    std::snprintf(buffer, sizeof(buffer),
                  "frame=%llu fps=%.1f dt=%.2fms fixed=%.2fms steps=%d "
                  "game=%.2fs scale=%.2fx%s",
                  static_cast<unsigned long long>(frame_.frameIndex),
                  static_cast<double>(frame_.fps),
                  frame_.deltaTime * 1000.0,
                  frame_.fixedDelta * 1000.0,
                  frame_.stepsThisFrame,
                  frame_.gameTime,
                  timeScale_,
                  paused_ ? " [PAUSED]" : "");
    return std::string(buffer);
}

} // namespace TacticalAI::Engine
