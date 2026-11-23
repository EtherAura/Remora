package com.remora.agent;

import android.content.pm.PackageManager;
import android.graphics.Bitmap;
import android.graphics.Canvas;
import android.graphics.drawable.Drawable;

import java.io.ByteArrayOutputStream;

/**
 * kind=app-icon: an app's launcher icon as a PNG, served like list-apps — what a client's launcher
 * needs without a shell on the device (the phone app's section for a profile's Android apps). The
 * desktop's app menu draws its icons the same way, through IconDump.
 */
final class AppIcon {
    // The host agent's bounds for the same request.
    static final int MIN_SIZE = 16, MAX_SIZE = 512;

    /**
     * The launcher icon drawn at size×size. getApplicationIcon composites an adaptive icon (its
     * layers and the device's mask), which no file in the APK holds.
     */
    static Bitmap render(PackageManager pm, String pkg, int size)
            throws PackageManager.NameNotFoundException {
        Drawable d = pm.getApplicationIcon(pkg);
        Bitmap bmp = Bitmap.createBitmap(size, size, Bitmap.Config.ARGB_8888);
        d.setBounds(0, 0, size, size);
        d.draw(new Canvas(bmp));
        return bmp;
    }

    static byte[] png(String pkg, int size) throws PackageManager.NameNotFoundException {
        if (pkg == null || pkg.isEmpty()) throw new PackageManager.NameNotFoundException("");
        Bitmap bmp = render(Main.context().getPackageManager(), pkg,
                            Math.max(MIN_SIZE, Math.min(MAX_SIZE, size)));
        ByteArrayOutputStream out = new ByteArrayOutputStream();
        bmp.compress(Bitmap.CompressFormat.PNG, 100, out);
        return out.toByteArray();
    }

    private AppIcon() {}
}
