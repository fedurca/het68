#!/usr/bin/env bash
set -Eeuo pipefail

MODEL="${MODEL:-ollama_chat/qwen2.5-coder:7b}"
OLLAMA_API_BASE="${OLLAMA_API_BASE:-http://127.0.0.1:11434}"
TEST_CMD="${TEST_CMD:-./build.sh}"
RUN_BOOTSTRAP_PROMPT="${RUN_BOOTSTRAP_PROMPT:-1}"
UNTRACK_GENERATED="${UNTRACK_GENERATED:-1}"

export OLLAMA_API_BASE

say() {
    printf '\033[1;32m[ai.sh]\033[0m %s\n' "$*"
}

warn() {
    printf '\033[1;33m[ai.sh WARNING]\033[0m %s\n' "$*" >&2
}

die() {
    printf '\033[1;31m[ai.sh ERROR]\033[0m %s\n' "$*" >&2
    exit 1
}

need_cmd() {
    command -v "$1" >/dev/null 2>&1 || die "Chybí příkaz: $1"
}

check_repo() {
    git rev-parse --is-inside-work-tree >/dev/null 2>&1 || die "Spusť ai.sh uvnitř git repozitáře."
    cd "$(git rev-parse --show-toplevel)"
    say "Repo root: $(pwd)"
}

check_tools() {
    need_cmd git
    need_cmd curl
    need_cmd aider

    if ! command -v arm-none-eabi-gcc >/dev/null 2>&1; then
        warn "Chybí arm-none-eabi-gcc. Build může selhat."
        warn "Instalace: sudo apt install -y gcc-arm-none-eabi libnewlib-arm-none-eabi"
    fi

    if ! command -v make >/dev/null 2>&1 && ! command -v ninja >/dev/null 2>&1; then
        warn "Chybí make i ninja. Build může selhat."
        warn "Instalace: sudo apt install -y make ninja-build"
    fi
}

check_ollama() {
    say "Testuji Ollama API: $OLLAMA_API_BASE"

    if ! curl -fsS "$OLLAMA_API_BASE/api/tags" >/dev/null; then
        die "Ollama neodpovídá na $OLLAMA_API_BASE/api/tags

Zkus:
  sudo systemctl start ollama

nebo:
  OLLAMA_HOST=127.0.0.1:11434 ollama serve"
    fi

    say "Ollama API odpovídá."

    RAW_MODEL="$MODEL"
    RAW_MODEL="${RAW_MODEL#ollama_chat/}"
    RAW_MODEL="${RAW_MODEL#ollama/}"

    if command -v ollama >/dev/null 2>&1; then
        if ! ollama list | awk 'NR > 1 {print $1}' | grep -Fxq "$RAW_MODEL"; then
            warn "Model '$RAW_MODEL' není v ollama list."
            warn "Stáhni ho: ollama pull $RAW_MODEL"
        else
            say "Model nalezen: $RAW_MODEL"
        fi
    fi
}

write_aiderignore() {
    cat > .aiderignore <<'AIDERIGNORE_EOF'
build/
generated/
pico-sdk/
picotool/

*.uf2
*.elf
*.bin
*.hex
*.map
*.dis
*.list
*.o
*.a
*.log
err_build.log

.DS_Store
.aider.chat.history.md
.aider.input.history
.aider.tags.cache*
.aider.repo.map*
.aider.bootstrap.prompt.md
AIDERIGNORE_EOF

    say "Vytvořena/aktualizována .aiderignore."
}

append_gitignore() {
    touch .gitignore

    if grep -Fq "# ai.sh aider bootstrap" .gitignore; then
        say ".gitignore už obsahuje ai.sh blok."
        return
    fi

    cat >> .gitignore <<'GITIGNORE_EOF'

# ai.sh aider bootstrap
build/
generated/
*.uf2
*.elf
*.bin
*.hex
*.map
*.dis
*.list
*.o
*.a
*.log
err_build.log

pico-sdk/
picotool/

.DS_Store
*.swp
*.swo

.aider.chat.history.md
.aider.input.history
.aider.tags.cache*
.aider.repo.map*
.aider.bootstrap.prompt.md
GITIGNORE_EOF

    say "Doplněn blok do .gitignore."
}

write_agents_md() {
    if [ -f AGENTS.md ] && [ "${FORCE_AGENTS:-0}" != "1" ]; then
        say "AGENTS.md už existuje, nepřepisuji. Pro přepsání spusť: FORCE_AGENTS=1 ./ai.sh"
        return
    fi

    cat > AGENTS.md <<'AGENTS_EOF'
# het68 AI agent rules

## Project

This is a Raspberry Pi Pico 2 / RP2350 embedded USB audio firmware project.

Goal:
- 6-channel USB audio input
- 3x stereo I2S microphone pairs
- I2S RX via PIO
- DMA transfer into memory
- TinyUSB USB Audio device

## Hard rules

- Do not edit build/, generated/, pico-sdk/ or picotool/ unless explicitly asked.
- Do not remove checks or tests to make the build pass.
- Do not introduce malloc/free into the realtime audio path.
- Do not block in IRQ handlers, DMA callbacks or USB audio callbacks.
- Do not change pin mappings without updating documentation.
- Keep USB descriptors and tusb_config.h consistent.
- Keep PIO, DMA and USB frame sizes consistent.
- Prefer small, reviewable changes.
- After firmware code changes, run ./build.sh.

## Build

Build command:

./build.sh

Default target board:

PICO_BOARD=pico2

## Flash/debug assumption

Raspberry Pi Debug Probe is connected externally.

Preferred future flow:
- build ELF/UF2
- flash ELF via OpenOCD/SWD
- use UART/serial logging for smoke tests

## Files/directories AI should usually ignore

- build/
- generated/
- pico-sdk/
- picotool/
- *.uf2
- *.elf
- *.bin
- *.map
- *.log

## Definition of done

- CMake configure passes.
- Firmware build passes.
- ELF/UF2 is produced.
- No generated/vendor files are modified unintentionally.
- Realtime audio constraints remain respected.
AGENTS_EOF

    say "Vytvořen/aktualizován AGENTS.md."
}

write_bootstrap_prompt() {
    cat > .aider.bootstrap.prompt.md <<'PROMPT_EOF'
Use AGENTS.md as the project rules.

Do not modify firmware logic yet.

Tasks:
1. Review AGENTS.md, .gitignore and .aiderignore.
2. Analyze the current project at a high level.
3. Focus on these files if present:
   - main.c
   - CMakeLists.txt
   - usb_descriptors.c
   - tusb_config.h
   - pio/i2s_rx.pio
   - writing_and_bom.md
4. Explain the current data path:
   - I2S PIO RX
   - DMA buffers
   - USB audio descriptors
   - Pico 2 / RP2350 compatibility risks
5. Do not edit C firmware files in this bootstrap step.
6. If AGENTS.md misses important safety rules, update AGENTS.md only.
PROMPT_EOF

    say "Vytvořen bootstrap prompt."
}

untrack_generated() {
    if [ "$UNTRACK_GENERATED" != "1" ]; then
        say "UNTRACK_GENERATED=0, git index neupravuji."
        return
    fi

    for path in build generated pico-sdk picotool; do
        if git ls-files "$path" | grep -q .; then
            say "Vyndávám z git indexu: $path/  — soubory na disku zůstanou."
            git rm -r --cached --ignore-unmatch "$path" >/dev/null
        fi
    done
}

collect_files() {
    AIDER_FILES=()

    for file in AGENTS.md CMakeLists.txt main.c usb_descriptors.c tusb_config.h writing_and_bom.md pio/i2s_rx.pio; do
        if [ -e "$file" ]; then
            AIDER_FILES+=("$file")
        fi
    done

    if [ "${#AIDER_FILES[@]}" -eq 0 ]; then
        die "Nenašel jsem očekávané soubory projektu."
    fi
}

run_bootstrap() {
    if [ "$RUN_BOOTSTRAP_PROMPT" != "1" ]; then
        say "Bootstrap prompt přeskakuji."
        return
    fi

    say "Spouštím jednorázový bootstrap prompt v Aideru."

    aider \
        --model "$MODEL" \
        --aiderignore .aiderignore \
        --message-file .aider.bootstrap.prompt.md \
        "${AIDER_FILES[@]}" || warn "Bootstrap prompt skončil chybou, pokračuji do interaktivního režimu."
}

run_interactive() {
    say "Spouštím interaktivní Aider."
    say "Model: $MODEL"
    say "OLLAMA_API_BASE: $OLLAMA_API_BASE"
    say "Build/test command: $TEST_CMD"

    exec aider \
        --model "$MODEL" \
        --aiderignore .aiderignore \
        --test-cmd "$TEST_CMD" \
        --auto-test \
        "${AIDER_FILES[@]}"
}

main() {
    check_repo
    check_tools
    check_ollama

    write_aiderignore
    append_gitignore
    write_agents_md
    write_bootstrap_prompt
    untrack_generated
    collect_files

    say "Aktuální git stav:"
    git status --short || true

    run_bootstrap
    run_interactive
}

main "$@"
