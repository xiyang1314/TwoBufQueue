"""Prepare GitHub-readable navigation without changing the HTML reading site."""
import argparse
import hashlib
from html import escape
from html.parser import HTMLParser
import json
import os
from pathlib import Path
import re
import unicodedata
from urllib.parse import unquote, urlsplit, urlunsplit

from document_links import document_links, rewrite_document_links, FENCED
from document_sections import number_sections, plain_title, without_contents

ROOT = Path(__file__).resolve().parents[1]
GENERATED = '<!-- Generated GitHub reading copy from '
VOID = {'area', 'base', 'br', 'col', 'embed', 'hr', 'img', 'input', 'link', 'meta', 'param', 'source', 'track', 'wbr'}


def slug(text):
    text = re.sub(r'<[^>]*>|[`*]', '', text).lower()
    return ''.join(c for c in text if c in '-_ ' or unicodedata.category(c)[0] in 'LNM').replace(' ', '-')


def with_toc(source, chinese):
    source = without_contents(source)
    rows, seen = [], {}
    for level, title in re.findall(r'^(#{2,3}) (.+)$', source, re.M):
        anchor = slug(title)
        number = seen.get(anchor, 0)
        seen[anchor] = number + 1
        if number:
            anchor += '-' + str(number)
        rows.append(('  ' if len(level) == 3 else '') + '- [' + title + '](#' + anchor + ')')
    block = '**' + ('目录' if chinese else 'Contents') + '**\n\n' + '\n'.join(rows) + '\n\n---\n\n'
    first = re.search(r'^## ', source, re.M)
    if not first:
        raise ValueError('Missing README chapters')
    insertion = first.start()
    preceding_anchor = re.search(r'<a name="[^"]+"></a>\s*$', source[:insertion])
    if preceding_anchor:
        insertion = preceding_anchor.start()
    return source[:insertion] + block + source[insertion:]


def style_flows(source):
    def replace(match):
        body = match[1]
        if 'classDef ' in body:
            return match[0]
        nodes = set(re.findall(r'\b([A-Z])\[', body))
        roles = ({'producer': 'A', 'shared': 'B', 'consumer': 'C'} if len(nodes) == 3 else
                 {'producer': 'A', 'source': 'B', 'copy': 'C', 'output': 'D', 'consumer': 'E'} if len(nodes) == 5 else
                 {'producer': 'A,D', 'source': 'B,E', 'copy': 'C,F', 'output': 'G', 'consumer': 'H,I'} if len(nodes) == 9 else {})
        colors = {'producer': ('#f0f5f8', '#d8e4ec'), 'consumer': ('#f0f5f8', '#d8e4ec'),
                  'copy': ('#f0f5f8', '#d8e4ec'), 'source': ('#e7f5ee', '#b6dace'),
                  'output': ('#eaf1fa', '#c3d6e9'), 'shared': ('#edf0f8', '#d6ddec')}
        styles = ['    classDef ' + role + ' fill:' + colors[role][0] + ',stroke:' + colors[role][1] + ',color:#315b73;' for role in roles]
        styles += ['    class ' + ids + ' ' + role + ';' for role, ids in roles.items()]
        return '```mermaid\n' + body.rstrip() + '\n' + '\n'.join(styles) + '\n```'
    return re.sub(r'```mermaid\n([\s\S]*?)\n```', replace, source)


class Element:
    def __init__(self, tag, attrs=()):
        self.tag, self.attrs, self.children = tag, dict(attrs), []


class Tree(HTMLParser):
    def __init__(self, source):
        super().__init__(convert_charrefs=True)
        self.root = Element('root')
        self.stack = [self.root]
        self.feed(source)

    def handle_starttag(self, tag, attrs):
        element = Element(tag, attrs)
        self.stack[-1].children.append(element)
        if tag not in VOID:
            self.stack.append(element)

    def handle_startendtag(self, tag, attrs):
        self.handle_starttag(tag, attrs)
        if tag not in VOID:
            self.handle_endtag(tag)

    def handle_endtag(self, tag):
        for index in range(len(self.stack) - 1, 0, -1):
            if self.stack[index].tag == tag:
                del self.stack[index:]
                break

    def handle_data(self, data):
        self.stack[-1].children.append(data)


def descendants(node, tag):
    if isinstance(node, str):
        return
    for child in node.children:
        if not isinstance(child, str):
            if child.tag == tag:
                yield child
            yield from descendants(child, tag)


def plain(node):
    return node if isinstance(node, str) else ''.join(plain(c) for c in node.children)


class Markdown:
    def __init__(self, chinese):
        self.chinese = chinese

    def text(self, node):
        if isinstance(node, str):
            return re.sub(r'\s+', ' ', node).replace('|', '\\|')
        tag, attrs = node.tag, node.attrs
        if tag in {'script', 'style', 'head', 'svg', 'button', 'input', 'noscript'} or 'hidden' in attrs or 'code-filter' in attrs.get('class', '').split():
            return ''
        body = ''
        for child in node.children:
            part = self.text(child)
            if body and part and not body[-1].isspace() and not part[0].isspace():
                if body.endswith(')') and part.startswith('['):
                    body += ' · '
                elif (body[-1].isalnum() or body[-1] in '*`])') and (part[0].isalnum() or part[0] in '*`['):
                    body += ' '
            body += part
        anchor = '\n\n<a name="' + escape(attrs['id'], quote=True) + '"></a>\n\n' if attrs.get('id') else ''
        if re.fullmatch(r'h[1-6]', tag):
            return anchor + '\n\n' + '#' * int(tag[1]) + ' ' + body.strip() + '\n\n'
        if tag == 'a':
            href = attrs.get('href')
            return '[' + body.strip() + '](' + href + ')' if href else body
        if tag == 'img':
            return '\n\n![' + attrs.get('alt', '') + '](' + attrs.get('src', '') + ')\n\n'
        if tag == 'pre':
            classes = attrs.get('class', '') + ' ' + ' '.join(c.attrs.get('class', '') for c in node.children if not isinstance(c, str))
            language = re.search(r'language-([\w+-]+)', classes)
            code = plain(node).rstrip('\n')
            fence = '`' * max(3, max((len(m[0]) + 1 for m in re.finditer(r'`+', code)), default=3))
            return '\n\n' + fence + (language[1] if language else '') + '\n' + code + '\n' + fence + '\n\n'
        if tag == 'code':
            return '`' + plain(node).replace('`', '\\`') + '`'
        if tag in {'strong', 'b'}:
            return '**' + body.strip() + '**' if body.strip() else ''
        if tag in {'em', 'i'}:
            return '*' + body.strip() + '*' if body.strip() else ''
        if tag == 'br':
            return '<br>'
        if tag == 'hr':
            return '\n\n---\n\n'
        if tag == 'table':
            return anchor + self.table(node)
        if tag in {'ul', 'ol'}:
            items = [c for c in node.children if not isinstance(c, str) and c.tag == 'li']
            return '\n\n' + '\n'.join(('1. ' if tag == 'ol' else '- ') + self.text(c).strip().replace('\n\n', '\n  ') for c in items) + '\n\n'
        if tag == 'details':
            return anchor + '\n\n<details>\n' + body + '\n</details>\n\n'
        if tag == 'summary':
            return '<summary>' + body.strip() + '</summary>\n\n'
        if tag == 'span' and 'range' in attrs.get('class', '').split():
            return '<br>' + body.strip()
        if tag in {'div', 'section', 'article', 'p', 'figure', 'figcaption', 'footer', 'nav', 'header', 'main', 'dl', 'dt', 'dd'}:
            return anchor + '\n\n' + body.strip() + '\n\n' if body.strip() else anchor
        return anchor + body

    def table(self, table):
        rows = list(descendants(table, 'tr'))
        if not rows:
            return ''
        width = max(sum(int(c.attrs.get('colspan', 1)) for c in row.children if not isinstance(c, str) and c.tag in ('td', 'th')) for row in rows)
        output, pending, grid = [], {}, []
        for row in rows:
            cells = [c for c in row.children if not isinstance(c, str) and c.tag in ('td', 'th')]
            if len(cells) == 1 and int(cells[0].attrs.get('colspan', 1)) == width and 'group-head' in row.attrs.get('class', ''):
                if len(grid) > 1:
                    output.append(self.grid(grid, width))
                title = self.text(cells[0]).strip().replace('\n', ' ')
                output.append('\n\n**' + title + '**\n\n')
                grid = [header] if 'header' in locals() else []
                continue
            values, column = [], 0
            for cell in cells:
                while column in pending:
                    value, remaining = pending.pop(column)
                    values.append(value)
                    if remaining > 1:
                        pending[column] = (value, remaining - 1)
                    column += 1
                value = re.sub(r'\s+', ' ', self.text(cell)).strip()
                span, lifetime = int(cell.attrs.get('colspan', 1)), int(cell.attrs.get('rowspan', 1))
                for offset in range(span):
                    entry = value if offset == 0 else ''
                    values.append(entry)
                    if lifetime > 1:
                        pending[column] = (entry, lifetime - 1)
                    column += 1
            while column < width:
                if column in pending:
                    value, remaining = pending.pop(column)
                    values.append(value)
                    if remaining > 1:
                        pending[column] = (value, remaining - 1)
                else:
                    values.append('')
                column += 1
            if not grid:
                header = values
            if row.attrs.get('data-library') == 'two_buf_queue' or row.attrs.get('class') in ('tb', 'tbq'):
                values = ['**' + v + '**' if v else v for v in values]
            grid.append(values)
        if grid:
            output.append(self.grid(grid, width))
        return '\n\n' + '\n\n'.join(output) + '\n\n'

    @staticmethod
    def grid(rows, width):
        line = lambda values: '| ' + ' | '.join((values + [''] * width)[:width]) + ' |'
        return '\n'.join([line(rows[0]), line(['---'] * width)] + [line(row) for row in rows[1:]])


def report_svg(root, html_name, source):
    packet = re.search(r'<script[^>]+id="report-data"[^>]*>([\s\S]*?)</script>', source)
    if not packet or 'zero_' not in html_name or 'plateau' not in html_name:
        return None
    data = json.loads(packet[1])
    rows = {(r['payload_bytes'], r['transport']): r['goals']['throughput']['value'] for r in data['rows']}
    queues = [('two_buf_queue', 'TwoBufQueue'), ('readerwriterqueue', 'ReaderWriterQueue'), ('rigtorp', 'rigtorp::SPSCQueue'), ('atomic_queue', 'atomic_queue')]
    chinese = '.zh-CN.' in html_name
    title = '生产者发送吞吐 · M条/s · 越高越好' if chinese else 'Producer submission throughput · million messages/s · higher is better'
    parts = ['<svg xmlns="http://www.w3.org/2000/svg" width="1060" height="510" viewBox="0 0 1060 510">', '<title>' + title + '</title>', '<desc>Saved report-data SHA256 ' + hashlib.sha256(packet[1].encode()).hexdigest() + '</desc>', '<rect width="1060" height="510" fill="white"/>', '<g font-family="Arial, sans-serif">', '<text x="24" y="32" font-size="20" fill="#163044">' + title + '</text>']
    palettes = ['#f0f5fa', '#f4f1fa', '#eff6f1', '#fcf5e9']
    for index, size in enumerate((8, 16, 32, 64)):
        x, y = 20 + index % 2 * 520, 66 + index // 2 * 212
        maximum = max(rows[(size, transport)] for transport, _ in queues)
        parts += [f'<rect x="{x}" y="{y}" width="500" height="192" rx="10" fill="{palettes[index]}" stroke="#d4e2ed"/>', f'<text x="{x+16}" y="{y+28}" font-size="14" fill="#315b73">{size} B</text>']
        for row, (transport, name) in enumerate(queues):
            value, top = rows[(size, transport)], y + 52 + row * 31
            color = '#087f6e' if row == 0 else '#8ea6b8'
            parts += [f'<text x="{x+16}" y="{top+9}" font-size="11" fill="{color}">{escape(name)}</text>', f'<rect x="{x+154}" y="{top}" width="245" height="9" rx="3" fill="white"/>', f'<rect x="{x+154}" y="{top}" width="{value/maximum*245:.3f}" height="9" rx="3" fill="{color}"/>', f'<text x="{x+478}" y="{top+9}" text-anchor="end" font-size="12" fill="{color}">{value:,.1f}</text>']
    parts += ['<text x="24" y="500" font-size="10" fill="#647586">' + ('各图分别缩放，完整参数见下方表格。' if chinese else 'Each panel uses its own scale. Complete parameters are listed below.') + '</text>', '</g></svg>']
    asset = 'docs/assets/github-' + Path(html_name).stem + '.svg'
    (root / asset).write_text('\n'.join(parts) + '\n', encoding='utf-8')
    return asset


def build(root):
    root = root.resolve()
    publication = json.loads((root / 'release-manifest.json').read_text())['files']
    reading_manifest = root / 'tools/github_reading_pages.json'
    previous_pages = json.loads(reading_manifest.read_text())['pages'] if reading_manifest.exists() else []
    generated_paths = {page['markdown'] for page in previous_pages}
    originals = {r['path'] for r in publication if r['path'].endswith('.md') and r['path'] not in generated_paths and not (root / r['path']).read_text().startswith(GENERATED)}
    originals.update(json.loads((root / 'tools/documentation_pages.json').read_text())['sources'])
    code = json.loads((root / 'docs/code/code-manifest.json').read_text())['files']
    source_views = {r[k]: r['source_path'] for r in code for k in ('html_path', 'html_path_zh_cn') if r.get(k)}
    generated, assets, rewritten = {}, set(), 0
    markdown_fragments, html_fragments = {}, {}
    site_config = json.loads((root / 'tools/document_site.json').read_text())
    site_url = site_config.get('site_url')
    if site_url:
        parsed_site = urlsplit(site_url)
        if parsed_site.scheme != 'https' or not parsed_site.netloc or parsed_site.username or parsed_site.password or parsed_site.query or parsed_site.fragment:
            raise ValueError('site_url must be a plain HTTPS base URL')
        site_url = site_url.rstrip('/') + '/'
    html_for_markdown = {page['markdown']: page['html'] for page in previous_pages}
    html_for_markdown.update({name: name[:-3] + '.html' for name in originals})
    html_for_source = {r['source_path']: r['html_path'] for r in code}
    chinese_source_views = {r['source_path']: r['html_path_zh_cn'] for r in code if r.get('html_path_zh_cn')}

    def local_reference(document, href):
        if site_url and href.startswith(site_url):
            parsed = urlsplit(href[len(site_url):])
            target = root / unquote(parsed.path)
            return urlunsplit(('', '', os.path.relpath(target, document.parent).replace(os.sep, '/'), parsed.query, parsed.fragment))
        return href

    def site_reference(document, href):
        if not site_url:
            return href
        parsed = urlsplit(href.strip('<>'))
        if parsed.scheme or parsed.netloc or not parsed.path:
            return href
        target = (document.parent / unquote(parsed.path)).resolve()
        if root not in target.parents:
            return href
        name = target.relative_to(root).as_posix()
        html_name = html_for_markdown.get(name) or html_for_source.get(name)
        if document.name.endswith('.zh-CN.md') and name in chinese_source_views:
            html_name = chinese_source_views[name]
        if html_name:
            fragment = html_fragments.get(name, {}).get(unquote(parsed.fragment), parsed.fragment)
            return site_url + html_name + ('?' + parsed.query if parsed.query else '') + ('#' + fragment if fragment else '')
        return href
    compression = json.loads((root / 'compression-manifest.json').read_text())['files']
    packed = {r['original_path']: r['compressed_path'] for r in compression}

    def route(document, href):
        parsed = urlsplit(href.strip('<>'))
        if parsed.scheme or parsed.netloc or not parsed.path:
            return href
        target = (document.parent / unquote(parsed.path)).resolve()
        if root not in target.parents:
            return href
        name = target.relative_to(root).as_posix()
        if target.suffix != '.html':
            if not target.exists() and name in packed:
                destination = root / packed[name]
                return urlunsplit(('', '', os.path.relpath(destination, document.parent).replace(os.sep, '/'), parsed.query, parsed.fragment))
            return href
        if name in source_views:
            destination = root / source_views[name]
        else:
            destination = target.with_suffix('.md')
            if destination.relative_to(root).as_posix() not in originals:
                generate(name)
        return urlunsplit(('', '', os.path.relpath(destination, document.parent).replace(os.sep, '/'), parsed.query, parsed.fragment))

    def rewrite(document, text):
        def replace(match):
            old = match['url'] or match['mdurl']
            new = route(document, local_reference(document, old))
            parsed = urlsplit(new.strip('<>'))
            label = match[0].split('](', 1)[0].lstrip('![').strip()
            english_link = label == 'EN' or label.lower() == 'english' or label.lower().startswith('english ')
            if document.name.endswith('.zh-CN.md') and not parsed.scheme and not parsed.netloc and parsed.path.endswith('.md') and not parsed.path.endswith('.zh-CN.md') and not english_link:
                target = (document.parent / unquote(parsed.path)).resolve()
                chinese_target = target.with_name(target.stem + '.zh-CN.md')
                if chinese_target.is_file():
                    new = urlunsplit(('', '', os.path.relpath(chinese_target, document.parent).replace(os.sep, '/'), parsed.query, parsed.fragment))
            parsed = urlsplit(new.strip('<>'))
            if not parsed.scheme and not parsed.netloc and parsed.fragment:
                target = (document.parent / unquote(parsed.path)).resolve() if parsed.path else document
                if root in target.parents:
                    name = target.relative_to(root).as_posix()
                    fragment = markdown_fragments.get(name, {}).get(unquote(parsed.fragment), parsed.fragment)
                    new = urlunsplit(('', '', parsed.path, parsed.query, fragment))
            # A label can be identical to its destination, e.g. [01_basic.cpp](01_basic.cpp).
            # Replace the captured destination only; never rewrite the visible label.
            group = 'url' if match['url'] is not None else 'mdurl'
            begin, end = match.start(group) - match.start(), match.end(group) - match.start()
            return match[0][:begin] + site_reference(document, new) + match[0][end:]
        return rewrite_document_links(document, text, replace)

    def generate(name):
        destination = (root / name).with_suffix('.md')
        md_name = destination.relative_to(root).as_posix()
        if md_name in generated:
            return
        source = (root / name).read_text(encoding='utf-8')
        if destination.exists() and md_name not in generated_paths and not destination.read_text().startswith(GENERATED):
            raise ValueError('Refusing to replace an authored document: ' + md_name)
        generated[md_name] = name
        html_for_markdown[md_name] = name
        tree = Tree(source).root
        body = next(descendants(tree, 'body'), tree)
        chinese = 'lang="zh' in source or "lang='zh" in source
        text = Markdown(chinese).text(body)
        image = report_svg(root, name, source)
        if image:
            assets.add(image)
            first = re.search(r'^# [^\n]+', text, re.M)
            image_md = '\n\n![' + ('生产者发送吞吐对比' if chinese else 'Producer submission throughput comparison') + '](' + os.path.relpath(root/image, destination.parent).replace(os.sep, '/') + ')\n\n'
            if first:
                text = text[:first.end()] + image_md + text[first.end():]
        text = re.sub(r'[ \t]+(?=\n)', '', text)
        text = re.sub(r'\n{3,}', '\n\n', text).strip() + '\n'
        destination.write_text(rewrite(destination, text), encoding='utf-8')

    for name in sorted(originals):
        document = root / name
        before = document.read_text(encoding='utf-8')
        after = rewrite(document, before)
        if not name.startswith('test/') and name not in ('LICENSE.zh-CN.md', 'CHANGELOG.md'):
            after = number_sections(after, name.endswith('.zh-CN.md'))
        if name in ('README.md', 'README.zh-CN.md', 'README_DETAIL.md', 'README_DETAIL.zh-CN.md'):
            after = style_flows(with_toc(after, name.endswith('.zh-CN.md')))
            after = after.replace('[HTML overview](README.md)', '[Documentation index](docs/index.md)')
            after = after.replace('[HTML 首页](README.zh-CN.md)', '[文档导航](docs/index.zh-CN.md)')
            # Include both locale indexes even before a website has been published.
            generate('docs/reports/index.zh-CN.html' if name.endswith('.zh-CN.md') else 'docs/reports/index.html')
        if after != before:
            document.write_text(after, encoding='utf-8')
            rewritten += 1
    # Regenerate previous report copies even when no authored document links to
    # their HTML names anymore; they are generated data, not new source documents.
    old_manifest = root / 'tools/github_reading_pages.json'
    if old_manifest.exists():
        for record in json.loads(old_manifest.read_text())['pages']:
            generate(record['html'])
    def clean_markup(source):
        # Code examples retain their exact source text.
        pieces, position = [], 0
        def clean(body):
            body = re.sub(r'<a\s+name=[\x22\x27][^\x22\x27]+[\x22\x27]\s*></a>\s*', '', body)
            body = re.sub(r'<summary>([\s\S]*?)</summary>', lambda m: '\n\n### ' + re.sub('<[^>]+>', '', m[1]).strip() + '\n\n', body)
            body = re.sub(r'</?details\b[^>]*>\s*', '', body)
            return re.sub(r'<br\s*/?>', ' · ', body)
        for fence in FENCED.finditer(source):
            pieces.extend((clean(source[position:fence.start()]), fence[0]))
            position = fence.end()
        pieces.append(clean(source[position:]))
        return ''.join(pieces)

    def map_fragments(name, source, cleaned):
        headings = []
        github_seen, html_seen = {}, {}
        for match in re.finditer(r'^(#{1,6}) (.+)$', cleaned, re.M):
            title = match[2]
            github_base = slug(title)
            number = github_seen.get(github_base, 0); github_seen[github_base] = number + 1
            github = github_base + ('-' + str(number) if number else '')
            base_title = plain_title(title)
            html_base = re.sub(r'[\W_]+', '-', base_title.lower()).strip('-')
            number = html_seen.get(html_base, 0); html_seen[html_base] = number + 1
            html = html_base + ('-' + str(number + 1) if number else '')
            headings.append((title, github, html))
        aliases, back = {}, {}
        for title, github, html in headings:
            aliases[github] = github
            aliases.setdefault(slug(plain_title(title)), github)
            aliases[html] = github
            back[github] = html
        # Original HTML reports also have section/table anchors. Resolve them to
        # the adjacent heading in their Markdown reading copy.
        old_headings = list(re.finditer(r'^#{1,6} (.+)$', source, re.M))
        for anchor in re.finditer(r'<a name="([^"]+)"></a>', source):
            following = next((m for m in old_headings if m.start() > anchor.end()), None)
            previous = [m for m in old_headings if m.start() < anchor.start()]
            summary = re.search(r'<summary>([\s\S]*?)</summary>', source[anchor.end():following.start() if following else len(source)])
            if summary:
                title = re.sub('<[^>]+>', '', summary[1]).strip()
            elif following and not source[anchor.end():following.start()].strip():
                title = following[1]
            elif following and not previous:
                title = following[1]
            elif previous:
                title = previous[-1][1]
            else:
                continue
            found = next((item for item in headings if item[0] == title), None)
            if found:
                aliases[anchor[1]] = found[1]
                if not name in originals:
                    back[found[1]] = anchor[1]
        markdown_fragments[name], html_fragments[name] = aliases, back

    clean_documents = {}
    for name in sorted(originals | set(generated)):
        path = root / name
        source = path.read_text(encoding='utf-8')
        cleaned = clean_markup(source)
        map_fragments(name, source, cleaned)
        clean_documents[name] = cleaned
    for name, text in clean_documents.items():
        path = root / name
        rendered = rewrite(path, text)
        if name in generated:
            rendered = rendered.rstrip() + '\n'
        path.write_text(rendered, encoding='utf-8')
    manifest = {'version': 1, 'markdown_fragments': markdown_fragments, 'html_fragments': html_fragments, 'pages': [{'markdown': name, 'html': generated[name], 'html_sha256': hashlib.sha256((root / generated[name]).read_bytes()).hexdigest()} for name in sorted(generated)], 'assets': sorted(assets)}
    old_manifest.write_text(json.dumps(manifest, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')
    print(json.dumps({'authored_documents_rewritten': rewritten, 'generated_reading_pages': len(generated), 'generated_charts': len(assets)}))


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root', type=Path, default=ROOT)
    args = parser.parse_args()
    build(args.root)
