// ===========================================================================
//  src/game/AutoPilot.h
//
//  自动演示驾驶员（--demo）。
//
//  【为什么它属于 game 层，而不是 debug 层】
//  它操作的是**玩家角色本身**（和真人按键走同一条 API：setMoveIntent /
//  setLookAtPoint / requestFire / tryStartRoll），因此它是一条真实的输入
//  来源，而不是"演示特供逻辑"。放在 game 层意味着它可以被单元测试驱动，
//  也保证演示里看到的行为与真人操作完全一致。
//
//  【为什么不用随机数】
//  整个引擎建立在"固定步长 + 确定性逻辑"上：同样的一串输入必然产生同样的
//  一局。演示驾驶员如果掺进随机数，这段性质就没了（录制无法复现）。
//  所以这里的"变化"全部来自确定性的计数与几何，而不是 RNG。
// ===========================================================================
#pragma once

#include <cstdint>

#include <glm/glm.hpp>

namespace TacticalAI::Game {

class World;
class Player;

/// 演示用自动驾驶。策略刻意保持简单可读（走位 + 瞄准 + 开火 + 躲弹），
/// 目标是"看起来像个在打得有来有回的玩家"，而不是"打得最好"。
class AutoPilot {
public:
    /// 每个固定逻辑步调用一次。内部会写满玩家本步的全部意图。
    void update(World& world, Player& player, double gameTime, double fixedDelta);

    /// 当前走位目标点（调试绘制用；没有目标时返回玩家自身位置）。
    [[nodiscard]] glm::vec2 waypoint() const noexcept { return waypoint_; }

private:
    /// 理想交战距离：比玩家射程(520)近、比敌人射程(400)略近，
    /// 这样"边打边走"时既在输出、又处在敌人的有效射程边缘。
    static constexpr float kPreferredRange = 300.0f;

    /// 太近就后撤的距离。留出余量，避免贴脸被近战式的连续命中打穿。
    static constexpr float kKeepAwayRange = 200.0f;

    /// 换向节流：横向走位保持同一个方向一段时间，看起来才像有意图的走位。
    static constexpr double kStrafeSwitchInterval = 1.6;

    /// 躲弹判定半径：来袭子弹进入这个范围就翻滚。
    static constexpr float kDodgeRadius = 150.0f;

    glm::vec2 waypoint_{0.0f, 0.0f};
    glm::vec2 strafeDirection_{1.0f, 0.0f};
    double    lastStrafeSwitch_ = -1e9;
    int       waypointIndex_    = 0;
    bool      initialized_      = false;
};

} // namespace TacticalAI::Game
