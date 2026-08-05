export interface AppConfig {
  device_id: string;
  device_name: string;
  source: string;
  ezshare_url: string;
  ezshare_range: boolean;
  local_dir: string;
  /**
   * SDD-012: where collected card files are written. Required by the Mule and
   * Miner path, and until now settable only in the first-run wizard, so an
   * onboarded user had to edit config.json to move where their nights land.
   * Applies to every source, unlike local_dir.
   */
  archive_dir: string;
  burst_interval: number;
  web_port: number;
  /** Angular bundle location. A wrong value serves no UI at all. */
  static_dir: string;
  setup_complete: boolean;
  database: {
    type: string;
    sqlite_path: string;
    host: string;
    port: number;
    name: string;
    user: string;
    password: string;
  };
  mqtt: {
    enabled: boolean;
    broker: string;
    port: number;
    username: string;
    password: string;
    client_id: string;
  };
  llm: {
    enabled: boolean;
    provider: string;
    endpoint: string;
    model: string;
    api_key: string;
    max_tokens: number;
    prompt_file: string;
  };
  ml_training: {
    enabled: boolean;
    schedule: string;
    model_dir: string;
    min_days: number;
    max_training_days: number;
  };
  o2ring: {
    enabled: boolean;
    mode: string;          // 'http' | 'ble'
    mule_url: string;      // for HTTP mode
  };
  sleephq: {
    enabled: boolean;
    client_id: string;
    client_secret: string;
    auto_on_session: boolean;   // upload when a live session completes
    auto_on_backfill: boolean;  // upload when local-mode/backfill ingests a night
    quiet_minutes: number;      // SDD-003: archive must be quiet this long first
  };
  /** SDD-012: requires LLM and PostgreSQL; shown disabled without them. */
  agent: {
    enabled: boolean;
    embed_model: string;
    temperature: number;
    max_iterations: number;
  };
  sleep_stage: {
    enabled: boolean;
    live_inference: boolean;
    model_dir: string;
    model_version: string;
  };
  /** Raw SD sector push mode. Shown only when source === 'fysetc'. */
  fysetc: {
    enabled: boolean;
    listen_port: number;
    listen_bind: string;
    connection_timeout_s: number;
    archive_dir: string;
    log_dir: string;
  };
  /** SDD-004 optional cloud mirror. Local stays the source of truth. */
  cpapdash: {
    enabled: boolean;
    api_url: string;
    token: string;      // redacted to '********' on read; resend it to keep
    auto_sync: boolean;
  };
}

/**
 * SDD-005: a CpapDash Mule and Miner unit found on the LAN via mDNS
 * (`_cpapdash._tcp`). Mirrors the JSON from GET /api/discover/devices.
 */
export interface DiscoveredDevice {
  instance: string;       // mDNS instance label, which is the unit serial
  host: string;           // dotted-quad when an A record was supplied
  port: number;
  serial: string;
  fw: string;
  mode: string;           // 'proxy' (serves /dir + /download) or 'cloud'
  local_capable: boolean; // only a proxy-mode unit can feed a local install
  base_url: string;       // what gets written into config.ezshare_url
}
