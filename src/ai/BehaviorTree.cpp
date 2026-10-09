// ===========================================================================
//  src/ai/BehaviorTree.cpp
// ===========================================================================
#include "ai/BehaviorTree.h"

#include <algorithm>
#include <sstream>

namespace TacticalAI::AI {

const char* toString(NodeStatus status) noexcept {
    switch (status) {
        case NodeStatus::Success: return "Success";
        case NodeStatus::Failure: return "Failure";
        case NodeStatus::Running: return "Running";
        case NodeStatus::Invalid: return "Invalid";
    }
    return "Invalid";
}

// ===========================================================================
//  BehaviorNode
// ===========================================================================
NodeStatus BehaviorNode::tick(TickContext& context) {
    ++tickCount_;

    // 首次进入（或从中断后重新进入）：触发 onEnter 做准备工作
    if (!entered_) {
        entered_ = true;
        onEnter(context);
    }

    const NodeStatus status = onTick(context);

    // 状态转换：本步结束（非 Running）时必须收尾。
    //
    // 关键语义：onEnter 与 onExit 必须严格配对——只要节点「进入过」，
    // 那么它结束的那一步就必须收到 onExit，无论中途是否经过 Running。
    // 一个 action 在同一 tick 内 enter -> Success 也要走 exit，
    // 否则「释放锁 / 提交事务 / 记录统计」这类收尾逻辑会被漏掉。
    if (status == NodeStatus::Running) {
        isRunning_ = true;
    } else {
        // 非 Running：本次执行结束，entered_ 归位以便下次重新 onEnter
        if (entered_) {
            entered_ = false;
        }
        isRunning_ = false;
        onExit(context, status);
    }

    lastStatus_ = status;
    return status;
}

void BehaviorNode::abort(TickContext& context) {
    if (!entered_) {
        return;
    }

    // 只有真正在运行的分支才需要收尾。中止一个未启动的节点是 no-op，
    // 但对 Running 节点必须调 onExit，否则临时状态（如「已锁定目标」）不会释放。
    const NodeStatus previous = lastStatus_;
    isRunning_ = false;
    entered_   = false;
    lastStatus_ = NodeStatus::Invalid;
    onExit(context, previous);
}

void BehaviorNode::reset() {
    isRunning_ = false;
    entered_   = false;
    lastStatus_ = NodeStatus::Invalid;
    // tickCount_ 刻意不重置：它是累计性能统计，跨重置保留才有意义
    onReset();
}

// ===========================================================================
//  SequenceNode
// ===========================================================================
SequenceNode* SequenceNode::addChild(std::unique_ptr<BehaviorNode> child) {
    if (child) {
        children_.push_back(std::move(child));
    }
    return this;
}

void SequenceNode::onEnter(TickContext& context) {
    (void)context;
    currentIndex_ = 0;
}

NodeStatus SequenceNode::onTick(TickContext& context) {
    if (children_.empty()) {
        // 空顺序节点：逻辑上「全部完成」为真，故返回 Success
        return NodeStatus::Success;
    }

    while (currentIndex_ < children_.size()) {
        BehaviorNode* child = children_[currentIndex_].get();
        const NodeStatus status = child->tick(context);

        if (status == NodeStatus::Running) {
            // 本步停在这里，下步从同一个子节点继续
            return NodeStatus::Running;
        }
        if (status == NodeStatus::Failure) {
            // 一个失败即整体失败，且需要重置进度，
            // 否则下次进入时会从中间开始，跳过前置步骤。
            onReset();
            return NodeStatus::Failure;
        }

        // Success：推进到下一个子节点，本步继续（不消耗额外逻辑步）
        ++currentIndex_;
    }

    // 全部子节点成功
    onReset();
    return NodeStatus::Success;
}

void SequenceNode::onExit(TickContext& context, NodeStatus status) {
    // 被中断时（无论成功失败），必须中止正在 Running 的子节点
    if (currentIndex_ < children_.size()) {
        children_[currentIndex_]->abort(context);
    }
    (void)status;
}

void SequenceNode::onReset() {
    currentIndex_ = 0;
}

// ===========================================================================
//  SelectorNode
// ===========================================================================
SelectorNode* SelectorNode::addChild(std::unique_ptr<BehaviorNode> child) {
    if (child) {
        children_.push_back(std::move(child));
    }
    return this;
}

void SelectorNode::onEnter(TickContext& context) {
    (void)context;
    currentIndex_ = 0;
}

NodeStatus SelectorNode::onTick(TickContext& context) {
    if (children_.empty()) {
        // 空选择节点：没有任何可用行为，只能失败
        return NodeStatus::Failure;
    }

    // 反应式选择器：每一步都从最高优先级子节点重新评估。
    //
    // 这是战术 AI 的关键语义。非反应式（默认）选择器一旦进入某个
    // Running 的分支就会「记住」它（currentIndex_ 不重置），直到该分支
    // 结束才重新评估。对于「撤退 > 交战 > 搜索 > 巡逻」这种根选择器，
    // 这意味着一旦落到最末的巡逻分支（它永远 Running），AI 就再也
    // 不会回头检查「我是不是看见敌人了」——表现为敌人从玩家面前走过
    // 却毫无反应。
    //
    // reactive_ 的实现在此前缺失：setReactive(true) 只改了标志位，
    // onTick 从未读取它。这里补齐。

    // --- 非反应式：沿用「记住当前分支」的旧语义 ---
    if (!reactive_) {
        while (currentIndex_ < children_.size()) {
            BehaviorNode* child = children_[currentIndex_].get();
            const NodeStatus status = child->tick(context);

            if (status == NodeStatus::Running) {
                return NodeStatus::Running;
            }
            if (status == NodeStatus::Success) {
                onReset();
                return NodeStatus::Success;
            }

            // Failure：尝试下一个优先级更低的分支
            ++currentIndex_;
        }

        onReset();
        return NodeStatus::Failure;
    }

    // --- 反应式：每步从最高优先级重新评估 ---
    //
    // 【修复】旧实现在「尚未知道高优先级分支是否会接管」时就无条件
    // abort 当前 Running 的分支，然后从头重评。后果是任何 index > 0 的
    // 跨帧行为（Wait、移动到某点、持续射击）每帧被 abort → 重新 onEnter
    // → 状态归零，永远无法完成；同时每帧多出一对 onExit/onEnter 抖动。
    // 而 index == 0 的分支因为有 `currentIndex_ != 0` 的条件而幸免，
    // 恰好掩盖了这个问题的普遍性。
    //
    // 正确语义：先从 index 0 重评；只有当**别的**分支真正接管
    // （返回 Running / Success）时，才让旧分支 abort 收尾。
    // abort 对未处于 Running 的节点是 no-op（见 BehaviorNode::abort），
    // 因此这里可以无条件调用而不必担心误伤已结束的分支。
    const size_t runningIndex = currentIndex_;
    const bool   hasRunning   = runningIndex < children_.size();

    for (size_t i = 0; i < children_.size(); ++i) {
        BehaviorNode* child = children_[i].get();
        const NodeStatus status = child->tick(context);

        if (status == NodeStatus::Running) {
            // i == runningIndex 表示仍是同一分支在跑：不能 abort，
            // 否则它的跨帧状态（如 WaitNode 的 elapsed_）会被清零。
            if (hasRunning && i != runningIndex) {
                children_[runningIndex]->abort(context);
            }
            currentIndex_ = i;
            return NodeStatus::Running;
        }
        if (status == NodeStatus::Success) {
            if (hasRunning && i != runningIndex) {
                children_[runningIndex]->abort(context);
            }
            onReset();
            return NodeStatus::Success;
        }

        // Failure：继续尝试下一个优先级更低的分支
    }

    // 所有分支都失败：让旧分支收尾并重置进度
    if (hasRunning) {
        children_[runningIndex]->abort(context);
    }
    onReset();
    return NodeStatus::Failure;
}

void SelectorNode::onExit(TickContext& context, NodeStatus status) {
    if (currentIndex_ < children_.size()) {
        children_[currentIndex_]->abort(context);
    }
    (void)status;
}

void SelectorNode::onReset() {
    currentIndex_ = 0;
}

// ===========================================================================
//  ParallelNode
// ===========================================================================
ParallelNode* ParallelNode::addChild(std::unique_ptr<BehaviorNode> child) {
    if (child) {
        children_.push_back(std::move(child));
        childStatuses_.push_back(NodeStatus::Invalid);
    }
    return this;
}

void ParallelNode::onEnter(TickContext& context) {
    (void)context;
    std::fill(childStatuses_.begin(), childStatuses_.end(), NodeStatus::Invalid);
}

NodeStatus ParallelNode::onTick(TickContext& context) {
    if (children_.empty()) {
        return NodeStatus::Success;
    }

    int successCount = 0;
    int failureCount = 0;
    int runningCount = 0;

    for (size_t i = 0; i < children_.size(); ++i) {
        // 已结束的子节点不再 tick，避免重复触发副作用
        // （例如「射击」动作被 tick 两次会消耗两发子弹）
        if (childStatuses_[i] == NodeStatus::Success ||
            childStatuses_[i] == NodeStatus::Failure) {
            if (childStatuses_[i] == NodeStatus::Success) {
                ++successCount;
            } else {
                ++failureCount;
            }
            continue;
        }

        const NodeStatus status = children_[i]->tick(context);
        childStatuses_[i] = status;

        switch (status) {
            case NodeStatus::Success: ++successCount; break;
            case NodeStatus::Failure: ++failureCount; break;
            case NodeStatus::Running: ++runningCount; break;
            case NodeStatus::Invalid: break;
        }
    }

    // 先判失败：多数并行场景里「失败」是硬约束，应立即终止
    const bool failureReached =
        (failurePolicy_ == FailurePolicy::RequireOne)
            ? (failureCount > 0)
            : (failureCount == static_cast<int>(children_.size()));

    if (failureReached) {
        return NodeStatus::Failure;
    }

    const bool successReached =
        (successPolicy_ == SuccessPolicy::RequireAll)
            ? (successCount == static_cast<int>(children_.size()))
            : (successCount > 0);

    // RequireAll：必须所有子节点都结束（无 Running）且都成功。
    // RequireOne：只要有一个子节点成功就立刻判定整体成功——
    //   这正是「任一成功即成功」的语义，继续等其余 Running 子节点
    //   会违背意图（例如「射击成功」不必等「移动到位」）。
    //   onExit 会负责中断剩下的 Running 子节点。
    if (successPolicy_ == SuccessPolicy::RequireAll) {
        if (successReached) {
            return NodeStatus::Success;
        }
    } else if (successReached) {
        return NodeStatus::Success;
    }

    return NodeStatus::Running;
}

void ParallelNode::onExit(TickContext& context, NodeStatus status) {
    // 中断所有仍在 Running 的子节点
    for (size_t i = 0; i < children_.size(); ++i) {
        if (childStatuses_[i] == NodeStatus::Running) {
            children_[i]->abort(context);
        }
    }
    (void)status;
}

void ParallelNode::onReset() {
    std::fill(childStatuses_.begin(), childStatuses_.end(), NodeStatus::Invalid);
}

// ===========================================================================
//  DecoratorNode
// ===========================================================================
void DecoratorNode::abort(TickContext& context) {
    if (child_) {
        child_->abort(context);
    }
    BehaviorNode::abort(context);
}

void DecoratorNode::reset() {
    if (child_) {
        child_->reset();
    }
    BehaviorNode::reset();
}

// ===========================================================================
//  InverterNode
// ===========================================================================
NodeStatus InverterNode::onTick(TickContext& context) {
    if (!child_) {
        return NodeStatus::Failure;
    }

    const NodeStatus status = child_->tick(context);

    // Running 必须透传：否则「取反一个正在执行的行为」会立刻得到假结果
    if (status == NodeStatus::Running)  { return NodeStatus::Running; }
    if (status == NodeStatus::Success)  { return NodeStatus::Failure; }
    if (status == NodeStatus::Failure)  { return NodeStatus::Success; }
    return NodeStatus::Invalid;
}

// ===========================================================================
//  SucceederNode
// ===========================================================================
NodeStatus SucceederNode::onTick(TickContext& context) {
    if (!child_) {
        return NodeStatus::Success;
    }

    const NodeStatus status = child_->tick(context);
    if (status == NodeStatus::Running) {
        return NodeStatus::Running;
    }
    // 无论子节点成功还是失败，都报告成功——
    // 这样父级 Sequence 不会因为「可选步骤失败」而中断整体流程
    return NodeStatus::Success;
}

// ===========================================================================
//  RepeaterNode
// ===========================================================================
NodeStatus RepeaterNode::onTick(TickContext& context) {
    if (!child_) {
        return NodeStatus::Failure;
    }

    const NodeStatus status = child_->tick(context);

    if (status == NodeStatus::Running) {
        return NodeStatus::Running;
    }
    if (status == NodeStatus::Failure) {
        // 子节点失败：整个重复节点失败，并复位计数
        onReset();
        return NodeStatus::Failure;
    }

    ++completed_;

    // repeatCount_ < 0 表示无限重复
    if (repeatCount_ < 0 || completed_ < repeatCount_) {
        // 重置子节点以便重新开始（关键：否则子节点会带着已完成状态立即返回 Success，
        // 导致「重复」变成瞬间完成的空转）
        child_->reset();
        return NodeStatus::Running;
    }

    completed_ = 0;
    return NodeStatus::Success;
}

void RepeaterNode::onReset() {
    completed_ = 0;
}

// ===========================================================================
//  CooldownNode
// ===========================================================================
NodeStatus CooldownNode::onTick(TickContext& context) {
    if (!child_) {
        return NodeStatus::Failure;
    }

    // 冷却中：直接失败，让 Selector 落到下一个优先级分支
    if (context.gameTime < readyAtTime_) {
        return NodeStatus::Failure;
    }

    const NodeStatus status = child_->tick(context);

    if (status == NodeStatus::Success) {
        // 成功即进入冷却
        readyAtTime_ = context.gameTime + cooldownSeconds_;
    }

    return status;
}

void CooldownNode::onReset() {
    readyAtTime_ = 0.0;
}

// ===========================================================================
//  BehaviorTree
// ===========================================================================
BehaviorTree::BehaviorTree(std::string name) : name_(std::move(name)) {}

BehaviorTree::~BehaviorTree() = default;

void BehaviorTree::setRoot(std::unique_ptr<BehaviorNode> root) {
    root_ = std::move(root);
    lastStatus_ = NodeStatus::Invalid;
}

NodeStatus BehaviorTree::tick(TickContext& context) {
    if (!root_) {
        lastStatus_ = NodeStatus::Invalid;
        return NodeStatus::Invalid;
    }

    lastTickHadAbort_ = false;
    ++tickCount_;

    lastStatus_ = root_->tick(context);
    return lastStatus_;
}

void BehaviorTree::abort(TickContext& context) {
    if (root_) {
        root_->abort(context);
        lastTickHadAbort_ = true;
    }
}

void BehaviorTree::reset() {
    if (root_) {
        root_->reset();
    }
    lastStatus_ = NodeStatus::Invalid;
    lastTickHadAbort_ = false;
}

size_t BehaviorTree::nodeCount() const {
    size_t count = 0;
    std::string ignored;
    describeNode(root_.get(), 0, ignored, count);
    return count;
}

std::string BehaviorTree::describe() const {
    std::string out;
    out += "BehaviorTree \"" + name_ + "\" (ticks=" + std::to_string(tickCount_) + ")\n";
    size_t count = 0;
    describeNode(root_.get(), 1, out, count);
    out += "节点总数: " + std::to_string(count) + "\n";
    return out;
}

void BehaviorTree::describeNode(const BehaviorNode* node, int depth, std::string& out,
                                size_t& totalNodes) {
    if (!node) {
        return;
    }

    ++totalNodes;
    out.append(static_cast<size_t>(depth) * 2, ' ');
    out += "- ";
    out += node->name();
    out += "  [";
    out += toString(node->lastStatus());
    out += "]\n";

    // 组合节点的子节点
    if (const auto* children = node->children()) {
        for (const auto& child : *children) {
            describeNode(child.get(), depth + 1, out, totalNodes);
        }
        return;
    }

    // 装饰器：通过 dynamic_cast 拿唯一子节点。
    // 这里的开销只在调试路径上，可以接受。
    if (const auto* decorator = dynamic_cast<const DecoratorNode*>(node)) {
        describeNode(decorator->child(), depth + 1, out, totalNodes);
    }
}

// ===========================================================================
//  工厂函数
// ===========================================================================
namespace bt {

std::unique_ptr<BehaviorNode> always(std::string name, NodeStatus result) {
    return std::make_unique<AlwaysNode>(std::move(name), result);
}

std::unique_ptr<BehaviorNode> wait(std::string name, double seconds) {
    return std::make_unique<WaitNode>(std::move(name), seconds);
}

std::unique_ptr<BehaviorNode> succeed(std::string name, std::unique_ptr<BehaviorNode> child) {
    return std::make_unique<SucceederNode>(std::move(name), std::move(child));
}

std::unique_ptr<BehaviorNode> invert(std::string name, std::unique_ptr<BehaviorNode> child) {
    return std::make_unique<InverterNode>(std::move(name), std::move(child));
}

std::unique_ptr<BehaviorNode> repeat(std::string name, std::unique_ptr<BehaviorNode> child,
                                     int count) {
    return std::make_unique<RepeaterNode>(std::move(name), std::move(child), count);
}

std::unique_ptr<BehaviorNode> cooldown(std::string name, std::unique_ptr<BehaviorNode> child,
                                       double seconds) {
    return std::make_unique<CooldownNode>(std::move(name), std::move(child), seconds);
}

// 模板工厂在头文件中声明，此处提供显式实例化辅助。
// 由于模板需要在使用点可见，实际定义放在 BehaviorTree.inl 风格的内联实现里。
// 这里通过一个内部辅助函数统一插入逻辑。

namespace detail {

/// 把一批子节点挂到 Sequence 上。
inline void appendChildren(SequenceNode* parent,
                           std::vector<std::unique_ptr<BehaviorNode>>& children) {
    for (auto& child : children) {
        parent->addChild(std::move(child));
    }
}

inline void appendChildren(SelectorNode* parent,
                           std::vector<std::unique_ptr<BehaviorNode>>& children) {
    for (auto& child : children) {
        parent->addChild(std::move(child));
    }
}

inline void appendChildren(ParallelNode* parent,
                           std::vector<std::unique_ptr<BehaviorNode>>& children) {
    for (auto& child : children) {
        parent->addChild(std::move(child));
    }
}

} // namespace detail

} // namespace bt

} // namespace TacticalAI::AI
