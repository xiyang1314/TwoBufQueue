# Documentation index

[English](index.md) · [简体中文](index.zh-CN.md) · [Project overview](../README.md)

Start with the API and integration guides. Performance reports, development instructions and source views have separate directories.

## 1. Usage guides

| Document | What it covers |
|---|---|
| [API guide](guides/API.md) | Initialization, writes, WriteSession, flush and receive callbacks |
| [Integration](guides/INTEGRATION.md) | Local source, CMake and installed packages |
| [Async logging example](guides/ASYNC_LOGGING.md) | Passing log records in an application |
| [Example index](../examples/README.md) | Complete examples by use case |
| [Detailed introduction](../README_DETAIL.md) | Performance questions, two-layer design and use cases |

## 2. Performance reports

| Directory | Contents |
|---|---|
| [reports/](reports/index.md) | Report entry point and main comparisons |
| `reports/comparison/` | Ordinary writes, WriteSession and upstream queue comparisons |
| `reports/burst/` | Business performance after a continuous message batch |
| `reports/block_size/` | Block-size sweeps and publication strategies |
| `reports/analysis/` | Cache, latency and implementation investigations |
| [Methodology](reports/PERFORMANCE.md) | Metrics, test conditions and reproduction |

Original measurements remain in `performance_data/`. Source views remain in [code/](code/index.md).

## 3. Development and publication

| Document | Contents |
|---|---|
| [Testing](development/TESTING.md) | Functional tests, benchmark contracts and integration checks |
| [Publication contents](development/RELEASE_CONTENTS.md) | Files included in the publication tree |
| [Publishing](development/PUBLISHING.md) | GitHub repository, Pages and bilingual packages |
| [Contributing](../CONTRIBUTING.md) | Proposing code and documentation changes |
| [Test index](../test/README.md) | Tests grouped by component |
| [Benchmark index](../benchmarks/README.md) | Performance experiments and parameter searches |
