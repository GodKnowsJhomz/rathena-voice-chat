-- Voice license table for rathena-voice-chat
-- Keyed on account_id — one license per account covers all characters.
-- expires_at NULL = permanent license, otherwise the maintenance loop
-- removes the row and notifies the DLL when it elapses.

CREATE TABLE IF NOT EXISTS `voice_licenses` (
  `account_id`  INT          NOT NULL,
  `granted_by`  VARCHAR(24)  NOT NULL DEFAULT '',
  `granted_at`  DATETIME     NOT NULL DEFAULT CURRENT_TIMESTAMP,
  `expires_at`  DATETIME     NULL     DEFAULT NULL,
  PRIMARY KEY (`account_id`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;
