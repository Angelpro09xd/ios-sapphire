#!/bin/bash
# run-tests.sh: the plain-Java parts (IPA inbox layout and range checks, plists, CgBI icons, IPA parsing)
# tested on the PC against files -- nothing here touches a phone. android.jar is on the class path only so
# the classes that mention Android types load; no Android code runs.
set -e
cd "$(dirname "$0")/.."
SDK=${ANDROID_HOME:-$HOME/android-sdk}
JAR=$SDK/platforms/android-36/android.jar
T=$(mktemp -d)
mkdir -p out/test
javac -source 17 -target 17 -nowarn -Xlint:-options -cp $JAR -d out/test $(find src test -name '*.java' -not -name '*Cli.java' -not -name 'HfsWalk.java')
python3 test/mkfixtures.py $T/fixtures
java -cp out/test:$JAR com.sapphire.dualboot.FormatsTest $T/fixtures
java -cp out/test:$JAR com.sapphire.dualboot.IpaInboxTest $T
rm -rf $T
