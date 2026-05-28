-- ============================================================================
-- Voice Chat tables for rathena-voice-chat
-- ----------------------------------------------------------------------------
-- All three tables are also auto-created by the voice server at startup
-- (CREATE TABLE IF NOT EXISTS), so importing this file is optional. It is
-- provided for admins who prefer to provision the schema manually.
-- ============================================================================

-- ── Voice bans ──────────────────────────────────────────────────────────────
-- Keyed on account_id so players cannot bypass by switching characters.
-- banned_until NULL = permanent ban.
CREATE TABLE IF NOT EXISTS `voice_bans` (
  `account_id`   INT          NOT NULL,
  `banned_by`    VARCHAR(24)  NOT NULL DEFAULT '',
  `banned_at`    DATETIME     NOT NULL DEFAULT CURRENT_TIMESTAMP,
  `banned_until` DATETIME     NULL     DEFAULT NULL,
  PRIMARY KEY (`account_id`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

-- ── Voice licenses ────────────────────────────────────────────────────────────
-- Keyed on account_id — one license per account covers all characters.
-- expires_at NULL = permanent license. Only enforced when the voice server's
-- voice_license_required option is on.
CREATE TABLE IF NOT EXISTS `voice_licenses` (
  `account_id`  INT          NOT NULL,
  `granted_by`  VARCHAR(24)  NOT NULL DEFAULT '',
  `granted_at`  DATETIME     NOT NULL DEFAULT CURRENT_TIMESTAMP,
  `expires_at`  DATETIME     NULL     DEFAULT NULL,
  PRIMARY KEY (`account_id`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

-- ── Voice blocks ──────────────────────────────────────────────────────────────
-- Player-driven block list. blocker_account_id has blocked blocked_account_id
-- from being heard. Asymmetric by default (blocked person can still hear the
-- blocker). Keyed on account_id so the block follows character switches.
CREATE TABLE IF NOT EXISTS `voice_blocks` (
  `blocker_account_id`  INT          NOT NULL,
  `blocked_account_id`  INT          NOT NULL,
  `blocked_name`        VARCHAR(24)  NOT NULL DEFAULT '',
  `created_at`          DATETIME     NOT NULL DEFAULT CURRENT_TIMESTAMP,
  PRIMARY KEY (`blocker_account_id`, `blocked_account_id`),
  INDEX (`blocker_account_id`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;
