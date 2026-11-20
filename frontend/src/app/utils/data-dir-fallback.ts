/**
 * SDD-067 D7: what GET /api/capabilities reports when the Home Assistant
 * add-on could not write the data directory it was given, and this run keeps
 * its data in /data instead.
 */
export interface DataDirFallback {
  /** The directory that could not be written. */
  wanted: string;
  /** The data directory this run uses. */
  in_use: string;
}

/** The fallback from a capabilities answer, or null when this run is not one. */
export function dataDirFallback(caps: unknown): DataDirFallback | null {
  const fb = (caps as { data_dir_fallback?: unknown } | null | undefined)?.data_dir_fallback as
    | { wanted?: unknown; in_use?: unknown }
    | undefined;
  if (!fb || typeof fb.wanted !== 'string' || typeof fb.in_use !== 'string') return null;
  if (!fb.wanted || !fb.in_use) return null;
  return { wanted: fb.wanted, in_use: fb.in_use };
}
