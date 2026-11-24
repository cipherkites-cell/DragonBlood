#pragma once

// Windows.h 必须在 CommonLibSSE 之前进来（和 Skygwent.dll 一样的顺序），
// 否则 MagelightUI_API.h 里的 Win32 声明会和已经展开的 CommonLib 头冲突。
#ifndef WIN32_LEAN_AND_MEAN
#	define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#	define NOMINMAX
#endif
#include <Windows.h>

// Windows.h 把 GetObject 定义成 GetObjectA/W，会盖掉
// RE::BGSDefaultObjectManager::GetObject —— 在预编译头里一次性清掉，
// 保证任何编译单元都不会再看到这个宏。
#ifdef GetObject
#	undef GetObject
#endif
#ifdef GetObjectA
#	undef GetObjectA
#endif
#ifdef GetObjectW
#	undef GetObjectW
#endif

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
