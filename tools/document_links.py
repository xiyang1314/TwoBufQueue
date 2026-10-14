"""Find document links without interpreting C++ or shell examples as Markdown."""
import re
from pathlib import Path

LINK = re.compile(r'(?P<html>(?:href|src)=["\x27])(?P<url>[^"\x27]+)(?=["\x27])|'
                  r'(?P<markdown>!?\[[^\]\n]*\]\()(?P<mdurl>[^)\n]+)(?=\))')
HTML_LINK = re.compile(r'(?P<html>(?:href|src)=["\x27])(?P<url>[^"\x27]+)(?=["\x27])')
FENCED = re.compile(r'(?ms)^\s*(`{3,}|~{3,})[^\n]*\n.*?^\s*\1\s*$')
INLINE_CODE = re.compile(r'(`+)(?!`)[^\n]*?\1(?!`)')
HTML_RAW = re.compile(r'(<(?:script|style)\b[^>]*>)(.*?)(</(?:script|style)\s*>)', re.I | re.S)


def blank(match):
    return ''.join('\n' if char == '\n' else ' ' for char in match.group(0))


def document_links(document, text):
    """Yield matches in the original text; ignore fenced and inline code."""
    is_html = Path(document).suffix == '.html'
    masked = HTML_RAW.sub(lambda m: m[1] + ''.join('\n' if c == '\n' else ' ' for c in m[2]) + m[3], text) if is_html else INLINE_CODE.sub(blank, FENCED.sub(blank, text))
    pattern = HTML_LINK if is_html else LINK
    for candidate in pattern.finditer(masked):
        match = pattern.match(text, candidate.start())
        if match is not None:
            yield match


def rewrite_document_links(document, text, replace):
    parts = []
    cursor = 0
    for match in document_links(document, text):
        parts.append(text[cursor:match.start()])
        parts.append(replace(match))
        cursor = match.end()
    return ''.join(parts) + text[cursor:]
