"""Generate separate English and Chinese offline reading directories."""
import argparse
import hashlib
import html
from html.parser import HTMLParser
import json
from pathlib import Path
import posixpath
import re
from urllib.parse import quote, unquote, urlsplit, urlunsplit

ROOT = Path(__file__).resolve().parents[1]
MARKER = '<!-- Generated language reading page from '
ATTRIBUTES = re.compile(r'''(?P<prefix>(?<![\w:-])(?:href|src|poster)\s*=\s*)(?P<quote>["'])(?P<url>.*?)(?P=quote)''', re.I | re.S)


def digest(data):
    return hashlib.sha256(data).hexdigest()


def build(root):
    root = root.resolve()
    selected = {record['path'] for record in json.loads((root / 'release-manifest.json').read_text())['files']}
    selected.update(name[:-3] + '.html' for name in json.loads((root / 'tools/documentation_pages.json').read_text())['sources'])
    code = json.loads((root / 'docs/code/code-manifest.json').read_text())
    selected.update(record[key] for record in code['files'] for key in ('html_path', 'html_path_zh_cn') if record.get(key))
    selected.update(page['html'] for page in json.loads((root / 'tools/github_reading_pages.json').read_text())['pages'])
    sources = sorted(name for name in selected if name.endswith('.html')
                     and not name.startswith(('en/', 'zh_cn/', 'docs/performance_data/'))
                     and name != 'START_HERE.html')
    site = json.loads((root / 'tools/document_site.json').read_text())
    site_url = site.get('site_url', '').rstrip('/')
    parsed_site = urlsplit(site_url)
    mapping = {}
    owners = {}
    language_for = {}
    for source in sources:
        text = (root / source).read_text(encoding='utf-8')
        explicit_chinese = source.endswith('.zh-CN.html')
        paired = source[:-5] + '.zh-CN.html' in selected
        detected = re.search(r'<html\b[^>]*\blang=["\x27]([^"\x27]+)', text, re.I)
        chinese = explicit_chinese or (not paired and detected and detected[1].lower().startswith('zh'))
        language = 'zh_cn' if chinese else 'en'
        relative = source.replace('.zh-CN.html', '.html')
        if relative == 'README.html':
            relative = 'index.html'
        target = language + '/' + relative
        if target in owners:
            raise ValueError('Language page collision: ' + target + ': ' + owners[target] + ', ' + source)
        owners[target] = source
        mapping[source] = target
        language_for[source] = language

    def rewrite_url(value, source, target):
        value = html.unescape(value)
        parsed = urlsplit(value)
        if parsed.scheme or parsed.netloc:
            if parsed.scheme not in ('http', 'https') or parsed.netloc != parsed_site.netloc:
                return None
            prefix = parsed_site.path.rstrip('/') + '/'
            if not parsed.path.startswith(prefix):
                return None
            resolved = unquote(parsed.path[len(prefix):])
        elif not parsed.path:
            return None
        elif parsed.path.startswith('/'):
            prefix = parsed_site.path.rstrip('/') + '/'
            resolved = unquote(parsed.path[len(prefix):] if parsed.path.startswith(prefix) else parsed.path.lstrip('/'))
        else:
            resolved = posixpath.normpath(posixpath.join(posixpath.dirname(source), unquote(parsed.path)))
        if resolved.startswith('../') or resolved.startswith('/'):
            raise ValueError('Link escapes source tree: ' + source + ': ' + value)
        resolved = posixpath.normpath(resolved)
        if resolved in ('.', ''):
            resolved = 'START_HERE.html'
        directory = (root / resolved).is_dir()
        if directory and (root / resolved / 'index.html').is_file():
            resolved = resolved.rstrip('/') + '/index.html'
            directory = False
        destination = mapping.get(resolved, resolved)
        relative = posixpath.relpath(destination, posixpath.dirname(target))
        if directory:
            relative += '/'
        return urlunsplit(('', '', quote(relative, safe='/@:+-._~'), parsed.query, parsed.fragment))

    records = []
    for source in sources:
        target = mapping[source]
        text = (root / source).read_text(encoding='utf-8')
        offsets = [0] + [match.end() for match in re.finditer('\n', text)]
        edits = []

        class Links(HTMLParser):
            def __init__(self):
                super().__init__(convert_charrefs=False)

            def handle_starttag(self, tag, attrs):
                raw = self.get_starttag_text()

                def replacement(match):
                    rewritten = rewrite_url(match['url'], source, target)
                    if rewritten is None:
                        return match[0]
                    return match['prefix'] + match['quote'] + html.escape(rewritten, quote=True) + match['quote']

                updated = ATTRIBUTES.sub(replacement, raw)
                if updated != raw:
                    line, column = self.getpos()
                    start = offsets[line - 1] + column
                    edits.append((start, start + len(raw), updated))

            def handle_startendtag(self, tag, attrs):
                self.handle_starttag(tag, attrs)

        parser = Links()
        parser.feed(text)
        parser.close()
        result = text
        for start, end, updated in reversed(edits):
            result = result[:start] + updated + result[end:]
        result = MARKER + source + '; source SHA256 ' + digest(text.encode()) + '. -->\n' + result
        path = root / target
        if path.exists() and not path.read_text(encoding='utf-8').startswith(MARKER):
            raise FileExistsError('Refusing to overwrite unrelated language document: ' + target)
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(result, encoding='utf-8')
        records.append({'source_path': source, 'source_sha256': digest(text.encode()),
                        'path': target, 'language': language_for[source], 'sha256': digest(result.encode())})
    entries = {'en': 'en/index.html', 'zh_cn': 'zh_cn/index.html'}
    assert set(entries.values()) <= set(owners)
    manifest = {'schema_version': 1, 'entries': entries, 'policy': 'Generated reading copies; one shared C++ source tree and shared assets.', 'files': records}
    (root / 'tools/language_pages.json').write_text(json.dumps(manifest, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')
    return {'language_pages': len(records), 'english_pages': sum(r['language'] == 'en' for r in records),
            'chinese_pages': sum(r['language'] == 'zh_cn' for r in records), 'entries': entries}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root', type=Path, default=ROOT)
    args = parser.parse_args()
    print(json.dumps(build(args.root), ensure_ascii=False))


if __name__ == '__main__':
    main()
