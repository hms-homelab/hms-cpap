import { Component, OnInit } from '@angular/core';
import { Router, RouterOutlet, NavigationEnd } from '@angular/router';
import { filter } from 'rxjs';
import { NavBarComponent } from './components/nav-bar/nav-bar.component';
import { CpapApiService } from './services/cpap-api.service';

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
  imports: [RouterOutlet, NavBarComponent],
  template: `
    @if (!isSetup) {
      <app-nav-bar />
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
          <strong>Nothing is being imported.</strong>
          <span class="problem">{{ configError.problem }}.</span>
          <span class="remedy">{{ configError.remedy }}</span>
        </div>
        <a class="action" href="/settings">Open Settings</a>
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
    .config-error .body { flex: 1; display: flex; flex-direction: column; gap: 0.15rem; }
    .config-error strong { color: #ff8a80; }
    .config-error .remedy { color: #ffb3ad; }
    .config-error .action {
      flex-shrink: 0;
      align-self: center;
      padding: 0.4rem 0.75rem;
      border: 1px solid rgba(244, 67, 54, 0.5);
      border-radius: 4px;
      color: #ff8a80;
      text-decoration: none;
      white-space: nowrap;
    }
    .config-error .action:hover { background: rgba(244, 67, 54, 0.16); }
  `]
})
export class AppComponent implements OnInit {
  isSetup = false;
  configError: ConfigError | null = null;

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
      },
      error: () => {
        // Capabilities unavailable. Stay quiet rather than cry wolf.
      }
    });
  }
}
