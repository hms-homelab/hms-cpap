import { NgZone } from '@angular/core';
import { WatchFrame, parseWatchFrame, watchUrl } from '../pages/setup/bridge-setup';

/**
 * SDD-073: the watch socket (api/sleepbridge/watch), shared by the setup
 * steps and Settings. The server pushes a frame on every bridge event (a
 * hello, a card statement, a rebuild, a close) and on every CpapDash stream
 * state; nothing here asks on a timer. A closed socket is reported once and
 * never reopened by itself: the caller offers a Reconnect the person clicks.
 */
export class SleepBridgeWatch {
  private ws: WebSocket | null = null;

  constructor(private zone: NgZone,
              private onFrame: (f: WatchFrame) => void,
              private onClosed: () => void) {}

  get open(): boolean { return !!this.ws; }

  connect(): void {
    this.close();
    let ws: WebSocket;
    try {
      ws = new WebSocket(watchUrl(document.baseURI));
    } catch {
      this.onClosed();
      return;
    }
    this.ws = ws;
    ws.onmessage = (m) => this.zone.run(() => {
      const f = typeof m.data === 'string' ? parseWatchFrame(m.data) : null;
      if (f) this.onFrame(f);
    });
    ws.onclose = () => this.zone.run(() => {
      if (this.ws !== ws) return;
      this.ws = null;
      this.onClosed();
    });
  }

  close(): void {
    const ws = this.ws;
    this.ws = null;
    if (!ws) return;
    ws.onclose = null;
    ws.onmessage = null;
    ws.close();
  }
}
