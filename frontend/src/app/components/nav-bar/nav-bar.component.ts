import { Component, OnInit } from '@angular/core';
import { CommonModule } from '@angular/common';
import { RouterLink, RouterLinkActive } from '@angular/router';
import { CpapApiService } from '../../services/cpap-api.service';

@Component({
  selector: 'app-nav-bar',
  standalone: true,
  imports: [CommonModule, RouterLink, RouterLinkActive],
  template: `
    <nav class="nav-bar">
      <div class="nav-brand"><img src="logo.png" alt="" class="nav-logo" />HMS-CPAP <span class="nav-version" *ngIf="version">v{{version}}</span></div>
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
    .nav-brand { display: flex; align-items: center; gap: 0.5rem; font-size: 1.2rem; font-weight: 700; color: #64b5f6; }
    .nav-logo { width: 24px; height: 24px; }
    .nav-version { font-size: 0.7rem; font-weight: 400; color: #666; }
    .nav-links { display: flex; gap: 1rem; }
    .nav-links a {
      color: #aaa; text-decoration: none; padding: 0.4rem 0.8rem;
      border-radius: 4px; transition: all 0.2s;
    }
    .nav-links a:hover { color: #fff; background: rgba(255,255,255,0.08); }
    .nav-links a.active { color: #64b5f6; background: rgba(100,181,246,0.12); }
  `]
})
export class NavBarComponent implements OnInit {
  version = '';
  constructor(private api: CpapApiService) {}
  ngOnInit() { this.api.getHealth().subscribe(h => this.version = h.version); }
}
