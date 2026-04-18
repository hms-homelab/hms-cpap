export interface AppConfig {
  device_id: string;
  device_name: string;
  source: string;
  ezshare_url: string;
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
}
