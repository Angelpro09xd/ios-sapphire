#!/usr/bin/env python3
# iigfix.py IIG_TOOLS_SRC: iig-lite counted "= 0" methods with a TARGET parameter (DriverKit action
# declarations such as IODataQueueDispatchSource::DataAvailable, which the generated header does not declare
# at all) as pure virtual, so the class got OSDefineMetaClassAndAbstractStructors and no default
# constructor, unlike Apple's iig (IOKit.exports lists IODataQueueDispatchSource::IODataQueueDispatchSource()).
import sys
from pathlib import Path

p = Path(sys.argv[1]) / 'src/codegen.cpp'
t = p.read_text()
old = '      if (m.isPureVirtual) {\n        isAbstract = true;'
new = '      if (m.isPureVirtual && !m.targetParam()) {\n        isAbstract = true;'
if new not in t:
    assert t.count(old) == 1
    p.write_text(t.replace(old, new))
print('ok')
