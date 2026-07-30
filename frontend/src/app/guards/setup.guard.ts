import { inject } from '@angular/core';
import { CanActivateFn, Router } from '@angular/router';
import { catchError, map, of } from 'rxjs';

import { CpapApiService } from '../services/cpap-api.service';

/**
 * SDD-006: send a first-run user to the wizard instead of an empty dashboard.
 *
 * `setup_complete` has been written by POST /api/setup since the wizard shipped,
 * but nothing ever read it, so someone starting the binary for the first time
 * landed on a dashboard with no data and no indication that anything needed
 * configuring.
 *
 * Answered once per app load and cached. The flag only changes when the wizard
 * itself finishes, and that path navigates explicitly, so re-asking on every
 * route change would add a request per navigation for a value that cannot have
 * moved.
 *
 * On an API error this ALLOWS navigation rather than trapping the user in the
 * wizard. A backend that cannot answer /api/config is broken in a way the setup
 * flow cannot fix, and a redirect loop would hide the real failure.
 */
let cachedSetupComplete: boolean | null = null;

export const setupGuard: CanActivateFn = (_route, state) => {
  const api = inject(CpapApiService);
  const router = inject(Router);

  // The wizard itself must stay reachable, or completing setup is impossible.
  if (state.url.startsWith('/setup')) return true;

  if (cachedSetupComplete === true) return true;

  return api.getConfig().pipe(
    map((config: any) => {
      cachedSetupComplete = config?.setup_complete === true;
      return cachedSetupComplete ? true : router.parseUrl('/setup');
    }),
    catchError(() => of(true)),
  );
};

/** Called by the wizard once setup completes, so the gate stops redirecting. */
export function markSetupComplete(): void {
  cachedSetupComplete = true;
}
