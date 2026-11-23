package io.github.etheraura.remora.phone;

import android.Manifest;
import android.app.Activity;
import android.content.Context;
import android.content.Intent;
import android.content.pm.PackageManager;
import android.graphics.Color;
import android.graphics.ImageFormat;
import android.hardware.camera2.CameraAccessException;
import android.hardware.camera2.CameraCaptureSession;
import android.hardware.camera2.CameraCharacteristics;
import android.hardware.camera2.CameraDevice;
import android.hardware.camera2.CameraManager;
import android.hardware.camera2.CaptureRequest;
import android.hardware.camera2.params.OutputConfiguration;
import android.hardware.camera2.params.SessionConfiguration;
import android.hardware.camera2.params.StreamConfigurationMap;
import android.media.Image;
import android.media.ImageReader;
import android.net.Uri;
import android.os.Bundle;
import android.os.Handler;
import android.os.HandlerThread;
import android.os.Looper;
import android.util.Size;
import android.view.Surface;
import android.view.SurfaceHolder;
import android.view.SurfaceView;
import android.view.View;
import android.view.ViewGroup;
import android.widget.Button;
import android.widget.EditText;
import android.widget.FrameLayout;
import android.widget.LinearLayout;
import android.widget.TextView;

import java.nio.ByteBuffer;
import java.util.Arrays;

/**
 * Pairing from inside the app: the back camera, read for the code the computer shows (`remora
 * link pair`, or the workspace's Phone page). The code's link goes to PairActivity, as when the
 * phone's own camera app opens it. The link can be pasted instead, which is also the way on a
 * device with no camera.
 */
public final class ScanActivity extends Activity implements SurfaceHolder.Callback {
    private static final int ASK_CAMERA = 1;
    // Enough pixels for a dense code a forearm away, few enough to read several frames a second.
    private static final int MAX_PIXELS = 1920 * 1080;

    private final Handler main = new Handler(Looper.getMainLooper());
    private CameraManager cameras;
    private String cameraId;
    private Size frameSize, previewSize;
    private int afMode = CaptureRequest.CONTROL_AF_MODE_OFF;
    private boolean sideways;  // the sensor is mounted at a right angle to the screen

    private SurfaceView preview;
    private TextView status;
    private boolean resumed, surfaceReady, asked;

    // The camera's state, touched on the main thread only; gen ends a camera that opens late.
    private int gen;
    private boolean opening;
    private CameraDevice camera;
    private CameraCaptureSession session;
    private ImageReader reader;
    private HandlerThread frames;

    private volatile boolean done;
    private byte[] lum;  // the frames thread's

    @Override
    protected void onCreate(Bundle saved) {
        super.onCreate(saved);
        LinearLayout col = new LinearLayout(this);
        col.setOrientation(LinearLayout.VERTICAL);
        col.setFitsSystemWindows(true);
        int pad = dp(16);

        Fit fit = new Fit(this);
        fit.setBackgroundColor(Color.BLACK);
        preview = new SurfaceView(this);
        preview.getHolder().addCallback(this);
        fit.addView(preview);
        col.addView(fit, new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, 0, 1f));

        status = new TextView(this);
        status.setTextSize(15);
        status.setPadding(pad, pad, pad, dp(8));
        col.addView(status);

        LinearLayout row = new LinearLayout(this);
        row.setPadding(pad, 0, pad, pad);
        EditText paste = new EditText(this);
        paste.setHint("or paste the link: remora-link://pair?…");
        paste.setSingleLine(true);
        row.addView(paste, new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f));
        Button pair = new Button(this);
        pair.setText("Pair");
        pair.setOnClickListener(v -> {
            String text = paste.getText().toString().trim();
            if (Link.PairingCode.parse(text) != null) pair(text);
            else status.setText("That is not a Remora pairing link.");
        });
        row.addView(pair);
        col.addView(row);
        setContentView(col);

        cameras = getSystemService(CameraManager.class);
        if (pickCamera()) {
            preview.getHolder().setFixedSize(previewSize.getWidth(), previewSize.getHeight());
            // The preview is upright in the phone's natural orientation, which this screen keeps.
            fit.aspect = sideways ? previewSize.getHeight() / (float) previewSize.getWidth()
                                      : previewSize.getWidth() / (float) previewSize.getHeight();
            status.setText("Point the camera at the code on the computer's screen.");
        } else {
            preview.setVisibility(View.GONE);
            status.setText("This device has no camera to scan with. Paste the link instead: "
                    + "`remora link pair` prints it, and the Phone page has a Copy button.");
        }
    }

    /** The back camera (else any), its frame size for reading and for the preview, its focus. */
    private boolean pickCamera() {
        try {
            String fallback = null;
            for (String id : cameras.getCameraIdList()) {
                CameraCharacteristics cc = cameras.getCameraCharacteristics(id);
                if (cc.get(CameraCharacteristics.SCALER_STREAM_CONFIGURATION_MAP) == null) continue;
                Integer facing = cc.get(CameraCharacteristics.LENS_FACING);
                if (facing != null && facing == CameraCharacteristics.LENS_FACING_BACK) {
                    cameraId = id;
                    break;
                }
                if (fallback == null) fallback = id;
            }
            if (cameraId == null) cameraId = fallback;
            if (cameraId == null) return false;
            CameraCharacteristics cc = cameras.getCameraCharacteristics(cameraId);
            StreamConfigurationMap map = cc.get(CameraCharacteristics.SCALER_STREAM_CONFIGURATION_MAP);
            Size[] yuv = map.getOutputSizes(ImageFormat.YUV_420_888);
            Size[] shown = map.getOutputSizes(SurfaceHolder.class);
            if (yuv == null || shown == null) return false;
            frameSize = largest(yuv, null);
            // The preview at the frames' shape, so what the screen shows is what is read.
            previewSize = largest(shown, frameSize);
            if (previewSize == null) previewSize = largest(shown, null);
            if (frameSize == null || previewSize == null) return false;
            Integer orientation = cc.get(CameraCharacteristics.SENSOR_ORIENTATION);
            sideways = orientation != null && orientation % 180 == 90;
            int[] af = cc.get(CameraCharacteristics.CONTROL_AF_AVAILABLE_MODES);
            if (af != null) {
                Arrays.sort(af);
                if (Arrays.binarySearch(af, CaptureRequest.CONTROL_AF_MODE_CONTINUOUS_PICTURE) >= 0)
                    afMode = CaptureRequest.CONTROL_AF_MODE_CONTINUOUS_PICTURE;
                else if (Arrays.binarySearch(af, CaptureRequest.CONTROL_AF_MODE_CONTINUOUS_VIDEO) >= 0)
                    afMode = CaptureRequest.CONTROL_AF_MODE_CONTINUOUS_VIDEO;
            }
            return true;
        } catch (CameraAccessException | RuntimeException e) {
            return false;
        }
    }

    /** The largest size of at most MAX_PIXELS, of {@code shape}'s aspect when one is given. */
    private static Size largest(Size[] sizes, Size shape) {
        Size best = null;
        for (Size s : sizes) {
            long px = (long) s.getWidth() * s.getHeight();
            if (px > MAX_PIXELS) continue;
            if (shape != null && (long) s.getWidth() * shape.getHeight() != (long) s.getHeight() * shape.getWidth())
                continue;
            if (best == null || px > (long) best.getWidth() * best.getHeight()) best = s;
        }
        return best;
    }

    @Override
    protected void onResume() {
        super.onResume();
        resumed = true;
        if (cameraId == null || done) return;
        if (checkSelfPermission(Manifest.permission.CAMERA) == PackageManager.PERMISSION_GRANTED) {
            open();
        } else if (!asked) {
            asked = true;
            requestPermissions(new String[] {Manifest.permission.CAMERA}, ASK_CAMERA);
        } else {
            refused();
        }
    }

    @Override
    protected void onPause() {
        resumed = false;
        close();
        super.onPause();
    }

    @Override
    public void onRequestPermissionsResult(int request, String[] permissions, int[] results) {
        if (request != ASK_CAMERA) return;
        if (results.length > 0 && results[0] == PackageManager.PERMISSION_GRANTED) open();
        else refused();
    }

    private void refused() {
        status.setText("Without the camera, paste the link instead. The camera can be allowed "
                + "for Remora in the phone's settings, under Apps.");
    }

    @Override
    public void surfaceCreated(SurfaceHolder holder) {}

    @Override
    public void surfaceChanged(SurfaceHolder holder, int format, int width, int height) {
        surfaceReady = true;
        open();
    }

    @Override
    public void surfaceDestroyed(SurfaceHolder holder) {
        surfaceReady = false;
        close();
    }

    private void open() {
        if (!resumed || !surfaceReady || done || opening || camera != null || cameraId == null) return;
        if (checkSelfPermission(Manifest.permission.CAMERA) != PackageManager.PERMISSION_GRANTED) return;
        final int mine = ++gen;
        frames = new HandlerThread("scan");
        frames.start();
        reader = ImageReader.newInstance(frameSize.getWidth(), frameSize.getHeight(), ImageFormat.YUV_420_888, 2);
        reader.setOnImageAvailableListener(this::onFrame, new Handler(frames.getLooper()));
        opening = true;
        try {
            cameras.openCamera(cameraId, new CameraDevice.StateCallback() {
                @Override
                public void onOpened(CameraDevice c) {
                    if (mine != gen) {
                        c.close();
                        return;
                    }
                    opening = false;
                    camera = c;
                    startSession(mine);
                }

                @Override
                public void onDisconnected(CameraDevice c) {
                    c.close();
                    if (mine != gen) return;
                    opening = false;
                    camera = null;
                    status.setText("Another app took the camera. Come back to this screen to scan again.");
                }

                @Override
                public void onError(CameraDevice c, int error) {
                    c.close();
                    if (mine != gen) return;
                    opening = false;
                    camera = null;
                    status.setText("The camera stopped (error " + error + "). Paste the link instead, "
                            + "or come back to this screen.");
                }
            }, main);
        } catch (CameraAccessException | RuntimeException e) {
            opening = false;
            close();
            status.setText("The camera would not open: " + e.getMessage());
        }
    }

    private void startSession(int mine) {
        Surface shown = preview.getHolder().getSurface();
        Surface read = reader.getSurface();
        try {
            camera.createCaptureSession(new SessionConfiguration(SessionConfiguration.SESSION_REGULAR,
                    Arrays.asList(new OutputConfiguration(shown), new OutputConfiguration(read)),
                    getMainExecutor(), new CameraCaptureSession.StateCallback() {
                        @Override
                        public void onConfigured(CameraCaptureSession s) {
                            if (mine != gen || camera == null) {
                                s.close();
                                return;
                            }
                            session = s;
                            try {
                                CaptureRequest.Builder b = camera.createCaptureRequest(CameraDevice.TEMPLATE_PREVIEW);
                                b.addTarget(shown);
                                b.addTarget(read);
                                b.set(CaptureRequest.CONTROL_MODE, CaptureRequest.CONTROL_MODE_AUTO);
                                b.set(CaptureRequest.CONTROL_AF_MODE, afMode);
                                s.setRepeatingRequest(b.build(), null, main);
                            } catch (CameraAccessException | RuntimeException e) {
                                status.setText("The camera would not start: " + e.getMessage());
                            }
                        }

                        @Override
                        public void onConfigureFailed(CameraCaptureSession s) {
                            if (mine == gen) status.setText("The camera would not start. Paste the link instead.");
                        }
                    }));
        } catch (CameraAccessException | RuntimeException e) {
            status.setText("The camera would not start: " + e.getMessage());
        }
    }

    private void close() {
        gen++;
        opening = false;
        if (session != null) {
            try {
                session.close();
            } catch (RuntimeException ignored) {
                // Already closed with its camera.
            }
            session = null;
        }
        if (camera != null) {
            camera.close();
            camera = null;
        }
        // The reader closes on its own thread, after any frame that thread is still reading.
        ImageReader r = reader;
        HandlerThread t = frames;
        reader = null;
        frames = null;
        if (t != null) {
            new Handler(t.getLooper()).post(() -> {
                if (r != null) r.close();
            });
            t.quitSafely();
        }
    }

    /** On the frames thread: the newest frame's luminance, read for a code. */
    private void onFrame(ImageReader r) {
        Image img;
        try {
            img = r.acquireLatestImage();
        } catch (RuntimeException e) {
            return;  // the reader is closing
        }
        if (img == null) return;
        int w, h, stride;
        try {
            if (done) return;
            Image.Plane y = img.getPlanes()[0];  // the Y plane: one byte a pixel, by contract
            ByteBuffer buf = y.getBuffer();
            w = img.getWidth();
            h = img.getHeight();
            stride = y.getRowStride();
            int n = buf.remaining();
            if ((long) (h - 1) * stride + w > n) return;
            if (lum == null || lum.length < n) lum = new byte[n];
            buf.get(lum, 0, n);
        } catch (RuntimeException e) {
            return;
        } finally {
            img.close();
        }
        String text = QrDecoder.decode(lum, w, h, stride);
        if (text != null) main.post(() -> found(text));
    }

    private void found(String text) {
        if (done) return;
        if (Link.PairingCode.parse(text) != null) pair(text);
        else status.setText("That code is not a Remora pairing code. Scan the one on the "
                + "computer's Phone page, or the one `remora link pair` prints.");
    }

    private void pair(String link) {
        done = true;
        close();
        startActivity(new Intent(Intent.ACTION_VIEW, Uri.parse(link), this, PairActivity.class));
        finish();
    }

    private int dp(int v) {
        return Math.round(v * getResources().getDisplayMetrics().density);
    }

    /** Its one child at the camera picture's shape, as large as fits, centred. */
    private static final class Fit extends FrameLayout {
        float aspect;  // width over height, as shown

        Fit(Context c) {
            super(c);
        }

        @Override
        protected void onMeasure(int wSpec, int hSpec) {
            int w = MeasureSpec.getSize(wSpec), h = MeasureSpec.getSize(hSpec);
            setMeasuredDimension(w, h);
            int cw = w, ch = h;
            if (aspect > 0 && w > 0 && h > 0) {
                if (w > h * aspect) cw = Math.round(h * aspect);
                else ch = Math.round(w / aspect);
            }
            for (int i = 0; i < getChildCount(); i++)
                getChildAt(i).measure(MeasureSpec.makeMeasureSpec(cw, MeasureSpec.EXACTLY),
                        MeasureSpec.makeMeasureSpec(ch, MeasureSpec.EXACTLY));
        }

        @Override
        protected void onLayout(boolean changed, int l, int t, int r, int b) {
            for (int i = 0; i < getChildCount(); i++) {
                View c = getChildAt(i);
                int x = (r - l - c.getMeasuredWidth()) / 2, y = (b - t - c.getMeasuredHeight()) / 2;
                c.layout(x, y, x + c.getMeasuredWidth(), y + c.getMeasuredHeight());
            }
        }
    }
}
