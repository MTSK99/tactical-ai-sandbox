// ===========================================================================
//  src/debug/AIDebugDraw.h
//
//  AI 调试可视化。这是整个项目最有价值的部分之一——
//  AI 是「看不见的逻辑」，把它画出来是唯一能真正理解它、
//  以及发现「敌人为什么卡在墙角」这类问题的办法。
//
//  可视化每种开关的含义：
//    感知 FOV   —— 黄色扇形。直观看到 AI 的视野死角，解释「它为什么没发现我」
//    视线射线   —— 从 AI 到目标的细线。绿色通、红色断
//    EQS 候选点 —— 灰点。所有被评估过的位置
//    EQS 最优   —— 绿色高亮。AI 最终选了哪里，以及它的各测试项得分
//    路径       —— 青绿色折线。AI 计划怎么走
//    状态标签   —— 实体头顶的文字。当前 AI 状态与关键数值
// ===========================================================================
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "engine/Renderer.h"
#include "game/World.h"

namespace TacticalAI::Debug {

/// 调试绘制的开关集合。每个开关对应一个快捷键。
struct DebugDrawFlags {
    bool showPerception   = false;   ///< 视野锥与视线
    bool showBehaviorTree = false;   ///< 行为树结构与执行状态
    bool showEQSCandidates = true;   ///< EQS 候选点与评分
    bool showPaths        = true;    ///< 寻路结果
    bool showEntities     = true;    ///< 实体本体与朝向
    bool showLabels       = true;    ///< 实体上方标签
    bool showGrid         = true;    ///< 导航网格可通行性
    bool showNavGridBlocked = false; ///< 用色块标出不可通行格（较费性能）
    bool showDamageEvents = true;    ///< 命中闪光效果
    bool showThreatSources = false;  ///< 威胁源位置
    bool showShotTracers  = true;    ///< 子弹与弹着残影（谁朝谁开火）

    /// 把开关状态转成一行文本，用于 HUD 显示。
    [[nodiscard]] std::string summary() const;
};

/// AI 调试绘制器。
///
/// 只读地访问 World —— 它不修改任何游戏状态。
/// 这是刻意的约束：调试代码绝不能影响被调试的系统，
/// 否则「打开调试后行为就变了」会成为无法调试的噩梦。
class AIDebugDraw {
public:
    AIDebugDraw() = default;

    /// 绘制所有启用的调试信息。
    /// 必须在 BeginMode2D（世界空间）内调用。
    void draw(const Engine::Renderer& renderer, const Game::World& world);

    /// 绘制屏幕空间的叠加信息（伤害数字、图例）。
    /// 必须在 EndMode2D 之后调用。
    void drawOverlay(const Engine::Renderer& renderer, const Game::World& world);

    [[nodiscard]] DebugDrawFlags& flags() noexcept { return flags_; }
    [[nodiscard]] const DebugDrawFlags& flags() const noexcept { return flags_; }

    /// 伤害事件闪光的持续时间（秒）。
    void setDamageFlashDuration(double seconds) noexcept {
        damageFlashDuration_ = seconds;
    }

private:
    // -- 分类绘制 ------------------------------------------------------------

    /// 绘制网格可通行性（用稀疏点阵，避免每格都画导致的性能问题）。
    void drawNavGrid(const Engine::Renderer& renderer, const Game::World& world) const;

    /// 绘制所有实体：本体、朝向、半径。
    void drawEntities(const Engine::Renderer& renderer, const Game::World& world) const;

    /// 绘制单个敌人的感知可视化。
    void drawPerception(const Engine::Renderer& renderer, const Game::Enemy& enemy) const;

    /// 绘制单个敌人的 EQS 候选点。
    void drawEQSCandidates(const Engine::Renderer& renderer, const Game::Enemy& enemy) const;

    /// 绘制单个敌人的路径。
    void drawPath(const Engine::Renderer& renderer, const Game::Enemy& enemy) const;

    /// 绘制单个敌人的头顶标签。
    void drawEntityLabel(const Engine::Renderer& renderer, const Game::Enemy& enemy) const;

    /// 绘制威胁源标记。
    void drawThreatSources(const Engine::Renderer& renderer, const Game::World& world) const;

    /// 绘制伤害闪光。
    void drawDamageFlashes(const Engine::Renderer& renderer, const Game::World& world) const;

    /// 绘制飞行中的子弹（本体 + 拖尾）。
    ///
    /// 这是"攻击是实体"的视觉兑现：屏幕上的亮点就是参与碰撞判定的那个东西，
    /// 它飞到哪、什么时候撞上墙、最后落在谁身上，都与逻辑完全一致。
    void drawProjectiles(const Engine::Renderer& renderer, const Game::World& world) const;

    /// 绘制弹着残影。子弹终止时留下的短命轨迹，用来回答"这一枪从哪来"。
    void drawImpactMarks(const Engine::Renderer& renderer, const Game::World& world) const;

    /// 在给定位置绘制一组「测试项得分」小条，直观看出 EQS 的权重作用。
    ///
    /// 注意：这里只画得分条，不显示测试项名字。名字需要由查询定义提供
    /// （EQSQuery 目前未导出测试名），硬编码名单会与实际 testScores
    /// 数量脱节，因此刻意不提供该参数。
    void drawScoreBreakdown(const Engine::Renderer& renderer, glm::vec2 anchor,
                            const AI::EQSCandidate& candidate) const;

    DebugDrawFlags flags_;
    double damageFlashDuration_ = 0.35;
};

} // namespace TacticalAI::Debug
