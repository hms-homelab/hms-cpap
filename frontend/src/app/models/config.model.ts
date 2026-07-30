export interface AppConfig {
  device_id: string;
  device_name: string;
  source: string;
  ezshare_url: string;
  ezshare_range: boolean;
  local_dir: string;
  burst_interval: number;
  web_port: number;
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
  };
  llm: {
    enabled: boolean;
    provider: string;
    endpoint: string;
    model: string;
    api_key: string;
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
