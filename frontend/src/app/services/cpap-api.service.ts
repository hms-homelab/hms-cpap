import { Injectable } from '@angular/core';
import { HttpClient, HttpParams } from '@angular/common/http';
import { Observable } from 'rxjs';
import { DashboardData, SessionListItem, SessionDetail, TrendPoint } from '../models/session.model';
import { AppConfig } from '../models/config.model';

@Injectable({ providedIn: 'root' })
export class CpapApiService {
  constructor(private http: HttpClient) {}

  getDashboard(): Observable<DashboardData> {
    return this.http.get<DashboardData>('/api/dashboard');
  }

  getSessions(days = 30, limit = 20): Observable<SessionListItem[]> {
    const params = new HttpParams().set('days', days).set('limit', limit);
    return this.http.get<SessionListItem[]>('/api/sessions', { params });
  }

  getSessionDetail(date: string): Observable<SessionDetail[]> {
    return this.http.get<SessionDetail[]>(`/api/sessions/${date}`);
  }

  getTrend(metric: string, days = 30): Observable<TrendPoint[]> {
    const params = new HttpParams().set('days', days);
    return this.http.get<TrendPoint[]>(`/api/trends/${metric}`, { params });
  }

  getConfig(): Observable<AppConfig> {
    return this.http.get<AppConfig>('/api/config');
  }

  updateConfig(partial: Partial<AppConfig>): Observable<AppConfig> {
    return this.http.put<AppConfig>('/api/config', partial);
  }

  completeSetup(): Observable<any> {
    return this.http.post('/api/setup', {});
  }

  testEzshare(url: string): Observable<{ status: string; url: string }> {
    return this.http.get<{ status: string; url: string }>(
      `/api/config/test-ezshare?url=${encodeURIComponent(url)}`
    );
  }
}
