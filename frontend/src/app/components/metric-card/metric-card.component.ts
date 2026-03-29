import { Component, Input } from '@angular/core';

@Component({
  selector: 'app-metric-card',
  standalone: true,
  template: `
    <div class="metric-card">
      <div class="label">{{ label }}</div>
      <div class="value">{{ value }}<span class="unit">{{ unit }}</span></div>
    </div>
  `,
  styles: [`
    .metric-card {
      background: #1e1e2f; border: 1px solid #333; border-radius: 8px;
      padding: 1.2rem; min-width: 160px;
    }
    .label { font-size: 0.8rem; color: #888; margin-bottom: 0.4rem; text-transform: uppercase; letter-spacing: 0.5px; }
    .value { font-size: 1.8rem; font-weight: 700; color: #e0e0e0; }
    .unit { font-size: 0.9rem; font-weight: 400; color: #888; margin-left: 4px; }
  `]
})
export class MetricCardComponent {
  @Input() label = '';
  @Input() value = '';
  @Input() unit = '';
}
