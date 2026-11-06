// Product pictures of the data sources, one per device kind, in public/sources/.
// Blender renders on a transparent background (256 x 256, shown at up to 128 px),
// each in two variants: plain, and with a soft violet (#8B8CFF) glow behind it.
// PNG, not WebP: Drogon's static file router serves a fixed list of extensions
// and .webp is not on it.

/** Show the glow variant. One switch for every picture. */
export const SOURCE_GLOW = true;

/** The kinds that have a picture: the Devices page's kinds. */
export const SOURCE_KINDS = ['mule', 'sleepbridge', 'o2ring', 'ezshare', 'flashair', 'as11_ble'] as const;

/**
 * The picture of a source kind, relative to <base href> (SDD-021: under Home
 * Assistant Ingress the UI does not sit at the site root). The setup wizard's
 * 'mm' is the Mule & Miner. null for a kind with no picture.
 */
export function sourceImage(kind: string, glow: boolean = SOURCE_GLOW): string | null {
  const k = kind === 'mm' ? 'mule' : kind;
  if (!(SOURCE_KINDS as readonly string[]).includes(k)) return null;
  return `sources/${k}${glow ? '-glow' : ''}.png`;
}
