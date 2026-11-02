#!/usr/bin/env bash
# 在服务器上运行：sudo /opt/warden/configure.sh
# 输入不会回显，也不会写进 shell history。
set -euo pipefail

if [[ ${EUID:-$(id -u)} -ne 0 ]]; then
    echo "请用 root 运行：sudo $0" >&2
    exit 1
fi

ENV_FILE=/etc/warden/warden.env
install -d -o root -g warden -m 0750 /etc/warden

read -r -p "BotFather token: " BOT_TOKEN
if [[ -z "${BOT_TOKEN}" || "${BOT_TOKEN}" =~ [[:space:]] ]]; then
    echo "token 不能为空或包含空白" >&2
    exit 1
fi

read -r -p "Owner Telegram user IDs（逗号分隔）: " OWNERS
if [[ -z "${OWNERS}" ]]; then
    echo "至少填一个 owner user ID" >&2
    exit 1
fi

read -r -s -p "Drain control token（空 = 禁用 /drain）: " CONTROL_TOKEN
echo
read -r -p "Feishu webhook（空 = 不启用）: " FEISHU_URL
read -r -p "Discord webhook（空 = 不启用）: " DISCORD_URL

for value in "${CONTROL_TOKEN}" "${FEISHU_URL}" "${DISCORD_URL}"; do
    if [[ "${value}" =~ [[:space:]] ]]; then
        echo "控制 token / webhook 不能包含空白" >&2
        exit 1
    fi
done

umask 077
{
    printf 'TG_BOT_TOKEN=%s\n' "${BOT_TOKEN}"
    printf 'WARDEN_OWNERS=%s\n' "${OWNERS}"
    printf 'WARDEN_MODE=poll\n'
    printf 'WARDEN_DB=/var/lib/warden/warden.db\n'
    printf 'WARDEN_OFFSET_FILE=/var/lib/warden/warden.offset\n'
    printf 'WARDEN_LOG_DIR=/var/lib/warden/logs\n'
    printf 'WARDEN_LOG_LEVEL=info\n'
    printf 'WARDEN_HEALTH_HOST=127.0.0.1\n'
    printf 'WARDEN_HEALTH_PORT=9090\n'
    printf 'WARDEN_GROUP_MENUS=true\n'
    printf 'WARDEN_REPLY_TTL=20\n'
    printf 'WARDEN_DROP_PENDING=false\n'
    printf 'WARDEN_UPDATE_WORKERS=8\n'
    printf 'WARDEN_MAX_PENDING=2000\n'
    printf 'WARDEN_MAX_PENDING_MB=32\n'
    if [[ -n "${CONTROL_TOKEN}" ]]; then
        printf 'WARDEN_CONTROL_TOKEN=%s\n' "${CONTROL_TOKEN}"
    fi
    if [[ -n "${FEISHU_URL}" ]]; then
        printf 'WARDEN_ALERT_FEISHU_URL=%s\n' "${FEISHU_URL}"
    fi
    if [[ -n "${DISCORD_URL}" ]]; then
        printf 'WARDEN_ALERT_DISCORD_URL=%s\n' "${DISCORD_URL}"
    fi
} > "${ENV_FILE}"

chown root:warden "${ENV_FILE}"
chmod 0640 "${ENV_FILE}"

install -d -o warden -g warden -m 0750 /var/lib/warden /var/lib/warden/logs

systemctl daemon-reload
systemctl enable warden
systemctl restart warden
systemctl --no-pager --lines=15 status warden || true
