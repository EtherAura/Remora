package io.github.etheraura.remora.phone;

import android.app.Activity;
import android.content.Intent;
import android.os.Build;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.provider.Settings;
import android.widget.TextView;

/**
 * Pairs with the computer a scanned code names. The code arrives as its link (the phone's camera
 * opens remora-link:// here), and pairing is one request: the one-time token, and the name the
 * computer should know this phone by.
 */
public final class PairActivity extends Activity {
    @Override
    protected void onCreate(Bundle saved) {
        super.onCreate(saved);
        TextView status = new TextView(this);
        int pad = Math.round(24 * getResources().getDisplayMetrics().density);
        status.setPadding(pad, pad, pad, pad);
        status.setTextSize(17);
        setContentView(status);

        Intent in = getIntent();
        Link.PairingCode code = in.getData() != null ? Link.PairingCode.parse(in.getData().toString())
                                                     : null;
        if (code == null) {
            status.setText("That is not a Remora pairing code.");
            return;
        }
        status.setText("Pairing with " + code.hostName + "…");
        Handler ui = new Handler(Looper.getMainLooper());
        new Thread(() -> {
            try {
                Link.Host host = Link.pair(this, code, deviceName());
                ui.post(() -> {
                    status.setText("Paired with " + host.name + ".");
                    startActivity(new Intent(this, MainActivity.class)
                            .addFlags(Intent.FLAG_ACTIVITY_CLEAR_TOP));
                    finish();
                });
            } catch (Exception e) {
                ui.post(() -> status.setText("Could not pair with " + code.hostName + ":\n\n"
                        + e.getMessage()));
            }
        }, "pair").start();
    }

    /** The name the person gave the phone, else its model. */
    private String deviceName() {
        String name = Settings.Global.getString(getContentResolver(), Settings.Global.DEVICE_NAME);
        return name != null && !name.isEmpty() ? name : Build.MODEL;
    }
}
