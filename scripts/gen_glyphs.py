#!/usr/bin/env python3
"""
从源码里提取所有会显示到屏幕上的非 ASCII 字符，生成 C++ 码点表头文件。

【为什么必须自动生成】
项目最早版本靠人工往 CjkFont.cpp 里抄一份"UI 常用字表"。结果是：
源码里出现 410 个不同的非 ASCII 字符，手工表只覆盖了 157 个，
**253 个字符会显示成方块**（例如"敌人状态速览"里的"状态速览"四个字）。

人工维护字符表的失败是必然的——每当有人往界面文案里加一个词，
就得记得回来补字表，而漏掉的表现只是"某几个字变方块"，极难被发现。

因此改为构建期自动扫描源码，把"界面真正会显示的字"精确提取出来。

【扫描范围】
只扫描 src/ 下会被渲染到屏幕的字面量。日志（std::printf / fprintf）
不会显示在窗口里，但一并收录也无害（多几十个字形，代价可忽略）。
"""
import os
import re
import sys

# 【为什么强制把 stdout/stderr 切成 UTF-8】
# Windows 下 Python 默认用本地代码页（中文系统是 GBK）写 stdout。
# 本脚本的中文消息会被 CMake 捕获后转发到构建输出，一旦编码不匹配
# 构建日志里就会出现乱码。
#
# 注意：即便如此，CMake 自身在**捕获**子进程输出时也会用本地代码页解码，
# 因此脚本里的进度消息仍然统一用英文（见下方 print 调用），
# 从根本上避开这条编码链。中文只保留在源码注释与生成的文件内容里。
if hasattr(sys.stdout, 'reconfigure'):
    try:
        sys.stdout.reconfigure(encoding='utf-8')
        sys.stderr.reconfigure(encoding='utf-8')
    except (AttributeError, OSError):
        pass

# 需要扫描的源文件（相对项目根目录）
SOURCE_FILES = [
    'src/main.cpp',
    'src/debug/DebugUI.cpp',
    'src/debug/AIDebugDraw.cpp',
    'src/game/World.cpp',
    'src/game/Entity.cpp',
    'src/game/Player.cpp',
    'src/game/Enemy.cpp',
    'src/game/Projectile.cpp',
    'src/ai/BehaviorTree.cpp',
    'src/ai/Perception.cpp',
    'src/ai/EQS.cpp',
    'src/ai/Pathfinding.cpp',
]

HEADER_TEMPLATE = '''// ===========================================================================
//  src/engine/GeneratedGlyphs.h
//
//  【自动生成 —— 请勿手工编辑】
//  由 scripts/gen_glyphs.py 扫描 src/ 下的界面文案生成。
//  重新生成:  cmake --build build --target generate_glyphs
//             （或直接运行 python scripts/gen_glyphs.py）
//
//  内容: 项目界面里实际出现过的所有非 ASCII 字符的码点。
//  用途: 交给 raylib 的 LoadFontEx 烘焙字形，保证"界面上有的字都画得出来"。
// ===========================================================================
#pragma once

#include <array>
#include <cstddef>

namespace TacticalAI::Engine::GeneratedGlyphs {

/// 从源码界面文案中提取的非 ASCII 字符码点（升序、去重）。
inline constexpr std::array<int, @COUNT@> kCodepoints = {{
@VALUES@
}};

/// 提取到的字符数量。
inline constexpr std::size_t kCount = @COUNT@;

} // namespace TacticalAI::Engine::GeneratedGlyphs
'''


def extract_chars(root):
    """扫描源文件，返回所有非 ASCII 字符的码点集合。"""
    codepoints = set()

    for rel in SOURCE_FILES:
        path = os.path.join(root, rel)
        if not os.path.exists(path):
            continue
        try:
            text = open(path, encoding='utf-8').read()
        except UnicodeDecodeError:
            print(f"  [warn] cannot read as UTF-8: {rel}", file=sys.stderr)
            continue

        # 提取双引号字符串字面量（跳过转义序列）
        for match in re.finditer(r'"((?:[^"\\]|\\.)*)"', text):
            for ch in match.group(1):
                if ord(ch) > 0x7F:
                    codepoints.add(ord(ch))

        # 也提取字符字面量 'x'（少见但可能有）
        for match in re.finditer(r"'((?:[^'\\]|\\.)*)'", text):
            value = match.group(1)
            if len(value) == 1 and ord(value) > 0x7F:
                codepoints.add(ord(value))

    return codepoints


def main():
    root = sys.argv[1] if len(sys.argv) > 1 else '.'
    output = sys.argv[2] if len(sys.argv) > 2 else 'src/engine/GeneratedGlyphs.h'

    codepoints = sorted(extract_chars(root))

    if not codepoints:
        print("[gen_glyphs] no non-ASCII characters found, skipping", file=sys.stderr)
        return 0

    # 每行 12 个，保持可读
    lines = []
    for i in range(0, len(codepoints), 12):
        chunk = codepoints[i:i + 12]
        lines.append('    ' + ' '.join(f'0x{cp:04X},' for cp in chunk))
    values = '\n'.join(lines)

    content = (HEADER_TEMPLATE
               .replace('@COUNT@', str(len(codepoints)))
               .replace('@VALUES@', values))

    # 只在内容变化时写入，避免每次构建都触发重新编译
    if os.path.exists(output):
        existing = open(output, encoding='utf-8').read()
        if existing == content:
            print(f"[gen_glyphs] unchanged: {len(codepoints)} codepoints")
            return 0

    os.makedirs(os.path.dirname(output), exist_ok=True)
    with open(output, 'w', encoding='utf-8', newline='\n') as handle:
        handle.write(content)

    print(f"[gen_glyphs] generated {output}: {len(codepoints)} codepoints")
    return 0


if __name__ == '__main__':
    sys.exit(main())
