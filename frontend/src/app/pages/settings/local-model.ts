// SDD-013: whether Settings shows the "Local Sleep Model" section. Pure, so it
// is tested with node --test and no Angular.

/** What GET /api/local-model/status says, as far as the section cares. */
export interface LocalModelStatusLike {
  published?: boolean;
}

/**
 * Albin, 2026-11-19: the section is hidden while no model is published (the
 * pinned URL and hash empty). Shown only when the server says one is; an
 * older server with no status (null) shows nothing either.
 */
export function showLocalModelSection(status: LocalModelStatusLike | null | undefined): boolean {
  return status?.published === true;
}
