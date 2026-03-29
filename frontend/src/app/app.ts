import { Component, OnInit } from '@angular/core';
import { Router, RouterOutlet, NavigationEnd } from '@angular/router';
import { filter } from 'rxjs';
import { NavBarComponent } from './components/nav-bar/nav-bar.component';
import { CpapApiService } from './services/cpap-api.service';

@Component({
  selector: 'app-root',
  standalone: true,
  imports: [RouterOutlet, NavBarComponent],
  template: `
    @if (!isSetup) {
      <app-nav-bar />
    }
    <main>
      <router-outlet />
    </main>
  `,
  styles: [`
    :host { display: block; min-height: 100vh; background: #121212; }
    main { max-width: 1200px; margin: 0 auto; }
  `]
})
export class AppComponent implements OnInit {
  isSetup = false;

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
  }
}
