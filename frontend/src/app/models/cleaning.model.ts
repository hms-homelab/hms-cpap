/**
 * SDD-007 cleaning schedules.
 *
 * The wash half of upkeep, deliberately separate from supplies: a mask is
 * REPLACED every 90 days and WIPED every day, and one interval cannot mean both.
 *
 * `status` is computed by the server on every read and never stored, exactly as
 * supply wear is, so there is no cached value here that can go stale.
 */

/** Matches the backend's canonical lowercase strings. */
export type CleaningState = 'due' | 'upcoming' | 'disabled';

export interface CleaningStatus {
  state: CleaningState;
  /** Unix seconds. 0 when the task is disabled. */
  next_due_epoch: number;
  /** Negative once the slot is past. */
  days_until: number;
}

/** One preset from the seeded catalog. */
export interface CleaningTaskType {
  id: number;
  task_key: string;
  label: string;
  /** '' means the task applies to the whole setup rather than one item. */
  applies_to_type_key: string;
  default_interval_days: number;
  is_system: boolean;
}

export interface CleaningTask {
  id: number;
  profile_id: number;
  /** 0 means the task is setup-wide rather than pinned to one item. */
  item_id: number;
  client_uuid: string;
  task_key: string;
  /** Snapshotted when the task was created, so renaming a preset is not retroactive. */
  label: string;
  interval_days: number;
  /** Local wall-clock minutes since midnight. 510 = 08:30. */
  time_minutes: number;
  start_date: string;   // yyyy-mm-dd
  enabled: boolean;
  last_done_at: string; // '' when never done
  status: CleaningStatus;
}

/** Response from POST /api/cleaning/suggest. */
export interface CleaningSuggestResult {
  created: CleaningTask[];
  /** Tasks the setup already had. /suggest is idempotent, so this is normal. */
  already_present: number;
}
