import { Component, Input } from '@angular/core';
import { CommonModule } from '@angular/common';

@Component({
  selector: 'app-therapy-insights',
  standalone: true,
  imports: [CommonModule],
  template: `
    <div class="section-card">
      <div class="section-header">
        <div class="section-title">Therapy Insights</div>
        <div class="section-subtitle">{{ insights.length }} automated observations</div>
      </div>
      <div class="insights-list">
        <div class="insight-item" *ngFor="let i of insights">
          <span class="insight-icon">{{ categoryIcon(i.category) }}</span>
          <div class="insight-content">
            <div class="insight-title">{{ i.title }}</div>
            <div class="insight-body">{{ i.body }}</div>
          </div>
        </div>
      </div>
    </div>
  `,
  styles: [`
    .section-card {
      background: #1e1e2f; border: 1px solid #333; border-radius: 12px;
      padding: 1.25rem; height: 100%; box-sizing: border-box;
    }
    .section-header { margin-bottom: 0.75rem; }
    .section-title { color: #e0e0e0; font-size: 1rem; font-weight: 600; }
    .section-subtitle { color: #888; font-size: 0.8rem; }
    .insights-list { max-height: 400px; overflow-y: auto; }
    .insight-item {
      display: flex; gap: 0.6rem; padding: 0.6rem 0;
      border-bottom: 1px solid #2a2a3d;
    }
    .insight-item:last-child { border-bottom: none; }
    .insight-icon { font-size: 1.1rem; flex-shrink: 0; line-height: 1.4; }
    .insight-content { min-width: 0; }
    .insight-title { color: #e0e0e0; font-size: 0.85rem; font-weight: 600; }
    .insight-body { color: #aaa; font-size: 0.8rem; line-height: 1.5; margin-top: 0.15rem; }
  `]
})
export class TherapyInsightsComponent {
  @Input() insights: any[] = [];

  categoryIcon(category: string): string {
    switch (category) {
      case 'positive': return '✅';
      case 'warning': return '⚠️';
      case 'alert': return '🔴';
      case 'actionable': return '💡';
      default: return 'ℹ️';
    }
  }
}
