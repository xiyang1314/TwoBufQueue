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

## Show the HTML reports

In repository Settings → Pages → Build and deployment, select **GitHub Actions**. Run the **Publish performance reports** workflow. It packages only the manifest files as a static site, with the root redirecting to `docs/index.html`; GitHub supplies the actual website URL. Put that URL in the repository Website field. Reports, evidence and source links keep their relative paths. Markdown files remain downloadable source documents.

The workflow uses GitHub's [Pages actions](https://docs.github.com/en/pages/getting-started-with-github-pages/using-custom-workflows-with-github-pages). It is manually dispatched so preparing or pushing source alone does not publish the site.

## Package the first release

```sh
python tools/build_release_assets.py --version 0.1.0
```

The `.release-assets` directory contains the source archive, complete packaged measurement evidence and SHA-256 checksums. It is ignored by Git. Keep all original samples; packing is lossless and is verified by `tools/verify_release.py`.

After remote CI passes, tag the intended commit as `v0.1.0`, push the tag and create a GitHub Release. Use `CHANGELOG.md` as release notes and attach the generated files. No tag or public release is created by the exporter.

Suggested repository description: **C++20 two-stage queue with small producer buffers, batched copying and independent callbacks.** Topics: `cpp20`, `spsc`, `logging`, `low-latency`, `message-queue`, `benchmark`. These describe the component; performance conclusions remain specific to the disclosed experiments.
