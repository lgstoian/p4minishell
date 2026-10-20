/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 */
/**
 * @file event_bus_topics.h
 * @brief Predefined event topic name constants.
 *
 * Using these prevents typos and documents the canonical topics.
 * All strings fit within P4_CONFIG_EVENT_BUS_TOPIC_LEN.
 */

#ifndef P4MINISHELL_EVENT_BUS_TOPICS_H
#define P4MINISHELL_EVENT_BUS_TOPICS_H

#define EVT_SYSTEM_BOOT         "system.boot"
#define EVT_SYSTEM_SHUTDOWN     "system.shutdown"
#define EVT_SYSTEM_ERROR        "system.error"
#define EVT_COMMAND_START       "command.start"
#define EVT_COMMAND_END         "command.end"
#define EVT_COMMAND_ERROR       "command.error"
#define EVT_WIFI_CONNECTED      "wifi.connected"
#define EVT_WIFI_DISCONNECTED   "wifi.disconnected"
#define EVT_SD_MOUNT            "sd.mount"
#define EVT_SD_UNMOUNT          "sd.unmount"
#define EVT_BATTERY_LOW         "battery.low"
#define EVT_ALARM_TRIGGER       "alarm.trigger"
#define EVT_MCP_MESSAGE         "mcp.message"

#endif /* P4MINISHELL_EVENT_BUS_TOPICS_H */
