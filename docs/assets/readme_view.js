/* Local overview navigation. No network calls or external dependencies. */
(() => {
  'use strict';
  const toggle = document.getElementById('toc-toggle');
  const sidebar = document.getElementById('overview-toc');
  const layout = document.querySelector('.overview-layout');
  if (!toggle || !sidebar || !layout) return;
  const label = toggle.querySelector('.toc-toggle-label');
  const preferenceKey = 'twobufqueue.overview.toc-visible';

  // Local files or privacy settings can disable either storage API.
  function savedVisibility() {
    for (const name of ['localStorage', 'sessionStorage']) {
      try {
        const value = window[name].getItem(preferenceKey);
        if (value === 'true' || value === 'false') return value === 'true';
      } catch { /* Try the next storage mechanism. */ }
    }
    try {
      const value = window.history.state?.[preferenceKey];
      if (typeof value === 'boolean') return value;
    } catch { /* Keep the current display if history is unavailable. */ }
  }

  function rememberVisibility(show) {
    for (const name of ['localStorage', 'sessionStorage']) {
      try {
        window[name].setItem(preferenceKey, String(show));
      } catch { /* History also preserves the choice for back navigation. */ }
    }
    try {
      const state = window.history.state;
      if (state == null || (typeof state === 'object' && !Array.isArray(state))) {
        window.history.replaceState({ ...state, [preferenceKey]: show }, '');
      }
    } catch { /* Storage or the preserved page can still retain the choice. */ }
  }

  function showContents(show) {
    sidebar.hidden = !show;
    layout.classList.toggle('toc-collapsed', !show);
    const text = show ? toggle.dataset.hideLabel : toggle.dataset.showLabel;
    toggle.setAttribute('aria-expanded', String(show));
    toggle.setAttribute('aria-label', text);
    toggle.title = text;
    if (label) label.textContent = text;
  }
  toggle.addEventListener('click', () => {
    const show = sidebar.hidden;
    showContents(show);
    rememberVisibility(show);
  });
  showContents(savedVisibility() ?? true);
  // Also restore pages returned from the browser's back/forward cache.
  window.addEventListener('pageshow', () => {
    showContents(savedVisibility() ?? !sidebar.hidden);
  });
})();
