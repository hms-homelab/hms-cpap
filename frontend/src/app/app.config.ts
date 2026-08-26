import { ApplicationConfig, provideBrowserGlobalErrorListeners, provideZoneChangeDetection } from '@angular/core';
import { provideRouter } from '@angular/router';
import { provideHttpClient, withInterceptors } from '@angular/common/http';

import { routes } from './app.routes';
import { apiBaseInterceptor } from './interceptors/api-base.interceptor';

export const appConfig: ApplicationConfig = {
  providers: [
    provideZoneChangeDetection({ eventCoalescing: true }),
    provideBrowserGlobalErrorListeners(),
    provideRouter(routes),
    // SDD-021: rewrites root-absolute /api calls to sit under <base href>, so
    // the same build works at the root and underneath a Home Assistant Ingress
    // prefix. A no-op outside Ingress.
    provideHttpClient(withInterceptors([apiBaseInterceptor])),
  ]
};
