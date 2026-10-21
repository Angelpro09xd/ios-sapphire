"""Guarda el log del kernel que llega por UDP 5140 mientras la interfaz exista.

    uslog.py SALIDA.txt INTERFAZ
"""
import os
import socket
import sys
import time

out, ifname = sys.argv[1], sys.argv[2]
s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
s.bind(('0.0.0.0', 5140))
s.settimeout(2)
n = 0
with open(out, 'w') as f:
    while os.path.exists('/sys/class/net/' + ifname):
        try:
            d, _ = s.recvfrom(65536)
        except socket.timeout:
            continue
        n += len(d)
        f.write(d.decode(errors='replace'))
        f.flush()
print('%s %s: %d bytes' % (time.strftime('%H:%M:%S'), out, n), flush=True)
