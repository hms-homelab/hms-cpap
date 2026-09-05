import { Injectable, inject, signal } from '@angular/core';
import { DOCUMENT } from '@angular/common';
import { TranslateService } from '@ngx-translate/core';

export type Lang = 'en' | 'es' | 'fr' | 'pt' | 'hu';

/**
 * Languages this frontend ships dictionaries for, in switcher display order.
 * `pt` is Brazilian Portuguese under the neutral code (SDD-080).
 */
const LANGS: readonly Lang[] = ['en', 'es', 'fr', 'pt', 'hu'];

/** Native name for each language, shown in its own language in the switcher. */
export const LANG_NAMES: Readonly<Record<Lang, string>> = {
  en: 'English',
  es: 'Español',
  fr: 'Français',
  pt: 'Português',
  hu: 'Magyar',
};

const STORAGE_KEY = 'hms_cpap_lang';

/**
 * App-wide language state on top of ngx-translate.
 *
 * DELIBERATELY SIMPLER THAN THE CLOUD FRONTEND'S SERVICE OF THE SAME NAME.
 * That one mirrors its choice into a blog key and PUTs it to /v1/user/language
 * so the API can localize summaries and emails. hms-cpap has no accounts, no
 * blog and no server-side text generation — it is a local collector with a
 * dashboard — so the choice lives entirely in this browser and there is nothing
 * to sync. Adding a server round-trip here would invent a dependency the
 * product does not have.
 *
 * Resolution order: explicit stored choice -> `navigator.language` primary
 * subtag -> English.
 */
@Injectable({ providedIn: 'root' })
export class LanguageService {
  readonly available: readonly Lang[] = LANGS;
  readonly current = signal<Lang>('en');

  private translate = inject(TranslateService);
  private doc = inject(DOCUMENT);

  /** Call once at bootstrap, before the first view renders. */
  init(): void {
    this.translate.addLangs([...LANGS]);
    this.translate.setFallbackLang('en');
    this.apply(this.resolveInitial());
  }

  /** User-driven switch. Persists to this browser only. */
  use(lang: Lang): void {
    if (!LANGS.includes(lang)) return;
    this.apply(lang);
    try {
      localStorage.setItem(STORAGE_KEY, lang);
    } catch {
      /* storage disabled (private mode) - keep runtime state only */
    }
  }

  private apply(lang: Lang): void {
    this.translate.use(lang);
    this.current.set(lang);
    // Keeps `:lang()` CSS, hyphenation and screen-reader pronunciation correct.
    this.doc.documentElement?.setAttribute('lang', lang);
  }

  private resolveInitial(): Lang {
    const stored = this.readStored();
    if (stored) return stored;
    // Match on the primary subtag so pt-BR, es-MX, fr-CA and hu-HU all land on
    // the right dictionary. Anything we do not ship falls through to English.
    const nav = (navigator.language || navigator.languages?.[0] || '').toLowerCase();
    const primary = nav.split('-')[0];
    return LANGS.find((l) => l === primary) ?? 'en';
  }

  private readStored(): Lang | null {
    try {
      const v = localStorage.getItem(STORAGE_KEY) as Lang | null;
      return v && LANGS.includes(v) ? v : null;
    } catch {
      return null;
    }
  }
}
