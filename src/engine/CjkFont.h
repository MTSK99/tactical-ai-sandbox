// ===========================================================================
//  src/engine/CjkFont.h
//
//  中文字体加载辅助。
//
//  【它解决什么问题】
//  raylib 的默认字体（GetFontDefault）是内嵌的点阵 ASCII 字体，
//  字符集只覆盖 32..126 —— 任何中文都会画成一个空白方块 / 问号。
//  想在窗口里显示中文，必须显式加载一个含 CJK 字形的 TTF/OTF，
//  并且在加载时把需要的码点（codepoints）列出来。
//
//  【为什么要把码点显式列出来】
//  LoadFontEx 的第二个参数可以传 nullptr + 码点数，让 raylib 自动
//  加载字体里的前 N 个字形——对中文来说这是个陷阱：
//  CJK 字体的前几千个字形通常是日文假名和符号，中文汉字排在很后面，
//  于是「自动加载 2000 个字形」的结果往往是中文依然缺字。
//
//  正确做法：扫描项目里实际用到的所有中文字符，去重后只加载这些码点。
//  本模块用「字形表 + 按需收集」两步走：
//    1. collectCommonCodepoints() 提供一个覆盖项目常用汉字的基线集合；
//    2. loadWithCodepoints() 真正调 LoadFontEx。
//
//  【字体从哪来】
//  优先查找项目内置字体（assets/fonts），找不到则回退到系统字体。
//  完整查找路径见 candidateFontPaths()。
// ===========================================================================
#pragma once

#include <string>
#include <vector>

#include "raylib.h"

namespace TacticalAI::Engine {

/// 中文字体加载结果。
struct CjkFontLoadResult {
    Font        font{};              ///< 加载成功时的字体；失败保持默认值
    bool        success       = false;
    bool        usedSystemFont = false;  ///< true 表示用了系统字体而非项目内置
    std::string sourcePath;          ///< 实际加载的字体路径
    int         glyphCount    = 0;   ///< 成功加载的字形数量（含 ASCII 基线）
    std::string message;             ///< 失败原因或成功摘要（可直接打印）
};

/// 返回候选字体路径列表（按优先级排序）。
/// 顺序：项目 assets/fonts -> Windows 系统字体 -> 常见 Linux/macOS 字体。
[[nodiscard]] std::vector<std::string> candidateFontPaths();

/// 收集项目界面里出现的中文/符号码点（去重、升序）。
///
/// 之所以内置一份字表而不是运行时扫描源码：运行时扫描源码目录在发布版
/// 里不成立（源码可能不在），而漏字的表现是「个别字变方块」，
/// 比整体失败更难排查。宁可多加载几百个字形的显存。
[[nodiscard]] std::vector<int> collectCommonCodepoints();

/// 加载一个带中文字形的字体。
///
/// @param fontSize        基准字号（raylib 会据此把字形烘焙成纹理图集）
/// @param extraCodepoints 额外需要的码点（例如从 UI 文案里动态提取的字）
/// @return 加载结果；success=false 时调用方应回退到默认字体并打印 message。
[[nodiscard]] CjkFontLoadResult loadDefaultCjkFont(int fontSize = 32,
                                                   const std::vector<int>& extraCodepoints = {});

/// 从一段文本里提取所有非 ASCII 码点（UTF-8 解码）。
/// 用于把「界面上实际会显示的文案」喂给字体加载，做到精确覆盖。
[[nodiscard]] std::vector<int> extractNonAsciiCodepoints(const std::vector<std::string>& texts);

} // namespace TacticalAI::Engine
