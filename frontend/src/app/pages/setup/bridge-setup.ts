// SDD-073 D9: the web wizard's Sleep Bridge steps as data. Pure, like
// setup-source.ts, so it is tested with node --test and no Angular.
//
// Every wait here is for an EVENT the server pushes on the watch socket (a
// bridge's hello, its card statement, the CpapDash stream's state), never a
// clock: nothing in this file knows what time it is except through the
// values the server sent.

// The receiver's default port (setup-source.ts says the same); kept here so
// this file imports nothing and runs under node --test as it is.
const SLEEP_BRIDGE_DEFAULT_PORT = 8894;

/** The sub-steps, in order. */
export const BRIDGE_STEPS = ['card', 'assemble', 'connect', 'wait', 'machine', 'online'] as const;
export type BridgeStep = (typeof BRIDGE_STEPS)[number];

/** The step after `s`, or null after the last (the sequence is over). */
export function nextBridgeStep(s: BridgeStep): BridgeStep | null {
  const i = BRIDGE_STEPS.indexOf(s);
  return i >= 0 && i < BRIDGE_STEPS.length - 1 ? BRIDGE_STEPS[i + 1] : null;
}

/** The step before `s`, or null before the first (back to the source step). */
export function previousBridgeStep(s: BridgeStep): BridgeStep | null {
  const i = BRIDGE_STEPS.indexOf(s);
  return i > 0 ? BRIDGE_STEPS[i - 1] : null;
}

/**
 * The pictures, in public/setup/sleep-bridge/, relative to <base href>. These
 * are stills: what the desktop setup shows (it decodes PNG on every platform)
 * and what the web wizard shows to anyone who asked for reduced motion.
 */
export const BRIDGE_PICTURES = {
  card: 'setup/sleep-bridge/d1-card-into-computer.png',
  cardIntoBridge: 'setup/sleep-bridge/d2a-card-into-bridge.png',
  bridgeOntoExtender: 'setup/sleep-bridge/d2b-bridge-onto-extender.png',
  power: 'setup/sleep-bridge/d2c-power.png',
  joinWifi: 'setup/sleep-bridge/d3-join-bridge-wifi.png',
  intoMachine: 'setup/sleep-bridge/d5a-into-machine.png',
  powerCycle: 'setup/sleep-bridge/d5b-power-cycle.png',
} as const;

/**
 * The steps that also have a looping animation (animated WebP) of the same
 * render as their still. The web wizard plays it unless reduced motion is on.
 */
export const BRIDGE_ANIMATIONS: Partial<Record<keyof typeof BRIDGE_PICTURES, string>> = {
  cardIntoBridge: 'setup/sleep-bridge/d2a-card-into-bridge.webp',
  bridgeOntoExtender: 'setup/sleep-bridge/d2b-bridge-onto-extender.webp',
  power: 'setup/sleep-bridge/d2c-power.webp',
  intoMachine: 'setup/sleep-bridge/d5a-into-machine.webp',
};

/** What one bridge in GET /api/sleepbridge says, as far as these steps read it. */
export interface WatchedBridge {
  sender: string;
  online?: boolean;
  card_root?: string;
  card_format?: string;
  session_started_at?: number;
  card_stated_at?: number;
  learned?: { verdict?: string } | null;
}

export interface WatchedStatus {
  port?: number;
  bridges?: WatchedBridge[];
}

/** One frame of the watch socket (api/sleepbridge/watch). */
export type WatchFrame =
  | { t: 'sleepbridge'; event: string; sender?: string; status?: WatchedStatus }
  | { t: 'cpapdash'; event: string; stream?: { state?: string; [k: string]: unknown } };

/** A text frame, read; null for anything that is not one of ours. */
export function parseWatchFrame(text: string): WatchFrame | null {
  try {
    const v = JSON.parse(text);
    if (!v || typeof v !== 'object') return null;
    if (v.t === 'sleepbridge' || v.t === 'cpapdash') return v as WatchFrame;
    return null;
  } catch {
    return null;
  }
}

/**
 * The watch socket's URL, under <base href> so it works behind Home Assistant
 * Ingress: http becomes ws, https becomes wss.
 */
export function watchUrl(baseURI: string): string {
  const u = new URL('api/sleepbridge/watch', baseURI);
  u.protocol = u.protocol === 'https:' ? 'wss:' : 'ws:';
  return u.toString();
}

/**
 * D4: the bridge whose own hello this step waits for, from one frame.
 *
 * A "hello" event names it. A bridge online in a later snapshot counts too,
 * but only when its session started after `openedAt` (the server's clock,
 * read from the first snapshot), so a bridge that was streaming before the
 * step opened is not mistaken for the new one; those are offered instead
 * (onlineBridges) for the person to pick. A test hello (the bridge's own
 * probe) is never an event, so it never counts.
 */
export function helloBridge(frame: WatchFrame, openedAt: number): string | null {
  if (frame.t !== 'sleepbridge') return null;
  if (frame.event === 'hello' && frame.sender) return frame.sender;
  for (const b of frame.status?.bridges ?? []) {
    if (b.online && (b.session_started_at ?? 0) > openedAt) return b.sender;
  }
  return null;
}

/** The bridges online in a status: offered on D4 when one was already connected. */
export function onlineBridges(status: WatchedStatus | undefined): string[] {
  return (status?.bridges ?? []).filter(b => b.online).map(b => b.sender);
}

/**
 * The server's "now" as far as these steps know it: the newest session or
 * card time in a snapshot. Taken from the first snapshot as D4's openedAt,
 * so no browser clock is compared with the server's.
 */
export function snapshotMark(status: WatchedStatus | undefined): number {
  let mark = 0;
  for (const b of status?.bridges ?? []) {
    mark = Math.max(mark, b.session_started_at ?? 0, b.card_stated_at ?? 0);
  }
  return mark;
}

/** One bridge out of a status, or null. */
export function findBridge(status: WatchedStatus | undefined, sender: string): WatchedBridge | null {
  return (status?.bridges ?? []).find(b => b.sender === sender) ?? null;
}

/** D5: what the machine step says about the card. */
export type CardOutcome = 'waiting' | 'seen' | 'mismatch';

/**
 * D5: the card statement arrived when the bridge's card_stated_at is newer
 * than it was at the hello (`statedAtHello`); then the card is the learned
 * one unless the bridge's learned verdict says another card.
 */
export function cardOutcome(bridge: WatchedBridge | null, statedAtHello: number): CardOutcome {
  if (!bridge) return 'waiting';
  const stated = bridge.card_stated_at ?? 0;
  if (stated <= 0 || stated <= statedAtHello) return 'waiting';
  return bridge.learned?.verdict === 'mismatch' ? 'mismatch' : 'seen';
}

/** The translation key for a card outcome. */
export function cardOutcomeKey(o: CardOutcome): string {
  switch (o) {
    case 'waiting': return 'setup.bridge.machine.waiting';
    case 'seen': return 'setup.bridge.machine.seen';
    case 'mismatch': return 'setup.bridge.machine.mismatch';
  }
}

/**
 * D5: the source D4 set needs setting again when the bridge now knows its
 * card format and it is not the one D4 used.
 */
export function formatChanged(setFormat: string, bridge: WatchedBridge | null): boolean {
  const now = bridge?.card_format ?? '';
  return !!now && !!setFormat && now !== setFormat;
}

/** D3: the port is mentioned only when it is not the default. */
export function portNote(port: number): number | null {
  return port !== SLEEP_BRIDGE_DEFAULT_PORT ? port : null;
}

/**
 * D3: the address prefilled for the bridge: the one the browser used to
 * reach this page, when it is an IPv4 address other than loopback (in
 * Docker that is the host, which the container's own address is not).
 * "" otherwise, and the person types it.
 */
export function prefillAddress(hostname: string): string {
  const m = /^(\d{1,3})\.(\d{1,3})\.(\d{1,3})\.(\d{1,3})$/.exec(hostname);
  if (!m || m.slice(1).some(o => +o > 255) || m[1] === '127' || m[1] === '0') return '';
  return hostname;
}

/** D3: an address the bridge can take: an IPv4 address or a hostname, nothing else. */
export function validBridgeAddress(a: string): boolean {
  const s = a.trim();
  if (!s || s.length > 64) return false;
  const ip = /^(\d{1,3})\.(\d{1,3})\.(\d{1,3})\.(\d{1,3})$/.exec(s);
  if (ip) return ip.slice(1).every(o => +o <= 255);
  return /^[A-Za-z0-9]([A-Za-z0-9-]*[A-Za-z0-9])?(\.[A-Za-z0-9]([A-Za-z0-9-]*[A-Za-z0-9])?)*$/.test(s);
}

/**
 * D6: the translation key for the line under the switch, which says where
 * the person is, from the stream's state (the watch socket) and what they
 * saved here: `savedOff` true when they saved with the switch off, false when
 * on, null before any save. "Sending is off." only after they turned it off
 * and saved; before a token is saved, "Not sending yet".
 */
export function streamStateKey(state: string | undefined, savedOff: boolean | null = null): string {
  const s = state ?? '';
  if (s === 'off') return 'setup.bridge.online.state.' + (savedOff === true ? 'off' : 'notYet');
  if (s === 'no_token' || s === '') return 'setup.bridge.online.state.notYet';
  const known = ['connecting', 'connected', 'unreachable', 'refused', 'revoked', 'closed', 'unavailable'];
  return 'setup.bridge.online.state.' + (known.includes(s) ? s : 'unknown');
}

/** One dark module of a QR code, as an SVG rect. */
export interface QrRect { x: number; y: number; }

/**
 * The QR answer (GET /api/sleepbridge/qr: rows of '0' and '1') as the dark
 * modules to draw, shifted by a 4-module quiet zone, and the full side.
 */
export function qrRects(rows: string[], quiet = 4): { side: number; rects: QrRect[] } {
  const rects: QrRect[] = [];
  rows.forEach((row, y) => {
    for (let x = 0; x < row.length; x++) if (row[x] === '1') rects.push({ x: x + quiet, y: y + quiet });
  });
  const width = rows.reduce((w, r) => Math.max(w, r.length), 0);
  return { side: Math.max(width, rows.length) + 2 * quiet, rects };
}
