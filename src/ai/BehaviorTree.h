// ===========================================================================
//  src/ai/BehaviorTree.h
//
//  行为树。战术 AI 的决策骨架。
//
//  节点语义（业界标准四态，比常见的三态多一个 Running）：
//    Success —— 本节点目标已达成，本步结束
//    Failure —— 本节点目标无法达成，本步结束
//    Running —— 本节点还在执行中（跨多个逻辑步的持续行为，
//               如「移动到掩体」需要多帧才能走完）
//    Invalid —— 节点未初始化或已被中断（内部用于检测误用）
//
//  组合节点：
//    Sequence  （→）从左到右执行，任一子节点 Failure 则整体 Failure，
//                   全部 Success 才 Success，遇到 Running 则本步停在 Running。
//                   语义 = 逻辑与（AND），用于「先接近，再攻击」这类有序流程。
//    Selector  （?）从左到右尝试，任一 Success 则整体 Success，
//                   全部 Failure 才 Failure，遇到 Running 则本步停在 Running。
//                   语义 = 逻辑或（OR），用于「能不能射就射，不能就追，再不能就撤」。
//    Parallel      并行执行所有子节点，按策略决定成功条件。
//
//  装饰节点：
//    Inverter      取反 Success/Failure
//    Succeeder     恒返回 Success（用于「可选步骤」）
//    Repeater      重复 N 次
//    Cooldown      冷却时间限制（如射击间隔）
//    Condition     包裹一个布尔判断，把「条件」统一成节点
//
//  中断机制：
//    BehaviorTree::tick 每步从根节点执行一次。Running 状态的节点需要在
//    onExit 中清理临时状态。若上层节点本步改变了决策（例如从「追击」切到
//    「撤退」），Selector 会先 abort 之前 Running 的分支再尝试新分支。
// ===========================================================================
#pragma once

#include <chrono>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "ai/Blackboard.h"

namespace TacticalAI::AI {

/// 节点执行结果。
enum class NodeStatus : uint8_t {
    Success = 0,
    Failure,
    Running,
    Invalid
};

[[nodiscard]] const char* toString(NodeStatus status) noexcept;

/// 节点执行上下文。每步传入，避免节点持有可变引用导致的悬垂。
struct TickContext {
    Blackboard& blackboard;
    double      fixedDelta  = 0.0;   ///< 固定步长（秒）
    double      gameTime    = 0.0;   ///< 当前游戏时间（秒）
    uint64_t    tickIndex   = 0;     ///< 本 agent 的累计 tick 次数

    /// 因超时或主动中断而中止某分支时置位，节点可据此放弃工作。
    bool        aborted     = false;

    /// 执行本步的 agent 名字，写入黑板时用作 lastWriter，便于追溯。
    std::string agentName;
};

/// 节点基类。
///
/// 生命周期：onEnter（首次进入本节点时）-> onTick（每逻辑步）-> onExit（离开时）。
/// 派生类通常只需重写 onTick；需要维护跨帧状态时才重写另外两个。
class BehaviorNode {
public:
    explicit BehaviorNode(std::string name) : name_(std::move(name)) {}
    virtual ~BehaviorNode() = default;

    BehaviorNode(const BehaviorNode&)            = delete;
    BehaviorNode& operator=(const BehaviorNode&) = delete;

    /// 执行一步。外部（组合节点 / 行为树）调用这个而非直接调 onTick，
    /// 因为它负责维护 Running 状态的进入/退出配对。
    NodeStatus tick(TickContext& context);

    /// 强制中止当前 Running 的行为，并递归中止所有子节点。
    /// 中断比继续执行更重要：如果「追击」分支正在 Running，
    /// 而本步决定改为「撤退」，必须先让追击分支清理状态。
    virtual void abort(TickContext& context);

    /// 立即重置到未执行状态（不触发 onExit）。
    /// 用于 agent 重生、关卡重载等场景。
    virtual void reset();

    [[nodiscard]] const std::string& name() const noexcept { return name_; }

    /// 上次 tick 的结果，供调试 UI 显示（在树视图上标绿/红/黄）。
    [[nodiscard]] NodeStatus lastStatus() const noexcept { return lastStatus_; }

    /// 本节点及其子树的累计 tick 次数，用于性能分析。
    [[nodiscard]] uint64_t tickCount() const noexcept { return tickCount_; }

    /// 是否处于 Running 状态。
    [[nodiscard]] bool isRunning() const noexcept { return isRunning_; }

    /// 子节点只读访问（调试 UI 遍历用）。
    [[nodiscard]] virtual const std::vector<std::unique_ptr<BehaviorNode>>* children() const noexcept {
        return nullptr;
    }

protected:
    /// 首次进入本节点（或从中断后重新进入）时调用一次。
    virtual void onEnter(TickContext& context) { (void)context; }

    /// 每步执行的核心逻辑。
    virtual NodeStatus onTick(TickContext& context) = 0;

    /// 本节点返回非 Running 结果、或被 abort 时调用。
    virtual void onExit(TickContext& context, NodeStatus status) {
        (void)context;
        (void)status;
    }

    /// 清理本节点自身的状态（不含子节点），供 reset 复用。
    virtual void onReset() {}

    std::string name_;
    NodeStatus  lastStatus_ = NodeStatus::Invalid;
    uint64_t    tickCount_  = 0;
    bool        isRunning_  = false;
    bool        entered_    = false;
};

// ===========================================================================
//  叶子节点
// ===========================================================================

/// 立刻返回固定结果的节点。构造行为树的「占位」与「必然分支」。
class AlwaysNode final : public BehaviorNode {
public:
    AlwaysNode(std::string name, NodeStatus fixedResult)
        : BehaviorNode(std::move(name)), fixedResult_(fixedResult) {}

protected:
    NodeStatus onTick(TickContext& context) override {
        (void)context;
        return fixedResult_;
    }

private:
    NodeStatus fixedResult_;
};

/// 持续 N 秒的「等待」节点。用于「原地观察 2 秒再行动」这类战术等待。
/// 是 Running 状态最直观的示例。
class WaitNode final : public BehaviorNode {
public:
    explicit WaitNode(std::string name, double duration = 1.0)
        : BehaviorNode(std::move(name)), duration_(duration) {}

    void setDuration(double seconds) noexcept { duration_ = seconds; }

protected:
    void onEnter(TickContext& context) override {
        (void)context;
        elapsed_ = 0.0;
    }

    NodeStatus onTick(TickContext& context) override {
        elapsed_ += context.fixedDelta;
        if (elapsed_ >= duration_) {
            return NodeStatus::Success;
        }
        return NodeStatus::Running;
    }

    void onReset() override { elapsed_ = 0.0; }

private:
    double duration_ = 1.0;
    double elapsed_  = 0.0;
};

/// 条件节点：包裹一个布尔谓词。谓词在「进入时」求值并缓存整个持续期间，
/// 避免条件在 Running 过程中翻转导致行为抖动。
/// 若希望每步重新求值，把 reEvaluateEveryTick 设为 true。
class ConditionNode final : public BehaviorNode {
public:
    using Predicate = std::function<bool(const TickContext&)>;

    ConditionNode(std::string name, Predicate predicate, bool reEvaluateEveryTick = false)
        : BehaviorNode(std::move(name)),
          predicate_(std::move(predicate)),
          reEvaluate_(reEvaluateEveryTick) {}

protected:
    NodeStatus onTick(TickContext& context) override {
        // 缓存模式下只在首次求值，之后沿用结果——
        // 这使「生命值 > 30%」这类条件在整个分支执行期间保持稳定。
        if (reEvaluate_ || !evaluated_) {
            cached_ = predicate_ ? predicate_(context) : false;
            evaluated_ = true;
        }
        return cached_ ? NodeStatus::Success : NodeStatus::Failure;
    }

    void onReset() override {
        evaluated_ = false;
        cached_ = false;
    }

private:
    Predicate predicate_;
    bool reEvaluate_ = false;
    bool evaluated_  = false;
    bool cached_     = false;
};

/// 动作节点：执行一个具体行为。与 ConditionNode 的区别在于
/// 它可以返回 Running（持续行为），且通常会产生副作用（写黑板、开火）。
class ActionNode final : public BehaviorNode {
public:
    using Action = std::function<NodeStatus(TickContext&)>;
    using Callback = std::function<void(TickContext&)>;

    ActionNode(std::string name, Action action)
        : BehaviorNode(std::move(name)), action_(std::move(action)) {}

    /// 设置进入/退出回调，用于准备与清理（如设置「正在射击」标志）。
    void onEnterCallback(Callback callback)  { enterCallback_ = std::move(callback); }
    void onExitCallback(Callback callback)   { exitCallback_  = std::move(callback); }

protected:
    void onEnter(TickContext& context) override {
        if (enterCallback_) {
            enterCallback_(context);
        }
    }

    NodeStatus onTick(TickContext& context) override {
        return action_ ? action_(context) : NodeStatus::Failure;
    }

    void onExit(TickContext& context, NodeStatus status) override {
        // 无论成功失败还是被中断，都要走清理回调——
        // 否则被中断的「射击」分支会忘记复位瞄准标志。
        if (exitCallback_) {
            exitCallback_(context);
        }
        (void)status;
    }

private:
    Action   action_;
    Callback enterCallback_;
    Callback exitCallback_;
};

// ===========================================================================
//  组合节点
// ===========================================================================

/// 顺序节点（AND）。子节点按顺序执行，全部成功才算成功。
/// 常用于表达「先做 A，再做 B」，例如「移动到掩体 → 等待 → 探出射击」。
class SequenceNode final : public BehaviorNode {
public:
    explicit SequenceNode(std::string name) : BehaviorNode(std::move(name)) {}

    /// 添加子节点。链式调用：seq->addChild(a)->addChild(b)。
    SequenceNode* addChild(std::unique_ptr<BehaviorNode> child);

    [[nodiscard]] const std::vector<std::unique_ptr<BehaviorNode>>* children() const noexcept override {
        return &children_;
    }

protected:
    void onEnter(TickContext& context) override;
    NodeStatus onTick(TickContext& context) override;
    void onExit(TickContext& context, NodeStatus status) override;
    void onReset() override;

private:
    std::vector<std::unique_ptr<BehaviorNode>> children_;
    size_t currentIndex_ = 0;
};

/// 选择节点（OR）。子节点按优先级从左到右尝试，第一个成功的胜出。
/// 这是行为树「优先级调度」的核心——把最高优先级的行为放最左边。
class SelectorNode final : public BehaviorNode {
public:
    explicit SelectorNode(std::string name) : BehaviorNode(std::move(name)) {}

    SelectorNode* addChild(std::unique_ptr<BehaviorNode> child);

    [[nodiscard]] const std::vector<std::unique_ptr<BehaviorNode>>* children() const noexcept override {
        return &children_;
    }

    /// 中断模式。反应式选择器每步都重新评估，行为切换更灵敏但开销更大。
    /// 默认非反应式：一旦某分支 Running，就继续执行它直到结束。
    void setReactive(bool reactive) noexcept { reactive_ = reactive; }
    [[nodiscard]] bool isReactive() const noexcept { return reactive_; }

protected:
    void onEnter(TickContext& context) override;
    NodeStatus onTick(TickContext& context) override;
    void onExit(TickContext& context, NodeStatus status) override;
    void onReset() override;

private:
    std::vector<std::unique_ptr<BehaviorNode>> children_;
    size_t currentIndex_ = 0;
    bool   reactive_ = false;
};

/// 并行节点。同时 tick 所有子节点，按成功/失败策略判定。
/// 战术示例：「一边压制射击，一边侧移」。
class ParallelNode final : public BehaviorNode {
public:
    /// 成功条件。
    enum class SuccessPolicy {
        RequireAll,      ///< 所有子节点成功才算成功
        RequireOne       ///< 任一子节点成功即成功
    };

    /// 失败条件。
    enum class FailurePolicy {
        RequireAll,      ///< 所有子节点失败才算失败
        RequireOne       ///< 任一子节点失败即失败
    };

    explicit ParallelNode(std::string name,
                          SuccessPolicy successPolicy = SuccessPolicy::RequireAll,
                          FailurePolicy failurePolicy = FailurePolicy::RequireAll)
        : BehaviorNode(std::move(name)),
          successPolicy_(successPolicy),
          failurePolicy_(failurePolicy) {}

    ParallelNode* addChild(std::unique_ptr<BehaviorNode> child);

    [[nodiscard]] const std::vector<std::unique_ptr<BehaviorNode>>* children() const noexcept override {
        return &children_;
    }

protected:
    void onEnter(TickContext& context) override;
    NodeStatus onTick(TickContext& context) override;
    void onExit(TickContext& context, NodeStatus status) override;
    void onReset() override;

private:
    std::vector<std::unique_ptr<BehaviorNode>> children_;
    std::vector<NodeStatus> childStatuses_;

    SuccessPolicy successPolicy_;
    FailurePolicy failurePolicy_;
};

// ===========================================================================
//  装饰节点
// ===========================================================================

/// 单个子节点的装饰器基类。
class DecoratorNode : public BehaviorNode {
public:
    DecoratorNode(std::string name, std::unique_ptr<BehaviorNode> child)
        : BehaviorNode(std::move(name)), child_(std::move(child)) {}

    [[nodiscard]] const std::vector<std::unique_ptr<BehaviorNode>>* children() const noexcept override {
        // 装饰器只有一个子节点，为了复用统一接口仍以 vector 返回。
        // 这里用 mutable 成员缓存，避免每次构造临时对象。
        singleChildView_.clear();
        if (child_) {
            // vector<unique_ptr> 不可拷贝，因此用裸指针视图
            // 由调试 UI 用 child() 直接访问即可；此处返回 nullptr 表示无子集合。
        }
        return nullptr;
    }

    [[nodiscard]] BehaviorNode* child() noexcept { return child_.get(); }
    [[nodiscard]] const BehaviorNode* child() const noexcept { return child_.get(); }

    void abort(TickContext& context) override;
    void reset() override;

protected:
    std::unique_ptr<BehaviorNode> child_;
    mutable std::vector<std::unique_ptr<BehaviorNode>> singleChildView_;
};

/// 取反装饰器：Success <-> Failure，Running 透传。
class InverterNode final : public DecoratorNode {
public:
    InverterNode(std::string name, std::unique_ptr<BehaviorNode> child)
        : DecoratorNode(std::move(name), std::move(child)) {}

protected:
    NodeStatus onTick(TickContext& context) override;
};

/// 恒成功装饰器：无论子节点结果如何都返回 Success（Running 仍透传）。
/// 用于「这一步是可选优化，失败了也继续」。
class SucceederNode final : public DecoratorNode {
public:
    SucceederNode(std::string name, std::unique_ptr<BehaviorNode> child)
        : DecoratorNode(std::move(name), std::move(child)) {}

protected:
    NodeStatus onTick(TickContext& context) override;
};

/// 重复装饰器：重复执行子节点 N 次（N < 0 表示无限重复，直到子树向上被中断）。
class RepeaterNode final : public DecoratorNode {
public:
    RepeaterNode(std::string name, std::unique_ptr<BehaviorNode> child, int repeatCount = 3)
        : DecoratorNode(std::move(name), std::move(child)), repeatCount_(repeatCount) {}

protected:
    NodeStatus onTick(TickContext& context) override;
    void onReset() override;

private:
    int repeatCount_ = 3;
    int completed_   = 0;
};

/// 冷却装饰器：子节点成功后的 cooldownSeconds 内，本节点直接返回 Failure。
/// 是「射击间隔」「技能 CD」最干净的实现方式——不需要节点自己记时间。
class CooldownNode final : public DecoratorNode {
public:
    CooldownNode(std::string name, std::unique_ptr<BehaviorNode> child, double cooldownSeconds)
        : DecoratorNode(std::move(name), std::move(child)),
          cooldownSeconds_(cooldownSeconds) {}

protected:
    NodeStatus onTick(TickContext& context) override;
    void onReset() override;

private:
    double cooldownSeconds_ = 1.0;
    double readyAtTime_     = 0.0;
};

// ===========================================================================
//  行为树
// ===========================================================================

/// 行为树。持有一个根节点，负责驱动 tick 与收集统计。
class BehaviorTree {
public:
    explicit BehaviorTree(std::string name = "BehaviorTree");
    ~BehaviorTree();

    BehaviorTree(const BehaviorTree&)            = delete;
    BehaviorTree& operator=(const BehaviorTree&) = delete;

    /// 设置根节点。重复调用会替换整棵树。
    void setRoot(std::unique_ptr<BehaviorNode> root);
    [[nodiscard]] BehaviorNode* root() noexcept { return root_.get(); }
    [[nodiscard]] const BehaviorNode* root() const noexcept { return root_.get(); }

    /// 执行一步。返回根节点的结果。
    NodeStatus tick(TickContext& context);

    /// 中止当前所有 Running 行为，但不重置内部计数器。
    void abort(TickContext& context);

    /// 完全重置（agent 重生时使用）。
    void reset();

    [[nodiscard]] const std::string& name() const noexcept { return name_; }
    [[nodiscard]] NodeStatus lastStatus() const noexcept { return lastStatus_; }
    [[nodiscard]] uint64_t tickCount() const noexcept { return tickCount_; }

    /// 上次 tick 是否发生了「Running 分支被中断」。
    /// 调试时这是最有价值的信号之一——频繁中断说明优先级设计有问题。
    [[nodiscard]] bool lastTickHadAbort() const noexcept { return lastTickHadAbort_; }

    /// 生成树结构的文本缩略图，供调试面板或日志使用。
    [[nodiscard]] std::string describe() const;

    /// 统计树中节点总数（含组合与叶子）。
    [[nodiscard]] size_t nodeCount() const;

private:
    /// 递归生成单节点的缩略图。totalNodes 以引用累加，便于统计整棵树。
    static void describeNode(const BehaviorNode* node, int depth, std::string& out,
                             size_t& totalNodes);

    std::string name_;
    std::unique_ptr<BehaviorNode> root_;
    NodeStatus  lastStatus_       = NodeStatus::Invalid;
    uint64_t    tickCount_        = 0;
    bool        lastTickHadAbort_ = false;
};

// ===========================================================================
//  建造器辅助（让树的定义读起来像它执行的逻辑）
// ===========================================================================

/// 创建一个 Sequence 节点并直接返回其裸指针，便于链式构造：
/// @code
///   tree.setRoot(seq("Root",
///       action("Aim", ...),
///       cond("HasAmmo", ...)
///   ));
/// @endcode
/// 所有权在构造过程中转移给父节点，因此返回裸指针是安全的。
namespace bt {

/// 顺序节点构造器。
template <typename... Children>
[[nodiscard]] std::unique_ptr<BehaviorNode> seq(std::string name, Children&&... children);

/// 选择节点构造器。
template <typename... Children>
[[nodiscard]] std::unique_ptr<BehaviorNode> sel(std::string name, Children&&... children);

/// 并行节点构造器。
template <typename... Children>
[[nodiscard]] std::unique_ptr<BehaviorNode> par(std::string name, Children&&... children);

/// 以下工厂函数便于在构造表达式内联使用
[[nodiscard]] std::unique_ptr<BehaviorNode> always(std::string name, NodeStatus result);
[[nodiscard]] std::unique_ptr<BehaviorNode> wait(std::string name, double seconds);
[[nodiscard]] std::unique_ptr<BehaviorNode> succeed(std::string name,
                                                    std::unique_ptr<BehaviorNode> child);
[[nodiscard]] std::unique_ptr<BehaviorNode> invert(std::string name,
                                                   std::unique_ptr<BehaviorNode> child);
[[nodiscard]] std::unique_ptr<BehaviorNode> repeat(std::string name,
                                                   std::unique_ptr<BehaviorNode> child,
                                                   int count);
[[nodiscard]] std::unique_ptr<BehaviorNode> cooldown(std::string name,
                                                     std::unique_ptr<BehaviorNode> child,
                                                     double seconds);

} // namespace bt

} // namespace TacticalAI::AI
