"""Where the journal lives, from the journal info block: jib.py IMG."""
import struct
import sys

with open(sys.argv[1], 'rb') as f:
    f.seek(1024)
    v = f.read(512)
    assert v[:2] == b'HX', v[:2]
    jib_block = struct.unpack_from('>I', v, 12)[0]
    f.seek(jib_block * 4096)
    d = f.read(4096)
flags, = struct.unpack_from('>I', d, 0)
offset, size = struct.unpack_from('>QQ', d, 36)
print(jib_block, offset, size, '%#x' % flags)
