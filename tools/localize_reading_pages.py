"""Build separate reading languages without translating measurement/code packets."""
import argparse
import hashlib
from html import escape, unescape
from html.parser import HTMLParser
import json
from pathlib import Path
import re
from urllib.parse import unquote, urlsplit
import os

ROOT = Path(__file__).resolve().parents[1]
HAN = re.compile(r'[\u4e00-\u9fff]')


def chinese_name(name):
    return name[:-5] + '.zh-CN.html'


class Translate(HTMLParser):
    def __init__(self, source, root, name, language, pairs, translations, code=False):
        super().__init__(convert_charrefs=False)
        self.source, self.root, self.name, self.language = source, root, name, language
        self.pairs, self.translations, self.code = pairs, translations, code
        self.offsets = [0]
        for line in source.splitlines(keepends=True):
            self.offsets.append(self.offsets[-1] + len(line))
        self.skip, self.changes, self.missing = [], [], set()
        self.anchor = None

    def position(self):
        line, column = self.getpos()
        return self.offsets[line - 1] + column

    def translated(self, text):
        index_translations = self.translations.get('_index_zh_cn', {})
        if self.language == 'zh-CN' and self.name == 'docs/reports/index.zh-CN.html':
            return index_translations.get(text, text)
        if self.language == 'en' and self.name == 'docs/reports/index.html':
            # Subsequent builds read the localized Chinese index as their input.
            reverse = {chinese: english for english, chinese in index_translations.items()}
            if text in reverse:
                return reverse[text]
        if not HAN.search(text) or self.language == 'zh-CN':
            if self.code and self.language == 'zh-CN' and ' / ' in text:
                if re.fullmatch(r'\d+ (?:files|lines) / .+', text.strip()):
                    return text.split()[0] + (' 个文件' if ' files ' in text else ' 行')
                return (text.split(' · ')[0] + ' · ' if ' · ' in text else '') + text.rsplit(' / ', 1)[1]
            return text
        if self.code:
            if ' / ' in text:
                return text.rsplit(' / ', 1)[0]
            if text.strip() == '中文':
                return text
            if self.anchor and '.zh-CN.html' in self.anchor:
                target = (self.root / Path(self.name).parent / unquote(urlsplit(self.anchor).path)).resolve()
                counterpart = self.pairs.get(target)
                if counterpart and counterpart.is_file():
                    title = re.search(r'<title>(.*?)</title>', counterpart.read_text(encoding='utf-8'), re.S)
                    if title:
                        label = unescape(title.group(1).replace(' · TwoBufQueue', ''))
                        return self.translations.get(label, label) + ' (Chinese)'
        result = self.translations.get(text)
        if result is None:
            self.missing.add(text)
            return text
        return result

    def handle_starttag(self, tag, attrs):
        start = self.position()
        raw = self.get_starttag_text()
        if tag in ('script', 'style', 'pre', 'code'):
            self.skip.append(tag)
        if tag == 'a':
            self.anchor = dict(attrs).get('href')
        if not self.skip:
            changed = raw
            for key, value in attrs:
                if value is None:
                    continue
                replacement = value
                if tag == 'html' and key == 'lang':
                    replacement = self.language
                elif key in ('title', 'aria-label', 'alt', 'placeholder'):
                    replacement = self.translated(value)
                elif key == 'href' and tag == 'a':
                    parsed = urlsplit(value)
                    if parsed.path and not parsed.scheme and not parsed.netloc:
                        target = (self.root / Path(self.name).parent / unquote(parsed.path)).resolve()
                        counterpart = self.pairs.get(target)
                        # Preserve language selectors while routing content links to
                        # the current language, including repeated CN-to-EN builds.
                        following = self.source[start + len(raw):].split('</a>', 1)[0].strip()
                        chinese_target = target.name.endswith('.zh-CN.html')
                        language_selector = (following == 'EN' or following.startswith('English')
                                             or following in ('中文', '简体中文'))
                        needs_counterpart = chinese_target != (self.language == 'zh-CN')
                        if counterpart and needs_counterpart and not language_selector:
                            replacement = os.path.relpath(counterpart, self.root / Path(self.name).parent).replace(os.sep, '/')
                            replacement += ('?' + parsed.query if parsed.query else '') + ('#' + parsed.fragment if parsed.fragment else '')
                if replacement != value:
                    pattern = re.compile(r'(\b' + re.escape(key) + r'\s*=\s*)([\x22\x27])([\s\S]*?)\2')
                    changed = pattern.sub(lambda m: m.group(1) + m.group(2) + escape(replacement, quote=True) + m.group(2), changed, count=1)
            if changed != raw:
                self.changes.append((start, start + len(raw), changed))

    def handle_endtag(self, tag):
        if self.skip and self.skip[-1] == tag:
            self.skip.pop()
        if tag == 'a':
            self.anchor = None

    def handle_data(self, text):
        if self.skip:
            return
        replacement = self.translated(text)
        if replacement != text:
            start = self.position()
            self.changes.append((start, start + len(text), escape(replacement, quote=False)))

    def result(self):
        self.feed(self.source)
        if self.missing:
            raise ValueError('Untranslated reading text in ' + self.name + ': ' + json.dumps(sorted(self.missing), ensure_ascii=False))
        output = self.source
        for start, end, replacement in sorted(self.changes, reverse=True):
            output = output[:start] + replacement + output[end:]
        return output


def packets(source):
    return re.findall(r'<script\b[^>]*type=[\x22\x27]application/json[\x22\x27][^>]*>([\s\S]*?)</script>', source, re.I)


def language_bar(name):
    en, cn = Path(name).name, Path(chinese_name(name)).name
    return ('<div class="reading-language-switch" style="max-width:1180px;margin:12px auto;padding:8px 20px;display:flex;gap:20px;font:14px system-ui">'
            '<a href="' + escape(en) + '">English</a><a href="' + escape(cn) + '">中文</a></div>')


def run(root):
    root = root.resolve()
    report_names = json.loads((root / 'tools/report_pages.json').read_text(encoding='utf-8'))['reports']
    translations = json.loads((root / 'tools/report_translations.json').read_text(encoding='utf-8'))
    docs = json.loads((root / 'tools/documentation_pages.json').read_text(encoding='utf-8'))['sources']
    pairs = {}
    for name in docs:
        if name.endswith('.zh-CN.md') and name.replace('.zh-CN.md', '.md') in docs:
            en = root / name.replace('.zh-CN.md', '.html')
            cn = root / name.replace('.md', '.html')
            pairs[en], pairs[cn] = cn, en
    manifest_path = root / 'docs/code/code-manifest.json'
    code = json.loads(manifest_path.read_text(encoding='utf-8'))
    code_names = ['docs/code/index.html'] + [record['html_path'] for record in code['files']]
    for name in report_names + code_names:
        en, cn = root / name, root / chinese_name(name)
        pairs[en], pairs[cn] = cn, en

    prepared = []
    for name in report_names:
        en, cn = root / name, root / chinese_name(name)
        original = en.read_text(encoding='utf-8')
        marker = '<!-- Localized report: ' + name + ' -->'
        if marker in original:
            original = cn.read_text(encoding='utf-8')
        original = original.replace(marker, '')
        # The Chinese input keeps its original report body and JSON packet.
        original = re.sub(r'<div class="reading-language-switch"[^>]*>[\s\S]*?</div>', '', original)
        english = Translate(original, root, name, 'en', pairs, translations).result()
        if name == 'docs/reports/index.html':
            english = english.replace('Additional measurements and earlier experiments', 'Additional measurements and earlier experiments (Chinese records)')
        chinese = Translate(original, root, chinese_name(name), 'zh-CN', pairs, translations).result()
        for target, text in ((en, english), (cn, chinese)):
            assert packets(text) == packets(original), ('measurement packet changed', name)
            text = text.replace('<body', marker + '<body', 1)
            text = re.sub(r'(<body\b[^>]*>)', lambda m: m.group(1) + language_bar(name), text, count=1)
            prepared.append((target, text))

    for name in code_names:
        original = (root / name).read_text(encoding='utf-8')
        original = re.sub(r'<div class="reading-language-switch"[^>]*>[\s\S]*?</div>', '', original)
        for language, output in (('en', name), ('zh-CN', chinese_name(name))):
            text = Translate(original, root, output, language, pairs, translations, code=True).result()
            assert packets(text) == packets(original), ('code copy packet changed', name)
            # Source pages' existing EN/CN links should switch this same view.
            text = re.sub(r'<div class="languages">[\s\S]*?</div>', language_bar(name), text, count=1)
            prepared.append((root / output, text))
    for record in code['files']:
        record['html_path_zh_cn'] = chinese_name(record['html_path'])
    for name in docs:
        if name.endswith('.zh-CN.md'):
            output = name[:-3] + '.html'
            path = root / output
            prepared.append((path, Translate(path.read_text(encoding='utf-8'), root, output, 'zh-CN', pairs, translations).result()))
    for path, text in prepared:
        path.write_text(text, encoding='utf-8')
    manifest_path.write_text(json.dumps(code, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')
    return {'localized_reports': len(report_names), 'bilingual_source_views': len(code_names), 'pages_written': len(prepared)}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root', type=Path, default=ROOT)
    args = parser.parse_args()
    print(json.dumps(run(args.root)))


if __name__ == '__main__':
    main()
