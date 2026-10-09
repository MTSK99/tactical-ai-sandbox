// ===========================================================================
//  src/ai/BehaviorTree.inl
//
//  行为树模板工厂的内联实现。
//
//  为什么单独一个 .inl：
//    seq()/sel()/par() 是变参模板，必须在调用点可见。把它们塞进 .h 会让
//    主头文件被实现细节淹没；放在 .inl 并只在需要时 include，保持了
//    「接口头文件干净、实现细节可选」的层次。
//
//  用法：在使用 bt::seq(...) 的文件里 #include "ai/BehaviorTree.inl"
//  （Enemy.cpp 等构造树的地方已自动包含）
// ===========================================================================
#pragma once

#ifndef TACTICALAI_BEHAVIORTREE_INL_GUARD
#define TACTICALAI_BEHAVIORTREE_INL_GUARD

#include <memory>
#include <utility>
#include <vector>

#include "ai/BehaviorTree.h"

namespace TacticalAI::AI::bt {

namespace detail {

/// 把变参列表里的 unique_ptr 收集成 vector，统一交给组合节点。
/// 用折叠表达式（C++17）实现，比递归模板更易读。
template <typename... Children>
[[nodiscard]] std::vector<std::unique_ptr<BehaviorNode>> collect(Children&&... children) {
    std::vector<std::unique_ptr<BehaviorNode>> result;
    result.reserve(sizeof...(children));
    // 每项都 move 进去：工厂函数的返回值是临时对象，转移所有权是唯一正确做法
    (result.push_back(std::unique_ptr<BehaviorNode>(std::forward<Children>(children))), ...);
    return result;
}

} // namespace detail

template <typename... Children>
std::unique_ptr<BehaviorNode> seq(std::string name, Children&&... children) {
    auto node = std::make_unique<SequenceNode>(std::move(name));
    auto collected = detail::collect(std::forward<Children>(children)...);
    for (auto& child : collected) {
        node->addChild(std::move(child));
    }
    return node;
}

template <typename... Children>
std::unique_ptr<BehaviorNode> sel(std::string name, Children&&... children) {
    auto node = std::make_unique<SelectorNode>(std::move(name));
    auto collected = detail::collect(std::forward<Children>(children)...);
    for (auto& child : collected) {
        node->addChild(std::move(child));
    }
    return node;
}

/// 反应式选择器：每一步都从最高优先级子节点重新评估。
///
/// 为什么需要单独一个工厂函数：
///   普通 sel() 返回的是 unique_ptr<BehaviorNode>，调用点无法直接调用
///   SelectorNode::setReactive()（基类没有这个成员）。与其让每个调用点
///   写 static_cast，不如在这里把「构造 + 设为反应式」封装成一个语义
///   明确的工厂，让「我要一个反应式选择器」这个意图在调用点一目了然。
///
/// 战术 AI 的根选择器几乎总是应该用这个版本：否则一旦落入某个永远
/// Running 的分支（例如巡逻），更高优先级的分支（例如「发现敌人」）
/// 就再也得不到重新评估的机会。
template <typename... Children>
std::unique_ptr<BehaviorNode> reactiveSel(std::string name, Children&&... children) {
    auto node = std::make_unique<SelectorNode>(std::move(name));
    node->setReactive(true);
    auto collected = detail::collect(std::forward<Children>(children)...);
    for (auto& child : collected) {
        node->addChild(std::move(child));
    }
    return node;
}

template <typename... Children>
std::unique_ptr<BehaviorNode> par(std::string name, Children&&... children) {
    auto node = std::make_unique<ParallelNode>(std::move(name));
    auto collected = detail::collect(std::forward<Children>(children)...);
    for (auto& child : collected) {
        node->addChild(std::move(child));
    }
    return node;
}

/// 带并行策略的版本。
///
/// 为什么单独写一个重载而不是往上面塞变参：并行策略是两个枚举，
/// 而其余参数全是节点指针。若混在同一个变参列表里，collect() 会
/// 试图把枚举也当成 BehaviorNode 包装，编译期就会炸（而且报错极难读）。
/// 显式拆成「策略 + 节点」两段，调用点一旦写错立刻就能看出来。
template <typename... Children>
std::unique_ptr<BehaviorNode> par(std::string name,
                                  ParallelNode::SuccessPolicy successPolicy,
                                  ParallelNode::FailurePolicy failurePolicy,
                                  Children&&... children) {
    auto node = std::make_unique<ParallelNode>(std::move(name), successPolicy, failurePolicy);
    auto collected = detail::collect(std::forward<Children>(children)...);
    for (auto& child : collected) {
        node->addChild(std::move(child));
    }
    return node;
}

} // namespace TacticalAI::AI::bt

#endif // TACTICALAI_BEHAVIORTREE_INL_GUARD
