// ===========================================================================
//  tests/test_time.cpp
//
//  时间系统测试。
//
//  这些测试全部走真实时钟（Time::now 内部使用 QPC / steady_clock），
//  因此会引入少量等待。为了让测试快速且稳定：
//    - 每次等待都很短（毫秒级）
//    - 断言用宽松的范围而非精确值
//    - 不对具体帧率做硬断言
// ===========================================================================
#include <gtest/gtest.h>

#include <chrono>
#include <thread>

#include "engine/Time.h"

using namespace TacticalAI::Engine;

namespace {

/// 忙等指定毫秒数。用 sleep_for 而非自旋，避免测试进程占满一个核心。
void waitMs(int milliseconds) {
    std::this_thread::sleep_for(std::chrono::milliseconds(milliseconds));
}

/// 跑若干帧，每帧之间等待指定毫秒。
void runFrames(Time& time, int frameCount, int msPerFrame) {
    for (int i = 0; i < frameCount; ++i) {
        time.beginFrame();
        while (time.consumeFixedStep()) {
            // 消费掉所有逻辑步，模拟真实循环
        }
        time.endFrame();
        waitMs(msPerFrame);
    }
}

} // namespace

// ===========================================================================
//  构造与配置
// ===========================================================================
TEST(TimeTest, DefaultFixedDeltaIs60Hz) {
    Time time;
    // 默认 1/60 秒，这是本项目约定的逻辑帧率
    EXPECT_NEAR(time.fixedDelta(), 1.0 / 60.0, 1e-9);
}

TEST(TimeTest, SetFixedDeltaIsClamped) {
    Time time;

    // 合法值
    time.setFixedDelta(1.0 / 120.0);
    EXPECT_NEAR(time.fixedDelta(), 1.0 / 120.0, 1e-9);

    // 非法值（0 或负数）应被忽略，保持原值。
    // 若允许 0，accumulator 将永远不会减少 -> 死循环。
    const double before = time.fixedDelta();
    time.setFixedDelta(0.0);
    EXPECT_DOUBLE_EQ(time.fixedDelta(), before);

    time.setFixedDelta(-1.0);
    EXPECT_DOUBLE_EQ(time.fixedDelta(), before);

    // 过大/过小的合法值应被钳制到有意义区间
    time.setFixedDelta(100.0);
    EXPECT_LE(time.fixedDelta(), 1.0);

    time.setFixedDelta(1e-9);
    EXPECT_GE(time.fixedDelta(), 0.001);
}

TEST(TimeTest, SetTimeScaleIsClamped) {
    Time time;

    time.setTimeScale(2.5);
    EXPECT_DOUBLE_EQ(time.timeScale(), 2.5);

    time.setTimeScale(0.0);
    EXPECT_DOUBLE_EQ(time.timeScale(), 0.0);   // 0 合法 = 完全冻结逻辑

    // 负数非法，应被忽略
    time.setTimeScale(-1.0);
    EXPECT_GE(time.timeScale(), 0.0);
}

// ===========================================================================
//  帧生命周期
// ===========================================================================
TEST(TimeTest, FirstFrameHasZeroDelta) {
    Time time;

    // 首帧没有上一帧参考点，deltaTime 必须是 0。
    // 否则会得到「从进程启动到窗口出现」的巨大时间差，
    // 导致第一次固定步循环爆出几百个逻辑步。
    time.beginFrame();

    EXPECT_DOUBLE_EQ(time.deltaTime(), 0.0);
    EXPECT_EQ(time.frameCount(), 0u);
}

TEST(TimeTest, DeltaTimeReflectsElapsedTime) {
    Time time;

    time.beginFrame();     // 首帧建立基准
    time.endFrame();

    waitMs(20);

    time.beginFrame();
    const double delta = time.deltaTime();

    // 允许较大误差：Windows 上 sleep 精度约 15ms，CI 负载也会影响
    EXPECT_GT(delta, 0.005) << "deltaTime 应反映真实经过的时间";
    EXPECT_LT(delta, 0.5) << "deltaTime 不应异常大";
}

TEST(TimeTest, FrameCountIncrementsOnEndFrame) {
    Time time;

    EXPECT_EQ(time.frameCount(), 0u);

    time.beginFrame();
    time.endFrame();
    EXPECT_EQ(time.frameCount(), 1u);

    time.beginFrame();
    time.endFrame();
    EXPECT_EQ(time.frameCount(), 2u);
}

TEST(TimeTest, RealTimeIncreasesMonotonically) {
    Time time;

    time.beginFrame();
    const double first = time.realTime();
    time.endFrame();

    waitMs(10);

    time.beginFrame();
    const double second = time.realTime();
    time.endFrame();

    EXPECT_GT(second, first);
}

TEST(TimeTest, MaxDeltaTimeClampsLargeGaps) {
    Time time;
    time.setMaxDeltaTime(0.05);   // 50ms 上限

    time.beginFrame();            // 建立基准
    time.endFrame();

    // 制造一个远超上限的间隔（模拟窗口拖动 / 断点调试）
    waitMs(120);

    time.beginFrame();
    // deltaTime 必须被钳制——否则会产生大量补步，形成死亡螺旋
    EXPECT_LE(time.deltaTime(), 0.05 + 1e-6);
}

// ===========================================================================
//  固定步长消费
// ===========================================================================
TEST(TimeTest, NoFixedStepsWhenNoTimeAccumulated) {
    Time time;

    time.beginFrame();   // 首帧 deltaTime = 0

    // 没有累积时间时不应产生逻辑步
    int steps = 0;
    while (time.consumeFixedStep()) {
        ++steps;
    }
    EXPECT_EQ(steps, 0);
}

TEST(TimeTest, AccumulatesFixedStepsOverTime) {
    Time time;

    time.beginFrame();   // 基准帧
    time.endFrame();

    waitMs(50);          // 约 3 个 16.67ms 的逻辑步

    time.beginFrame();

    int steps = 0;
    while (time.consumeFixedStep()) {
        ++steps;
    }

    // 至少 1 步（保守），最多不超过保护上限
    EXPECT_GE(steps, 1);
    EXPECT_LE(steps, Time::kMaxStepsPerFrame);
}

TEST(TimeTest, GameTimeAdvancesByFixedDeltaPerStep) {
    Time time;

    time.beginFrame();
    time.endFrame();

    waitMs(50);
    time.beginFrame();

    const double before = time.gameTime();
    int steps = 0;
    while (time.consumeFixedStep()) {
        ++steps;
    }

    // gameTime 应恰好增加 steps * fixedDelta——这保证逻辑时间的确定性
    EXPECT_NEAR(time.gameTime() - before, steps * time.fixedDelta(), 1e-9);
}

TEST(TimeTest, StepsThisFrameIsTracked) {
    Time time;

    time.beginFrame();
    time.endFrame();

    waitMs(40);
    time.beginFrame();

    int consumed = 0;
    while (time.consumeFixedStep()) {
        ++consumed;
    }

    EXPECT_EQ(time.frame().stepsThisFrame, consumed);
}

TEST(TimeTest, MaxStepsPerFramePreventsDeathSpiral) {
    Time time;
    const int maxSteps = Time::kMaxStepsPerFrame;

    time.beginFrame();
    time.endFrame();

    // 制造极长间隔（模拟断点后继续执行）
    waitMs(300);

    time.beginFrame();

    int steps = 0;
    while (time.consumeFixedStep()) {
        ++steps;
    }

    // 关键保护：无论累积了多少时间，单帧逻辑步数必须有上限。
    // 否则「卡顿 -> 补步 -> 更卡顿」的正反馈会让程序彻底失去响应。
    EXPECT_LE(steps, maxSteps);
}

// ===========================================================================
//  累积器与插值
// ===========================================================================
TEST(TimeTest, AccumulatorDecreasesWithSteps) {
    Time time;

    time.beginFrame();
    time.endFrame();

    waitMs(50);

    time.beginFrame();
    const double beforeAccumulator = time.accumulator();

    while (time.consumeFixedStep()) {
        // 消费掉
    }

    // 累积器应减少（被转换成逻辑步了）
    EXPECT_LT(time.accumulator(), beforeAccumulator);
    // 但不应变成负数——否则 consumeFixedStep 的判定逻辑有问题
    EXPECT_GE(time.accumulator(), 0.0);
}

TEST(TimeTest, InterpolationAlphaIsWithinRange) {
    Time time;

    runFrames(time, 5, 5);

    const float alpha = time.interpolationAlpha();
    // 插值系数必须在 [0,1)，用于在逻辑帧间平滑显示
    EXPECT_GE(alpha, 0.0f);
    EXPECT_LE(alpha, 1.0f);
}

TEST(TimeTest, InterpolationAlphaIsZeroBeforeAnyAccumulation) {
    Time time;

    time.beginFrame();
    // 首帧 deltaTime = 0，累积器为 0
    EXPECT_FLOAT_EQ(time.interpolationAlpha(), 0.0f);
}

// ===========================================================================
//  暂停与单步
// ===========================================================================
TEST(TimeTest, InitiallyNotPaused) {
    Time time;
    EXPECT_FALSE(time.isPaused());
    EXPECT_FALSE(time.isStepping());
}

TEST(TimeTest, PauseStopsGameTimeAdvancement) {
    Time time;

    time.beginFrame();
    time.endFrame();

    time.setPaused(true);
    EXPECT_TRUE(time.isPaused());

    const double gameTimeBefore = time.gameTime();

    waitMs(50);

    time.beginFrame();
    int steps = 0;
    while (time.consumeFixedStep()) {
        ++steps;
    }

    // 暂停时不应产生任何逻辑步，gameTime 冻结
    EXPECT_EQ(steps, 0);
    EXPECT_DOUBLE_EQ(time.gameTime(), gameTimeBefore);
}

TEST(TimeTest, PauseStillAdvancesRealTimeAndFrameCount) {
    Time time;

    time.beginFrame();
    time.endFrame();

    time.setPaused(true);

    const double realTimeBefore = time.realTime();
    const uint64_t frameCountBefore = time.frameCount();

    waitMs(20);
    time.beginFrame();
    time.endFrame();

    // realTime 与 frameCount 反映渲染循环的健康状况，不应受暂停影响
    EXPECT_GT(time.realTime(), realTimeBefore);
    EXPECT_EQ(time.frameCount(), frameCountBefore + 1);
}

TEST(TimeTest, SingleStepProducesExactlyOneLogicalStep) {
    Time time;

    time.beginFrame();
    time.endFrame();

    time.setPaused(true);
    time.requestSingleStep();
    EXPECT_TRUE(time.isStepping());

    time.beginFrame();

    int steps = 0;
    while (time.consumeFixedStep()) {
        ++steps;
    }

    // 单步必须恰好产生 1 个逻辑步——这是逐帧调试 AI 的基础
    EXPECT_EQ(steps, 1);
    EXPECT_FALSE(time.isStepping()) << "单步请求应被消费掉";
}

TEST(TimeTest, TogglePauseSwitchesState) {
    Time time;

    EXPECT_FALSE(time.isPaused());
    time.togglePaused();
    EXPECT_TRUE(time.isPaused());
    time.togglePaused();
    EXPECT_FALSE(time.isPaused());
}

// ===========================================================================
//  时间缩放
// ===========================================================================
TEST(TimeTest, SlowMotionReducesLogicalSteps) {
    // 慢动作应让相同真实时间内产生更少的逻辑步。
    // 对比两个 Time 实例，比断言绝对值更稳健。
    Time normalTime;
    Time slowTime;
    slowTime.setTimeScale(0.25);

    normalTime.beginFrame();
    normalTime.endFrame();
    slowTime.beginFrame();
    slowTime.endFrame();

    waitMs(60);

    normalTime.beginFrame();
    slowTime.beginFrame();

    int normalSteps = 0;
    while (normalTime.consumeFixedStep()) {
        ++normalSteps;
    }

    int slowSteps = 0;
    while (slowTime.consumeFixedStep()) {
        ++slowSteps;
    }

    // 1/4 速度下逻辑步应明显更少（允许调度抖动）
    EXPECT_LT(slowSteps, normalSteps)
        << "慢动作下逻辑步数应少于正常速度（正常=" << normalSteps
        << ", 慢动作=" << slowSteps << "）";
}

TEST(TimeTest, ZeroTimeScaleFreezesLogic) {
    Time time;
    time.setTimeScale(0.0);

    time.beginFrame();
    time.endFrame();

    waitMs(40);
    time.beginFrame();

    int steps = 0;
    while (time.consumeFixedStep()) {
        ++steps;
    }

    // timeScale = 0 等价于「冻结逻辑但不暂停渲染」
    EXPECT_EQ(steps, 0);
}

// ===========================================================================
//  帧率统计
// ===========================================================================
TEST(TimeTest, FpsIsPositiveAfterFrames) {
    Time time;

    runFrames(time, 8, 8);

    // 帧率应为正数。不硬断言具体值——CI 环境负载波动很大。
    EXPECT_GT(time.fps(), 0.0f);
}

TEST(TimeTest, FpsIsZeroBeforeAnyFrame) {
    Time time;
    EXPECT_FLOAT_EQ(time.fps(), 0.0f);
}

TEST(TimeTest, FpsIsSmoothedNotInstantaneous) {
    Time time;

    // 跑一批稳定帧
    runFrames(time, 12, 8);
    const float smoothedFps = time.fps();

    // 制造一个异常长的帧
    waitMs(100);
    time.beginFrame();
    time.endFrame();

    const float afterSpike = time.fps();

    // 滑动平均的好处：单次抖动不会让帧率读数暴跌。
    // 若用瞬时值，这里会掉到 ~10 FPS。
    EXPECT_GT(afterSpike, 0.0f);
    EXPECT_GT(smoothedFps, 0.0f);

    // 平滑后的值不应出现极端跳变（允许一定下降，但不该归零）
    EXPECT_GT(afterSpike, smoothedFps * 0.1f)
        << "帧率读数应被平滑，不应被单次抖动击穿";
}

// ===========================================================================
//  复位
// ===========================================================================
TEST(TimeTest, ResetClearsRuntimeStateButKeepsConfiguration) {
    Time time;

    time.setFixedDelta(1.0 / 30.0);
    time.setMaxDeltaTime(0.1);

    runFrames(time, 3, 5);

    EXPECT_GT(time.frameCount(), 0u);

    time.reset();

    // 运行时状态被清空
    EXPECT_EQ(time.frameCount(), 0u);
    EXPECT_DOUBLE_EQ(time.gameTime(), 0.0);
    EXPECT_FALSE(time.isPaused());
    EXPECT_FALSE(time.isStepping());

    // 但配置保留（reset 不该让调用方重新设一遍参数）
    EXPECT_NEAR(time.fixedDelta(), 1.0 / 30.0, 1e-9);
}

// ===========================================================================
//  调试输出
// ===========================================================================
TEST(TimeTest, DebugStringContainsKeyMetrics) {
    Time time;

    runFrames(time, 3, 5);

    const std::string text = time.debugString();
    EXPECT_FALSE(text.empty());
    EXPECT_NE(text.find("frame"), std::string::npos);
    EXPECT_NE(text.find("fps"), std::string::npos);
}

TEST(TimeTest, DebugStringShowsPausedState) {
    Time time;

    time.beginFrame();
    time.endFrame();
    time.setPaused(true);

    const std::string text = time.debugString();
    EXPECT_NE(text.find("PAUSED"), std::string::npos);
}

// ===========================================================================
//  确定性（最核心的保证）
// ===========================================================================
TEST(TimeTest, FixedDeltaIsConstantAcrossSteps) {
    Time time;

    time.beginFrame();
    time.endFrame();

    waitMs(80);   // 累积多个逻辑步
    time.beginFrame();

    // 每一步消耗的时间量必须完全相同。
    // 这是「同一输入序列 -> 同一结果」的前提，
    // 也是 AI 可复现调试的根基。
    std::vector<double> gameTimes;
    while (time.consumeFixedStep()) {
        gameTimes.push_back(time.gameTime());
    }

    ASSERT_GE(gameTimes.size(), 2u);

    const double expectedStep = time.fixedDelta();
    for (size_t i = 1; i < gameTimes.size(); ++i) {
        const double actualStep = gameTimes[i] - gameTimes[i - 1];
        EXPECT_NEAR(actualStep, expectedStep, 1e-9)
            << "第 " << i << " 步的时间增量偏离固定步长";
    }
}
