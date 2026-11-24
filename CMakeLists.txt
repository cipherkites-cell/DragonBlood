cmake_minimum_required(VERSION 3.24)
project(DragonBlood VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ---- CommonLibSSE-NG：直接用源码编译（不做 vcpkg 叠加端口，避免版本对不上）
# CI 里会用 git clone 把 alandtse/CommonLibSSE-NG 的 ng 分支放到 extern/CommonLibSSE-NG；
# 本机编也一样，先 clone 再 cmake。它会生成 SKSEPluginInfo，并链接 CommonLibSSE::CommonLibSSE。
set(ENABLE_SKYRIM_SE ON CACHE BOOL "" FORCE)
set(ENABLE_SKYRIM_AE ON CACHE BOOL "" FORCE)
set(ENABLE_SKYRIM_VR OFF CACHE BOOL "" FORCE)
set(BUILD_TESTS OFF CACHE BOOL "" FORCE)
add_subdirectory(extern/CommonLibSSE-NG EXCLUDE_FROM_ALL)
include(extern/CommonLibSSE-NG/cmake/CommonLibSSE.cmake)

# USE_ADDRESS_LIBRARY：按 Address Library 定位引擎函数（需要玩家装 Address Library for SKSE Plugins）
add_commonlibsse_plugin(${PROJECT_NAME}
	AUTHOR "cipherkites-cell"
	EMAIL "noreply@localhost"
	USE_ADDRESS_LIBRARY
	SOURCES src/main.cpp)

target_precompile_headers(${PROJECT_NAME} PRIVATE src/pch.h)
target_compile_definitions(${PROJECT_NAME} PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)

if(MSVC)
	# /utf-8：源码里有中文字面量（音频类别的中文名兜底匹配）
	target_compile_options(${PROJECT_NAME} PRIVATE /utf-8 /permissive- /Zc:preprocessor)
	target_link_options(${PROJECT_NAME} PRIVATE "$<$<CONFIG:Release>:/DEBUG:FULL;/OPT:REF;/OPT:ICF>")
endif()
