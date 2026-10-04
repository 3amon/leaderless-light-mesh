#!/bin/bash
# Start the onboarding watcher detached, without pattern-matching this script.
cd /home/pi/onboard-watch
for pid in $(pgrep -f "onboard-watch/watcher.py"); do
  [ "$pid" != "$$" ] && kill "$pid" 2>/dev/null
done
sleep 1
setsid nohup python3 -u /home/pi/onboard-watch/watcher.py \
    >> /home/pi/onboard-watch/watch.log 2>&1 < /dev/null &
disown
sleep 3
echo "running pids: $(pgrep -f 'onboard-watch/watcher.py' | tr '\n' ' ')"
