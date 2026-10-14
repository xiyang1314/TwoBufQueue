# CMake integration / CMake 接入

[English README](../README.html) · [中文首页](../README.zh-CN.html)

All three methods expose the same `TwoBufQueue::TwoBufQueue` target and `<TwoBufQueue.hpp>` header. The target propagates C++20 and thread linkage; consumers do not need to add internal include paths or link a platform synchronization library by hand. macOS consumers require 14.4+; set their deployment target before `project()` or during configure.

三种方式使用相同 target 和公共头文件。嵌入时默认不构建测试和示例；独立构建时默认构建全部当前测试和示例。不要将自己的 target 命名为 `TwoBufQueue`。

## Vendored source / 本地源码

Place the checkout under `external/TwoBufQueue`:

```cmake
cmake_minimum_required(VERSION 3.20)
project(my_app LANGUAGES CXX)
add_subdirectory(external/TwoBufQueue)
add_executable(my_app main.cpp)
target_link_libraries(my_app PRIVATE TwoBufQueue::TwoBufQueue)
```

Use `BRQ_INSTALL=OFF` before `add_subdirectory` if your application should not install the dependency. Override `BRQ_BUILD_TESTS` / `BRQ_BUILD_EXAMPLES` explicitly if you want its tests and examples in the parent build.

## FetchContent

For a local checkout, this is fully offline and needs no unpublished GitHub address:

```cmake
include(FetchContent)
FetchContent_Declare(TwoBufQueue SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/external/TwoBufQueue")
FetchContent_MakeAvailable(TwoBufQueue)
add_executable(my_app main.cpp)
target_link_libraries(my_app PRIVATE TwoBufQueue::TwoBufQueue)
```

When a repository is published, replace the declaration above with the actual URL and a pinned commit. No repository URL or release tag is invented here:

```cmake
set(TWOBUFQUEUE_REPOSITORY "" CACHE STRING "Published repository URL")
set(TWOBUFQUEUE_REVISION "" CACHE STRING "Pinned full commit hash")
if(NOT TWOBUFQUEUE_REPOSITORY OR NOT TWOBUFQUEUE_REVISION)
  message(FATAL_ERROR "Set TWOBUFQUEUE_REPOSITORY and TWOBUFQUEUE_REVISION")
endif()
FetchContent_Declare(TwoBufQueue
  GIT_REPOSITORY "${TWOBUFQUEUE_REPOSITORY}"
  GIT_TAG "${TWOBUFQUEUE_REVISION}")
FetchContent_MakeAvailable(TwoBufQueue)
```

远程接入时填写实际仓库地址和固定提交，不要用会持续变化的分支作为可复现依赖版本。

## Installed package / 安装后的 find_package

From the library's root, install to a writable prefix:

```sh
cmake -S . -B build-package -DCMAKE_BUILD_TYPE=Release -DBRQ_BUILD_TESTS=OFF -DBRQ_BUILD_EXAMPLES=OFF
cmake --build build-package --config Release --parallel
cmake --install build-package --config Release --prefix ./install
```

Consumer `CMakeLists.txt`:

```cmake
cmake_minimum_required(VERSION 3.20)
project(my_app LANGUAGES CXX)
find_package(TwoBufQueue 0.1 CONFIG REQUIRED)
add_executable(my_app main.cpp)
target_link_libraries(my_app PRIVATE TwoBufQueue::TwoBufQueue)
```

Configure the consumer with `-DCMAKE_PREFIX_PATH=/absolute/path/to/install`. On macOS add `-DCMAKE_OSX_DEPLOYMENT_TARGET=14.4` when needed. Libraries, real headers, CMake package files, LICENSE and NOTICE are installed. The prefix may be moved as a whole: installed includes contain no source-tree forwarders. A compiled package is specific to its platform, architecture and compatible toolchain; this does not make binaries portable between operating systems.

安装完成后，应用无需依赖原源码目录。将整个安装目录搬到另一个路径后，更新 `CMAKE_PREFIX_PATH` 即可。包版本为 0.1.0，当前版本文件只接受相同 minor 版本；这不表示已经创建了 GitHub Release。

The [integration consumer](code/test/integration/CMakeLists.txt.html) is runnable through `BRQ_INTEGRATION_MODE=add_subdirectory`, `fetchcontent`, or `package`. Example source-mode check:

```sh
cmake -S test/integration -B build-consumer -DBRQ_SOURCE_DIR=../.. -DBRQ_INTEGRATION_MODE=fetchcontent
cmake --build build-consumer --config Release --parallel
ctest --test-dir build-consumer -C Release --output-on-failure
```

For package mode, omit `BRQ_SOURCE_DIR`, use `-DBRQ_INTEGRATION_MODE=package` and set `CMAKE_PREFIX_PATH` to the installed prefix.

Package export follows CMake's [Importing and Exporting Guide](https://cmake.org/cmake/help/latest/guide/importing-exporting/index.html); source population uses its [FetchContent module](https://cmake.org/cmake/help/latest/module/FetchContent.html).
