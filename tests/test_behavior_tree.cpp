// ===========================================================================
//  tests/test_behavior_tree.cpp
//
//  行为树测试。重点验证三件容易出错的事：
//    1. 组合节点的短路语义（Sequence / Selector）
//    2. Running 状态的跨步维持与中断清理
//    3. 装饰节点的边界行为
// ===========================================================================
#include <gtest/gtest.h>

#include <memory>
#include <vector>

#include "ai/BehaviorTree.h"
#include "ai/BehaviorTree.inl"

using namespace TacticalAI::AI;

namespace {

/// 测试用上下文。固定 gameTime 可控使其可预测。
struct TestFixture {
    Blackboard blackboard;
    double gameTime = 0.0;
    double fixedDelta = 1.0 / 60.0;
    uint64_t tickIndex = 0;

    TickContext makeContext() {
        return TickContext{blackboard, fixedDelta, gameTime, tickIndex++, false, "TestAgent"};
    }

    /// 推进一个逻辑步。
    void advance(double seconds = 1.0 / 60.0) {
        gameTime += seconds;
    }
};

/// 记录执行轨迹的节点，用于验证「哪些节点被调到、按什么顺序」。
class TraceNode final : public BehaviorNode {
public:
    TraceNode(std::string name, NodeStatus result, std::vector<std::string>* trace,
              int runningSteps = 0)
        : BehaviorNode(std::move(name)),
          result_(result),
          trace_(trace),
          remainingRunningSteps_(runningSteps) {}

    /// 进入/退出是否被记录（验证生命周期配对）。
    /// 默认关闭：绝大多数测试只关心「哪些节点执行了」，
    /// 打开后每个节点会多出 enter/exit 两条记录，干扰 trace.size() 断言。
    void setRecordLifecycle(bool value) noexcept { recordLifecycle_ = value; }

protected:
    void onEnter(TickContext& context) override {
        (void)context;
        if (recordLifecycle_ && trace_) {
            trace_->push_back(name_ + ":enter");
        }
        steps_ = 0;
    }

    NodeStatus onTick(TickContext& context) override {
        (void)context;
        if (trace_) {
            trace_->push_back(name_ + ":tick");
        }

        if (remainingRunningSteps_ > 0) {
            ++steps_;
            if (steps_ <= remainingRunningSteps_) {
                return NodeStatus::Running;
            }
        }
        return result_;
    }

    void onExit(TickContext& context, NodeStatus status) override {
        (void)context;
        if (recordLifecycle_ && trace_) {
            trace_->push_back(name_ + ":exit(" + toString(status) + ")");
        }
    }

    void onReset() override { steps_ = 0; }

private:
    NodeStatus  result_;
    std::vector<std::string>* trace_;
    int  remainingRunningSteps_ = 0;
    int  steps_ = 0;
    bool recordLifecycle_ = false;
};

} // namespace

// ===========================================================================
//  NodeStatus
// ===========================================================================
TEST(NodeStatusTest, ToStringCoversAllStates) {
    EXPECT_STREQ(toString(NodeStatus::Success), "Success");
    EXPECT_STREQ(toString(NodeStatus::Failure), "Failure");
    EXPECT_STREQ(toString(NodeStatus::Running), "Running");
    EXPECT_STREQ(toString(NodeStatus::Invalid), "Invalid");
}

// ===========================================================================
//  SequenceNode
// ===========================================================================
TEST(SequenceNodeTest, AllChildrenSucceedReturnsSuccess) {
    TestFixture fixture;
    std::vector<std::string> trace;

    auto sequence = std::make_unique<SequenceNode>("Seq");
    sequence->addChild(std::make_unique<TraceNode>("A", NodeStatus::Success, &trace));
    sequence->addChild(std::make_unique<TraceNode>("B", NodeStatus::Success, &trace));
    sequence->addChild(std::make_unique<TraceNode>("C", NodeStatus::Success, &trace));

    TickContext context = fixture.makeContext();
    EXPECT_EQ(sequence->tick(context), NodeStatus::Success);

    // 三个子节点都应在同一 tick 内被执行（Sequence 不消耗额外逻辑步）
    ASSERT_EQ(trace.size(), 3u);
    EXPECT_EQ(trace[0], "A:tick");
    EXPECT_EQ(trace[1], "B:tick");
    EXPECT_EQ(trace[2], "C:tick");
}

TEST(SequenceNodeTest, FailureShortCircuitsRemainingChildren) {
    TestFixture fixture;
    std::vector<std::string> trace;

    auto sequence = std::make_unique<SequenceNode>("Seq");
    sequence->addChild(std::make_unique<TraceNode>("A", NodeStatus::Success, &trace));
    sequence->addChild(std::make_unique<TraceNode>("B", NodeStatus::Failure, &trace));
    sequence->addChild(std::make_unique<TraceNode>("C", NodeStatus::Success, &trace));

    TickContext context = fixture.makeContext();
    EXPECT_EQ(sequence->tick(context), NodeStatus::Failure);

    // C 绝不应被执行——短路是 Sequence 的核心语义
    ASSERT_EQ(trace.size(), 2u);
    EXPECT_EQ(trace[0], "A:tick");
    EXPECT_EQ(trace[1], "B:tick");
}

TEST(SequenceNodeTest, RunningHaltsAndResumesFromSameChild) {
    TestFixture fixture;
    std::vector<std::string> trace;

    auto sequence = std::make_unique<SequenceNode>("Seq");
    // A 成功，B 需要 2 步才完成，C 成功
    sequence->addChild(std::make_unique<TraceNode>("A", NodeStatus::Success, &trace));
    sequence->addChild(std::make_unique<TraceNode>("B", NodeStatus::Success, &trace, 2));
    sequence->addChild(std::make_unique<TraceNode>("C", NodeStatus::Success, &trace));

    TickContext context1 = fixture.makeContext();
    fixture.advance();
    EXPECT_EQ(sequence->tick(context1), NodeStatus::Running);

    // 第一步：A 执行，B 开始（Running），C 未执行
    ASSERT_EQ(trace.size(), 2u);
    EXPECT_EQ(trace[0], "A:tick");
    EXPECT_EQ(trace[1], "B:tick");

    trace.clear();

    // 第二步：A 不应重新执行（Sequence 记住进度），继续 B
    TickContext context2 = fixture.makeContext();
    fixture.advance();
    EXPECT_EQ(sequence->tick(context2), NodeStatus::Running);
    ASSERT_EQ(trace.size(), 1u);
    EXPECT_EQ(trace[0], "B:tick");

    trace.clear();

    // 第三步：B 完成，推进到 C
    TickContext context3 = fixture.makeContext();
    fixture.advance();
    EXPECT_EQ(sequence->tick(context3), NodeStatus::Success);
    ASSERT_EQ(trace.size(), 2u);
    EXPECT_EQ(trace[0], "B:tick");
    EXPECT_EQ(trace[1], "C:tick");
}

TEST(SequenceNodeTest, FailureResetsProgress) {
    TestFixture fixture;
    std::vector<std::string> trace;

    auto sequence = std::make_unique<SequenceNode>("Seq");
    sequence->addChild(std::make_unique<TraceNode>("A", NodeStatus::Success, &trace));
    sequence->addChild(std::make_unique<TraceNode>("B", NodeStatus::Failure, &trace));

    TickContext context1 = fixture.makeContext();
    EXPECT_EQ(sequence->tick(context1), NodeStatus::Failure);

    trace.clear();

    // 第二次 tick 必须从 A 重新开始，而不是从 B。
    // 若不重置进度，前置条件会被跳过——这是行为树最常见的 bug 之一。
    TickContext context2 = fixture.makeContext();
    EXPECT_EQ(sequence->tick(context2), NodeStatus::Failure);
    ASSERT_GE(trace.size(), 1u);
    EXPECT_EQ(trace[0], "A:tick");
}

TEST(SequenceNodeTest, EmptySequenceReturnsSuccess) {
    TestFixture fixture;
    auto sequence = std::make_unique<SequenceNode>("Empty");

    TickContext context = fixture.makeContext();
    // 空 Sequence 的语义是「所有（零个）子节点都成功」= 真
    EXPECT_EQ(sequence->tick(context), NodeStatus::Success);
}

// ===========================================================================
//  SelectorNode
// ===========================================================================
TEST(SelectorNodeTest, ReturnsFirstSuccess) {
    TestFixture fixture;
    std::vector<std::string> trace;

    auto selector = std::make_unique<SelectorNode>("Sel");
    selector->addChild(std::make_unique<TraceNode>("A", NodeStatus::Failure, &trace));
    selector->addChild(std::make_unique<TraceNode>("B", NodeStatus::Success, &trace));
    selector->addChild(std::make_unique<TraceNode>("C", NodeStatus::Success, &trace));

    TickContext context = fixture.makeContext();
    EXPECT_EQ(selector->tick(context), NodeStatus::Success);

    // 找到 B 成功后就应停止，C（优先级更低）不执行
    ASSERT_EQ(trace.size(), 2u);
    EXPECT_EQ(trace[0], "A:tick");
    EXPECT_EQ(trace[1], "B:tick");
}

TEST(SelectorNodeTest, AllFailReturnsFailure) {
    TestFixture fixture;
    std::vector<std::string> trace;

    auto selector = std::make_unique<SelectorNode>("Sel");
    selector->addChild(std::make_unique<TraceNode>("A", NodeStatus::Failure, &trace));
    selector->addChild(std::make_unique<TraceNode>("B", NodeStatus::Failure, &trace));

    TickContext context = fixture.makeContext();
    EXPECT_EQ(selector->tick(context), NodeStatus::Failure);
    EXPECT_EQ(trace.size(), 2u);
}

TEST(SelectorNodeTest, RunningHaltsOnThatBranch) {
    TestFixture fixture;
    std::vector<std::string> trace;

    auto selector = std::make_unique<SelectorNode>("Sel");
    // A 失败，B 需要 2 步
    selector->addChild(std::make_unique<TraceNode>("A", NodeStatus::Failure, &trace));
    selector->addChild(std::make_unique<TraceNode>("B", NodeStatus::Success, &trace, 2));

    TickContext context1 = fixture.makeContext();
    EXPECT_EQ(selector->tick(context1), NodeStatus::Running);

    trace.clear();

    // 第二次仍停留在 B（非反应式模式），A 不应被重新评估。
    // 这保证了「一旦选定某个行为就执行完」——避免 AI 反复横跳。
    TickContext context2 = fixture.makeContext();
    EXPECT_EQ(selector->tick(context2), NodeStatus::Running);
    ASSERT_EQ(trace.size(), 1u);
    EXPECT_EQ(trace[0], "B:tick");
}

TEST(SelectorNodeTest, EmptySelectorReturnsFailure) {
    TestFixture fixture;
    auto selector = std::make_unique<SelectorNode>("Empty");

    TickContext context = fixture.makeContext();
    // 没有任何可用行为 -> 只能失败
    EXPECT_EQ(selector->tick(context), NodeStatus::Failure);
}

// ---------------------------------------------------------------------------
//  反应式选择器
//
//  背景（这是一个真实踩过的坑，因此必须有回归测试）：
//  默认选择器是「非反应式」的——一旦某分支进入 Running，它会把 currentIndex_
//  停在那里，后续 tick 不再重新评估更高优先级的分支。
//
//  对「撤退 > 交战 > 搜索 > 巡逻」这类根选择器，这会造成严重后果：
//  巡逻分支永远返回 Running，于是 AI 一旦进入巡逻，就再也不会回头检查
//  「我是不是看见敌人了」。表现为敌人从玩家面前走过却毫无反应。
//
//  setReactive(true) 是解决方案，而且它的实现曾经是缺失的（只设置了标志位，
//  onTick 从未读取）。下面两个测试分别锁定「非反应式」与「反应式」两种语义。
// ---------------------------------------------------------------------------
TEST(SelectorNodeTest, NonReactiveSelectorStaysOnRunningBranch) {
    TestFixture fixture;
    std::vector<std::string> trace;

    auto selector = std::make_unique<SelectorNode>("Sel");
    // 高优先级分支：第一次失败，之后若要重新评估本可成功
    selector->addChild(std::make_unique<TraceNode>("High", NodeStatus::Failure, &trace));
    // 低优先级分支：持续 Running，会把选择器「粘住」
    selector->addChild(std::make_unique<TraceNode>("Low", NodeStatus::Success, &trace, 99));

    TickContext c1 = fixture.makeContext();
    EXPECT_EQ(selector->tick(c1), NodeStatus::Running);
    // 第一步两个分支都被评估过
    ASSERT_EQ(trace.size(), 2u);

    trace.clear();
    TickContext c2 = fixture.makeContext();
    EXPECT_EQ(selector->tick(c2), NodeStatus::Running);

    // 非反应式：只重入 Low，High 不再被评估
    ASSERT_EQ(trace.size(), 1u);
    EXPECT_EQ(trace[0], "Low:tick");
}

TEST(SelectorNodeTest, ReactiveSelectorReevaluatesHigherPriorityEveryTick) {
    TestFixture fixture;
    std::vector<std::string> trace;

    auto selector = std::make_unique<SelectorNode>("Sel");
    selector->setReactive(true);

    selector->addChild(std::make_unique<TraceNode>("High", NodeStatus::Failure, &trace));
    selector->addChild(std::make_unique<TraceNode>("Low", NodeStatus::Success, &trace, 99));

    TickContext c1 = fixture.makeContext();
    EXPECT_EQ(selector->tick(c1), NodeStatus::Running);
    trace.clear();

    TickContext c2 = fixture.makeContext();
    EXPECT_EQ(selector->tick(c2), NodeStatus::Running);

    // 反应式：每一步都从 High 重新评估，High 失败后才落到 Low
    ASSERT_EQ(trace.size(), 2u);
    EXPECT_EQ(trace[0], "High:tick");
    EXPECT_EQ(trace[1], "Low:tick");
}

TEST(SelectorNodeTest, ReactiveSelectorSwitchesToHigherPriorityWhenItBecomesReady) {
    TestFixture fixture;
    std::vector<std::string> trace;

    auto selector = std::make_unique<SelectorNode>("Sel");
    selector->setReactive(true);

    // 用黑板开关模拟「外部世界变化」：敌人一开始没看见玩家，后来看见了。
    selector->addChild(std::make_unique<ConditionNode>(
        "SeesEnemy",
        [](const TickContext& context) {
            return context.blackboard.getOr<bool>("seesEnemy", false);
        },
        /*reEvaluateEveryTick=*/true));

    // 低优先级：持续 Running 的行为
    selector->addChild(std::make_unique<TraceNode>("Patrol", NodeStatus::Success, &trace, 999));

    // t0：没看见 -> 落到 Patrol
    TickContext c1 = fixture.makeContext();
    EXPECT_EQ(selector->tick(c1), NodeStatus::Running);

    // t1：仍然没看见 -> 继续 Patrol
    fixture.advance();
    TickContext c2 = fixture.makeContext();
    EXPECT_EQ(selector->tick(c2), NodeStatus::Running);

    // 外部世界变化：看见敌人了
    fixture.blackboard.set("seesEnemy", true);

    // t2：反应式选择器应立即抢占 Patrol，改由高优先级条件分支胜出
    fixture.advance();
    TickContext c3 = fixture.makeContext();
    EXPECT_EQ(selector->tick(c3), NodeStatus::Success);
}

// ---------------------------------------------------------------------------
//  反应式选择器 + 装饰循环的真实场景
//
//  这里复现「根选择器 + 巡逻分支永远 Running + 交战条件后来变为真」的完整
//  结构，确保 AI 能从巡逻中被正确抢占。这是无头冒烟测试暴露的核心问题。
// ---------------------------------------------------------------------------
TEST(SelectorNodeTest, ReactiveRootPreemptsPatrolWhenEnemyAppears) {
    TestFixture fixture;

    auto root = std::make_unique<SelectorNode>("Root");
    root->setReactive(true);

    // 高优先级：交战条件（外部世界，每步重新求值）
    root->addChild(std::make_unique<ConditionNode>(
        "HasEnemy",
        [](const TickContext& context) {
            return context.blackboard.getOr<bool>("hasEnemy", false);
        },
        /*reEvaluateEveryTick=*/true));

    // 低优先级：巡逻（永远 Running）
    root->addChild(std::make_unique<TraceNode>("Patrol", NodeStatus::Success, nullptr, 999));

    for (int i = 0; i < 10; ++i) {
        fixture.advance();
        TickContext context = fixture.makeContext();
        EXPECT_EQ(root->tick(context), NodeStatus::Running) << "step " << i;
    }

    fixture.blackboard.set("hasEnemy", true);
    fixture.advance();
    TickContext context = fixture.makeContext();
    EXPECT_EQ(root->tick(context), NodeStatus::Success);

    // 取消条件后应该能回到巡逻
    fixture.blackboard.set("hasEnemy", false);
    fixture.advance();
    TickContext context2 = fixture.makeContext();
    EXPECT_EQ(root->tick(context2), NodeStatus::Running);
}

// ---------------------------------------------------------------------------
//  反应式选择器构建器：reactiveSel()
// ---------------------------------------------------------------------------
TEST(BehaviorTreeBuilderTest, ReactiveSelBuildsReevaluatingSelector) {
    using namespace bt;
    TestFixture fixture;

    auto root = reactiveSel("Root",
        std::make_unique<ConditionNode>(
            "HasEnemy",
            [](const TickContext& context) {
                return context.blackboard.getOr<bool>("hasEnemy", false);
            },
            /*reEvaluateEveryTick=*/true),
        always("Patrol", NodeStatus::Running));

    TickContext c1 = fixture.makeContext();
    EXPECT_EQ(root->tick(c1), NodeStatus::Running);

    fixture.blackboard.set("hasEnemy", true);
    TickContext c2 = fixture.makeContext();
    EXPECT_EQ(root->tick(c2), NodeStatus::Success);
}

// ===========================================================================
//  ParallelNode
// ===========================================================================
TEST(ParallelNodeTest, RequireAllSuccessWaitsForEveryChild) {
    TestFixture fixture;
    std::vector<std::string> trace;

    auto parallel = std::make_unique<ParallelNode>(
        "Par", ParallelNode::SuccessPolicy::RequireAll,
        ParallelNode::FailurePolicy::RequireAll);

    parallel->addChild(std::make_unique<TraceNode>("A", NodeStatus::Success, &trace, 1));
    parallel->addChild(std::make_unique<TraceNode>("B", NodeStatus::Success, &trace, 2));

    // 第一步：A 尚未成功，B 也在 Running
    TickContext context1 = fixture.makeContext();
    EXPECT_EQ(parallel->tick(context1), NodeStatus::Running);

    // 第二步：A 成功，B 仍 Running
    TickContext context2 = fixture.makeContext();
    EXPECT_EQ(parallel->tick(context2), NodeStatus::Running);

    // 第三步：都成功
    TickContext context3 = fixture.makeContext();
    EXPECT_EQ(parallel->tick(context3), NodeStatus::Success);
}

TEST(ParallelNodeTest, RequireOneSuccessCompletesEarly) {
    TestFixture fixture;
    std::vector<std::string> trace;

    auto parallel = std::make_unique<ParallelNode>(
        "Par", ParallelNode::SuccessPolicy::RequireOne,
        ParallelNode::FailurePolicy::RequireAll);

    parallel->addChild(std::make_unique<TraceNode>("A", NodeStatus::Success, &trace));
    parallel->addChild(std::make_unique<TraceNode>("B", NodeStatus::Success, &trace, 5));

    // A 立刻成功 -> RequireOne 满足，整体成功
    TickContext context = fixture.makeContext();
    EXPECT_EQ(parallel->tick(context), NodeStatus::Success);
}

TEST(ParallelNodeTest, RequireOneFailureFailsImmediately) {
    TestFixture fixture;
    std::vector<std::string> trace;

    auto parallel = std::make_unique<ParallelNode>(
        "Par", ParallelNode::SuccessPolicy::RequireAll,
        ParallelNode::FailurePolicy::RequireOne);

    parallel->addChild(std::make_unique<TraceNode>("A", NodeStatus::Failure, &trace));
    parallel->addChild(std::make_unique<TraceNode>("B", NodeStatus::Success, &trace, 5));

    // 任一失败即整体失败（硬约束语义）
    TickContext context = fixture.makeContext();
    EXPECT_EQ(parallel->tick(context), NodeStatus::Failure);
}

TEST(ParallelNodeTest, FinishedChildrenAreNotReticked) {
    TestFixture fixture;
    std::vector<std::string> trace;

    auto parallel = std::make_unique<ParallelNode>(
        "Par", ParallelNode::SuccessPolicy::RequireAll,
        ParallelNode::FailurePolicy::RequireAll);

    parallel->addChild(std::make_unique<TraceNode>("A", NodeStatus::Success, &trace));
    parallel->addChild(std::make_unique<TraceNode>("B", NodeStatus::Success, &trace, 2));

    TickContext context1 = fixture.makeContext();
    parallel->tick(context1);
    fixture.advance();

    trace.clear();

    TickContext context2 = fixture.makeContext();
    parallel->tick(context2);

    // A 已成功，不应被再次 tick。
    // 这很重要：重复 tick 会让「射击」这类有副作用的动作重复消耗子弹。
    for (const std::string& entry : trace) {
        EXPECT_NE(entry, "A:tick") << "已完成的子节点不应被重复 tick";
    }
}

// ===========================================================================
//  装饰节点
// ===========================================================================
TEST(InverterNodeTest, InvertsSuccessAndFailure) {
    TestFixture fixture;

    auto invertSuccess = std::make_unique<InverterNode>(
        "Inv", std::make_unique<TraceNode>("Child", NodeStatus::Success, nullptr));

    TickContext context1 = fixture.makeContext();
    EXPECT_EQ(invertSuccess->tick(context1), NodeStatus::Failure);

    auto invertFailure = std::make_unique<InverterNode>(
        "Inv", std::make_unique<TraceNode>("Child", NodeStatus::Failure, nullptr));

    TickContext context2 = fixture.makeContext();
    EXPECT_EQ(invertFailure->tick(context2), NodeStatus::Success);
}

TEST(InverterNodeTest, PassesThroughRunning) {
    TestFixture fixture;

    // Running 必须透传：否则「取反一个正在执行的行为」会立刻得到假结果，
    // 导致持续行为被打断。
    auto inverter = std::make_unique<InverterNode>(
        "Inv", std::make_unique<TraceNode>("Child", NodeStatus::Success, nullptr, 3));

    TickContext context = fixture.makeContext();
    EXPECT_EQ(inverter->tick(context), NodeStatus::Running);
}

TEST(SucceederNodeTest, AlwaysReturnsSuccess) {
    TestFixture fixture;

    auto succeeder = std::make_unique<SucceederNode>(
        "Succ", std::make_unique<TraceNode>("Child", NodeStatus::Failure, nullptr));

    TickContext context = fixture.makeContext();
    // 即使子节点失败，也报告成功——让「可选步骤」不打断 Sequence
    EXPECT_EQ(succeeder->tick(context), NodeStatus::Success);
}

TEST(RepeaterNodeTest, RepeatsSpecifiedCount) {
    TestFixture fixture;
    std::vector<std::string> trace;

    auto repeater = std::make_unique<RepeaterNode>(
        "Rep", std::make_unique<TraceNode>("Child", NodeStatus::Success, &trace), 3);

    // 前两次返回 Running（重复未完成）
    TickContext context1 = fixture.makeContext();
    EXPECT_EQ(repeater->tick(context1), NodeStatus::Running);

    TickContext context2 = fixture.makeContext();
    EXPECT_EQ(repeater->tick(context2), NodeStatus::Running);

    // 第三次完成
    TickContext context3 = fixture.makeContext();
    EXPECT_EQ(repeater->tick(context3), NodeStatus::Success);

    // 子节点被执行 3 次
    EXPECT_EQ(trace.size(), 3u);
}

TEST(CooldownNodeTest, BlocksDuringCooldown) {
    TestFixture fixture;
    std::vector<std::string> trace;

    auto cooldown = std::make_unique<CooldownNode>(
        "Cd", std::make_unique<TraceNode>("Child", NodeStatus::Success, &trace), 1.0);

    // 第一次：冷却已过（readyAtTime = 0），执行并通过
    TickContext context1 = fixture.makeContext();
    EXPECT_EQ(cooldown->tick(context1), NodeStatus::Success);
    EXPECT_EQ(trace.size(), 1u);

    // 冷却中：直接失败，不执行子节点
    fixture.advance(0.5);
    TickContext context2 = fixture.makeContext();
    EXPECT_EQ(cooldown->tick(context2), NodeStatus::Failure);
    EXPECT_EQ(trace.size(), 1u) << "冷却期间不应执行子节点";

    // 冷却结束：可以再次执行
    fixture.advance(0.6);
    TickContext context3 = fixture.makeContext();
    EXPECT_EQ(cooldown->tick(context3), NodeStatus::Success);
    EXPECT_EQ(trace.size(), 2u);
}

TEST(CooldownNodeTest, DoesNotStartCooldownOnFailure) {
    TestFixture fixture;
    std::vector<std::string> trace;

    auto cooldown = std::make_unique<CooldownNode>(
        "Cd", std::make_unique<TraceNode>("Child", NodeStatus::Failure, &trace), 5.0);

    // 失败不触发冷却——否则「尝试失败」会被惩罚，AI 会失去重试能力
    TickContext context1 = fixture.makeContext();
    EXPECT_EQ(cooldown->tick(context1), NodeStatus::Failure);

    fixture.advance(0.1);
    TickContext context2 = fixture.makeContext();
    EXPECT_EQ(cooldown->tick(context2), NodeStatus::Failure);
    EXPECT_EQ(trace.size(), 2u) << "失败后应立即可以重试";
}

// ===========================================================================
//  WaitNode / ConditionNode / ActionNode
// ===========================================================================
TEST(WaitNodeTest, RunsUntilDurationElapsed) {
    TestFixture fixture;
    auto wait = std::make_unique<WaitNode>("Wait", 0.1);   // 100ms

    // 前 5 步（约 83ms）仍 Running
    int runningCount = 0;
    for (int i = 0; i < 5; ++i) {
        TickContext context = fixture.makeContext();
        if (wait->tick(context) == NodeStatus::Running) {
            ++runningCount;
        }
        fixture.advance(1.0 / 60.0);
    }
    EXPECT_EQ(runningCount, 5);

    // 继续到超过 100ms 后应成功
    NodeStatus finalStatus = NodeStatus::Running;
    for (int i = 0; i < 5; ++i) {
        TickContext context = fixture.makeContext();
        finalStatus = wait->tick(context);
        fixture.advance(1.0 / 60.0);
        if (finalStatus == NodeStatus::Success) {
            break;
        }
    }
    EXPECT_EQ(finalStatus, NodeStatus::Success);
}

TEST(ConditionNodeTest, CachesResultByDefault) {
    TestFixture fixture;
    int evaluationCount = 0;

    auto condition = std::make_unique<ConditionNode>(
        "Cond",
        [&evaluationCount](const TickContext&) {
            ++evaluationCount;
            return true;
        },
        /*reEvaluateEveryTick=*/false);

    TickContext context1 = fixture.makeContext();
    EXPECT_EQ(condition->tick(context1), NodeStatus::Success);

    TickContext context2 = fixture.makeContext();
    EXPECT_EQ(condition->tick(context2), NodeStatus::Success);

    // 缓存模式：只求值一次。
    // 这使「生命值 > 30%」这类条件在整个分支执行期间保持稳定，
    // 避免行为在边界值附近反复切换。
    EXPECT_EQ(evaluationCount, 1);
}

TEST(ConditionNodeTest, CanReEvaluateEveryTick) {
    TestFixture fixture;
    int evaluationCount = 0;

    auto condition = std::make_unique<ConditionNode>(
        "Cond",
        [&evaluationCount](const TickContext&) {
            ++evaluationCount;
            return true;
        },
        /*reEvaluateEveryTick=*/true);

    TickContext context1 = fixture.makeContext();
    condition->tick(context1);
    TickContext context2 = fixture.makeContext();
    condition->tick(context2);

    EXPECT_EQ(evaluationCount, 2);
}

TEST(ConditionNodeTest, FalseConditionReturnsFailure) {
    TestFixture fixture;

    auto condition = std::make_unique<ConditionNode>(
        "Cond", [](const TickContext&) { return false; });

    TickContext context = fixture.makeContext();
    EXPECT_EQ(condition->tick(context), NodeStatus::Failure);
}

TEST(ActionNodeTest, InvokesEnterAndExitCallbacks) {
    TestFixture fixture;
    int enterCount = 0;
    int exitCount = 0;

    auto action = std::make_unique<ActionNode>("Act",
        [](TickContext&) { return NodeStatus::Success; });

    action->onEnterCallback([&enterCount](TickContext&) { ++enterCount; });
    action->onExitCallback([&exitCount](TickContext&) { ++exitCount; });

    TickContext context = fixture.makeContext();
    EXPECT_EQ(action->tick(context), NodeStatus::Success);

    EXPECT_EQ(enterCount, 1);
    EXPECT_EQ(exitCount, 1);
}

TEST(ActionNodeTest, ExitCalledOnRunningToFinishedTransition) {
    TestFixture fixture;
    int exitCount = 0;

    auto action = std::make_unique<ActionNode>("Act",
        [](TickContext&) { return NodeStatus::Success; });

    action->onExitCallback([&exitCount](TickContext&) { ++exitCount; });

    // 首次 tick：进入并立即结束 -> 一次 exit
    TickContext context1 = fixture.makeContext();
    action->tick(context1);
    EXPECT_EQ(exitCount, 1);

    // 第二次 tick：重新进入，再次结束 -> 又一次 exit
    TickContext context2 = fixture.makeContext();
    action->tick(context2);
    EXPECT_EQ(exitCount, 2);
}

// ===========================================================================
//  abort —— 中断清理
// ===========================================================================
TEST(BehaviorNodeTest, AbortTriggersExitOnRunningNode) {
    TestFixture fixture;
    std::vector<std::string> trace;

    TraceNode node("Node", NodeStatus::Success, &trace, /*runningSteps=*/10);
    node.setRecordLifecycle(true);   // 本用例专门验证 enter/exit 配对

    TickContext context1 = fixture.makeContext();
    EXPECT_EQ(node.tick(context1), NodeStatus::Running);

    // 中断必须触发 onExit，否则 Running 分支的临时状态不会被清理
    node.abort(context1);

    bool hasExit = false;
    for (const std::string& entry : trace) {
        if (entry.find(":exit") != std::string::npos) {
            hasExit = true;
        }
    }
    EXPECT_TRUE(hasExit) << "中断 Running 节点时必须调用 onExit 做清理";
}

TEST(SequenceNodeTest, AbortPropagatesToRunningChild) {
    TestFixture fixture;
    std::vector<std::string> trace;

    auto sequence = std::make_unique<SequenceNode>("Seq");
    auto childA = std::make_unique<TraceNode>("A", NodeStatus::Success, &trace);
    auto childB = std::make_unique<TraceNode>("B", NodeStatus::Success, &trace, 10);
    childA->setRecordLifecycle(true);
    childB->setRecordLifecycle(true);   // 本用例验证中断是否触发 onExit 清理
    sequence->addChild(std::move(childA));
    sequence->addChild(std::move(childB));

    TickContext context1 = fixture.makeContext();
    EXPECT_EQ(sequence->tick(context1), NodeStatus::Running);

    trace.clear();
    sequence->abort(context1);

    // 中断应传递到正在 Running 的子节点 B
    bool bExited = false;
    for (const std::string& entry : trace) {
        if (entry.find("B:exit") != std::string::npos) {
            bExited = true;
        }
    }
    EXPECT_TRUE(bExited) << "中断必须递归传递到 Running 子节点";
}

// ===========================================================================
//  BehaviorTree
// ===========================================================================
TEST(BehaviorTreeTest, TickWithNoRootReturnsInvalid) {
    TestFixture fixture;
    BehaviorTree tree("Empty");

    TickContext context = fixture.makeContext();
    EXPECT_EQ(tree.tick(context), NodeStatus::Invalid);
}

TEST(BehaviorTreeTest, TickDelegatesToRoot) {
    TestFixture fixture;

    BehaviorTree tree("Test");
    tree.setRoot(std::make_unique<TraceNode>("Root", NodeStatus::Success, nullptr));

    TickContext context = fixture.makeContext();
    EXPECT_EQ(tree.tick(context), NodeStatus::Success);
    EXPECT_EQ(tree.tickCount(), 1u);
    EXPECT_EQ(tree.lastStatus(), NodeStatus::Success);
}

TEST(BehaviorTreeTest, ResetClearsStatusButKeepsTickCount) {
    TestFixture fixture;

    BehaviorTree tree("Test");
    tree.setRoot(std::make_unique<TraceNode>("Root", NodeStatus::Success, nullptr));

    TickContext context = fixture.makeContext();
    tree.tick(context);
    EXPECT_EQ(tree.lastStatus(), NodeStatus::Success);

    tree.reset();
    EXPECT_EQ(tree.lastStatus(), NodeStatus::Invalid);
    // tickCount 是累计统计，跨重置保留
    EXPECT_EQ(tree.tickCount(), 1u);
}

TEST(BehaviorTreeTest, NodeCountTraversesFullTree) {
    using namespace bt;

    BehaviorTree tree("Count");
    tree.setRoot(seq("Root",
        always("A", NodeStatus::Success),
        sel("B",
            always("B1", NodeStatus::Failure),
            always("B2", NodeStatus::Success)),
        wait("C", 1.0)
    ));

    // Root + A + B + B1 + B2 + C = 6
    EXPECT_EQ(tree.nodeCount(), 6u);
}

TEST(BehaviorTreeTest, DescribeIncludesNodeNames) {
    using namespace bt;

    BehaviorTree tree("DescribeTest");
    tree.setRoot(sel("RootSelector",
        always("FirstOption", NodeStatus::Failure),
        always("SecondOption", NodeStatus::Success)));

    TestFixture fixture;
    TickContext context = fixture.makeContext();
    tree.tick(context);

    const std::string description = tree.describe();
    EXPECT_NE(description.find("DescribeTest"), std::string::npos);
    EXPECT_NE(description.find("RootSelector"), std::string::npos);
    EXPECT_NE(description.find("FirstOption"), std::string::npos);
    // 描述里应包含执行结果，方便在日志里分析
    EXPECT_NE(description.find("Failure"), std::string::npos);
    EXPECT_NE(description.find("Success"), std::string::npos);
}

// ===========================================================================
//  建造器（bt::seq / bt::sel / bt::par）
// ===========================================================================
TEST(BehaviorTreeBuilderTest, SeqBuildsWorkingSequence) {
    using namespace bt;
    TestFixture fixture;

    auto root = seq("Root",
        always("A", NodeStatus::Success),
        always("B", NodeStatus::Success));

    TickContext context = fixture.makeContext();
    EXPECT_EQ(root->tick(context), NodeStatus::Success);
}

TEST(BehaviorTreeBuilderTest, SelShortCircuitsOnFirstSuccess) {
    using namespace bt;
    TestFixture fixture;
    std::vector<std::string> trace;

    auto root = sel("Root",
        std::make_unique<TraceNode>("First", NodeStatus::Failure, &trace),
        std::make_unique<TraceNode>("Second", NodeStatus::Success, &trace),
        std::make_unique<TraceNode>("Third", NodeStatus::Success, &trace));

    TickContext context = fixture.makeContext();
    EXPECT_EQ(root->tick(context), NodeStatus::Success);

    // Third 不应执行
    ASSERT_EQ(trace.size(), 2u);
}

TEST(BehaviorTreeBuilderTest, NestedBuildersComposeCorrectly) {
    using namespace bt;
    TestFixture fixture;

    // 构造一个真实场景的形状：
    // 选择器 -> [ 低血撤退, 有目标则交战, 否则待命 ]
    auto root = sel("Root",
        seq("Retreat",
            always("LowHealth", NodeStatus::Failure),
            always("Flee", NodeStatus::Success)),
        seq("Engage",
            always("HasTarget", NodeStatus::Success),
            always("Shoot", NodeStatus::Success)),
        always("Idle", NodeStatus::Success));

    TickContext context = fixture.makeContext();
    // Retreat 分支因 LowHealth 失败而被跳过，Engage 分支应成功
    EXPECT_EQ(root->tick(context), NodeStatus::Success);
}

TEST(BehaviorTreeBuilderTest, InverterWrapsBuilderOutput) {
    using namespace bt;
    TestFixture fixture;

    auto inverted = invert("NotAlways", always("Always", NodeStatus::Success));

    TickContext context = fixture.makeContext();
    EXPECT_EQ(inverted->tick(context), NodeStatus::Failure);
}

TEST(BehaviorTreeBuilderTest, CooldownWrapsBuilderOutput) {
    using namespace bt;
    TestFixture fixture;

    auto root = cooldown("Cd", always("Act", NodeStatus::Success), 1.0);

    TickContext context1 = fixture.makeContext();
    EXPECT_EQ(root->tick(context1), NodeStatus::Success);

    fixture.advance(0.3);
    TickContext context2 = fixture.makeContext();
    EXPECT_EQ(root->tick(context2), NodeStatus::Failure);

    fixture.advance(0.8);
    TickContext context3 = fixture.makeContext();
    EXPECT_EQ(root->tick(context3), NodeStatus::Success);
}

// ===========================================================================
//  黑板集成
// ===========================================================================
TEST(BehaviorTreeBlackboardTest, ActionCanReadAndWriteBlackboard) {
    using namespace bt;
    TestFixture fixture;
    fixture.blackboard.set("input", 10);
    fixture.blackboard.set("multiplier", 3);

    auto action = std::make_unique<ActionNode>("Multiply",
        [](TickContext& context) -> NodeStatus {
            const int input = context.blackboard.getOr<int>("input", 0);
            const int multiplier = context.blackboard.getOr<int>("multiplier", 1);
            context.blackboard.set("result", input * multiplier, "Multiply");
            return NodeStatus::Success;
        });

    TickContext context = fixture.makeContext();
    EXPECT_EQ(action->tick(context), NodeStatus::Success);
    EXPECT_EQ(fixture.blackboard.get<int>("result"), 30);
}

TEST(BehaviorTreeBlackboardTest, ConditionDrivesBranchSelection) {
    using namespace bt;
    TestFixture fixture;

    auto root = sel("Root",
        seq("HighThreatBranch",
            std::make_unique<ConditionNode>("IsHighThreat",
                [](const TickContext& context) {
                    return context.blackboard.getOr<float>("threat", 0.0f) > 0.7f;
                }),
            always("Retreat", NodeStatus::Success)),
        always("Idle", NodeStatus::Success));

    // 低威胁 -> 走 Idle 分支
    fixture.blackboard.set("threat", 0.2f);
    TickContext context1 = fixture.makeContext();
    EXPECT_EQ(root->tick(context1), NodeStatus::Success);

    // 高威胁 -> 走 Retreat 分支（结果同为 Success，但路径不同）
    fixture.blackboard.set("threat", 0.9f);
    TickContext context2 = fixture.makeContext();
    EXPECT_EQ(root->tick(context2), NodeStatus::Success);
}
