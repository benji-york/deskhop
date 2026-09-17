#!/usr/bin/env python3
"""Explicit foreground entrypoint; see docs/clipboard.md for lifecycle/limitations."""
from clipboard.helper import main

if __name__ == '__main__':
    raise SystemExit(main())
