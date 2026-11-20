// SDD-073 D8 / D10: one bridge's folder as a source, as data. Pure, tested
// with node --test.

/**
 * The configured local folder is this bridge's folder: the same path, with
 * a trailing separator or not, and on Windows in any case of letters.
 */
export function sameFolder(localDir: string | undefined | null, cardRoot: string | undefined | null): boolean {
  const norm = (p: string) => p.trim().replace(/[\\/]+$/, '').replace(/\\/g, '/');
  if (!localDir || !cardRoot) return false;
  const a = norm(localDir);
  const b = norm(cardRoot);
  if (!a || !b) return false;
  const windows = /^[A-Za-z]:\//.test(a) || /^[A-Za-z]:\//.test(b);
  return windows ? a.toLowerCase() === b.toLowerCase() : a === b;
}

/** The card format's name, or null when the bridge has not recognised its card yet. */
export function formatLabel(format: string | undefined | null): string | null {
  if (format === 'resmed') return 'ResMed';
  if (format === 'bmc') return 'BMC / React Health Luna';
  return format ? format : null;
}

/** The configured source is a local folder at this bridge's folder. */
export function bridgeInUse(config: { transport?: string; local_dir?: string } | null | undefined,
                            cardRoot: string | undefined | null): boolean {
  if (!config) return false;
  if (config.transport && config.transport !== 'local') return false;
  return sameFolder(config.local_dir, cardRoot);
}
