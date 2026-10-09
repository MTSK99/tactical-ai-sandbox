// ===========================================================================
//  src/engine/CjkFont.cpp
// ===========================================================================
#include "engine/CjkFont.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <set>
#include <string>

#include "engine/GeneratedGlyphs.h"

namespace TacticalAI::Engine {
namespace {

/// 探测一个路径是否存在且可读。
/// 用 fopen 而不是平台 API，是为了避免为一个「找字体」的小需求引入
/// <filesystem> 的链接负担（MinGW 上 std::filesystem 需要额外的链接库）。
bool fileExists(const std::string& path) {
    if (path.empty()) {
        return false;
    }
    if (FILE* handle = std::fopen(path.c_str(), "rb")) {
        std::fclose(handle);
        return true;
    }
    return false;
}

/// 环境变量取值；不存在时返回空串。
/// 用 getenv 而非 Windows API，保持与 raylib 的跨平台风格一致。
std::string envOrEmpty(const char* name) {
    if (const char* value = std::getenv(name)) {
        return std::string(value);
    }
    return {};
}

} // namespace

// ---------------------------------------------------------------------------
//  候选字体路径
// ---------------------------------------------------------------------------
std::vector<std::string> candidateFontPaths() {
    std::vector<std::string> paths;

    // --- 1. 项目内置字体（最高优先级）---
    // 仓库自带字体的好处是行为可复现：不依赖用户机器上装了什么。
    const char* builtinCandidates[] = {
        "assets/fonts/NotoSansSC-Regular.ttf",
        "assets/fonts/NotoSansSC-Regular.otf",
        "assets/fonts/SourceHanSansSC-Regular.ttf",
        "assets/fonts/SourceHanSansSC-Regular.otf",
        "assets/fonts/simhei.ttf",
    };
    for (const char* path : builtinCandidates) {
        paths.emplace_back(path);
    }

    // --- 2. 允许用户用环境变量指定（部署时的逃生口）---
    if (const std::string custom = envOrEmpty("TACTICALAI_FONT"); !custom.empty()) {
        paths.push_back(custom);
    }

    // --- 3. Windows 系统字体 ---
    const std::string windir = envOrEmpty("WINDIR");

    // 【重要：为什么整段都要「.ttf 排在 .ttc 前面」】
    // raylib 的 LoadFontEx 无法解析 .ttc（TrueType Collection，
    // 一个文件里打包多个字面）。传入 .ttc 时它不会报错，
    // 而是静默返回默认字体（baseSize 会变成 10 而不是请求的字号），
    // 于是中文依旧全是方块——这个坑极难从现象反推。
    //
    // 因此所有 .ttf 必须整体先于所有 .ttc 出现。这不是「每一段内部有序」
    // 就够的：如果第 3 段以 .ttc 结尾、第 4 段以 .ttf 开头，
    // 那么 WINDIR 缺失时就会先命中第 3 段的 .ttc。
    // tests/test_cjk_font.cpp 里的 CandidateFontPathsPrefersTtfOverTtc
    // 就是为守住这条全局不变量而写的。

    // 3a. 先放所有 TTF（实测加载稳定的单文件字体）
    if (!windir.empty()) {
        const std::string fontsDir = windir + "\\Fonts\\";
        paths.push_back(fontsDir + "simhei.ttf");    // 黑体
        paths.push_back(fontsDir + "Deng.ttf");      // 等线
    }
    paths.emplace_back("C:/Windows/Fonts/simhei.ttf");   // 应对 WINDIR 缺失
    paths.emplace_back("C:/Windows/Fonts/Deng.ttf");

    // 3b. 再放 TTC 作为最后手段（万一某个 raylib 构建支持它）
    if (!windir.empty()) {
        const std::string fontsDir = windir + "\\Fonts\\";
        paths.push_back(fontsDir + "msyh.ttc");      // 微软雅黑
        paths.push_back(fontsDir + "msyhbd.ttc");
        paths.push_back(fontsDir + "simsun.ttc");    // 宋体
    }
    paths.emplace_back("C:/Windows/Fonts/msyh.ttc");

    // --- 4. Linux / macOS 常见路径（保持跨平台可用）---
    paths.emplace_back("/usr/share/fonts/truetype/wqy/wqy-microhei.ttf");
    paths.emplace_back("/usr/share/fonts/truetype/arphic/uming.ttf");
    paths.emplace_back("/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc");
    paths.emplace_back("/System/Library/Fonts/PingFang.ttc");
    paths.emplace_back("/System/Library/Fonts/STHeiti Light.ttc");

    // --- 5. 稳定排序：所有 .ttf/.otf 排到所有 .ttc 之前 ---
    // 前面手工分组容易在后续维护中被打乱（比如新加一条 Linux 路径就破坏了
    // 全局顺序）。这里用一次稳定排序把「TTF 优先」变成代码保证的硬约束，
    // 而不是依赖列表书写顺序——人写顺序迟早会出错。
    //
    // 用 stable_sort 保持同组内的原有优先级（先系统后兜底）。
    auto isCollection = [](const std::string& path) {
        const size_t dot = path.find_last_of('.');
        if (dot == std::string::npos) {
            return false;
        }
        // 大小写不敏感：Windows 上 .TTC 同样无法加载
        std::string ext = path.substr(dot);
        for (char& c : ext) {
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
        return ext == ".ttc";
    };

    std::stable_sort(paths.begin(), paths.end(),
                     [&isCollection](const std::string& a, const std::string& b) {
                         return !isCollection(a) && isCollection(b);
                     });

    return paths;
}

// ---------------------------------------------------------------------------
//  常用码点表
// ---------------------------------------------------------------------------
std::vector<int> collectCommonCodepoints() {
    std::set<int> codepoints;

    // --- ASCII 基线（32..126 可打印字符）---
    // 注意必须显式加入：一旦 LoadFontEx 收到自定义码点列表，
    // 默认字体里的 ASCII 就不会自动带进来，漏掉会导致英文全变方块。
    for (int c = 32; c <= 126; ++c) {
        codepoints.insert(c);
    }

    // --- 常用中文标点与全角符号 ---
    // 这些通常在 UI 文案里高频出现，且不少字体把它们排在非连续区间，
    // 必须逐个点名。
    const int punctuation[] = {
        0x00B7,  // ·
        0x2014,  // —
        0x2018,  // ‘
        0x2019,  // ’
        0x201C,  // “
        0x201D,  // ”
        0x2026,  // …
        0x2039,  // ‹
        0x203A,  // ›
        0x2190,  // ←
        0x2191,  // ↑
        0x2192,  // →
        0x2193,  // ↓
        0x25A0,  // ■
        0x25B2,  // ▲
        0x25BC,  // ▼
        0x25CF,  // ●
        0x25CB,  // ○
        0x3001,  // 、
        0x3002,  // 。
        0x3008,  // 〈
        0x3009,  // 〉
        0x300A,  // 《
        0x300B,  // 》
        0x300C,  // 「
        0x300D,  // 」
        0x300E,  // 『
        0x300F,  // 』
        0x3010,  // 【
        0x3011,  // 】
        0xFF01,  // ！
        0xFF08,  // （
        0xFF09,  // ）
        0xFF0C,  // ，
        0xFF0F,  // ／
        0xFF1A,  // ：
        0xFF1B,  // ；
        0xFF1F,  // ？
        0xFF5E,  // ～
    };
    for (int cp : punctuation) {
        codepoints.insert(cp);
    }

    // --- 项目界面用到的汉字 ---
    // 【已改为自动生成】
    // 早期这里是一份人工维护的"UI 常用字表"。结果是源码里出现 410 个
    // 不同的非 ASCII 字符，手工表只覆盖了 157 个，**253 个字符显示成方块**
    // （比如"敌人状态速览"里的"状态速览"）。
    //
    // 人工维护字符表注定失败：往界面加一个词就得记得回来补字表，
    // 而漏掉的表现只是"某几个字变方块"，几乎不可能被发现。
    //
    // 现在由 scripts/gen_glyphs.py 在构建期扫描源码自动提取，
    // 生成 GeneratedGlyphs.h。见该脚本头部注释。
    for (const int cp : GeneratedGlyphs::kCodepoints) {
        codepoints.insert(cp);
    }

    return std::vector<int>(codepoints.begin(), codepoints.end());
}

// ---------------------------------------------------------------------------
//  UTF-8 解码
// ---------------------------------------------------------------------------
std::vector<int> extractNonAsciiCodepoints(const std::vector<std::string>& texts) {
    std::set<int> result;

    for (const std::string& text : texts) {
        const unsigned char* cursor = reinterpret_cast<const unsigned char*>(text.data());
        const unsigned char* end = cursor + text.size();

        while (cursor < end) {
            const unsigned char lead = *cursor;
            int codepoint = 0;
            int extraBytes = 0;

            // 按首字节判断序列长度；非法首字节直接跳过一字节，
            // 保证解析过程永远不会越界或死循环。
            if (lead < 0x80) {
                ++cursor;          // ASCII 不收集
                continue;
            } else if ((lead & 0xE0) == 0xC0) {
                codepoint = lead & 0x1F;
                extraBytes = 1;
            } else if ((lead & 0xF0) == 0xE0) {
                codepoint = lead & 0x0F;
                extraBytes = 2;
            } else if ((lead & 0xF8) == 0xF0) {
                codepoint = lead & 0x07;
                extraBytes = 3;
            } else {
                ++cursor;
                continue;
            }

            if (cursor + extraBytes >= end) {
                break;             // 截断的序列，丢弃
            }

            bool valid = true;
            for (int i = 1; i <= extraBytes; ++i) {
                const unsigned char continuation = cursor[i];
                if ((continuation & 0xC0) != 0x80) {
                    valid = false;
                    break;
                }
                codepoint = (codepoint << 6) | (continuation & 0x3F);
            }

            if (valid && codepoint > 0x7F) {
                result.insert(codepoint);
            }

            cursor += (valid ? (1 + extraBytes) : 1);
        }
    }

    return std::vector<int>(result.begin(), result.end());
}

// ---------------------------------------------------------------------------
//  加载字体
// ---------------------------------------------------------------------------
CjkFontLoadResult loadDefaultCjkFont(int fontSize, const std::vector<int>& extraCodepoints) {
    CjkFontLoadResult result;

    const int safeSize = std::clamp(fontSize, 8, 256);

    // --- 合并码点 ---
    // 注意：必须先存下 collectCommonCodepoints() 的结果再构造 set。
    // 早期版本写成 std::set<int> merged(collectCommonCodepoints().begin(), ...),
    // 两次调用返回的是两个不同的临时 vector，begin() 来自第一个、end() 来自
    // 第二个——迭代器不属于同一容器，构造 set 时就是未定义行为（实测会段错误）。
    const std::vector<int> baseCodepoints = collectCommonCodepoints();
    std::set<int> merged(baseCodepoints.begin(), baseCodepoints.end());
    for (const int cp : extraCodepoints) {
        if (cp > 0x7F) {           // ASCII 已在基线里，避免重复
            merged.insert(cp);
        }
    }
    const std::vector<int> codepoints(merged.begin(), merged.end());

    // --- 码点数量上限 ---
    // LoadFontEx 会把所有码点烘进一张纹理图集。码点太多会超出 GPU 的
    // 最大纹理尺寸，raylib 会打印错误并回退默认字体（又是一次静默失败）。
    // 这里设一个保守上限：宁可少几个生僻字，也不要整体加载失败。
    constexpr size_t kMaxCodepoints = 2400;
    std::vector<int> trimmed = codepoints;
    bool trimmedForLimit = false;
    if (trimmed.size() > kMaxCodepoints) {
        trimmed.resize(kMaxCodepoints);
        trimmedForLimit = true;
    }

    // --- 逐个候选真正尝试加载 ---
    // 【为什么不能只判断「文件存在」】
    // raylib 的 LoadFontEx 在遇到无法解析的格式（典型是 .ttc，
    // 即 TrueType Collection —— 一个文件打包多个字面）时不会报错，
    // 而是静默返回默认字体。如果只检查文件存在就选中它，
    // 中文照样是方块，且排查时完全看不出哪里错了。
    //
    // 所以必须「试加载 + 校验结果」：
    // 成功的标志是 font.baseSize 等于请求的 fontSize；
    // 失败时它会是默认字体的 10（或 0）。
    std::string chosen;
    Font loadedFont{};
    bool found = false;

    for (const std::string& path : candidateFontPaths()) {
        if (!fileExists(path)) {
            continue;
        }

        Font attempt = LoadFontEx(path.c_str(), safeSize,
                                  const_cast<int*>(trimmed.data()),
                                  static_cast<int>(trimmed.size()));

        // 【修复】成功判据增加一条：字形数必须等于请求的码点数。
        // 只有 attempt 真正包含我们要的码点，才算加载成功；否则可能把
        // "加载失败回退的默认字体"误判为成功（默认字体 baseSize = 10，
        // 若请求字号恰好也是 10，三个条件会全部满足，中文却仍是方块）。
        const bool looksValid = attempt.texture.id != 0
                                && attempt.baseSize == safeSize
                                && attempt.glyphCount == static_cast<int>(trimmed.size());

        if (looksValid) {
            chosen     = path;
            loadedFont = attempt;
            found      = true;
            break;
        }

        // 【修复】这里原先"刻意不做任何释放"，理由成立的前提是「失败的
        // 尝试 = 拿到的就是全局默认字体」。但还有一种情况：LoadFontData
        // 成功分配了 glyphs/recs（CPU 侧内存），却在上传图集纹理时失败
        // （texture.id == 0，即上面注释提到的超出 GPU 最大纹理尺寸）。
        // 此时 attempt 是一份**新分配**的资源，直接丢弃就是内存泄漏，
        // 而且循环还会继续用同样的码点表去撞下一个候选字体。
        //
        // 判据：只有当 attempt 确实是「默认字体的拷贝」时才不能释放 ——
        // 它的 glyphs/recs 指向全局默认字体的数组，UnloadFont 会连带把
        // 默认字体一起释放，之后所有文字绘制都会崩溃。
        const ::Font defaultFont = GetFontDefault();
        const bool isDefaultFontCopy =
            (attempt.texture.id != 0 && attempt.texture.id == defaultFont.texture.id)
            || (attempt.glyphs != nullptr && attempt.glyphs == defaultFont.glyphs);

        if (!isDefaultFontCopy && attempt.glyphCount > 0) {
            // glyphs/recs 是本次 LoadFontEx 新分配的 CPU 内存。
            // 若纹理上传失败（texture.id == 0），raylib 的 UnloadTexture
            // 会自行跳过，这里只释放 CPU 侧数据，仍然安全。
            UnloadFont(attempt);
        }
    }

    if (!found) {
        result.message =
            "未找到可加载的中文字体（.ttc 集合格式在 raylib 下不可用，需要 .ttf/.otf），"
            "中文将显示为方块。可用环境变量 TACTICALAI_FONT 指定字体路径";
        return result;
    }

    result.usedSystemFont = (chosen.find("assets/fonts") == std::string::npos);

    result.font         = loadedFont;
    result.success      = true;
    result.sourcePath   = chosen;
    result.glyphCount   = loadedFont.glyphCount;

    char summary[320];
    std::snprintf(summary, sizeof(summary),
                  "字体加载成功: %s (%s, %d 字形%s)",
                  chosen.c_str(),
                  result.usedSystemFont ? "系统字体" : "项目内置",
                  loadedFont.glyphCount,
                  trimmedForLimit ? "，已达上限截断" : "");
    result.message = summary;

    return result;
}

} // namespace TacticalAI::Engine
