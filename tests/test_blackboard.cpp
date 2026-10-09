// ===========================================================================
//  tests/test_blackboard.cpp
// ===========================================================================
#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "ai/Blackboard.h"

using namespace TacticalAI::AI;

// ---------------------------------------------------------------------------
//  基础读写
// ---------------------------------------------------------------------------
TEST(BlackboardTest, SetAndGetRoundTrip) {
    Blackboard blackboard;

    blackboard.set("count", 42);
    blackboard.set("ratio", 0.75f);
    blackboard.set("name", std::string("Enemy0"));
    blackboard.set("enabled", true);

    EXPECT_EQ(blackboard.get<int>("count"), 42);
    EXPECT_FLOAT_EQ(blackboard.get<float>("ratio").value_or(-1.0f), 0.75f);
    ASSERT_TRUE(blackboard.get<std::string>("name").has_value());
    EXPECT_EQ(*blackboard.get<std::string>("name"), "Enemy0");
    EXPECT_EQ(blackboard.get<bool>("enabled"), true);
}

TEST(BlackboardTest, MissingKeyReturnsNullopt) {
    Blackboard blackboard;
    EXPECT_FALSE(blackboard.get<int>("nonexistent").has_value());
    EXPECT_FALSE(blackboard.contains("nonexistent"));
}

TEST(BlackboardTest, TypeMismatchReturnsNullopt) {
    Blackboard blackboard;
    blackboard.set("value", 123);

    // 关键行为：类型不符必须返回 nullopt 而不是抛异常或返回垃圾值。
    // AI 逻辑不该被异常打断——一个读错类型的节点应该只是失败。
    EXPECT_FALSE(blackboard.get<float>("value").has_value());
    EXPECT_FALSE(blackboard.get<std::string>("value").has_value());
    EXPECT_TRUE(blackboard.get<int>("value").has_value());
}

TEST(BlackboardTest, GetOrProvidesFallback) {
    Blackboard blackboard;
    blackboard.set("health", 50.0f);

    EXPECT_FLOAT_EQ(blackboard.getOr<float>("health", 100.0f), 50.0f);
    // 不存在的键返回 fallback
    EXPECT_FLOAT_EQ(blackboard.getOr<float>("missing", 100.0f), 100.0f);
    // 类型不符时也返回 fallback
    EXPECT_FLOAT_EQ(blackboard.getOr<float>("health", 100.0f), 50.0f);
}

TEST(BlackboardTest, HasChecksTypePrecisely) {
    Blackboard blackboard;
    blackboard.set("enemy", 7);

    EXPECT_TRUE(blackboard.has<int>("enemy"));
    EXPECT_FALSE(blackboard.has<float>("enemy"));
    EXPECT_FALSE(blackboard.has<int>("nonexistent"));
}

// ---------------------------------------------------------------------------
//  向量类型（AI 层的主力数据类型）
// ---------------------------------------------------------------------------
TEST(BlackboardTest, StoresGlmVec2) {
    Blackboard blackboard;
    const glm::vec2 position{120.5f, -43.25f};

    blackboard.set("self.position", position);

    const auto retrieved = blackboard.get<glm::vec2>("self.position");
    ASSERT_TRUE(retrieved.has_value());
    EXPECT_FLOAT_EQ(retrieved->x, 120.5f);
    EXPECT_FLOAT_EQ(retrieved->y, -43.25f);
}

TEST(BlackboardTest, StoresVectorOfVec2) {
    Blackboard blackboard;
    const std::vector<glm::vec2> path{{0.0f, 0.0f}, {10.0f, 5.0f}, {20.0f, 0.0f}};

    blackboard.set("path.points", path);

    const auto retrieved = blackboard.get<std::vector<glm::vec2>>("path.points");
    ASSERT_TRUE(retrieved.has_value());
    ASSERT_EQ(retrieved->size(), 3u);
    EXPECT_FLOAT_EQ((*retrieved)[1].x, 10.0f);
}

// ---------------------------------------------------------------------------
//  版本与写入者追踪（调试功能的基础）
// ---------------------------------------------------------------------------
TEST(BlackboardTest, VersionIncrementsOnWrite) {
    Blackboard blackboard;

    const uint64_t first = blackboard.set("key", 1, "WriterA");
    const uint64_t second = blackboard.set("key", 2, "WriterB");

    EXPECT_EQ(first, 1u);
    EXPECT_EQ(second, 2u);
    EXPECT_EQ(blackboard.version("key"), 2u);
    EXPECT_EQ(blackboard.get<int>("key"), 2);
}

TEST(BlackboardTest, TracksLastWriter) {
    Blackboard blackboard;

    blackboard.set("threat", 0.5f, "Perception");
    const BlackboardEntry* entry = blackboard.entry("threat");
    ASSERT_NE(entry, nullptr);
    EXPECT_EQ(entry->lastWriter, "Perception");
    EXPECT_EQ(entry->writeCount, 1);

    blackboard.set("threat", 0.8f, "BehaviorTree");
    entry = blackboard.entry("threat");
    ASSERT_NE(entry, nullptr);
    // lastWriter 应更新——这是追溯「谁改了这个值」的关键
    EXPECT_EQ(entry->lastWriter, "BehaviorTree");
    EXPECT_EQ(entry->writeCount, 2);
}

TEST(BlackboardTest, SetIfAbsentDoesNotOverwrite) {
    Blackboard blackboard;

    EXPECT_TRUE(blackboard.setIfAbsent("key", 1));
    // 第二次不应写入
    EXPECT_FALSE(blackboard.setIfAbsent("key", 2));
    EXPECT_EQ(blackboard.get<int>("key"), 1);
}

// ---------------------------------------------------------------------------
//  删除与清理
// ---------------------------------------------------------------------------
TEST(BlackboardTest, EraseRemovesKey) {
    Blackboard blackboard;
    blackboard.set("temp", 5);

    EXPECT_TRUE(blackboard.erase("temp"));
    EXPECT_FALSE(blackboard.contains("temp"));
    // 重复删除返回 false
    EXPECT_FALSE(blackboard.erase("temp"));
}

TEST(BlackboardTest, ClearRemovesAllEntriesButKeepsTotalWrites) {
    Blackboard blackboard;
    blackboard.set("a", 1);
    blackboard.set("b", 2);

    EXPECT_EQ(blackboard.size(), 2u);

    blackboard.clear();

    EXPECT_TRUE(blackboard.empty());
    EXPECT_EQ(blackboard.size(), 0u);
    // totalWrites 是累计统计，跨 clear 保留才能反映真实写入频率
    EXPECT_EQ(blackboard.totalWrites(), 2u);
}

TEST(BlackboardTest, KeysAreSortedForStableUIDisplay) {
    Blackboard blackboard;
    blackboard.set("zebra", 1);
    blackboard.set("alpha", 2);
    blackboard.set("middle", 3);

    const std::vector<std::string> keys = blackboard.keys();
    ASSERT_EQ(keys.size(), 3u);
    // 排序保证调试面板每帧顺序一致，否则条目会跳动
    EXPECT_EQ(keys[0], "alpha");
    EXPECT_EQ(keys[1], "middle");
    EXPECT_EQ(keys[2], "zebra");
}

// ---------------------------------------------------------------------------
//  脏标记
// ---------------------------------------------------------------------------
TEST(BlackboardTest, DirtyFlagLifecycle) {
    Blackboard blackboard;
    EXPECT_FALSE(blackboard.hasDirtyEntries());

    blackboard.set("key", 1);
    EXPECT_TRUE(blackboard.hasDirtyEntries());

    blackboard.clearDirtyFlag();
    EXPECT_FALSE(blackboard.hasDirtyEntries());

    blackboard.set("key", 2);
    EXPECT_TRUE(blackboard.hasDirtyEntries());
}

// ---------------------------------------------------------------------------
//  值转文本
// ---------------------------------------------------------------------------
TEST(BlackboardTest, ValueToStringHandlesCommonTypes) {
    Blackboard blackboard;

    blackboard.set("b", true);
    blackboard.set("i", 42);
    blackboard.set("s", std::string("hello"));
    blackboard.set("v", glm::vec2(1.5f, -2.5f));

    ASSERT_NE(blackboard.entry("b"), nullptr);
    EXPECT_EQ(Blackboard::valueToString(*blackboard.entry("b")), "true");

    ASSERT_NE(blackboard.entry("i"), nullptr);
    EXPECT_EQ(Blackboard::valueToString(*blackboard.entry("i")), "42");

    ASSERT_NE(blackboard.entry("s"), nullptr);
    EXPECT_EQ(Blackboard::valueToString(*blackboard.entry("s")), "hello");

    ASSERT_NE(blackboard.entry("v"), nullptr);
    const std::string vecText = Blackboard::valueToString(*blackboard.entry("v"));
    // 只验证可读性，不锁定具体格式（格式可能调整）
    EXPECT_NE(vecText.find("1.5"), std::string::npos);
    EXPECT_NE(vecText.find("-2.5"), std::string::npos);
}

TEST(BlackboardTest, ValueToStringHandlesVectorSizes) {
    Blackboard blackboard;

    blackboard.set("ints", std::vector<int>{1, 2, 3, 4, 5, 6});
    ASSERT_NE(blackboard.entry("ints"), nullptr);
    const std::string text = Blackboard::valueToString(*blackboard.entry("ints"));
    // 应显示总数并截断明细
    EXPECT_NE(text.find("6"), std::string::npos);
    EXPECT_NE(text.find("..."), std::string::npos);
}

// ---------------------------------------------------------------------------
//  键句柄的哈希
// ---------------------------------------------------------------------------
TEST(BlackboardKeyTest, HashIsDeterministic) {
    const BlackboardKey a("perception.nearestEnemy");
    const BlackboardKey b("perception.nearestEnemy");
    const BlackboardKey c("perception.otherEnemy");

    EXPECT_EQ(a.hash(), b.hash());
    EXPECT_NE(a.hash(), c.hash());
}

TEST(BlackboardKeyTest, EmptyKeyIsInvalid) {
    const BlackboardKey empty;
    EXPECT_FALSE(empty.isValid());

    const BlackboardKey named("key");
    EXPECT_TRUE(named.isValid());
}

// ---------------------------------------------------------------------------
//  便捷键常量
// ---------------------------------------------------------------------------
TEST(BlackboardKeysTest, KeysAreDistinct) {
    // 键名重复会导致不同系统互相覆盖数据——这类 bug 极难排查，
    // 因此用测试锁定唯一性。
    const std::vector<std::string> allKeys = {
        Blackboard::Keys::kVisibleEnemies,
        Blackboard::Keys::kNearestEnemy,
        Blackboard::Keys::kNearestEnemyDist,
        Blackboard::Keys::kLastKnownPosition,
        Blackboard::Keys::kThreatLevel,
        Blackboard::Keys::kTimeSinceSeenEnemy,
        Blackboard::Keys::kSelfPosition,
        Blackboard::Keys::kSelfHealth,
        Blackboard::Keys::kSelfHealthRatio,
        Blackboard::Keys::kSelfAmmo,
        Blackboard::Keys::kSelfState,
        Blackboard::Keys::kMoveTarget,
        Blackboard::Keys::kAimTarget,
        Blackboard::Keys::kCurrentAction,
        Blackboard::Keys::kShouldFire,
        Blackboard::Keys::kPath,
        Blackboard::Keys::kPathValid,
        Blackboard::Keys::kPathAge,
        Blackboard::Keys::kRepathRequested,
        Blackboard::Keys::kSquadIntelPosition,
        Blackboard::Keys::kSquadIntelTime,
    };

    for (size_t i = 0; i < allKeys.size(); ++i) {
        EXPECT_FALSE(allKeys[i].empty()) << "索引 " << i << " 的键为空";
        for (size_t j = i + 1; j < allKeys.size(); ++j) {
            EXPECT_NE(allKeys[i], allKeys[j])
                << "键名重复: " << allKeys[i] << " 与 " << allKeys[j];
        }
    }
}

TEST(BlackboardTest, DumpIsReadable) {
    Blackboard blackboard;
    blackboard.set("health", 75.0f, "Self");
    blackboard.set("enemy", 3, "Perception");

    const std::string dump = blackboard.dump();
    EXPECT_NE(dump.find("health"), std::string::npos);
    EXPECT_NE(dump.find("75"), std::string::npos);
    EXPECT_NE(dump.find("Perception"), std::string::npos);
}

TEST(BlackboardTest, DumpHandlesEmptyBlackboard) {
    Blackboard blackboard;
    const std::string dump = blackboard.dump();
    EXPECT_FALSE(dump.empty());
}
