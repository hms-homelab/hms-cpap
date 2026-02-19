-- Retrofit script: Convert checkpoint_files from bytes to KB
-- Run on: PostgreSQL cpap_monitoring database (192.168.2.15)
-- Date: 2026-02-19
-- Reason: Previous code stored filesystem bytes instead of ezShare KB values
--
-- Schema: checkpoint_files is a JSONB column in cpap_sessions table
-- Logic: Any value > 10000 is clearly bytes (KB values are ~1000-2000)
--        Convert by dividing by 1024 and rounding

-- Show current state
SELECT id, session_start, checkpoint_files
FROM cpap_sessions
WHERE checkpoint_files != '{}'::jsonb
ORDER BY session_start DESC
LIMIT 10;

-- Show what needs fixing (values > 10000 are bytes)
SELECT id, session_start,
    key as filename,
    value::int as old_value,
    CASE WHEN value::int > 10000 THEN 'BYTES (needs fix)' ELSE 'KB (ok)' END as status,
    ROUND(value::int / 1024.0) as would_become_kb
FROM cpap_sessions,
jsonb_each_text(checkpoint_files) AS kv(key, value)
WHERE value::int > 10000
ORDER BY session_start DESC;

-- Apply the fix
UPDATE cpap_sessions
SET checkpoint_files = (
    SELECT jsonb_object_agg(
        key,
        CASE
            WHEN value::int > 10000 THEN ROUND(value::int / 1024.0)::int
            ELSE value::int
        END
    )
    FROM jsonb_each_text(checkpoint_files) AS kv(key, value)
)
WHERE EXISTS (
    SELECT 1 FROM jsonb_each_text(checkpoint_files) AS kv(key, value)
    WHERE value::int > 10000
);

-- Verify after fix
SELECT id, session_start, checkpoint_files
FROM cpap_sessions
WHERE checkpoint_files != '{}'::jsonb
ORDER BY session_start DESC
LIMIT 5;
