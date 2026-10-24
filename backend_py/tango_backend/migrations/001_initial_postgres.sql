-- Tango backend only. Desktop history/preferences continue to use local SQLite.
CREATE TABLE IF NOT EXISTS users (
  id TEXT PRIMARY KEY, username TEXT NOT NULL, username_key TEXT NOT NULL UNIQUE,
  password_salt TEXT NOT NULL, password_hash TEXT NOT NULL, created_at BIGINT NOT NULL
);
CREATE TABLE IF NOT EXISTS sessions (
  token_hash TEXT PRIMARY KEY, user_id TEXT NOT NULL REFERENCES users(id) ON DELETE CASCADE,
  created_at BIGINT NOT NULL, expires_at BIGINT NOT NULL
);
CREATE INDEX IF NOT EXISTS sessions_user ON sessions(user_id, created_at);
CREATE INDEX IF NOT EXISTS sessions_expiry ON sessions(expires_at);
CREATE TABLE IF NOT EXISTS ai_usage (
  user_id TEXT NOT NULL REFERENCES users(id) ON DELETE CASCADE,
  utc_day TEXT NOT NULL, request_count BIGINT NOT NULL CHECK(request_count >= 0),
  PRIMARY KEY(user_id, utc_day)
);
CREATE TABLE IF NOT EXISTS music_tasks (
  id TEXT PRIMARY KEY, user_id TEXT NOT NULL REFERENCES users(id) ON DELETE CASCADE,
  device_id TEXT NOT NULL, revision INTEGER NOT NULL, state_version BIGINT NOT NULL,
  status TEXT NOT NULL, snapshot_json JSONB NOT NULL,
  created_at BIGINT NOT NULL, updated_at BIGINT NOT NULL,
  owner_id TEXT, owner_token TEXT, owner_epoch BIGINT NOT NULL DEFAULT 0,
  lease_until BIGINT,
  CHECK ((owner_id IS NULL AND owner_token IS NULL AND lease_until IS NULL) OR
         (owner_id IS NOT NULL AND owner_token IS NOT NULL AND lease_until IS NOT NULL))
);
CREATE INDEX IF NOT EXISTS music_tasks_owner ON music_tasks(user_id, device_id, updated_at);
CREATE INDEX IF NOT EXISTS music_tasks_recovery ON music_tasks(status, lease_until);
CREATE TABLE IF NOT EXISTS music_task_requests (
  user_id TEXT NOT NULL, device_id TEXT NOT NULL, request_id TEXT NOT NULL,
  payload_hash TEXT NOT NULL, task_id TEXT NOT NULL REFERENCES music_tasks(id) ON DELETE CASCADE,
  request_metadata_json JSONB,
  PRIMARY KEY(user_id, device_id, request_id)
);
CREATE TABLE IF NOT EXISTS music_task_calls (
  id TEXT PRIMARY KEY, task_id TEXT NOT NULL REFERENCES music_tasks(id) ON DELETE CASCADE,
  revision INTEGER NOT NULL, run_id TEXT NOT NULL DEFAULT '', node TEXT NOT NULL,
  status TEXT NOT NULL, started_at BIGINT NOT NULL, finished_at BIGINT,
  input_tokens BIGINT, output_tokens BIGINT, error_code TEXT, diagnostics_json JSONB
);
CREATE INDEX IF NOT EXISTS music_task_calls_task ON music_task_calls(task_id, run_id);
CREATE TABLE IF NOT EXISTS music_task_global_usage (
  utc_day TEXT PRIMARY KEY, request_count BIGINT NOT NULL CHECK(request_count >= 0)
);
-- One task OR legacy request can own an account at a time.
CREATE TABLE IF NOT EXISTS music_task_account_leases (
  user_id TEXT PRIMARY KEY REFERENCES users(id) ON DELETE CASCADE,
  task_id TEXT REFERENCES music_tasks(id) ON DELETE CASCADE,
  owner_id TEXT NOT NULL, owner_token TEXT NOT NULL, owner_epoch BIGINT NOT NULL,
  lease_until BIGINT NOT NULL
);
-- A unique user slot prevents overlapping model calls even across processes.
-- Expiring slots are capacity reservations, never proof a provider request ran once.
CREATE TABLE IF NOT EXISTS music_task_model_slots (
  call_id TEXT PRIMARY KEY, user_id TEXT NOT NULL UNIQUE REFERENCES users(id) ON DELETE CASCADE,
  task_id TEXT REFERENCES music_tasks(id) ON DELETE CASCADE,
  owner_id TEXT NOT NULL, owner_token TEXT NOT NULL, owner_epoch BIGINT NOT NULL,
  lease_until BIGINT NOT NULL
);
CREATE INDEX IF NOT EXISTS music_task_model_slots_expiry ON music_task_model_slots(lease_until);
