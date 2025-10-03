#!/bin/bash

# This script creates a clean source archive named project.tar.gz

echo "Creating archive..."

tar -czf project.tar.gz \
  --exclude='./.git' \
  --exclude='./build' \
  --exclude='./pico-sdk' \
  --exclude='./datasheets' \
  --exclude='./project.tar.gz' \
  --exclude='./.vscode' \
  --exclude='*.old' \
  --exclude='*.bak' \
  --exclude='lsusb_output.txt' \
  --exclude='.DS_Store' \
  .

echo "Archive 'project.tar.gz' created successfully."
