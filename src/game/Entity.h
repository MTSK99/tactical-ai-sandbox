// ===========================================================================
//  src/game/Entity.h
//
//  游戏实体基类。
//
//  设计取舍：这里刻意不做 ECS（实体组件系统）。
//  战术沙盒的实体数量在几十到几百量级，虚函数 + 组合已经完全够用，
//  而 ECS 会让「AI 调试可视化」这种需要按名字查实体、按类型遍历的场景变得笨拙。
//  如果将来实体数上万，再把主体数据搬到 SoA 结构里也不迟。
//
//  实体通过 unique_ptr 由 World 拥有，外部只持 uint32_t ID，
//  避免悬垂指针（这是长时间运行的游戏里最常见的崩溃原因）。
// ===========================================================================
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "glm/vec2.hpp"

namespace TacticalAI::Game {

/// 实体类型。用位掩码便于做「只对敌人做感知」这类过滤。
enum class EntityType : uint32_t {
    None      = 0,
    Player    = 1u << 0,
    Enemy     = 1u << 1,
    Obstacle  = 1u << 2,
    Cover     = 1u << 3,
    Marker    = 1u << 4,   ///< 纯调试标记，不参与游戏逻辑
    Projectile= 1u << 5,   ///< 飞行中的子弹（见 Projectile.h）

    /// 会被 AI 感知 / 选为目标的实体。刻意不含 Projectile：
    /// 子弹是短暂的物理对象，不是"敌人"，把 AI 的注意力吸引到子弹上
    /// 会让敌人对着飞过去的流弹转身——这是纯粹的 Bug，不是智能。
    AnyCombatant = static_cast<uint32_t>(Player) | static_cast<uint32_t>(Enemy)
};

/// 实体标签：比类型更细的语义分组，AI 用它筛选目标。
enum class EntityTag : uint32_t {
    None      = 0,
    Hostile   = 1u << 0,
    Friendly  = 1u << 1,
    Destructible = 1u << 2,
    HighValue = 1u << 3,   ///< 高价值目标（AI 优先攻击）
    Generator = 1u << 4    ///< 会持续产生威胁的建筑
};

[[nodiscard]] const char* toString(EntityType type) noexcept;

/// 实体基类。
///
/// 生命周期：构造 -> 每固定步 update() -> 销毁。
/// 渲染由 debug 层读取实体状态绘制，实体本身不持有任何图形资源——
/// 这让「AI 逻辑」与「显示」彻底分离，也让单元测试可以无窗口运行。
class Entity {
public:
    /// 全局唯一的 ID 生成器。用函数的静态局部变量避免全局构造顺序问题。
    [[nodiscard]] static uint32_t nextId() noexcept;

    Entity(EntityType type, std::string name, glm::vec2 position);
    virtual ~Entity() = default;

    Entity(const Entity&)            = delete;
    Entity& operator=(const Entity&) = delete;

    // -- 标识 ----------------------------------------------------------------

    [[nodiscard]] uint32_t id() const noexcept { return id_; }
    [[nodiscard]] EntityType type() const noexcept { return type_; }
    [[nodiscard]] const std::string& name() const noexcept { return name_; }
    void setName(std::string name) { name_ = std::move(name); }

    [[nodiscard]] bool isType(EntityType type) const noexcept {
        return (static_cast<uint32_t>(type_) & static_cast<uint32_t>(type)) != 0;
    }
    [[nodiscard]] bool isAlive() const noexcept { return alive_; }

    /// 立即杀死实体（跳过伤害结算，例如调试面板的"全部击杀"）。
    ///
    /// 同时清零血量：否则面板会显示 `hp=70/70 [DEAD]`——
    /// "活着"与"还有血"本就是同一个事实的两种问法，不该给出矛盾的答案。
    void kill() noexcept {
        alive_  = false;
        health_ = 0.0f;
    }

    // -- 标签 ----------------------------------------------------------------

    void addTag(EntityTag tag) noexcept {
        tags_ |= static_cast<uint32_t>(tag);
    }
    void removeTag(EntityTag tag) noexcept {
        tags_ &= ~static_cast<uint32_t>(tag);
    }
    [[nodiscard]] bool hasTag(EntityTag tag) const noexcept {
        return (tags_ & static_cast<uint32_t>(tag)) != 0;
    }

    void setTags(uint32_t tags) noexcept { tags_ = tags; }
    [[nodiscard]] uint32_t tags() const noexcept { return tags_; }

    // -- 变换 ----------------------------------------------------------------

    [[nodiscard]] glm::vec2 position() const noexcept { return position_; }
    void setPosition(glm::vec2 position) noexcept { position_ = position; }
    void translate(glm::vec2 delta) noexcept { position_ += delta; }

    /// 朝向（单位向量）。
    [[nodiscard]] glm::vec2 facing() const noexcept { return facing_; }
    void setFacing(glm::vec2 facing) noexcept;

    /// 从当前位置指向某点的单位朝向。
    void faceTowards(glm::vec2 target) noexcept;

    /// 速度（世界单位 / 秒）。
    [[nodiscard]] glm::vec2 velocity() const noexcept { return velocity_; }
    void setVelocity(glm::vec2 velocity) noexcept { velocity_ = velocity; }

    /// 碰撞/占据半径。
    [[nodiscard]] float radius() const noexcept { return radius_; }
    void setRadius(float radius) noexcept { radius_ = std::max(radius, 0.5f); }

    // -- 属性 ----------------------------------------------------------------

    [[nodiscard]] float health() const noexcept { return health_; }
    [[nodiscard]] float maxHealth() const noexcept { return maxHealth_; }
    [[nodiscard]] float healthRatio() const noexcept {
        return (maxHealth_ > 0.0f) ? (health_ / maxHealth_) : 0.0f;
    }
    void setHealth(float health) noexcept;
    void setMaxHealth(float maxHealth) noexcept;
    /// 施加伤害。返回实际扣除的血量（可能因已死亡而为 0）。
    float applyDamage(float amount) noexcept;
    void heal(float amount) noexcept;

    [[nodiscard]] bool isDead() const noexcept { return health_ <= 0.0f || !alive_; }

    /// 本步是否处于无敌状态（翻滚的无敌帧等）。默认 false。
    ///
    /// 【为什么放在 Entity 这一层，而不是在 Player 里单独判断】
    /// 伤害的入口是 World::applyDamage，它拿到的是 `Entity*`。
    /// 把"能不能被打中"做成实体自己的属性，World 就不需要认识 Player 这个
    /// 具体类型，将来给敌人加"霸体/冲刺无敌"也是覆写同一个函数。
    /// 这条查询对所有伤害来源都成立（子弹、将来的近战、陷阱、毒圈），
    /// 不必在每个来源里各写一次判断——漏掉一处就是"无敌有时不生效"。
    [[nodiscard]] virtual bool isInvulnerable() const noexcept { return false; }

    // -- 主更新 --------------------------------------------------------------

    /// 固定步长更新。派生类重写 onUpdate。
    /// 基类负责通用的积分（位置 += 速度 * dt）。
    void update(double fixedDelta);

    /// 本步是否移动过（AI 用它判断「到位」还是「还在走」）。
    [[nodiscard]] bool movedThisStep() const noexcept { return movedThisStep_; }

    /// 本步实际位移距离。比 velocity 更能反映真实机动情况。
    [[nodiscard]] float distanceMovedThisStep() const noexcept { return distanceMovedThisStep_; }

    /// 一行状态摘要，供调试面板显示。
    [[nodiscard]] virtual std::string debugString() const;

protected:
    /// 派生类重写：实现自己的每步逻辑。
    virtual void onUpdate(double fixedDelta) { (void)fixedDelta; }

    /// 限制速度到最大值，并消除极小的残余速度（防止实体永远在微微漂移）。
    void clampVelocity(glm::vec2 maxVelocity, float epsilon = 0.5f) noexcept;

    /// 按**标量**速度上限裁剪（推荐用这个）。
    ///
    /// 存在的原因：向量重载内部取的是 maxVelocity 的模长，于是
    /// clampVelocity({s, s}) 的实际上限是 s·√2 —— 本想表达"上限 s"，
    /// 却因为传了分量形式而把上限静默放大 41%。
    void clampVelocity(float maxSpeed) noexcept {
        clampVelocity(glm::vec2{maxSpeed, 0.0f});
    }

    /// 是否让朝向自动跟随速度方向。
    ///
    /// 默认开启：多数实体「面朝前方走」比「横着平移」自然得多。
    ///
    /// 但 Enemy 必须关掉它。原因见 base update() 里的注释：
    /// 交战中的敌人需要一边侧移一边把枪口锁在目标上，而"朝向 = 速度方向"
    /// 会把枪口强行拽回移动方向，导致它永远打偏。
    void setFaceVelocityDirection(bool enabled) noexcept {
        faceVelocityDirection_ = enabled;
    }

    uint32_t    id_ = 0;
    EntityType  type_ = EntityType::None;
    std::string name_;
    uint32_t    tags_ = 0;

    glm::vec2 position_{0.0f, 0.0f};
    glm::vec2 facing_{1.0f, 0.0f};
    glm::vec2 velocity_{0.0f, 0.0f};
    float     radius_ = 10.0f;

    float health_    = 100.0f;
    float maxHealth_ = 100.0f;

    bool  alive_ = true;
    bool  movedThisStep_ = false;
    float distanceMovedThisStep_ = 0.0f;

    /// 朝向是否跟随速度方向。见 setFaceVelocityDirection()。
    bool  faceVelocityDirection_ = true;
};

/// 静态障碍物。只占据空间，不参与 AI 逻辑，但会阻挡视线与寻路。
class Obstacle final : public Entity {
public:
    Obstacle(std::string name, glm::vec2 position, glm::vec2 size, bool provideCover = false);

    [[nodiscard]] glm::vec2 size() const noexcept { return size_; }
    void setSize(glm::vec2 size) noexcept;

    /// 是否可作为掩体（EQS 的掩护度测试会查询这个）。
    [[nodiscard]] bool providesCover() const noexcept { return providesCover_; }
    void setProvidesCover(bool value) noexcept { providesCover_ = value; }

    /// 点是否落在障碍物内（用于视线检测的粗略判定）。
    [[nodiscard]] bool containsPoint(glm::vec2 point) const noexcept;

    /// 矩形的四个角（调试绘制用）。
    [[nodiscard]] std::vector<glm::vec2> corners() const;

    [[nodiscard]] std::string debugString() const override;

private:
    glm::vec2 size_{32.0f, 32.0f};
    bool providesCover_ = false;
};

} // namespace TacticalAI::Game
