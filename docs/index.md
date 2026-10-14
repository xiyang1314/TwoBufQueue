# Documentation index

[English](https://xiyang1314.github.io/TwoBufQueue/docs/index.html) · [简体中文](https://xiyang1314.github.io/TwoBufQueue/docs/index.zh-CN.html) · [Project overview](https://xiyang1314.github.io/TwoBufQueue/README.html)

Start with the API and integration guides. Performance reports, development instructions and source views have separate directories.

## 1. Usage guides

| Document | What it covers |
|---|---|
| [API guide](https://xiyang1314.github.io/TwoBufQueue/docs/guides/API.html) | Initialization, writes, WriteSession, flush and receive callbacks |
| [Integration](https://xiyang1314.github.io/TwoBufQueue/docs/guides/INTEGRATION.html) | Local source, CMake and installed packages |
| [Async logging example](https://xiyang1314.github.io/TwoBufQueue/docs/guides/ASYNC_LOGGING.html) | Passing log records in an application |
| [Example index](https://xiyang1314.github.io/TwoBufQueue/examples/README.html) | Complete examples by use case |
| [Detailed introduction](https://xiyang1314.github.io/TwoBufQueue/README_DETAIL.html) | Performance questions, two-layer design and use cases |

## 2. Performance reports

| Directory | Contents |
|---|---|
| [reports/](https://xiyang1314.github.io/TwoBufQueue/docs/reports/index.html) | Report entry point and main comparisons |
| `reports/comparison/` | Ordinary writes, WriteSession and upstream queue comparisons |
| `reports/burst/` | Business performance after a continuous message batch |
| `reports/block_size/` | Block-size sweeps and publication strategies |
| `reports/analysis/` | Cache, latency and implementation investigations |
| [Methodology](https://xiyang1314.github.io/TwoBufQueue/docs/reports/PERFORMANCE.html) | Metrics, test conditions and reproduction |

Original measurements remain in `performance_data/`. Source views remain in [code/](https://xiyang1314.github.io/TwoBufQueue/docs/code/index.html).

## 3. Development and publication

| Document | Contents |
|---|---|
| [Testing](https://xiyang1314.github.io/TwoBufQueue/docs/development/TESTING.html) | Functional tests, benchmark contracts and integration checks |
| [Publication contents](https://xiyang1314.github.io/TwoBufQueue/docs/development/RELEASE_CONTENTS.html) | Files included in the publication tree |
| [Publishing](https://xiyang1314.github.io/TwoBufQueue/docs/development/PUBLISHING.html) | GitHub repository, Pages and bilingual packages |
| [Contributing](https://xiyang1314.github.io/TwoBufQueue/CONTRIBUTING.html) | Proposing code and documentation changes |
| [Test index](https://xiyang1314.github.io/TwoBufQueue/test/README.html) | Tests grouped by component |
| [Benchmark index](https://xiyang1314.github.io/TwoBufQueue/benchmarks/README.html) | Performance experiments and parameter searches |
