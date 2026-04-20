import { Component, OnInit, OnDestroy } from '@angular/core';
import { CommonModule } from '@angular/common';
import { FormsModule } from '@angular/forms';
import { Subject, Subscription, timer } from 'rxjs';
import { switchMap, takeUntil, takeWhile, tap } from 'rxjs/operators';
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
          </div>
        </div>

        <!-- Section: O2 Ring Oximetry -->
        <div class="section">
          <div class="section-header" (click)="toggle('o2ring')">
            <span class="chevron" [class.open]="open['o2ring']">&#9654;</span>
            O2 Ring Oximetry
            <span class="badge" *ngIf="!config.o2ring?.enabled">optional</span>
          </div>
          <div class="section-body" *ngIf="open['o2ring']">
            <label class="toggle-row">
              Enabled
              <input type="checkbox" [(ngModel)]="config.o2ring.enabled" name="o2ring_enabled" />
            </label>
            <ng-container *ngIf="config.o2ring.enabled">
              <label>
                Mode
                <select [(ngModel)]="config.o2ring.mode" name="o2ring_mode">
                  <option value="http">HTTP (via Mule C3)</option>
                  <option value="ble">BLE Direct</option>
                </select>
              </label>
              <label *ngIf="config.o2ring.mode === 'http'">
                Mule URL
                <input type="text" [(ngModel)]="config.o2ring.mule_url" name="o2ring_mule_url"
                       placeholder="http://192.168.2.74" />
                <span class="hint">IP of the mule C3 bridging the O2 Ring via BLE</span>
              </label>
              <label *ngIf="config.o2ring.mode === 'ble'">
                <span class="hint" *ngIf="bleStatus === 'checking'">Checking Bluetooth adapter...</span>
                <span class="hint" *ngIf="bleStatus === 'ok'" style="color: #4ade80;">Bluetooth adapter detected. BLE direct mode ready.</span>
                <span class="hint" *ngIf="bleStatus === 'no_adapter'" style="color: #ef4444;">No Bluetooth adapter detected. Plug in a USB BLE adapter.</span>
                <span class="hint" *ngIf="bleStatus === 'not_compiled'" style="color: #fb923c;">BLE support not compiled. Rebuild with -DBUILD_WITH_BLE=ON.</span>
                <span class="hint" *ngIf="bleStatus === 'error'" style="color: #ef4444;">BlueZ error — check Bluetooth service.</span>
              </label>
            </ng-container>
          </div>
        </div>

        <!-- Section: Collection Timing -->
        <div class="section">
          <div class="section-header" (click)="toggle('timing')">
            <span class="chevron" [class.open]="open['timing']">&#9654;</span>
            Collection Timing
          </div>
          <div class="section-body" *ngIf="open['timing']">
            <label>
              Burst Interval (seconds)
              <input type="number" [(ngModel)]="config.burst_interval" name="burst_interval" min="1" />
            </label>
          </div>
        </div>

        <!-- Section: Import History -->
        <div class="section" *ngIf="config.local_dir">
          <div class="section-header" (click)="toggle('backfill')">
            <span class="chevron" [class.open]="open['backfill']">&#9654;</span>
            Import History
          </div>
          <div class="section-body" *ngIf="open['backfill']">
            <p class="section-desc">
              Import therapy sessions from your DATALOG directory into the database.
              Existing sessions in the date range will be re-parsed.
            </p>
            <div class="backfill-dates">
              <label>
                Start Date
                <input type="date" [(ngModel)]="backfillStart" name="backfill_start" />
              </label>
              <label>
                End Date
                <input type="date" [(ngModel)]="backfillEnd" name="backfill_end" />
              </label>
            </div>
            <span class="hint" *ngIf="backfillStart && backfillEnd">
              Leave empty to import all available data.
            </span>

            <!-- Backfill Status -->
            <div class="ml-status" *ngIf="backfillProgress">
              <div class="status-row">
                <span class="status-label">Status</span>
                <span class="status-value" [class.status-active]="backfillProgress.status === 'running'">
                  {{ backfillProgress.status }}
                </span>
              </div>
              <div class="status-row" *ngIf="backfillProgress.folders_total > 0">
                <span class="status-label">Folders</span>
                <span class="status-value">{{ backfillProgress.folders_done }} / {{ backfillProgress.folders_total }}</span>
              </div>
              <div class="status-row" *ngIf="backfillProgress.sessions_saved > 0 || backfillProgress.sessions_parsed > 0">
                <span class="status-label">Sessions</span>
                <span class="status-value">{{ backfillProgress.sessions_saved }} saved ({{ backfillProgress.sessions_parsed }} parsed)</span>
              </div>
              <div class="status-row" *ngIf="backfillProgress.sessions_deleted > 0">
                <span class="status-label">Replaced</span>
                <span class="status-value">{{ backfillProgress.sessions_deleted }} old session(s)</span>
              </div>
              <div class="status-row" *ngIf="backfillProgress.errors > 0">
                <span class="status-label">Errors</span>
                <span class="status-value" style="color: #ef5350;">{{ backfillProgress.errors }}</span>
              </div>
              <div class="status-row" *ngIf="backfillProgress.completed_at">
                <span class="status-label">Completed</span>
                <span class="status-value">{{ backfillProgress.completed_at }}</span>
              </div>
            </div>

            <!-- Progress bar -->
            <div class="progress-bar" *ngIf="backfillRunning && backfillProgress?.folders_total > 0">
              <div class="progress-fill"
                   [style.width.%]="(backfillProgress.folders_done / backfillProgress.folders_total) * 100">
              </div>
            </div>

            <div class="ml-actions">
              <button type="button" class="btn-train" (click)="startBackfill()" [disabled]="backfillRunning">
                {{ backfillRunning ? 'Importing...' : 'Import History' }}
              </button>
            </div>
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

        <!-- Section 5: ML Training -->
        <div class="section">
          <div class="section-header" (click)="toggle('ml_training')">
            <span class="chevron" [class.open]="open['ml_training']">&#9654;</span>
            ML Training
            <span class="badge" *ngIf="!config.ml_training.enabled">optional</span>
          </div>
          <div class="section-body" *ngIf="open['ml_training']">
            <label class="toggle-row">
              Enabled
              <input type="checkbox" [(ngModel)]="config.ml_training.enabled" name="ml_enabled" />
            </label>
            <ng-container *ngIf="config.ml_training.enabled">
              <label>
                Retrain Schedule
                <select [(ngModel)]="config.ml_training.schedule" name="ml_schedule">
                  <option value="daily">Daily</option>
                  <option value="weekly">Weekly</option>
                  <option value="monthly">Monthly</option>
                </select>
              </label>
              <label>
                Minimum Therapy Days
                <input type="number" [(ngModel)]="config.ml_training.min_days" name="ml_min_days" min="7" />
              </label>
              <label>
                Max Training Lookback (days)
                <input type="number" [(ngModel)]="config.ml_training.max_training_days" name="ml_max_days" min="0"
                       placeholder="0 = use all data" />
                <span class="hint">0 = train on all available data</span>
              </label>
              <label>
                Model Directory
                <input type="text" [(ngModel)]="config.ml_training.model_dir" name="ml_model_dir"
                       placeholder="~/.hms-cpap/models" />
              </label>

              <!-- ML Status -->
              <div class="ml-status" *ngIf="mlStatus">
                <div class="status-row">
                  <span class="status-label">Status</span>
                  <span class="status-value" [class.status-active]="mlStatus.status === 'training'">
                    {{ mlStatus.status }}
                  </span>
                </div>
                <div class="status-row">
                  <span class="status-label">Last Trained</span>
                  <span class="status-value">{{ mlStatus.last_trained || 'Never' }}</span>
                </div>
                <div class="status-row" *ngIf="mlStatus.models_loaded">
                  <span class="status-label">Models</span>
                  <span class="status-value">{{ mlStatus.model_count }} loaded</span>
                </div>
                <div class="model-metrics" *ngIf="mlStatus.models?.length">
                  <div class="metric-row" *ngFor="let m of mlStatus.models">
                    <span class="metric-name">{{ m.name }}</span>
                    <span class="metric-val">{{ m.primary_metric | number:'1.4-4' }}</span>
                  </div>
                </div>
              </div>

              <div class="ml-actions">
                <button type="button" class="btn-train" (click)="trainNow()" [disabled]="mlTraining">
                  {{ mlTraining ? 'Training...' : 'Train Now' }}
                </button>
              </div>
            </ng-container>
          </div>
        </div>

        <!-- Section 6: LLM Prompt -->
        <div class="section">
          <div class="section-header" (click)="toggle('llm_prompt')">
            <span class="chevron" [class.open]="open['llm_prompt']">&#9654;</span>
            LLM Prompt Template
          </div>
          <div class="section-body" *ngIf="open['llm_prompt']">
            <label>
              Prompt Text
              <textarea [(ngModel)]="llmPrompt" name="llm_prompt_text" rows="10"
                        placeholder="Loading..."></textarea>
            </label>
            <div class="prompt-path" *ngIf="llmPromptPath">File: {{ llmPromptPath }}</div>
            <div class="prompt-actions">
              <button type="button" class="btn-save-prompt" (click)="saveLlmPrompt()" [disabled]="savingPrompt">
                {{ savingPrompt ? 'Saving...' : 'Save Prompt' }}
              </button>
              <button type="button" class="btn-reset" (click)="resetLlmPrompt()">Reset to Default</button>
            </div>
          </div>
        </div>

        <!-- Section 7: Device -->
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

    /* ML Training */
    .ml-status { margin-top: 1rem; }
    .status-row {
      display: flex; justify-content: space-between; padding: 0.3rem 0;
      font-size: 0.85rem; border-bottom: 1px solid #2a2a3d;
    }
    .status-label { color: #888; }
    .status-value { color: #e0e0e0; }
    .status-active { color: #66bb6a; font-weight: 600; }
    .model-metrics { margin-top: 0.5rem; }
    .metric-row {
      display: flex; justify-content: space-between; padding: 0.2rem 0;
      font-size: 0.8rem; color: #aaa;
    }
    .metric-name { color: #90caf9; }
    .metric-val { font-family: monospace; }
    .ml-actions { margin-top: 0.75rem; }
    .btn-train {
      background: #7e57c2; color: #fff; border: none; border-radius: 6px;
      padding: 0.5rem 1.5rem; font-size: 0.9rem; font-weight: 600;
      cursor: pointer; transition: background 0.2s;
    }
    .btn-train:hover { background: #9575cd; }
    .btn-train:disabled { opacity: 0.5; cursor: not-allowed; }

    /* LLM Prompt */
    textarea {
      background: #15152a; border: 1px solid #444; border-radius: 4px;
      color: #e0e0e0; padding: 0.5rem 0.6rem; font-size: 0.85rem;
      font-family: monospace; width: 100%; resize: vertical;
      outline: none; transition: border-color 0.2s;
    }
    textarea:focus { border-color: #64b5f6; }
    .prompt-path { font-size: 0.75rem; color: #666; margin-top: 0.3rem; }
    .prompt-actions { margin-top: 0.75rem; display: flex; gap: 0.75rem; }
    .btn-save-prompt {
      background: #64b5f6; color: #111; border: none; border-radius: 6px;
      padding: 0.5rem 1.5rem; font-size: 0.9rem; font-weight: 600;
      cursor: pointer; transition: background 0.2s;
    }
    .btn-save-prompt:hover { background: #90caf9; }
    .btn-save-prompt:disabled { opacity: 0.5; cursor: not-allowed; }
    .btn-reset {
      background: transparent; color: #888; border: 1px solid #555; border-radius: 6px;
      padding: 0.5rem 1rem; font-size: 0.85rem; cursor: pointer;
      transition: all 0.2s;
    }
    .btn-reset:hover { color: #e0e0e0; border-color: #888; }
    .hint { font-size: 0.7rem; color: #666; font-style: italic; }

    /* Backfill / Import History */
    .section-desc {
      color: #999; font-size: 0.85rem; margin: 0.5rem 0 0.75rem;
      line-height: 1.4;
    }
    .backfill-dates {
      display: grid; grid-template-columns: 1fr 1fr; gap: 0.75rem;
    }
    .progress-bar {
      margin-top: 0.75rem; height: 6px; background: #2a2a3d;
      border-radius: 3px; overflow: hidden;
    }
    .progress-fill {
      height: 100%; background: #64b5f6; border-radius: 3px;
      transition: width 0.3s ease;
    }
  `]
})
export class SettingsComponent implements OnInit, OnDestroy {
  config: AppConfig | null = null;
  loading = true;
  saving = false;
  error = '';
  toast = '';
  toastError = false;

  // ML Training state
  mlStatus: any = null;
  mlTraining = false;

  // Backfill state
  backfillStart = '';
  backfillEnd = '';
  backfillRunning = false;
  backfillProgress: any = null;

  private destroy$ = new Subject<void>();

  // BLE adapter status
  bleStatus = '';

  // LLM Prompt state
  llmPrompt = '';
  llmPromptPath = '';
  savingPrompt = false;
  defaultPrompt = '';

  open: Record<string, boolean> = {
    source: true,
    o2ring: false,
    timing: false,
    backfill: false,
    database: true,
    mqtt: false,
    llm: false,
    ml_training: false,
    llm_prompt: false,
    device: true,
  };

  constructor(private api: CpapApiService) {}

  ngOnDestroy(): void {
    this.destroy$.next();
    this.destroy$.complete();
  }

  ngOnInit(): void {
    this.api.getConfig().subscribe({
      next: (cfg) => {
        // Ensure ml_training exists with defaults
        if (!cfg.ml_training) {
          cfg.ml_training = { enabled: false, schedule: 'weekly', model_dir: '', min_days: 30, max_training_days: 0 };
        }
        if (!cfg.o2ring) {
          cfg.o2ring = { enabled: false, mode: 'http', mule_url: '' };
        }
        this.config = cfg;
        this.loading = false;
        // Load ML status if enabled
        if (cfg.ml_training?.enabled) this.loadMlStatus();
        // Auto-detect backfill date range if local_dir is set
        if (cfg.local_dir) this.scanBackfillDates();
        // Check BLE adapter if BLE mode
        if (cfg.o2ring?.mode === 'ble') this.checkBleAdapter();
      },
      error: (err) => {
        this.error = 'Failed to load configuration.';
        this.loading = false;
      },
    });

    // Load LLM prompt
    this.api.getLlmPrompt().subscribe({
      next: (res) => {
        this.llmPrompt = res.prompt;
        this.llmPromptPath = res.path;
        this.defaultPrompt = res.prompt;
      },
      error: () => {},
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

  loadMlStatus(): void {
    this.api.getMlStatus().subscribe({
      next: (status) => this.mlStatus = status,
      error: () => {},
    });
  }

  trainNow(): void {
    this.mlTraining = true;
    this.api.triggerMlTraining().subscribe({
      next: () => {
        this.showToast('Training started.', false);
        // Poll status every 5s for up to 2 minutes using switchMap to prevent overlapping requests
        let polls = 0;
        timer(0, 5000).pipe(
          takeUntil(this.destroy$),
          switchMap(() => this.api.getMlStatus()),
          tap((status) => {
            this.mlStatus = status;
            polls++;
          }),
          takeWhile((status) => status?.status === 'training' && polls <= 24),
        ).subscribe({
          complete: () => {
            this.mlTraining = false;
            if (this.mlStatus?.status !== 'training') {
              this.showToast('Training complete.', false);
            }
          },
        });
      },
      error: () => {
        this.mlTraining = false;
        this.showToast('Failed to start training.', true);
      },
    });
  }

  scanBackfillDates(): void {
    this.api.scanBackfillDates().subscribe({
      next: (res) => {
        if (res.start_date) this.backfillStart = res.start_date;
        if (res.end_date) this.backfillEnd = res.end_date;
      },
      error: () => {},
    });
    // Also load any existing backfill progress
    this.api.getBackfillStatus().subscribe({
      next: (status) => {
        if (status.status !== 'idle' && status.status !== 'not_available') {
          this.backfillProgress = status;
          if (status.status === 'running') {
            this.backfillRunning = true;
            this.pollBackfillStatus();
          }
        }
      },
      error: () => {},
    });
  }

  startBackfill(): void {
    this.backfillRunning = true;
    this.backfillProgress = null;
    const start = this.backfillStart || undefined;
    const end = this.backfillEnd || undefined;
    this.api.triggerBackfill(start, end).subscribe({
      next: () => {
        this.showToast('Import started.', false);
        this.pollBackfillStatus();
      },
      error: () => {
        this.backfillRunning = false;
        this.showToast('Failed to start import.', true);
      },
    });
  }

  private pollBackfillStatus(): void {
    let polls = 0;
    timer(0, 3000).pipe(
      takeUntil(this.destroy$),
      switchMap(() => this.api.getBackfillStatus()),
      tap((status) => {
        this.backfillProgress = status;
        polls++;
      }),
      takeWhile((status) => status.status === 'running' && polls <= 200),
    ).subscribe({
      complete: () => {
        this.backfillRunning = false;
        const status = this.backfillProgress;
        if (status?.status === 'complete') {
          this.showToast(
            `Import complete: ${status.sessions_saved} session(s) saved.`, false);
        } else if (status?.status === 'error') {
          this.showToast('Import failed: ' + (status.error_message || 'unknown error'), true);
        }
      },
    });
  }

  saveLlmPrompt(): void {
    this.savingPrompt = true;
    this.api.updateLlmPrompt(this.llmPrompt).subscribe({
      next: () => {
        this.savingPrompt = false;
        this.showToast('Prompt saved.', false);
      },
      error: () => {
        this.savingPrompt = false;
        this.showToast('Failed to save prompt.', true);
      },
    });
  }

  resetLlmPrompt(): void {
    this.llmPrompt = this.defaultPrompt;
  }

  checkBleAdapter(): void {
    this.bleStatus = 'checking';
    this.api.testBle().subscribe({
      next: (res) => {
        if (!res.compiled) this.bleStatus = 'not_compiled';
        else if (res.available) this.bleStatus = 'ok';
        else this.bleStatus = 'no_adapter';
      },
      error: () => this.bleStatus = 'error',
    });
  }

  private showToast(msg: string, isError: boolean): void {
    this.toast = msg;
    this.toastError = isError;
    setTimeout(() => (this.toast = ''), 3000);
  }
}
