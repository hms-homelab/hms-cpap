export interface MetricCard {
  label: string;
  value: string;
  unit: string;
  trend?: 'up' | 'down' | 'stable';
}

/// SDD-019. Null on a night with none of usage, AHI or leak: that is a night we
/// know nothing about, which is not the same as a night that scored zero.
export type SleepIndexBand = 'excellent' | 'good' | 'fair' | 'needs_attention';

export interface DashboardData {
  latest_night: {
    date: string;
    ahi: string;
    /** SDD-024: 'ahi' | 'ungraded'. Absent means a real AHI. */
    index_kind?: string;
    usage_hours: string;
    leak_avg: string;
    /** SDD-026: the machine's own figures for the night, null without an STR. */
    ahi_str?: string | null;
    duration_minutes_str?: string | null;
    /** SDD-026: 'computed' when our sessions filled the row, 'str' when only the STR did. */
    index_source?: string | null;
    compliance_pct: string;
    therapy_mode: string;  // 0=CPAP, 1=APAP, 7=ASV, 8=ASVAuto
    sleep_index: number | null;
    sleep_index_band: SleepIndexBand | null;
  };
  sleep_index_7night: number | null;
  sleep_index_7night_band: SleepIndexBand | null;
  /**
   * SDD-024: each point carries its own kind, so a window spanning a machine
   * change can be recognised rather than assumed. The chart titles itself for
   * the weaker of the kinds present.
   */
  ahi_trend: { date: string; value: string; index_kind?: string }[];
  usage_trend: { date: string; value: string }[];
}

/// SDD-020: one night as we recorded it and as ResMed did.
///
/// `myair_present` is false when ResMed has NO DATA for that date, which is not
/// the same as a night with no therapy, and the deltas are null in that case
/// rather than measured against zero.
export interface MyAirComparisonRow {
  record_date: string;
  duration_minutes: string | number | null;
  ahi: string | number | null;
  /** SDD-024: 'ahi' | 'ungraded'. Absent means a real AHI. */
  index_kind?: string | null;
  leak_95: string | number | null;
  sleep_index: number | null;
  sleep_index_band: SleepIndexBand | null;

  /// False when CpapDash has no night for this date, which is every night for
  /// someone whose only data source is myAir.
  ours_present: boolean;
  myair_present: boolean;
  total_usage_min: string | number | null;
  sleep_score: string | number | null;
  usage_score: string | number | null;
  ahi_score: string | number | null;
  mask_score: string | number | null;
  leak_score: string | number | null;
  myair_ahi: string | number | null;
  mask_pair_count: string | number | null;
  leak_percentile: string | number | null;

  usage_delta_min: number | null;
  ahi_delta: number | null;
  leak_delta: number | null;
}

export interface SessionListItem {
  sleep_day?: string;
  session_start: string;
  session_end: string | null;
  has_live?: string;
  duration_hours: string;
  ahi: string;
  /** SDD-024: 'ahi' | 'ungraded'. Absent means a real AHI. Per ROW, because a
   *  user who changed machines has both kinds in one list. */
  index_kind?: string;
  total_events: string;
  obstructive_apneas: string;
  central_apneas: string;
  hypopneas: string;
  reras: string;
  avg_spo2: string | null;
  avg_heart_rate: string | null;
  /** SDD-008 transfer-ledger state: 'live' | 'partial' | 'complete'. */
  night_state?: string;
  /** '1' when the night exists only as an O2 ring recording (no CPAP session). */
  oximetry_only?: string;
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

/** SDD-009: one row from the cross-night /api/events search. */
export interface EventRow extends SessionEvent {
  sleep_day: string;
}

export interface TrendPoint {
  date: string;
  /**
   * SDD-024. The index trend carries the kind on every point, so the chart can
   * be titled for the index it actually plots.
   *
   * Declared even though the index signature would already admit it: the shared
   * isGradableAhi() helper takes a HasIndexKind, and a type whose only named
   * member is `date` has "no properties in common" with that interface. Naming
   * it is what lets a trend point be asked the same question a night or a
   * session is asked, through the one helper rather than a second copy of the
   * rule.
   */
  index_kind?: string;
  [key: string]: string | undefined;
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

export interface OximetryData {
  timestamps: string[];
  spo2: (number | null)[];
  heart_rate: (number | null)[];
  motion: (number | null)[];
}
