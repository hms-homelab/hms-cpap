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
  };
  ahi_trend: { date: string; value: string }[];
  usage_trend: { date: string; value: string }[];
}

export interface SessionListItem {
  session_start: string;
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
