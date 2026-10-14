"""Number reading-guide headings while retaining their original link anchors."""
import re
import unicodedata

PREFIX = re.compile(r'^(?:[一二三四五六七八九十百]+、\s*|\d+(?:\.\d+)*[.)、]?\s+)')


def plain_title(title):
    return PREFIX.sub('', title)


def slug(title):
    title = re.sub(r'<[^>]*>|[`*]', '', title).lower()
    return ''.join(c for c in title if c in '-_ ' or unicodedata.category(c)[0] in 'LNM').replace(' ', '-')


def chinese_number(value):
    digits = '零一二三四五六七八九'
    if value < 10:
        return digits[value]
    if value < 100:
        tens, units = divmod(value, 10)
        return (digits[tens] if tens > 1 else '') + '十' + (digits[units] if units else '')
    raise ValueError('Too many chapters')


def without_contents(source):
    source = re.sub(r'<!-- README_TOC:START -->[\s\S]*?<!-- README_TOC:END -->\s*', '', source)
    return re.sub(r'^\*\*(?:目录|Contents)\*\*\n\n(?:[ \t]*- \[[^\n]+\]\(#[^\n]+\)\n)+\n---\n*', '', source, flags=re.M)


def number_sections(source, chinese, add_anchors=False):
    # The contents list is rebuilt after chapter numbering.
    source = without_contents(source)
    output, ids = [], {}
    existing = set(re.findall(r'<a name="([^"]+)"></a>', source))
    chapter = section = item = 0
    fence = None
    for line in source.splitlines(keepends=True):
        marker = re.match(r'^\s*(`{3,}|~{3,})', line)
        if marker:
            if fence is None:
                fence = marker[1][0]
            elif marker[1][0] == fence:
                fence = None
            output.append(line)
            continue
        match = re.match(r'^(#{2,4}) (.+?)(\n?)$', line) if fence is None else None
        if not match:
            output.append(line)
            continue
        level, title = len(match[1]), plain_title(match[2])
        if level == 2:
            chapter += 1
            section = item = 0
            prefix = chinese_number(chapter) + '、' if chinese else str(chapter) + '. '
        elif level == 3 and chapter:
            section += 1
            item = 0
            prefix = str(chapter) + '.' + str(section) + ' '
        elif level == 4 and chapter and section:
            item += 1
            prefix = str(chapter) + '.' + str(section) + '.' + str(item) + ' '
        else:
            output.append(line)
            continue
        base = slug(title)
        occurrence = ids.get(base, 0)
        ids[base] = occurrence + 1
        anchor = base + ('-' + str(occurrence + 1) if occurrence else '')
        if add_anchors and anchor not in existing:
            output.append('<a name="' + anchor + '"></a>\n\n')
            existing.add(anchor)
        output.append(match[1] + ' ' + prefix + title + '\n')
    return ''.join(output)
