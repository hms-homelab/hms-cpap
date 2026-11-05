// Sending a Sleep Bridge back to the CpapDash cloud, as plain data, so it is
// tested without a browser (sleepbridge-cloud.spec.ts, run with `npm test`).
//
// One bridge at a time: the owner presses the button, confirms on the page
// (no blocking dialog), and the `stream` command with an empty url goes to
// that bridge (docs/SLEEP_BRIDGE_LOCAL_STREAM.md, "Commands"). The bridge
// answers, keeps the cloud and restarts, so it drops off this list as offline
// soon after; the outcome of the request stays shown under it.

/** idle, confirming (the in-page question is open), sending, sent, failed */
export type CloudPhase = 'idle' | 'confirming' | 'sending' | 'sent' | 'failed';

export interface CloudState {
  phase: CloudPhase;
  error: string;
}

/** The fields of a listed bridge this needs. */
export interface CloudBridge {
  sender: string;
  online: boolean;
}

/** The body of POST /api/sleepbridge/command that sends one bridge to the cloud. */
export function cloudCommand(sender: string): { sender: string; cmd: 'stream'; url: '' } {
  return { sender, cmd: 'stream', url: '' };
}

/** The error a failed request carries: the server's own words when it gave any. */
export function cloudError(e: any): string {
  return e?.error?.error || e?.message || '';
}

export class BridgeCloud {
  private states: Record<string, CloudState> = {};

  state(sender: string): CloudState {
    return this.states[sender] ?? { phase: 'idle', error: '' };
  }

  phase(sender: string): CloudPhase {
    return this.state(sender).phase;
  }

  /** The button is pressable: the bridge is online and nothing is in flight for it. */
  canAsk(b: CloudBridge): boolean {
    const p = this.phase(b.sender);
    return b.online && p !== 'confirming' && p !== 'sending';
  }

  /** Open the in-page confirmation. Ignored for an offline bridge. */
  ask(b: CloudBridge): void {
    if (!this.canAsk(b)) return;
    this.states[b.sender] = { phase: 'confirming', error: '' };
  }

  cancel(sender: string): void {
    if (this.phase(sender) === 'confirming') delete this.states[sender];
  }

  /**
   * The owner confirmed: returns the request body to send, or null when there
   * is nothing to send (not confirming, or the bridge went offline meanwhile).
   */
  confirm(b: CloudBridge): ReturnType<typeof cloudCommand> | null {
    if (this.phase(b.sender) !== 'confirming') return null;
    if (!b.online) {
      delete this.states[b.sender];
      return null;
    }
    this.states[b.sender] = { phase: 'sending', error: '' };
    return cloudCommand(b.sender);
  }

  sent(sender: string): void {
    this.states[sender] = { phase: 'sent', error: '' };
  }

  failed(sender: string, error: string): void {
    this.states[sender] = { phase: 'failed', error };
  }
}
