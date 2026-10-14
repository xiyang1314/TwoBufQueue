/** Generate the HTML reading pages listed in documentation_pages.json. */
import { readFile, writeFile } from 'node:fs/promises';
import { createHash } from 'node:crypto';
import { dirname, resolve, relative, sep } from 'node:path';
import { documentSite } from './document_site.mjs';

const digest = value => createHash('sha256').update(value).digest('hex');
const plainTitle = value => value.replace(/^(?:[一二三四五六七八九十百]+、\s*|\d+(?:\.\d+)*[.)、]?\s+)/u, '');
const slug = value => plainTitle(value).toLowerCase().replace(/[^\p{L}\p{N}]+/gu, '-').replace(/^-|-$/g, '') || 'section';

export async function buildDocuments({ root, Marked, highlight, escape, flow }) {
  const site = await documentSite(root);
  const manifest = JSON.parse(await readFile(resolve(root, 'tools/documentation_pages.json'), 'utf8'));
  let readingCopies = [];
  try {
    readingCopies = JSON.parse(await readFile(resolve(root, 'tools/github_reading_pages.json'), 'utf8')).pages;
  } catch (error) { if (error.code !== 'ENOENT') throw error; }
  const pages = new Map([
    ...manifest.sources.map(name => [resolve(root, name), resolve(root, name.replace(/\.md$/, '.html'))]),
    ...readingCopies.map(page => [resolve(root, page.markdown), resolve(root, page.html)]),
  ]);
  const overview = new Set(['README.md', 'README.zh-CN.md', 'README_DETAIL.md', 'README_DETAIL.zh-CN.md']);
  let generated = 0;
  const local = name => name.split(sep).join('/');
  const urlFrom = (document, target) => local(relative(dirname(document), resolve(root, target)));
  function rewriteLink(document, href) {
    href = site.localHref(document, href);
    if (/^(?:[a-z][a-z\d+.-]*:|\/\/)/i.test(href)) return href;
    const match = href.match(/^([^?#]*)([?#].*)?$/);
    if (!match || !match[1].endsWith('.md')) return href;
    const source = resolve(dirname(document), decodeURIComponent(match[1]));
    const target = pages.get(source);
    if (!target) throw new Error('Unregistered Markdown document: ' + local(relative(root, source)));
    return local(relative(dirname(document), target)) + (match[2] || '');
  }
  function rewriteHtmlLinks(document, html) {
    return html.replace(/((?:href|src)=["'])([^"']+)(["'])/g,
      (_, prefix, href, quote) => prefix + rewriteLink(document, href) + quote);
  }
  for (const name of manifest.sources) {
    if (overview.has(name)) continue;
    const document = resolve(root, name), output = pages.get(document);
    const source = await readFile(document, 'utf8');
    const zh = /zh-CN/.test(name) || /[\u4e00-\u9fff]/.test(source.split('\n')[0]);
    const markdown = new Marked({ gfm: true, breaks: false });
    const headings = [], ids = new Set();
    markdown.use({ renderer: {
      heading({ text, depth, tokens }) {
        let id = slug(text);
        const base = id; let n = 2;
        while (ids.has(id)) id = base + '-' + n++;
        ids.add(id);
        if (depth === 2 || depth === 3) headings.push({ id, text, depth });
        return '<h' + depth + ' id="' + escape(id) + '">' + this.parser.parseInline(tokens) + '</h' + depth + '>\n';
      },
      paragraph({ tokens }) {
        const content = this.parser.parseInline(tokens);
        return tokens.length === 1 && tokens[0].type === 'image' ? content : '<p>' + content + '</p>\n';
      },
      link({ href, title, tokens }) {
        return '<a href="' + escape(rewriteLink(document, href)) + '"'
          + (title ? ' title="' + escape(title) + '"' : '') + '>' + this.parser.parseInline(tokens) + '</a>';
      },
      image({ href, text }) {
        return '<figure class="archive-chart"><img src="' + escape(href) + '" alt="' + escape(text)
          + '" loading="lazy"><figcaption>' + escape(text) + '</figcaption></figure>';
      },
      code({ text, lang }) {
        const language = (lang || 'text').split(/\s/)[0];
        if (language === 'mermaid') return flow(text, zh);
        const label = language === 'cpp' ? 'C++' : language === 'sh' ? 'Shell' : language === 'cmake' ? 'CMake' : language;
        return '<div class="code-frame"><div class="code-label">' + escape(label)
          + '</div><pre tabindex="0"><code class="language-' + escape(language) + '">'
          + highlight(text, language) + '</code></pre></div>';
      },
    } });
    const bodySource = source.replace(/<a name="[^"]+"><\/a>\s*/g, '');
    const tokens = markdown.lexer(bodySource);
    const firstHeading = tokens.find(token => token.type === 'heading' && token.depth === 1);
    const title = firstHeading?.text || name;
    const remaining = tokens.filter(token => token !== firstHeading);
    const firstSection = remaining.findIndex(token => token.type === 'heading' && token.depth === 2);
    const intro = firstSection < 0 ? [] : remaining.slice(0, firstSection);
    const bodyTokens = firstSection < 0 ? remaining : remaining.slice(firstSection);
    const render = input => markdown.parse(input.map(token => token.raw).join(''));
    const introHtml = render(intro);
    let body = render(bodyTokens);
    body = body.replace(/<table>[\s\S]*?<\/table>/g, table => '<div class="table-scroll">' + table + '</div>');
    const toc = headings.map(item => '<a class="toc-depth-' + item.depth + '" href="#' + escape(item.id)
      + '">' + markdown.parseInline(item.text) + '</a>').join('');
    const home = urlFrom(document, zh ? 'README.zh-CN.html' : 'README.html');
    const api = urlFrom(document, zh ? 'docs/guides/API.zh-CN.html' : 'docs/guides/API.html');
    const examples = urlFrom(document, zh ? 'examples/README.zh-CN.html' : 'examples/README.html');
    const reports = urlFrom(document, 'docs/reports/index.html');
    const codeIndex = urlFrom(document, 'docs/code/index.html');
    const css = urlFrom(document, 'docs/assets/readme.css');
    const counterpartName = name.endsWith('.zh-CN.md') ? name.replace('.zh-CN.md', '.md') : name.replace('.md', '.zh-CN.md');
    const counterpart = pages.get(resolve(root, counterpartName));
    const englishPage = name.endsWith('.zh-CN.md') ? counterpart : output;
    const chinesePage = name.endsWith('.zh-CN.md') ? output : counterpart;
    const languages = counterpart ? '<div class="languages"><a href="' + escape(local(relative(dirname(document), englishPage)))
      + '"' + (!zh ? ' aria-current="page"' : '') + '>EN</a><a href="'
      + escape(local(relative(dirname(document), chinesePage))) + '"' + (zh ? ' aria-current="page"' : '')
      + '>中文</a></div>' : '';
    let html = '<!doctype html>\n<html lang="' + (zh ? 'zh-CN' : 'en') + '"><head><meta charset="utf-8">'
      + '<meta name="viewport" content="width=device-width,initial-scale=1"><title>' + escape(title) + ' · TwoBufQueue</title>'
      + '<link rel="stylesheet" href="' + escape(css) + '"></head><body>'
      + '<!-- Generated from ' + name + '; SHA256 ' + digest(source) + '. -->'
      + '<a class="skip-link" href="#content">' + (zh ? '跳到正文' : 'Skip to content') + '</a>'
      + '<header class="site-header"><div class="header-inner"><a class="brand" href="' + escape(home)
      + '"><span class="brand-symbol" aria-hidden="true"><i></i><i></i></span>TwoBufQueue<span class="brand-version">C++</span></a>'
      + '<nav aria-label="' + (zh ? '主要导航' : 'Main navigation') + '"><a href="' + escape(home) + '">'
      + (zh ? '项目首页' : 'Overview') + '</a><a href="' + escape(api) + '">' + (zh ? '接口指南' : 'API guide')
      + '</a><a href="' + escape(examples) + '">' + (zh ? '使用示例' : 'Examples') + '</a><a href="' + escape(codeIndex) + '">' + (zh ? '代码阅读' : 'Source browser') + '</a><a href="' + escape(reports)
      + '">' + (zh ? '性能报告' : 'Reports') + '</a></nav>' + languages + '</div></header>'
      + '<div class="page-layout guide-layout" id="top"><aside class="toc"><p class="toc-label">'
      + (zh ? '本页目录' : 'ON THIS PAGE') + '</p><nav aria-label="' + (zh ? '目录' : 'Table of contents') + '">' + toc
      + '</nav><div class="toc-footer"><a href="' + escape(home) + '">' + (zh ? '回到首页' : 'Overview') + '</a></div></aside>'
      + '<main id="content" class="guide-main"><section class="guide-hero"><div class="eyebrow"><span></span>'
      + (zh ? 'TWOBUFQUEUE · 文档' : 'TWOBUFQUEUE · DOCUMENTATION') + '</div><h1>' + escape(title) + '</h1>'
      + introHtml + '</section><details class="mobile-toc"><summary>' + (zh ? '本页目录' : 'On this page')
      + '</summary><nav>' + toc + '</nav></details><article class="guide-content">' + body + '</article>'
      + '<footer class="site-footer"><span>TwoBufQueue · C++ · Apache-2.0</span><span><a href="' + escape(home) + '">'
      + (zh ? '项目首页' : 'Overview') + '</a><a href="#top">' + (zh ? '回到顶部 ↑' : 'Back to top ↑')
      + '</a></span></footer></main></div></body></html>\n';
    html = rewriteHtmlLinks(document, html);
    try {
      const existing = await readFile(output, 'utf8');
      if (!existing.includes('<!-- Generated from ' + name + '; SHA256 ')) {
        throw new Error('Refusing to replace unrelated HTML: ' + local(relative(root, output)));
      }
    } catch (error) { if (error.code !== 'ENOENT') throw error; }
    await writeFile(output, html);
    ++generated;
  }
  // Keep existing report text and numbers. Only switch their document hyperlinks.
  let updated = 0;
  const { readdir } = await import('node:fs/promises');
  const linkDirectories = ['', 'docs'];
  async function collectReportDirectories(dir) {
    linkDirectories.push(dir);
    for (const entry of await readdir(resolve(root, dir), { withFileTypes: true })) {
      if (entry.isDirectory()) await collectReportDirectories(dir + '/' + entry.name);
    }
  }
  await collectReportDirectories('docs/reports');
  for (const dir of linkDirectories) {
    for (const entry of await readdir(resolve(root, dir), { withFileTypes: true })) {
      if (!entry.isFile() || !entry.name.endsWith('.html')) continue;
      const document = resolve(root, dir, entry.name);
      const before = await readFile(document, 'utf8');
      const after = rewriteHtmlLinks(document, before);
      if (after !== before) { await writeFile(document, after); ++updated; }
    }
  }
  console.log('Generated ' + generated + ' documentation pages; updated ' + updated + ' existing HTML link sets.');
}
