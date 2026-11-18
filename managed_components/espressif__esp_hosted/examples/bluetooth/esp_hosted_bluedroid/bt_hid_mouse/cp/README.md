# BT Classic HID Mouse — Bluedroid over Hosted HCI — Coprocessor

Acts as a Bluetooth Classic HID mouse — pairs with a host (PC or phone) and
sends mouse-movement reports. Ports upstream `host_bluedroid_bt_hid_mouse_device`
verbatim; the only ESP-Hosted-specific change is one call —
`esp_hosted_bt_host_stack_setup()` — that brings the controller up and binds Bluedroid
to the hosted HCI. Bluedroid
+ the HID device profile run
on the **host**; the BR/EDR **controller** runs on the ESP-Hosted **co-processor**,
reached over the hosted transport (SDIO / SPI / SPI-HD / UART) via VHCI.

**Classic Bluetooth (BR/EDR) needs an ESP32 or ESP32-S31 co-processor.** The
other chips are BLE-only. The host build stops with an error if the selected
co-processor has no Classic BT.

## Supported Platforms and Transports

### Supported Coprocessors (BR/EDR controller)

| Coprocessor       | ESP32 | ESP32-C2 | ESP32-C3 | ESP32-C5 | ESP32-C6 | ESP32-C61 | ESP32-H2 | ESP32-S3 | ESP32-S31 | ESP32-S2 |
| :---------------- | :---: | :------: | :------: | :------: | :------: | :-------: | :------: | :------: | :-------: | :------: |
| Classic BT (BR/EDR) | Yes | No       | No       | No       | No       | No        | No       | No       | Yes | No       |

### Supported Host Devices

| Host Device | ESP32-P4 | ESP32-H2 | Other MCUs |
| :---------- | :------: | :------: | :--------: |
| Support     | Yes      | Yes      | [Yes](https://github.com/espressif/esp-hosted/blob/master/docs/getting-started-mcu.md) |

### Supported HCI transports

| HCI over hosted bus (VHCI) | SDIO | SPI Full-Duplex | SPI Half-Duplex | UART |
| :------------------------- | :--: | :-------------: | :-------------: | :--: |
| MCU host                   | Yes  | Yes             | Yes             | Yes  |

The co-processor is the BR/EDR (Classic Bluetooth) controller. Use an ESP32 or
ESP32-S31 CP. `sdkconfig.defaults` already enables the
controller-only profile with BT HCI carried over the host bus (VHCI); you only
select the transport (must match the host):

```bash
cd examples/bluetooth/esp_hosted_bluedroid/bt_hid_mouse/cp
idf.py set-target esp32              # or: idf.py --preview set-target esp32s31
idf.py menuconfig
```

```text
Component config
└── ESP-Hosted
     └── Configure coprocessor
          └── CP transport
               ├── Communication bus (co-processor <== bus ==> host)
               │    ├── ( ) SPI Full Duplex
               │    ├── (X) SDIO                    <── default
               │    ├── ( ) SPI Half Duplex         ← MCU host only
               │    └── ( ) UART                    ← MCU host only
               └── SDIO Configuration               ← clock, GPIOs, checksum (defaults OK)
```

The Bluetooth profile is preselected by `sdkconfig.defaults`; confirm it under
**Features**:

```text
Component config
└── ESP-Hosted
     └── Configure coprocessor
          └── Features
               └── Bluetooth                              <── enabled by sdkconfig.defaults
                    ├── [*] Auto-initialise Bluetooth at boot
                    └── BT HCI transport
                         ├── (X) HCI over VHCI (SPI/SDIO/SPI-HD)   <── default
                         └── ( ) HCI over UART
```

The CP dependency config is pre-set in `sdkconfig.defaults` (do not remove):

```text
CONFIG_ESP_HOSTED_CP_FEAT_BT=y            # ESP-Hosted BT controller feature
CONFIG_ESP_HOSTED_CP_BT_ENABLED=y         # required by ESP_HOSTED_CP_FEAT_BT
CONFIG_ESP_HOSTED_CP_FEAT_BT_HCI_VHCI=y   # BT HCI over the host bus (VHCI)
CONFIG_BT_ENABLED=y                       # BT controller stack on the CP
CONFIG_BT_CONTROLLER_ONLY=y               # controller-only profile
CONFIG_ESP_HOSTED_CP_FEAT_WIFI=n          # Wi-Fi off — BT controller-only CP
```

Classic BT (BR/EDR) also needs an ESP32 or ESP32-S31 CP. On other chips, the
controller-only profile above gives BLE only.

Then flash and monitor:

```bash
idf.py -p <cp_usb_serial_port> flash monitor
```

<!-- generated from ../README.md — edit that file, not this one -->
