# tester bug 10: IPAs queued by the Android app (inbox after iOS volume), installed once SpringBoard is up (in the background)
setsid nohup /var/jb/bin/bash /redwood/bin/sapphire-ipa-install.sh >/dev/null 2>&1 &
