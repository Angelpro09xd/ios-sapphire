#!/bin/bash
# aptdeps.sh (as root): the packages the Linux xnu build needs (small ones only)
apt-get install -y -q clang-18 lld-18 llvm-18 cmake ninja-build bison flex libssl-dev zlib1g-dev libxml2-dev \
	uuid-dev unifdef tcsh ed gawk pax xxd python3-yaml 2>&1 | tail -3
for t in cmake ninja bison flex unifdef tcsh ed gawk pax xxd clang-18 ld64.lld-18; do printf '%s: %s\n' $t "$(command -v $t || echo MISSING)"; done
