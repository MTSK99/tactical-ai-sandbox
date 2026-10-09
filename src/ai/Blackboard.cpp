// ===========================================================================
//  src/ai/Blackboard.cpp
// ===========================================================================
#include "ai/Blackboard.h"

#include <algorithm>
#include <cstdio>
#include <functional>
#include <sstream>

namespace TacticalAI::AI {

// ---------------------------------------------------------------------------
// BlackboardKey
// ---------------------------------------------------------------------------
uint64_t BlackboardKey::hash() const noexcept {
    // FNV-1a：实现简单、分布均匀，且不像 std::hash 那样在不同标准库实现间有差异。
    // 确定性对「可复现的 AI 调试」很重要。
    constexpr uint64_t kOffsetBasis = 14695981039346656037ULL;
    constexpr uint64_t kPrime       = 1099511628211ULL;

    uint64_t hash = kOffsetBasis;
    for (const char character : name_) {
        hash ^= static_cast<uint64_t>(static_cast<unsigned char>(character));
        hash *= kPrime;
    }
    return hash;
}

// ---------------------------------------------------------------------------
// 基础操作
// ---------------------------------------------------------------------------
bool Blackboard::contains(std::string_view key) const {
    return entries_.find(std::string(key)) != entries_.end();
}

bool Blackboard::erase(std::string_view key) {
    const auto it = entries_.find(std::string(key));
    if (it == entries_.end()) {
        return false;
    }
    entries_.erase(it);
    hasDirtyEntries_ = true;
    return true;
}

void Blackboard::clear() {
    entries_.clear();
    hasDirtyEntries_ = true;
    // totalWrites_ 刻意不清零：它是累计统计，用于发现「每帧狂写」的节点，
    // 跨 clear 保留才能反映真实频率。
}

// ---------------------------------------------------------------------------
// 元信息
// ---------------------------------------------------------------------------
uint64_t Blackboard::version(std::string_view key) const {
    const auto it = entries_.find(std::string(key));
    return (it == entries_.end()) ? 0 : it->second.version;
}

const BlackboardEntry* Blackboard::entry(std::string_view key) const {
    const auto it = entries_.find(std::string(key));
    return (it == entries_.end()) ? nullptr : &it->second;
}

std::vector<std::string> Blackboard::keys() const {
    std::vector<std::string> result;
    result.reserve(entries_.size());
    for (const auto& [key, entry] : entries_) {
        (void)entry;
        result.push_back(key);
    }
    // 排序保证调试面板每帧顺序一致，否则条目会跳动，无法用眼睛跟踪变化
    std::sort(result.begin(), result.end());
    return result;
}

// ---------------------------------------------------------------------------
// 值 -> 文本
// ---------------------------------------------------------------------------
std::string Blackboard::valueToString(const BlackboardEntry& entry) {
    if (!entry.value.has_value()) {
        return "<empty>";
    }

    const std::type_index& type = entry.typeIndex;

    // 按类型分派。只覆盖战术沙盒实际会存进去的类型，
    // 未知类型回退到类型名——总比不显示要好。
    if (type == std::type_index(typeid(bool))) {
        return std::any_cast<bool>(entry.value) ? "true" : "false";
    }
    if (type == std::type_index(typeid(int))) {
        return std::to_string(std::any_cast<int>(entry.value));
    }
    if (type == std::type_index(typeid(float))) {
        char buffer[32];
        std::snprintf(buffer, sizeof(buffer), "%.2f", static_cast<double>(std::any_cast<float>(entry.value)));
        return std::string(buffer);
    }
    if (type == std::type_index(typeid(double))) {
        char buffer[32];
        std::snprintf(buffer, sizeof(buffer), "%.3f", std::any_cast<double>(entry.value));
        return std::string(buffer);
    }
    if (type == std::type_index(typeid(std::string))) {
        return std::any_cast<std::string>(entry.value);
    }
    if (type == std::type_index(typeid(glm::vec2))) {
        const glm::vec2 value = std::any_cast<glm::vec2>(entry.value);
        char buffer[64];
        std::snprintf(buffer, sizeof(buffer), "(%.1f, %.1f)",
                      static_cast<double>(value.x), static_cast<double>(value.y));
        return std::string(buffer);
    }
    if (type == std::type_index(typeid(std::vector<glm::vec2>))) {
        const auto& points = std::any_cast<const std::vector<glm::vec2>&>(entry.value);
        return "vec2[" + std::to_string(points.size()) + "]";
    }
    if (type == std::type_index(typeid(std::vector<int>))) {
        const auto& values = std::any_cast<const std::vector<int>&>(entry.value);
        std::string text = "int[" + std::to_string(values.size()) + "]{";
        const size_t limit = std::min<size_t>(values.size(), 4);
        for (size_t i = 0; i < limit; ++i) {
            if (i > 0) {
                text += ",";
            }
            text += std::to_string(values[i]);
        }
        if (values.size() > limit) {
            text += ",...";
        }
        text += "}";
        return text;
    }

    return std::string("<") + type.name() + ">";
}

std::string Blackboard::dump() const {
    std::ostringstream stream;
    for (const std::string& key : keys()) {
        const BlackboardEntry* e = entry(key);
        if (!e) {
            continue;
        }
        stream << key << " = " << valueToString(*e)
               << "  [v" << e->version
               << ", w=" << e->lastWriter
               << ", n=" << e->writeCount << "]\n";
    }
    if (entries_.empty()) {
        stream << "(空)\n";
    }
    return stream.str();
}

} // namespace TacticalAI::AI
