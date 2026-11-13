# 0.1.4: iOS software update off (it prepares an OTA it can never apply: restore error 60, a log and CPU each try)
for j in com.apple.mobile.softwareupdated com.apple.MobileSoftwareUpdate.CleanupPreparePathService com.apple.OTATaskingAgent com.apple.softwareupdateservicesd com.apple.OTACrashCopier; do
	launchctl disable system/$j 2>/dev/null
	launchctl bootout system/$j 2>/dev/null
done
true
