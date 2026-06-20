#!/usr/bin/env bash
# ssh_hardening.sh — SSH hardening + fail2ban pro Ubuntu 24.04
#
# Co skript dělá:
#  1. Vygeneruje ED25519 SSH klíč (pokud neexistuje) a zobrazí public key
#  2. Zkontroluje že authorized_keys obsahuje klíč před zakázáním hesla
#  3. Zpevní sshd_config (zakáže root login, hesla, prázdná hesla)
#  4. Změní SSH port na 2222 (volitelné — zakomentuj pokud nechceš)
#  5. Nainstaluje a nakonfiguruje fail2ban
#  6. Nastaví UFW firewall
#  7. Restartuje sshd

set -euo pipefail

# ── Konfigurace ───────────────────────────────────────────────────────────────
SSH_PORT=22          # Změň na 2222 nebo jiný pokud chceš nestandardní port
SSH_USER="${USER}"
MAX_AUTH_TRIES=3
MAX_SESSIONS=5
# ─────────────────────────────────────────────────────────────────────────────

RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'; NC='\033[0m'
info()  { echo -e "${GREEN}[INFO]${NC}  $*"; }
warn()  { echo -e "${YELLOW}[WARN]${NC}  $*"; }
error() { echo -e "${RED}[ERROR]${NC} $*"; exit 1; }

require_root() {
    [ "$EUID" -eq 0 ] || error "Spusť skript jako root: sudo bash $0"
}

# ── 1. SSH klíč pro aktuálního uživatele ─────────────────────────────────────
setup_ssh_key() {
    local key_file="/home/${SSH_USER}/.ssh/id_ed25519"
    local auth_file="/home/${SSH_USER}/.ssh/authorized_keys"

    mkdir -p "/home/${SSH_USER}/.ssh"
    chmod 700 "/home/${SSH_USER}/.ssh"
    chown "${SSH_USER}:${SSH_USER}" "/home/${SSH_USER}/.ssh"

    if [ ! -f "${key_file}" ]; then
        info "Generuji ED25519 SSH klíč pro ${SSH_USER}..."
        sudo -u "${SSH_USER}" ssh-keygen -t ed25519 -C "${SSH_USER}@$(hostname)" \
            -f "${key_file}" -N ""
    else
        info "SSH klíč již existuje: ${key_file}"
    fi

    echo
    echo "══════════════════════════════════════════════════════════════"
    echo "  PUBLIC KEY — zkopíruj do Cursor / notebooku:"
    echo "══════════════════════════════════════════════════════════════"
    cat "${key_file}.pub"
    echo "══════════════════════════════════════════════════════════════"
    echo
    echo "  Na notebooku přidej do ~/.ssh/authorized_keys NEBO spusť:"
    echo "  ssh-copy-id -i ${key_file}.pub ${SSH_USER}@<tailscale-ip>"
    echo

    # Přidej vlastní klíč do authorized_keys (pro lokální přístup / testování)
    if ! grep -qF "$(cat "${key_file}.pub")" "${auth_file}" 2>/dev/null; then
        cat "${key_file}.pub" >> "${auth_file}"
        chmod 600 "${auth_file}"
        chown "${SSH_USER}:${SSH_USER}" "${auth_file}"
        info "Klíč přidán do authorized_keys"
    fi
}

# ── 2. Ověření klíčové autentizace ───────────────────────────────────────────
check_key_auth() {
    local auth_file="/home/${SSH_USER}/.ssh/authorized_keys"
    if [ ! -s "${auth_file}" ]; then
        error "authorized_keys je prázdný! Nejdřív přidej SSH klíč ze svého notebooku,
       pak spusť skript znovu. Jinak se zamkneš ven!

       Na notebooku: ssh-keygen -t ed25519 (pokud nemáš)
       Pak: ssh-copy-id ${SSH_USER}@<ip>"
    fi
    info "authorized_keys obsahuje $(wc -l < "${auth_file}") klíč(ů) — OK"
}

# ── 3. Hardening sshd_config ─────────────────────────────────────────────────
harden_sshd() {
    local cfg="/etc/ssh/sshd_config"
    local bak="/etc/ssh/sshd_config.bak.$(date +%Y%m%d_%H%M%S)"

    info "Zálohuji ${cfg} → ${bak}"
    cp "${cfg}" "${bak}"

    # Funkce pro nastavení nebo přidání parametru
    set_param() {
        local key="$1" val="$2"
        if grep -qE "^#?${key}\s" "${cfg}"; then
            sed -i "s|^#\?${key}\s.*|${key} ${val}|" "${cfg}"
        else
            echo "${key} ${val}" >> "${cfg}"
        fi
    }

    set_param Port               "${SSH_PORT}"
    set_param AddressFamily      inet            # pouze IPv4; odstraň pro IPv6
    set_param PermitRootLogin    no
    set_param MaxAuthTries       "${MAX_AUTH_TRIES}"
    set_param MaxSessions        "${MAX_SESSIONS}"
    set_param PubkeyAuthentication yes
    set_param AuthorizedKeysFile ".ssh/authorized_keys"
    set_param PasswordAuthentication no          # zakáže hesla
    set_param PermitEmptyPasswords no
    set_param ChallengeResponseAuthentication no
    set_param UsePAM             yes
    set_param X11Forwarding      no
    set_param PrintMotd          no
    set_param AcceptEnv          "LANG LC_*"
    set_param Subsystem          "sftp /usr/lib/openssh/sftp-server"
    set_param LoginGraceTime     20
    set_param ClientAliveInterval 120
    set_param ClientAliveCountMax 2
    # Pouze silné šifry
    set_param KexAlgorithms      "curve25519-sha256,curve25519-sha256@libssh.org,ecdh-sha2-nistp256"
    set_param Ciphers            "aes256-gcm@openssh.com,chacha20-poly1305@openssh.com,aes128-gcm@openssh.com"
    set_param MACs               "hmac-sha2-256-etm@openssh.com,hmac-sha2-512-etm@openssh.com"

    info "Ověřuji sshd_config..."
    sshd -t && info "sshd_config je syntakticky správný" \
             || error "Chyba v sshd_config! Obnov ze zálohy: cp ${bak} ${cfg}"
}

# ── 4. Fail2ban ───────────────────────────────────────────────────────────────
setup_fail2ban() {
    info "Instaluji fail2ban..."
    apt-get install -y fail2ban

    cat > /etc/fail2ban/jail.local << EOF
[DEFAULT]
bantime  = 1h
findtime = 10m
maxretry = ${MAX_AUTH_TRIES}
backend  = systemd

[sshd]
enabled  = true
port     = ${SSH_PORT}
logpath  = %(sshd_log)s
maxretry = ${MAX_AUTH_TRIES}
bantime  = 24h

[sshd-aggressive]
enabled  = true
port     = ${SSH_PORT}
logpath  = %(sshd_log)s
maxretry = 2
bantime  = 7d
findtime = 1d
filter   = sshd[mode=aggressive]
EOF

    systemctl enable fail2ban
    systemctl restart fail2ban
    info "fail2ban nastaven (ban po ${MAX_AUTH_TRIES} pokusech / 1h, agresivní ban 7d)"
}

# ── 5. UFW firewall ───────────────────────────────────────────────────────────
setup_ufw() {
    if ! command -v ufw &>/dev/null; then
        apt-get install -y ufw
    fi

    ufw --force reset
    ufw default deny incoming
    ufw default allow outgoing
    ufw allow "${SSH_PORT}/tcp" comment "SSH"
    # Přidej další porty pokud potřebuješ:
    # ufw allow 80/tcp
    # ufw allow 443/tcp
    ufw --force enable
    info "UFW firewall aktivován, povoleno SSH na portu ${SSH_PORT}"
    ufw status verbose
}

# ── 6. Restart sshd ───────────────────────────────────────────────────────────
restart_sshd() {
    systemctl restart sshd
    info "sshd restartován"
}

# ── Hlavní tok ────────────────────────────────────────────────────────────────
main() {
    require_root

    echo
    echo "╔══════════════════════════════════════════════════╗"
    echo "║   SSH Hardening Script — Ubuntu 24.04            ║"
    echo "║   SSH port: ${SSH_PORT}  |  Uživatel: ${SSH_USER}          ║"
    echo "╚══════════════════════════════════════════════════╝"
    echo

    setup_ssh_key
    check_key_auth

    read -rp "Pokračovat? Zaheslové přihlášení bude ZAKÁZÁNO. [y/N] " confirm
    [[ "${confirm,,}" == "y" ]] || { echo "Přerušeno."; exit 0; }

    harden_sshd
    setup_fail2ban
    setup_ufw
    restart_sshd

    echo
    echo "╔══════════════════════════════════════════════════╗"
    echo "║   HOTOVO — SSH je zabezpečené                    ║"
    echo "╚══════════════════════════════════════════════════╝"
    echo
    echo "  Nech toto SSH sezení OTEVŘENÉ a v novém okně otestuj:"
    echo "  ssh -p ${SSH_PORT} ${SSH_USER}@<ip>"
    echo
    echo "  Cursor / VS Code konfigurace (~/.ssh/config na notebooku):"
    echo "  Host lab-pc"
    echo "      HostName <tailscale-ip>"
    echo "      User ${SSH_USER}"
    echo "      Port ${SSH_PORT}"
    echo "      IdentityFile ~/.ssh/id_ed25519"
    echo "      ServerAliveInterval 120"
    echo
}

main "$@"
