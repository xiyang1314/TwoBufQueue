# 把 TwoBufQueue 接入工程

当前需要 C++20、CMake 3.20+ 和线程支持。应用只需包含 `<TwoBufQueue.hpp>`，并链接 `TwoBufQueue::TwoBufQueue`；不需要自己添加内部头文件路径。macOS 当前要求 14.4+ 和支持相应接口的 SDK。

[中文首页](../README.zh-CN.html) · [接口使用指南](API.zh-CN.html) · [English integration](INTEGRATION.html)

## 最简单：源码放在工程里

把 TwoBufQueue 放到应用的 `external/TwoBufQueue` 目录，在应用的 CMakeLists.txt 中加入：

```cmake
cmake_minimum_required(VERSION 3.20)
project(my_app LANGUAGES CXX)
add_subdirectory(external/TwoBufQueue)
add_executable(my_app main.cpp)
target_link_libraries(my_app PRIVATE TwoBufQueue::TwoBufQueue)
```

`main.cpp` 使用接口指南中的完整例子即可。被其他工程引用时，库的测试和示例默认不构建；编译标准和线程链接由 target 自动传给应用。应用自己的 target 不要命名为 `TwoBufQueue`。

## 已安装：使用 find_package

先在库目录构建并安装到一个有写权限的位置：

```sh
cmake -S . -B build-package -DCMAKE_BUILD_TYPE=Release -DBRQ_BUILD_TESTS=OFF -DBRQ_BUILD_EXAMPLES=OFF
cmake --build build-package --config Release --parallel
cmake --install build-package --config Release --prefix ./install
```

应用的 CMakeLists.txt：

```cmake
cmake_minimum_required(VERSION 3.20)
project(my_app LANGUAGES CXX)
find_package(TwoBufQueue 0.1 CONFIG REQUIRED)
add_executable(my_app main.cpp)
target_link_libraries(my_app PRIVATE TwoBufQueue::TwoBufQueue)
```

配置应用时，用 `-DCMAKE_PREFIX_PATH=/安装目录的绝对路径` 告诉 CMake 包在哪里。整个安装目录可以移动，之后更新这个路径即可；已经编译的库仍须匹配操作系统、CPU 架构和工具链。

## 使用 FetchContent 管理源码

本地源码也可以这样接入，不需要网络：

```cmake
include(FetchContent)
FetchContent_Declare(TwoBufQueue
    SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/external/TwoBufQueue")
FetchContent_MakeAvailable(TwoBufQueue)
target_link_libraries(my_app PRIVATE TwoBufQueue::TwoBufQueue)
```

此处假定应用 target `my_app` 已创建。远程仓库发布后，可换成实际 `GIT_REPOSITORY` 和固定提交号 `GIT_TAG`；不要用不断变化的分支代替可复现版本。远程写法及接入自测见[完整接入说明](INTEGRATION.html)。

## 常见问题

| 遇到的问题 | 检查什么 |
|---|---|
| 找不到头文件 | 包含公共头文件，并链接上述 target；不要手动引用开发机器的路径 |
| find_package 找不到包 | `CMAKE_PREFIX_PATH` 是否指向完整安装目录 |
| macOS 编译接口不可用 | 系统与 SDK 是否支持 14.4+；必要时配置 `CMAKE_OSX_DEPLOYMENT_TARGET=14.4` |
| 不想构建库的测试或示例 | 设置 `BRQ_BUILD_TESTS=OFF`、`BRQ_BUILD_EXAMPLES=OFF` |

下一步看[接口指南](API.zh-CN.html)或直接运行[基础示例](code/examples/01_basic.cpp.html)。
