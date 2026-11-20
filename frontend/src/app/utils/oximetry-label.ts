// SDD-078 D3: oximetry says where it came from. The night's recordings carry
// a source ("o2ring", "garmin", "upload", ...) and a device name; the page
// says "Oximetry - <device>" for every source instead of assuming a ring.

export interface OximetryOrigin {
  source: string;
  device_name: string;
}

/** What a source is called when its recording has no device name. */
const FALLBACK: Record<string, string> = {
  o2ring: 'O2 Ring',
  garmin: 'Garmin',
};

/**
 * The devices behind a night's oximetry, as one line: the device names in the
 * order the recordings started, each once, joined by " + ". A recording with
 * no name is called by its source ("O2 Ring", "Garmin"); an upload with no
 * name is `uploadName`. Empty when there is no recording (or an older server
 * that does not say), so the caller can fall back to its own words.
 */
export function oximetryDevices(
  origins: OximetryOrigin[] | null | undefined,
  uploadName: string,
): string {
  if (!origins || !origins.length) return '';
  const names: string[] = [];
  for (const o of origins) {
    const name = (o.device_name || '').trim() ||
      FALLBACK[o.source] || (o.source === 'upload' ? uploadName : o.source);
    if (name && !names.includes(name)) names.push(name);
  }
  return names.join(' + ');
}

/** The origins of a GET /api/sessions/{date}/oximetry answer. */
export function originsOf(data: { sources?: OximetryOrigin[]; source?: string | null;
                                  device_name?: string | null } | null | undefined): OximetryOrigin[] {
  if (!data) return [];
  if (data.sources && data.sources.length) return data.sources;
  if (data.source) return [{ source: data.source, device_name: data.device_name || '' }];
  return [];
}
