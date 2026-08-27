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
                <option value="fysetc">Fysetc</option>
              </select>
            </label>
            <!-- Asked for by the first person to look here for myAir, which will
                 not be the last. myAir cannot BE a source: it returns daily
                 summaries only, with no waveforms, events, sessions or
                 oximetry, so an install pointed at it would have no charts, no
                 events explorer, no reports and no sleep staging. -->
            <small class="hint source-note">
              These are the ways CpapDash reads your SD card. ResMed myAir is not
              one of them: it only reports a summary of each night, not the
              recordings the charts and reports are built from. You can still
              connect it, under <strong>ResMed myAir</strong> below, to compare
              ResMed's numbers against CpapDash's.
            </small>
            <label *ngIf="config.source === 'ezshare'">
              ezShare URL
              <input type="text" [(ngModel)]="config.ezshare_url" name="ezshare_url"
                     placeholder="http://192.168.4.1" />
            </label>
            <label class="toggle-row" *ngIf="config.source === 'ezshare'">
              <input type="checkbox" [(ngModel)]="config.ezshare_range" name="ezshare_range" />
              Range request downloads
              <span class="restart-tag">restart</span>
            </label>
            <label *ngIf="config.source === 'local'">
              Local Directory
              <input type="text" [(ngModel)]="config.local_dir" name="local_dir" placeholder="/path/to/sd/DATALOG" />
            </label>
            <!-- SDD-012: applies to EVERY source, unlike local_dir. This is the
                 folder OSCAR and other file-based tools read. -->
            <label>
              Archive Directory
              <span class="required-tag" *ngIf="sourceNeedsArchive()">required</span>
              <input type="text" [(ngModel)]="config.archive_dir" name="archive_dir"
                     placeholder="/path/to/CPAPData"
                     [class.field-missing]="archiveDirMissing()" />
              <!-- Ticket 67: an empty value here switches off OSCAR archiving and
                   SleepHQ export silently, while the dashboard keeps filling in
                   normally, so it reads as data loss rather than a missing
                   setting. Mirrors PreflightService::checkArchiveDir. -->
              <small class="hint warn" *ngIf="archiveDirMissing()">
                Not set. Your {{ config.source === 'fysetc' ? 'Fysetc' : 'Mule and Miner' }}
                downloads files that need somewhere to land, so nothing is being
                written to disk: OSCAR has nothing to import and SleepHQ export
                stays blocked. Nights already collected are safe in the database
                and appear here as normal.
              </small>
              <small class="hint">
                Where collected nights are written, as a card layout (STR.edf and
                DATALOG/ side by side). Point OSCAR's card import here. Use forward
                slashes on Windows, e.g. C:/Users/you/CPAPData
              </small>
            </label>
          </div>
        </div>

        <!-- Section: Fysetc (raw SD sector push). Shown only for its own source,
             which is also what makes that source configurable at all. -->
        <div class="section" *ngIf="config.source === 'fysetc'">
          <div class="section-header" (click)="toggle('fysetc')">
            <span class="chevron" [class.open]="open['fysetc']">&#9654;</span>
            Fysetc TCP
            <span class="restart-tag">restart</span>
          </div>
          <div class="section-body" *ngIf="open['fysetc']">
            <label class="toggle-row">
              <input type="checkbox" [(ngModel)]="config.fysetc.enabled" name="fysetc_enabled" />
              Enabled
            </label>
            <label>
              Listen Port
              <input type="number" [(ngModel)]="config.fysetc.listen_port" name="fysetc_listen_port" />
            </label>
            <label>
              Listen Address
              <input type="text" [(ngModel)]="config.fysetc.listen_bind" name="fysetc_listen_bind"
                     placeholder="0.0.0.0" />
            </label>
            <label>
              Connection Timeout (seconds)
              <input type="number" [(ngModel)]="config.fysetc.connection_timeout_s" name="fysetc_timeout" />
            </label>
            <label>
              Reconstructed Card Directory
              <input type="text" [(ngModel)]="config.fysetc.archive_dir" name="fysetc_archive_dir" />
            </label>
            <label>
              Log Directory
              <input type="text" [(ngModel)]="config.fysetc.log_dir" name="fysetc_log_dir" />
            </label>
          </div>
        </div>

        <!-- Section: Support log. Not gated on source: every install can be
             asked for its log, which is the whole reason it exists. -->
        <div class="section">
          <div class="section-header" (click)="toggle('logging')">
            <span class="chevron" [class.open]="open['logging']">&#9654;</span>
            Support Log
            <span class="restart-tag">restart</span>
          </div>
          <div class="section-body" *ngIf="open['logging']">
            <p class="hint">
              Keeps a copy of everything the service prints, so it can be sent
              when something goes wrong. Leave the file blank to use the default:
              next to the program if that folder is writable, otherwise beside
              config.json.
            </p>
            <label class="toggle-row">
              <input type="checkbox" [(ngModel)]="config.logging.enabled" name="logging_enabled" />
              Enabled
            </label>
            <label>
              Log File
              <input type="text" [(ngModel)]="config.logging.file" name="logging_file"
                     placeholder="(default)" />
            </label>
            <label>
              Rotate After (MB)
              <input type="number" [(ngModel)]="config.logging.max_mb" name="logging_max_mb" />
            </label>
            <label>
              Old Logs Kept
              <input type="number" [(ngModel)]="config.logging.keep" name="logging_keep" />
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

        <!-- Section: SleepHQ Sync -->
        <div class="section">
          <div class="section-header" (click)="toggle('sleephq')">
            <span class="chevron" [class.open]="open['sleephq']">&#9654;</span>
            SleepHQ Sync
            <span class="badge" *ngIf="!config.sleephq.enabled">optional</span>
          </div>
          <div class="section-body" *ngIf="open['sleephq']">
            <p class="section-desc">
              Forward your therapy nights to SleepHQ via their public API. Create
              API credentials in SleepHQ under Account &rarr; API.
            </p>
            <label class="toggle-row">
              Enabled
              <input type="checkbox" [(ngModel)]="config.sleephq.enabled" name="sleephq_enabled" />
            </label>
            <ng-container *ngIf="config.sleephq.enabled">
              <label>
                Client ID
                <input type="text" [(ngModel)]="config.sleephq.client_id" name="sleephq_client_id"
                       placeholder="SleepHQ API client ID" />
              </label>
              <label>
                Client Secret
                <input type="password" [(ngModel)]="config.sleephq.client_secret" name="sleephq_client_secret"
                       placeholder="SleepHQ API client secret" />
              </label>
              <label class="toggle-row">
                <input type="checkbox" [(ngModel)]="config.sleephq.auto_on_session" name="sleephq_auto_session" />
                Upload automatically when a session completes
              </label>
              <label class="toggle-row">
                <input type="checkbox" [(ngModel)]="config.sleephq.auto_on_backfill" name="sleephq_auto_backfill" />
                Upload automatically when importing local data
              </label>
              <label>
                Quiet Period (minutes)
                <input type="number" [(ngModel)]="config.sleephq.quiet_minutes" name="sleephq_quiet_minutes" min="1" />
                <span class="restart-tag">restart</span>
                <small class="hint">
                  How long the archive must stop changing before a night is
                  exported, so a night still being written is not sent early.
                </small>
              </label>
            </ng-container>
          </div>
        </div>

        <!-- Section: myAir (SDD-020). Read-only, and deliberately NOT part of
             the config form: the password is exchanged for a token by a
             dedicated endpoint and is never saved, so binding it to the config
             object would be exactly the wrong shape. -->
        <div class="section">
          <div class="section-header" (click)="toggle('myair')">
            <span class="chevron" [class.open]="open['myair']">&#9654;</span>
            ResMed myAir
            <span class="badge" *ngIf="!myair.connected">optional</span>
            <span class="badge connected" *ngIf="myair.connected">connected</span>
          </div>
          <div class="section-body" *ngIf="open['myair']">
            <p class="section-desc">
              Read your own nights back from ResMed and show their score next to
              CpapDash's, per night and per component. Read only: nothing is ever
              written back to ResMed.
            </p>

            <div class="myair-note">
              Your password is used once to sign in and is <strong>not stored</strong>.
              ResMed issues a revocable token that replaces it, which can only read
              this account's sleep data and cannot sign in as you anywhere.
            </div>

            <div class="myair-state" *ngIf="myair.needs_reauth">
              The saved token is no longer valid, so myAir has been disconnected.
              Sign in again to reconnect.
            </div>

            <ng-container *ngIf="!myair.connected && !myair.awaiting_code">
              <label>
                myAir email
                <input type="email" [(ngModel)]="myairForm.username" name="myair_username"
                       [ngModelOptions]="{standalone: true}" placeholder="you@example.com" />
              </label>
              <label>
                Password
                <input type="password" [(ngModel)]="myairForm.password" name="myair_password"
                       [ngModelOptions]="{standalone: true}" placeholder="Used once, never saved" />
              </label>
              <label>
                Region
                <!-- Australia is served by the North America endpoints, which is
                     not guessable and is why it is named here rather than left
                     to a user in Sydney to work out. Europe is the region with
                     an email verification step, so saying that up front stops
                     the code prompt being a surprise. -->
                <select [(ngModel)]="myairForm.region" name="myair_region"
                        [ngModelOptions]="{standalone: true}">
                  <option value="NA">North America and Australia</option>
                  <option value="EU">Europe (emails a verification code)</option>
                </select>
              </label>
              <button type="button" class="myair-btn" (click)="myairConnect()"
                      [disabled]="myairBusy || !myairForm.username || !myairForm.password">
                {{ myairBusy ? 'Signing in...' : 'Connect' }}
              </button>
            </ng-container>

            <!-- Europe emails a code the first time. After that the remembered
                 device stops it asking. -->
            <ng-container *ngIf="myair.awaiting_code">
              <label>
                Verification code
                <input type="text" [(ngModel)]="myairForm.code" name="myair_code"
                       [ngModelOptions]="{standalone: true}" placeholder="Emailed by ResMed" />
              </label>
              <button type="button" class="myair-btn" (click)="myairVerify()"
                      [disabled]="myairBusy || !myairForm.code">
                {{ myairBusy ? 'Checking...' : 'Verify' }}
              </button>
            </ng-container>

            <ng-container *ngIf="myair.connected">
              <div class="myair-connected">
                Connected as <strong>{{ myair.username }}</strong> ({{ myair.region }}),
                checking every {{ myair.poll_minutes }} minutes.
              </div>
              <button type="button" class="myair-btn danger" (click)="myairDisconnect()"
                      [disabled]="myairBusy">Disconnect</button>
              <small class="hint">
                Disconnecting forgets the token. Nights already fetched are kept.
              </small>
            </ng-container>

            <div class="myair-msg" *ngIf="myairMessage">{{ myairMessage }}</div>
          </div>
        </div>

        <!-- Section: CpapDash Cloud (SDD-004 mirror). Local stays authoritative. -->
        <div class="section">
          <div class="section-header" (click)="toggle('cpapdash')">
            <span class="chevron" [class.open]="open['cpapdash']">&#9654;</span>
            CpapDash Cloud
            <span class="badge" *ngIf="!config.cpapdash.enabled">optional</span>
          </div>
          <div class="section-body" *ngIf="open['cpapdash']">
            <p class="section-desc">
              Mirror your equipment and supplies to your CpapDash account. Your
              local install stays the source of truth, and turning this off leaves
              the equipment feature working exactly as it does now.
            </p>
            <label class="toggle-row">
              Enabled
              <input type="checkbox" [(ngModel)]="config.cpapdash.enabled" name="cpapdash_enabled" />
            </label>
            <ng-container *ngIf="config.cpapdash.enabled">
              <label>
                API URL
                <input type="text" [(ngModel)]="config.cpapdash.api_url" name="cpapdash_api_url"
                       placeholder="https://api.cpapdash.com" />
              </label>
              <label>
                Token
                <input type="password" [(ngModel)]="config.cpapdash.token" name="cpapdash_token"
                       placeholder="Paste a long-lived API token" />
                <small class="hint">
                  Stored redacted. Leave the masked value untouched to keep the
                  current token. A token can be revoked without changing your
                  account password.
                </small>
              </label>
              <label class="toggle-row">
                <input type="checkbox" [(ngModel)]="config.cpapdash.auto_sync" name="cpapdash_auto_sync" />
                Also sync on the collection sweep after local edits
              </label>
            </ng-container>
          </div>
        </div>

        <!-- Section: Sleep Stage Inference -->
        <div class="section">
          <div class="section-header" (click)="toggle('sleep_stage')">
            <span class="chevron" [class.open]="open['sleep_stage']">&#9654;</span>
            Sleep Stage Inference
            <span class="badge" *ngIf="!config.sleep_stage.enabled">optional</span>
            <span class="restart-tag">restart</span>
          </div>
          <div class="section-body" *ngIf="open['sleep_stage']">
            <label class="toggle-row">
              Enabled
              <input type="checkbox" [(ngModel)]="config.sleep_stage.enabled" name="sleep_stage_enabled" />
            </label>
            <ng-container *ngIf="config.sleep_stage.enabled">
              <label class="toggle-row">
                <input type="checkbox" [(ngModel)]="config.sleep_stage.live_inference" name="sleep_stage_live" />
                Infer during the night, not only once it ends
              </label>
              <label>
                Model Directory
                <input type="text" [(ngModel)]="config.sleep_stage.model_dir" name="sleep_stage_model_dir"
                       placeholder="Leave empty to use the data directory" />
              </label>
              <label>
                Model Version
                <input type="text" [(ngModel)]="config.sleep_stage.model_version" name="sleep_stage_model_version"
                       placeholder="shhs-rf-v1" />
              </label>
            </ng-container>
          </div>
        </div>

        <!-- Section: Agent. Needs LLM and PostgreSQL, so it is shown disabled
             with the reason rather than hidden, per SDD-012 decision 5. -->
        <div class="section">
          <div class="section-header" (click)="toggle('agent')">
            <span class="chevron" [class.open]="open['agent']">&#9654;</span>
            Agent
            <span class="badge" *ngIf="!config.agent.enabled">optional</span>
            <span class="restart-tag">restart</span>
          </div>
          <div class="section-body" *ngIf="open['agent']">
            <p class="section-desc" *ngIf="!agentAvailable()">
              Unavailable: the agent needs LLM Summaries enabled and a PostgreSQL
              database. {{ agentBlockedReason() }}
            </p>
            <label class="toggle-row">
              Enabled
              <input type="checkbox" [(ngModel)]="config.agent.enabled" name="agent_enabled"
                     [disabled]="!agentAvailable()" />
            </label>
            <ng-container *ngIf="config.agent.enabled && agentAvailable()">
              <label>
                Embedding Model
                <input type="text" [(ngModel)]="config.agent.embed_model" name="agent_embed_model"
                       placeholder="nomic-embed-text" />
              </label>
              <label>
                Temperature
                <input type="number" step="0.1" min="0" max="2"
                       [(ngModel)]="config.agent.temperature" name="agent_temperature" />
              </label>
              <label>
                Max Iterations
                <input type="number" min="1" [(ngModel)]="config.agent.max_iterations" name="agent_max_iterations" />
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
                <input type="text" [(ngModel)]="config.mqtt.broker" name="mqtt_broker" placeholder="127.0.0.1" />
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
              <label>
                Client ID
                <input type="text" [(ngModel)]="config.mqtt.client_id" name="mqtt_client_id"
                       placeholder="hms_cpap" />
                <small class="hint">
                  Changing only this does not take effect until the service restarts.
                </small>
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
                  <option value="ollama">Ollama (native API)</option>
                  <option value="openai">OpenAI-compatible</option>
                  <option value="gemini">Gemini</option>
                  <option value="anthropic">Anthropic</option>
                </select>
                <span class="hint" *ngIf="config.llm.provider === 'openai'">
                  Any server speaking the OpenAI chat API: OpenAI itself,
                  OpenRouter, LM Studio, vLLM, llama.cpp, or Ollama's own
                  /v1 surface. Put its base URL in Endpoint.
                </span>
              </label>
              <label>
                Endpoint
                <input type="text" [(ngModel)]="config.llm.endpoint" name="llm_endpoint" placeholder="http://127.0.0.1:11434 (base URL, no path)" />
              </label>
              <label>
                Model
                <input type="text" [(ngModel)]="config.llm.model" name="llm_model" placeholder="llama3.1:8b" />
              </label>
              <label>
                API Key
                <input type="password" [(ngModel)]="config.llm.api_key" name="llm_api_key" placeholder="Optional for Ollama and most local servers" />
              </label>
              <label>
                Max Tokens
                <input type="number" [(ngModel)]="config.llm.max_tokens" name="llm_max_tokens" min="1" />
                <span class="restart-tag">restart</span>
              </label>
              <label>
                Prompt File
                <input type="text" [(ngModel)]="config.llm.prompt_file" name="llm_prompt_file"
                       placeholder="Leave empty to use the built-in prompt" />
                <span class="restart-tag">restart</span>
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

        <!-- Section: Advanced. Both of these are resolved once at startup and
             both can make the app unreachable, so they sit last and warn. -->
        <div class="section">
          <div class="section-header" (click)="toggle('advanced')">
            <span class="chevron" [class.open]="open['advanced']">&#9654;</span>
            Advanced
            <span class="restart-tag">restart</span>
          </div>
          <div class="section-body" *ngIf="open['advanced']">
            <label>
              Web Port
              <input type="number" [(ngModel)]="config.web_port" name="web_port" min="1" max="65535" />
              <small class="hint warn">
                This is the port serving this page. After saving and restarting,
                this tab moves itself to the new port. If the new port is blocked
                or already in use, the app becomes unreachable in the browser and
                you would have to fix it in config.json.
              </small>
            </label>
            <label>
              Static Directory
              <input type="text" [(ngModel)]="config.static_dir" name="static_dir"
                     (focus)="confirmStaticDir()" [readonly]="!staticDirUnlocked" />
              <small class="hint warn" *ngIf="!staticDirUnlocked">
                Locked. This is where the web interface itself is served from, and
                a wrong value serves no interface at all. Click the field to unlock.
              </small>
              <small class="hint" *ngIf="staticDirUnlocked">
                Leave empty to let the app find its own bundle.
              </small>
            </label>
          </div>
        </div>

        <div class="actions">
          <button type="submit" class="btn-save" [disabled]="saving">
            {{ saving ? 'Saving...' : 'Save' }}
          </button>
        </div>
      </form>

      <!-- SDD-012: saved is not the same as in effect. Everything absent from
           BurstCollectorService's ConfigSnapshot only lands on a restart, and
           until now the page said "saved" and left it at that. -->
      <div class="restart-banner" *ngIf="restartPending">
        <div class="restart-text">
          <strong>Saved, but not yet in effect.</strong>
          {{ restartReason }}
          <span *ngIf="portMoved"> This page will move to port {{ config?.web_port }}.</span>
        </div>
        <div class="restart-actions">
          <button type="button" class="btn-save" (click)="restartNow()" [disabled]="restarting">
            {{ restarting ? 'Restarting...' : 'Restart now' }}
          </button>
          <button type="button" class="btn-link" (click)="dismissRestart()" [disabled]="restarting">
            Later
          </button>
        </div>
      </div>
      <div class="restart-banner manual" *ngIf="restartManual">
        <div class="restart-text">
          <strong>Restart hms-cpap for these settings to take effect.</strong>
          This install cannot restart itself, so it has to be done the same way it
          was started.
        </div>
      </div>

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

    /* SDD-012: marks a field that only takes effect after a restart. */
    .restart-tag {
      margin-left: 0.5rem; font-size: 0.62rem; font-weight: 600;
      color: #ffb74d; text-transform: uppercase; letter-spacing: 0.06em;
      border: 1px solid #ffb74d55; border-radius: 3px; padding: 0.05rem 0.3rem;
      white-space: nowrap;
    }
    /* SDD-020 myAir */
    .source-note { display: block; margin-top: 0.5rem; line-height: 1.5; }
    .badge.connected { background: #14532d; color: #4ade80; }
    .myair-note { background: rgba(100, 181, 246, 0.08); border: 1px solid rgba(100, 181, 246, 0.35);
                  border-radius: 6px; padding: 0.6rem 0.8rem; color: #cfe6fb; font-size: 0.8rem;
                  line-height: 1.5; margin-bottom: 0.75rem; }
    .myair-state { background: rgba(251, 191, 36, 0.08); border: 1px solid rgba(251, 191, 36, 0.4);
                   border-radius: 6px; padding: 0.6rem 0.8rem; color: #fde68a; font-size: 0.8rem;
                   margin-bottom: 0.75rem; }
    .myair-connected { color: #ddd; font-size: 0.85rem; margin-bottom: 0.6rem; }
    .myair-msg { color: #9ad0a0; font-size: 0.8rem; margin-top: 0.6rem; }
    .myair-btn { padding: 0.45rem 1rem; border-radius: 5px; border: 1px solid #1976d2;
                 background: #1976d2; color: #fff; font-weight: 600; cursor: pointer;
                 font-size: 0.8rem; }
    .myair-btn:disabled { opacity: 0.5; cursor: not-allowed; }
    .myair-btn.danger { background: transparent; border-color: #f87171; color: #f87171; }

    .section-header .restart-tag { margin-left: auto; }
    .section-header .badge + .restart-tag { margin-left: 0.5rem; }

    .restart-banner {
      display: flex; align-items: center; gap: 1rem; flex-wrap: wrap;
      margin-top: 1rem; padding: 0.85rem 1rem; border-radius: 6px;
      background: #2a2416; border: 1px solid #ffb74d55; color: #f0e6d2;
      font-size: 0.85rem;
    }
    .restart-banner.manual { background: #2a1c16; border-color: #ff8a6555; }
    .restart-text { flex: 1; min-width: 260px; line-height: 1.45; }
    .restart-actions { display: flex; align-items: center; gap: 0.5rem; }
    .btn-link {
      background: none; border: none; color: #90a4ae; cursor: pointer;
      font-size: 0.8rem; text-decoration: underline; padding: 0.4rem;
    }
    .btn-link:disabled { opacity: 0.5; cursor: default; }
    .hint.warn { color: #ffb74d; font-style: normal; }

    /* A field the current mode cannot work without. */
    .required-tag {
      margin-left: 0.5rem; font-size: 0.62rem; font-weight: 600;
      color: #ff8a65; text-transform: uppercase; letter-spacing: 0.06em;
      border: 1px solid #ff8a6555; border-radius: 3px; padding: 0.05rem 0.3rem;
    }
    /* A plain label is a flex COLUMN, so a bare tag span stretches to the full
       width and reads as a banner rather than a marker. Both tags opt out. */
    label > .required-tag,
    label > .restart-tag { align-self: flex-start; margin-left: 0; }
    input.field-missing {
      border-color: #ff8a65 !important;
      background: #2a1c16 !important;
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

  // SDD-020 myAir. Held apart from `config` on purpose: the password is
  // exchanged for a token by its own endpoint and is never saved, so binding it
  // into the config form would invite it to be written to disk.
  myair: any = { available: false, enabled: false, connected: false };
  myairForm = { username: '', password: '', region: 'NA', code: '' };
  myairBusy = false;
  myairMessage = '';

  // BLE adapter status
  bleStatus = '';

  // LLM Prompt state
  llmPrompt = '';
  llmPromptPath = '';
  savingPrompt = false;
  defaultPrompt = '';

  // ── SDD-020 myAir ────────────────────────────────────────────────────────
  //
  // Deliberately not part of save(): the password goes to /api/myair/connect,
  // which exchanges it for a token and discards it. It is cleared from the form
  // the moment the request is sent, so it does not linger in the page either.

  loadMyAirStatus(): void {
    this.api.getMyAirStatus().subscribe({
      next: (s) => {
        this.myair = s || { available: false, connected: false };
        if (this.myair.username && !this.myairForm.username) {
          this.myairForm.username = this.myair.username;
        }
        if (this.myair.region) this.myairForm.region = this.myair.region;
      },
      error: () => { this.myair = { available: false, connected: false }; },
    });
  }

  myairConnect(): void {
    this.myairBusy = true;
    this.myairMessage = '';
    const password = this.myairForm.password;
    this.myairForm.password = '';  // out of the page as soon as it is sent

    this.api.myairConnect(this.myairForm.username, password, this.myairForm.region)
      .subscribe({
        next: (r: any) => {
          this.myairBusy = false;
          this.myairMessage = r?.message || '';
          this.loadMyAirStatus();
        },
        error: (e) => {
          this.myairBusy = false;
          this.myairMessage = e?.error?.error || 'Could not reach myAir.';
        },
      });
  }

  myairVerify(): void {
    this.myairBusy = true;
    this.myairMessage = '';
    const code = this.myairForm.code;
    this.myairForm.code = '';

    this.api.myairVerify(code).subscribe({
      next: (r: any) => {
        this.myairBusy = false;
        this.myairMessage = r?.message || '';
        this.loadMyAirStatus();
      },
      error: (e) => {
        this.myairBusy = false;
        this.myairMessage = e?.error?.error || 'Could not verify the code.';
      },
    });
  }

  myairDisconnect(): void {
    this.myairBusy = true;
    this.myairMessage = '';
    this.api.myairDisconnect().subscribe({
      next: (r: any) => {
        this.myairBusy = false;
        this.myairMessage = r?.message || '';
        this.myairForm.password = '';
        this.loadMyAirStatus();
      },
      error: () => {
        this.myairBusy = false;
        this.myairMessage = 'Could not disconnect.';
      },
    });
  }

  open: Record<string, boolean> = {
    source: true,
    fysetc: false,
    logging: false,
    o2ring: false,
    sleephq: false,
    myair: false,
    cpapdash: false,
    sleep_stage: false,
    agent: false,
    timing: false,
    backfill: false,
    database: true,
    mqtt: false,
    llm: false,
    ml_training: false,
    llm_prompt: false,
    device: true,
    advanced: false,
  };

  // SDD-012 restart state.
  //
  // `pristine` is the config as the server last confirmed it. Restart-required
  // changes are detected by diffing against it AFTER a successful save, so the
  // banner reflects what the server actually accepted rather than what is
  // currently typed into the form.
  private pristine: AppConfig | null = null;
  restartPending = false;
  restartManual = false;
  restarting = false;
  restartReason = '';
  portMoved = false;
  staticDirUnlocked = false;

  /**
   * Settings absent from BurstCollectorService::ConfigSnapshot, which is the
   * entire hot-reload surface. Anything listed here reaches the config file and
   * the in-memory config but never the running service until it restarts.
   *
   * `database` is listed despite reloadConfig() rebuilding db_service_, because
   * main.cpp hands the original IDatabase to five consumers and only the burst
   * collector's copy is swapped. A partial swap is not something to advertise
   * as working.
   */
  private static readonly RESTART_KEYS: ReadonlyArray<[string, string]> = [
    ['web_port', 'the web port'],
    ['static_dir', 'the static directory'],
    ['ezshare_range', 'the range-request setting'],
    ['llm.max_tokens', 'the LLM token limit'],
    ['llm.prompt_file', 'the LLM prompt file'],
    ['mqtt.client_id', 'the MQTT client ID'],
    ['sleephq', 'SleepHQ settings'],
    ['agent', 'agent settings'],
    ['sleep_stage', 'sleep stage settings'],
    ['fysetc', 'Fysetc settings'],
    ['logging', 'support log settings'],
    ['database', 'database settings'],
  ];

  constructor(private api: CpapApiService) {}

  ngOnDestroy(): void {
    this.destroy$.next();
    this.destroy$.complete();
  }

  ngOnInit(): void {
    this.loadMyAirStatus();

    this.api.getConfig().subscribe({
      next: (cfg) => {
        // Ensure ml_training exists with defaults
        if (!cfg.ml_training) {
          cfg.ml_training = { enabled: false, schedule: 'weekly', model_dir: '', min_days: 30, max_training_days: 0 };
        }
        if (!cfg.o2ring) {
          cfg.o2ring = { enabled: false, mode: 'http', mule_url: '' };
        }
        if (!cfg.sleephq) {
          cfg.sleephq = { enabled: false, client_id: '', client_secret: '',
                          auto_on_session: true, auto_on_backfill: true,
                          quiet_minutes: 15 };
        }
        if (cfg.sleephq.quiet_minutes == null) cfg.sleephq.quiet_minutes = 15;
        // SDD-012: an older config.json predates these blocks entirely, and an
        // absent block would otherwise blow up the template's [(ngModel)] paths.
        if (!cfg.agent) {
          cfg.agent = { enabled: false, embed_model: 'nomic-embed-text',
                        temperature: 0.3, max_iterations: 5 };
        }
        if (!cfg.sleep_stage) {
          cfg.sleep_stage = { enabled: false, live_inference: true,
                              model_dir: '', model_version: 'shhs-rf-v1' };
        }
        if (!cfg.fysetc) {
          cfg.fysetc = { enabled: false, listen_port: 9000, listen_bind: '0.0.0.0',
                         connection_timeout_s: 30, archive_dir: '',
                         log_dir: '/var/log/maestro_hub' };
        }
        if (!cfg.logging) {
          cfg.logging = { enabled: true, file: '', max_mb: 5, keep: 3 };
        }
        if (!cfg.cpapdash) {
          cfg.cpapdash = { enabled: false, api_url: 'https://api.cpapdash.com',
                           token: '', auto_sync: false };
        }
        if (cfg.archive_dir == null) cfg.archive_dir = '';
        if (cfg.static_dir == null) cfg.static_dir = '';
        this.config = cfg;
        this.pristine = JSON.parse(JSON.stringify(cfg));
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
    const submitted: AppConfig = JSON.parse(JSON.stringify(this.config));
    this.api.updateConfig(this.config).subscribe({
      next: () => {
        this.saving = false;
        this.showToast('Configuration saved.', false);
        this.evaluateRestart(submitted);
        this.pristine = submitted;
      },
      error: () => {
        this.saving = false;
        this.showToast('Save failed.', true);
      },
    });
  }

  /** Read a dotted path off a config object, for the RESTART_KEYS comparison. */
  private valueAt(cfg: any, path: string): any {
    return path.split('.').reduce((o, k) => (o == null ? o : o[k]), cfg);
  }

  /**
   * Decide whether the save that just succeeded needs a restart, and say which
   * settings caused it. Compared against `pristine` so an untouched field never
   * triggers the banner just because the user opened its section.
   */
  private evaluateRestart(saved: AppConfig): void {
    if (!this.pristine) return;
    const changed: string[] = [];
    for (const [path, label] of SettingsComponent.RESTART_KEYS) {
      const before = JSON.stringify(this.valueAt(this.pristine, path) ?? null);
      const after = JSON.stringify(this.valueAt(saved, path) ?? null);
      if (before !== after) changed.push(label);
    }
    this.portMoved = this.valueAt(this.pristine, 'web_port') !== this.valueAt(saved, 'web_port');
    if (!changed.length) return;

    const list = changed.length === 1
      ? changed[0]
      : changed.slice(0, -1).join(', ') + ' and ' + changed[changed.length - 1];
    this.restartReason = `You changed ${list}, which the running service only picks up on a restart.`;
    this.restartPending = true;
    this.restartManual = false;
  }

  dismissRestart(): void {
    this.restartPending = false;
  }

  /**
   * SDD-012: POST /api/config/restart, then wait for the process to answer again.
   *
   * When the web port changed, the old origin is already dead, so /health is
   * polled on the NEW port and the page moves itself there. Otherwise the
   * current origin is polled and the page simply reloads.
   */
  restartNow(): void {
    if (!this.config || this.restarting) return;
    this.restarting = true;
    const target = this.portMoved
      ? `${window.location.protocol}//${window.location.hostname}:${this.config.web_port}`
      : '';
    this.api.restartService().subscribe({
      next: (res: any) => {
        if (res && res.restarting === false) {
          // Cannot restart itself. Say so rather than spinning on a process
          // that is never going to go away.
          this.restarting = false;
          this.restartPending = false;
          this.restartManual = true;
          return;
        }
        this.pollUntilBack(target, 1);
      },
      error: () => {
        this.restarting = false;
        this.showToast('Restart request failed.', true);
      },
    });
  }

  /** Poll /health until the restarted process answers, then land the user back. */
  private pollUntilBack(origin: string, attempt: number): void {
    // ~20s, matching the wizard's window and far beyond a normal restart.
    if (attempt > 40) {
      this.restarting = false;
      this.showToast(
        origin
          ? `No answer on ${origin}. The port may be blocked or already in use.`
          : 'The service did not come back. Check that it is running.',
        true);
      return;
    }
    const url = (origin || '') + '/health';
    fetch(url, { cache: 'no-store' })
      .then((r) => {
        if (!r.ok) throw new Error('not ready');
        if (origin) window.location.href = origin + '/settings';
        else window.location.reload();
      })
      .catch(() => setTimeout(() => this.pollUntilBack(origin, attempt + 1), 500));
  }

  /** SDD-012 decision 3: static_dir is editable, but not by accident. */
  confirmStaticDir(): void {
    if (this.staticDirUnlocked) return;
    this.staticDirUnlocked = window.confirm(
      'Change the static directory?\n\n' +
      'This is where the web interface itself is served from. A wrong value ' +
      'serves no interface at all, and recovering means editing config.json by ' +
      'hand.\n\nLeave it empty unless you know you need it.');
  }

  /**
   * Does the selected source download files that have to be written somewhere?
   *
   * Mirrors PreflightService::sourceNeedsArchive. local and lowenstein read
   * files already on disk; ezShare and Fysetc receive them over the network.
   */
  sourceNeedsArchive(): boolean {
    const s = this.config?.source;
    return s === 'ezshare' || s === 'fysetc';
  }

  /** Required by the current source, and empty. Ticket 67's silent failure. */
  archiveDirMissing(): boolean {
    return this.sourceNeedsArchive() && !this.config?.archive_dir?.trim();
  }

  /** The agent needs LLM and PostgreSQL; shown disabled with the reason. */
  agentAvailable(): boolean {
    return !!this.config?.llm?.enabled && this.config?.database?.type === 'postgresql';
  }

  agentBlockedReason(): string {
    if (!this.config) return '';
    const missing: string[] = [];
    if (!this.config.llm?.enabled) missing.push('LLM Summaries is off');
    if (this.config.database?.type !== 'postgresql') missing.push('the database is not PostgreSQL');
    return missing.length ? `Right now ${missing.join(' and ')}.` : '';
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
