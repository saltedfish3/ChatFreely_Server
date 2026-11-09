CREATE DATABASE IF NOT EXISTS ChatFreely DEFAULT CHARACTER SET utf8mb4 COLLATE utf8mb4_general_ci;
USE ChatFreely;
CREATE TABLE `user` (
  `id` bigint unsigned NOT NULL AUTO_INCREMENT COMMENT '用户ID（自增，从1000000000开始）',
  `snowid` bigint unsigned NOT NULL COMMENT '分布式唯一ID',
  `username` varchar(64) NOT NULL,
  `password` varchar(128) NOT NULL,
  `email` varchar(254) CHARACTER SET utf8mb4 COLLATE utf8mb4_0900_ai_ci NOT NULL,
  `avatar_url` varchar(512) CHARACTER SET utf8mb4 COLLATE utf8mb4_0900_ai_ci DEFAULT NULL,
  `created_at` datetime NOT NULL DEFAULT CURRENT_TIMESTAMP,
  `status` tinyint(1) NOT NULL DEFAULT '0' COMMENT '0=未删除，1=已删除',
  PRIMARY KEY (`id`),
  UNIQUE KEY `uk_snowid` (`snowid`),
  UNIQUE KEY `uk_username` (`username`),
  UNIQUE KEY `uk_email` (`email`)
) ENGINE=InnoDB AUTO_INCREMENT=1000000122 DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_0900_ai_ci;

CREATE TABLE `user_refresh_tokens` (
  `user_snowid` bigint unsigned NOT NULL,
  `token_hash` char(64) NOT NULL COMMENT 'SHA-256 哈希后的 Refresh Token',
  `expires_at` datetime NOT NULL COMMENT '过期时间',
  `created_at` datetime NOT NULL DEFAULT CURRENT_TIMESTAMP,
  PRIMARY KEY (`user_snowid`),
  KEY `idx_token_hash` (`token_hash`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_0900_ai_ci;

CREATE TABLE `friendships` (
  `id` bigint NOT NULL,
  `user_a` bigint NOT NULL,
  `user_b` bigint NOT NULL,
  `status` tinyint NOT NULL DEFAULT '0' COMMENT '0是待处理 1是已通过 2是拒绝 3是拉黑',
  `lastaction_uid` bigint NOT NULL COMMENT '最后一次操作的用户',
  `verification_msg` varchar(30) CHARACTER SET utf8mb4 COLLATE utf8mb4_0900_ai_ci DEFAULT NULL COMMENT '验证信息',
  `created_at` datetime NOT NULL DEFAULT CURRENT_TIMESTAMP,
  PRIMARY KEY (`id`),
  UNIQUE KEY `uk_usera_userb` (`user_a`,`user_b`),
  KEY `idx_userb_id` (`user_b`),
  KEY `idx_user_a_status` (`user_a`,`status`),
  KEY `idx_user_b_status` (`user_b`,`status`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_0900_ai_ci;

CREATE TABLE `chat_groups` (
  `id` bigint unsigned NOT NULL AUTO_INCREMENT COMMENT '群短号ID, 从100000000起',
  `snowid` bigint unsigned NOT NULL COMMENT 'ConversationID',
  `owner_uid` bigint unsigned NOT NULL COMMENT '群主snowid',
  `name` varchar(64) NOT NULL COMMENT '群名称',
  `avatar_url` varchar(512) DEFAULT NULL,
  `created_at` datetime NOT NULL DEFAULT CURRENT_TIMESTAMP COMMENT '创建时间',
  `status` tinyint NOT NULL DEFAULT '0' COMMENT '0=正常 1=已解散 2=封禁',
  PRIMARY KEY (`id`),
  UNIQUE KEY `uk_snowid` (`snowid`),
  KEY `idx_owner_uid` (`owner_uid`)
) ENGINE=InnoDB AUTO_INCREMENT=100000029 DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_0900_ai_ci;

CREATE TABLE `group_members` (
  `snowid` bigint unsigned NOT NULL,
  `conversation_id` bigint unsigned NOT NULL COMMENT '群会话ID即snowid',
  `uid` bigint unsigned NOT NULL,
  `role` tinyint NOT NULL DEFAULT '1' COMMENT '0=群主 1=普通成员',
  `join_time` datetime NOT NULL DEFAULT CURRENT_TIMESTAMP,
  `status` tinyint NOT NULL DEFAULT '0' COMMENT '0=正常 1=已退群 2=被踢出',
  PRIMARY KEY (`snowid`),
  UNIQUE KEY `uk_conv_uid` (`conversation_id`,`uid`),
  KEY `idx_uid_status` (`uid`,`status`),
  KEY `idx_conv_role_status` (`conversation_id`,`role`,`status`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_0900_ai_ci;

CREATE TABLE `group_join_requests` (
  `snowid` bigint unsigned NOT NULL,
  `conversation_id` bigint unsigned NOT NULL,
  `initiator_uid` bigint unsigned NOT NULL COMMENT '邀请人 或 申请人',
  `target_uid` bigint unsigned NOT NULL COMMENT '被邀请人 或 申请人本人',
  `source` tinyint NOT NULL COMMENT '0=成员邀请 1=外部申请',
  `verification_msg` varchar(30) DEFAULT NULL,
  `status` tinyint NOT NULL DEFAULT '0' COMMENT '0=待处理 1=已同意 2=已拒绝',
  `handler_uid` bigint unsigned DEFAULT NULL,
  `created_at` datetime NOT NULL DEFAULT CURRENT_TIMESTAMP,
  `handled_at` datetime DEFAULT NULL,
  PRIMARY KEY (`snowid`),
  UNIQUE KEY `uk_conv_target_source` (`conversation_id`,`target_uid`,`source`) USING BTREE,
  KEY `idx_conv_status` (`conversation_id`,`status`),
  KEY `idx_target_status` (`target_uid`,`status`),
  KEY `idx_initiator` (`initiator_uid`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_0900_ai_ci;

CREATE TABLE `conversation_seq` (
  `conversation_id` bigint unsigned NOT NULL,
  `conversation_type` tinyint NOT NULL DEFAULT '0' COMMENT '0=单聊 1=群聊',
  `last_conv_seq` bigint unsigned NOT NULL DEFAULT '0',
  PRIMARY KEY (`conversation_id`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_0900_ai_ci;

CREATE TABLE `conversations` (
  `uid` bigint unsigned NOT NULL,
  `conversation_id` bigint unsigned NOT NULL,
  `last_seq` bigint unsigned DEFAULT '0',
  PRIMARY KEY (`uid`,`conversation_id`),
  KEY `idx_uid_last_seq` (`uid`,`last_seq`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_0900_ai_ci;

CREATE TABLE `messages` (
  `snowid` bigint unsigned NOT NULL COMMENT '雪花全局唯一ID',
  `conversation_id` bigint unsigned NOT NULL COMMENT '会话ID',
  `conv_seq` bigint unsigned NOT NULL DEFAULT '0' COMMENT '会话内严格递增消息序列号（从1开始）',
  `sender_id` bigint unsigned NOT NULL,
  `type` tinyint NOT NULL DEFAULT '0' COMMENT '消息类型：0-文本，1-图片，2-文件，3-视频',
  `content` text NOT NULL COMMENT '消息内容',
  `created_at` datetime NOT NULL DEFAULT CURRENT_TIMESTAMP,
  `idempotent_key` varchar(128) NOT NULL DEFAULT '',
  `timestamp` bigint unsigned NOT NULL DEFAULT '0' COMMENT '秒级时间戳',
  PRIMARY KEY (`snowid`),
  UNIQUE KEY `uk_conversation_seq` (`conversation_id`,`conv_seq`),
  UNIQUE KEY `idx_idempotent_key` (`idempotent_key`),
  KEY `idx_conversation_id_time` (`conversation_id`,`snowid`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_0900_ai_ci;

CREATE TABLE `global_seq` (
  `seq_name` varchar(32) NOT NULL COMMENT '序号生成器的名字',
  `current_value` bigint unsigned NOT NULL DEFAULT '0',
  PRIMARY KEY (`seq_name`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_0900_ai_ci;
