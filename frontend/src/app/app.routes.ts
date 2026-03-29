import { Routes } from '@angular/router';

export const routes: Routes = [
  { path: '', redirectTo: 'dashboard', pathMatch: 'full' },
  { path: 'dashboard', loadComponent: () => import('./pages/dashboard/dashboard.component').then(m => m.DashboardComponent) },
  { path: 'sessions', loadComponent: () => import('./pages/sessions/sessions.component').then(m => m.SessionsComponent) },
  { path: 'sessions/:date', loadComponent: () => import('./pages/session-detail/session-detail.component').then(m => m.SessionDetailComponent) },
  { path: 'setup', loadComponent: () => import('./pages/setup/setup.component').then(m => m.SetupComponent) },
  { path: 'settings', loadComponent: () => import('./pages/settings/settings.component').then(m => m.SettingsComponent) },
  { path: '**', redirectTo: 'dashboard' },
];
