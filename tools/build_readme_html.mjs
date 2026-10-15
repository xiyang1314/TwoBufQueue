/**
 * Build offline HTML overviews and all registered documentation pages.
 * Optional documentation dependency: Node 20+ and marked@17.0.5.
 * npm install --prefix .deps/readme marked@17.0.5
 * node tools/build_readme_html.mjs
 */
import { readFile, writeFile } from 'node:fs/promises';
import { createHash } from 'node:crypto';
import { createRequire } from 'node:module';
import { dirname, resolve } from 'node:path';
import { fileURLToPath, pathToFileURL } from 'node:url';
import { buildDocuments } from './build_docs_html.mjs';
import { buildCodePages } from './build_code_html.mjs';
import { documentSite } from './document_site.mjs';

const args = process.argv.slice(2);
let root = resolve(dirname(fileURLToPath(import.meta.url)), '..');
let modulePath;
for (let i = 0; i < args.length; ++i) {
  if (args[i] === '--root' && args[i + 1]) root = resolve(args[++i]);
  else if (args[i] === '--marked-module' && args[i + 1]) modulePath = resolve(args[++i]);
  else throw new Error('Supported options: --root PATH, --marked-module PATH');
}
if (Number(process.versions.node.split('.')[0]) < 20) throw new Error('Use Node 20 or newer.');
if (!modulePath) {
  try {
    modulePath = createRequire(import.meta.url).resolve('marked', {
      paths: [resolve(root, '.deps/readme'), root],
    });
  } catch {
    throw new Error('Install the optional documentation parser: npm install --prefix .deps/readme marked@17.0.5');
  }
}
let site;
const { Marked } = await import(pathToFileURL(modulePath).href);
const plainTitle = text => text.replace(/^(?:[一二三四五六七八九十百]+、\s*|\d+(?:\.\d+)*[.)、]?\s+)/u, '');
const titlePrefix = text => text.slice(0, text.length - plainTitle(text).length);
const hash = text => createHash('sha256').update(text).digest('hex');
const escape = text => String(text).replace(/[&<>"']/g, ch => ({
  '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;',
}[ch]));
const sourceData = await readFile(resolve(root, 'docs/performance_data/burst_from_empty_20261013/formal/report_results.json'), 'utf8');
const result = JSON.parse(sourceData);
if (!result.verified || result.parameters.start_mode !== 'empty_each_burst') {
  throw new Error('The overview requires the verified empty-start business result.');
}
const queues = ['TwoBufQueue', 'ReaderWriterQueue', 'rigtorp', 'atomic_queue'];
const names = { TwoBufQueue: 'TwoBufQueue', ReaderWriterQueue: 'ReaderWriterQueue', rigtorp: 'rigtorp::SPSCQueue', atomic_queue: 'atomic_queue' };
const bursts = [2, 4, 8, 16];
const rows = new Map(result.rows.map(row => [row.queue + '/' + row.burst_mib, row]));
for (const burst of bursts) for (const queue of queues) {
  if (!rows.has(queue + '/' + burst)) throw new Error('Missing saved chart row.');
}

function highlight(text, language) {
  const patterns = {
    cpp: /\/\/[^\n]*|"(?:\\.|[^"\\])*"|'(?:\\.|[^'\\])*'|#[A-Za-z_]+|\b(?:auto|bool|const|int|return|static_cast|false|true|noexcept|void|std|TwoBufQueue)\b|\b\d+\b/g,
    cmake: /#[^\n]*|"(?:\\.|[^"\\])*"|\b(?:add_subdirectory|target_link_libraries|find_package|CONFIG|REQUIRED|PRIVATE)\b/g,
    sh: /#[^\n]*|"(?:\\.|[^"\\])*"|\b(?:cmake|ctest|python3|node|npm)\b/g,
  };
  const pattern = patterns[language];
  if (!pattern) return escape(text);
  let output = '', offset = 0;
  for (const match of text.matchAll(pattern)) {
    output += escape(text.slice(offset, match.index));
    const value = match[0];
    const kind = value.startsWith('//') || (value.startsWith('#') && language !== 'cpp') ? 'comment'
      : /^["']/.test(value) ? 'string' : /^\d+$/.test(value) ? 'number'
      : value === 'std' || value === 'TwoBufQueue' ? 'type' : 'keyword';
    output += '<span class="syntax-' + kind + '">' + escape(value) + '</span>';
    offset = match.index + value.length;
  }
  return output + escape(text.slice(offset));
}

function mpmcFlow(labels, zh) {
  const caption = zh ? 'TwoBufQueue：MPMC（多生产者、多消费者）' : 'TwoBufQueue: MPMC (multiple producers, multiple consumers)';
  const node = (id, role, detail = '') => '<div class="mpmc-node ' + role + '" data-node="' + id
    + '"><strong>' + escape(labels.get(id)) + '</strong>' + (detail ? '<small>' + escape(detail) + '</small>' : '') + '</div>';
  const stage = (ids, role) => '<div class="mpmc-stage">' + ids.map(id => node(id, role)).join('') + '</div>';
  const connector = kind => {
    const width = kind === 'straight' ? 22 : 40;
    const path = kind === 'straight' ? 'M1 56 H21 M17 52 L21 56 L17 60 M1 184 H21 M17 180 L21 184 L17 188'
      : kind === 'merge' ? 'M1 56 H9 L29 120 H39 M1 184 H9 L29 120 M35 116 L39 120 L35 124'
      : 'M1 120 H11 L31 56 H39 M11 120 L31 184 H39 M35 52 L39 56 L35 60 M35 180 L39 184 L35 188';
    return '<svg class="mpmc-connector" viewBox="0 0 ' + width + ' 240" aria-hidden="true" focusable="false"><path d="' + path + '"></path></svg>';
  };
  return '<figure class="flow-diagram mpmc-diagram" aria-label="' + escape(caption)
    + '"><figcaption class="flow-caption">' + escape(caption) + '</figcaption><div class="mpmc-layout">'
    + stage(['A', 'D'], 'producer') + connector('straight')
    + '<div class="mpmc-stage">' + node('B', 'source', 'Writer 1') + node('E', 'source', 'Writer 2') + '</div>'
    + connector('straight') + stage(['C', 'F'], 'copy') + connector('merge')
    + node('G', 'output', zh ? '共享池' : 'Shared pool')
    + connector('split') + stage(['H', 'I'], 'consumer') + '</div></figure>';
}

function flow(text, zh) {
  const labels = new Map();
  for (const match of text.matchAll(/\b([A-Z])\[([^\]]+)\]/g)) labels.set(match[1], match[2]);
  if (labels.size === 9 && text.includes('F --> G') && text.includes('G --> I')) return mpmcFlow(labels, zh);
  if (![3, 5].includes(labels.size)) return '<pre><code>' + escape(text) + '</code></pre>';
  const dual = labels.size === 5;
  const classes = dual ? ['producer', 'source', 'copy', 'output', 'consumer'] : ['producer', 'shared', 'consumer'];
  const caption = dual ? (zh ? 'TwoBufQueue：双层缓存' : 'TwoBufQueue: two layers')
    : (zh ? '常见单层队列' : 'Typical single-layer queue');
  return '<figure class="flow-diagram flow-' + labels.size + '" aria-label="' + escape(caption)
    + '"><figcaption class="flow-caption">' + escape(caption) + '</figcaption><ol>'
    + [...labels.values()].map((label, i) => '<li class="' + classes[i] + '"><span class="flow-number">0' + (i + 1) + '</span><strong>' + escape(label) + '</strong></li>').join('')
    + '</ol></figure>';
}

const submissionSourceName = 'docs/reports/comparison/performance_256m_zero_session_plateau.html';
const submissionSource = await readFile(resolve(root, submissionSourceName), 'utf8');
const submissionPacket = submissionSource.match(/<script[^>]+id="report-data"[^>]*>([\s\S]*?)<\/script>/)?.[1];
if (!submissionPacket) throw new Error('Missing saved Session report data.');
const submissionResult = JSON.parse(submissionPacket);
const payloads = [8, 16, 32, 64];
const transportFor = { TwoBufQueue: 'two_buf_queue', ReaderWriterQueue: 'readerwriterqueue', rigtorp: 'rigtorp', atomic_queue: 'atomic_queue' };
const submissionRows = new Map();
for (const bytes of payloads) for (const queue of queues) {
  const saved = submissionResult.rows.find(row => row.payload_bytes === bytes && row.transport === transportFor[queue]);
  const value = saved?.goals?.throughput?.value;
  if (!Number.isFinite(value) || value <= 0) throw new Error('Missing valid saved throughput: ' + queue + '/' + bytes);
  submissionRows.set(queue + '/' + bytes, value);
}
const businessReferenceNs = 13;
const chartSpecs = {
  submission: {
    asset: 'session_write_throughput.svg', sizes: payloads,
    title: 'Session producer submission throughput', better: 'Higher is better',
    source: submissionSourceName, report_data_sha256: hash(submissionPacket),
    value: (queue, size) => submissionRows.get(queue + '/' + size),
    label: size => size + ' B / message', reference: null,
    format: value => value.toLocaleString('en-US', { minimumFractionDigits: 1, maximumFractionDigits: 1 }),
  },
  business: {
    asset: 'burst_business_ratio.svg', sizes: bursts,
    title: 'Post-send business cost relative to normal', better: 'Lower is better; normal = 1.00x (13 ns)',
    source: 'docs/performance_data/burst_from_empty_20261013/formal/report_results.json', source_sha256: hash(sourceData),
    value: (queue, size) => rows.get(queue + '/' + size).p50 / businessReferenceNs,
    label: size => size + ' MiB / batch', reference: 1,
    format: value => value.toFixed(2) + '×',
  },
};

function metricChart(zh, alt, kind) {
  const spec = chartSpecs[kind], isSubmission = kind === 'submission';
  const intro = isSubmission ? (zh ? 'Session 发送吞吐' : 'Session submission throughput')
    : (zh ? '发送后业务耗时 / 正常耗时 · p50' : 'Post-send cost / normal cost · p50');
  const direction = isSubmission ? (zh ? 'M条/s · 越高越好' : 'Million messages/s · higher is better')
    : (zh ? '正常参考：1.00×（13 ns）' : 'Normal reference: 1.00× (13 ns)');
  return '<figure class="business-chart ' + (isSubmission ? 'submission-chart' : 'business-ratio-chart')
    + '" data-metric="' + kind + '" aria-label="' + escape(alt) + '"><div class="chart-intro"><span>'
    + intro + '</span><span>' + direction + '</span></div><div class="chart-grid">'
    + spec.sizes.map((size, index) => {
      const max = Math.max(...queues.map(queue => spec.value(queue, size)), spec.reference || 0);
      return '<article class="chart-card palette-' + index + '" data-size="' + size + '"><h3>' + size
        + (isSubmission ? ' B' : zh ? ' MiB / 批' : ' MiB / batch') + '</h3>'
        + queues.map(queue => {
          const value = spec.value(queue, size);
          const marker = spec.reference ? '<b class="normal-marker" style="left:' + (spec.reference / max * 100).toFixed(3) + '%"></b>' : '';
          return '<div class="chart-row' + (queue === 'TwoBufQueue' ? ' is-tbq' : '') + '" data-queue="' + queue
            + '" data-value="' + value + '"><span class="chart-name">' + names[queue] + '</span>'
            + '<div class="bar-track" aria-hidden="true"><i style="width:' + (value / max * 100).toFixed(3)
            + '%"></i>' + marker + '</div><span class="chart-value">' + spec.format(value) + '</span></div>';
        }).join('') + '</article>';
    }).join('') + '</div><figcaption>'
    + (isSubmission ? (zh ? '单位 M条/s，越高越好。' : 'Million messages/s, higher is better.')
      : (zh ? '比值 = 业务耗时 ÷ 13 ns，越低越好。1.00× 表示正常耗时。' : 'Ratio = business cost ÷ 13 ns; lower is better. 1.00× is normal cost.'))
    + ' <a href="' + (isSubmission ? submissionSourceName : 'docs/reports/burst/performance_burst_from_empty.html') + '">'
    + (zh ? '完整报告与参数' : 'Full report and parameters') + '</a></figcaption></figure>';
}

// Standalone SVGs keep the Markdown overview consistent with the HTML charts.
function svgChart(spec) {
  const palettes = [['#f0f5fa', '#d4e2ed'], ['#f4f1fa', '#dfd5ed'], ['#eff6f1', '#d4e4d9'], ['#fcf5e9', '#eadbbc']];
  const content = ['<svg xmlns="http://www.w3.org/2000/svg" width="1060" height="510" viewBox="0 0 1060 510" role="img">',
    '<title>' + escape(spec.title) + '</title><desc>' + escape(spec.better) + '. Each panel scales to its own maximum.</desc>',
    '<rect width="1060" height="510" fill="#ffffff"/>',
    '<g font-family="Arial, Helvetica, sans-serif">',
    '<text x="24" y="30" fill="#163044" font-size="21" font-weight="700">' + escape(spec.title) + '</text>',
    '<text x="24" y="50" fill="#647586" font-size="11">' + escape(spec.better) + '</text>'];
  spec.sizes.forEach((size, index) => {
    const x = 20 + (index % 2) * 520, y = 66 + Math.floor(index / 2) * 212;
    const max = Math.max(...queues.map(queue => spec.value(queue, size)), spec.reference || 0);
    content.push('<rect x="' + x + '" y="' + y + '" width="500" height="192" rx="10" fill="' + palettes[index][0] + '" stroke="' + palettes[index][1] + '"/>',
      '<text x="' + (x + 16) + '" y="' + (y + 28) + '" fill="#315b73" font-size="14" font-weight="700">' + escape(spec.label(size)) + '</text>');
    queues.forEach((queue, row) => {
      const value = spec.value(queue, size), top = y + 52 + row * 31, color = queue === 'TwoBufQueue' ? '#087f6e' : '#8ea6b8';
      content.push('<text x="' + (x + 16) + '" y="' + (top + 9) + '" fill="' + (queue === 'TwoBufQueue' ? '#087f6e' : '#4e6b7d')
        + '" font-size="11"' + (queue === 'TwoBufQueue' ? ' font-weight="700"' : '') + '>' + escape(names[queue]) + '</text>',
        '<rect x="' + (x + 154) + '" y="' + top + '" width="245" height="9" rx="3" fill="#ffffff"/>',
        '<rect x="' + (x + 154) + '" y="' + top + '" width="' + (value / max * 245).toFixed(3) + '" height="9" rx="3" fill="' + color + '"/>');
      if (spec.reference) {
        const marker = x + 154 + spec.reference / max * 245;
        content.push('<line x1="' + marker.toFixed(3) + '" y1="' + (top - 4) + '" x2="' + marker.toFixed(3) + '" y2="' + (top + 13) + '" stroke="#986732" stroke-dasharray="3 3"/>');
      }
      content.push('<text x="' + (x + 478) + '" y="' + (top + 9) + '" text-anchor="end" font-size="12" fill="'
        + (queue === 'TwoBufQueue' ? '#087f6e' : '#3e596b') + '"' + (queue === 'TwoBufQueue' ? ' font-weight="700"' : '') + '>' + escape(spec.format(value)) + '</text>');
    });
  });
  content.push('<text x="24" y="500" font-size="10" fill="#647586">Each panel uses its own scale. Saved measurements; full parameters are in the linked reports.</text>', '</g></svg>');
  return content.join('\n') + '\n';
}
async function buildOverviewFigures() {
  for (const [kind, spec] of Object.entries(chartSpecs)) {
    const metadata = { source: spec.source, ...(spec.source_sha256 ? { source_sha256: spec.source_sha256 } : { report_data_sha256: spec.report_data_sha256 }),
      metric: kind === 'submission' ? 'producer sending throughput in million messages/s' : 'mean per-run business group-average p50 divided by 13 ns',
      ...(kind === 'business' ? { normal_business_ns: businessReferenceNs } : {}),
      rows: spec.sizes.flatMap(size => queues.map(queue => ({ queue, [kind === 'submission' ? 'payload_bytes' : 'burst_mib']: size,
        value: spec.value(queue, size), ...(kind === 'business' ? { business_p50_ns: rows.get(queue + '/' + size).p50 } : {}) }))) };
    await writeFile(resolve(root, 'docs/assets', spec.asset), svgChart(spec));
    await writeFile(resolve(root, 'docs/assets', spec.asset.replace('.svg', '.json')), JSON.stringify(metadata, null, 2) + '\n');
  }
}


async function buildOverviews() {
for (const detailed of [true, false]) {
for (const zh of [false, true]) {
  const stem = detailed ? 'README_DETAIL' : 'README';
  const sourceName = stem + (zh ? '.zh-CN' : '') + '.md';
  const outputName = stem + (zh ? '.zh-CN' : '') + '.html';
  const englishOverview = stem + '.html', chineseOverview = stem + '.zh-CN.html';
  const source = await readFile(resolve(root, sourceName), 'utf8');
  const markdown = new Marked({ gfm: true, breaks: false });
  markdown.use({ renderer: {
    paragraph({ tokens }) {
      const content = this.parser.parseInline(tokens);
      if (tokens.length === 1 && tokens[0].type === 'image') return content;
      const paragraphText = tokens.map(token => token.text || '').join('');
      const feedback = paragraphText.startsWith('同时，我会持续完善这个组件。')
        || paragraphText.startsWith('I will continue improving this component.');
      return '<p' + (feedback ? ' class="feedback-invitation"' : '') + '>' + content + '</p>\n';
    },
    code({ text, lang }) {
      const language = (lang || 'text').split(/\s/)[0];
      if (language === 'mermaid') return flow(text, zh);
      const label = language === 'cpp' ? 'C++' : language === 'sh' ? 'Shell' : language === 'cmake' ? 'CMake' : language;
      return '<div class="code-frame"><div class="code-label">' + escape(label)
        + '</div><pre tabindex="0"><code class="language-' + escape(language) + '">'
        + highlight(text, language) + '</code></pre></div>';
    },
    image({ href, text, title }) {
      if (href === 'docs/assets/session_write_throughput.svg') return metricChart(zh, text, 'submission');
      if (href === 'docs/assets/burst_business_ratio.svg') return metricChart(zh, text, 'business');
      return '<figure class="archive-chart"><img src="' + escape(href) + '" alt="' + escape(text)
        + '"' + (title ? ' title="' + escape(title) + '"' : '') + ' loading="lazy"><figcaption>'
        + escape(text) + '</figcaption></figure>';
    },
    table({ header, rows }) {
      const cell = (item, tag) => '<' + tag + (item.align ? ' style="text-align:' + escape(item.align) + '"' : '')
        + '>' + this.parser.parseInline(item.tokens) + '</' + tag + '>';
      const body = rows.map(row => '<tr' + (row[0]?.text?.includes('TwoBufQueue') ? ' class="table-tbq"' : '')
        + '>' + row.map(item => cell(item, 'td')).join('') + '</tr>').join('');
      return '<div class="table-scroll"><table><thead><tr>' + header.map(item => cell(item, 'th')).join('')
        + '</tr></thead><tbody>' + body + '</tbody></table></div>';
    },
    blockquote({ tokens }) {
      return '<blockquote class="overview-note">' + this.parser.parse(tokens) + '</blockquote>';
    },
    link({ href, title, tokens }) {
      href = site.localHref(resolve(root, sourceName), href);
      const target = /^(?:[a-z][a-z\d+.-]*:|\/\/)/i.test(href) ? href : href.replace(/\.md(?=[?#]|$)/, '.html');
      return '<a href="' + escape(target) + '"' + (title ? ' title="' + escape(title) + '"' : '')
        + '>' + this.parser.parseInline(tokens) + '</a>';
    },
  } });
  const intro = [], sections = [];
  let current, child;
  const bodySource = source.replace(/<!-- README_TOC:START -->[\s\S]*?<!-- README_TOC:END -->\s*/g, '').replace(/^\*\*(?:目录|Contents)\*\*\n\n(?:[ \t]*- \[[^\n]+\]\(#[^\n]+\)\n)+\n---\n*/gm, '').replace(/<a name="[^"]+"><\/a>\s*/g, '');
  for (const token of markdown.lexer(bodySource)) {
    if (token.type === 'heading' && token.depth === 1) continue;
    if (token.type === 'heading' && token.depth === 2) {
      current = { title: token.text, tokens: [], children: [] };
      sections.push(current); child = undefined;
    } else if (current && token.type === 'heading' && token.depth === 3) {
      child = { title: token.text, tokens: [] }; current.children.push(child);
    } else if (child) child.tokens.push(token);
    else if (current) current.tokens.push(token);
    else intro.push(token);
  }
  let schema = zh ? [
    { label: '有多快？', children: [
      { label: '消息提交速度', aliases: ['当前生产者写入性能对比'] },
      { label: '发送后的业务性能', aliases: ['连续发送一批消息-对业务性能产生什么影响'] },
      { label: '历史实验', hidden: true },
    ] },
    { label: '问题与解决', questions: true, children: [
      { label: '无背压为何变慢' }, { label: '批量交接能快多少' },
      { label: '缓存满了怎么办', planned: true, aliases: ['消费者偶尔慢一下-生产者也必须跟着等吗', '消息缓存满了-生产者必须等吗'] },
      { label: '容量该设多大', aliases: ['队列到底应该预留多少内存'] },
      { label: '大消息怎么发送', planned: true, aliases: ['消息大小不一会影响性能吗'] }, { label: '优先复用有用吗' },
    ] },
    { label: '与常规单层缓存队列对比', aliases: ['单层缓存与双层缓存', '单层与双层的设计对比'], children: [{ label: 'SPSC' }, { label: 'MPMC' }, { label: '设计要点' }] },
    { label: '使用场景', children: [{ label: '优势场景', scenarios: 6 }, { label: '劣势场景', scenarios: 2 }] },
    { label: '如何使用', children: [
      { label: '最小示例' }, { label: '按用途选示例' }, { label: '构建与接入' }, { label: '使用边界' },
    ] },
    { label: '后续计划', children: [] },
  ] : [
    { label: 'How fast?', children: [
      { label: 'Submission speed', aliases: ['current-producer-write-comparisons'] },
      { label: 'Business impact', aliases: ['how-a-continuous-message-batch-affects-business-performance'] },
      { label: 'Earlier experiments', hidden: true },
    ] },
    { label: 'Problems & solutions', questions: true, children: [
      { label: 'No wait, still slow' }, { label: 'Batch handoff' },
      { label: 'Full buffers', planned: true, aliases: ['must-a-brief-consumer-slowdown-make-the-producer-wait-too', 'when-message-buffers-are-full-must-the-producer-wait'] },
      { label: 'Capacity choice', aliases: ['how-much-queue-memory-should-be-reserved'] },
      { label: 'Large messages', planned: true, aliases: ['do-varying-message-sizes-affect-performance'] }, { label: 'Reuse recent blocks' },
    ] },
    { label: 'Comparison with typical single-layer queues', aliases: ['single-layer-and-two-layer-buffers', 'single-layer-and-two-layer-designs'], children: [{ label: 'SPSC' }, { label: 'MPMC' }, { label: 'Design highlights' }] },
    { label: 'Use cases', children: [{ label: 'Benefits', scenarios: 6 }, { label: 'Tradeoffs', scenarios: 2 }] },
    { label: 'Getting started', children: [
      { label: 'Minimal example' }, { label: 'Examples by task' }, { label: 'Build & integrate' }, { label: 'Usage contracts' },
    ] },
    { label: 'Roadmap', aliases: ['planned-improvements'], children: [] },
  ];
  if (!detailed) schema = zh ? [
    { label: '几个有趣的问题', questions: true, closingNote: true, aliases: ['为什么选择-twobufqueue', '几个问题'], children: [
      { label: '发送后为何变慢', aliases: ['发送完后-业务还能保持快', '发送完后业务还能保持快'] },
      { label: '批量提交快多少', aliases: ['批量发送更快'] },
      { label: '缓存满了怎么办', aliases: ['消费者偶尔慢一下-生产者也必须跟着等吗', '为消费者的临时波动留出余量', '消息缓存满了-生产者必须等吗'] },
      { label: '大消息怎么发送' },
      { label: '容量该设多大' },
    ] },
    { label: '有多快？', children: [{ label: '消息提交速度' }, { label: '发送后的业务性能' }] },
    { label: '适用与取舍', children: [{ label: '适合的场景' }, { label: '需要考虑的取舍' }] },
    { label: '如何开始', children: [{ label: '最小使用示例' }, { label: '构建与接入' }] },
    { label: '完整文档', children: [] },
  ] : [
    { label: 'A few interesting questions', questions: true, closingNote: true, aliases: ['why-choose-twobufqueue', 'a-few-questions'], children: [
      { label: 'Business-cache interference', aliases: ['keep-subsequent-business-work-fast'] },
      { label: 'Batch submission', aliases: ['submit-prepared-batches-quickly'] },
      { label: 'Full buffers', aliases: ['must-a-brief-consumer-slowdown-make-the-producer-wait-too', 'allow-for-temporary-consumer-slowdowns', 'when-message-buffers-are-full-must-the-producer-wait'] },
      { label: 'Large messages' },
      { label: 'Capacity choice' },
    ] },
    { label: 'How fast?', children: [{ label: 'Submission throughput' }, { label: 'Business impact' }] },
    { label: 'Fit and tradeoffs', children: [{ label: 'Useful scenarios' }, { label: 'Tradeoffs' }] },
    { label: 'Get started', children: [{ label: 'Minimal example' }, { label: 'Build and integrate' }] },
    { label: 'Full documentation', children: [] },
  ];
  if (sections.length !== schema.length) throw new Error('Expected ' + schema.length + ' README groups.');
  const ids = new Set();
  const uniqueId = title => {
    const base = plainTitle(title).toLowerCase().replace(/[^\p{L}\p{N}]+/gu, '-').replace(/^-|-$/g, '') || 'section';
    let id = base, suffix = 2;
    while (ids.has(id)) id = base + '-' + suffix++;
    ids.add(id); return id;
  };
  for (let i = 0; i < sections.length; ++i) {
    const section = sections[i], metadata = schema[i];
    if (section.children.length !== metadata.children.length) throw new Error('Unexpected child sections: ' + section.title);
    section.label = metadata.label; section.questions = metadata.questions; section.aliases = metadata.aliases;
    section.id = uniqueId(section.title);
    section.children = section.children.map((item, j) => ({ ...item, ...metadata.children[j], id: uniqueId(item.title) }));
    if (metadata.closingNote) {
      // The shared invitation follows all questions, rather than belonging to the last one.
      const last = section.children.at(-1);
      const note = last.tokens.findLastIndex(token => token.type !== 'space');
      if (last.tokens[note]?.type !== 'blockquote') throw new Error('Missing question-chapter closing note.');
      section.closingTokens = last.tokens.splice(note);
    }
  }
  const tagline = intro.find(token => token.type === 'paragraph' && /^\*\*/.test(token.text));
  if (!tagline) throw new Error('Missing README tagline.');
  const navigation = intro.find(token => token.type === 'paragraph' && token.text.includes('README'));
  const introduction = intro.filter(token => token !== tagline && token !== navigation);
  const toHtml = input => markdown.parse(input.map(token => token.raw).join(''));
  const aliasHtml = item => (item.aliases || []).filter(id => id !== item.id).map(id => {
    if (ids.has(id)) throw new Error('Duplicate anchor alias: ' + id);
    ids.add(id); return '<span class="anchor-alias" id="' + escape(id) + '"></span>';
  }).join('');
  const tocText = title => ['有多快？', 'How fast is it?'].includes(plainTitle(title)) ? title : title.replace(/[?？]/g, '');
  const toc = sections.map((section, index) => '<div class="toc-group"><a class="toc-group-link" href="#' + escape(section.id)
    + '" title="' + escape(section.title) + '">' + escape(tocText(section.title)) + '</a>'
    + (section.children.some(item => !item.hidden) ? '<ul class="toc-children">' + section.children.filter(item => !item.hidden)
      .map(item => '<li><a href="#' + escape(item.id) + '" title="' + escape(item.title) + '" aria-label="'
        + escape(item.title) + '">' + escape(tocText(index < 2 ? item.title : titlePrefix(item.title) + item.label)) + '</a></li>').join('') + '</ul>' : '') + '</div>').join('');
  function scenarioCards(item) {
    const lead = [], cards = []; let card;
    for (const token of item.tokens) {
      if (token.type === 'heading' && token.depth === 4) {
        card = { title: token.text, tokens: [] }; cards.push(card);
      } else if (card) card.tokens.push(token);
      else lead.push(token);
    }
    if (cards.length !== item.scenarios) throw new Error('Unexpected scenario count: ' + item.title);
    return toHtml(lead) + '<div class="scenario-grid">' + cards.map((card, index) => {
      const title = plainTitle(card.title);
      const number = titlePrefix(card.title).trim().replace(/[.)、]$/, '');
      const body = toHtml(card.tokens);
      const labels = zh ? ['场景', item.scenarios === 6 ? '优势' : '劣势'] : ['Scenario', item.scenarios === 6 ? 'Benefit' : 'Drawback'];
      if (!labels.every(label => body.includes('<strong>' + label + ':') || body.includes('<strong>' + label + '：'))) {
        throw new Error('Missing scenario/benefit labels: ' + card.title);
      }
      return '<article class="scenario-card"><h4 class="scenario-title"><span class="scenario-number">' + (number || index + 1)
        + '</span><span>' + markdown.parseInline(title) + '</span></h4><div class="scenario-body">' + body + '</div></article>';
    }).join('') + '</div>';
  }
  const content = sections.map((section, i) => {
    const children = section.children.map((item, j) => {
      const body = item.scenarios ? scenarioCards(item) : toHtml(item.tokens);
      const heading = markdown.parseInline(item.title), aliases = aliasHtml(item);
      if (item.hidden) return '<article class="readme-subsection history-subsection" id="' + escape(item.id) + '">' + aliases
        + '<details><summary>' + heading + '</summary><div class="history-content">' + body + '</div></details></article>';
      const question = section.questions && j < 6;
      return '<article class="readme-subsection' + (question ? ' question-subsection' : '') + '" id="' + escape(item.id)
        + '">' + aliases + '<h3 class="subsection-title">'
        + '<span>' + heading + '</span>'
        + (item.planned ? '<span class="planned-badge">' + (zh ? '计划中' : 'Planned') + '</span>' : '')
        + '</h3><div class="subsection-content">' + body + '</div></article>';
    }).join('');
    return '<section class="doc-section grouped-section' + (i < 2 ? ' feature-section' : '') + '" id="'
      + escape(section.id) + '">' + aliasHtml(section) + '<div class="section-heading"><h2>' + markdown.parseInline(section.title)
      + '</h2></div><div class="section-content">' + toHtml(section.tokens) + children
      + (section.closingTokens ? toHtml(section.closingTokens) : '') + '</div></section>';
  }).join('\n');
  const performanceGroup = sections[detailed ? 0 : 1], usageGroup = sections[detailed ? 4 : 3];
  const performance = performanceGroup.children[0], example = usageGroup.children[0], integration = usageGroup.children[detailed ? 2 : 1];
  const benefitLabels = zh ? ['批量发送更快', '发送后业务更稳', '缓解突发背压', '内存按需管理']
    : ['Faster batch submission', 'Less business-cache disruption', 'Buffer temporary slowdowns', 'Manage memory on demand'];
  const benefitTargets = detailed ? [1, 0, 2, 3].map(index => sections[1].children[index])
    : [1, 0, 2].map(index => sections[0].children[index]);
  const menu = '<a href="#' + escape(performanceGroup.id) + '">' + (zh ? '性能实测' : 'Performance') + '</a>'
    + '<a href="#' + escape(example.id) + '">' + (zh ? '开始使用' : 'Quick start') + '</a>'
    + '<a href="#' + escape(integration.id) + '">' + (zh ? '构建接入' : 'Integration') + '</a>'
    + '<a href="docs/reports/index.html">' + (zh ? '全部报告' : 'All reports') + '</a>';
  const tocToggle = '<button class="toc-toggle" id="toc-toggle" type="button" aria-controls="overview-toc" aria-expanded="true"'
    + ' data-hide-label="' + (zh ? '隐藏目录' : 'Hide contents') + '" data-show-label="' + (zh ? '显示目录' : 'Show contents') + '">'
    + '<span aria-hidden="true">☰</span><span class="toc-toggle-label">' + (zh ? '隐藏目录' : 'Hide contents') + '</span></button>';
  const html = '<!doctype html>\n<html lang="' + (zh ? 'zh-CN' : 'en') + '"><head><meta charset="utf-8">'
    + '<meta name="viewport" content="width=device-width,initial-scale=1"><meta name="description" content="'
    + escape(tagline.text.replace(/\*\*/g, '')) + '"><title>TwoBufQueue · ' + (detailed ? (zh ? '完整介绍' : 'Full guide') : (zh ? '简短介绍' : 'Quick overview')) + '</title>'
    + '<link rel="stylesheet" href="docs/assets/readme.css"><script src="docs/assets/readme_view.js" defer></script>'
    + '<noscript><style>.toc-toggle{display:none}</style></noscript><link rel="alternate" hreflang="en" href="' + englishOverview + '">'
    + '<link rel="alternate" hreflang="zh-CN" href="' + chineseOverview + '"></head><body>'
    + '<!-- Generated from ' + sourceName + '; SHA256 ' + hash(source) + '; saved business result SHA256 ' + hash(sourceData) + '; saved submission report-data SHA256 ' + hash(submissionPacket) + '. -->'
    + '<a class="skip-link" href="#content">' + (zh ? '跳到正文' : 'Skip to content') + '</a>'
    + '<header class="site-header"><div class="header-inner"><a class="brand" href="#top"><span class="brand-symbol" aria-hidden="true"><i></i><i></i></span>TwoBufQueue<span class="brand-version">C++</span></a>' + tocToggle + '<nav aria-label="'
    + (zh ? '主要导航' : 'Main navigation') + '">' + menu + '</nav><div class="languages">'
    + '<a href="' + englishOverview + '"' + (!zh ? ' aria-current="page"' : '') + '>EN</a><a href="' + chineseOverview + '"'
    + (zh ? ' aria-current="page"' : '') + '>中文</a></div></div></header>'
    + '<div class="page-layout overview-layout' + (detailed ? '' : ' quick-overview')
    + '" id="top"><aside class="toc overview-toc" id="overview-toc"><p class="toc-label">' + (zh ? '项目介绍' : 'ON THIS PAGE')
    + '</p><nav aria-label="' + (zh ? '目录' : 'Table of contents') + '">' + toc + '</nav><div class="toc-footer"><a href="'
    + (zh ? 'docs/guides/API.zh-CN.html' : 'docs/guides/API.html') + '">' + (zh ? '接口指南' : 'API guide') + '</a><a href="LICENSE">Apache-2.0</a></div></aside>'
    + '<main id="content"><section class="hero"><div class="eyebrow"><span></span>'
    + (zh ? '双层缓存 · 批量发布与传输 · 生产者热路径' : 'TWO-LAYER BUFFERS · BATCH PUBLICATION AND TRANSFER · PRODUCER HOT PATH')
    + '</div><h1>TwoBufQueue</h1><div class="hero-tagline">' + markdown.parseInline(tagline.text)
    + '</div><div class="hero-description">' + toHtml(introduction) + '</div><div class="hero-actions"><a class="button primary" href="#'
    + escape(performance.id) + '">' + (zh ? '查看性能实测' : 'Explore the measurements') + '<span aria-hidden="true">↗</span></a><a class="button secondary" href="#'
    + escape(example.id) + '">' + (zh ? '开始使用' : 'Start using TwoBufQueue') + '</a></div>'
    + '<ul class="hero-benefits" aria-label="' + (zh ? '设计重点' : 'Design priorities') + '">'
    + benefitTargets.map((target, index) => '<li><a href="#' + escape(target.id) + '">' + escape(benefitLabels[index]) + '</a></li>').join('') + '</ul>'
    + '<div class="resources">' + (navigation ? markdown.parse(navigation.raw) : '') + '</div></section>'
    + content + '<footer class="site-footer"><span>TwoBufQueue · C++ · Apache-2.0</span><span><a href="'
    + (zh ? 'docs/guides/API.zh-CN.html' : 'docs/guides/API.html') + '">' + (zh ? '接口指南' : 'API guide') + '</a><a href="docs/reports/index.html">' + (zh ? '性能报告' : 'Performance reports')
    + '</a><a href="#top">' + (zh ? '回到顶部 ↑' : 'Back to top ↑') + '</a></span></footer></main></div></body></html>\n';
  await writeFile(resolve(root, outputName), html);
  console.log('Generated ' + outputName + ' from ' + sourceName);
}
}
}

const { spawnSync } = await import('node:child_process');
const readingPython = process.env.BRQ_DOC_PYTHON || (process.platform === 'win32' ? 'python' : 'python3');
function buildGithubReading() {
  const result = spawnSync(readingPython, [resolve(root, 'tools/build_github_reading.py'), '--root', root], { stdio: 'inherit' });
  if (result.error) throw result.error;
  if (result.status !== 0) throw new Error('GitHub reading-page generation failed.');
}
buildGithubReading();
site = await documentSite(root);
await buildOverviewFigures();
await buildOverviews();
await buildDocuments({ root, Marked, highlight, escape, flow });
const codePages = await buildCodePages({ root });
if (codePages.markdownChanged) {
  // Regenerate from the updated links so each page records the correct source hash.
  await buildOverviews();
  await buildDocuments({ root, Marked, highlight, escape, flow });
}

const localizedReading = spawnSync(readingPython, [resolve(root, 'tools/localize_reading_pages.py'), '--root', root], { stdio: 'inherit' });
if (localizedReading.error) throw localizedReading.error;
if (localizedReading.status !== 0) throw new Error('Reading-page localization failed.');

buildGithubReading();

const languageDirectories = spawnSync(readingPython, [resolve(root, 'tools/build_language_docs.py'), '--root', root], { stdio: 'inherit' });
if (languageDirectories.error) throw languageDirectories.error;
if (languageDirectories.status !== 0) throw new Error('Language-directory generation failed.');
