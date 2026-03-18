# Licence Notice

## Project code
Unless a file or directory states otherwise, the original project-specific code in this repository is:

Copyright (c) 2026 Stoian Alexandru
All rights reserved.

No permission is granted to copy, redistribute, sublicense, publish, or use the original project-specific source, documentation, or compiled outputs except with prior written permission from Stoian Alexandru.

This proprietary notice applies to the project-authored parts of the repository, including the shell application logic, parser and command-dispatch maintenance fixes, serial-console bridge logic, project documentation, board metadata, command reference files, roadmap notes, and other original files created for P4MiniShell.

## Third-party code
This repository also contains third-party components and dependencies that remain under their own licenses. Those licenses are not replaced by the proprietary notice above.

### Verified third-party licenses in this workspace
- `managed_components/espressif__esp_hosted`: Apache License 2.0
- `managed_components/espressif__esp_wifi_remote`: Apache License 2.0
- `managed_components/espressif__esp_lvgl_port`: Apache License 2.0
- `managed_components/espressif__esp_lcd_jd9165`: Apache License 2.0
- `managed_components/espressif__esp_lcd_touch`: Apache License 2.0
- `managed_components/espressif__esp_lcd_touch_gt911`: Apache License 2.0
- `managed_components/lvgl__lvgl`: MIT License
- `managed_components/espressif__esp_hosted/common/protobuf-c`: BSD-style license from the protobuf-c project

### Project dependencies that should also be respected
- ESP-IDF and Espressif BSP components used by this project are distributed under their own licenses, commonly Apache License 2.0 in the checked-in Espressif components.
- Additional notices inside `managed_components`, `coprocessor`, or future imported components must continue to be preserved exactly as provided by their upstream licensors.

## Distribution rule
Any distribution of this repository or derivative work must preserve:
- this `licence.md` file
- all third-party `LICENSE`, `LICENCE`, `license.txt`, or equivalent notice files shipped with dependencies
- all upstream copyright and attribution notices required by those dependencies

## Scope clarification
This file is a project-level notice only. It is not legal advice, and it does not rewrite the license terms of third-party code already included in the repository.