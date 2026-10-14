# GitHub publication / GitHub 发布

[English](PUBLISHING.md) · [简体中文](PUBLISHING.zh-CN.md)

此目录现在作为唯一正式源码维护。日常修改直接在这里完成，不要用旧开发目录导出来覆盖这里。修改文档后先运行 `node tools/build_readme_html.mjs`，发布前运行 `python3 tools/refresh_release_manifest.py` 和 `python3 tools/verify_release.py`。刷新只处理明确的发布文件和已登记页面，不删除或自动纳入临时文件。

The upload tree is `~/Documents/TwoBufQueueRelease/TwoBufQueue`. The development directory, temporary experiments, original samples and existing destination extras remain on disk. `release-manifest.json` lists publication files; unselected existing destination files are ignored by Git.

## 一、Validate the prepared tree

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

## 二、Create and connect the repository

Create an empty public repository named `TwoBufQueue` under the intended GitHub account. Do not ask GitHub to generate a second README or license. From the upload tree, use the real URL supplied by GitHub:

```sh
git remote add origin <actual-repository-URL>
git push -u origin main
```

If this directory has not been initialized as a Git repository, first initialize `main` and commit the verified manifest files. The preparation record states whether that step has already been done. Credentials belong in the user's Git credential manager or SSH configuration; they are never stored in this tree.

Wait for the **TwoBufQueue** workflow to pass on Linux, Windows and macOS. The local validation record identifies macOS results and marks remote execution as pending; it does not imply other platforms have run.

## 三、Generate all HTML documentation

`README.md` and `README.zh-CN.md` remain the editable source and GitHub repository README. The HTML versions add navigation, responsive performance charts, readable tables and syntax highlighting. Scenario descriptions remain fully visible. API, integration, examples and supporting documents also have HTML reading pages. The pages use local assets and can be opened offline. Source-reading pages use a small local script only for copying code, line wrapping and filtering files; no network requests or external JavaScript libraries are needed.

After editing the Markdown, regenerate all registered HTML pages before refreshing the publication manifest. Add new source documents to `tools/documentation_pages.json`; one command generates the overview, documentation and source-reading pages, then switches local document and code links. Linked examples, headers, Python scripts, assembly and CMake files are rendered under `docs/code/` with syntax colors and line numbers. Source files themselves stay unchanged; the source manifest records their SHA-256 hashes. Only this optional documentation step needs Node 20+ and Marked 17.0.5; the C++ library and viewing the generated pages do not:

```sh
npm install --prefix .deps/readme marked@17.0.5
node tools/build_readme_html.mjs
```

[English HTML overview](../README.md) · [中文 HTML 首页](../README.zh-CN.md) · [代码阅读](code/index.zh-CN.md). On GitHub the HTML file has a source view; use the actual GitHub Pages website URL to present the styled page and add it to the repository Website field. Keep the relative links for local clones until the real published URL is known.

## 四、Show the HTML overview and reports

In repository Settings → Pages → Build and deployment, select **GitHub Actions**. Run the **Publish performance reports** workflow. It packages only the manifest files as a static site, with the root redirecting to `README.html`; GitHub supplies the actual website URL. Put that URL in the repository Website field. Reports, evidence and source links keep their relative paths. Markdown files remain downloadable source documents.

README and the report index feature [continuous sending and business performance](performance_burst_from_empty.zh-CN.md), where every batch starts with zero pending messages. The earlier [prepared-backlog report](performance_burst_cache.md) is labelled as a separate historical experiment. The HTML generator rebuilds both current overview figures from saved results: Session submission throughput for four message sizes, and post-send business cost divided by the fixed 13 ns reference. It also writes SVG figures for the Markdown overview. The historical absolute-cost figure remains available through `python3 tools/plot_burst_release.py`. These commands do not run benchmarks.

The workflow uses GitHub's [Pages actions](https://docs.github.com/en/pages/getting-started-with-github-pages/using-custom-workflows-with-github-pages). It is manually dispatched so preparing or pushing source alone does not publish the site.

## 五、Package the first release

```sh
python tools/build_release_assets.py --version 0.1.0
```

The visible `packages/` directory contains the source archive, complete packaged measurement evidence and SHA-256 checksums. It is ignored by Git. Keep all original samples; packing is lossless and is verified by `tools/verify_release.py`.

After remote CI passes, tag the intended commit as `v0.1.0`, push the tag and create a GitHub Release. Use `CHANGELOG.md` as release notes and attach the generated files. No tag or public release is created by the exporter.

Suggested repository description: **C++ producer–consumer queue with two-layer buffers, batch publication and transfer, and small producer memory use.** Topics: `cpp20`, `spsc`, `logging`, `low-latency`, `message-queue`, `benchmark`. These describe the component; performance conclusions remain specific to the disclosed experiments.


运行 `python3 tools/build_release_assets.py --version 0.1.0` 后，包在可见的 `packages/` 目录：`TwoBufQueue-0.1.0-CN.zip`、`TwoBufQueue-0.1.0-EN.zip`，同时提供解压目录和 `packages/index.html` 下载导航。中文包打开 `index.html` 默认进入中文首页，英文包进入英文首页。两者的 C++ 源码、示例和编译配置完全相同；文档保留两种语言，功能 PR 只修改正式仓库。完整原始中文备份与发布包是两个不同用途的文件。

## 六、GitHub 首页与 HTML 文档网站

仓库首页显示 `README.md`，中文入口显示 `README.zh-CN.md`。网站地址尚未配置时，Markdown 中的文档链接指向 `.md`，代码链接指向实际源码；因此在 GitHub 连续点击多级页面时，不会打开 HTML 源码。网站发布后，可以按照第七章切换到 HTML 网页链接。仅有 HTML 的报告会自动生成 Markdown 阅读副本，文字、表格和性能图取自已保存的报告，不会重跑测试。

运行 `node tools/build_readme_html.mjs` 会同时更新 Markdown 阅读副本和完整 HTML 网站。GitHub 的 README 使用平台自己的排版；固定侧边目录、自定义样式和交互只在 HTML 网站中保留。

创建 GitHub 仓库后，在 **Settings → Pages → Source** 选择 **GitHub Actions**，再运行 **Publish performance reports** 工作流。部署完成后，将仓库的 Website 设置为实际 Pages 地址。需要链接到完整网页时，使用这个网站的 HTTPS 地址，不要使用仓库中的相对 `.html` 文件地址。仓库尚未创建时，不填写虚构的网站地址。

## 七、将 GitHub 文档入口切换到 HTML 网站

完整 HTML 网站的阅读效果更好。仓库中的相对 `.html` 链接会打开源码；要从 GitHub README 进入渲染后的页面，需要链接到已发布的网站。

网站部署完成后，在 `tools/document_site.json` 的 `site_url` 字段填写实际 HTTPS 网站根地址，再运行以下命令：

```sh
node tools/build_readme_html.mjs
python3 tools/refresh_release_manifest.py
python3 tools/verify_release.py
```

程序会把 Markdown 中的文档、语言切换和示例链接改成对应 HTML 网页的完整地址。HTML 文件本身仍使用本地相对链接，下载包也可以离线阅读。`site_url` 为 `null` 时，GitHub 文档继续使用 Markdown，不生成虚构或未发布的网站地址。

生成元数据和章节跳转对应关系保存在工具配置里，阅读文档中不会写入 HTML 注释、隐藏锚点或目录包装标签。
