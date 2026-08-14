import { Routes } from '@angular/router';

import { setupGuard } from './guards/setup.guard';

export const routes: Routes = [
  { path: '', redirectTo: 'dashboard', pathMatch: 'full' },
  { path: 'dashboard', loadComponent: () => import('./pages/dashboard/dashboard.component').then(m => m.DashboardComponent) , canActivate: [setupGuard] },
  { path: 'sessions', loadComponent: () => import('./pages/sessions/sessions.component').then(m => m.SessionsComponent) , canActivate: [setupGuard] },
  { path: 'sessions/:date', loadComponent: () => import('./pages/session-detail/session-detail.component').then(m => m.SessionDetailComponent) , canActivate: [setupGuard] },
  { path: 'compare/:a/:b', loadComponent: () => import('./pages/compare/compare.component').then(m => m.CompareComponent) , canActivate: [setupGuard] },
  { path: 'events', loadComponent: () => import('./pages/events/events.component').then(m => m.EventsComponent) , canActivate: [setupGuard] },
  { path: 'reports', loadComponent: () => import('./pages/reports/reports.component').then(m => m.ReportsComponent) , canActivate: [setupGuard] },
  { path: 'equipment', loadComponent: () => import('./pages/equipment/equipment.component').then(m => m.EquipmentComponent) , canActivate: [setupGuard] },
  { path: 'upload', loadComponent: () => import('./pages/upload/upload.component').then(m => m.UploadComponent) , canActivate: [setupGuard] },
  { path: 'setup', loadComponent: () => import('./pages/setup/setup.component').then(m => m.SetupComponent) },
  { path: 'logs', loadComponent: () => import('./pages/logs/logs.component').then(m => m.LogsComponent) , canActivate: [setupGuard] },
  { path: 'settings', loadComponent: () => import('./pages/settings/settings.component').then(m => m.SettingsComponent) , canActivate: [setupGuard] },
  { path: '**', redirectTo: 'dashboard' },
];
