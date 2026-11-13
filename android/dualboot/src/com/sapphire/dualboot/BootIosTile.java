package com.sapphire.dualboot;

import android.app.PendingIntent;
import android.content.Intent;
import android.os.Build;
import android.service.quicksettings.Tile;
import android.service.quicksettings.TileService;

/**
 * The Quick Settings tile "Reiniciar en iOS". It never switches by itself: it opens the app with
 * ACTION_BOOT_IOS, which shows the same confirmation and then runs the same checked path (bootIos) as the
 * button on Inicio.
 */
public class BootIosTile extends TileService {
	@Override
	public void onStartListening() {
		Tile t = getQsTile();
		if (t != null) {
			t.setState(Tile.STATE_INACTIVE);
			t.setLabel(L.t("Reiniciar en iOS", "Restart to iOS"));
			if (Build.VERSION.SDK_INT >= 29) {
				t.setSubtitle(L.t("Pide confirmación", "Asks for confirmation"));
			}
			t.updateTile();
		}
	}

	@Override
	public void onClick() {
		Intent i = new Intent(MainActivity.ACTION_BOOT_IOS).setClass(this, MainActivity.class)
		    .addFlags(Intent.FLAG_ACTIVITY_NEW_TASK | Intent.FLAG_ACTIVITY_SINGLE_TOP);
		Runnable open = () -> {
			if (Build.VERSION.SDK_INT >= 34) {
				startActivityAndCollapse(PendingIntent.getActivity(this, 0, i, PendingIntent.FLAG_IMMUTABLE | PendingIntent.FLAG_UPDATE_CURRENT));
			} else {
				startActivityAndCollapse(i);
			}
		};
		if (isLocked()) {
			unlockAndRun(open);         /* restarting the phone is not for a locked screen */
		} else {
			open.run();
		}
	}
}
