import {
  ApplicationConfig,
  inject,
  provideAppInitializer,
  provideBrowserGlobalErrorListeners,
  provideZoneChangeDetection,
} from '@angular/core';
import { provideRouter } from '@angular/router';
import { provideHttpClient, withInterceptors } from '@angular/common/http';
import { TranslateLoader, provideTranslateService } from '@ngx-translate/core';

import { routes } from './app.routes';
import { apiBaseInterceptor } from './interceptors/api-base.interceptor';
import { BundledTranslateLoader } from './i18n/bundled-loader';
import { LanguageService } from './services/language.service';

export const appConfig: ApplicationConfig = {
  providers: [
    provideZoneChangeDetection({ eventCoalescing: true }),
    provideBrowserGlobalErrorListeners(),
    provideRouter(routes),
    // SDD-021: rewrites root-absolute /api calls to sit under <base href>, so
    // the same build works at the root and underneath a Home Assistant Ingress
    // prefix. A no-op outside Ingress.
    provideHttpClient(withInterceptors([apiBaseInterceptor])),
    // SDD-080. The loader is bundled rather than HTTP-backed for the same
    // reason the interceptor above exists: under an Ingress prefix a
    // root-absolute asset fetch resolves somewhere else. See bundled-loader.ts.
    provideTranslateService({
      fallbackLang: 'en',
      // The token MUST be TranslateLoader, the abstract class TranslateService
      // injects. Providing the concrete class under its own name satisfies
      // nobody: TranslateService then fails to construct, and because every
      // component pulls in TranslatePipe the whole app stops bootstrapping.
      loader: { provide: TranslateLoader, useClass: BundledTranslateLoader },
    }),
    // Resolve and apply the language BEFORE the first view renders, so nobody
    // sees a frame of raw translation keys.
    provideAppInitializer(() => inject(LanguageService).init()),
  ]
};
