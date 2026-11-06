#!/bin/sh
# LING OS WebUI kiosk 启动：优先 Chromium，其次 firefox/WebKit；失败回退普通浏览器。
set -u
URL="${LINGOS_UI_URL:-http://127.0.0.1:8080/ui}"
CHROME="$(command -v chromium || command -v chromium-browser || command -v google-chrome || true)"
FIREFOX="$(command -v firefox || true)"
if [ -n "$CHROME" ]; then
  exec "$CHROME" --kiosk --app="$URL" --no-first-run --no-default-browser-check \
    --disable-features=Translate --autoplay-policy=no-user-gesture-required
elif [ -n "$FIREFOX" ]; then
  exec "$FIREFOX" --kiosk "$URL"
else
  echo "未找到 Chromium/Firefox，回退：请手动打开 $URL" >&2
  exit 1
fi
