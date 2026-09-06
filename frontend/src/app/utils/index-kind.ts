/**
 * Is this night's index an apnea-HYPOPNEA index, or apneas only? (SDD-024)
 *
 * A Sefam S.Box scores apneas and does not mark hypopneas in any readable form,
 * so its events-per-hour is roughly two thirds of what the same night would
 * score on a ResMed -- hypopneas are 35% of a real AHI's numerator (534 of 1536
 * across 175 nights). It is a DIFFERENT MEASUREMENT, not a smaller one, and it
 * must never be graded against AHI thresholds.
 *
 * The value is still computed, stored and shown. Only the label and the grading
 * change. When there is eventually something to compare it against, the history
 * is already there.
 *
 * ONE helper, asked by every surface, rather than the same condition written
 * out per component. The label and the decision to grade are the same decision.
 */

/** What the API sends. Anything unrecognised is treated as a real AHI. */
export type IndexKind = 'ahi' | 'ungraded';

/** A row, night or session that may carry a kind. */
export interface HasIndexKind {
  index_kind?: string | null;
}

/**
 * True when the number may be called an AHI and graded against AHI thresholds.
 *
 * Absent means yes, deliberately: every row written before SDD-024 is ResMed,
 * and the API already COALESCEs to 'ahi'. Being wrong in this direction shows a
 * real AHI as one; being wrong the other way would grade an apnea-only number.
 */
export function isGradableAhi(row: HasIndexKind | null | undefined): boolean {
  return !row || row.index_kind !== 'ungraded';
}

/**
 * The i18n key for what this index should be CALLED.
 *
 * A key rather than a string: this renders in five languages and the parity
 * gate fails the build if any of them drifts.
 */
export function indexLabelKey(row: HasIndexKind | null | undefined): string {
  return isGradableAhi(row) ? 'metric.ahi' : 'metric.apneaIndex';
}

/**
 * Whether to apply severity colouring to this value.
 *
 * Identical to isGradableAhi today and deliberately its own function: "may I
 * call it an AHI" and "may I paint it red" are different questions, and when
 * an apnea-only index eventually gets bands of its own, only this one changes.
 */
export function isGradable(row: HasIndexKind | null | undefined): boolean {
  return isGradableAhi(row);
}
