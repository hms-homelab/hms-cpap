import { TranslateLoader, TranslationObject } from '@ngx-translate/core';
import { Observable, of } from 'rxjs';
import en from './en.json';
import es from './es.json';
import fr from './fr.json';
import pt from './pt.json';
import hu from './hu.json';

/**
 * SDD-080. Dictionaries are bundled rather than fetched over HTTP.
 *
 * This matters more here than it does in the cloud frontend. hms-cpap is served
 * from disk by the Drogon binary and also runs behind a Home Assistant Ingress
 * prefix, where a root-absolute fetch of /assets/i18n/xx.json would resolve to
 * the wrong place — the same class of problem the apiBaseInterceptor exists to
 * fix for /api. Bundling sidesteps the question: there is no URL to get wrong,
 * and no flash of raw translation keys while a request is in flight.
 */
const DICTS: Record<string, TranslationObject> = {
  en: en as unknown as TranslationObject,
  es: es as unknown as TranslationObject,
  fr: fr as unknown as TranslationObject,
  pt: pt as unknown as TranslationObject,
  hu: hu as unknown as TranslationObject,
};

export class BundledTranslateLoader implements TranslateLoader {
  getTranslation(lang: string): Observable<TranslationObject> {
    return of(DICTS[lang] ?? DICTS['en']);
  }
}
