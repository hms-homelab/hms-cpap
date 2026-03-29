import { Component } from '@angular/core';
import { Router } from '@angular/router';
import { FormsModule } from '@angular/forms';
import { CpapApiService } from '../../services/cpap-api.service';
import { switchMap } from 'rxjs';

@Component({
  selector: 'app-setup',
  standalone: true,
  imports: [FormsModule],
  template: `
    <div class="setup-container">
      <div class="setup-card">
        <div class="step-indicator">Step {{ currentStep }} of 3</div>

        @if (currentStep === 1) {
          <div class="step">
            <h1>Welcome to HMS-CPAP</h1>
            <p class="subtitle">
              This wizard will help you configure your CPAP data source.
              HMS-CPAP collects sleep therapy data from your ResMed device
              and presents it in a clear, actionable dashboard.
            </p>
            <p class="subtitle">
              You can change these settings later from the configuration page.
            </p>
            <div class="actions">
              <button class="btn-primary" (click)="currentStep = 2">Next</button>
            </div>
          </div>
        }

        @if (currentStep === 2) {
          <div class="step">
            <h1>Data Source</h1>
            <p class="subtitle">How should HMS-CPAP access your CPAP data?</p>

            <div class="radio-group">
              <label class="radio-option" [class.selected]="source === 'ezshare'">
                <input type="radio" name="source" value="ezshare"
                       [(ngModel)]="source" />
                <div class="radio-content">
                  <strong>ezShare WiFi SD</strong>
                  <span>Wirelessly pull data from an ezShare WiFi SD card in your CPAP machine.</span>
                </div>
              </label>

              <label class="radio-option" [class.selected]="source === 'local'">
                <input type="radio" name="source" value="local"
                       [(ngModel)]="source" />
                <div class="radio-content">
                  <strong>Local SD Card Path</strong>
                  <span>Read data from a mounted SD card or local directory.</span>
                </div>
              </label>

              <label class="radio-option" [class.selected]="source === 'skip'">
                <input type="radio" name="source" value="skip"
                       [(ngModel)]="source" />
                <div class="radio-content">
                  <strong>Skip (configure later)</strong>
                  <span>Skip data source setup and configure it manually later.</span>
                </div>
              </label>
            </div>

            @if (source === 'ezshare') {
              <div class="source-config">
                <label for="ezshare-url">ezShare URL</label>
                <div class="input-row">
                  <input id="ezshare-url" type="text"
                         [(ngModel)]="ezshareUrl"
                         placeholder="http://192.168.4.1" />
                  <button class="btn-secondary" (click)="testEzshare()"
                          [disabled]="testing">
                    {{ testing ? 'Testing...' : 'Test' }}
                  </button>
                </div>
                @if (testResult) {
                  <div class="test-result" [class.success]="testResult === 'ok'"
                       [class.error]="testResult !== 'ok'">
                    {{ testResult === 'ok' ? 'Connection successful' : 'Connection failed: ' + testResult }}
                  </div>
                }
              </div>
            }

            @if (source === 'local') {
              <div class="source-config">
                <label for="local-dir">Directory Path</label>
                <input id="local-dir" type="text"
                       [(ngModel)]="localDir"
                       placeholder="/media/sdcard/DATALOG" />
              </div>
            }

            <div class="actions">
              <button class="btn-ghost" (click)="currentStep = 1">Back</button>
              <button class="btn-primary" (click)="finish()"
                      [disabled]="saving">
                {{ saving ? 'Saving...' : 'Finish' }}
              </button>
            </div>

            @if (error) {
              <div class="test-result error">{{ error }}</div>
            }
          </div>
        }

        @if (currentStep === 3) {
          <div class="step">
            <h1>Setup Complete</h1>
            <p class="subtitle">Redirecting to dashboard...</p>
          </div>
        }
      </div>
    </div>
  `,
  styles: [`
    .setup-container {
      display: flex;
      justify-content: center;
      align-items: center;
      min-height: 100vh;
      padding: 2rem;
      background: #121212;
    }

    .setup-card {
      background: #1a1a2e;
      border: 1px solid #333;
      border-radius: 12px;
      padding: 2.5rem;
      max-width: 560px;
      width: 100%;
    }

    .step-indicator {
      font-size: 0.8rem;
      color: #888;
      text-transform: uppercase;
      letter-spacing: 0.05em;
      margin-bottom: 1.5rem;
    }

    h1 {
      color: #64b5f6;
      font-size: 1.6rem;
      margin: 0 0 0.75rem;
    }

    .subtitle {
      color: #aaa;
      line-height: 1.6;
      margin: 0 0 1.25rem;
    }

    .radio-group {
      display: flex;
      flex-direction: column;
      gap: 0.75rem;
      margin-bottom: 1.5rem;
    }

    .radio-option {
      display: flex;
      align-items: flex-start;
      gap: 0.75rem;
      padding: 1rem;
      border: 1px solid #333;
      border-radius: 8px;
      cursor: pointer;
      transition: all 0.2s;
    }

    .radio-option:hover {
      border-color: #555;
    }

    .radio-option.selected {
      border-color: #64b5f6;
      background: rgba(100, 181, 246, 0.06);
    }

    .radio-option input[type="radio"] {
      margin-top: 3px;
      accent-color: #64b5f6;
    }

    .radio-content {
      display: flex;
      flex-direction: column;
      gap: 0.25rem;
    }

    .radio-content strong {
      color: #e0e0e0;
    }

    .radio-content span {
      color: #888;
      font-size: 0.85rem;
    }

    .source-config {
      margin-bottom: 1.5rem;
    }

    .source-config label {
      display: block;
      color: #aaa;
      font-size: 0.85rem;
      margin-bottom: 0.4rem;
    }

    .input-row {
      display: flex;
      gap: 0.5rem;
    }

    input[type="text"] {
      flex: 1;
      background: #0d0d1a;
      border: 1px solid #333;
      border-radius: 6px;
      color: #e0e0e0;
      padding: 0.6rem 0.8rem;
      font-size: 0.95rem;
    }

    input[type="text"]:focus {
      outline: none;
      border-color: #64b5f6;
    }

    .test-result {
      margin-top: 0.5rem;
      padding: 0.5rem 0.75rem;
      border-radius: 6px;
      font-size: 0.85rem;
    }

    .test-result.success {
      color: #81c784;
      background: rgba(129, 199, 132, 0.1);
    }

    .test-result.error {
      color: #e57373;
      background: rgba(229, 115, 115, 0.1);
    }

    .actions {
      display: flex;
      justify-content: flex-end;
      gap: 0.75rem;
      margin-top: 1.5rem;
    }

    .btn-primary {
      background: #64b5f6;
      color: #121212;
      border: none;
      border-radius: 6px;
      padding: 0.6rem 1.5rem;
      font-size: 0.95rem;
      font-weight: 600;
      cursor: pointer;
      transition: background 0.2s;
    }

    .btn-primary:hover:not(:disabled) {
      background: #90caf9;
    }

    .btn-primary:disabled {
      opacity: 0.5;
      cursor: not-allowed;
    }

    .btn-secondary {
      background: transparent;
      color: #64b5f6;
      border: 1px solid #64b5f6;
      border-radius: 6px;
      padding: 0.6rem 1rem;
      font-size: 0.95rem;
      cursor: pointer;
      transition: all 0.2s;
    }

    .btn-secondary:hover:not(:disabled) {
      background: rgba(100, 181, 246, 0.1);
    }

    .btn-secondary:disabled {
      opacity: 0.5;
      cursor: not-allowed;
    }

    .btn-ghost {
      background: transparent;
      color: #aaa;
      border: none;
      padding: 0.6rem 1rem;
      font-size: 0.95rem;
      cursor: pointer;
      transition: color 0.2s;
    }

    .btn-ghost:hover {
      color: #e0e0e0;
    }
  `]
})
export class SetupComponent {
  currentStep = 1;
  source = 'ezshare';
  ezshareUrl = 'http://192.168.4.1';
  localDir = '';
  testing = false;
  saving = false;
  testResult: string | null = null;
  error: string | null = null;

  constructor(private api: CpapApiService, private router: Router) {}

  testEzshare(): void {
    this.testing = true;
    this.testResult = null;
    this.api.testEzshare(this.ezshareUrl).subscribe({
      next: (res) => {
        this.testResult = res.status;
        this.testing = false;
      },
      error: () => {
        this.testResult = 'unreachable';
        this.testing = false;
      }
    });
  }

  finish(): void {
    this.saving = true;
    this.error = null;

    const configUpdate: Record<string, string> = {};

    if (this.source === 'ezshare') {
      configUpdate['source'] = 'ezshare';
      configUpdate['ezshare_url'] = this.ezshareUrl;
    } else if (this.source === 'local') {
      configUpdate['source'] = 'local';
      configUpdate['local_dir'] = this.localDir;
    }
    // skip: no config update needed

    this.api.updateConfig(configUpdate).pipe(
      switchMap(() => this.api.completeSetup())
    ).subscribe({
      next: () => {
        this.currentStep = 3;
        this.saving = false;
        setTimeout(() => this.router.navigate(['/dashboard']), 1500);
      },
      error: (err) => {
        this.error = 'Failed to save configuration. Please try again.';
        this.saving = false;
        console.error('Setup error:', err);
      }
    });
  }
}
