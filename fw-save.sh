#!/usr/bin/env bash
# fw-save.sh — backup the current build, or flash a saved backup to the device
#
# Usage:
#   ./fw-save.sh [label]          save current build as firmware/backups/<label>.bin
#                                 (label defaults to YYYY-MM-DD_HHMMSS)
#   ./fw-save.sh --flash [file]   flash <file> (or the latest backup) to the device

set -euo pipefail

DEVICE="${DEVICE:-/dev/ttyACM0}"
BUILD_BIN="firmware/.esphome/build/transit-tracker/.pioenvs/transit-tracker/firmware.factory.bin"
BACKUP_DIR="firmware/backups"

flash_backup() {
    local target="$1"
    if [[ ! -f "$target" ]]; then
        echo "Error: file not found: $target" >&2
        exit 1
    fi
    echo "Flashing $target -> $DEVICE ..."
    esptool.py --chip esp32s3 --port "$DEVICE" write_flash 0x0 "$target"
}

if [[ "${1:-}" == "--flash" ]]; then
    if [[ -n "${2:-}" ]]; then
        flash_backup "$2"
    else
        latest=$(ls -t "$BACKUP_DIR"/*.bin 2>/dev/null | head -1)
        if [[ -z "$latest" ]]; then
            echo "Error: no backups found in $BACKUP_DIR" >&2
            exit 1
        fi
        echo "Using latest backup: $latest"
        flash_backup "$latest"
    fi
else
    if [[ ! -f "$BUILD_BIN" ]]; then
        echo "Error: build output not found: $BUILD_BIN" >&2
        echo "Run 'esphome compile firmware/local-tracker-vars.yaml' first." >&2
        exit 1
    fi
    label="${1:-$(date +%Y-%m-%d_%H%M%S)}"
    dest="$BACKUP_DIR/${label}.bin"
    mkdir -p "$BACKUP_DIR"
    cp "$BUILD_BIN" "$dest"
    echo "Saved: $dest ($(du -h "$dest" | cut -f1))"
fi
