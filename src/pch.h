#pragma once

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
