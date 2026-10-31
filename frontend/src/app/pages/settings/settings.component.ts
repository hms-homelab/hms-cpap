import { Component, OnInit, OnDestroy } from '@angular/core';
import { CommonModule } from '@angular/common';
import { FormsModule } from '@angular/forms';
import { TranslatePipe, TranslateService } from '@ngx-translate/core';
import { Subject, Subscription, timer } from 'rxjs';
import { switchMap, takeUntil, takeWhile, tap } from 'rxjs/operators';
import { CpapApiService, UpdateStatus, MuleUnit, MuleCommand, MuleFirmware, MuleUpdateJob,
         SleepBridge, SleepBridgeUpdate } from '../../services/cpap-api.service';
import { UpdateFlowService } from '../../services/update-flow.service';
import { AppConfig } from '../../models/config.model';

@Component({
  selector: 'app-settings',
  standalone: true,
  imports: [CommonModule, FormsModule, TranslatePipe],
  template: `
    <div class="settings">
      <h2>{{ 'settings.title' | translate }}</h2>

      <div *ngIf="loading" class="loading">{{ 'settings.loading' | translate }}</div>
      <div *ngIf="error" class="error">{{ error }}</div>

      <form *ngIf="config" (ngSubmit)="save()">

        <!-- Section 1: Data Source -->
        <div class="section">
          <div class="section-header" (click)="toggle('source')">
            <span class="chevron" [class.open]="open['source']">&#9654;</span>
            {{ 'settings.source.heading' | translate }}
          </div>
          <div class="section-body" *ngIf="open['source']">
            <!-- SDD-022: two questions. Where the files come from, and what
                 wrote them. One field answering both is why a Prisma or Sefam
                 owner had to hand-edit config.json to select their machine. -->
            <label>
              {{ 'settings.source.transport' | translate }}
              <select [(ngModel)]="config.transport" name="transport"
                      (ngModelChange)="onTransportChange($event)">
                <option value="ezshare">{{ 'settings.source.ezshare' | translate }}</option>
                <option value="flashair">{{ 'settings.source.flashair' | translate }}</option>
                <option value="local">{{ 'settings.source.local' | translate }}</option>
              </select>
            </label>
            <!-- A folder takes every format. An ez Share takes ResMed and, since
                 SDD-031, a Sefam S.Box card in its slot; not a Löwenstein, which
                 answers the ez Share with error 601. -->
            <label *ngIf="config.transport === 'local'">
              {{ 'settings.source.format' | translate }}
              <select [(ngModel)]="config.format" name="format">
                <option value="resmed">ResMed</option>
                <option value="lowenstein">Löwenstein Prisma</option>
                <option value="sefam">Sefam S.Box</option>
                <option value="bmc">{{ 'settings.source.bmc' | translate }}</option>
                <option value="philips">{{ 'settings.source.philips' | translate }}</option>
              </select>
            </label>
            <label *ngIf="config.transport === 'ezshare'">
              {{ 'settings.source.format' | translate }}
              <select [(ngModel)]="config.format" name="format_ezshare">
                <option value="resmed">ResMed</option>
                <option value="sefam">Sefam S.Box</option>
              </select>
            </label>
            <!-- Asked for by the first person to look here for myAir, which will
                 not be the last. myAir cannot BE a source: it returns daily
                 summaries only, with no waveforms, events, sessions or
                 oximetry, so an install pointed at it would have no charts, no
                 events explorer, no reports and no sleep staging. -->
            <small class="hint source-note">
              {{ 'settings.source.notePre' | translate }}
              <strong>{{ 'settings.myair.heading' | translate }}</strong>
              {{ 'settings.source.notePost' | translate }}
            </small>
            <label *ngIf="config.transport === 'ezshare'">
              {{ 'settings.source.ezshareUrl' | translate }}
              <input type="text" [(ngModel)]="config.ezshare_url" name="ezshare_url"
                     placeholder="http://192.168.4.1" />
            </label>
            <label class="toggle-row" *ngIf="config.transport === 'ezshare'">
              <input type="checkbox" [(ngModel)]="config.ezshare_range" name="ezshare_range" />
              {{ 'settings.source.rangeDownloads' | translate }}
              <span class="restart-tag">{{ 'settings.tag.restart' | translate }}</span>
            </label>
            <!-- SDD-056: a FlashAir on the home WiFi answers as flashair.local.
                 It carries a ResMed card; onTransportChange sets the format. -->
            <label *ngIf="config.transport === 'flashair'">
              {{ 'settings.source.flashairUrl' | translate }}
              <input type="text" [(ngModel)]="config.flashair_url" name="flashair_url"
                     placeholder="http://flashair.local" />
            </label>
            <label *ngIf="config.transport === 'local'">
              {{ 'settings.source.localDir' | translate }}
              <input type="text" [(ngModel)]="config.local_dir" name="local_dir" placeholder="/path/to/sd/DATALOG" />
            </label>
            <!-- SDD-012: applies to EVERY source, unlike local_dir. This is the
                 folder OSCAR and other file-based tools read. -->
            <label>
              {{ 'settings.source.archiveDir' | translate }}
              <span class="required-tag" *ngIf="sourceNeedsArchive()">{{ 'settings.tag.required' | translate }}</span>
              <input type="text" [(ngModel)]="config.archive_dir" name="archive_dir"
                     placeholder="/path/to/CPAPData"
                     [class.field-missing]="archiveDirMissing()" />
              <!-- Ticket 67: an empty value here switches off OSCAR archiving and
                   SleepHQ export silently, while the dashboard keeps filling in
                   normally, so it reads as data loss rather than a missing
                   setting. Mirrors PreflightService::checkArchiveDir. -->
              <!-- The device name is interpolated rather than concatenated, so
                   a language can put it anywhere in the sentence. -->
              <small class="hint warn" *ngIf="archiveDirMissing()">
                {{ 'settings.source.archiveMissing' | translate:{ device: 'Mule and Miner' } }}
              </small>
              <!-- SDD-037 D3 (ticket 129): a local source reads the folder it
                   was given and copies nothing, so the "nights are written
                   here" sentence was wrong for it and sent people looking for
                   a DATALOG that was never going to appear. Their own folder
                   already IS the card copy. -->
              <small class="hint">
                {{ (config.transport === 'local' ? 'settings.source.archiveHintLocal'
                                                 : 'settings.source.archiveHint') | translate }}
              </small>
            </label>
          </div>
        </div>

        <!-- Section: Support log. Not gated on source: every install can be
             asked for its log, which is the whole reason it exists. -->
        <div class="section">
          <div class="section-header" (click)="toggle('logging')">
            <span class="chevron" [class.open]="open['logging']">&#9654;</span>
            {{ 'settings.logging.heading' | translate }}
            <span class="restart-tag">{{ 'settings.tag.restart' | translate }}</span>
          </div>
          <div class="section-body" *ngIf="open['logging']">
            <p class="hint">
              {{ 'settings.logging.hint' | translate }}
            </p>
            <label class="toggle-row">
              <input type="checkbox" [(ngModel)]="config.logging.enabled" name="logging_enabled" />
              {{ 'settings.logging.enabled' | translate }}
            </label>
            <label>
              {{ 'settings.logging.file' | translate }}
              <input type="text" [(ngModel)]="config.logging.file" name="logging_file"
                     [placeholder]="'settings.logging.filePlaceholder' | translate" />
            </label>
            <label>
              {{ 'settings.logging.rotateMb' | translate }}
              <input type="number" [(ngModel)]="config.logging.max_mb" name="logging_max_mb" />
            </label>
            <label>
              {{ 'settings.logging.keep' | translate }}
              <input type="number" [(ngModel)]="config.logging.keep" name="logging_keep" />
            </label>
          </div>
        </div>

        <!-- Section: O2 Ring Oximetry -->
        <div class="section">
          <div class="section-header" (click)="toggle('o2ring')">
            <span class="chevron" [class.open]="open['o2ring']">&#9654;</span>
            {{ 'settings.o2ring.heading' | translate }}
            <span class="badge" *ngIf="!config.o2ring?.enabled">{{ 'settings.tag.optional' | translate }}</span>
          </div>
          <div class="section-body" *ngIf="open['o2ring']">
            <label class="toggle-row">
              {{ 'settings.o2ring.enabled' | translate }}
              <input type="checkbox" [(ngModel)]="config.o2ring.enabled" name="o2ring_enabled" />
            </label>
            <ng-container *ngIf="config.o2ring.enabled">
              <label>
                {{ 'settings.o2ring.mode' | translate }}
                <select [(ngModel)]="config.o2ring.mode" name="o2ring_mode">
                  <option value="http">{{ 'settings.o2ring.modeHttp' | translate }}</option>
                  <option value="ble">{{ 'settings.o2ring.modeBle' | translate }}</option>
                </select>
              </label>
              <label *ngIf="config.o2ring.mode === 'http'">
                {{ 'settings.o2ring.muleUrl' | translate }}
                <input type="text" [(ngModel)]="config.o2ring.mule_url" name="o2ring_mule_url"
                       placeholder="http://192.168.2.74" />
                <span class="hint">{{ 'settings.o2ring.muleHint' | translate }}</span>
              </label>
              <label *ngIf="config.o2ring.mode === 'ble'">
                <span class="hint" *ngIf="bleStatus === 'checking'">{{ 'settings.o2ring.bleChecking' | translate }}</span>
                <span class="hint" *ngIf="bleStatus === 'ok'" style="color: #4ade80;">{{ 'settings.o2ring.bleOk' | translate }}</span>
                <span class="hint" *ngIf="bleStatus === 'no_adapter'" style="color: #ef4444;">{{ 'settings.o2ring.bleNoAdapter' | translate }}</span>
                <span class="hint" *ngIf="bleStatus === 'not_compiled'" style="color: #fb923c;">{{ 'settings.o2ring.bleNotCompiled' | translate }}</span>
                <span class="hint" *ngIf="bleStatus === 'error'" style="color: #ef4444;">{{ 'settings.o2ring.bleError' | translate }}</span>
              </label>
            </ng-container>
          </div>
        </div>

        <!-- Section: AirSense 11 over Bluetooth (SDD-057). The switch and the
             address are config; the pairing is not: it goes through its own
             endpoints and is kept beside config.json, never in it. -->
        <div class="section" *ngIf="config.airsense11_ble">
          <div class="section-header" (click)="toggle('as11')">
            <span class="chevron" [class.open]="open['as11']">&#9654;</span>
            {{ 'settings.as11.heading' | translate }}
            <span class="badge" *ngIf="!as11.paired">{{ 'settings.tag.optional' | translate }}</span>
            <span class="badge connected" *ngIf="as11.paired">{{ 'settings.as11.badgePaired' | translate }}</span>
          </div>
          <div class="section-body" *ngIf="open['as11']">
            <p class="section-desc">{{ 'settings.as11.desc' | translate }}</p>
            <div class="myair-state" *ngIf="!as11.compiled">{{ 'settings.as11.notCompiled' | translate }}</div>
            <label class="toggle-row">
              {{ 'settings.as11.enabled' | translate }}
              <input type="checkbox" [(ngModel)]="config.airsense11_ble.enabled" name="as11_enabled" />
            </label>
            <ng-container *ngIf="config.airsense11_ble.enabled">
              <ng-container *ngIf="as11.compiled">
                <!-- Keyed off the saved config, not the status: the running
                     service picks a save up within a second, so by the time
                     the button is pressed the client exists. -->
                <div class="hint" *ngIf="!as11Saved()">{{ 'settings.as11.saveFirst' | translate }}</div>
                <ng-container *ngIf="as11Saved()">
                  <div class="myair-connected">
                    {{ (as11.paired ? 'settings.as11.paired' : 'settings.as11.notPaired') | translate }}
                  </div>
                  <div class="myair-note">{{ 'settings.as11.pairNote' | translate }}</div>

                  <ng-container *ngIf="!as11.pairing">
                    <button type="button" class="myair-btn" (click)="as11PairStart()" [disabled]="as11Busy">
                      {{ (as11Busy ? 'settings.as11.searching'
                                   : (as11.paired ? 'settings.as11.pairAgain' : 'settings.as11.pair')) | translate }}
                    </button>
                  </ng-container>

                  <ng-container *ngIf="as11.pairing">
                    <label>
                      {{ 'settings.as11.code' | translate }}
                      <input type="text" inputmode="numeric" autocomplete="one-time-code"
                             [(ngModel)]="as11Code" name="as11_code"
                             [ngModelOptions]="{standalone: true}" placeholder="0000" />
                      <span class="hint">{{ 'settings.as11.codeHint' | translate }}</span>
                    </label>
                    <div class="prompt-actions">
                      <button type="button" class="myair-btn" (click)="as11PairConfirm()"
                              [disabled]="as11Busy || !as11Code">
                        {{ (as11Busy ? 'settings.as11.confirming' : 'settings.as11.confirm') | translate }}
                      </button>
                      <button type="button" class="myair-btn secondary" (click)="as11PairCancel()"
                              [disabled]="as11Busy">{{ 'settings.as11.cancel' | translate }}</button>
                    </div>
                  </ng-container>
                </ng-container>
              </ng-container>
            </ng-container>
            <div class="myair-msg" *ngIf="as11Message">{{ as11Message }}</div>
          </div>
        </div>

        <!-- Section: the Mule & Miner units streaming to this host, and their
             firmware. Nothing here is config: the units list themselves when
             they connect, and an update goes to the unit over the local
             network (docs/MULE_LOCAL_STREAM.md). -->
        <div class="section">
          <div class="section-header" (click)="toggle('mule')">
            <span class="chevron" [class.open]="open['mule']">&#9654;</span>
            {{ 'settings.mule.heading' | translate }}
            <span class="badge connected" *ngIf="muleOnline() > 0">
              {{ 'settings.mule.badgeOnline' | translate:{ n: muleOnline() } }}
            </span>
          </div>
          <div class="section-body" *ngIf="open['mule']">
            <p class="section-desc">{{ 'settings.mule.desc' | translate }}</p>
            <div class="hint" *ngIf="muleUnits.length === 0">{{ 'settings.mule.none' | translate }}</div>
            <div class="mule-unit" *ngFor="let u of muleUnits">
              <div class="mule-head">
                <strong>{{ u.serial }}</strong>
                <span class="badge" [class.connected]="u.online">
                  {{ (u.online ? 'settings.mule.online' : 'settings.mule.offline') | translate }}
                </span>
                <span class="badge" *ngIf="u.streaming">{{ 'settings.mule.streaming' | translate }}</span>
                <span class="badge" *ngIf="u.night_landing">{{ 'settings.mule.landing' | translate }}</span>
              </div>
              <div class="mule-line" *ngIf="u.machine">
                <span class="mule-label">{{ 'settings.mule.machine' | translate }}</span> {{ u.machine }}
              </div>
              <div class="mule-line" *ngIf="u.last_block">
                <span class="mule-label">{{ 'settings.mule.lastBlock' | translate }}</span>
                {{ u.last_block * 1000 | date:'medium' }}
              </div>
              <div class="mule-line">
                <span class="mule-label">{{ 'settings.mule.firmware' | translate }}</span>
                {{ 'settings.mule.mule' | translate }} {{ u.fw || '?' }},
                {{ 'settings.mule.miner' | translate }} {{ u.miner_fw || '?' }}
                <ng-container *ngIf="muleFw[u.serial] as f">
                  <span class="mule-avail" *ngIf="f.update_available.mule || f.update_available.miner">
                    {{ 'settings.mule.updateAvailable' | translate:{ mule: f.latest.mule, miner: f.latest.miner } }}
                  </span>
                  <span class="hint inline" *ngIf="!f.update_available.mule && !f.update_available.miner && !f.manifest.error">
                    {{ 'settings.mule.upToDate' | translate }}
                  </span>
                  <span class="hint inline" *ngIf="f.manifest.error">{{ 'settings.mule.manifestUnavailable' | translate }}</span>
                </ng-container>
              </div>
              <!-- The unit's two Bluetooth switches, as its last state reports
                   them. A click posts a config command; the unit relays it to
                   the miner, which restarts, and the next state shows it. -->
              <div class="mule-line mule-switches" *ngIf="u.state">
                <label class="mule-switch" *ngIf="u.state.as11?.enabled !== undefined">
                  <input type="checkbox" [checked]="u.state.as11?.enabled"
                         [disabled]="!u.online || muleBusy[u.serial] || muleSwitchPending(u, 'as11_enabled')"
                         (change)="muleSwitch(u.serial, 'as11_enabled', $any($event.target).checked)" />
                  {{ 'settings.mule.as11Bluetooth' | translate }}
                  <span class="hint inline" *ngIf="muleSwitchState(u, 'as11_enabled') as s">{{ s }}</span>
                </label>
                <label class="mule-switch" *ngIf="u.state.o2_enabled !== undefined">
                  <input type="checkbox" [checked]="u.state.o2_enabled"
                         [disabled]="!u.online || muleBusy[u.serial] || muleSwitchPending(u, 'o2_enabled')"
                         (change)="muleSwitch(u.serial, 'o2_enabled', $any($event.target).checked)" />
                  {{ 'settings.mule.o2Ring' | translate }}
                  <span class="hint inline" *ngIf="muleSwitchState(u, 'o2_enabled') as s">{{ s }}</span>
                </label>
              </div>
              <ng-container *ngIf="muleFw[u.serial] as f">
                <div class="hint" *ngIf="f.night_open">{{ 'settings.mule.nightOpen' | translate }}</div>
                <div class="mule-actions" *ngIf="!f.job?.running">
                  <button type="button" class="myair-btn" (click)="muleUpdate(u.serial)"
                          *ngIf="f.update_available.mule || f.update_available.miner"
                          [disabled]="f.night_open || !u.online || muleBusy[u.serial]">
                    {{ 'settings.mule.update' | translate }}
                  </button>
                  <label class="mule-file">
                    <select [(ngModel)]="muleTarget[u.serial]" [ngModelOptions]="{standalone: true}">
                      <option value="mule">{{ 'settings.mule.mule' | translate }}</option>
                      <option value="miner">{{ 'settings.mule.miner' | translate }}</option>
                    </select>
                    <input type="file" accept=".bin" (change)="muleFile(u.serial, $event)"
                           [disabled]="f.night_open || !u.online || muleBusy[u.serial]" />
                    <span class="hint">{{ 'settings.mule.fileHint' | translate }}</span>
                  </label>
                </div>
                <div class="mule-job" *ngIf="f.job" [class.err]="f.job.phase === 'failed'"
                     [class.ok]="f.job.phase === 'done'">{{ muleJobText(f.job) }}</div>
              </ng-container>
              <div class="myair-msg err" *ngIf="muleMessage[u.serial]">{{ muleMessage[u.serial] }}</div>
            </div>
          </div>
        </div>

        <!-- Section: the Sleep Bridges streaming to this host, and their
             firmware. Nothing here is config: a bridge lists itself when it
             connects, and an image goes to it over its own stream connection
             (docs/SLEEP_BRIDGE_LOCAL_STREAM.md, "Updating the bridge"). -->
        <div class="section">
          <div class="section-header" (click)="toggle('sleepbridge')">
            <span class="chevron" [class.open]="open['sleepbridge']">&#9654;</span>
            {{ 'settings.sleepbridge.heading' | translate }}
            <span class="badge connected" *ngIf="sbOnline() > 0">
              {{ 'settings.sleepbridge.badgeOnline' | translate:{ n: sbOnline() } }}
            </span>
          </div>
          <div class="section-body" *ngIf="open['sleepbridge']">
            <p class="section-desc">{{ 'settings.sleepbridge.desc' | translate }}</p>
            <div class="hint" *ngIf="sbBridges.length === 0">{{ 'settings.sleepbridge.none' | translate }}</div>
            <div class="sb-unit" *ngFor="let b of sbBridges">
              <div class="sb-head">
                <strong>{{ b.sender }}</strong>
                <span class="badge" [class.connected]="b.online">
                  {{ (b.online ? 'settings.sleepbridge.online' : 'settings.sleepbridge.offline') | translate }}
                </span>
                <span class="badge" *ngIf="b.night_open">{{ 'settings.sleepbridge.nightOpen' | translate }}</span>
              </div>
              <div class="sb-line" *ngIf="b.card_machine">
                <span class="sb-label">{{ 'settings.sleepbridge.machine' | translate }}</span> {{ b.card_machine }}
              </div>
              <div class="sb-line" *ngIf="b.last_batch">
                <span class="sb-label">{{ 'settings.sleepbridge.lastWrite' | translate }}</span>
                {{ b.last_batch * 1000 | date:'medium' }}
              </div>
              <div class="sb-line">
                <span class="sb-label">{{ 'settings.sleepbridge.firmware' | translate }}</span> {{ b.fw_version || '?' }}
              </div>
              <div class="hint" *ngIf="b.night_open">{{ 'settings.sleepbridge.nightOpenHint' | translate }}</div>
              <div class="sb-actions" *ngIf="!b.update?.running">
                <label class="sb-file">
                  <input type="file" accept=".bin" (change)="sbFile(b.sender, $event)"
                         [disabled]="b.night_open || !b.online || sbBusy[b.sender]" />
                  <span class="hint">{{ 'settings.sleepbridge.fileHint' | translate }}</span>
                </label>
              </div>
              <div class="sb-job" *ngIf="b.update" [class.err]="b.update.phase === 'failed'"
                   [class.ok]="b.update.phase === 'done'">{{ sbJobText(b.update) }}</div>
              <div class="sb-msg" *ngIf="sbMessage[b.sender]">{{ sbMessage[b.sender] }}</div>
            </div>
          </div>
        </div>

        <!-- Section: SleepHQ Sync -->
        <div class="section">
          <div class="section-header" (click)="toggle('sleephq')">
            <span class="chevron" [class.open]="open['sleephq']">&#9654;</span>
            {{ 'settings.sleephq.heading' | translate }}
            <span class="badge" *ngIf="!config.sleephq.enabled">{{ 'settings.tag.optional' | translate }}</span>
          </div>
          <div class="section-body" *ngIf="open['sleephq']">
            <p class="section-desc">
              {{ 'settings.sleephq.desc' | translate }}
            </p>
            <label class="toggle-row">
              {{ 'settings.sleephq.enabled' | translate }}
              <input type="checkbox" [(ngModel)]="config.sleephq.enabled" name="sleephq_enabled" />
            </label>
            <ng-container *ngIf="config.sleephq.enabled">
              <label>
                {{ 'settings.sleephq.clientId' | translate }}
                <input type="text" [(ngModel)]="config.sleephq.client_id" name="sleephq_client_id"
                       [placeholder]="'settings.sleephq.clientIdPlaceholder' | translate" />
              </label>
              <label>
                {{ 'settings.sleephq.clientSecret' | translate }}
                <input type="password" [(ngModel)]="config.sleephq.client_secret" name="sleephq_client_secret"
                       [placeholder]="'settings.sleephq.clientSecretPlaceholder' | translate" />
              </label>
              <label class="toggle-row">
                <input type="checkbox" [(ngModel)]="config.sleephq.auto_on_session" name="sleephq_auto_session" />
                {{ 'settings.sleephq.autoSession' | translate }}
              </label>
              <label class="toggle-row">
                <input type="checkbox" [(ngModel)]="config.sleephq.auto_on_backfill" name="sleephq_auto_backfill" />
                {{ 'settings.sleephq.autoBackfill' | translate }}
              </label>
              <label>
                {{ 'settings.sleephq.quietMinutes' | translate }}
                <input type="number" [(ngModel)]="config.sleephq.quiet_minutes" name="sleephq_quiet_minutes" min="1" />
                <span class="restart-tag">{{ 'settings.tag.restart' | translate }}</span>
                <small class="hint">
                  {{ 'settings.sleephq.quietHint' | translate }}
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
            {{ 'settings.myair.heading' | translate }}
            <span class="badge" *ngIf="!myair.connected">{{ 'settings.tag.optional' | translate }}</span>
            <span class="badge connected" *ngIf="myair.connected">{{ 'settings.tag.connected' | translate }}</span>
          </div>
          <div class="section-body" *ngIf="open['myair']">
            <p class="section-desc">
              {{ 'settings.myair.desc' | translate }}
            </p>

            <div class="myair-note">
              {{ 'settings.myair.notePre' | translate }}
              <strong>{{ 'settings.myair.noteBold' | translate }}</strong>{{ 'settings.myair.notePost' | translate }}
            </div>

            <div class="myair-state" *ngIf="myair.needs_reauth">
              {{ 'settings.myair.needsReauth' | translate }}
            </div>

            <ng-container *ngIf="!myair.connected && !myair.awaiting_code">
              <label>
                {{ 'settings.myair.email' | translate }}
                <input type="email" [(ngModel)]="myairForm.username" name="myair_username"
                       [ngModelOptions]="{standalone: true}" placeholder="you@example.com" />
              </label>
              <label>
                {{ 'settings.myair.password' | translate }}
                <input type="password" [(ngModel)]="myairForm.password" name="myair_password"
                       [ngModelOptions]="{standalone: true}"
                       [placeholder]="'settings.myair.passwordPlaceholder' | translate" />
              </label>
              <label>
                {{ 'settings.myair.region' | translate }}
                <!-- Australia is served by the North America endpoints, which is
                     not guessable and is why it is named here rather than left
                     to a user in Sydney to work out. Europe is the region with
                     an email verification step, so saying that up front stops
                     the code prompt being a surprise. -->
                <select [(ngModel)]="myairForm.region" name="myair_region"
                        [ngModelOptions]="{standalone: true}">
                  <option value="NA">{{ 'settings.myair.regionNa' | translate }}</option>
                  <option value="EU">{{ 'settings.myair.regionEu' | translate }}</option>
                </select>
              </label>
              <button type="button" class="myair-btn" (click)="myairConnect()"
                      [disabled]="myairBusy || !myairForm.username || !myairForm.password">
                {{ (myairBusy ? 'settings.myair.signingIn' : 'settings.myair.connect') | translate }}
              </button>
            </ng-container>

            <!-- Europe emails a code the first time. After that the remembered
                 device stops it asking. -->
            <ng-container *ngIf="myair.awaiting_code">
              <label>
                {{ 'settings.myair.code' | translate }}
                <input type="text" [(ngModel)]="myairForm.code" name="myair_code"
                       [ngModelOptions]="{standalone: true}"
                       [placeholder]="'settings.myair.codePlaceholder' | translate" />
              </label>
              <button type="button" class="myair-btn" (click)="myairVerify()"
                      [disabled]="myairBusy || !myairForm.code">
                {{ (myairBusy ? 'settings.myair.checking' : 'settings.myair.verify') | translate }}
              </button>
            </ng-container>

            <ng-container *ngIf="myair.connected">
              <div class="myair-connected">
                {{ 'settings.myair.connectedPre' | translate }} <strong>{{ myair.username }}</strong>
                {{ 'settings.myair.connectedPost' | translate:{ region: myair.region, minutes: myair.poll_minutes } }}
              </div>
              <button type="button" class="myair-btn danger" (click)="myairDisconnect()"
                      [disabled]="myairBusy">{{ 'settings.myair.disconnect' | translate }}</button>
              <small class="hint">
                {{ 'settings.myair.disconnectHint' | translate }}
              </small>
            </ng-container>

            <div class="myair-msg" *ngIf="myairMessage">{{ myairMessage }}</div>
          </div>
        </div>

        <!-- Section: CpapDash Cloud (SDD-004 mirror). Local stays authoritative. -->
        <div class="section">
          <div class="section-header" (click)="toggle('cpapdash')">
            <span class="chevron" [class.open]="open['cpapdash']">&#9654;</span>
            {{ 'settings.cloud.heading' | translate }}
            <span class="badge" *ngIf="!config.cpapdash.enabled">{{ 'settings.tag.optional' | translate }}</span>
          </div>
          <div class="section-body" *ngIf="open['cpapdash']">
            <p class="section-desc">
              {{ 'settings.cloud.desc' | translate }}
            </p>
            <label class="toggle-row">
              {{ 'settings.cloud.enabled' | translate }}
              <input type="checkbox" [(ngModel)]="config.cpapdash.enabled" name="cpapdash_enabled" />
            </label>
            <ng-container *ngIf="config.cpapdash.enabled">
              <label>
                {{ 'settings.cloud.apiUrl' | translate }}
                <input type="text" [(ngModel)]="config.cpapdash.api_url" name="cpapdash_api_url"
                       placeholder="https://api.cpapdash.com" />
              </label>
              <label>
                {{ 'settings.cloud.token' | translate }}
                <input type="password" [(ngModel)]="config.cpapdash.token" name="cpapdash_token"
                       [placeholder]="'settings.cloud.tokenPlaceholder' | translate" />
                <small class="hint">
                  {{ 'settings.cloud.tokenHint' | translate }}
                </small>
              </label>
              <label class="toggle-row">
                <input type="checkbox" [(ngModel)]="config.cpapdash.auto_sync" name="cpapdash_auto_sync" />
                {{ 'settings.cloud.autoSync' | translate }}
              </label>
            </ng-container>
          </div>
        </div>

        <!-- Section: Sleep Stage Inference -->
        <div class="section">
          <div class="section-header" (click)="toggle('sleep_stage')">
            <span class="chevron" [class.open]="open['sleep_stage']">&#9654;</span>
            {{ 'settings.sleepStage.heading' | translate }}
            <span class="badge" *ngIf="!config.sleep_stage.enabled">{{ 'settings.tag.optional' | translate }}</span>
            <span class="restart-tag">{{ 'settings.tag.restart' | translate }}</span>
          </div>
          <div class="section-body" *ngIf="open['sleep_stage']">
            <label class="toggle-row">
              {{ 'settings.sleepStage.enabled' | translate }}
              <input type="checkbox" [(ngModel)]="config.sleep_stage.enabled" name="sleep_stage_enabled" />
            </label>
            <ng-container *ngIf="config.sleep_stage.enabled">
              <label class="toggle-row">
                <input type="checkbox" [(ngModel)]="config.sleep_stage.live_inference" name="sleep_stage_live" />
                {{ 'settings.sleepStage.live' | translate }}
              </label>
              <label>
                {{ 'settings.sleepStage.modelDir' | translate }}
                <input type="text" [(ngModel)]="config.sleep_stage.model_dir" name="sleep_stage_model_dir"
                       [placeholder]="'settings.sleepStage.modelDirPlaceholder' | translate" />
              </label>
              <label>
                {{ 'settings.sleepStage.modelVersion' | translate }}
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
            {{ 'settings.agent.heading' | translate }}
            <span class="badge" *ngIf="!config.agent.enabled">{{ 'settings.tag.optional' | translate }}</span>
            <span class="restart-tag">{{ 'settings.tag.restart' | translate }}</span>
          </div>
          <div class="section-body" *ngIf="open['agent']">
            <p class="section-desc" *ngIf="!agentAvailable()">
              {{ 'settings.agent.unavailable' | translate }} {{ agentBlockedReason() }}
            </p>
            <label class="toggle-row">
              {{ 'settings.agent.enabled' | translate }}
              <input type="checkbox" [(ngModel)]="config.agent.enabled" name="agent_enabled"
                     [disabled]="!agentAvailable()" />
            </label>
            <ng-container *ngIf="config.agent.enabled && agentAvailable()">
              <label>
                {{ 'settings.agent.embedModel' | translate }}
                <input type="text" [(ngModel)]="config.agent.embed_model" name="agent_embed_model"
                       placeholder="nomic-embed-text" />
              </label>
              <label>
                {{ 'settings.agent.temperature' | translate }}
                <input type="number" step="0.1" min="0" max="2"
                       [(ngModel)]="config.agent.temperature" name="agent_temperature" />
              </label>
              <label>
                {{ 'settings.agent.maxIterations' | translate }}
                <input type="number" min="1" [(ngModel)]="config.agent.max_iterations" name="agent_max_iterations" />
              </label>
            </ng-container>
          </div>
        </div>

        <!-- Section: Collection Timing -->
        <div class="section">
          <div class="section-header" (click)="toggle('timing')">
            <span class="chevron" [class.open]="open['timing']">&#9654;</span>
            {{ 'settings.timing.heading' | translate }}
          </div>
          <div class="section-body" *ngIf="open['timing']">
            <label>
              {{ 'settings.timing.burstInterval' | translate }}
              <input type="number" [(ngModel)]="config.burst_interval" name="burst_interval" min="1" />
            </label>
          </div>
        </div>

        <!-- Section: Import History -->
        <div class="section" *ngIf="config.local_dir">
          <div class="section-header" (click)="toggle('backfill')">
            <span class="chevron" [class.open]="open['backfill']">&#9654;</span>
            {{ 'settings.backfill.heading' | translate }}
          </div>
          <div class="section-body" *ngIf="open['backfill']">
            <p class="section-desc">
              {{ 'settings.backfill.desc' | translate }}
            </p>
            <div class="backfill-dates">
              <label>
                {{ 'settings.backfill.startDate' | translate }}
                <input type="date" [(ngModel)]="backfillStart" name="backfill_start" />
              </label>
              <label>
                {{ 'settings.backfill.endDate' | translate }}
                <input type="date" [(ngModel)]="backfillEnd" name="backfill_end" />
              </label>
            </div>
            <span class="hint" *ngIf="backfillStart && backfillEnd">
              {{ 'settings.backfill.emptyHint' | translate }}
            </span>

            <!-- Backfill Status -->
            <div class="ml-status" *ngIf="backfillProgress">
              <div class="status-row">
                <span class="status-label">{{ 'settings.status.label' | translate }}</span>
                <!-- The state word itself comes from /api/backfill/status and is
                     the server's own vocabulary, so it stays as sent. -->
                <span class="status-value" [class.status-active]="backfillProgress.status === 'running'">
                  {{ backfillProgress.status }}
                </span>
              </div>
              <div class="status-row" *ngIf="backfillProgress.folders_total > 0">
                <span class="status-label">{{ 'settings.backfill.folders' | translate }}</span>
                <span class="status-value">{{ backfillProgress.folders_done }} / {{ backfillProgress.folders_total }}</span>
              </div>
              <div class="status-row" *ngIf="backfillProgress.sessions_saved > 0 || backfillProgress.sessions_parsed > 0">
                <span class="status-label">{{ 'settings.backfill.sessions' | translate }}</span>
                <span class="status-value">{{ 'settings.backfill.sessionsValue' | translate:{ saved: backfillProgress.sessions_saved, parsed: backfillProgress.sessions_parsed } }}</span>
              </div>
              <div class="status-row" *ngIf="backfillProgress.sessions_deleted > 0">
                <span class="status-label">{{ 'settings.backfill.replaced' | translate }}</span>
                <span class="status-value">{{ 'settings.backfill.replacedValue' | translate:{ count: backfillProgress.sessions_deleted } }}</span>
              </div>
              <div class="status-row" *ngIf="backfillProgress.errors > 0">
                <span class="status-label">{{ 'settings.backfill.errors' | translate }}</span>
                <span class="status-value" style="color: #ef5350;">{{ backfillProgress.errors }}</span>
              </div>
              <div class="status-row" *ngIf="backfillProgress.completed_at">
                <span class="status-label">{{ 'settings.backfill.completed' | translate }}</span>
                <span class="status-value">{{ backfillProgress.completed_at }}</span>
              </div>
            </div>

            <!-- Progress bar -->
            <div class="progress-bar" *ngIf="backfillRunning && backfillProgress?.folders_total > 0">
              <div class="progress-fill"
                   [style.transform]="'scaleX(' + backfillFraction() + ')'">
              </div>
            </div>

            <div class="ml-actions">
              <button type="button" class="btn-train" (click)="startBackfill()" [disabled]="backfillRunning">
                {{ (backfillRunning ? 'settings.backfill.importing' : 'settings.backfill.start') | translate }}
              </button>
            </div>
          </div>
        </div>

        <!-- Section 2: Database -->
        <div class="section">
          <div class="section-header" (click)="toggle('database')">
            <span class="chevron" [class.open]="open['database']">&#9654;</span>
            {{ 'settings.database.heading' | translate }}
          </div>
          <div class="section-body" *ngIf="open['database']">
            <label>
              {{ 'settings.database.type' | translate }}
              <!-- Engine names are product names, not copy. -->
              <select [(ngModel)]="config.database.type" name="db_type">
                <option value="sqlite">SQLite</option>
                <option value="mysql">MySQL</option>
                <option value="postgresql">PostgreSQL</option>
              </select>
            </label>
            <label *ngIf="config.database.type === 'sqlite'">
              {{ 'settings.database.sqlitePath' | translate }}
              <input type="text" [(ngModel)]="config.database.sqlite_path" name="db_sqlite_path" placeholder="/var/lib/hms-cpap/cpap.db" />
            </label>
            <ng-container *ngIf="config.database.type !== 'sqlite'">
              <label>
                {{ 'settings.database.host' | translate }}
                <input type="text" [(ngModel)]="config.database.host" name="db_host" placeholder="localhost" />
              </label>
              <label>
                {{ 'settings.database.port' | translate }}
                <input type="number" [(ngModel)]="config.database.port" name="db_port" />
              </label>
              <label>
                {{ 'settings.database.name' | translate }}
                <input type="text" [(ngModel)]="config.database.name" name="db_name" />
              </label>
              <label>
                {{ 'settings.database.user' | translate }}
                <input type="text" [(ngModel)]="config.database.user" name="db_user" />
              </label>
              <label>
                {{ 'settings.database.password' | translate }}
                <input type="password" [(ngModel)]="config.database.password" name="db_password" />
              </label>
            </ng-container>
          </div>
        </div>

        <!-- Section 3: MQTT -->
        <div class="section">
          <div class="section-header" (click)="toggle('mqtt')">
            <span class="chevron" [class.open]="open['mqtt']">&#9654;</span>
            {{ 'settings.mqtt.heading' | translate }}
            <span class="badge" *ngIf="!config.mqtt.enabled">{{ 'settings.tag.optional' | translate }}</span>
          </div>
          <div class="section-body" *ngIf="open['mqtt']">
            <label class="toggle-row">
              {{ 'settings.mqtt.enabled' | translate }}
              <input type="checkbox" [(ngModel)]="config.mqtt.enabled" name="mqtt_enabled" />
            </label>
            <ng-container *ngIf="config.mqtt.enabled">
              <label>
                {{ 'settings.mqtt.broker' | translate }}
                <input type="text" [(ngModel)]="config.mqtt.broker" name="mqtt_broker" placeholder="127.0.0.1" />
              </label>
              <label>
                {{ 'settings.mqtt.port' | translate }}
                <input type="number" [(ngModel)]="config.mqtt.port" name="mqtt_port" />
              </label>
              <label>
                {{ 'settings.mqtt.username' | translate }}
                <input type="text" [(ngModel)]="config.mqtt.username" name="mqtt_username" />
              </label>
              <label>
                {{ 'settings.mqtt.password' | translate }}
                <input type="password" [(ngModel)]="config.mqtt.password" name="mqtt_password" />
              </label>
              <label>
                {{ 'settings.mqtt.clientId' | translate }}
                <input type="text" [(ngModel)]="config.mqtt.client_id" name="mqtt_client_id"
                       placeholder="hms_cpap" />
                <small class="hint">
                  {{ 'settings.mqtt.clientIdHint' | translate }}
                </small>
              </label>
            </ng-container>
          </div>
        </div>

        <!-- Section 4: LLM Summaries -->
        <div class="section">
          <div class="section-header" (click)="toggle('llm')">
            <span class="chevron" [class.open]="open['llm']">&#9654;</span>
            {{ 'settings.llm.heading' | translate }}
            <span class="badge" *ngIf="!config.llm.enabled">{{ 'settings.tag.optional' | translate }}</span>
          </div>
          <div class="section-body" *ngIf="open['llm']">
            <label class="toggle-row">
              {{ 'settings.llm.enabled' | translate }}
              <input type="checkbox" [(ngModel)]="config.llm.enabled" name="llm_enabled" />
            </label>
            <ng-container *ngIf="config.llm.enabled">
              <label>
                {{ 'settings.llm.provider' | translate }}
                <!-- Gemini and Anthropic are bare vendor names; the other two
                     carry a descriptor that does need translating. -->
                <select [(ngModel)]="config.llm.provider" name="llm_provider">
                  <option value="ollama">{{ 'settings.llm.providerOllama' | translate }}</option>
                  <option value="openai">{{ 'settings.llm.providerOpenai' | translate }}</option>
                  <option value="gemini">Gemini</option>
                  <option value="anthropic">Anthropic</option>
                </select>
                <span class="hint" *ngIf="config.llm.provider === 'openai'">
                  {{ 'settings.llm.openaiHint' | translate }}
                </span>
              </label>
              <label>
                {{ 'settings.llm.endpoint' | translate }}
                <input type="text" [(ngModel)]="config.llm.endpoint" name="llm_endpoint"
                       [placeholder]="'settings.llm.endpointPlaceholder' | translate" />
              </label>
              <label>
                {{ 'settings.llm.model' | translate }}
                <input type="text" [(ngModel)]="config.llm.model" name="llm_model" placeholder="llama3.1:8b" />
              </label>
              <label>
                {{ 'settings.llm.apiKey' | translate }}
                <input type="password" [(ngModel)]="config.llm.api_key" name="llm_api_key"
                       [placeholder]="'settings.llm.apiKeyPlaceholder' | translate" />
              </label>
              <label>
                {{ 'settings.llm.maxTokens' | translate }}
                <input type="number" [(ngModel)]="config.llm.max_tokens" name="llm_max_tokens" min="1" />
                <span class="restart-tag">{{ 'settings.tag.restart' | translate }}</span>
              </label>
              <label>
                {{ 'settings.llm.promptFile' | translate }}
                <input type="text" [(ngModel)]="config.llm.prompt_file" name="llm_prompt_file"
                       [placeholder]="'settings.llm.promptFilePlaceholder' | translate" />
                <span class="restart-tag">{{ 'settings.tag.restart' | translate }}</span>
              </label>
            </ng-container>
          </div>
        </div>

        <!-- Section 5: ML Training -->
        <div class="section">
          <div class="section-header" (click)="toggle('ml_training')">
            <span class="chevron" [class.open]="open['ml_training']">&#9654;</span>
            {{ 'settings.ml.heading' | translate }}
            <span class="badge" *ngIf="!config.ml_training.enabled">{{ 'settings.tag.optional' | translate }}</span>
          </div>
          <div class="section-body" *ngIf="open['ml_training']">
            <label class="toggle-row">
              {{ 'settings.ml.enabled' | translate }}
              <input type="checkbox" [(ngModel)]="config.ml_training.enabled" name="ml_enabled" />
            </label>
            <ng-container *ngIf="config.ml_training.enabled">
              <label>
                {{ 'settings.ml.schedule' | translate }}
                <select [(ngModel)]="config.ml_training.schedule" name="ml_schedule">
                  <option value="daily">{{ 'settings.ml.daily' | translate }}</option>
                  <option value="weekly">{{ 'settings.ml.weekly' | translate }}</option>
                  <option value="monthly">{{ 'settings.ml.monthly' | translate }}</option>
                </select>
              </label>
              <label>
                {{ 'settings.ml.minDays' | translate }}
                <input type="number" [(ngModel)]="config.ml_training.min_days" name="ml_min_days" min="7" />
              </label>
              <label>
                {{ 'settings.ml.maxLookback' | translate }}
                <input type="number" [(ngModel)]="config.ml_training.max_training_days" name="ml_max_days" min="0"
                       [placeholder]="'settings.ml.maxLookbackPlaceholder' | translate" />
                <span class="hint">{{ 'settings.ml.maxLookbackHint' | translate }}</span>
              </label>
              <label>
                {{ 'settings.ml.modelDir' | translate }}
                <input type="text" [(ngModel)]="config.ml_training.model_dir" name="ml_model_dir"
                       placeholder="~/.hms-cpap/models" />
              </label>

              <!-- ML Status -->
              <div class="ml-status" *ngIf="mlStatus">
                <div class="status-row">
                  <span class="status-label">{{ 'settings.status.label' | translate }}</span>
                  <!-- Server's own state word, same as the backfill status above. -->
                  <span class="status-value" [class.status-active]="mlStatus.status === 'training'">
                    {{ mlStatus.status }}
                  </span>
                </div>
                <div class="status-row">
                  <span class="status-label">{{ 'settings.ml.lastTrained' | translate }}</span>
                  <span class="status-value">{{ mlStatus.last_trained || ('settings.status.never' | translate) }}</span>
                </div>
                <div class="status-row" *ngIf="mlStatus.models_loaded">
                  <span class="status-label">{{ 'settings.ml.models' | translate }}</span>
                  <span class="status-value">{{ 'settings.ml.modelsLoaded' | translate:{ count: mlStatus.model_count } }}</span>
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
                  {{ (mlTraining ? 'settings.ml.training' : 'settings.ml.train') | translate }}
                </button>
              </div>
            </ng-container>
          </div>
        </div>

        <!-- Section 6: LLM Prompt -->
        <div class="section">
          <div class="section-header" (click)="toggle('llm_prompt')">
            <span class="chevron" [class.open]="open['llm_prompt']">&#9654;</span>
            {{ 'settings.prompt.heading' | translate }}
          </div>
          <div class="section-body" *ngIf="open['llm_prompt']">
            <label>
              {{ 'settings.prompt.text' | translate }}
              <textarea [(ngModel)]="llmPrompt" name="llm_prompt_text" rows="10"
                        [placeholder]="'common.loading' | translate"></textarea>
            </label>
            <div class="prompt-path" *ngIf="llmPromptPath">{{ 'settings.prompt.file' | translate:{ path: llmPromptPath } }}</div>
            <div class="prompt-actions">
              <button type="button" class="btn-save-prompt" (click)="saveLlmPrompt()" [disabled]="savingPrompt">
                {{ (savingPrompt ? 'settings.prompt.saving' : 'settings.prompt.save') | translate }}
              </button>
              <button type="button" class="btn-reset" (click)="resetLlmPrompt()">{{ 'settings.prompt.reset' | translate }}</button>
            </div>
          </div>
        </div>

        <!-- SDD-041: the detail behind the dashboard's update banner. Not part
             of save(): nothing here is written to config.json. -->
        <div class="section">
          <div class="section-header" (click)="toggle('updates')">
            <span class="chevron" [class.open]="open['updates']">&#9654;</span>
            {{ 'settings.updates.heading' | translate }}
            <span class="badge" *ngIf="update?.available">{{ 'settings.updates.newBadge' | translate }}</span>
          </div>
          <div class="section-body" *ngIf="open['updates']">
            <p class="section-desc" *ngIf="update?.containerised">
              {{ 'settings.updates.containerised' | translate }}
            </p>
            <ng-container *ngIf="update && !update.containerised">
              <p class="section-desc">
                {{ 'settings.updates.running' | translate:{ current: update.current } }}
                <ng-container *ngIf="update.latest">
                  &middot; {{ 'settings.updates.latest' | translate:{ latest: update.latest } }}
                </ng-container>
              </p>
              <p class="section-desc" *ngIf="update.checked && !update.available && !update.error">
                {{ 'settings.updates.current' | translate }}
              </p>
              <p class="section-desc" *ngIf="update.available">
                {{ 'settings.updates.available' | translate:{ latest: update.latest } }}
                <a *ngIf="update.release_url" [href]="update.release_url" target="_blank" rel="noopener">
                  {{ 'settings.updates.releasePage' | translate }}</a>
              </p>
              <pre class="update-notes" *ngIf="update.available && update.notes">{{ update.notes }}</pre>
              <p class="hint" *ngIf="update.error">
                {{ 'settings.updates.lastError' | translate:{ error: update.error } }}
              </p>
              <p class="hint" *ngIf="update.checked_at">
                {{ 'settings.updates.checkedAt' | translate:{ when: (update.checked_at | date:'medium') } }}
              </p>

              <!-- The last install attempt, as its helper reported it. -->
              <p class="section-desc" *ngIf="update.last_result?.present && update.last_result.ok">
                {{ 'settings.updates.lastOk' | translate:{ version: update.last_result.version,
                   when: (update.last_result.at | date:'medium') } }}
              </p>
              <p class="hint warn" *ngIf="update.last_result?.present && !update.last_result.ok">
                {{ 'settings.updates.lastFailed' | translate:{ version: update.last_result.version,
                   step: update.last_result.step, message: update.last_result.message } }}
              </p>

              <!-- D6: who owns a rollback of the data. -->
              <p class="hint" *ngIf="update.can_apply">
                {{ (update.database === 'sqlite' ? 'settings.updates.backupSqlite'
                                                 : 'settings.updates.backupServer')
                   | translate:{ database: update.database } }}
              </p>
              <p class="hint" *ngIf="update.available && !update.installer">
                {{ 'settings.updates.manual' | translate }}
              </p>

              <p class="section-desc" *ngIf="updateFlow.phase !== 'idle'">
                <ng-container [ngSwitch]="updateFlow.phase">
                  <span *ngSwitchCase="'applying'">{{ ('updateBanner.step.' + (updateFlow.step || 'downloading')) | translate:{ latest: update.latest } }}</span>
                  <span *ngSwitchCase="'restarting'">{{ 'updateBanner.restarting' | translate:{ latest: update.latest } }}</span>
                  <span *ngSwitchCase="'busy'">{{ 'updateBanner.busy' | translate }}</span>
                  <span *ngSwitchCase="'failed'">{{ 'updateBanner.failed' | translate:{ error: updateFlow.error } }}</span>
                </ng-container>
              </p>

              <div class="update-actions">
                <button type="button" class="btn-train" (click)="checkForUpdate()"
                        [disabled]="updateChecking || updateFlow.phase === 'applying' || updateFlow.phase === 'restarting'">
                  {{ (updateChecking ? 'settings.updates.checking' : 'settings.updates.checkNow') | translate }}
                </button>
                <button type="button" class="btn-train" *ngIf="update.can_apply && updateFlow.phase === 'idle'"
                        (click)="updateFlow.apply(update)">
                  {{ 'settings.updates.apply' | translate:{ latest: update.latest } }}
                </button>
                <button type="button" class="btn-train" *ngIf="updateFlow.phase === 'busy'"
                        (click)="updateFlow.apply(update, true)">
                  {{ 'updateBanner.applyNow' | translate }}
                </button>
              </div>

              <!-- D1 opt-in. Saved with the form; off by default. -->
              <label class="toggle-row" *ngIf="update.installer">
                <input type="checkbox" [(ngModel)]="config.auto_update" name="auto_update" />
                {{ 'settings.updates.auto' | translate }}
              </label>
              <p class="hint" *ngIf="update.installer">{{ 'settings.updates.autoHint' | translate }}</p>
            </ng-container>
          </div>
        </div>

        <!-- Section 7: Device -->
        <div class="section">
          <div class="section-header" (click)="toggle('device')">
            <span class="chevron" [class.open]="open['device']">&#9654;</span>
            {{ 'settings.device.heading' | translate }}
            <span class="restart-tag">{{ 'settings.tag.restart' | translate }}</span>
          </div>
          <div class="section-body" *ngIf="open['device']">
            <label>
              {{ 'settings.device.id' | translate }}
              <input type="text" [(ngModel)]="config.device_id" name="device_id" placeholder="23243570851" />
            </label>
            <label>
              {{ 'settings.device.name' | translate }}
              <input type="text" [(ngModel)]="config.device_name" name="device_name" placeholder="ResMed AirSense 11" />
            </label>
          </div>
        </div>

        <!-- Section: Advanced. Both of these are resolved once at startup and
             both can make the app unreachable, so they sit last and warn. -->
        <div class="section">
          <div class="section-header" (click)="toggle('advanced')">
            <span class="chevron" [class.open]="open['advanced']">&#9654;</span>
            {{ 'settings.advanced.heading' | translate }}
            <span class="restart-tag">{{ 'settings.tag.restart' | translate }}</span>
          </div>
          <div class="section-body" *ngIf="open['advanced']">
            <label>
              {{ 'settings.advanced.webPort' | translate }}
              <input type="number" [(ngModel)]="config.web_port" name="web_port" min="1" max="65535" />
              <small class="hint warn">
                {{ 'settings.advanced.webPortHint' | translate }}
              </small>
            </label>
            <label>
              {{ 'settings.advanced.staticDir' | translate }}
              <input type="text" [(ngModel)]="config.static_dir" name="static_dir"
                     (focus)="confirmStaticDir()" [readonly]="!staticDirUnlocked" />
              <small class="hint warn" *ngIf="!staticDirUnlocked">
                {{ 'settings.advanced.staticLocked' | translate }}
              </small>
              <small class="hint" *ngIf="staticDirUnlocked">
                {{ 'settings.advanced.staticHint' | translate }}
              </small>
            </label>
          </div>
        </div>

        <div class="actions">
          <button type="submit" class="btn-save" [disabled]="saving">
            {{ (saving ? 'settings.actions.saving' : 'settings.actions.save') | translate }}
          </button>
        </div>
      </form>

      <!-- SDD-012: saved is not the same as in effect. Everything absent from
           BurstCollectorService's ConfigSnapshot only lands on a restart, and
           until now the page said "saved" and left it at that. -->
      <div class="restart-banner" *ngIf="restartPending">
        <div class="restart-text">
          <strong>{{ 'settings.restart.banner' | translate }}</strong>
          {{ restartReason }}
          <span *ngIf="portMoved"> {{ 'settings.restart.portMove' | translate:{ port: config?.web_port } }}</span>
        </div>
        <div class="restart-actions">
          <button type="button" class="btn-save" (click)="restartNow()" [disabled]="restarting">
            {{ (restarting ? 'settings.restart.restarting' : 'settings.restart.now') | translate }}
          </button>
          <button type="button" class="btn-link" (click)="dismissRestart()" [disabled]="restarting">
            {{ 'settings.restart.later' | translate }}
          </button>
        </div>
      </div>
      <div class="restart-banner manual" *ngIf="restartManual">
        <div class="restart-text">
          <strong>{{ 'settings.restart.manual' | translate }}</strong>
          {{ 'settings.restart.manualBody' | translate }}
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
    .myair-msg.err { color: #f87171; }
    .mule-unit { border: 1px solid #2a2d45; border-radius: 8px; padding: 0.7rem 0.9rem; margin-bottom: 0.7rem; }
    .mule-head { display: flex; align-items: center; gap: 0.5rem; flex-wrap: wrap; margin-bottom: 0.4rem; }
    .mule-line { color: #ddd; font-size: 0.85rem; margin: 0.2rem 0; }
    .mule-label { color: #888; margin-right: 0.3rem; }
    .mule-avail { color: #fbbf24; margin-left: 0.5rem; }
    .mule-switches { display: flex; gap: 1.2rem; flex-wrap: wrap; }
    .mule-switch { display: flex; align-items: center; gap: 0.4rem; cursor: pointer; }
    .mule-switch input { margin: 0; }
    .hint.inline { display: inline; margin-left: 0.5rem; }
    .mule-actions { display: flex; align-items: center; gap: 0.8rem; flex-wrap: wrap; margin-top: 0.5rem; }
    .mule-file { display: flex; align-items: center; gap: 0.5rem; flex-wrap: wrap; font-size: 0.8rem; }
    .mule-file select { padding: 0.3rem; }
    .mule-job { font-size: 0.85rem; margin-top: 0.5rem; color: #9ad0a0; }
    .mule-job.err { color: #f87171; }
    .mule-job.ok { color: #4ade80; }
    .myair-btn.danger { background: transparent; border-color: #f87171; color: #f87171; }
    .myair-btn.secondary { background: transparent; border-color: #555; color: #aaa; }
    .myair-btn.secondary:hover:not(:disabled) { color: #e0e0e0; border-color: #888; }
    .sb-unit { border: 1px solid #2a2d45; border-radius: 8px; padding: 0.7rem 0.9rem; margin-bottom: 0.7rem; }
    .sb-head { display: flex; align-items: center; gap: 0.5rem; flex-wrap: wrap; margin-bottom: 0.4rem; }
    .sb-line { color: #ddd; font-size: 0.85rem; margin: 0.2rem 0; }
    .sb-label { color: #888; margin-right: 0.3rem; }
    .sb-actions { display: flex; align-items: center; gap: 0.8rem; flex-wrap: wrap; margin-top: 0.5rem; }
    .sb-file { display: flex; align-items: center; gap: 0.5rem; flex-wrap: wrap; font-size: 0.8rem; }
    .sb-job { font-size: 0.85rem; margin-top: 0.5rem; color: #9ad0a0; }
    .sb-job.err { color: #f87171; }
    .sb-job.ok { color: #4ade80; }
    .sb-msg { font-size: 0.85rem; margin-top: 0.4rem; color: #f87171; }

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
    /* Scaled rather than resized: animating width relayouts the bar on every
       poll tick, and transform stays on the compositor. The origin has to be
       explicit or the fill grows from its centre in both directions. */
    .progress-fill {
      width: 100%; height: 100%; background: #64b5f6; border-radius: 3px;
      transform-origin: left center;
      transition: transform 0.3s ease;
    }
    .update-actions { display: flex; gap: 0.5rem; flex-wrap: wrap; margin: 0.25rem 0 0.75rem; }
    .update-notes {
      white-space: pre-wrap; font-family: inherit; color: #bbb; font-size: 0.8rem; background: #16161f;
      border: 1px solid #333; border-radius: 6px; padding: 0.6rem 0.75rem;
      max-height: 14rem; overflow: auto; margin: 0 0 0.75rem;
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

  // SDD-057 AirSense 11 pairing. Held apart from `config`: the code goes to
  // its own endpoint and the pairing it yields is never part of config.json.
  as11 = { compiled: false, enabled: false, available: false, paired: false, pairing: false };
  as11Code = '';
  as11Busy = false;
  as11Message = '';

  // SDD-041: the last update check, and whether "check now" is in flight.
  update: UpdateStatus | null = null;
  updateChecking = false;

  // The Mule & Miner units streaming here, and each one's firmware answer.
  // Polled while the section is open, so a running update shows as it goes.
  muleUnits: MuleUnit[] = [];
  muleFw: Record<string, MuleFirmware> = {};
  muleTarget: Record<string, 'mule' | 'miner'> = {};
  muleBusy: Record<string, boolean> = {};
  muleMessage: Record<string, string> = {};
  private muleTimer: any = null;

  // The Sleep Bridges streaming here, with each one's firmware and the
  // update as it runs. Polled while the section is open.
  sbBridges: SleepBridge[] = [];
  sbBusy: Record<string, boolean> = {};
  sbMessage: Record<string, string> = {};
  private sbTimer: any = null;

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

  // ── SDD-041 updates ──────────────────────────────────────────────────────

  loadUpdateStatus(): void {
    this.api.getUpdateStatus().subscribe({
      next: (s) => { this.update = s; },
      error: () => { this.update = null; },
    });
  }

  checkForUpdate(): void {
    this.updateChecking = true;
    this.api.checkForUpdate().subscribe({
      next: (s) => { this.update = s; this.updateChecking = false; },
      error: () => { this.updateChecking = false; },
    });
  }

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
          this.myairMessage = e?.error?.error || this.t.instant('settings.myair.errConnect');
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
        this.myairMessage = e?.error?.error || this.t.instant('settings.myair.errVerify');
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
        this.myairMessage = this.t.instant('settings.myair.errDisconnect');
      },
    });
  }

  // ── Mule & Miner units and their firmware ──────────────────────────────

  loadMule(): void {
    this.api.getMuleUnits().subscribe({
      next: (r) => {
        this.muleUnits = r?.units ?? [];
        for (const u of this.muleUnits) {
          if (!this.muleTarget[u.serial]) this.muleTarget[u.serial] = 'mule';
          this.api.getMuleFirmware(u.serial).subscribe({
            next: (f) => { this.muleFw[u.serial] = f; },
            error: () => {},
          });
        }
      },
      error: () => {},
    });
  }

  muleOnline(): number {
    return this.muleUnits.filter((u) => u.online).length;
  }

  // ── the unit's Bluetooth switches ──

  /** The newest config command that set this switch, if any. */
  private muleSwitchCommand(u: MuleUnit, key: 'as11_enabled' | 'o2_enabled'): MuleCommand | null {
    for (const c of u.commands ?? [])
      if (c.cmd === 'config' && c.args?.['config']?.[key] !== undefined) return c;
    return null;
  }

  /** True while a switch is queued or sent and not answered yet. */
  muleSwitchPending(u: MuleUnit, key: 'as11_enabled' | 'o2_enabled'): boolean {
    const c = this.muleSwitchCommand(u, key);
    return !!c && c.state !== 'answered';
  }

  /**
   * The switch's last command as text: pending (queued or sent), answered,
   * or refused with the unit's reason. Nothing once the unit's state shows
   * what was asked.
   */
  muleSwitchState(u: MuleUnit, key: 'as11_enabled' | 'o2_enabled'): string {
    const c = this.muleSwitchCommand(u, key);
    if (!c) return '';
    const asked = c.args['config'][key] as boolean;
    if (c.state === 'queued') return this.t.instant('settings.mule.switchQueued');
    if (c.state === 'sent') return this.t.instant('settings.mule.switchSent');
    if (c.result && c.result.ok === false)
      return this.t.instant('settings.mule.switchRefused', { error: c.result.error || '' });
    const now = key === 'as11_enabled' ? u.state?.as11?.enabled : u.state?.o2_enabled;
    return now === asked ? '' : this.t.instant('settings.mule.switchAnswered');
  }

  /** One switch, as the unit's own config takes it: {"as11_enabled": B} or {"o2_enabled": B}. */
  muleSwitch(serial: string, key: 'as11_enabled' | 'o2_enabled', on: boolean): void {
    this.muleBusy[serial] = true;
    this.muleMessage[serial] = '';
    this.api.sendMuleCommand(serial, 'config', { config: { [key]: on } }).subscribe({
      next: () => { this.muleBusy[serial] = false; this.loadMule(); },
      error: (e) => { this.muleBusy[serial] = false; this.muleMessage[serial] = this.muleError(e); this.loadMule(); },
    });
  }

  /** The update from the manifest: the mule first, then the miner. */
  muleUpdate(serial: string): void {
    this.muleBusy[serial] = true;
    this.muleMessage[serial] = '';
    this.api.startMuleUpdate(serial, 'both').subscribe({
      next: () => { this.muleBusy[serial] = false; this.loadMule(); },
      error: (e) => { this.muleBusy[serial] = false; this.muleMessage[serial] = this.muleError(e); },
    });
  }

  /** An image file the owner picked: it goes at once, to the target chosen. */
  muleFile(serial: string, ev: Event): void {
    const input = ev.target as HTMLInputElement;
    const file = input.files?.[0];
    if (!file) return;
    input.value = '';
    const target = this.muleTarget[serial] || 'mule';
    this.muleBusy[serial] = true;
    this.muleMessage[serial] = '';
    this.api.uploadMuleImage(serial, target, file).subscribe({
      next: () => { this.muleBusy[serial] = false; this.loadMule(); },
      error: (e) => { this.muleBusy[serial] = false; this.muleMessage[serial] = this.muleError(e); },
    });
  }

  private muleError(e: any): string {
    return e?.error?.error || e?.message || this.t.instant('settings.mule.errGeneric');
  }

  muleJobText(job: MuleUpdateJob): string {
    const which = this.t.instant(job.step === 'miner' ? 'settings.mule.miner' : 'settings.mule.mule');
    const pct = job.total > 0 ? Math.round((job.sent * 100) / job.total) : 0;
    switch (job.phase) {
      case 'downloading': return this.t.instant('settings.mule.jobDownloading', { which });
      case 'sending': return this.t.instant('settings.mule.jobSending', { which, pct });
      case 'waiting': return this.t.instant('settings.mule.jobWaiting');
      case 'done': return this.t.instant('settings.mule.jobDone',
        { mule: job.fw_after || '?', miner: job.miner_fw_after || '?' });
      case 'failed': return this.t.instant('settings.mule.jobFailed', { error: job.error });
      default: return job.phase;
    }
  }

  // ── SDD-057 AirSense 11 over Bluetooth ──────────────────────────────────

  loadAs11Status(): void {
    this.api.getAs11BleStatus().subscribe({
      next: (s) => { this.as11 = { ...this.as11, ...s }; },
      error: () => {},
    });
  }

  /** Bluetooth reading is on in the config the server last accepted. */
  as11Saved(): boolean {
    return !!this.pristine?.airsense11_ble?.enabled;
  }

  /** The translated sentence for a failed step, from the stable `result` code. */
  private as11Error(e: any): string {
    const result = e?.error?.result ?? e?.result ?? '';
    const key: Record<string, string> = {
      connect: 'settings.as11.errConnect',
      radio_off: 'settings.as11.errRadioOff',
      verify: 'settings.as11.errVerify',
      protocol: 'settings.as11.errTimeout',
      bad_code: 'settings.as11.errBadCode',
      no_pairing: 'settings.as11.errNoPairing',
      disabled: 'settings.as11.errDisabled',
      not_compiled: 'settings.as11.notCompiled',
    };
    return this.t.instant(key[result] ?? 'settings.as11.errGeneric');
  }

  as11PairStart(): void {
    this.as11Busy = true;
    this.as11Message = '';
    this.as11Code = '';
    this.api.as11PairStart().subscribe({
      next: (r: any) => {
        this.as11Busy = false;
        if (!r?.ok) this.as11Message = this.as11Error(r);
        this.loadAs11Status();
      },
      error: (e) => {
        this.as11Busy = false;
        this.as11Message = this.as11Error(e);
        this.loadAs11Status();
      },
    });
  }

  as11PairConfirm(): void {
    this.as11Busy = true;
    this.as11Message = '';
    const code = this.as11Code;
    this.as11Code = '';
    this.api.as11PairConfirm(code).subscribe({
      next: (r: any) => {
        this.as11Busy = false;
        this.as11Message = r?.ok ? this.t.instant('settings.as11.done') : this.as11Error(r);
        this.loadAs11Status();
      },
      error: (e) => {
        this.as11Busy = false;
        this.as11Message = this.as11Error(e);
        this.loadAs11Status();
      },
    });
  }

  as11PairCancel(): void {
    this.as11Busy = true;
    this.as11Message = '';
    this.as11Code = '';
    this.api.as11PairCancel().subscribe({
      next: () => { this.as11Busy = false; this.loadAs11Status(); },
      error: () => { this.as11Busy = false; this.loadAs11Status(); },
    });
  }

  open: Record<string, boolean> = {
    source: true,
    logging: false,
    o2ring: false,
    as11: false,
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
    updates: false,
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
   *
   * The second element is a translation key, not a label: the reason sentence
   * is assembled at render time so a language can name its own settings.
   */
  private static readonly RESTART_KEYS: ReadonlyArray<[string, string]> = [
    ['web_port', 'settings.restart.key.webPort'],
    ['static_dir', 'settings.restart.key.staticDir'],
    ['ezshare_range', 'settings.restart.key.ezshareRange'],
    ['llm.max_tokens', 'settings.restart.key.llmMaxTokens'],
    ['llm.prompt_file', 'settings.restart.key.llmPromptFile'],
    ['mqtt.client_id', 'settings.restart.key.mqttClientId'],
    ['sleephq', 'settings.restart.key.sleephq'],
    ['agent', 'settings.restart.key.agent'],
    ['sleep_stage', 'settings.restart.key.sleepStage'],
    ['logging', 'settings.restart.key.logging'],
    ['database', 'settings.restart.key.database'],
    // SDD-042: the id every night is stored and read under. A live switch moved
    // the collector alone and left the pages reading the old one (ticket 129).
    ['device_id', 'settings.restart.key.deviceId'],
  ];

  constructor(private api: CpapApiService, private t: TranslateService,
              public updateFlow: UpdateFlowService) {}

  // ── Sleep Bridges and their firmware ────────────────────────────────────

  loadSleepBridges(): void {
    this.api.getSleepBridges().subscribe({
      next: (r) => { this.sbBridges = r?.bridges ?? []; },
      error: () => {},
    });
  }

  sbOnline(): number {
    return this.sbBridges.filter((b) => b.online).length;
  }

  /** An image file the owner picked: it goes at once, over the bridge's connection. */
  sbFile(sender: string, ev: Event): void {
    const input = ev.target as HTMLInputElement;
    const file = input.files?.[0];
    if (!file) return;
    input.value = '';
    this.sbBusy[sender] = true;
    this.sbMessage[sender] = '';
    this.api.uploadSleepBridgeImage(sender, file).subscribe({
      next: () => { this.sbBusy[sender] = false; this.loadSleepBridges(); },
      error: (e) => {
        this.sbBusy[sender] = false;
        this.sbMessage[sender] = e?.error?.error || e?.message || this.t.instant('settings.sleepbridge.errGeneric');
      },
    });
  }

  sbJobText(job: SleepBridgeUpdate): string {
    const pct = job.total > 0 ? Math.round((job.sent * 100) / job.total) : 0;
    switch (job.phase) {
      case 'sending': return this.t.instant('settings.sleepbridge.jobSending', { pct });
      case 'waiting': return this.t.instant('settings.sleepbridge.jobWaiting');
      case 'restarting': return this.t.instant('settings.sleepbridge.jobRestarting');
      case 'done': return this.t.instant('settings.sleepbridge.jobDone', { version: job.fw_after || '?' });
      case 'failed': return this.t.instant('settings.sleepbridge.jobFailed', { error: job.error });
      default: return job.phase;
    }
  }

  ngOnDestroy(): void {
    this.destroy$.next();
    this.destroy$.complete();
    if (this.muleTimer) clearInterval(this.muleTimer);
    if (this.sbTimer) clearInterval(this.sbTimer);
  }

  ngOnInit(): void {
    this.loadMyAirStatus();
    this.loadAs11Status();
    this.loadUpdateStatus();
    this.loadMule();
    this.muleTimer = setInterval(() => { if (this.open['mule']) this.loadMule(); }, 3000);
    this.loadSleepBridges();
    this.sbTimer = setInterval(() => { if (this.open['sleepbridge']) this.loadSleepBridges(); }, 3000);

    this.api.getConfig().subscribe({
      next: (cfg) => {
        // Ensure ml_training exists with defaults
        if (!cfg.ml_training) {
          cfg.ml_training = { enabled: false, schedule: 'weekly', model_dir: '', min_days: 30, max_training_days: 0 };
        }
        if (!cfg.o2ring) {
          cfg.o2ring = { enabled: false, mode: 'http', mule_url: '' };
        }
        if (!cfg.airsense11_ble) {
          cfg.airsense11_ble = { enabled: false, address: '' };
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
        this.error = this.t.instant('settings.loadError');
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
        this.showToast(this.t.instant('settings.actions.saved'), false);
        this.evaluateRestart(submitted);
        this.pristine = submitted;
        this.loadAs11Status();
      },
      error: () => {
        this.saving = false;
        this.showToast(this.t.instant('settings.actions.saveFailed'), true);
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

    const labels = changed.map((k) => this.t.instant(k));
    const list = labels.length === 1
      ? labels[0]
      : labels.slice(0, -1).join(', ') + ' ' + this.t.instant('settings.restart.and') +
        ' ' + labels[labels.length - 1];
    this.restartReason = this.t.instant('settings.restart.reason', { list });
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
        this.showToast(this.t.instant('settings.restart.requestFailed'), true);
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
          ? this.t.instant('settings.restart.noAnswer', { origin })
          : this.t.instant('settings.restart.noComeback'),
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
    this.staticDirUnlocked = window.confirm(this.t.instant('settings.advanced.confirm'));
  }

  /**
   * Does the selected source download files that have to be written somewhere?
   *
   * Mirrors PreflightService::sourceNeedsArchive. local and lowenstein read
   * files already on disk; ezShare and FlashAir receive them over the network.
   */
  sourceNeedsArchive(): boolean {
    // The transport, which the picker above edits; the legacy `source` only
    // catches up on save. SDD-031: a Sefam card behind an ez Share is copied
    // into this folder, so it needs one exactly as a ResMed card does.
    const t = this.config?.transport ?? this.config?.source;
    return t === 'ezshare' || t === 'flashair';
  }

  /** SDD-031: an ez Share reads ResMed or Sefam; any other format goes back to
   *  ResMed. SDD-056: a FlashAir reads ResMed only. */
  onTransportChange(transport: string): void {
    if (!this.config) return;
    if (transport === 'ezshare' && !['resmed', 'sefam'].includes(this.config.format)) {
      this.config.format = 'resmed';
    }
    if (transport === 'flashair') this.config.format = 'resmed';
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
    if (!this.config.llm?.enabled) missing.push(this.t.instant('settings.agent.blockedLlm'));
    if (this.config.database?.type !== 'postgresql') missing.push(this.t.instant('settings.agent.blockedDb'));
    if (!missing.length) return '';
    const list = missing.join(` ${this.t.instant('settings.restart.and')} `);
    return this.t.instant('settings.agent.blocked', { list });
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
        this.showToast(this.t.instant('settings.ml.started'), false);
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
              this.showToast(this.t.instant('settings.ml.complete'), false);
            }
          },
        });
      },
      error: () => {
        this.mlTraining = false;
        this.showToast(this.t.instant('settings.ml.failed'), true);
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

  /**
   * Progress as a 0..1 scale factor for the bar's transform.
   *
   * Guards its own divisor rather than leaning on the template's *ngIf: a
   * scaleX(NaN) silently renders nothing, which would read as "no progress"
   * instead of as the bug it is.
   */
  backfillFraction(): number {
    const total = this.backfillProgress?.folders_total ?? 0;
    if (total <= 0) return 0;
    const done = this.backfillProgress?.folders_done ?? 0;
    return Math.min(1, Math.max(0, done / total));
  }

  startBackfill(): void {
    this.backfillRunning = true;
    this.backfillProgress = null;
    const start = this.backfillStart || undefined;
    const end = this.backfillEnd || undefined;
    this.api.triggerBackfill(start, end).subscribe({
      next: () => {
        this.showToast(this.t.instant('settings.backfill.started'), false);
        this.pollBackfillStatus();
      },
      error: () => {
        this.backfillRunning = false;
        this.showToast(this.t.instant('settings.backfill.startFailed'), true);
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
            this.t.instant('settings.backfill.complete', { count: status.sessions_saved }), false);
        } else if (status?.status === 'error') {
          this.showToast(this.t.instant('settings.backfill.failed', {
            error: status.error_message || this.t.instant('settings.backfill.unknownError'),
          }), true);
        }
      },
    });
  }

  saveLlmPrompt(): void {
    this.savingPrompt = true;
    this.api.updateLlmPrompt(this.llmPrompt).subscribe({
      next: () => {
        this.savingPrompt = false;
        this.showToast(this.t.instant('settings.prompt.saved'), false);
      },
      error: () => {
        this.savingPrompt = false;
        this.showToast(this.t.instant('settings.prompt.saveFailed'), true);
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
