-- MySQL 8.4/InnoDB。DDL 会隐式提交，只在业务服务就绪前独立执行。
-- 标识与用户名键使用 NO PAD 二进制排序，保留大小写及末尾空格差异。
CREATE TABLE IF NOT EXISTS music_storage_locks (
  lock_key VARCHAR(191) PRIMARY KEY
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_0900_bin;
CREATE TABLE IF NOT EXISTS users (
  id VARCHAR(100) PRIMARY KEY,username VARCHAR(128) NOT NULL,username_key VARCHAR(128) NOT NULL UNIQUE,
  password_salt VARCHAR(32) NOT NULL,password_hash VARCHAR(160) NOT NULL,created_at BIGINT NOT NULL
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_0900_bin;
CREATE TABLE IF NOT EXISTS sessions (
  token_hash VARCHAR(64) PRIMARY KEY,user_id VARCHAR(100) NOT NULL,created_at BIGINT NOT NULL,expires_at BIGINT NOT NULL,
  INDEX sessions_user(user_id,created_at),INDEX sessions_expiry(expires_at),
  FOREIGN KEY(user_id) REFERENCES users(id) ON DELETE CASCADE
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_0900_bin;
CREATE TABLE IF NOT EXISTS ai_usage (
  user_id VARCHAR(100) NOT NULL,utc_day VARCHAR(10) NOT NULL,request_count BIGINT NOT NULL CHECK(request_count>=0),
  PRIMARY KEY(user_id,utc_day),FOREIGN KEY(user_id) REFERENCES users(id) ON DELETE CASCADE
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_0900_bin;
CREATE TABLE IF NOT EXISTS music_account_quota (
  user_id VARCHAR(100) PRIMARY KEY,daily_limit BIGINT NOT NULL CHECK(daily_limit BETWEEN 0 AND 1000000000),
  total_limit BIGINT NOT NULL CHECK(total_limit BETWEEN 0 AND 1000000000),enabled TINYINT NOT NULL CHECK(enabled IN (0,1)),
  updated_at BIGINT NOT NULL,FOREIGN KEY(user_id) REFERENCES users(id) ON DELETE CASCADE
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_0900_bin;
CREATE TABLE IF NOT EXISTS music_registration_invites (
  id VARCHAR(100) PRIMARY KEY,code_hash VARCHAR(64) NOT NULL UNIQUE CHECK(CHAR_LENGTH(code_hash)=64),
  label VARCHAR(400) NOT NULL,expires_at BIGINT NOT NULL,max_uses BIGINT NOT NULL CHECK(max_uses BETWEEN 1 AND 10000),
  used_count BIGINT NOT NULL DEFAULT 0,
  disabled TINYINT NOT NULL DEFAULT 0 CHECK(disabled IN (0,1)),
  daily_limit BIGINT NOT NULL CHECK(daily_limit BETWEEN 0 AND 1000000000),
  total_limit BIGINT NOT NULL CHECK(total_limit BETWEEN 0 AND 1000000000),created_at BIGINT NOT NULL,
  CHECK(used_count BETWEEN 0 AND max_uses)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_0900_bin;
CREATE TABLE IF NOT EXISTS music_registration_invite_uses (
  user_id VARCHAR(100) PRIMARY KEY,invite_id VARCHAR(100) NOT NULL,created_at BIGINT NOT NULL,
  FOREIGN KEY(user_id) REFERENCES users(id) ON DELETE CASCADE,
  FOREIGN KEY(invite_id) REFERENCES music_registration_invites(id)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_0900_bin;
CREATE TABLE IF NOT EXISTS music_tasks (
  id VARCHAR(100) PRIMARY KEY,user_id VARCHAR(100) NOT NULL,device_id VARCHAR(100) NOT NULL,
  revision BIGINT NOT NULL,state_version BIGINT NOT NULL,status VARCHAR(40) NOT NULL,snapshot_json JSON NOT NULL,
  created_at BIGINT NOT NULL,updated_at BIGINT NOT NULL,owner_id VARCHAR(100),owner_token VARCHAR(100),
  owner_epoch BIGINT NOT NULL DEFAULT 0,lease_until BIGINT,
  INDEX music_tasks_owner(user_id,device_id,updated_at),INDEX music_tasks_recovery(status,lease_until),
  FOREIGN KEY(user_id) REFERENCES users(id) ON DELETE CASCADE,
  CHECK((owner_id IS NULL AND owner_token IS NULL AND lease_until IS NULL) OR
        (owner_id IS NOT NULL AND owner_token IS NOT NULL AND lease_until IS NOT NULL))
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_0900_bin;
CREATE TABLE IF NOT EXISTS music_task_requests (
  user_id VARCHAR(100) NOT NULL,device_id VARCHAR(100) NOT NULL,request_id VARCHAR(100) NOT NULL,
  payload_hash VARCHAR(64) NOT NULL,task_id VARCHAR(100) NOT NULL,request_metadata_json JSON,
  PRIMARY KEY(user_id,device_id,request_id),
  FOREIGN KEY(task_id) REFERENCES music_tasks(id) ON DELETE CASCADE
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_0900_bin;
CREATE TABLE IF NOT EXISTS music_task_calls (
  id VARCHAR(100) PRIMARY KEY,task_id VARCHAR(100) NOT NULL,revision BIGINT NOT NULL,run_id VARCHAR(100) NOT NULL DEFAULT '',
  node VARCHAR(100) NOT NULL,status VARCHAR(40) NOT NULL,started_at BIGINT NOT NULL,finished_at BIGINT,
  input_tokens BIGINT,output_tokens BIGINT,error_code VARCHAR(100),diagnostics_json JSON,
  INDEX music_task_calls_task(task_id,run_id),FOREIGN KEY(task_id) REFERENCES music_tasks(id) ON DELETE CASCADE
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_0900_bin;
CREATE TABLE IF NOT EXISTS music_task_global_usage (
  utc_day VARCHAR(10) PRIMARY KEY,request_count BIGINT NOT NULL CHECK(request_count>=0)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_0900_bin;
CREATE TABLE IF NOT EXISTS music_task_account_leases (
  user_id VARCHAR(100) PRIMARY KEY,task_id VARCHAR(100),owner_id VARCHAR(100) NOT NULL,owner_token VARCHAR(100) NOT NULL,
  owner_epoch BIGINT NOT NULL,lease_until BIGINT NOT NULL,
  FOREIGN KEY(user_id) REFERENCES users(id) ON DELETE CASCADE,FOREIGN KEY(task_id) REFERENCES music_tasks(id) ON DELETE CASCADE
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_0900_bin;
CREATE TABLE IF NOT EXISTS music_task_model_slots (
  call_id VARCHAR(100) PRIMARY KEY,user_id VARCHAR(100) NOT NULL UNIQUE,task_id VARCHAR(100),
  owner_id VARCHAR(100) NOT NULL,owner_token VARCHAR(100) NOT NULL,owner_epoch BIGINT NOT NULL,lease_until BIGINT NOT NULL,
  FOREIGN KEY(user_id) REFERENCES users(id) ON DELETE CASCADE,FOREIGN KEY(task_id) REFERENCES music_tasks(id) ON DELETE CASCADE
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_0900_bin;
CREATE TABLE IF NOT EXISTS development_identities (
  identity_key VARCHAR(100) PRIMARY KEY CHECK(identity_key='tango_local_development_v1'),
  user_id VARCHAR(100) NOT NULL UNIQUE,created_at BIGINT NOT NULL,FOREIGN KEY(user_id) REFERENCES users(id)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_0900_bin;
CREATE TABLE IF NOT EXISTS music_conversations (
  id VARCHAR(100) PRIMARY KEY,user_id VARCHAR(100) NOT NULL,device_id VARCHAR(100) NOT NULL,request_id VARCHAR(100) NOT NULL,
  payload_hash VARCHAR(64) NOT NULL,journal_json JSON NOT NULL,created_at BIGINT NOT NULL,updated_at BIGINT NOT NULL,cleared_at BIGINT,
  UNIQUE(user_id,device_id,request_id),INDEX music_conversations_latest(user_id,device_id,updated_at),
  FOREIGN KEY(user_id) REFERENCES users(id) ON DELETE CASCADE
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_0900_bin;
CREATE TABLE IF NOT EXISTS music_conversation_clears (
  user_id VARCHAR(100) NOT NULL,request_id VARCHAR(100) NOT NULL,receipt_json JSON NOT NULL,created_at BIGINT NOT NULL,
  PRIMARY KEY(user_id,request_id),FOREIGN KEY(user_id) REFERENCES users(id) ON DELETE CASCADE
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_0900_bin;
CREATE TABLE IF NOT EXISTS music_conversation_routes (
  id VARCHAR(100) PRIMARY KEY,conversation_id VARCHAR(100) NOT NULL,user_id VARCHAR(100) NOT NULL,
  device_id VARCHAR(100) NOT NULL,message_id VARCHAR(100) NOT NULL,input_hash VARCHAR(64) NOT NULL,input_json JSON NOT NULL,
  capability_version BIGINT NOT NULL,status VARCHAR(40) NOT NULL,paid TINYINT NOT NULL CHECK(paid IN (0,1)),
  attempt_id VARCHAR(100) NOT NULL UNIQUE,owner_id VARCHAR(100) NOT NULL,owner_token VARCHAR(100) NOT NULL,
  owner_epoch BIGINT NOT NULL,lease_until BIGINT NOT NULL,result_json JSON,error_code VARCHAR(100),
  created_at BIGINT NOT NULL,finished_at BIGINT,UNIQUE(conversation_id,message_id),
  INDEX music_conversation_routes_owner(user_id,device_id),
  FOREIGN KEY(conversation_id) REFERENCES music_conversations(id) ON DELETE CASCADE,
  FOREIGN KEY(user_id) REFERENCES users(id) ON DELETE CASCADE
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_0900_bin;
CREATE TABLE IF NOT EXISTS music_conversation_route_attempts (
  id VARCHAR(100) PRIMARY KEY,route_id VARCHAR(100) NOT NULL UNIQUE,status VARCHAR(40) NOT NULL,
  paid TINYINT NOT NULL CHECK(paid IN (0,1)),started_at BIGINT NOT NULL,finished_at BIGINT,
  input_tokens BIGINT,output_tokens BIGINT,error_code VARCHAR(100),
  FOREIGN KEY(route_id) REFERENCES music_conversation_routes(id) ON DELETE CASCADE
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_0900_bin;
-- 保留旧 SQLite→PG 导入审计；历史迁移不能因切换存储而丢失。
CREATE TABLE IF NOT EXISTS tango_storage_imports (
  migration_id VARCHAR(100) PRIMARY KEY,source_sha256 VARCHAR(64) NOT NULL UNIQUE,
  target_sha256 VARCHAR(64) NOT NULL,counts_json JSON NOT NULL,applied_at BIGINT NOT NULL
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_0900_bin;
