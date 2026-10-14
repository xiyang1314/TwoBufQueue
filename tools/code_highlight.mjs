/** Small offline lexical highlighter. Source text is escaped, never executed. */
const words = text => new Set(text.split(/\s+/));
const cppKeywords = words('alignas alignof asm auto bool break case catch char char8_t char16_t char32_t class concept const consteval constexpr constinit const_cast continue co_await co_return co_yield decltype default delete do double dynamic_cast else enum explicit export extern false final float for friend if inline int long mutable namespace new noexcept nullptr operator override private protected public register reinterpret_cast requires return short signed sizeof static static_assert static_cast struct switch template this thread_local throw true try typedef typeid typename union unsigned using virtual void volatile wchar_t while');
const cppTypes = words('std size_t ptrdiff_t int8_t int16_t int32_t int64_t uint8_t uint16_t uint32_t uint64_t byte string string_view vector array atomic chrono unique_ptr shared_ptr mutex thread deque tuple optional variant numeric_limits index_sequence index_sequence_for');
const pyKeywords = words('False None True and as assert async await break class continue def del elif else except finally for from global if import in is lambda nonlocal not or pass raise return try while with yield match case');
const pyBuiltins = words('bool bytes dict enumerate float int len list max min open print range set sorted str sum super tuple zip isinstance getattr setattr hasattr type');
const cmakeKeywords = words('if elseif else endif foreach endforeach while endwhile function endfunction macro endmacro set option project cmake_minimum_required add_library add_executable add_subdirectory find_package target_link_libraries target_compile_features target_include_directories include install enable_testing add_test message file string list configure_file');
const asmKeywords = words('add adds sub subs and ands orr eor bic b bl br blr ret cbz cbnz tbz tbnz cmp cmn csel csinc csinv cset ldr ldrb ldrh ldrsb ldrsh ldrsw ldur ldurb ldurh ldapur ldarb ldar ldaxr ldxr ldp str strb strh stur sturb sturh stlur stlr stlxr stxr stp mov movk movz movn fmov fadd fsub fmul fdiv mul madd msub umulh smulh udiv sdiv lsl lsr asr ror ubfx sbfx ubfiz bfi clz rbit rev prfm dmb dsb isb nop adr adrp tbnz stnp ldnp');
const shellKeywords = words('if then else elif fi for do done while case esac function in export return exit set echo printf sudo cmake ctest python python3 node npm git');
const patterns = {
  cpp: /\/\*[\s\S]*?\*\/|\/\/[^\r\n]*|(?:u8|u|U|L)?R"([^\s\\()]*)\([\s\S]*?\)\1"|(?:u8|u|U|L)?"(?:\\[\s\S]|[^"\\])*"|(?:u8|u|U|L)?'(?:\\[\s\S]|[^'\\])*'|^[ \t]*#[^\r\n]*|\b(?:0[xX][\da-fA-F]+|0[bB][01]+|\d[\d']*(?:\.\d*)?(?:[eEpP][+-]?\d+)?)(?:[uUlLfF]+)?\b|\b[A-Za-z_]\w*/gm,
  python: /#[^\r\n]*|(?:fr|rf|r|u|b|f)?"""[\s\S]*?"""|(?:fr|rf|r|u|b|f)?'''[\s\S]*?'''|(?:fr|rf|r|u|b|f)?"(?:\\[\s\S]|[^"\\])*"|(?:fr|rf|r|u|b|f)?'(?:\\[\s\S]|[^'\\])*'|\b(?:0[xX][\da-fA-F]+|\d+(?:\.\d*)?(?:[eE][+-]?\d+)?)\b|\b[A-Za-z_]\w*/g,
  cmake: /#\[(=*)\[[\s\S]*?\]\1\]|#[^\r\n]*|"(?:\\[\s\S]|[^"\\])*"|\$\{[^}]*\}|\b\d+(?:\.\d+)*\b|\b[A-Za-z_]\w*/g,
  asm: /;[^\r\n]*|\/\/[^\r\n]*|"(?:\\[\s\S]|[^"\\])*"|\b(?:0[xX][\da-fA-F]+|\d+)\b|[A-Za-z_.$][\w.$]*/g,
  shell: /#[^\r\n]*|"(?:\\[\s\S]|[^"\\])*"|'[^']*'|\$\{[^}]*\}|\$[A-Za-z_]\w*|\b\d+(?:\.\d+)*\b|\b[A-Za-z_]\w*/g,
};
export const escapeCode = value => String(value).replace(/[&<>"']/g, ch => ({
  '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;',
}[ch]));

export function languageFor(path) {
  if (/(?:^|\/)CMakeLists\.txt$|\.cmake$/i.test(path)) return 'cmake';
  if (/\.(?:cpp|hpp|h|c|cc|cxx|hh|hxx|ipp|inl)$/i.test(path)) return 'cpp';
  if (/\.py$/i.test(path)) return 'python';
  if (/\.(?:asm|s)$/i.test(path)) return 'asm';
  if (/\.(?:sh|bash|zsh)$/i.test(path)) return 'shell';
  return 'text';
}

function tokenKind(value, language, following) {
  if (language === 'cpp') {
    if (value.startsWith('//') || value.startsWith('/*')) return 'comment';
    if (/^\s*#/.test(value)) return 'meta';
    if (/^(?:(?:u8|u|U|L)?R?"|(?:u8|u|U|L)?')/.test(value)) return 'string';
    if (cppKeywords.has(value)) return 'keyword';
    if (cppTypes.has(value) || /^[A-Z][A-Za-z\d]*[a-z]/.test(value)) return 'type';
    if (/^[A-Z_][A-Z_\d]+$/.test(value)) return 'meta';
  } else if (language === 'python') {
    if (value.startsWith('#')) return 'comment';
    if (/^(?:fr|rf|r|u|b|f)?["']/.test(value)) return 'string';
    if (pyKeywords.has(value)) return 'keyword';
    if (pyBuiltins.has(value) || /^[A-Z]/.test(value)) return 'type';
  } else if (language === 'cmake') {
    if (value.startsWith('#')) return 'comment';
    if (value.startsWith('"')) return 'string';
    if (value.startsWith('$')) return 'variable';
    if (cmakeKeywords.has(value.toLowerCase())) return 'keyword';
    if (/^[A-Z_][A-Z_\d]+$/.test(value)) return 'meta';
  } else if (language === 'asm') {
    if (value.startsWith(';') || value.startsWith('//')) return 'comment';
    if (value.startsWith('"')) return 'string';
    if (value.startsWith('.')) return 'meta';
    if (asmKeywords.has(value.toLowerCase()) || /^b\.[a-z]+$/i.test(value)) return 'keyword';
    if (/^(?:[xwqvdshbr]\d+|sp|fp|lr|pc|xzr|wzr)$/i.test(value)) return 'variable';
    if (/^\s*:/.test(following)) return 'function';
  } else if (language === 'shell') {
    if (value.startsWith('#')) return 'comment';
    if (/^["']/.test(value)) return 'string';
    if (value.startsWith('$')) return 'variable';
    if (shellKeywords.has(value)) return 'keyword';
  }
  if (/^\d/.test(value)) return 'number';
  if (/^\s*\(/.test(following)) return 'function';
  return '';
}

/** Return independently balanced HTML for each source line. */
export function highlightedLines(source, language) {
  const chunks = [];
  const pattern = patterns[language];
  let offset = 0;
  if (pattern) {
    for (const match of source.matchAll(pattern)) {
      chunks.push([source.slice(offset, match.index), '']);
      chunks.push([match[0], tokenKind(match[0], language, source.slice(match.index + match[0].length))]);
      offset = match.index + match[0].length;
    }
  }
  chunks.push([source.slice(offset), '']);
  const lines = [''];
  for (const [text, kind] of chunks) {
    const pieces = text.split('\n');
    for (let i = 0; i < pieces.length; ++i) {
      if (i) lines.push('');
      const escaped = escapeCode(pieces[i].replace(/\r$/, ''));
      if (escaped) lines[lines.length - 1] += kind ? '<span class="code-' + kind + '">' + escaped + '</span>' : escaped;
    }
  }
  if (source.endsWith('\n')) lines.pop();
  return lines.length ? lines : [''];
}
