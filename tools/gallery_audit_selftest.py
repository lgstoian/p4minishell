#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Stoian Alexandru
# SPDX-License-Identifier: MIT
"""Run the gallery identity guard against synthetic bad captures.

A gate that has never been seen failing is not a gate. This is the canary for
`gallery_audit.py`: it must reject a shell transcript dump and a duplicate app
image, and accept a clean directory.
"""
import sys

from gallery_audit import self_test

if __name__ == "__main__":
    sys.exit(self_test())
