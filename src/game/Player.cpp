// ===========================================================================
//  src/game/Player.cpp
// ===========================================================================
#include "game/Player.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace TacticalAI::Game {

Player::Player(glm::vec2 position, PlayerConfig config)
    : Entity(EntityType::Player, "Player", position),
      config_(config) {
    setMaxHealth(config_.maxHealth);
    setHealth(config_.maxHealth);
    setRadius(config_.radius);

    // 【修复】关闭「朝向跟随速度方向」。
    //
    // 基类默认开启该行为（Entity::update 在 onUpdate 之后用速度向量重写
    // facing_），而玩家的设计是「面朝鼠标」（见 updateFacing）。
    // 只要移动速度 > 1，鼠标朝向就会在每步末尾被移动方向覆盖 ——
    // 结果是移动中开火时子弹沿移动方向飞出，翻滚时更是必然朝翻滚方向。
    // Enemy 早就关掉了它（Enemy.cpp 构造函数），Player 漏了这一行。
    setFaceVelocityDirection(false);

    addTag(EntityTag::Friendly);
    addTag(EntityTag::HighValue);   // 玩家总是 AI 的优先目标
}

float Player::currentMaxSpeed() const noexcept {
    // 冲刺提升上限，但瞄准时降低——鼓励「要么跑要么打」的取舍，
    // 这也是战术射击游戏的常见设计。
    float speed = sprinting_ ? config_.sprintSpeed : config_.walkSpeed;
    if (aiming_) {
        speed *= 0.55f;
    }
    return speed;
}

float Player::speed() const noexcept {
    return std::sqrt(velocity_.x * velocity_.x + velocity_.y * velocity_.y);
}

void Player::onUpdate(double fixedDelta) {
    const float delta = static_cast<float>(fixedDelta);

    // --- 0. 翻滚优先接管速度 ---
    //
    // 翻滚期间速度完全由技能曲线决定：既不参与"向意图速度逼近"，
    // **也不参与末尾的 clampVelocity**。
    //
    // 【为什么必须绕开速度裁剪】
    // 翻滚起手速度约 857 单位/秒（见 advanceRoll 里的曲线推导），
    // 而裁剪上限是冲刺速度的两倍 = 560。照常裁剪的话，起手那几步会被
    // 削掉一大截，"翻滚距离 = 150" 这条契约会静默失效——而且因为上限与
    // 冲刺挂钩，问题只在按住 Shift 时最明显，属于极难排查的那类偏差。
    if (rolling_) {
        advanceRoll(fixedDelta);
        updateFacing();
        return;
    }

    // --- 1. 求目标速度 ---
    const float maxSpeed = currentMaxSpeed();
    const glm::vec2 desiredVelocity = moveIntent_ * maxSpeed;

    // --- 2. 向目标速度逼近 ---
    // 有输入时用加速度插值（手感偏「有重量」）；
    // 无输入时用摩擦力减速（松键后滑行一小段）。
    const float inputMagnitude = std::sqrt(moveIntent_.x * moveIntent_.x
                                           + moveIntent_.y * moveIntent_.y);

    if (inputMagnitude > 0.01f) {
        const glm::vec2 toDesired = desiredVelocity - velocity_;
        const float distance = std::sqrt(toDesired.x * toDesired.x + toDesired.y * toDesired.y);

        // 本步最多能改变的速度量
        const float maxChange = config_.acceleration * delta;

        if (distance <= maxChange) {
            // 一步到位，避免在目标值附近来回抖动
            velocity_ = desiredVelocity;
        } else {
            velocity_ += (toDesired / distance) * maxChange;
        }
    } else {
        // 指数衰减：摩擦力越大，减速越干脆。
        // 用指数而非线性，手感更自然且不会出现突然停住的感觉。
        const float damping = std::max(0.0f, 1.0f - config_.friction * delta);
        velocity_ *= damping;
    }

    // --- 3. 朝向 ---
    updateFacing();

    // --- 4. 速度裁剪 ---
    //
    // 注意：这里**不再**处理开火请求。
    //
    // 旧版本在 onUpdate 里清掉 firePending_ 并 ++shotsFired_，但 onUpdate
    // 是在 World 的通用实体更新阶段被调用的，而那一阶段排在战斗结算之后。
    // 于是真正开枪的 World::processWeaponFire 先一步把请求消费掉了，
    // onUpdate 看到的永远是 false —— 结果是**玩家的 shots 计数恒为 0**，
    // HUD 上一直显示"射击 0"。开火与冷却统一收敛到 tryConsumeFireRequest，
    // 只有一个出口，就不会再出现这种"两处都在管同一件事"的错位。
    // 传入两倍上限作为「硬上限」，防止加速度插值偶尔超出 MAX
    //
    // 【修复】原写法传的是向量 {maxSpeed*2, maxSpeed*2}，而 clampVelocity
    // 内部取该向量的模长，实际上限变成 2.83 倍而非注释所说的 2 倍。
    // 改用标量重载，语义与注释一致。
    clampVelocity(maxSpeed * 2.0f);
}

void Player::updateFacing() noexcept {
    // 玩家面朝鼠标指向，而非移动方向——这是俯视射击的标准操作感。
    // 翻滚中也保持这条：滚向左侧、同时把枪口留在右侧是合理诉求
    // （只是翻滚期间开火被禁用，见 tryConsumeFireRequest）。
    const glm::vec2 toLook = lookAtPoint_ - position_;
    const float lookDistance = std::sqrt(toLook.x * toLook.x + toLook.y * toLook.y);
    if (lookDistance > 1e-4f) {
        facing_ = toLook / lookDistance;
    } else if (aimDirection_.x != 0.0f || aimDirection_.y != 0.0f) {
        setFacing(aimDirection_);
    }
}

// ---------------------------------------------------------------------------
//  翻滚
// ---------------------------------------------------------------------------
namespace {

/// 起手方向的最小模长。小于它视为"没有输入"。
///
/// moveAxis() 在没有按键时返回零向量，有按键时返回单位向量，所以这个阈值
/// 只需要挡住数值噪声，不需要区分"轻推摇杆"那种模拟量。
constexpr float kMinDirectionMagnitude = 1e-3f;

/// 速度曲线 v(τ) = k·(1 - c·τ) 里的收尾衰减系数 c。
///
/// 【为什么取 1 而不是更小的值】
/// c 决定收尾有多"软"。取 1 时 v(1) = 0，也就是翻滚**结束时速度恰好归零**，
/// 于是不会出现"翻滚已经结束，人还带着残余速度往前滑一小段"的现象——
/// 那段滑动会让实测位移比 150 多出几个单位，让"翻滚距离"变成一个
/// 只能靠肉眼估的量。
/// 同时曲线的归一化系数 k = 1/(1 - c/2) = 2，起手速度为 2 倍平均速度，
/// 正好符合翻滚"蹬地爆发"的观感。
constexpr float kRollSpeedDecay = 1.0f;

/// 归一化系数：使 ∫₀¹ k·(1 - c·τ) dτ = 1，从而总位移精确等于 rollDistance。
constexpr float kRollSpeedShape = 1.0f / (1.0f - kRollSpeedDecay * 0.5f);

/// 速度曲线的归一化原函数 S(τ)，满足 S(0) = 0、S(1) = 1。
///
/// 本步位移 = rollDistance × (S(τ1) - S(τ0))。
///
/// 【为什么用原函数而不是"瞬时速度 × 步长"】
/// 两者对一个线性曲线在数学上等价，但原函数的形式让"总位移恰好等于
/// rollDistance"变成一条**代数恒等式**：不管步长怎么分、固定步长被调成多少，
/// 只要 τ 从 0 走到 1，位移就必然是 rollDistance。
/// 用瞬时速度乘步长则要靠采样点选得恰好（比如取步中点）才精确，
/// 换个采样方式就悄悄偏掉——那正是"距离 150"最容易失效的地方。
float rollTravelledFraction(float tau) noexcept {
    return kRollSpeedShape * (tau - 0.5f * kRollSpeedDecay * tau * tau);
}

} // namespace

bool Player::tryStartRoll(double gameTime) noexcept {
    if (isDead() || rolling_) {
        return false;   // 翻滚中不能再次触发
    }

    if (gameTime - lastRollTime_ < static_cast<double>(config_.rollCooldown)) {
        return false;   // 冷却未过
    }

    // --- 方向解析 ---
    // 优先级：移动输入方向 > 鼠标方向 > 当前朝向。
    // 前两条是需求明确规定的；第三条是退化兜底。
    glm::vec2 direction = moveIntent_;
    float magnitude = std::sqrt(direction.x * direction.x + direction.y * direction.y);

    if (magnitude <= kMinDirectionMagnitude) {
        // 没有移动输入 → 朝鼠标方向
        direction = lookAtPoint_ - position_;
        magnitude = std::sqrt(direction.x * direction.x + direction.y * direction.y);
    }

    if (magnitude <= kMinDirectionMagnitude) {
        // 鼠标正好压在角色身上。这不是假想情况：把光标停在鼠标瞄准的角色
        // 中心是很自然的操作，不做兜底就会拿到零向量，翻滚原地不动，
        // 表现为"按了空格但什么都没发生"。
        direction = facing_;
        magnitude = std::sqrt(direction.x * direction.x + direction.y * direction.y);
    }

    if (magnitude <= kMinDirectionMagnitude) {
        // 连朝向都退化了（理论上到不了这里），给一个确定的默认值，
        // 保证函数的前置条件永远成立。
        direction = glm::vec2{1.0f, 0.0f};
        magnitude = 1.0f;
    }

    rolling_      = true;
    rollElapsed_  = 0.0;
    lastRollTime_ = gameTime;
    rollDirection_ = direction / magnitude;
    ++rollsStarted_;

    // 这里不设置 velocity_：翻滚期间的速度由 advanceRoll() 独占。
    // 两个地方都写速度公式，改一处就会悄悄不一致。
    return true;
}

void Player::advanceRoll(double fixedDelta) noexcept {
    const double duration = static_cast<double>(config_.rollDuration);

    if (rollElapsed_ >= duration) {
        // 兜底：正常路径下状态已在最后一步翻掉，到不了这里。
        // 留着是为了防止任何意外路径把玩家卡在"永久翻滚"。
        rolling_ = false;
        rollElapsed_ = duration;
        velocity_ = {0.0f, 0.0f};
        return;
    }

    // 本步能否走完整个翻滚。用一个小容差吸收浮点累加误差：
    // 21 步 × (1/60) 在 double 下未必正好等于 0.35，
    // 不容差的话最后会多出一步几乎为零的位移。
    constexpr double kEpsilon = 1e-9;
    const bool isFinalStep = (duration - rollElapsed_) <= fixedDelta + kEpsilon;

    const float tau0 = static_cast<float>(rollElapsed_ / duration);
    const float tau1 = isFinalStep
                           ? 1.0f
                           : static_cast<float>((rollElapsed_ + fixedDelta) / duration);

    // 曲线见 kRollSpeedDecay / rollTravelledFraction 的说明。
    // 沿原函数取差值，因此整段翻滚的位移必然等于 rollDistance。
    const float stepDistance = config_.rollDistance *
                               (rollTravelledFraction(tau1) - rollTravelledFraction(tau0));

    if (isFinalStep) {
        // 【最后一步要自己提交位移，不能留给基类】
        //
        // 基类的积分是 `position_ += velocity_ * fixedDelta`，发生在 onUpdate
        // **之后**。而这一步同时要把翻滚状态收掉。两条路都错：
        //   - 现在就清状态 → 速度也被清了 → 最后一段位移被吞掉（少走约 0.3 个单位）
        //   - 不清状态，等下一步再收 → isRolling() 比实际位移多真一帧，
        //     翻滚时长从 0.35s 变成 0.367s，同时那一帧还开不了枪
        //
        // 所以这里显式把剩余位移落到位置上，然后一次性收掉状态与速度。
        // 位置仍然会被后续的 resolveMovement/constrainEntity 约束，
        // 所以撞墙行为不受影响。
        position_ += rollDirection_ * stepDistance;
        velocity_ = {0.0f, 0.0f};
        rolling_ = false;
        rollElapsed_ = duration;
        return;
    }

    // 普通步：交给基类积分。用 步位移/步长 反推速度，
    // 保证"基类乘回去"之后恰好是本步应有的位移。
    velocity_ = rollDirection_ * (stepDistance / static_cast<float>(fixedDelta));
    rollElapsed_ += fixedDelta;
}

bool Player::isInvulnerable() const noexcept {
    // 无敌帧只覆盖翻滚的前一段，不是整个翻滚。
    // 见 PlayerConfig::rollInvulnTime 的说明：全程无敌会让翻滚退化成
    // 无代价位移，"一有动静就滚"成为最优解，AI 的射击压力就没了意义。
    return rolling_ &&
           rollElapsed_ < static_cast<double>(config_.rollInvulnTime);
}

float Player::rollProgress() const noexcept {
    if (!rolling_) {
        return 0.0f;
    }
    const float progress = static_cast<float>(rollElapsed_ / config_.rollDuration);
    return std::clamp(progress, 0.0f, 1.0f);
}

double Player::rollCooldownRemaining(double gameTime) const noexcept {
    const double remaining =
        static_cast<double>(config_.rollCooldown) - (gameTime - lastRollTime_);
    return (remaining > 0.0) ? remaining : 0.0;
}

bool Player::tryConsumeFireRequest(double gameTime) noexcept {
    const bool pending = firePending_;
    firePending_ = false;   // 无论成功与否都消费掉，见头文件里的说明

    if (!pending || isDead()) {
        return false;
    }

    // 翻滚期间不能射击。
    // 放在冷却检查之前，是因为它表达的是更强的约束：
    // 就算武器已经就绪，翻滚这个动作本身也占着手。
    if (rolling_) {
        return false;
    }

    if (gameTime - lastFireTime_ < static_cast<double>(config_.fireCooldown)) {
        return false;   // 还在冷却，这一枪被吞掉
    }

    lastFireTime_ = gameTime;
    ++shotsFired_;
    return true;
}

double Player::fireCooldownRemaining(double gameTime) const noexcept {
    const double remaining =
        static_cast<double>(config_.fireCooldown) - (gameTime - lastFireTime_);
    return (remaining > 0.0) ? remaining : 0.0;
}

std::string Player::debugString() const {
    char buffer[384];
    std::snprintf(buffer, sizeof(buffer),
                  "#%u Player pos=(%.0f,%.0f) speed=%.0f max=%.0f hp=%.0f/%.0f %s%s%s%s shots=%d dmg=%d rolls=%d",
                  id(),
                  static_cast<double>(position_.x), static_cast<double>(position_.y),
                  static_cast<double>(speed()), static_cast<double>(currentMaxSpeed()),
                  static_cast<double>(health()), static_cast<double>(maxHealth()),
                  sprinting_ ? "[SPRINT] " : "",
                  aiming_ ? "[AIM] " : "",
                  rolling_ ? "[ROLL] " : "",
                  isDead() ? "[DEAD]" : "",
                  shotsFired_, damageDealt_, rollsStarted_);
    return std::string(buffer);
}

} // namespace TacticalAI::Game
