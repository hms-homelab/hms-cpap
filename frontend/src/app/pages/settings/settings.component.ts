import { Component, OnInit } from '@angular/core';
import { CommonModule } from '@angular/common';
import { FormsModule } from '@angular/forms';
import { CpapApiService } from '../../services/cpap-api.service';
import { AppConfig } from '../../models/config.model';

@Component({
  selector: 'app-settings',
  standalone: true,
  imports: [CommonModule, FormsModule],
  template: `
    <div class="settings">
      <h2>Settings</h2>

      <div *ngIf="loading" class="loading">Loading configuration...</div>
      <div *ngIf="error" class="error">{{ error }}</div>

      <form *ngIf="config" (ngSubmit)="save()">

        <!-- Section 1: Data Source -->
        <div class="section">
          <div class="section-header" (click)="toggle('source')">
            <span class="chevron" [class.open]="open['source']">&#9654;</span>
            Data Source
          </div>
          <div class="section-body" *ngIf="open['source']">
            <label>
              Source Type
              <select [(ngModel)]="config.source" name="source">
                <option value="ezshare">ezShare WiFi SD</option>
                <option value="local">Local Directory</option>
                <option value="fysetc_poll">Fysetc Poll (HTTP)</option>
                <option value="fysetc">Fysetc (MQTT)</option>
              </select>
            </label>
            <label *ngIf="config.source === 'ezshare' || config.source === 'fysetc_poll'">
              {{ config.source === 'ezshare' ? 'ezShare URL' : 'Fysetc Device URL' }}
              <input type="text" [(ngModel)]="config.ezshare_url" name="ezshare_url"
                     [placeholder]="config.source === 'ezshare' ? 'http://192.168.4.1' : 'http://192.168.2.134'" />
            </label>
            <label *ngIf="config.source === 'local'">
              Local Directory
              <input type="text" [(ngModel)]="config.local_dir" name="local_dir" placeholder="/path/to/sd/DATALOG" />
            </label>
            <label>
              Burst Interval (seconds)
              <input type="number" [(ngModel)]="config.burst_interval" name="burst_interval" min="1" />
            </label>
          </div>
        </div>

        <!-- Section 2: Database -->
        <div class="section">
          <div class="section-header" (click)="toggle('database')">
            <span class="chevron" [class.open]="open['database']">&#9654;</span>
            Database
          </div>
          <div class="section-body" *ngIf="open['database']">
            <label>
              Type
              <select [(ngModel)]="config.database.type" name="db_type">
                <option value="sqlite">SQLite</option>
                <option value="mysql">MySQL</option>
                <option value="postgresql">PostgreSQL</option>
              </select>
            </label>
            <label *ngIf="config.database.type === 'sqlite'">
              SQLite Path
              <input type="text" [(ngModel)]="config.database.sqlite_path" name="db_sqlite_path" placeholder="/var/lib/hms-cpap/cpap.db" />
            </label>
            <ng-container *ngIf="config.database.type !== 'sqlite'">
              <label>
                Host
                <input type="text" [(ngModel)]="config.database.host" name="db_host" placeholder="localhost" />
              </label>
              <label>
                Port
                <input type="number" [(ngModel)]="config.database.port" name="db_port" />
              </label>
              <label>
                Database Name
                <input type="text" [(ngModel)]="config.database.name" name="db_name" />
              </label>
              <label>
                User
                <input type="text" [(ngModel)]="config.database.user" name="db_user" />
              </label>
              <label>
                Password
                <input type="password" [(ngModel)]="config.database.password" name="db_password" />
              </label>
            </ng-container>
          </div>
        </div>

        <!-- Section 3: MQTT -->
        <div class="section">
          <div class="section-header" (click)="toggle('mqtt')">
            <span class="chevron" [class.open]="open['mqtt']">&#9654;</span>
            MQTT
            <span class="badge" *ngIf="!config.mqtt.enabled">optional</span>
          </div>
          <div class="section-body" *ngIf="open['mqtt']">
            <label class="toggle-row">
              Enabled
              <input type="checkbox" [(ngModel)]="config.mqtt.enabled" name="mqtt_enabled" />
            </label>
            <ng-container *ngIf="config.mqtt.enabled">
              <label>
                Broker
                <input type="text" [(ngModel)]="config.mqtt.broker" name="mqtt_broker" placeholder="192.168.2.15" />
              </label>
              <label>
                Port
                <input type="number" [(ngModel)]="config.mqtt.port" name="mqtt_port" />
              </label>
              <label>
                Username
                <input type="text" [(ngModel)]="config.mqtt.username" name="mqtt_username" />
              </label>
              <label>
                Password
                <input type="password" [(ngModel)]="config.mqtt.password" name="mqtt_password" />
              </label>
            </ng-container>
          </div>
        </div>

        <!-- Section 4: LLM Summaries -->
        <div class="section">
          <div class="section-header" (click)="toggle('llm')">
            <span class="chevron" [class.open]="open['llm']">&#9654;</span>
            LLM Summaries
            <span class="badge" *ngIf="!config.llm.enabled">optional</span>
          </div>
          <div class="section-body" *ngIf="open['llm']">
            <label class="toggle-row">
              Enabled
              <input type="checkbox" [(ngModel)]="config.llm.enabled" name="llm_enabled" />
            </label>
            <ng-container *ngIf="config.llm.enabled">
              <label>
                Provider
                <select [(ngModel)]="config.llm.provider" name="llm_provider">
                  <option value="ollama">Ollama</option>
                  <option value="openai">OpenAI</option>
                  <option value="gemini">Gemini</option>
                  <option value="anthropic">Anthropic</option>
                </select>
              </label>
              <label>
                Endpoint
                <input type="text" [(ngModel)]="config.llm.endpoint" name="llm_endpoint" placeholder="http://192.168.2.5:11434" />
              </label>
              <label>
                Model
                <input type="text" [(ngModel)]="config.llm.model" name="llm_model" placeholder="llama3.1:8b" />
              </label>
              <label>
                API Key
                <input type="password" [(ngModel)]="config.llm.api_key" name="llm_api_key" placeholder="Optional for Ollama" />
              </label>
            </ng-container>
          </div>
        </div>

        <!-- Section 5: Device -->
        <div class="section">
          <div class="section-header" (click)="toggle('device')">
            <span class="chevron" [class.open]="open['device']">&#9654;</span>
            Device
          </div>
          <div class="section-body" *ngIf="open['device']">
            <label>
              Device ID
              <input type="text" [(ngModel)]="config.device_id" name="device_id" placeholder="23243570851" />
            </label>
            <label>
              Device Name
              <input type="text" [(ngModel)]="config.device_name" name="device_name" placeholder="ResMed AirSense 11" />
            </label>
          </div>
        </div>

        <div class="actions">
          <button type="submit" class="btn-save" [disabled]="saving">
            {{ saving ? 'Saving...' : 'Save' }}
          </button>
        </div>
      </form>

      <!-- Toast -->
      <div class="toast" *ngIf="toast" [class.toast-error]="toastError">{{ toast }}</div>
    </div>
  `,
  styles: [`
    .settings { padding: 1.5rem; max-width: 720px; }
    h2 { color: #e0e0e0; margin-bottom: 1.25rem; }

    .loading { color: #aaa; }
    .error { color: #ef5350; margin-bottom: 1rem; }

    .section {
      background: #1e1e2f; border: 1px solid #333; border-radius: 8px;
      margin-bottom: 0.75rem; overflow: hidden;
    }
    .section-header {
      display: flex; align-items: center; gap: 0.5rem;
      padding: 0.85rem 1rem; cursor: pointer;
      color: #e0e0e0; font-weight: 600; font-size: 0.95rem;
      user-select: none; transition: background 0.15s;
    }
    .section-header:hover { background: rgba(255,255,255,0.04); }

    .chevron {
      display: inline-block; font-size: 0.7rem; transition: transform 0.2s;
      color: #64b5f6;
    }
    .chevron.open { transform: rotate(90deg); }

    .badge {
      margin-left: auto; font-size: 0.7rem; font-weight: 400;
      color: #888; text-transform: uppercase; letter-spacing: 0.05em;
    }

    .section-body { padding: 0 1rem 1rem; }

    label {
      display: flex; flex-direction: column; gap: 0.3rem;
      color: #bbb; font-size: 0.85rem; margin-top: 0.75rem;
    }
    label.toggle-row {
      flex-direction: row; align-items: center; gap: 0.6rem;
    }
    label.toggle-row input[type="checkbox"] {
      width: 18px; height: 18px; accent-color: #64b5f6; cursor: pointer;
    }

    input, select {
      background: #15152a; border: 1px solid #444; border-radius: 4px;
      color: #e0e0e0; padding: 0.5rem 0.6rem; font-size: 0.9rem;
      outline: none; transition: border-color 0.2s;
    }
    input:focus, select:focus { border-color: #64b5f6; }
    select { cursor: pointer; }
    input[type="number"] { max-width: 160px; }

    .actions { margin-top: 1.25rem; display: flex; }
    .btn-save {
      background: #64b5f6; color: #111; border: none; border-radius: 6px;
      padding: 0.6rem 2rem; font-size: 0.95rem; font-weight: 600;
      cursor: pointer; transition: background 0.2s;
    }
    .btn-save:hover { background: #90caf9; }
    .btn-save:disabled { opacity: 0.5; cursor: not-allowed; }

    .toast {
      position: fixed; bottom: 1.5rem; right: 1.5rem;
      background: #2e7d32; color: #fff; padding: 0.75rem 1.25rem;
      border-radius: 6px; font-size: 0.9rem; z-index: 1000;
      animation: fadeIn 0.2s;
    }
    .toast-error { background: #c62828; }
    @keyframes fadeIn { from { opacity: 0; transform: translateY(8px); } to { opacity: 1; transform: translateY(0); } }
  `]
})
export class SettingsComponent implements OnInit {
  config: AppConfig | null = null;
  loading = true;
  saving = false;
  error = '';
  toast = '';
  toastError = false;

  open: Record<string, boolean> = {
    source: true,
    database: true,
    mqtt: false,
    llm: false,
    device: true,
  };

  constructor(private api: CpapApiService) {}

  ngOnInit(): void {
    this.api.getConfig().subscribe({
      next: (cfg) => {
        this.config = cfg;
        this.loading = false;
      },
      error: (err) => {
        this.error = 'Failed to load configuration.';
        this.loading = false;
      },
    });
  }

  toggle(section: string): void {
    this.open[section] = !this.open[section];
  }

  save(): void {
    if (!this.config || this.saving) return;
    this.saving = true;
    this.api.updateConfig(this.config).subscribe({
      next: () => {
        this.saving = false;
        this.showToast('Configuration saved.', false);
      },
      error: () => {
        this.saving = false;
        this.showToast('Save failed.', true);
      },
    });
  }

  private showToast(msg: string, isError: boolean): void {
    this.toast = msg;
    this.toastError = isError;
    setTimeout(() => (this.toast = ''), 3000);
  }
}
