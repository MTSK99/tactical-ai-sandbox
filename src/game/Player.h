// ===========================================================================
//  src/game/Player.h
//
//  玩家实体。
//
//  玩家是沙盒里的「对照组」：它的输入由键盘直接驱动，没有 AI。
//  这让设计者可以直观感受「AI 对手是否表现得像个人」——
//  把玩家当成基准，AI 太快/太慢/太准一眼就能看出来。
// ===========================================================================
#pragma once

#include <string>

#include "game/Entity.h"

namespace TacticalAI::Game {

/// 玩家移动参数。
struct PlayerConfig {
    float walkSpeed    = 160.0f;   ///< 世界单位 / 秒
    float sprintSpeed  = 280.0f;
    float acceleration = 1200.0f;  ///< 世界单位 / 秒^2，越大手感越硬
    float friction     = 10.0f;    ///< 无输入时的减速系数
    float maxHealth    = 100.0f;
    float radius       = 12.0f;

    /// 两次开火之间的最小间隔（秒）。
    ///
    /// 【为什么玩家也需要冷却】
    /// 玩家的输入走的是"按下即触发"（consumeActionPressed），它真正的职责
    /// 是给**子弹系统**兜底：没有这个上限，一次异常的输入重复（按键粘连、
    /// 脚本化输入、将来加的全自动模式）就能在一步里塞进几十颗子弹，
    /// 屏幕上会出现一片弹幕，AI 也无从躲避。
    ///
    /// 【数值修正：0.15s → 0.10s】
    /// 这里原本写着"0.15s ≈ 每秒 6.7 发，比人类极限（约 8 次/秒）略快，
    /// 感觉不到但拦得住"——**算错了方向**：6.7 比 8 慢，所以快速点击
    /// （8~10 次/秒）会被这个上限挡住，玩家看到的是"点了却没开枪"，
    /// 冷却条明明在走、枪就是不出。这与注释声明的意图正好相反。
    ///
    /// 改成 0.10s ≈ 每秒 10 发：真正落在人类点击极限之上，恢复它
    /// "兜底而非限速"的定位。理论射速上限随之从 6.7 升到 10 发/秒，
    /// 但实际射速仍然由玩家的点击速度决定 —— 上限只对异常输入生效，
    /// 这正是它存在的意义。
    float fireCooldown = 0.10f;

    // -- 翻滚（Dodge Roll）---------------------------------------------------
    //
    // 翻滚存在的意义不只是"位移技能"：这是沙盒里唯一一个能让玩家**主动
    // 改写 AI 命中判定**的手段。在它之前，玩家面对子弹只有"躲到掩体后面"
    // 和"跑开"两种选择，而这两种都无法应对"子弹已经在飞、自己又在空旷处"。
    // 有了翻滚，玩家多了一个时间窗口内的确定性答案，AI 的预判射击也第一次
    // 有了"打得准但可能被闪掉"的博弈——这才是这个功能值得存在的理由。
    //
    // 这几个参数是**成组**的，单独调一个很容易破坏手感：
    //   位移 150 / 时长 0.35 = 平均 429 单位/秒，比冲刺(280)快但只持续 0.35s。
    //   也就是说翻滚是"短距爆发"，不能当移动手段一直用——这正是冷却 0.8s
    //   要保证的事。

    /// 翻滚总位移（世界单位，等价于像素）。翻滚距离是**可断言的契约**：
    /// 步进速度经过整形后积分出来必须仍然精确等于它。
    float rollDistance = 150.0f;

    /// 翻滚持续时间（秒）。
    float rollDuration = 0.35f;

    /// 两次翻滚之间的最小间隔（秒）。
    ///
    /// 【从"起手"还是从"结束"计时？】
    /// 这里从**起手**计时，也就是"两次翻滚的起手间隔 ≥ 0.8s"。
    /// 于是翻滚结束（0.35s）之后还要再等 0.45s 才能再滚。
    /// 选这个读法的原因：动作游戏里"冷却"通常指"你按下这个键的频率上限"，
    /// 玩家心里数的是按键间隔，而不是"上一个动作播完"的时间点。
    float rollCooldown = 0.80f;

    /// 翻滚开始后的无敌时长（秒）。
    ///
    /// 只覆盖前 0.25s（约 71% 的时长），**不是整个翻滚**。
    /// 这是刻意的：全程序无敌会让"翻滚"退化成一段无代价的位移，玩家最优解
    /// 变成"一有动静就滚"，AI 的射击压力形同虚设。留出最后 0.1s 的破绽，
    /// 才会出现"滚早了照样吃子弹"的时机判断。
    float rollInvulnTime = 0.25f;
};

/// 玩家。
class Player final : public Entity {
public:
    explicit Player(glm::vec2 position, PlayerConfig config = {});

    // -- 输入 ----------------------------------------------------------------
    /// 设置本步的移动意图（归一化方向，模长 <= 1）。
    /// 由游戏层从 Input 系统读取后传入——玩家类不直接依赖 Input，
    /// 这样它可以在测试里被脚本化的输入驱动。
    void setMoveIntent(glm::vec2 intent) noexcept { moveIntent_ = intent; }

    /// 设置是否冲刺。
    void setSprinting(bool sprinting) noexcept { sprinting_ = sprinting; }

    /// 设置瞄准方向（世界坐标系的方向向量）。
    void setAimDirection(glm::vec2 direction) noexcept { aimDirection_ = direction; }

    /// 请求开火（单次触发，会被移动逻辑消费）。
    void requestFire() noexcept { firePending_ = true; }

    /// 设置玩家在屏幕上指向的世界坐标点（用于朝向）。
    void setLookAtPoint(glm::vec2 point) noexcept { lookAtPoint_ = point; }

    // -- 状态 ----------------------------------------------------------------

    [[nodiscard]] bool isSprinting() const noexcept { return sprinting_; }
    [[nodiscard]] bool aiming() const noexcept { return aiming_; }
    void setAiming(bool aiming) noexcept { aiming_ = aiming; }

    [[nodiscard]] float speed() const noexcept;
    [[nodiscard]] float currentMaxSpeed() const noexcept;

    /// 尝试消费本步的开火请求。**这是玩家开火的唯一出口。**
    ///
    /// 只有当「有请求」且「冷却已过」时才返回 true。无论成功与否都会
    /// 清掉待处理的请求——冷却期内的一次点击不排队，否则连点几下之后
    /// 松开手还会"欠"出一串子弹，手感会变得莫名其妙。
    ///
    /// @param gameTime 当前游戏时间（冷却判定必须用游戏时间而不是真实时间，
    ///                 否则慢动作/暂停时冷却会以错误的速度前进）
    [[nodiscard]] bool tryConsumeFireRequest(double gameTime) noexcept;

    /// 距离下一次可以开火还剩多少秒（0 表示已就绪）。HUD 用来画冷却条。
    [[nodiscard]] double fireCooldownRemaining(double gameTime) const noexcept;

    /// 开火间隔（秒）。
    [[nodiscard]] float fireCooldown() const noexcept { return config_.fireCooldown; }

    [[nodiscard]] int shotsFired() const noexcept { return shotsFired_; }
    /// 累计造成的伤害。内部按浮点累加（与 Enemy 一致），
    /// 对外仍以整数呈现（HUD 只需要整数）。
    ///
    /// 【修复】此前内部是 int，而调用方用 static_cast<int>(dealt) 截断，
    /// 每次命中都向下取整（18.7 → 18），多次命中后累计值系统性偏低，
    /// 且与 Enemy 的 float 口径不一致。
    [[nodiscard]] int damageDealt() const noexcept {
        return static_cast<int>(damageDealt_ + 0.5f);
    }
    void addDamageDealt(float amount) noexcept { damageDealt_ += amount; }

    // -- 翻滚 ----------------------------------------------------------------

    /// 尝试起手一次翻滚。
    ///
    /// 方向解析顺序（这就是"翻滚方向"这条需求的落点）：
    ///   1. 有移动输入 → 朝**当前移动方向**
    ///   2. 没有移动输入 → 朝**鼠标方向**（lookAtPoint 指向的世界坐标）
    ///   3. 鼠标正好压在角色身上（方向退化）→ 朝当前朝向
    /// 第 3 条不是画蛇添足：玩家把鼠标停在角色中心是常见操作，
    /// 不做兜底就会拿到一个零向量，翻滚原地不动，看起来像"按键失灵"。
    ///
    /// @param gameTime 当前游戏时间（冷却判定用游戏时间，慢动作/暂停时才不会跑偏）
    /// @return 是否成功起手；已在翻滚中或冷却未过时返回 false
    ///
    /// 【为什么拒绝时**不排队**】
    /// 与开火一致：失败就是失败。若把请求存起来等冷却结束再执行，玩家在
    /// 冷却里连按几下，之后会"欠"出一串翻滚，完全无法预测自己在哪一刻会滚。
    [[nodiscard]] bool tryStartRoll(double gameTime) noexcept;

    /// 是否正在翻滚。
    [[nodiscard]] bool isRolling() const noexcept { return rolling_; }

    /// 是否处于无敌帧。覆写 Entity 的虚函数，由 World::applyDamage 查询。
    [[nodiscard]] bool isInvulnerable() const noexcept override;

    /// 翻滚进度 0..1（渲染残影与护盾环用）。
    [[nodiscard]] float rollProgress() const noexcept;

    /// 本帧翻滚方向（单位向量）。非翻滚状态下是上一次的翻滚方向。
    [[nodiscard]] glm::vec2 rollDirection() const noexcept { return rollDirection_; }

    /// 距离下一次可以翻滚还剩多少秒（0 表示已就绪）。
    [[nodiscard]] double rollCooldownRemaining(double gameTime) const noexcept;

    [[nodiscard]] int rollsStarted() const noexcept { return rollsStarted_; }
    [[nodiscard]] const PlayerConfig& config() const noexcept { return config_; }
    [[nodiscard]] std::string debugString() const override;

protected:
    void onUpdate(double fixedDelta) override;

private:
    /// 让朝对准鼠标（lookAtPoint_），退化时用 aimDirection_。
    /// 常规移动与翻滚两条路径都要用，因此抽出来避免两处各写一遍。
    void updateFacing() noexcept;

    /// 推进翻滚状态与速度。只在 rolling_ 为真时调用。
    void advanceRoll(double fixedDelta) noexcept;

    PlayerConfig config_;

    glm::vec2 moveIntent_{0.0f, 0.0f};
    glm::vec2 aimDirection_{1.0f, 0.0f};
    glm::vec2 lookAtPoint_{0.0f, 0.0f};

    bool sprinting_   = false;
    bool aiming_      = false;
    bool firePending_ = false;

    /// 上次成功开火的游戏时间。初值取一个极大的负数，让第一枪永远不受冷却限制。
    double lastFireTime_ = -1e9;

    /// 上次成功起手翻滚的游戏时间。同样用极大负数让第一次翻滚不受冷却限制。
    double lastRollTime_ = -1e9;

    /// 本帧翻滚方向。
    glm::vec2 rollDirection_{1.0f, 0.0f};

    /// 是否正在翻滚。
    bool rolling_ = false;

    /// 翻滚已进行的秒数。
    ///
    /// 【为什么用固定步长累加，而不是记录起手时刻再和 gameTime 相减】
    /// 伤害判定发生在 World 的"子弹推进"阶段，而那一步排在"实体更新"之前。
    /// 累加式的时间戳恰好与之对齐：本步伤害判定读到的 rollElapsed_ 正是
    /// "本步开始时已经滚了多久"，值是对的，不存在一步偏差。
    /// 若改成与 gameTime 相减，反而要在两个阶段之间传递时间戳。
    double rollElapsed_ = 0.0;

    int shotsFired_   = 0;
    float damageDealt_ = 0.0f;
    int rollsStarted_ = 0;
};

} // namespace TacticalAI::Game
