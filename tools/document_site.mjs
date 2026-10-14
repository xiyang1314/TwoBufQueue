/** Keep offline HTML navigation local when GitHub reading links use Pages. */
import { readFile } from 'node:fs/promises';
import { dirname, relative, resolve, sep } from 'node:path';

export async function documentSite(root) {
  const config = JSON.parse(await readFile(resolve(root, 'tools/document_site.json'), 'utf8'));
  const reading = JSON.parse(await readFile(resolve(root, 'tools/github_reading_pages.json'), 'utf8'));
  const value = config.site_url;
  if (value) {
    const url = new URL(value);
    if (url.protocol !== 'https:' || url.username || url.password || url.search || url.hash) {
      throw new Error('Document site_url must be a plain HTTPS base URL.');
    }
  }
  const base = value ? value.replace(/\/+$/, '') + '/' : null;
  return {
    localHref(document, href) {
      if (base && href.startsWith(base)) {
        const target = href.slice(base.length).match(/^([^?#]*)([?#].*)?$/);
        if (!target) return href;
        const path = resolve(root, decodeURIComponent(target[1]));
        if (!path.startsWith(root + sep)) throw new Error('Document site link escapes the publication tree.');
        return relative(dirname(document), path).split(sep).join('/') + (target[2] || '');
      }
      if (/^(?:[a-z][a-z\d+.-]*:|\/\/)/i.test(href)) return href;
      const target = href.match(/^([^?#]*)(\?[^#]*)?(?:#(.*))?$/);
      if (!target || !target[3] || (target[1] && !target[1].endsWith('.md'))) return href;
      const path = target[1] ? resolve(dirname(document), decodeURIComponent(target[1])) : document;
      const name = relative(root, path).split(sep).join('/');
      const fragment = reading.html_fragments?.[name]?.[decodeURIComponent(target[3])];
      return fragment ? target[1] + (target[2] || '') + '#' + fragment : href;
    },
  };
}
