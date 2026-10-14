# Contributing

[English](CONTRIBUTING.md) · [简体中文](CONTRIBUTING.zh-CN.md)

TwoBufQueue maintains **one source tree** for all users. Fork and clone this repository, create a topic branch, and submit a pull request against the main branch. Source files, public identifiers and build targets are shared across documentation languages; do not maintain separate Chinese and English copies of the library.

## 1. Report a problem

Provide a minimal reproducer and include the operating system and architecture, compiler version, commit id, source/output block sizes and counts, Writer publication policy, reader configuration, callback behavior, expected result and actual result. For lifecycle issues, describe shutdown order, resource ownership and when each resource is returned. Do not include private application data.

## 2. Build and check your change

Requires C++20, CMake 3.20+ and Python 3.9+. The macOS implementation requires macOS 14.4 or newer.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release --parallel
ctest --test-dir build -C Release --output-on-failure
```

Run the current tests and examples before submitting. A behavior fix should include a focused regression test. State what you actually ran and any untested limits in the pull request. Performance thresholds on shared CI are not reliable regression gates. For an optimization, provide before/after commands, matching workloads and settings, environment information and all raw results; explain memory, thread and latency tradeoffs in the [benchmark methodology](docs/reports/PERFORMANCE.md).

## 3. Code and language conventions

- Write source comments, public diagnostics and command-line help in English. Keep names consistent with the [naming guide](NAMING.md).
- Documentation is available in English and Simplified Chinese. Update the relevant language source; maintainers can help synchronize the other version. Code fixes do not require contributors to create a second implementation or translate every document.
- Preserve callback acknowledgement, resource ownership and per-producer ordering. Explain any allocation, blocking or shutdown change on a hot path.
- Update runnable examples and public API documentation when changing the interface.
- Generated HTML and source-reading pages are reading artifacts. Edit the actual source file or Markdown, then regenerate them; never contribute a code fix by editing only its HTML display.

## 4. Regenerate documentation

The C++ library has no documentation-tool runtime dependency. To rebuild the checked-in HTML pages, use Node 20+ with the optional Marked parser:

```sh
npm install --prefix .deps/readme marked@17.0.5
node tools/build_readme_html.mjs
```

This updates the overview, registered documentation and syntax-highlighted source views. Before packaging or deploying Pages, run `python3 tools/refresh_release_manifest.py` and `python3 tools/verify_release.py`. Original measurement data and measured source archives remain historical evidence and must not be rewritten as if they came from the current source tree. See [publication steps](docs/development/PUBLISHING.md) for manifest and packaging checks.
