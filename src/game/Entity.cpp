// ===========================================================================
//  src/game/Entity.cpp
// ===========================================================================
#include "game/Entity.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace TacticalAI::Game {
namespace {

inline glm::vec2 normalizeOr(glm::vec2 v, glm::vec2 fallback) noexcept {
    const float length = std::sqrt(v.x * v.x + v.y * v.y);
    if (length < 1e-6f) {
        return fallback;
    }
    return {v.x / length, v.y / length};
}

} // namespace

// ---------------------------------------------------------------------------
// ID 分配
// ---------------------------------------------------------------------------
uint32_t Entity::nextId() noexcept {
    // 从 1 开始：0 被约定为「空 ID」，这样 AI 层可以用 0 表示「无目标」。
    static uint32_t counter = 1;
    return counter++;
}

const char* toString(EntityType type) noexcept {
    switch (type) {
        case EntityType::None:     return "None";
        case EntityType::Player:   return "Player";
        case EntityType::Enemy:    return "Enemy";
        case EntityType::Obstacle: return "Obstacle";
        case EntityType::Cover:    return "Cover";
        case EntityType::Marker:   return "Marker";
        case EntityType::Projectile: return "Projectile";
        default:                   return "Mixed";
    }
}

// ---------------------------------------------------------------------------
// 构造
// ---------------------------------------------------------------------------
Entity::Entity(EntityType type, std::string name, glm::vec2 position)
    : id_(nextId()),
      type_(type),
      name_(std::move(name)),
      position_(position) {}

// ---------------------------------------------------------------------------
// 变换
// ---------------------------------------------------------------------------
void Entity::setFacing(glm::vec2 facing) noexcept {
    facing_ = normalizeOr(facing, facing_);
}

void Entity::faceTowards(glm::vec2 target) noexcept {
    facing_ = normalizeOr(target - position_, facing_);
}

// ---------------------------------------------------------------------------
// 属性
// ---------------------------------------------------------------------------
void Entity::setHealth(float health) noexcept {
    health_ = std::clamp(health, 0.0f, maxHealth_);
    if (health_ <= 0.0f) {
        alive_ = false;
    }
}

void Entity::setMaxHealth(float maxHealth) noexcept {
    maxHealth_ = std::max(maxHealth, 1.0f);
    health_ = std::min(health_, maxHealth_);
}

float Entity::applyDamage(float amount) noexcept {
    if (amount <= 0.0f || isDead()) {
        return 0.0f;
    }

    const float before = health_;
    health_ = std::max(0.0f, health_ - amount);

    if (health_ <= 0.0f) {
        alive_ = false;
    }
    return before - health_;
}

void Entity::heal(float amount) noexcept {
    if (amount <= 0.0f || !alive_) {
        return;
    }
    health_ = std::min(maxHealth_, health_ + amount);
}

// ---------------------------------------------------------------------------
// 更新
// ---------------------------------------------------------------------------
void Entity::update(double fixedDelta) {
    if (!alive_) {
        // 死亡实体不再移动，但保留在世界上（尸体/残骸），
        // 由 World 决定何时回收。
        velocity_ = {0.0f, 0.0f};
        movedThisStep_ = false;
        distanceMovedThisStep_ = 0.0f;
        return;
    }

    const glm::vec2 before = position_;

    // 派生类逻辑先行：它可以修改 velocity（例如 AI 决定加速冲刺）
    onUpdate(fixedDelta);

    // 位置积分用固定步长，保证确定性
    position_ += velocity_ * static_cast<float>(fixedDelta);

    // 朝向跟随速度方向（除非实体自己设置了朝向）。
    // 这让移动看起来是「面朝前方走」而不是「横着平移」。
    //
    // 【为什么会有一个开关】
    // Enemy 需要关掉它。交战中敌人要一边侧移一边把枪口锁在目标上，
    // 而"朝向 = 速度方向"会在这一步强行把枪口拽回移动方向——瞄准逻辑
    // 在行为树里刚算好的朝向，会在这行被速度向量覆盖掉。
    //
    // 在"瞬时射线命中"的旧模型里这个问题看不出来（伤害在扣扳机时就结算了，
    // 朝向只影响画面）。改成实体子弹之后，子弹沿着 facing 飞出去，
    // 朝向被覆盖就等价于"AI 永远打偏"。
    const float speed = std::sqrt(velocity_.x * velocity_.x + velocity_.y * velocity_.y);
    if (faceVelocityDirection_ && speed > 1.0f) {
        facing_ = velocity_ / speed;
    }

    const glm::vec2 delta = position_ - before;
    distanceMovedThisStep_ = std::sqrt(delta.x * delta.x + delta.y * delta.y);
    movedThisStep_ = distanceMovedThisStep_ > 0.01f;
}

void Entity::clampVelocity(glm::vec2 maxVelocity, float epsilon) noexcept {
    const float speed = std::sqrt(velocity_.x * velocity_.x + velocity_.y * velocity_.y);

    // 微小速度直接归零：否则浮点残余会让实体永远「抖动」，
    // 在调试可视化里表现为实体位置的小数点后一直变化。
    if (speed < epsilon) {
        velocity_ = {0.0f, 0.0f};
        return;
    }

    const float maxSpeed = std::sqrt(maxVelocity.x * maxVelocity.x
                                     + maxVelocity.y * maxVelocity.y);
    if (maxSpeed > 1e-6f && speed > maxSpeed) {
        velocity_ = velocity_ * (maxSpeed / speed);
    }
}

// ---------------------------------------------------------------------------
// 调试
// ---------------------------------------------------------------------------
std::string Entity::debugString() const {
    char buffer[256];
    std::snprintf(buffer, sizeof(buffer),
                  "#%u %s \"%s\" pos=(%.0f,%.0f) hp=%.0f/%.0f %s",
                  id_, toString(type_), name_.c_str(),
                  static_cast<double>(position_.x), static_cast<double>(position_.y),
                  static_cast<double>(health_), static_cast<double>(maxHealth_),
                  isDead() ? "[DEAD]" : "");
    return std::string(buffer);
}

// ===========================================================================
//  Obstacle
// ===========================================================================
Obstacle::Obstacle(std::string name, glm::vec2 position, glm::vec2 size, bool provideCover)
    : Entity(EntityType::Obstacle, std::move(name), position),
      size_(size),
      providesCover_(provideCover) {
    // 障碍物的碰撞半径取对角线的一半：让「圆形近似」在视线检测里偏保守，
    // 即宁可判定为被挡住，不要判定为看得见。
    setRadius(std::sqrt(size.x * size.x + size.y * size.y) * 0.5f);
    setMaxHealth(1.0f);
    addTag(provideCover ? EntityTag::Destructible : EntityTag::None);
}

void Obstacle::setSize(glm::vec2 size) noexcept {
    size_ = {std::max(size.x, 1.0f), std::max(size.y, 1.0f)};
    setRadius(std::sqrt(size_.x * size_.x + size_.y * size_.y) * 0.5f);
}

bool Obstacle::containsPoint(glm::vec2 point) const noexcept {
    // 轴对齐矩形的包含判定。
    //
    // 【注释更正】size_ 是**全尺寸**（宽/高），不是半径：
    // 下面取 size_ * 0.5 作为半宽/半高，与 constrainEntity、
    // generateDemoObstacles 的用法一致。旧注释写反了，会误导后来者。
    const float halfX = size_.x * 0.5f;
    const float halfY = size_.y * 0.5f;

    return point.x >= position_.x - halfX && point.x <= position_.x + halfX
        && point.y >= position_.y - halfY && point.y <= position_.y + halfY;
}

std::vector<glm::vec2> Obstacle::corners() const {
    const float halfX = size_.x * 0.5f;
    const float halfY = size_.y * 0.5f;

    return {
        {position_.x - halfX, position_.y - halfY},
        {position_.x + halfX, position_.y - halfY},
        {position_.x + halfX, position_.y + halfY},
        {position_.x - halfX, position_.y + halfY},
    };
}

std::string Obstacle::debugString() const {
    char buffer[256];
    std::snprintf(buffer, sizeof(buffer),
                  "#%u Obstacle \"%s\" pos=(%.0f,%.0f) size=(%.0f,%.0f)%s",
                  id_, name_.c_str(),
                  static_cast<double>(position_.x), static_cast<double>(position_.y),
                  static_cast<double>(size_.x), static_cast<double>(size_.y),
                  providesCover_ ? " [COVER]" : "");
    return std::string(buffer);
}

} // namespace TacticalAI::Game
