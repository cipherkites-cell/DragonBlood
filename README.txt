DragonBloodAudio 源码（结构版）
目录就是仓库根，直接整目录拖到 GitHub 的上传页即可：
  CMakeLists.txt      构建脚本（源文件在 src/main.cpp）
  vcpkg.json          依赖（commonlibsse-ng）
  src/main.cpp        插件本体
  Scripts/Source/DragonBloodAudio.psc   Papyrus 接口声明（不进编译，仅供对照）
  .github/workflows/build.yml           GitHub Actions 工作流（必须存在，否则没得跑）
