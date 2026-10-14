/* Local source-view controls. No network requests or external dependencies. */
(() => {
  'use strict';
  const wrap = document.getElementById('wrap-code');
  if (wrap) wrap.addEventListener('change', () => {
    document.querySelector('.source-panel').classList.toggle('is-wrapped', wrap.checked);
  });
  const copy = document.getElementById('copy-code');
  if (copy) copy.addEventListener('click', async () => {
    const status = document.getElementById('copy-status');
    const source = JSON.parse(document.getElementById('source-data').textContent).source;
    let copied = false;
    if (navigator.clipboard && window.isSecureContext) {
      try { await navigator.clipboard.writeText(source); copied = true; } catch { /* Try the local-file fallback. */ }
    }
    if (!copied) {
      const input = document.createElement('textarea');
      input.value = source;
      input.setAttribute('readonly', '');
      input.style.cssText = 'position:fixed;left:-9999px;top:0;opacity:0';
      document.body.appendChild(input);
      input.select();
      try { copied = document.execCommand('copy'); } catch { copied = false; }
      input.remove();
      copy.focus();
    }
    status.textContent = copied ? '已复制完整代码' : '浏览器未允许复制，请手动选择代码。';
  });
  const search = document.getElementById('code-search');
  if (search) search.addEventListener('input', () => {
    const term = search.value.trim().toLowerCase();
    let count = 0;
    for (const row of document.querySelectorAll('[data-code-file]')) {
      row.hidden = !row.dataset.codeFile.includes(term);
      if (!row.hidden) ++count;
    }
    for (const group of document.querySelectorAll('.code-group')) {
      group.hidden = !Array.from(group.querySelectorAll('[data-code-file]')).some(row => !row.hidden);
    }
    document.getElementById('filter-count').textContent = count + ' 个文件';
    document.getElementById('empty-filter').hidden = count !== 0;
  });
})();
