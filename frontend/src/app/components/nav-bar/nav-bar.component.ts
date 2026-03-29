import { Component } from '@angular/core';
import { RouterLink, RouterLinkActive } from '@angular/router';

@Component({
  selector: 'app-nav-bar',
  standalone: true,
  imports: [RouterLink, RouterLinkActive],
  template: `
    <nav class="nav-bar">
      <div class="nav-brand">HMS-CPAP</div>
      <div class="nav-links">
        <a routerLink="/dashboard" routerLinkActive="active">Dashboard</a>
        <a routerLink="/sessions" routerLinkActive="active">Sessions</a>
        <a routerLink="/settings" routerLinkActive="active">Settings</a>
      </div>
    </nav>
  `,
  styles: [`
    .nav-bar {
      display: flex; align-items: center; gap: 2rem;
      padding: 0 1.5rem; height: 56px;
      background: #1a1a2e; color: #e0e0e0;
      border-bottom: 1px solid #333;
    }
    .nav-brand { font-size: 1.2rem; font-weight: 700; color: #64b5f6; }
    .nav-links { display: flex; gap: 1rem; }
    .nav-links a {
      color: #aaa; text-decoration: none; padding: 0.4rem 0.8rem;
      border-radius: 4px; transition: all 0.2s;
    }
    .nav-links a:hover { color: #fff; background: rgba(255,255,255,0.08); }
    .nav-links a.active { color: #64b5f6; background: rgba(100,181,246,0.12); }
  `]
})
export class NavBarComponent {}
