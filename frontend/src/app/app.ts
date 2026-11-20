import { Component, OnInit } from '@angular/core';
import { Router, RouterOutlet, RouterLink, NavigationEnd } from '@angular/router';
import { filter } from 'rxjs';
import { TranslatePipe } from '@ngx-translate/core';
import { NavBarComponent } from './components/nav-bar/nav-bar.component';
import { CpapApiService } from './services/cpap-api.service';
import { DataDirFallback, dataDirFallback } from './utils/data-dir-fallback';

/** SDD-010: what /api/capabilities reports when the local folder is misconfigured. */
interface ConfigError {
  layout: string;
  path: string;
  problem: string;
  remedy: string;
}

@Component({
  selector: 'app-root',
  standalone: true,
  imports: [RouterOutlet, RouterLink, NavBarComponent, TranslatePipe],
  template: `
    @if (!isSetup) {
      <app-nav-bar />
    }
    <!--
      SDD-067 D7. The Home Assistant add-on could not write the data directory
      it was given and this run keeps its data in /data. On every page, the
      setup wizard included. Gone on the first start that can write there.
    -->
    @if (dataDirFallback) {
      <div class="data-dir-fallback" role="alert">
        <span class="icon" aria-hidden="true">&#9888;</span>
        <div class="body">
          <strong>{{ 'shell.dataDirFallback.title' | translate }}</strong>
          <span>{{ 'shell.dataDirFallback.body' | translate: dataDirFallback }}</span>
        </div>
      </div>
    }
    <!--
      SDD-010. Ingestion has stopped, but the dashboard keeps serving nights
      already stored, so without this the product looks like it is working and
      quietly importing nothing. Lives in the shell so it shows on every page,
      not just the dashboard. Not polled: a misconfigured folder cannot fix
      itself, and correcting it in Settings reloads the view anyway.
    -->
    @if (configError && !isSetup) {
      <div class="config-error" role="alert">
        <span class="icon" aria-hidden="true">&#9888;</span>
        <div class="body">
          <strong>{{ 'shell.configError.title' | translate }}</strong>
          <!-- problem and remedy come from /api/capabilities, which is
               English-only server text. SDD-080 leaves them as-is rather than
               inventing a translation the backend cannot actually produce. -->
          <span class="problem">{{ configError.problem }}.</span>
          <span class="remedy">{{ configError.remedy }}</span>
        </div>
        <!-- routerLink, not href: a raw href ignores <base href> handling and
             would leave the Ingress prefix (SDD-021) behind. -->
        <a class="action" routerLink="/settings">{{ 'shell.configError.action' | translate }}</a>
      </div>
    }
    <main>
      <router-outlet />
    </main>
  `,
  styles: [`
    :host { display: block; min-height: 100vh; background: #121212; }
    main { max-width: 1200px; margin: 0 auto; }

    .config-error {
      max-width: 1200px;
      margin: 0.75rem auto;
      display: flex;
      /* The action drops under the text when both do not fit on one line
         (a phone), instead of running past the edge of the screen. */
      flex-wrap: wrap;
      align-items: flex-start;
      gap: 0.75rem;
      padding: 0.85rem 1rem;
      border: 1px solid rgba(244, 67, 54, 0.5);
      border-left: 4px solid #f44336;
      border-radius: 6px;
      background: rgba(244, 67, 54, 0.09);
      color: #ffd7d4;
      font-size: 0.9rem;
      line-height: 1.45;
    }
    .config-error .icon { color: #f44336; font-size: 1.1rem; line-height: 1.3; }
    /* min-width 0 and anywhere: the problem names the configured folder, a
       path with no spaces, which otherwise holds the column wider than a phone. */
    .config-error .body { flex: 1 1 14rem; min-width: 0; overflow-wrap: anywhere;
                          display: flex; flex-direction: column; gap: 0.15rem; }
    .config-error strong { color: #ff8a80; }
    .config-error .remedy { color: #ffb3ad; }
    /* A secondary button: outline in the dashboard blue, as everywhere in hms-cpap. */
    .config-error .action {
      flex-shrink: 0;
      align-self: center;
      padding: 0.6rem 1.25rem;
      border: 1px solid #1976d2;
      border-radius: 6px;
      color: #64b5f6;
      font-weight: 600;
      text-decoration: none;
      white-space: nowrap;
      transition: all 0.2s;
    }
    .config-error .action:hover { background: #1565c0; color: #fff; }

    .data-dir-fallback {
      max-width: 1200px;
      margin: 0.75rem auto;
      display: flex;
      align-items: flex-start;
      gap: 0.75rem;
      padding: 0.85rem 1rem;
      border: 1px solid rgba(255, 183, 77, 0.5);
      border-left: 4px solid #ffb74d;
      border-radius: 6px;
      background: rgba(255, 183, 77, 0.09);
      color: #ffe0b2;
      font-size: 0.9rem;
      line-height: 1.45;
    }
    .data-dir-fallback .icon { color: #ffb74d; font-size: 1.1rem; line-height: 1.3; }
    /* The two directories are paths with no spaces. */
    .data-dir-fallback .body { flex: 1 1 14rem; min-width: 0; overflow-wrap: anywhere;
                               display: flex; flex-direction: column; gap: 0.15rem; }
    .data-dir-fallback strong { color: #ffcc80; }
  `]
})
export class AppComponent implements OnInit {
  isSetup = false;
  configError: ConfigError | null = null;
  dataDirFallback: DataDirFallback | null = null;

  constructor(private api: CpapApiService, private router: Router) {
    this.router.events.pipe(
      filter((e): e is NavigationEnd => e instanceof NavigationEnd)
    ).subscribe(e => {
      this.isSetup = e.urlAfterRedirects.startsWith('/setup');
    });
  }

  ngOnInit(): void {
    this.api.getConfig().subscribe({
      next: (cfg) => {
        if (!cfg.setup_complete) {
          this.router.navigate(['/setup']);
        }
      },
      error: () => {
        // Config endpoint unavailable — proceed to dashboard
      }
    });

    this.api.getCapabilities().subscribe({
      next: (caps: any) => {
        this.configError = caps?.config_error ?? null;
        this.dataDirFallback = dataDirFallback(caps);
      },
      error: () => {
        // Capabilities unavailable. Stay quiet rather than cry wolf.
      }
    });
  }
}
