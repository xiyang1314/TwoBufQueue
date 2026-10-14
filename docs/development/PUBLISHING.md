# GitHub publication

[English](https://xiyang1314.github.io/TwoBufQueue/docs/development/PUBLISHING.html) · [简体中文](https://xiyang1314.github.io/TwoBufQueue/docs/development/PUBLISHING.zh-CN.html)

The upload tree is `~/Documents/TwoBufQueueRelease/TwoBufQueue`. The development directory, temporary experiments, original samples and existing destination extras remain on disk. `release-manifest.json` lists publication files; unselected existing destination files are ignored by Git.

## 1. Validate the prepared tree

```sh
python tools/verify_release.py
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release --parallel 2
ctest --test-dir build -C Release --output-on-failure
```

Run configurable benchmarks only after explicitly downloading their pinned, hash-checked benchmark dependencies:

```sh
python benchmarks/spsc/bootstrap_spsc.py
cmake -S . -B build-bench -DCMAKE_BUILD_TYPE=Release -DBRQ_BUILD_SPSC_BENCHMARKS=ON -DBRQ_ENABLE_POOL_DEPTH_STATS=ON
cmake --build build-bench --config Release --parallel 2
ctest --test-dir build-bench -C Release --output-on-failure -L performance_harness
```

Use an optimized Release build with pool-depth statistics disabled for formal performance measurements. An instrumented build is for statistics and correctness checks, not for replacing published performance scores.

## 2. Maintain this repository directly

This is the canonical shared source tree. Edit it directly for normal fixes and pull requests; do not export an older development copy over it. The export command is only for creating a separate publication copy from an intended source tree.

After source or documentation changes, regenerate the reading pages, refresh the explicit publication allowlist, and verify it:

```sh
node tools/build_readme_html.mjs
python3 tools/refresh_release_manifest.py
python3 tools/verify_release.py
```

The refresh updates current file sizes and hashes, adds registered document/code pages, and retains original export provenance. It never scans temporary files into the publication or deletes them. Existing measurement evidence remains unchanged. For an ordinary code PR, CI checks the working tree with `--skip-manifest`; refresh the manifest before Pages deployment or release packaging.

## 3. Create and connect the repository

Create an empty public repository named `TwoBufQueue` under the intended GitHub account. Do not ask GitHub to generate a second README or license. From the upload tree, use the real URL supplied by GitHub:

```sh
git remote add origin <actual-repository-URL>
git push -u origin main
```

If this directory has not been initialized as a Git repository, first initialize `main` and commit the verified manifest files. The preparation record states whether that step has already been done. Credentials belong in the user's Git credential manager or SSH configuration; they are never stored in this tree.

Wait for the **TwoBufQueue** workflow to pass on Linux, Windows and macOS. The local validation record identifies macOS results and marks remote execution as pending; it does not imply other platforms have run.

## 4. Generate all HTML documentation

`README.md` and `README.zh-CN.md` remain the editable source and GitHub repository README. The HTML versions add navigation, responsive performance charts, readable tables and syntax highlighting. Scenario descriptions remain fully visible. API, integration, examples and supporting documents also have HTML reading pages. The pages use local assets and can be opened offline. Source-reading pages use a small local script only for copying code, line wrapping and filtering files; no network requests or external JavaScript libraries are needed.

After editing the Markdown, regenerate all registered HTML pages before refreshing the publication manifest. Add new source documents to `tools/documentation_pages.json`; one command generates the overview, documentation and source-reading pages, then switches local document and code links. Linked examples, headers, Python scripts, assembly and CMake files are rendered under `docs/code/` with syntax colors and line numbers. Source files themselves stay unchanged; the source manifest records their SHA-256 hashes. Only this optional documentation step needs Node 20+ and Marked 17.0.5; the C++ library and viewing the generated pages do not:

```sh
npm install --prefix .deps/readme marked@17.0.5
node tools/build_readme_html.mjs
```

[English HTML overview](https://xiyang1314.github.io/TwoBufQueue/README.html) · [Chinese HTML overview](https://xiyang1314.github.io/TwoBufQueue/README.zh-CN.html) · [Source browser](https://xiyang1314.github.io/TwoBufQueue/docs/code/index.html). On GitHub the HTML file has a source view; use the actual GitHub Pages website URL to present the styled page and add it to the repository Website field. Keep the relative links for local clones until the real published URL is known.

## 5. Show the HTML overview and reports

In repository Settings → Pages → Build and deployment, select **GitHub Actions**. Run the **Publish performance reports** workflow. It packages only the manifest files as a static site, with the root redirecting to `README.html`; GitHub supplies the actual website URL. Put that URL in the repository Website field. Reports, evidence and source links keep their relative paths. Markdown files remain downloadable source documents.

README and the report index feature [continuous sending and business performance](https://xiyang1314.github.io/TwoBufQueue/docs/reports/burst/performance_burst_from_empty.html), where every batch starts with zero pending messages. The earlier [prepared-backlog report](https://xiyang1314.github.io/TwoBufQueue/docs/reports/burst/performance_burst_cache.html) is labelled as a separate historical experiment. The HTML generator rebuilds both current overview figures from saved results: Session submission throughput for four message sizes, and post-send business cost divided by the fixed 13 ns reference. It also writes SVG figures for the Markdown overview. The historical absolute-cost figure remains available through `python3 tools/plot_burst_release.py`. These commands do not run benchmarks.

The workflow uses GitHub's [Pages actions](https://docs.github.com/en/pages/getting-started-with-github-pages/using-custom-workflows-with-github-pages). It is manually dispatched so preparing or pushing source alone does not publish the site.

## 6. Package the first release

```sh
python tools/build_release_assets.py --version 0.1.0
```

The visible `packages/` directory contains the source archive, complete packaged measurement evidence and SHA-256 checksums. It is ignored by Git. Keep all original samples; packing is lossless and is verified by `tools/verify_release.py`.

After remote CI passes, tag the intended commit as `v0.1.0`, push the tag and create a GitHub Release. Use `CHANGELOG.md` as release notes and attach the generated files. No tag or public release is created by the exporter.

Suggested repository description: **C++ producer–consumer queue with two-layer buffers, batch publication and transfer, and small producer memory use.** Topics: `cpp20`, `spsc`, `logging`, `low-latency`, `message-queue`, `benchmark`. These describe the component; performance conclusions remain specific to the disclosed experiments.


The visible `packages/` directory contains `TwoBufQueue-0.1.0-EN.zip` and `TwoBufQueue-0.1.0-CN.zip`, unpacked copies, and an HTML download index. Open `index.html` inside either package for its default language. Both contain the same canonical C++ code and both documentation languages; they are generated release artifacts rather than independently maintained implementations. Submit feature changes against the shared repository. ZIPs belong in GitHub Release attachments; generated packages are ignored by Git. The separate original Chinese backup is not the CN release package.

## 7. Repository README and the HTML documentation site

The repository homepage displays `README.md`; its Chinese link opens `README.zh-CN.md`. Before a website URL is configured, Markdown documentation links target `.md` files and code links target actual source files, so following several pages on GitHub does not open HTML source. After publishing the website, follow section 8 to switch these links to HTML webpages. HTML-only reports get generated Markdown reading copies with text, tables and charts from the saved reports; generation does not rerun benchmarks.

`node tools/build_readme_html.mjs` updates both GitHub reading copies and the full HTML site. GitHub controls README layout; the fixed sidebar, custom styles and interactions are available in the HTML site.

After creating the GitHub repository, select **GitHub Actions** under **Settings → Pages → Source**, then run **Publish performance reports**. Once deployed, set the repository Website field to the actual Pages URL. Links to the full website must use its HTTPS address instead of relative `.html` repository paths. Do not invent a site URL before the repository exists.

## 8. Link GitHub documentation to the HTML site

The full HTML site provides the richer reading experience. Relative `.html` repository links open source; GitHub README links must use the published website URL to open rendered pages.

After deployment, set `site_url` in `tools/document_site.json` to the actual HTTPS site root, then run:

```sh
node tools/build_readme_html.mjs
python3 tools/refresh_release_manifest.py
python3 tools/verify_release.py
```

The generator switches Markdown documentation, language and example links to their HTML website URLs. HTML files keep relative local links so downloaded packages remain usable offline. With `site_url` set to `null`, GitHub reading links use Markdown; no invented or unpublished URL is generated.

Generation metadata and chapter-link mappings stay in tool configuration rather than HTML comments, hidden anchors or contents wrappers in the reading documents.
