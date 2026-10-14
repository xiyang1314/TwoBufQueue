# GitHub publication / GitHub 发布

The upload tree is `~/Documents/TwoBufQueueRelease/TwoBufQueue`. The development directory, temporary experiments, original samples and existing destination extras remain on disk. `release-manifest.json` lists publication files; unselected existing destination files are ignored by Git.

## Validate the prepared tree

```sh
python tools/verify_release.py
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release --parallel 2
ctest --test-dir build -C Release --output-on-failure
```

Run configurable benchmarks only after explicitly downloading their pinned, hash-checked benchmark dependencies:

```sh
python benchmarks/bootstrap_spsc.py
cmake -S . -B build-bench -DCMAKE_BUILD_TYPE=Release -DBRQ_BUILD_SPSC_BENCHMARKS=ON -DBRQ_ENABLE_POOL_DEPTH_STATS=ON
cmake --build build-bench --config Release --parallel 2
ctest --test-dir build-bench -C Release --output-on-failure -L performance_harness
```

正式性能测量必须使用未启用深度统计的 Release 构建；开启统计的构建用于统计测试和正确性检查，不用于替换已发布成绩。

## Create and connect the repository

Create an empty public repository named `TwoBufQueue` under the intended GitHub account. Do not ask GitHub to generate a second README or license. From the upload tree, use the real URL supplied by GitHub:

```sh
git remote add origin <actual-repository-URL>
git push -u origin main
```

If this directory has not been initialized as a Git repository, first initialize `main` and commit the verified manifest files. The preparation record states whether that step has already been done. Credentials belong in the user's Git credential manager or SSH configuration; they are never stored in this tree.

Wait for the **TwoBufQueue** workflow to pass on Linux, Windows and macOS. The local validation record identifies macOS results and marks remote execution as pending; it does not imply other platforms have run.

## Generate all HTML documentation

`README.md` and `README.zh-CN.md` remain the editable source and GitHub repository README. The HTML versions add navigation, responsive performance charts, readable tables and syntax highlighting. Scenario descriptions remain fully visible. API, integration, examples and supporting documents also have HTML reading pages. The pages use local assets and can be opened offline. Source-reading pages use a small local script only for copying code, line wrapping and filtering files; no network requests or external JavaScript libraries are needed.

After editing the Markdown, regenerate all registered HTML pages before refreshing the publication manifest. Add new source documents to `tools/documentation_pages.json`; one command generates the overview, documentation and source-reading pages, then switches local document and code links. Linked examples, headers, Python scripts, assembly and CMake files are rendered under `docs/code/` with syntax colors and line numbers. Source files themselves stay unchanged; the source manifest records their SHA-256 hashes. Only this optional documentation step needs Node 20+ and Marked 17.0.5; the C++ library and viewing the generated pages do not:

```sh
npm install --prefix .deps/readme marked@17.0.5
node tools/build_readme_html.mjs
```

[English HTML overview](../README.html) · [中文 HTML 首页](../README.zh-CN.html) · [代码阅读](code/index.html). On GitHub the HTML file has a source view; use the actual GitHub Pages website URL to present the styled page and add it to the repository Website field. Keep the relative links for local clones until the real published URL is known.

## Show the HTML overview and reports

In repository Settings → Pages → Build and deployment, select **GitHub Actions**. Run the **Publish performance reports** workflow. It packages only the manifest files as a static site, with the root redirecting to `README.html`; GitHub supplies the actual website URL. Put that URL in the repository Website field. Reports, evidence and source links keep their relative paths. Markdown files remain downloadable source documents.

README and the report index feature [continuous sending and business performance](performance_burst_from_empty.html), where every batch starts with zero pending messages. The earlier [prepared-backlog report](performance_burst_cache.html) is labelled as a separate historical experiment. The HTML generator rebuilds both current overview figures from saved results: Session submission throughput for four message sizes, and post-send business cost divided by the fixed 13 ns reference. It also writes SVG figures for the Markdown overview. The historical absolute-cost figure remains available through `python3 tools/plot_burst_release.py`. These commands do not run benchmarks.

The workflow uses GitHub's [Pages actions](https://docs.github.com/en/pages/getting-started-with-github-pages/using-custom-workflows-with-github-pages). It is manually dispatched so preparing or pushing source alone does not publish the site.

## Package the first release

```sh
python tools/build_release_assets.py --version 0.1.0
```

The `.release-assets` directory contains the source archive, complete packaged measurement evidence and SHA-256 checksums. It is ignored by Git. Keep all original samples; packing is lossless and is verified by `tools/verify_release.py`.

After remote CI passes, tag the intended commit as `v0.1.0`, push the tag and create a GitHub Release. Use `CHANGELOG.md` as release notes and attach the generated files. No tag or public release is created by the exporter.

Suggested repository description: **C++ producer–consumer queue with two-layer buffers, batch publication and transfer, and small producer memory use.** Topics: `cpp20`, `spsc`, `logging`, `low-latency`, `message-queue`, `benchmark`. These describe the component; performance conclusions remain specific to the disclosed experiments.
