package io.github.etheraura.remora.phone;

import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.PendingIntent;
import android.app.Service;
import android.content.Intent;
import android.content.pm.ServiceInfo;
import android.os.IBinder;

/**
 * What lets a session outlive the screen. A few seconds after an app leaves the screen, Android
 * (15 and later) destroys its network connections and mutes its sound, unless it runs a foreground
 * service. This one runs exactly as long as SessionActivity, holds nothing itself, and its
 * notification brings the session back.
 */
public final class SessionService extends Service {
    static final String APP_NAME = "appName";
    private static final String CHANNEL = "session";
    private static final int NOTIFICATION = 1;

    @Override
    public int onStartCommand(Intent intent, int flags, int startId) {
        NotificationManager nm = getSystemService(NotificationManager.class);
        nm.createNotificationChannel(new NotificationChannel(
                CHANNEL, "Open apps", NotificationManager.IMPORTANCE_LOW));
        // The session as it was: the running SessionActivity brought forward, never a new one.
        Intent back = new Intent(this, SessionActivity.class)
                .addFlags(Intent.FLAG_ACTIVITY_NEW_TASK | Intent.FLAG_ACTIVITY_REORDER_TO_FRONT);
        PendingIntent open = PendingIntent.getActivity(this, 0, back,
                PendingIntent.FLAG_IMMUTABLE | PendingIntent.FLAG_UPDATE_CURRENT);
        String app = intent != null ? intent.getStringExtra(APP_NAME) : null;
        Notification n = new Notification.Builder(this, CHANNEL)
                .setSmallIcon(R.drawable.ic_session)
                .setContentTitle(app != null ? app : getString(R.string.app_name))
                .setContentText("Still open. Tap to go back to it.")
                .setContentIntent(open)
                .setOngoing(true)
                .build();
        startForeground(NOTIFICATION, n, ServiceInfo.FOREGROUND_SERVICE_TYPE_MEDIA_PLAYBACK);
        return START_NOT_STICKY;
    }

    @Override
    public IBinder onBind(Intent intent) {
        return null;
    }
}
