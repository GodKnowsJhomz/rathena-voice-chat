-- Voice ban table for rathena-voice-chat
-- Bans are keyed on account_id so players cannot bypass by switching characters.
-- banned_until NULL = permanent ban.

CREATE TABLE IF NOT EXISTS `voice_bans` (
  `account_id`   INT          NOT NULL,
  `banned_by`    VARCHAR(24)  NOT NULL DEFAULT '',
  `banned_at`    DATETIME     NOT NULL DEFAULT CURRENT_TIMESTAMP,
  `banned_until` DATETIME     NULL     DEFAULT NULL,
  PRIMARY KEY (`account_id`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;
