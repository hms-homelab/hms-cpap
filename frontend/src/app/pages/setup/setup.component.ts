import { Component } from '@angular/core';
import { Router } from '@angular/router';
import { FormsModule } from '@angular/forms';
import { CpapApiService } from '../../services/cpap-api.service';
import { DiscoveredDevice } from '../../models/config.model';
import { markSetupComplete } from '../../guards/setup.guard';
import { switchMap, tap } from 'rxjs';

@Component({
  selector: 'app-setup',
  standalone: true,
  imports: [FormsModule],
  template: `
    <div class="setup-container">
      <div class="setup-card">
        <div class="step-indicator">Step {{ currentStep }} of 5</div>

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
            <h1>Where should your data live?</h1>
            <p class="subtitle">
              Everything stays on this machine either way. This only chooses what
              stores it.
            </p>

            <div class="radio-group">
              <label class="radio-option" [class.selected]="dbType === 'sqlite'">
                <input type="radio" name="dbtype" value="sqlite" [(ngModel)]="dbType" />
                <div class="radio-content">
                  <strong>Built in (recommended)</strong>
                  <span>A single file in your data folder. Nothing to install or run.</span>
                </div>
              </label>

              <label class="radio-option" [class.selected]="dbType === 'advanced'">
                <input type="radio" name="dbtype" value="advanced" [(ngModel)]="dbType" />
                <div class="radio-content">
                  <strong>Use my own database server</strong>
                  <span>PostgreSQL or MySQL you already run.</span>
                </div>
              </label>
            </div>

            @if (dbType === 'advanced') {
              <div class="advanced">
                <label class="fld">
                  <span>Engine</span>
                  <select [(ngModel)]="dbEngine" name="engine">
                    @for (b of serverBackends; track b) {
                      <option [value]="b">{{ b === 'postgresql' ? 'PostgreSQL' : 'MySQL' }}</option>
                    }
                  </select>
                </label>
                @if (serverBackends.length === 0) {
                  <!-- Capability honesty: this build genuinely cannot open either,
                       and offering them would write a config that refuses to boot. -->
                  <div class="test-result error">
                    This build was compiled without PostgreSQL and MySQL support.
                    Use the built in option.
                  </div>
                }
                <div class="row">
                  <label class="fld"><span>Host</span>
                    <input [(ngModel)]="dbHost" name="dbhost" placeholder="localhost"></label>
                  <label class="fld port"><span>Port</span>
                    <input type="number" [(ngModel)]="dbPort" name="dbport"></label>
                </div>
                <label class="fld"><span>Database name</span>
                  <input [(ngModel)]="dbName" name="dbname" placeholder="cpap"></label>
                <label class="fld"><span>User</span>
                  <input [(ngModel)]="dbUser" name="dbuser" placeholder="cpap_user"></label>
                <label class="fld"><span>Password</span>
                  <input type="password" [(ngModel)]="dbPassword" name="dbpass"></label>

                <label class="radio-option check" [class.selected]="createIt">
                  <input type="checkbox" [(ngModel)]="createIt" name="createit" />
                  <div class="radio-content">
                    <strong>It does not exist yet, create it for me</strong>
                    <span>Needs an administrator login. It is used once, for this
                          request only, and is never saved.</span>
                  </div>
                </label>

                @if (createIt) {
                  <div class="row">
                    <label class="fld"><span>Admin user</span>
                      <input [(ngModel)]="adminUser" name="adminuser"></label>
                    <label class="fld"><span>Admin password</span>
                      <input type="password" [(ngModel)]="adminPassword" name="adminpass"></label>
                  </div>
                }
              </div>
            }

            <div class="actions">
              <button class="btn-ghost" (click)="currentStep = 1">Back</button>
              @if (dbType === 'advanced') {
                <button class="btn-ghost" (click)="checkDb()" [disabled]="dbBusy">
                  {{ dbBusy ? 'Checking...' : (createIt ? 'Create and check' : 'Check connection') }}
                </button>
              }
              <button class="btn-primary" (click)="currentStep = 3"
                      [disabled]="dbType === 'advanced' && !dbVerified">Next</button>
            </div>

            @if (dbMessage) {
              <div class="test-result" [class.error]="!dbVerified">{{ dbMessage }}</div>
            }
          </div>
        }

        @if (currentStep === 3) {
          <div class="step">
            <h1>Data Source</h1>
            <p class="subtitle">How should HMS-CPAP access your CPAP data?</p>

            <div class="radio-group">
              <label class="radio-option" [class.selected]="source === 'mm'">
                <input type="radio" name="source" value="mm"
                       [(ngModel)]="source" />
                <div class="radio-content">
                  <strong>CpapDash Mule and Miner</strong>
                  <span>Find your CpapDash bridge on this network automatically.</span>
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

              <label class="radio-option" [class.selected]="source === 'ezshare'">
                <input type="radio" name="source" value="ezshare"
                       [(ngModel)]="source" />
                <div class="radio-content">
                  <strong>ezShare WiFi SD (advanced)</strong>
                  <span>Pull directly from an ezShare WiFi SD card in your CPAP machine.</span>
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

            @if (source === 'mm') {
              <div class="source-config">
                <div class="input-row">
                  <button class="btn-secondary" (click)="scan()" [disabled]="scanning">
                    {{ scanning ? 'Scanning...' : 'Scan network' }}
                  </button>
                </div>

                @if (devices.length > 0) {
                  <div class="device-list">
                    @for (d of devices; track d.instance) {
                      <label class="device"
                             [class.selected]="selectedDevice?.instance === d.instance"
                             [class.disabled]="!d.local_capable">
                        <input type="radio" name="device"
                               [value]="d.instance"
                               [checked]="selectedDevice?.instance === d.instance"
                               [disabled]="!d.local_capable"
                               (change)="selectDevice(d)" />
                        <div class="device-info">
                          <strong>{{ d.serial || d.instance }}</strong>
                          <span>
                            {{ d.host }}@if (d.fw) { &middot; fw {{ d.fw }} }@if (!d.local_capable) { &middot; cloud mode, cannot sync locally }
                          </span>
                        </div>
                      </label>
                    }
                  </div>
                }

                @if (scanned && devices.length === 0) {
                  <div class="test-result error">
                    No CpapDash units answered. Check that the bridge is powered on and
                    joined to this same network, then scan again. Some routers block the
                    discovery traffic; you can enter the address by hand with the
                    advanced option below.
                  </div>
                }
              </div>
            }

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

            @if (source === 'mm') {
              <div class="advanced">
                <label class="fld">
                  <span>Folder for reconstructed files (required)</span>
                  <input [(ngModel)]="archiveDir" name="archivedir"
                         placeholder="/Users/you/CpapDash" />
                </label>
                <p class="hint">
                  The Mule and Miner hands over the raw card files. They have to
                  land somewhere before anything can read them, so this folder is
                  where your night-by-night data actually lives.
                </p>
              </div>
            }

            <div class="actions">
              <button class="btn-ghost" (click)="currentStep = 2">Back</button>
              <button class="btn-primary" (click)="currentStep = 4"
                      [disabled]="source === 'mm' && !archiveDir">Next</button>
            </div>

            @if (error) {
              <div class="test-result error">{{ error }}</div>
            }
          </div>
        }

        @if (currentStep === 4) {
          <div class="step">
            <h1>Anything else?</h1>
            <p class="subtitle">
              All optional, and all changeable later from Settings. Skip straight
              to Finish if none of it applies.
            </p>

            <label class="radio-option check" [class.selected]="mqttEnabled">
              <input type="checkbox" [(ngModel)]="mqttEnabled" name="mqtton" />
              <div class="radio-content">
                <strong>Publish to Home Assistant (MQTT)</strong>
                <span>Sends nightly metrics to your broker as discovered sensors.</span>
              </div>
            </label>
            @if (mqttEnabled) {
              <div class="advanced">
                <div class="row">
                  <label class="fld"><span>Broker host</span>
                    <input [(ngModel)]="mqttBroker" name="mqttbroker" placeholder="192.168.1.10"></label>
                  <label class="fld port"><span>Port</span>
                    <input type="number" [(ngModel)]="mqttPort" name="mqttport"></label>
                </div>
                <div class="row">
                  <label class="fld"><span>Username</span>
                    <input [(ngModel)]="mqttUser" name="mqttuser"></label>
                  <label class="fld"><span>Password</span>
                    <input type="password" [(ngModel)]="mqttPassword" name="mqttpw"></label>
                </div>
              </div>
            }

            <label class="radio-option check" [class.selected]="llmEnabled">
              <input type="checkbox" [(ngModel)]="llmEnabled" name="llmon" />
              <div class="radio-content">
                <strong>Write plain-language night summaries (LLM)</strong>
                <span>Uses a local Ollama by default, so nothing leaves the machine.</span>
              </div>
            </label>
            @if (llmEnabled) {
              <div class="advanced">
                <label class="fld"><span>Endpoint</span>
                  <input [(ngModel)]="llmEndpoint" name="llmep" placeholder="http://localhost:11434"></label>
                <label class="fld"><span>Model</span>
                  <input [(ngModel)]="llmModel" name="llmmodel" placeholder="llama3.1"></label>
              </div>
            }

            <label class="radio-option check" [class.selected]="mlEnabled">
              <input type="checkbox" [(ngModel)]="mlEnabled" name="mlon" />
              <div class="radio-content">
                <strong>Machine-learning insights</strong>
                <span>Trains on your own history, on this machine. Needs about a
                      month of nights before it says anything useful.</span>
              </div>
            </label>

            <label class="radio-option check" [class.selected]="cloudEnabled">
              <input type="checkbox" [(ngModel)]="cloudEnabled" name="cloudon" />
              <div class="radio-content">
                <strong>Mirror equipment and cleaning to CpapDash</strong>
                <span>Off by default. Your therapy data stays here either way;
                      this only mirrors the upkeep lists so the phone app can
                      show them.</span>
              </div>
            </label>
            @if (cloudEnabled) {
              <div class="advanced">
                <label class="fld"><span>API URL</span>
                  <input [(ngModel)]="cloudApiUrl" name="cloudurl"></label>
                <label class="fld"><span>Token</span>
                  <input type="password" [(ngModel)]="cloudToken" name="cloudtoken"
                         placeholder="pasted from your CpapDash account"></label>
                <label class="radio-option check" [class.selected]="cloudAutoSync">
                  <input type="checkbox" [(ngModel)]="cloudAutoSync" name="cloudauto" />
                  <div class="radio-content">
                    <strong>Keep it in sync automatically</strong>
                    <span>Otherwise it mirrors only when you press Sync.</span>
                  </div>
                </label>
              </div>
            }

            @if (autostartSupported && autostart && autostartCanManage) {
              <div class="advanced">
                <label class="fld">
                  <span>When should it start?</span>
                  <select [(ngModel)]="autostartScope" name="asscope">
                    <option value="login">When I log in (no admin password)</option>
                    <option value="boot">At boot, before anyone logs in (needs admin)</option>
                  </select>
                </label>
                @if (autostartScope === 'boot') {
                  <p class="hint">
                    A boot service runs with no user session, so it is pinned to
                    your account and your data folder explicitly. Finishing here
                    writes the service file and shows you one command to run with
                    administrator rights; nothing elevates itself behind your back.
                  </p>
                }
              </div>
            }

            @if (autostartSupported) {
              <label class="radio-option check" [class.selected]="autostart"
                     [class.disabled]="!autostartCanManage">
                <input type="checkbox" [(ngModel)]="autostart" name="autostart"
                       [disabled]="!autostartCanManage" />
                <div class="radio-content">
                  <strong>Start when I log in</strong>
                  <span>
                    @if (autostartCanManage) {
                      Keeps collecting after a reboot. Starts at login, not at
                      boot, so it runs once you sign in.
                    } @else {
                      Managed by CpapDash Desktop.
                    }
                  </span>
                </div>
              </label>
            }

            <div class="actions">
              <button class="btn-ghost" (click)="currentStep = 3">Back</button>
              <button class="btn-primary" (click)="finish()" [disabled]="saving">
                {{ saving ? 'Saving...' : 'Finish' }}
              </button>
            </div>

            @if (error) {
              <div class="test-result error">{{ error }}</div>
            }
          </div>
        }

        @if (currentStep === 5) {
          <div class="step">
            <h1>Setup Complete</h1>
            <p class="subtitle">{{ applyMessage || 'Redirecting to dashboard...' }}</p>
            @if (bootCommand) {
              <div class="advanced">
                <p class="hint">One last step, to start it before login. Run this
                   once in a terminal:</p>
                <pre class="cmd">{{ bootCommand }}</pre>
              </div>
            }
          </div>
        }
      </div>
    </div>
  `,
  styles: [`
    .advanced { margin: 0.75rem 0 0.25rem; padding-left: 0.25rem; }
    .fld { display: block; margin: 0.6rem 0; }
    .fld > span { display: block; font-size: 0.78rem; color: #9e9e9e; margin-bottom: 0.2rem; }
    .fld input, .fld select {
      width: 100%; padding: 0.5rem; background: #121212; color: #e0e0e0;
      border: 1px solid #333; border-radius: 6px;
    }
    .row { display: flex; gap: 0.75rem; }
    .row .fld { flex: 1; }
    .row .fld.port { max-width: 7rem; }
    .radio-option.check { margin-top: 1rem; }
    .radio-option.disabled { opacity: 0.6; }
    .cmd {
      background: #121212; border: 1px solid #333; border-radius: 6px;
      padding: 0.6rem; font-size: 0.72rem; color: #a5d6a7;
      white-space: pre-wrap; word-break: break-all; user-select: all;
    }
    .hint { font-size: 0.75rem; color: #9e9e9e; margin: 0.4rem 0 0; line-height: 1.45; }
    .radio-option.check input[type="checkbox"] { margin-top: 3px; accent-color: #64b5f6; }

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

    .device-list {
      display: flex;
      flex-direction: column;
      gap: 0.5rem;
      margin-top: 0.75rem;
    }

    .device {
      display: flex;
      align-items: flex-start;
      gap: 0.75rem;
      padding: 0.75rem;
      border: 1px solid #333;
      border-radius: 8px;
      cursor: pointer;
      transition: all 0.2s;
    }

    .device:hover {
      border-color: #555;
    }

    .device.selected {
      border-color: #64b5f6;
      background: rgba(100, 181, 246, 0.06);
    }

    /* A cloud-mode unit is shown rather than hidden: seeing it greyed out with
       a reason beats "we found nothing" when the bridge is sitting right
       there and simply configured the other way. */
    .device.disabled {
      opacity: 0.5;
      cursor: not-allowed;
    }

    .device input[type="radio"] {
      margin-top: 3px;
      accent-color: #64b5f6;
    }

    .device-info {
      display: flex;
      flex-direction: column;
      gap: 0.2rem;
    }

    .device-info strong {
      color: #e0e0e0;
    }

    .device-info span {
      color: #888;
      font-size: 0.85rem;
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
  // SDD-005: the Mule and Miner is the default path now. It resolves to
  // source=ezshare underneath, because a proxy-mode mule serves the same
  // /dir and /download an ezShare card does; 'mm' only ever names the way the
  // user got there.
  source = 'mm';
  ezshareUrl = 'http://192.168.4.1';
  localDir = '';
  testing = false;
  saving = false;
  testResult: string | null = null;
  error: string | null = null;

  // SDD-006 phase 2: database step.
  dbType = 'sqlite';          // 'sqlite' | 'advanced'
  dbEngine = 'postgresql';
  dbHost = 'localhost';
  dbPort = 5432;
  dbName = 'cpap';
  dbUser = 'cpap_user';
  dbPassword = '';
  createIt = false;
  adminUser = '';
  adminPassword = '';
  dbBusy = false;
  dbVerified = false;
  dbMessage: string | null = null;
  /// Only the server backends this build actually has. Offering one it lacks
  /// would write a config that stops the service from starting, which since
  /// 4.6.3 is a refuse-to-boot condition rather than a silent SQLite fallback.
  serverBackends: string[] = [];

  // SDD-006 phase 3: the folder the Mule and Miner reconstructs files into.
  // REQUIRED for that source, because the bridge hands over raw files that have
  // to land somewhere before anything can parse them.
  archiveDir = '';

  // SDD-006 phase 3: advanced options. All default OFF, so the private local
  // path stays the path of least resistance.
  mqttEnabled = false;
  mqttBroker = '';
  mqttPort = 1883;
  mqttUser = '';
  mqttPassword = '';

  llmEnabled = false;
  llmEndpoint = 'http://localhost:11434';
  llmModel = 'llama3.1';

  mlEnabled = false;

  cloudEnabled = false;
  cloudApiUrl = 'https://api.cpapdash.com';
  cloudToken = '';
  cloudAutoSync = false;

  autostart = false;
  autostartScope: 'login' | 'boot' = 'login';
  autostartSupported = false;
  autostartCanManage = false;

  bootCommand: string | null = null;
  applyMessage: string | null = null;

  scanning = false;
  scanned = false;
  devices: DiscoveredDevice[] = [];
  selectedDevice: DiscoveredDevice | null = null;

  constructor(private api: CpapApiService, private router: Router) {
    this.api.getCapabilities().subscribe({
      next: (caps: any) => {
        this.serverBackends = (caps?.backends || [])
          .filter((b: string) => b === 'postgresql' || b === 'mysql');
        if (this.serverBackends.length > 0 &&
            !this.serverBackends.includes(this.dbEngine)) {
          this.dbEngine = this.serverBackends[0];
        }
        this.syncDefaultPort();
      },
      // A build that cannot answer still gets a working wizard on the built in
      // option, which is the path that needs no capabilities at all.
      error: () => { this.serverBackends = []; }
    });

    this.api.getAutostart().subscribe({
      next: (a: any) => {
        this.autostartSupported = !!a?.supported;
        this.autostartCanManage = !!a?.can_manage;
        this.autostart = !!a?.installed;
      },
      error: () => { this.autostartSupported = false; }
    });
  }

  syncDefaultPort(): void {
    this.dbPort = this.dbEngine === 'mysql' ? 3306 : 5432;
  }

  private dbPayload(): any {
    return {
      type: this.dbEngine,
      host: this.dbHost,
      port: +this.dbPort,
      name: this.dbName,
      user: this.dbUser,
      password: this.dbPassword,
      ...(this.createIt
        ? { admin_user: this.adminUser, admin_password: this.adminPassword }
        : {})
    };
  }

  checkDb(): void {
    this.dbBusy = true;
    this.dbVerified = false;
    this.dbMessage = null;

    const call = this.createIt
      ? this.api.createDatabase(this.dbPayload())
      : this.api.testDatabase(this.dbPayload());

    call.subscribe({
      next: (r) => {
        this.dbBusy = false;
        this.dbVerified = !!r.ok;
        if (!r.ok) {
          this.dbMessage = r.error || 'Could not connect.';
          return;
        }
        // Saying how much is already there is the point: it lets someone tell
        // "my database" from "someone else's data I am about to merge into".
        this.dbMessage = r.schema_present
          ? (r.session_count > 0
              ? `Connected. This database already holds ${r.session_count} session(s); they will be reused.`
              : 'Connected. Existing empty CpapDash schema found.')
          : 'Connected. The schema will be created on first start.';
      },
      error: (err) => {
        this.dbBusy = false;
        this.dbVerified = false;
        this.dbMessage = err?.error?.error || 'Could not reach the database.';
      }
    });
  }

  scan(): void {
    this.scanning = true;
    this.error = null;
    this.api.discoverDevices().subscribe({
      next: (res) => {
        this.devices = res.devices ?? [];
        // One unit is the expected case (SDD-005: one user, one machine, one
        // M&M). Preselecting the only usable one saves a click; when several
        // answer, the user picks rather than us guessing.
        const usable = this.devices.filter(d => d.local_capable);
        this.selectedDevice = usable.length === 1 ? usable[0] : null;
        this.scanning = false;
        this.scanned = true;
      },
      error: () => {
        this.devices = [];
        this.selectedDevice = null;
        this.scanning = false;
        this.scanned = true;
      }
    });
  }

  selectDevice(d: DiscoveredDevice): void {
    if (!d.local_capable) return;
    this.selectedDevice = d;
  }

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

    if (this.source === 'mm') {
      if (!this.selectedDevice) {
        this.error = 'Scan and choose your CpapDash unit first, or pick another option.';
        this.saving = false;
        return;
      }
      // The whole M&M integration is these two lines. A proxy-mode mule is an
      // ezShare server on the LAN, and EzShareClient already speaks to it.
      configUpdate['source'] = 'ezshare';
      configUpdate['ezshare_url'] = this.selectedDevice.base_url;
    } else if (this.source === 'ezshare') {
      configUpdate['source'] = 'ezshare';
      configUpdate['ezshare_url'] = this.ezshareUrl;
    } else if (this.source === 'local') {
      configUpdate['source'] = 'local';
      configUpdate['local_dir'] = this.localDir;
    }
    // skip: no config update needed

    // SDD-006 phase 2: one call that writes the config, marks setup complete
    // and restarts. A database change CANNOT be hot-reloaded (main.cpp builds
    // the IDatabase once and hands it to five consumers), so pretending
    // otherwise is what produces "it says configured but shows nothing".
    const payload: any = { ...configUpdate };
    payload['database'] = this.dbType === 'sqlite'
      ? { type: 'sqlite' }
      : this.dbPayload();

    if (this.archiveDir) payload['archive_dir'] = this.archiveDir;

    // Sent only when switched on. An untouched section must not overwrite
    // whatever an upgrading user already had in config.json.
    if (this.mqttEnabled) {
      payload['mqtt'] = {
        enabled: true, broker: this.mqttBroker, port: +this.mqttPort,
        username: this.mqttUser, password: this.mqttPassword
      };
    }
    if (this.llmEnabled) {
      payload['llm'] = {
        enabled: true, provider: 'ollama',
        endpoint: this.llmEndpoint, model: this.llmModel
      };
    }
    if (this.mlEnabled) {
      payload['ml_training'] = { enabled: true };
      payload['sleep_stage'] = { enabled: true };
    }
    if (this.cloudEnabled) {
      payload['cpapdash'] = {
        enabled: true, api_url: this.cloudApiUrl,
        token: this.cloudToken, auto_sync: this.cloudAutoSync
      };
    }

    // Installed before apply, because apply restarts the process and anything
    // queued after it would never run.
    if (this.autostartCanManage && this.autostart) {
      this.api.setAutostart(true, this.autostartScope).subscribe({
        next: (r: any) => {
          // A boot service cannot install itself, so surface the one command
          // the user has to run rather than pretending it is done.
          if (r?.needs_elevation && r?.command) this.bootCommand = r.command;
        },
        // Non-fatal: a failed autostart must not block a finished setup.
        error: (e) => console.warn('autostart install failed', e)
      });
    }

    this.api.applySetup(payload).pipe(
      // The gate caches setup_complete for the app's lifetime, so tell it the
      // answer changed; otherwise the redirect below bounces straight back here.
      tap(() => markSetupComplete())
    ).subscribe({
      next: (r) => {
        this.currentStep = 4;
        this.saving = false;
        if (r && r.restarting === false) {
          // Said plainly rather than spinning forever on a restart that is not
          // coming.
          this.applyMessage = r.message ||
            'Settings saved. Restart hms_cpap for them to take effect.';
          return;
        }
        this.applyMessage = 'Applying your settings...';
        this.waitForRestart();
      },
      error: (err) => {
        this.error = 'Failed to save configuration. Please try again.';
        this.saving = false;
        console.error('Setup error:', err);
      }
    });
  }

  /// Poll /health until the restarted process answers, then go to the dashboard.
  ///
  /// Same origin as the wizard, because the port is fixed, so there is nothing
  /// to reconnect to elsewhere. Errors during the gap are EXPECTED and ignored:
  /// the server is briefly gone by design.
  private waitForRestart(attempt = 0): void {
    if (attempt > 40) {           // ~20s, far beyond a two-second restart
      this.applyMessage = 'Saved, but the service did not come back. ' +
                          'Start hms_cpap again and reload this page.';
      return;
    }
    setTimeout(() => {
      this.api.health().subscribe({
        next: () => this.router.navigate(['/dashboard']),
        error: () => this.waitForRestart(attempt + 1)
      });
    }, 500);
  }
}
