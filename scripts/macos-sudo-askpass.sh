#!/usr/bin/env bash
# GUI password prompt for sudo -A (macOS). Used by run-megaflash-mame.sh so
# utun + pf NAT get a System Events dialog instead of a TTY-only sudo prompt.
set -euo pipefail

MSG="${SUDO_ASKPASS_PROMPT:-MegaFlash needs administrator access to create a utun interface and enable pf NAT for guest Wi-Fi (192.168.4.0/24 to the internet).}"

osascript - "$MSG" <<'EOF'
on run argv
  set MSG to item 1 of argv
  tell application "System Events"
    activate
    set result_ to display dialog MSG default answer "" with hidden answer with title "MegaFlash host network" buttons {"Cancel", "OK"} default button "OK"
    if button returned of result_ is "Cancel" then
      error number -128
    end if
    return text returned of result_
  end tell
end run
EOF
