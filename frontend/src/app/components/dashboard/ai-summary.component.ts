import { Component, Input } from '@angular/core';
import { CommonModule } from '@angular/common';

@Component({
  selector: 'app-ai-summary',
  standalone: true,
  imports: [CommonModule],
  template: `
    <div class="section-card">
      <div class="section-header">
        <div class="section-title">AI Session Summary</div>
        <div class="section-subtitle">LLM-generated analysis of last session</div>
      </div>
      <div class="summary-body" [innerHTML]="renderedHtml"></div>
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
    .summary-body {
      color: #ccc; font-size: 0.85rem; line-height: 1.6;
      max-height: 400px; overflow-y: auto;
    }
    :host ::ng-deep .summary-body strong { color: #e0e0e0; }
    :host ::ng-deep .summary-body ul { padding-left: 1.2rem; margin: 0.5rem 0; }
    :host ::ng-deep .summary-body li { margin-bottom: 0.4rem; }
    :host ::ng-deep .summary-body p { margin: 0.5rem 0; }
  `]
})
export class AiSummaryComponent {
  @Input() summaryText = '';

  get renderedHtml(): string {
    if (!this.summaryText) return '<em>No summary available yet.</em>';
    let html = this.summaryText;
    html = html.replace(/\*\*(.+?)\*\*/g, '<strong>$1</strong>');
    html = html.replace(/^(\* .+)$/gm, (_, content) => `<li>${content.substring(2)}</li>`);
    html = html.replace(/(<li>.*<\/li>\n?)+/g, (match) => `<ul>${match}</ul>`);
    html = html.replace(/\n\n/g, '</p><p>');
    html = html.replace(/\n/g, '<br>');
    if (!html.startsWith('<')) html = '<p>' + html + '</p>';
    return html;
  }
}
