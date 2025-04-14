/*
Navicat MySQL Data Transfer

Source Server         : 192.168.66.70
Source Server Version : 80029
Source Host           : 192.168.66.70:3306
Source Database       : freeswitch1011_2

Target Server Type    : MYSQL
Target Server Version : 80029
File Encoding         : 65001

Date: 2024-03-25 14:47:45
*/

SET FOREIGN_KEY_CHECKS=0;

-- ----------------------------
-- Table structure for sip_authentication
-- ----------------------------
DROP TABLE IF EXISTS `sip_authentication`;
CREATE TABLE `sip_authentication` (
  `nonce` varchar(255) DEFAULT NULL,
  `expires` bigint DEFAULT NULL,
  `profile_name` varchar(255) DEFAULT NULL,
  `hostname` varchar(255) DEFAULT NULL,
  `last_nc` int DEFAULT NULL,
  `algorithm` int NOT NULL DEFAULT '1',
  KEY `sa_nonce` (`nonce`),
  KEY `sa_hostname` (`hostname`),
  KEY `sa_expires` (`expires`),
  KEY `sa_last_nc` (`last_nc`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 ;

-- ----------------------------
-- Table structure for sip_dialogs
-- ----------------------------
DROP TABLE IF EXISTS `sip_dialogs`;
CREATE TABLE `sip_dialogs` (
  `call_id` varchar(255) DEFAULT NULL,
  `uuid` varchar(255) DEFAULT NULL,
  `sip_to_user` varchar(255) DEFAULT NULL,
  `sip_to_host` varchar(255) DEFAULT NULL,
  `sip_from_user` varchar(255) DEFAULT NULL,
  `sip_from_host` varchar(255) DEFAULT NULL,
  `contact_user` varchar(255) DEFAULT NULL,
  `contact_host` varchar(255) DEFAULT NULL,
  `state` varchar(255) DEFAULT NULL,
  `direction` varchar(255) DEFAULT NULL,
  `user_agent` varchar(255) DEFAULT NULL,
  `profile_name` varchar(255) DEFAULT NULL,
  `hostname` varchar(255) DEFAULT NULL,
  `contact` varchar(255) DEFAULT NULL,
  `presence_id` varchar(255) DEFAULT NULL,
  `presence_data` varchar(255) DEFAULT NULL,
  `call_info` varchar(255) DEFAULT NULL,
  `call_info_state` varchar(255) DEFAULT '',
  `expires` bigint DEFAULT '0',
  `status` varchar(255) DEFAULT NULL,
  `rpid` varchar(255) DEFAULT NULL,
  `sip_to_tag` varchar(255) DEFAULT NULL,
  `sip_from_tag` varchar(255) DEFAULT NULL,
  `rcd` int NOT NULL DEFAULT '0',
  KEY `sd_uuid` (`uuid`),
  KEY `sd_hostname` (`hostname`),
  KEY `sd_presence_data` (`presence_data`),
  KEY `sd_call_info` (`call_info`),
  KEY `sd_call_info_state` (`call_info_state`),
  KEY `sd_expires` (`expires`),
  KEY `sd_rcd` (`rcd`),
  KEY `sd_sip_to_tag` (`sip_to_tag`),
  KEY `sd_sip_from_user` (`sip_from_user`),
  KEY `sd_sip_from_host` (`sip_from_host`),
  KEY `sd_sip_to_host` (`sip_to_host`),
  KEY `sd_presence_id` (`presence_id`),
  KEY `sd_call_id` (`call_id`),
  KEY `sd_sip_from_tag` (`sip_from_tag`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 ;

-- ----------------------------
-- Table structure for sip_presence
-- ----------------------------
DROP TABLE IF EXISTS `sip_presence`;
CREATE TABLE `sip_presence` (
  `sip_user` varchar(255) DEFAULT NULL,
  `sip_host` varchar(255) DEFAULT NULL,
  `status` varchar(255) DEFAULT NULL,
  `rpid` varchar(255) DEFAULT NULL,
  `expires` bigint DEFAULT NULL,
  `user_agent` varchar(255) DEFAULT NULL,
  `profile_name` varchar(255) DEFAULT NULL,
  `hostname` varchar(255) DEFAULT NULL,
  `network_ip` varchar(255) DEFAULT NULL,
  `network_port` varchar(6) DEFAULT NULL,
  `open_closed` varchar(255) DEFAULT NULL,
  KEY `sp_hostname` (`hostname`),
  KEY `sp_open_closed` (`open_closed`),
  KEY `sp_sip_user` (`sip_user`),
  KEY `sp_sip_host` (`sip_host`),
  KEY `sp_profile_name` (`profile_name`),
  KEY `sp_expires` (`expires`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 ;

-- ----------------------------
-- Table structure for sip_registrations
-- ----------------------------
DROP TABLE IF EXISTS `sip_registrations`;
CREATE TABLE `sip_registrations` (
  `call_id` varchar(255) DEFAULT NULL,
  `sip_user` varchar(255) DEFAULT NULL,
  `sip_host` varchar(255) DEFAULT NULL,
  `presence_hosts` varchar(255) DEFAULT NULL,
  `contact` varchar(1024) DEFAULT NULL,
  `status` varchar(255) DEFAULT NULL,
  `ping_status` varchar(255) DEFAULT NULL,
  `ping_count` int DEFAULT NULL,
  `ping_time` bigint DEFAULT NULL,
  `force_ping` int DEFAULT NULL,
  `rpid` varchar(255) DEFAULT NULL,
  `expires` bigint DEFAULT NULL,
  `ping_expires` int NOT NULL DEFAULT '0',
  `user_agent` varchar(255) DEFAULT NULL,
  `server_user` varchar(255) DEFAULT NULL,
  `server_host` varchar(255) DEFAULT NULL,
  `profile_name` varchar(255) DEFAULT NULL,
  `hostname` varchar(255) DEFAULT NULL,
  `network_ip` varchar(255) DEFAULT NULL,
  `network_port` varchar(6) DEFAULT NULL,
  `sip_username` varchar(255) DEFAULT NULL,
  `sip_realm` varchar(255) DEFAULT NULL,
  `mwi_user` varchar(255) DEFAULT NULL,
  `mwi_host` varchar(255) DEFAULT NULL,
  `orig_server_host` varchar(255) DEFAULT NULL,
  `orig_hostname` varchar(255) DEFAULT NULL,
  `sub_host` varchar(255) DEFAULT NULL,
  KEY `sr_call_id` (`call_id`),
  KEY `sr_sip_user` (`sip_user`),
  KEY `sr_sip_host` (`sip_host`),
  KEY `sr_sub_host` (`sub_host`),
  KEY `sr_mwi_user` (`mwi_user`),
  KEY `sr_mwi_host` (`mwi_host`),
  KEY `sr_profile_name` (`profile_name`),
  KEY `sr_presence_hosts` (`presence_hosts`),
  KEY `sr_expires` (`expires`),
  KEY `sr_ping_expires` (`ping_expires`),
  KEY `sr_hostname` (`hostname`),
  KEY `sr_status` (`status`),
  KEY `sr_ping_status` (`ping_status`),
  KEY `sr_network_ip` (`network_ip`),
  KEY `sr_network_port` (`network_port`),
  KEY `sr_sip_username` (`sip_username`),
  KEY `sr_sip_realm` (`sip_realm`),
  KEY `sr_orig_server_host` (`orig_server_host`),
  KEY `sr_orig_hostname` (`orig_hostname`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 ;

-- ----------------------------
-- Table structure for sip_shared_appearance_dialogs
-- ----------------------------
DROP TABLE IF EXISTS `sip_shared_appearance_dialogs`;
CREATE TABLE `sip_shared_appearance_dialogs` (
  `profile_name` varchar(255) DEFAULT NULL,
  `hostname` varchar(255) DEFAULT NULL,
  `contact_str` varchar(255) DEFAULT NULL,
  `call_id` varchar(255) DEFAULT NULL,
  `network_ip` varchar(255) DEFAULT NULL,
  `expires` bigint DEFAULT NULL,
  KEY `ssd_profile_name` (`profile_name`),
  KEY `ssd_hostname` (`hostname`),
  KEY `ssd_contact_str` (`contact_str`),
  KEY `ssd_call_id` (`call_id`),
  KEY `ssd_expires` (`expires`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 ;

-- ----------------------------
-- Table structure for sip_shared_appearance_subscriptions
-- ----------------------------
DROP TABLE IF EXISTS `sip_shared_appearance_subscriptions`;
CREATE TABLE `sip_shared_appearance_subscriptions` (
  `subscriber` varchar(255) DEFAULT NULL,
  `call_id` varchar(255) DEFAULT NULL,
  `aor` varchar(255) DEFAULT NULL,
  `profile_name` varchar(255) DEFAULT NULL,
  `hostname` varchar(255) DEFAULT NULL,
  `contact_str` varchar(255) DEFAULT NULL,
  `network_ip` varchar(255) DEFAULT NULL,
  KEY `ssa_hostname` (`hostname`),
  KEY `ssa_network_ip` (`network_ip`),
  KEY `ssa_subscriber` (`subscriber`),
  KEY `ssa_profile_name` (`profile_name`),
  KEY `ssa_aor` (`aor`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 ;

-- ----------------------------
-- Table structure for sip_subscriptions
-- ----------------------------
DROP TABLE IF EXISTS `sip_subscriptions`;
CREATE TABLE `sip_subscriptions` (
  `proto` varchar(255) DEFAULT NULL,
  `sip_user` varchar(255) DEFAULT NULL,
  `sip_host` varchar(255) DEFAULT NULL,
  `sub_to_user` varchar(255) DEFAULT NULL,
  `sub_to_host` varchar(255) DEFAULT NULL,
  `presence_hosts` varchar(255) DEFAULT NULL,
  `event` varchar(255) DEFAULT NULL,
  `contact` varchar(1024) DEFAULT NULL,
  `call_id` varchar(255) DEFAULT NULL,
  `full_from` varchar(255) DEFAULT NULL,
  `full_via` varchar(255) DEFAULT NULL,
  `expires` bigint DEFAULT NULL,
  `user_agent` varchar(255) DEFAULT NULL,
  `accept` varchar(255) DEFAULT NULL,
  `profile_name` varchar(255) DEFAULT NULL,
  `hostname` varchar(255) DEFAULT NULL,
  `network_port` varchar(6) DEFAULT NULL,
  `network_ip` varchar(255) DEFAULT NULL,
  `version` int NOT NULL DEFAULT '0',
  `orig_proto` varchar(255) DEFAULT NULL,
  `full_to` varchar(255) DEFAULT NULL,
  KEY `ss_call_id` (`call_id`),
  KEY `ss_multi` (`call_id`,`profile_name`,`hostname`),
  KEY `ss_hostname` (`hostname`),
  KEY `ss_network_ip` (`network_ip`),
  KEY `ss_sip_user` (`sip_user`),
  KEY `ss_sip_host` (`sip_host`),
  KEY `ss_presence_hosts` (`presence_hosts`),
  KEY `ss_event` (`event`),
  KEY `ss_proto` (`proto`),
  KEY `ss_sub_to_user` (`sub_to_user`),
  KEY `ss_sub_to_host` (`sub_to_host`),
  KEY `ss_expires` (`expires`),
  KEY `ss_orig_proto` (`orig_proto`),
  KEY `ss_network_port` (`network_port`),
  KEY `ss_profile_name` (`profile_name`),
  KEY `ss_version` (`version`),
  KEY `ss_full_from` (`full_from`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 ;
