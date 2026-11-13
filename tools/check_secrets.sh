#!/usr/bin/env sh
# Fails if anything secret-looking is committed. Run by CI on every push.
set -e
cd "$(dirname "$0")/.."
fail=0

if git ls-files --error-unmatch firmware/secrets.h >/dev/null 2>&1; then
  echo "FAIL firmware/secrets.h is committed: it must stay local (it is in .gitignore)"; fail=1
else
  echo "ok   firmware/secrets.h is not in the repository"
fi

# Credentials must only ever come from secrets.h or NVS, never be written in the sketch.
if grep -nE '#define[[:space:]]+(WIFI_SSID|WIFI_PASSWORD|MQTT_USER|MQTT_PASSWORD)' firmware/*.ino; then
  echo "FAIL credentials defined inside the firmware source"; fail=1
else
  echo "ok   no credentials defined in firmware/*.ino"
fi

# The browser config must not carry a password (anything shipped to a browser is public).
if grep -nE 'password:[[:space:]]*"[^"]+"' dashboard/config.js; then
  echo "FAIL dashboard/config.js contains a password"; fail=1
else
  echo "ok   no password in dashboard/config.js"
fi

# Common key formats anywhere in tracked files.
if git ls-files -z | xargs -0 grep -nIE '(AKIA[0-9A-Z]{16}|-----BEGIN [A-Z ]*PRIVATE KEY-----|ghp_[A-Za-z0-9]{36})' 2>/dev/null; then
  echo "FAIL something that looks like a key or token is committed"; fail=1
else
  echo "ok   no API keys, tokens or private keys in tracked files"
fi
exit $fail
