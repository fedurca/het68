#!/bin/bash

# Tento skript vytvoří čistý archiv obsahující POUZE zdrojové kódy a logy

echo "Vytvářím archiv project.tar.gz..."

tar -czf project.tar.gz \
  --exclude='./.git' \
  --exclude='./build' \
  --exclude='./pico-sdk' \
  --exclude='./openocd' \
  --exclude='./picotool' \
  --exclude='./.aider*' \
  --exclude='./datasheets' \
  --exclude='./presentation' \
  --exclude='./.vscode' \
  --exclude='./project.tar.gz' \
  --exclude='*.old' \
  --exclude='*.bak' \
  --exclude='.DS_Store' \
  --exclude='err_build.log' \
.

echo "Archiv 'project.tar.gz' byl úspěšně vytvořen."
