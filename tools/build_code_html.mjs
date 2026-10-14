/** Generate an offline source browser and redirect documentation source links. */
import { readFile, writeFile, readdir, mkdir } from 'node:fs/promises';
import { resolve, dirname, relative, sep, extname } from 'node:path';
import { createHash } from 'node:crypto';
import { escapeCode as escape, languageFor, highlightedLines } from './code_highlight.mjs';

const local = path => path.split(sep).join('/');
const digest = bytes => createHash('sha256').update(bytes).digest('hex');
const isSource = path => /\.(?:cpp|hpp|h|c|cc|cxx|hh|hxx|ipp|inl|py|cmake|asm|s|sh|bash|zsh|txt)$/i.test(path);
const labels = { cpp: 'C++', python: 'Python', cmake: 'CMake', asm: 'Assembly', shell: 'Shell', text: 'Text' };
const categories = [
  ['examples', '使用示例', '从基础收发到批量、并发和同步回调。'],
  ['components', '组件实现', '需要了解底层行为时，再读这些头文件。'],
  ['benchmarks', '性能测试脚本', '测试执行、参数选择与历史结果呈现。'],
  ['assembly', '汇编与指令记录', '性能诊断中保存的实际指令。'],
  ['build', '工程接入配置', '把组件接入应用工程的 CMake 配置。'],
  ['notes', '技术记录', '保存的诊断说明与实验记录。'],
];
const descriptions = {
  'examples/01_basic.cpp': '最小完整流程：创建发送入口，接收消息，再结束发送并等待处理完。',
  'examples/two_buf_queue/02_batch_callback.cpp': '用整批回调接收数据，并读取每个来源范围中的记录。',
  'examples/two_buf_queue/03_multi_thread_writer.cpp': '多个生产线程各用自己的 Writer，后台接收并校验消息。',
  'examples/two_buf_queue/04_layer1_callback.cpp': '直接读取第一层源块，省去第二层复制。',
  'examples/two_buf_queue/05_seq_writer.cpp': '给消息分配序号，并通过回调读取来源、序号和负载。',
  'examples/two_buf_queue/06_sync_writer.cpp': '在生产者线程执行接收回调；write 返回前完成消息处理。',
  'examples/two_buf_queue/07_quick_write_muti_msg.cpp': '用双层循环快速写多条消息：内层写当前块，外层明确选择下一块。',
  'examples/two_buf_queue/08_all_parameters.cpp': '逐项展示 Writer、两层内存池、读取与消费线程的全部参数。',
  'examples/two_buf_queue/09_async_logger.cpp': '把业务日志交给后台写文件，演示缓存满时的处理与安全退出。',
  'examples/components/11_warm_pool.cpp': '借出和归还池中资源，观察资源何时可再次使用。',
  'examples/components/12_warm_pool_factory.cpp': '为内存池提供自定义资源创建和销毁方法。',
  'examples/components/13_spsc_live_handoff_queue.cpp': '读取尚在生产中的资源范围，并在处理完后归还。',
  'examples/components/14_spsc_live_handoff_queue_claim.cpp': '领取队首资源；暂时不能处理时取消领取并保留记录。',
  'examples/components/15_handoff_queue.cpp': '使用内置消费线程处理交接资源，拒绝时等待重试。',
  'examples/components/16_handoff_queue_mpmc.cpp': '多个生产者和消费者共享交接队列，校验并发处理结果。',
  'examples/example_support.hpp': '示例共用的检查、解析与辅助函数。',
  'src/base_components/WarmPool/warm_pool.hpp': '资源池实现：创建、借出、归还和容量控制。',
  'src/base_components/LiveHandoffQueue/spsc_live_handoff_queue.hpp': '单生产者与单消费者之间交接资源，并支持读取正在生产的内容。',
  'src/base_components/HandoffQueue/handoff_queue.hpp': '资源交接队列实现，包含多种并发模式和消费工作线程。',
  'benchmarks/run_release_benchmarks.py': '按配置运行正式测试、校验负载，并保存环境和全部测量记录。',
  'benchmarks/plot_release.py': '从保存的测量结果生成性能图表，不重新执行性能测试。',
  'test/integration/CMakeLists.txt': '验证本地源码、FetchContent 和安装包三种工程接入方式。',
};
function categoryFor(path, language) {
  if (path.startsWith('examples/')) return 'examples';
  if (path.startsWith('src/') || path.startsWith('include/')) return 'components';
  if (language === 'asm') return 'assembly';
  if (language === 'cmake') return 'build';
  if (path.startsWith('benchmarks/') || language === 'python') return 'benchmarks';
  return 'notes';
}
function descriptionFor(path, language) {
  return descriptions[path] || (language === 'asm' ? '性能分析中保存的汇编指令，可按行定位并阅读。'
    : path.startsWith('docs/performance_data/') && language === 'python' ? '保存的性能实验脚本，保留当时的测试与结果处理逻辑。'
    : language === 'text' ? '保存的技术说明与实验记录。' : '文档引用的完整源文件。');
}
function stripRawBodies(html) {
  return html.replace(/(<(?:script|style)\b[^>]*>)([\s\S]*?)(<\/(?:script|style)\s*>)/gi,
    (_, open, body, close) => open + body.replace(/[^\r\n]/g, ' ') + close);
}
function markdownMask(text) {
  const blank = value => value.replace(/[^\r\n]/g, ' ');
  return text.replace(/^[ \t]*(`{3,}|~{3,})[^\n]*\n[\s\S]*?^[ \t]*\1[ \t]*$/gm, blank)
    .replace(/(`+)(?!`)[^\n]*?\1(?!`)/g, blank);
}

export async function buildCodePages({ root }) {
  const sourceList = JSON.parse(await readFile(resolve(root, 'tools/documentation_pages.json'), 'utf8')).sources;
  const documents = new Set(sourceList.map(name => name.replace(/\.md$/, '.html')));
  for (const entry of await readdir(resolve(root, 'docs'), { withFileTypes: true })) {
    if (entry.isFile() && entry.name.endsWith('.html')) documents.add('docs/' + entry.name);
  }
  const oldTargets = new Map();
  const manifestPath = resolve(root, 'docs/code/code-manifest.json');
  try {
    const previous = JSON.parse(await readFile(manifestPath, 'utf8'));
    for (const file of previous.files) oldTargets.set(resolve(root, file.html_path), file.source_path);
  } catch (error) { if (error.code !== 'ENOENT') throw error; }
  const refs = new Map(), titles = new Map();
  function resolveSource(document, href) {
    if (/^(?:[a-z][a-z\d+.-]*:|\/\/)/i.test(href)) return null;
    const parsed = href.match(/^([^?#]*)([?#].*)?$/);
    if (!parsed || !parsed[1]) return null;
    const target = resolve(dirname(document), decodeURIComponent(parsed[1]));
    let source = oldTargets.get(target);
    if (!source) {
      source = local(relative(root, target));
      if (source.startsWith('../') || !isSource(source)) return null;
    }
    return { source, suffix: parsed[2] || '' };
  }
  for (const name of [...documents].sort()) {
    const document = resolve(root, name), html = await readFile(document, 'utf8');
    titles.set(name, (html.match(/<title>([\s\S]*?)<\/title>/i)?.[1] || name).replace(/\s*·\s*TwoBufQueue\s*$/, ''));
    const masked = stripRawBodies(html);
    for (const match of masked.matchAll(/href=["']([^"']+)["']/g)) {
      const resolved = resolveSource(document, match[1]);
      if (!resolved) continue;
      if (!refs.has(resolved.source)) refs.set(resolved.source, new Set());
      refs.get(resolved.source).add(name);
    }
  }
  const files = [];
  for (const sourcePath of [...refs.keys()].sort()) {
    const bytes = await readFile(resolve(root, sourcePath));
    const source = bytes.toString('utf8');
    if (!Buffer.from(source, 'utf8').equals(bytes)) throw new Error('Source file is not UTF-8: ' + sourcePath);
    const language = languageFor(sourcePath);
    files.push({ source_path: sourcePath, html_path: 'docs/code/' + sourcePath + '.html',
      language, category: categoryFor(sourcePath, language), lines: source ? source.split('\n').length - Number(source.endsWith('\n')) : 0,
      bytes: bytes.length, sha256: digest(bytes), description: descriptionFor(sourcePath, language),
      referenced_by: [...refs.get(sourcePath)].sort(), source });
  }
  const urlFrom = (output, name) => local(relative(dirname(output), resolve(root, name)));
  const header = output => '<header class="site-header"><div class="header-inner"><a class="brand" href="'
    + escape(urlFrom(output, 'README.zh-CN.html')) + '"><span class="brand-symbol" aria-hidden="true"><i></i><i></i></span>TwoBufQueue<span class="brand-version">C++</span></a>'
    + '<nav aria-label="主要导航"><a href="' + escape(urlFrom(output, 'README.zh-CN.html')) + '">项目首页</a><a href="'
    + escape(urlFrom(output, 'docs/API.zh-CN.html')) + '">接口指南</a><a href="' + escape(urlFrom(output, 'examples/README.html'))
    + '">使用示例</a><a href="' + escape(urlFrom(output, 'docs/code/index.html')) + '" aria-current="page">代码阅读</a></nav>'
    + '<div class="languages"><a href="' + escape(urlFrom(output, 'README.html')) + '">EN</a><a href="'
    + escape(urlFrom(output, 'README.zh-CN.html')) + '">中文</a></div></div></header>';
  const shell = (output, title, body) => '<!doctype html>\n<html lang="zh-CN"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">'
    + '<title>' + escape(title) + ' · TwoBufQueue</title><link rel="stylesheet" href="' + escape(urlFrom(output, 'docs/assets/readme.css'))
    + '"><link rel="stylesheet" href="' + escape(urlFrom(output, 'docs/assets/code.css')) + '"><script src="'
    + escape(urlFrom(output, 'docs/assets/code_view.js')) + '" defer></script></head><body class="code-page"><a class="skip-link" href="#content">跳到代码</a>'
    + header(output) + body + '</body></html>\n';
  async function save(output, html, marker) {
    try {
      const existing = await readFile(output, 'utf8');
      if (!existing.includes(marker)) throw new Error('Refusing to replace unrelated HTML: ' + local(relative(root, output)));
    } catch (error) { if (error.code !== 'ENOENT') throw error; }
    await mkdir(dirname(output), { recursive: true });
    await writeFile(output, html);
  }
  for (const file of files) {
    const output = resolve(root, file.html_path);
    const lineHtml = highlightedLines(file.source, file.language).map((line, i) => '<span class="source-line" id="L' + (i + 1)
      + '"><a class="line-number" href="#L' + (i + 1) + '" aria-label="第 ' + (i + 1) + ' 行">' + (i + 1)
      + '</a><span class="line-content">' + line + '</span></span>').join('');
    const links = file.referenced_by.map(name => '<a href="' + escape(urlFrom(output, name)) + '">' + titles.get(name) + '</a>').join('');
    const category = categories.find(group => group[0] === file.category)[1];
    const data = JSON.stringify({ source_path: file.source_path, sha256: file.sha256, source: file.source })
      .replace(/[<>&\u2028\u2029]/g, ch => '\\u' + ch.charCodeAt(0).toString(16).padStart(4, '0'));
    const marker = '<!-- Generated source view: ' + file.source_path + '; SHA256 ';
    const body = marker + file.sha256 + '. -->'
      + '<main class="code-main" id="content"><div class="code-breadcrumb"><a href="' + escape(urlFrom(output, 'docs/code/index.html'))
      + '">代码阅读</a><span>/</span><a href="' + escape(urlFrom(output, 'docs/code/index.html') + '#' + file.category) + '">' + category
      + '</a></div><section class="source-intro"><div class="source-kicker">' + labels[file.language] + ' · 完整文件</div><h1>'
      + escape(file.source_path.split('/').at(-1)) + '</h1><p>' + escape(file.description) + '</p><div class="source-path">'
      + escape(file.source_path) + '</div><div class="source-meta"><span>' + file.lines + ' 行</span><span>'
      + (file.bytes / 1024).toFixed(1) + ' KiB</span><span>点击行号可定位这一行</span></div></section>'
      + '<section class="source-panel is-wrapped" aria-label="完整源码"><div class="source-toolbar"><span class="source-tab">'
      + escape(file.source_path.split('/').at(-1)) + '</span><div class="source-controls"><label><input type="checkbox" id="wrap-code" checked>自动换行</label>'
      + '<button type="button" id="copy-code">复制代码</button><span class="copy-status" id="copy-status" role="status" aria-live="polite"></span></div></div>'
      + '<div class="code-scroller"><pre class="source-code" tabindex="0"><code>' + lineHtml + '</code></pre></div></section>'
      + '<section class="source-references"><h2>继续阅读</h2><div>' + links + '</div></section>'
      + '<footer class="site-footer"><span>TwoBufQueue · C++ · Apache-2.0</span><a href="'
      + escape(urlFrom(output, 'docs/code/index.html')) + '">全部代码文件</a></footer></main>'
      + '<script type="application/json" id="source-data">' + data + '</script>';
    await save(output, shell(output, file.source_path.split('/').at(-1), body), marker);
  }
  const index = resolve(root, 'docs/code/index.html');
  const groups = categories.map(([key, title, description]) => {
    const members = files.filter(file => file.category === key);
    if (key === 'examples') members.sort((a, b) => {
      const number = file => Number(file.source_path.split('/').at(-1).match(/^(\d+)_/)?.[1] || 99999);
      return number(a) - number(b) || a.source_path.localeCompare(b.source_path);
    });
    if (!members.length) return '';
    return '<section class="code-group" id="' + key + '"><div class="code-group-heading"><h2>' + title + '</h2><p>' + description
      + '</p></div><div class="code-file-list">' + members.map(file => '<article class="code-file" data-code-file="' + escape(file.source_path.toLowerCase())
        + '"><div><a class="code-file-name" href="' + escape(urlFrom(index, file.html_path)) + '">' + escape(file.source_path.split('/').at(-1))
        + '</a><p>' + escape(file.description) + '</p><span class="code-file-path">' + escape(file.source_path)
        + '</span></div><span class="code-file-info">' + labels[file.language] + '<br>' + file.lines + ' 行</span></article>').join('') + '</div></section>';
  }).join('');
  const indexMarker = '<!-- Generated source index. -->';
  const body = indexMarker + '<main id="content" class="code-index"><section class="code-index-hero"><div class="source-kicker">TWOBUFQUEUE · SOURCE BROWSER</div>'
    + '<h1>代码阅读</h1><p>先看基础示例，再按需要阅读批量、并发与同步回调。每个文件都提供完整代码、语法高亮和行号。</p>'
    + '<div class="code-index-actions"><a class="button primary" href="' + escape(urlFrom(index, 'docs/code/examples/01_basic.cpp.html'))
    + '">从基础示例开始 ↗</a><a class="button secondary" href="' + escape(urlFrom(index, 'docs/API.zh-CN.html'))
    + '">接口使用指南</a></div></section><div class="code-filter"><label for="code-search">搜索代码</label><input id="code-search" type="search" placeholder="搜索文件名或路径…">'
    + '<span id="filter-count" role="status">' + files.length + ' 个文件</span></div><nav class="code-category-nav" aria-label="代码分类">'
    + categories.filter(group => files.some(file => file.category === group[0])).map(group => '<a href="#' + group[0] + '">' + group[1] + '</a>').join('')
    + '</nav>' + groups + '<p class="empty-filter" id="empty-filter" hidden>没有匹配的文件。</p><footer class="site-footer"><span>TwoBufQueue · C++ · Apache-2.0</span>'
    + '<a href="' + escape(urlFrom(index, 'README.zh-CN.html')) + '">项目首页</a></footer></main>';
  await save(index, shell(index, '代码阅读', body), indexMarker);
  const records = files.map(({ source, ...record }) => record);
  await writeFile(manifestPath, JSON.stringify({ schema_version: 1, complete_source: true, files: records }, null, 2) + '\n');
  const targets = new Map(files.map(file => [file.source_path, file.html_path]));
  function replacement(document, href) {
    const resolved = resolveSource(document, href);
    if (!resolved || !targets.has(resolved.source)) return href;
    return local(relative(dirname(document), resolve(root, targets.get(resolved.source)))) + resolved.suffix;
  }
  function redirect(text, document, markdown) {
    const masked = markdown ? markdownMask(text) : stripRawBodies(text);
    const pattern = markdown ? /(?<prefix>\[[^\]\n]*\]\()(?<url>[^)\n]+)(?=\))|(?<html>href=["'])(?<href>[^"']+)(?=["'])/g
      : /(?<html>href=["'])(?<href>[^"']+)(?=["'])/g;
    const changes = [];
    for (const match of masked.matchAll(pattern)) {
      const prefix = match.groups.prefix || match.groups.html;
      const url = match.groups.url || match.groups.href;
      const start = match.index + prefix.length;
      const href = text.slice(start, start + url.length);
      const target = replacement(document, href);
      if (target !== href) changes.push([start, start + url.length, target]);
    }
    for (const [start, end, target] of changes.reverse()) text = text.slice(0, start) + target + text.slice(end);
    return text;
  }
  let markdownChanged = 0, htmlChanged = 0;
  for (const name of sourceList) {
    const document = resolve(root, name), before = await readFile(document, 'utf8');
    const after = redirect(before, document, true);
    if (after !== before) { await writeFile(document, after); ++markdownChanged; }
  }
  for (const name of documents) {
    const document = resolve(root, name), before = await readFile(document, 'utf8');
    const after = redirect(before, document, false);
    if (after !== before) { await writeFile(document, after); ++htmlChanged; }
  }
  console.log('Generated ' + files.length + ' source views; redirected ' + markdownChanged + ' Markdown and ' + htmlChanged + ' HTML link sets.');
  return { files: files.length, markdownChanged, htmlChanged };
}
