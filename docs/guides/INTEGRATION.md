# CMake integration

[English](https://xiyang1314.github.io/TwoBufQueue/docs/guides/INTEGRATION.html) · [简体中文](https://xiyang1314.github.io/TwoBufQueue/docs/guides/INTEGRATION.zh-CN.html)

[English README](https://xiyang1314.github.io/TwoBufQueue/README.html) · [中文首页](https://xiyang1314.github.io/TwoBufQueue/README.zh-CN.html)

All three methods expose the same `TwoBufQueue::TwoBufQueue` target and `<TwoBufQueue.hpp>` header. The target propagates C++20 and thread linkage; consumers do not need to add internal include paths or link a platform synchronization library by hand. macOS consumers require 14.4+; set their deployment target before `project()` or during configure.

When embedded, tests and examples are disabled by default; a standalone build enables the current tests and examples. Do not name your application target `TwoBufQueue`.

## 1. Vendored source

Place the checkout under `external/TwoBufQueue`:

```cmake
cmake_minimum_required(VERSION 3.20)
project(my_app LANGUAGES CXX)
add_subdirectory(external/TwoBufQueue)
add_executable(my_app main.cpp)
target_link_libraries(my_app PRIVATE TwoBufQueue::TwoBufQueue)
```

Use `BRQ_INSTALL=OFF` before `add_subdirectory` if your application should not install the dependency. Override `BRQ_BUILD_TESTS` / `BRQ_BUILD_EXAMPLES` explicitly if you want its tests and examples in the parent build.

## 2. FetchContent

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

For a remote dependency, use the actual repository URL and a pinned commit rather than a moving branch.

## 3. Installed package

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
find_package(TwoBufQueue 0.6 CONFIG REQUIRED)
add_executable(my_app main.cpp)
target_link_libraries(my_app PRIVATE TwoBufQueue::TwoBufQueue)
```

Configure the consumer with `-DCMAKE_PREFIX_PATH=/absolute/path/to/install`. On macOS add `-DCMAKE_OSX_DEPLOYMENT_TARGET=14.4` when needed. Libraries, real headers, CMake package files, LICENSE and NOTICE are installed. The prefix may be moved as a whole: installed includes contain no source-tree forwarders. A compiled package is specific to its platform, architecture and compatible toolchain; this does not make binaries portable between operating systems.

After installation, the application no longer depends on the original source directory. Move the complete install prefix and update `CMAKE_PREFIX_PATH`. The package version is 0.6.1 and accepts compatible versions within the same minor series; this does not imply that a GitHub Release has been created.

The [integration consumer](https://xiyang1314.github.io/TwoBufQueue/docs/code/test/integration/CMakeLists.txt.html) is runnable through `BRQ_INTEGRATION_MODE=add_subdirectory`, `fetchcontent`, or `package`. Example source-mode check:

```sh
cmake -S test/integration -B build-consumer -DBRQ_SOURCE_DIR=../.. -DBRQ_INTEGRATION_MODE=fetchcontent
cmake --build build-consumer --config Release --parallel
ctest --test-dir build-consumer -C Release --output-on-failure
```

For package mode, omit `BRQ_SOURCE_DIR`, use `-DBRQ_INTEGRATION_MODE=package` and set `CMAKE_PREFIX_PATH` to the installed prefix.

Package export follows CMake's [Importing and Exporting Guide](https://cmake.org/cmake/help/latest/guide/importing-exporting/index.html); source population uses its [FetchContent module](https://cmake.org/cmake/help/latest/module/FetchContent.html).
