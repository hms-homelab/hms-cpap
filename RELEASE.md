# Release Notes — v1.4.1

## Changed
- **Session gap threshold**: Default changed from 2 hours to 1 hour (60 minutes), matching confirmed ResMed behavior (session ends 1 hour after last file close).
- **Configurable session gap**: New `SESSION_GAP_MINUTES` env var to override the default. Shown in startup configuration output.
