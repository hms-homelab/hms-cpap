import { Routes } from '@angular/router';

export const routes: Routes = [
  { path: '', redirectTo: 'dashboard', pathMatch: 'full' },
  { path: 'dashboard', loadComponent: () => import('./pages/dashboard/dashboard.component').then(m => m.DashboardComponent) },
  { path: 'sessions', loadComponent: () => import('./pages/sessions/sessions.component').then(m => m.SessionsComponent) },
  { path: 'sessions/:date', loadComponent: () => import('./pages/session-detail/session-detail.component').then(m => m.SessionDetailComponent) },
  { path: 'compare/:a/:b', loadComponent: () => import('./pages/compare/compare.component').then(m => m.CompareComponent) },
  { path: 'reports', loadComponent: () => import('./pages/reports/reports.component').then(m => m.ReportsComponent) },
  { path: 'equipment', loadComponent: () => import('./pages/equipment/equipment.component').then(m => m.EquipmentComponent) },
  { path: 'upload', loadComponent: () => import('./pages/upload/upload.component').then(m => m.UploadComponent) },
  { path: 'setup', loadComponent: () => import('./pages/setup/setup.component').then(m => m.SetupComponent) },
  { path: 'settings', loadComponent: () => import('./pages/settings/settings.component').then(m => m.SettingsComponent) },
  { path: '**', redirectTo: 'dashboard' },
];
