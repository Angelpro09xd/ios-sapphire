# 0.1.7: the Metal driver of HackiOS (HakiosMetal.bundle, ported to the Adreno 610) draws the screen (kernel sweetmetal=2).
# Its compiled shader stages and CoreAnimation specialisations are kept here across boots; backboardd runs as mobile,
# so the directory is writable by all (as root 755 the cache went to /var/tmp).
mkdir -p /private/var/db/hakios-metal-cache
chown mobile:mobile /private/var/db/hakios-metal-cache
chmod 1777 /private/var/db/hakios-metal-cache
chmod 755 /System/Library/Extensions/HakiosMetal.bundle/HakiosMetal
