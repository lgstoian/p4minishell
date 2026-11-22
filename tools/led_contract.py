#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Stoian Alexandru
# SPDX-License-Identifier: MIT
"""Guard the documented auto-LED state machine against being flattened.

The LED contract is documented in ai-context.md ("LED Rules") and command.md
(`rgb auto`): connecting=amber pulse, connected=green, idle-with-no-target=
steady green, configured-network-drops=amber pulse, watchdog-timeout=red pulse,
HTTP start=blue flash, boot=green flash. A "make the LED green" edit once
replaced the whole state machine with a single green frame, which removed a
feature. This guard reads the source and refuses that class of change.
"""
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def read(path):
    with open(os.path.join(ROOT, path), encoding="utf-8", errors="replace") as fh:
        return fh.read()


def main():
    led = read("components/led/led.c")
    net = read("components/networking/networking.c")
    cfg = read("p4minishell_config.h")
    errors = []

    # 1. Auto status must start on the connecting pulse, not on idle green.
    if not re.search(r"led_event_frame\(\s*LED_EVENT_WIFI_CONNECTING\s*,\s*&s_status_frame", led):
        errors.append("led.c: persistent status frame must be initialised to "
                      "LED_EVENT_WIFI_CONNECTING (amber pulse)")

    # 2. The idle helper must distinguish "no target" (green) from "configured
    #    network dropped" (amber). Returning a single event removes the signal.
    m = re.search(r"networking_wifi_idle_led_event_locked\(void\)\s*\{(.*?)\}", net, re.S)
    if not m or "LED_EVENT_WIFI_DISCONNECTED" not in m.group(1) or \
            "LED_EVENT_WIFI_IDLE" not in m.group(1):
        errors.append("networking.c: idle LED event must return DISCONNECTED for "
                      "a configured target and IDLE when there is no target")

    # 3. A Wi-Fi status event must not cancel an active transient notification
    #    (boot green / HTTP blue / alarm cyan), which made the LED sit green.
    fn = re.search(r"void led_notify\(led_event_t event\)\s*\{(.*?)\n\}", led, re.S)
    if not fn or "s_notification.active = false" in fn.group(1).split("if (!s_auto_status)")[0]:
        errors.append("led.c: status events must not cancel active transient "
                      "notifications in auto mode")
    if "case LED_EVENT_HTTPD_STOPPED" not in led or "*frame = s_status_frame" not in led:
        errors.append("led.c: HTTPD_STOPPED must cue back to the status colour, "
                      "not a black default")

    # 4. LED2 must remain the CPU gauge, fed from telemetry through the ops
    #    table (no reverse dependency from led into shell/header).
    ledh = read("components/led/led.h")
    shell = read("components/shell/shell.c")
    command = read("components/command/command.c")
    if "led_set_cpu_usage" not in ledh or "led_set_cpu_usage" not in led:
        errors.append("led: CPU gauge API led_set_cpu_usage must exist")
    if "led_cpu_usage" not in shell or "led_cpu_usage" not in command:
        errors.append("shell/command: CPU telemetry must reach the LED engine "
                      "via shell_command_ops_t")

    # 5. Event colours must remain distinct (no flattening to one colour).
    expected = {
        "BOOT_OK": "0x00FF00",
        "WIFI_CONNECTED": "0x00FF00",
        "WIFI_IDLE": "0x00FF00",
        "WIFI_DISCONNECTED": "0xFF9900",
        "WIFI_ERROR": "0xFF4040",
        "HTTPD": "0x0080FF",
    }
    for event, colour in expected.items():
        macro = "P4_CONFIG_LED_COLOR_" + event
        if macro not in cfg or colour not in cfg:
            errors.append("p4minishell_config.h: %s must remain %s" % (macro, colour))

    # 4. The documented contract must still exist in the docs.
    docs = read("ai-context.md")
    if "Idle matters on boards like the Tab5" not in docs or \
            "connecting=amber pulse" not in docs:
        errors.append("ai-context.md: LED Rules section lost the documented "
                      "auto-status state machine")

    for err in errors:
        print("led_contract: FAIL " + err)
    print("led_contract: %d error(s)" % len(errors))
    return 1 if errors else 0


if __name__ == "__main__":
    sys.exit(main())
