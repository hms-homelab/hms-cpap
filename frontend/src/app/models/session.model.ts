export interface MetricCard {
  label: string;
  value: string;
  unit: string;
  trend?: 'up' | 'down' | 'stable';
}

export interface DashboardData {
  latest_night: {
    date: string;
    ahi: string;
    usage_hours: string;
    leak_avg: string;
    compliance_pct: string;
    therapy_mode: string;  // 0=CPAP, 1=APAP, 7=ASV, 8=ASVAuto
  };
  ahi_trend: { date: string; value: string }[];
  usage_trend: { date: string; value: string }[];
}

export interface SessionListItem {
  session_start: string;
  session_end: string | null;
  duration_hours: string;
  ahi: string;
  total_events: string;
  obstructive_apneas: string;
  central_apneas: string;
  hypopneas: string;
  reras: string;
  avg_spo2: string | null;
  avg_heart_rate: string | null;
}

export interface SessionDetail extends SessionListItem {
  session_end: string;
  min_spo2: string | null;
  max_heart_rate: string | null;
  min_heart_rate: string | null;
  avg_event_duration: string | null;
  max_event_duration: string | null;
  therapy_mode: string;  // 0=CPAP, 1=APAP, 7=ASV, 8=ASVAuto
  events: SessionEvent[];
}

export interface SessionEvent {
  event_type: string;
  event_timestamp: string;
  duration_seconds: string;
  details: string | null;
}

export interface TrendPoint {
  date: string;
  [key: string]: string;
}

export interface SignalData {
  timestamps: string[];
  flow_avg: (number | null)[];
  flow_max: (number | null)[];
  flow_min: (number | null)[];
  pressure_avg: (number | null)[];
  pressure_max: (number | null)[];
  pressure_min: (number | null)[];
  respiratory_rate: (number | null)[];
  tidal_volume: (number | null)[];
  minute_ventilation: (number | null)[];
  ie_ratio: (number | null)[];
  flow_limitation: (number | null)[];
  leak_rate: (number | null)[];
  mask_pressure: (number | null)[];
  epr_pressure: (number | null)[];
  snore_index: (number | null)[];
  target_ventilation: (number | null)[];
}

export interface VitalsData {
  timestamps: string[];
  spo2: (number | null)[];
  spo2_min: (number | null)[];
  heart_rate: (number | null)[];
  hr_min: (number | null)[];
  hr_max: (number | null)[];
}
