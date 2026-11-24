#pragma once

// 顺序要求：CommonLibSSE-NG 的 REX/W32/BASE.h 只要发现 Windows API 头在它之前
// 被包含就会 #error，所以预编译头里只放 CommonLib。
// Windows.h 由 MagelightUI_API.h 带进来，放在 main.cpp 的 CommonLib 之后，
// 紧跟其后的 #undef 把 GetObject 宏清掉。
#include <RE/Skyrim.h>
#include <SKSE/SKSE.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <map>
#include <mutex>
#include <string>

#include <fmt/format.h>

// CMake 生成的插件声明文件在全局作用域用了 "..."sv 字面量，这里补上
using namespace std::literals;
