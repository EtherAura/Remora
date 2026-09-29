// On-device app-icon renderer for the desktop app menu (vendor/host-scripts/remora-app-menu.sh).
// Runs in app_process on the device, gets a system Context, and renders each package's launcher
// icon (getApplicationIcon composites adaptive vector icons that can't be pulled from the APK) to
// <outDir>/<pkg>.png.
//
// It lives in the agent so every image built with the mirror_agent feature carries it, in
// /system_ext/framework/remora-agent.jar — it used to be a separately committed dex pushed per
// run, which a public clone could not rebuild. Invoked as its own main class, not through Main:
//   CLASSPATH=/system_ext/framework/remora-agent.jar app_process /system/bin \
//       com.remora.agent.IconDump <outDir> <pkg>...
package com.remora.agent;

import android.app.ActivityThread;
import android.os.Looper;
import android.content.Context;
import android.content.pm.PackageManager;
import android.graphics.Bitmap;
import android.graphics.Canvas;
import android.graphics.drawable.BitmapDrawable;
import android.graphics.drawable.Drawable;
import java.io.FileOutputStream;
public class IconDump {
    public static void main(String[] args) {
        try {
            int size = 192;
            String outDir = args[0];
            if (Looper.myLooper() == null) Looper.prepareMainLooper();
            ActivityThread at = ActivityThread.systemMain();
            Context ctx = at.getSystemContext();
            PackageManager pm = ctx.getPackageManager();
            for (int i = 1; i < args.length; i++) {
                String pkg = args[i];
                try {
                    Drawable d = pm.getApplicationIcon(pkg);
                    Bitmap bmp;
                    if (d instanceof BitmapDrawable && ((BitmapDrawable) d).getBitmap() != null) {
                        bmp = ((BitmapDrawable) d).getBitmap();
                    } else {
                        bmp = Bitmap.createBitmap(size, size, Bitmap.Config.ARGB_8888);
                        Canvas c = new Canvas(bmp);
                        d.setBounds(0, 0, size, size);
                        d.draw(c);
                    }
                    FileOutputStream fos = new FileOutputStream(outDir + "/" + pkg + ".png");
                    bmp.compress(Bitmap.CompressFormat.PNG, 100, fos);
                    fos.close();
                    System.out.println("OK " + pkg);
                } catch (Throwable t) {
                    System.out.println("FAIL " + pkg + " -> " + t);
                }
            }
        } catch (Throwable t) {
            System.out.println("FATAL -> " + t);
        }
    }
}
