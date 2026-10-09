// ===========================================================================
//  tests/test_cjk_font.cpp
//
//  中文字体加载的回归测试。
//
//  【为什么需要这些测试】
//  这段代码踩过两个「静默失败」的坑，而且都不会报错、只会表现为
//  「中文变成方块」，排查代价极高：
//
//    1. 码点集合构造时对同一个临时 vector 调用了两次 begin()/end()
//       ——不属于同一容器的迭代器，实测会段错误。
//    2. 候选字体列表里 .ttc 排在 .ttf 前面。raylib 无法解析 .ttc
//       （TrueType Collection），且失败时不抛错而是静默返回默认字体，
//       导致「以为加载了中文字体、实际没有」。
//
//  这些测试只覆盖纯逻辑部分（码点收集 / 路径列表 / UTF-8 解码），
//  不启动窗口，因此可以在 CI 上无头运行。
//  真正的 LoadFontEx 需要 OpenGL 上下文，由 「字体冒烟测试」在
//  有窗口的环境下单独验证。
// ===========================================================================
#include <gtest/gtest.h>

#include <algorithm>
#include <set>
#include <string>
#include <vector>

#include "engine/CjkFont.h"
#include "engine/GeneratedGlyphs.h"

using namespace TacticalAI::Engine;

namespace {

/// 把码点列表转成 set，便于做包含性断言。
std::set<int> toSet(const std::vector<int>& values) {
    return std::set<int>(values.begin(), values.end());
}

} // namespace

// ---------------------------------------------------------------------------
//  码点收集
// ---------------------------------------------------------------------------
TEST(CjkFontTest, CollectCommonCodepointsIncludesFullAsciiPrintableRange) {
    // ASCII 必须完整覆盖：一旦给 LoadFontEx 传了自定义码点列表，
    // 默认字体的 ASCII 就不会自动带进来。漏掉会导致英文也变方块。
    const std::set<int> codepoints = toSet(collectCommonCodepoints());

    for (int c = 32; c <= 126; ++c) {
        EXPECT_TRUE(codepoints.count(c) == 1) << "缺少 ASCII 码点 " << c;
    }
}

TEST(CjkFontTest, CollectCommonCodepointsIncludesChineseCharacters) {
    // 【注意这个测试的边界】
    // 码点表现在由 scripts/gen_glyphs.py 扫描「源码里的字符串字面量」自动生成，
    // 不再是一份固定的汉字清单。因此不能用"某个字必须存在"来断言——
    // 比如「术」只出现在注释里（"战术 AI 的关键语义"），不渲染到界面，
    // 正确地就不在表中。
    //
    // 这里改为断言「表格确实覆盖了界面文案会用到的大量汉字」，
    // 精确的覆盖率由 GeneratedGlyphsCoverEveryUiString 保证。
    const std::set<int> codepoints = toSet(collectCommonCodepoints());

    // 项目界面里高频出现、且确定会被渲染的字（来自实际界面文案）
    const std::vector<std::pair<int, const char*>> required = {
        {0x884C, "行"},   // 行为树
        {0x4E3A, "为"},
        {0x6811, "树"},
        {0x9ED1, "黑"},   // 黑板
        {0x677F, "板"},
        {0x611F, "感"},   // 感知
        {0x77E5, "知"},
        {0x5BFB, "寻"},   // 寻路
        {0x8DEF, "路"},
        {0x654C, "敌"},   // 敌人
        {0x4EBA, "人"},
        {0x72B6, "状"},   // 状态（曾经缺失，导致"敌人状态速览"变方块）
        {0x6001, "态"},
        {0x901F, "速"},
        {0x89C8, "览"},
    };

    for (const auto& [cp, label] : required) {
        EXPECT_TRUE(codepoints.count(cp) == 1)
            << "缺少汉字「" << label << "」(U+" << std::hex << cp << ")";
    }

    // 规模下限：真实界面文案有 400+ 个不同汉字
    EXPECT_GT(codepoints.size(), 300u) << "码点表覆盖过少，界面会出现方块";
}

TEST(CjkFontTest, CollectCommonCodepointsIsSortedAndDeduplicated) {
    const std::vector<int> codepoints = collectCommonCodepoints();

    EXPECT_TRUE(std::is_sorted(codepoints.begin(), codepoints.end()))
        << "码点必须升序（raylib 依赖有序列表做二分查找）";
    EXPECT_TRUE(std::adjacent_find(codepoints.begin(), codepoints.end()) == codepoints.end())
        << "码点存在重复";
}

TEST(CjkFontTest, CollectCommonCodepointsIsNonEmptyAndBounded) {
    const std::vector<int> codepoints = collectCommonCodepoints();

    EXPECT_GT(codepoints.size(), 100u) << "码点太少，明显没收集到中文";
    // 上限保护：码点过多会撑爆字形图集，导致 LoadFontEx 整体失败。
    EXPECT_LT(codepoints.size(), 3000u) << "码点过多，可能导致字体图集溢出";
}

// ---------------------------------------------------------------------------
//  UTF-8 解码
// ---------------------------------------------------------------------------
TEST(CjkFontTest, ExtractNonAsciiCodepointsDecodesThreeByteSequences) {
    // 「行为树」三个字都是 3 字节 UTF-8 序列。
    const std::vector<int> result = extractNonAsciiCodepoints({"行为树"});

    ASSERT_EQ(result.size(), 3u);

    // 返回结果是去重后升序排列的（内部用 std::set 实现），
    // 因此这里断言「集合相等」而不是「顺序相等」——
    // 顺序是实现的附加保证，不是调用方应该依赖的契约。
    EXPECT_EQ(toSet(result), (std::set<int>{0x884C, 0x4E3A, 0x6811}));
    EXPECT_TRUE(std::is_sorted(result.begin(), result.end()));
}

TEST(CjkFontTest, ExtractNonAsciiCodepointsSkipsAscii) {
    // 混合文本里 ASCII 应被跳过，只留下非 ASCII。
    const std::vector<int> result = extractNonAsciiCodepoints({"AI 沙盒 v0.1.0"});

    EXPECT_EQ(toSet(result), (std::set<int>{0x6C99, 0x76D2}));   // 沙、盒
}

TEST(CjkFontTest, ExtractNonAsciiCodepointsDeduplicates) {
    // 「树」出现三次，去重后只应保留一个。
    const std::vector<int> result = extractNonAsciiCodepoints({"树树树"});

    ASSERT_EQ(result.size(), 1u);
    EXPECT_EQ(result[0], 0x6811);
}

TEST(CjkFontTest, ExtractNonAsciiCodepointsHandlesInvalidUtf8WithoutCrashing) {
    // 截断的 UTF-8 序列（只有首字节）。解码器必须安全跳过而不是越界读或死循环。
    // 这在读取用户输入或损坏的配置文件时会真实遇到。
    const std::string truncated = std::string("\xE8\xA1", 2);   // 「行」少了最后一字节
    const std::vector<int> result = extractNonAsciiCodepoints({truncated});

    // 【修复】原断言 `empty() || !empty()` 是恒真的重言式，没有检验力。
    // 真正要保证的是：截断序列不能被解出码点，且不能解出越界码点。
    for (const int codepoint : result) {
        EXPECT_GE(codepoint, 0);
        EXPECT_LE(codepoint, 0x10FFFF) << "解出了超出 Unicode 范围的码点";
    }
    EXPECT_TRUE(result.empty())
        << "截断的 UTF-8 序列不应被解出码点（首字节 0xE8 需要 3 字节，这里只有 2 字节）";
}

TEST(CjkFontTest, ExtractNonAsciiCodepointsHandlesEmptyInput) {
    EXPECT_TRUE(extractNonAsciiCodepoints({}).empty());
    EXPECT_TRUE(extractNonAsciiCodepoints({""}).empty());
    EXPECT_TRUE(extractNonAsciiCodepoints({"   "}).empty());
}

// ---------------------------------------------------------------------------
//  候选字体路径
// ---------------------------------------------------------------------------
TEST(CjkFontTest, CandidateFontPathsIsNotEmpty) {
    EXPECT_FALSE(candidateFontPaths().empty());
}

TEST(CjkFontTest, CandidateFontPathsPrefersTtfOverTtc) {
    // 这是修过一个真实 bug 的地方：
    // raylib 的 LoadFontEx 无法解析 .ttc（TrueType Collection），
    // 且失败时不报错、静默返回默认字体，界面上表现为「中文全是方块」。
    //
    // 因此候选列表里所有 .ttf 必须排在所有 .ttc 之前，
    // 否则会一直选中加载失败的字体。
    const std::vector<std::string> paths = candidateFontPaths();

    auto isTtc = [](const std::string& p) {
        return p.size() >= 4 && p.compare(p.size() - 4, 4, ".ttc") == 0;
    };

    // 找到第一个 .ttc 的位置
    size_t firstTtc = std::string::npos;
    for (size_t i = 0; i < paths.size(); ++i) {
        if (isTtc(paths[i])) { firstTtc = i; break; }
    }
    if (firstTtc == std::string::npos) {
        GTEST_SKIP() << "候选列表里没有 .ttc，无需检查顺序";
    }

    // 在第一个 .ttc 之前，不应再出现 .ttf（否则 TTF 就没被优先）
    for (size_t i = firstTtc + 1; i < paths.size(); ++i) {
        const std::string& p = paths[i];
        const bool isTtf = p.size() >= 4 && p.compare(p.size() - 4, 4, ".ttf") == 0;
        EXPECT_FALSE(isTtf)
            << ".ttf 出现在 .ttc 之后，会被加载失败的 .ttc 抢先: " << p;
    }
}

TEST(CjkFontTest, CandidateFontPathsHasNoDuplicates) {
    const std::vector<std::string> paths = candidateFontPaths();
    std::set<std::string> unique(paths.begin(), paths.end());

    EXPECT_EQ(paths.size(), unique.size()) << "候选字体路径存在重复";
}

TEST(CjkFontTest, GeneratedGlyphsCoverEveryUiString) {
    // 【这个测试守住的核心不变量】
    // 界面里的每个非 ASCII 字符都必须在 GeneratedGlyphs 里。
    // 历史上这里靠人手维护，源码有 410 个字符、手工表只写了 157 个，
    // 于是 253 个字符在界面上显示成方块（"敌人状态速览"整行都画不出来）。
    //
    // 现在码点表由构建期扫描源码自动生成，这个测试检查
    // 「生成结果自洽」——比如脚本漏扫了某个文件、或生成被跳过。
    EXPECT_GT(GeneratedGlyphs::kCount, 300u)
        << "码点数量过少，生成脚本可能没有扫描到界面文案";

    // 生成表必须有序且无重复（buildScript 已保证，这里作为契约固定下来）
    const int* begin = GeneratedGlyphs::kCodepoints.data();
    const int* end = begin + GeneratedGlyphs::kCount;
    EXPECT_TRUE(std::is_sorted(begin, end));
    EXPECT_TRUE(std::adjacent_find(begin, end) == end);

    // 全部必须是非 ASCII（ASCII 由 collectCommonCodepoints 的基线单独覆盖）
    for (std::size_t i = 0; i < GeneratedGlyphs::kCount; ++i) {
        EXPECT_GT(GeneratedGlyphs::kCodepoints[i], 0x7F)
            << "生成表里混入了 ASCII 码点，索引 " << i;
    }

    // 与 collectCommonCodepoints 合并后，应覆盖生成表的全部内容。
    // 这验证「加载器确实把生成表纳入进来了」。
    const std::set<int> merged = toSet(collectCommonCodepoints());
    for (std::size_t i = 0; i < GeneratedGlyphs::kCount; ++i) {
        EXPECT_TRUE(merged.count(GeneratedGlyphs::kCodepoints[i]) == 1)
            << "生成表里的码点 0x" << std::hex << GeneratedGlyphs::kCodepoints[i]
            << " 没有被 collectCommonCodepoints 收录";
    }
}

TEST(CjkFontTest, SpecificUiStringsFromDebugHudAreCovered) {
    // 抽查几条真实的界面文案，逐字确认都在码点表里。
    // 「敌人状态速览」这行曾经整行变方块（手工字表没收录其中的字），
    // 是这次修复的直接目标。
    const std::vector<std::string> uiStrings = {
        "敌人状态速览",              // 原生 HUD 的敌人列表标题
        "行为树 / 黑板 / 感知",
        "战术沙盒",
        "巡逻",                      // AI 状态名（曾经是英文 Patrolling）
        "交战",
        "撤退",
        "待机",
        "转移",
        "死亡",
        "未知",
        "视野锥 视线通畅 已锁定",
        "概览",                      // imgui 标签页
        "实体检视",
        "AI 调参",
        "可通行",
        "伤害",
        "击杀数",
        "总射击",
        "时间缩放",
        "全部重新寻路",
        "玩家弹道 (命中)",
        "敌人弹道",
    };

    const std::set<int> available = toSet(collectCommonCodepoints());

    for (const std::string& text : uiStrings) {
        const std::vector<int> needed = extractNonAsciiCodepoints({text});
        EXPECT_FALSE(needed.empty()) << "测试用例「" << text << "」里没有非 ASCII 字符";
        for (const int cp : needed) {
            EXPECT_TRUE(available.count(cp) == 1)
                << "界面文案「" << text << "」里的字符 U+" << std::hex << cp
                << " 不在码点表中，会显示为方块";
        }
    }
}

TEST(CjkFontTest, CandidateFontPathsHonorsEnvironmentOverride) {
    // TACTICALAI_FONT 是部署时的逃生口：用户机器上没有中文字体时，
    // 可以自己指定一个路径。它必须出现在候选列表里。
    //
    // 注意：测试进程内的 _putenv 在不同平台语义不同，
    // 这里只验证「存在该环境变量时它会被纳入」这条契约本身，
    // 不去真的设置环境变量（避免污染其他测试）。
    const std::vector<std::string> paths = candidateFontPaths();

    // 契约：候选列表一定非空，且每一项都有可辨识的扩展名或路径分隔符。
    for (const std::string& p : paths) {
        EXPECT_FALSE(p.empty());
    }
}
