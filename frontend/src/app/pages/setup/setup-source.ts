// The setup wizard's source step as data: what each choice writes to the
// config, and where a Sleep Bridge is told to stream. Pure, so it is tested
// with node --test and no Angular.

/** The choices on the wizard's source step. 'mm' is the Mule and Miner. */
export type SetupSource = 'mm' | 'local' | 'ezshare' | 'flashair' | 'sleepbridge' | 'skip';

/**
 * The choice picked when the step opens, and the first one listed: the Sleep
 * Bridge. It writes nothing, so a user who clicks straight through changes no
 * source setting.
 */
export const DEFAULT_SETUP_SOURCE: SetupSource = 'sleepbridge';

/** What the user filled in beside the choice. */
export interface SetupSourceInputs {
  /** The machine behind an ez Share or a Mule and Miner. */
  cardFormat: string;
  /** The machine that wrote a local folder. */
  localFormat: string;
  ezshareUrl: string;
  flashairUrl: string;
  localDir: string;
  /** The picked Mule and Miner's base URL. */
  mmBaseUrl: string;
}

/** The Sleep Bridge receiver's port when the config does not set one. */
export const SLEEP_BRIDGE_DEFAULT_PORT = 8894;

/**
 * The source keys the wizard writes for a choice.
 *
 * 'sleepbridge' writes nothing, exactly as 'skip' does: the Sleep Bridge
 * receiver is on by default and listens on its own port, so it needs no source
 * setting, and its nights land under the bridge's own folder.
 */
export function sourceConfig(source: SetupSource, i: SetupSourceInputs): Record<string, string> {
  switch (source) {
    case 'mm':
      // A proxy-mode mule is an ezShare server on the LAN. SDD-060 D7: and the
      // machine behind it, which the unit cannot tell.
      return { source: 'ezshare', transport: 'ezshare', format: i.cardFormat,
               ezshare_url: i.mmBaseUrl };
    case 'ezshare':
      return { source: 'ezshare', transport: 'ezshare', format: i.cardFormat,
               ezshare_url: i.ezshareUrl };
    case 'flashair':
      return { source: 'flashair', transport: 'flashair', format: 'resmed',
               flashair_url: i.flashairUrl };
    case 'local':
      // SDD-022: a folder is ONE transport read by several parsers.
      return { transport: 'local', format: i.localFormat,
               source: i.localFormat === 'resmed' ? 'local' : i.localFormat,
               local_dir: i.localDir };
    case 'sleepbridge':
    case 'skip':
      return {};
  }
}

/** The receiver's port from GET /api/sleepbridge, or the default. */
export function sleepBridgePort(status: { port?: unknown } | null | undefined): number {
  const p = Number(status?.port);
  return Number.isInteger(p) && p > 0 && p <= 65535 ? p : SLEEP_BRIDGE_DEFAULT_PORT;
}

/**
 * "address:port" for the bridge, when the page was opened at an IPv4 address
 * other than loopback: that address is this computer as the network sees it.
 * null otherwise (a name, localhost, IPv6), and the note then asks for this
 * computer's IP address.
 */
export function sleepBridgeTarget(hostname: string, port: number): string | null {
  const m = /^(\d{1,3})\.(\d{1,3})\.(\d{1,3})\.(\d{1,3})$/.exec(hostname);
  if (!m || m.slice(1).some(o => +o > 255) || m[1] === '127' || m[1] === '0') return null;
  return `${hostname}:${port}`;
}
