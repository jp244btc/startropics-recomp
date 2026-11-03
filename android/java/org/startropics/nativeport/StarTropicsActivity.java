package org.startropics.nativeport;

import android.content.Intent;
import android.database.Cursor;
import android.net.Uri;
import android.provider.OpenableColumns;

import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.net.HttpURLConnection;
import java.net.URL;
import java.nio.charset.StandardCharsets;

import org.libsdl.app.SDLActivity;

/**
 * The game runs in native code (libmain.so) on SDL. This activity adds the two things that
 * need Java: the system document picker for choosing your cartridge dump, and HTTP for
 * RetroAchievements.
 */
public class StarTropicsActivity extends SDLActivity {
    private static final int PICK_ROM = 0x5754;

    /** Called from native code: show the system document picker. */
    public void pickRom() {
        runOnUiThread(new Runnable() {
            @Override
            public void run() {
                Intent i = new Intent(Intent.ACTION_OPEN_DOCUMENT);
                i.addCategory(Intent.CATEGORY_OPENABLE);
                i.setType("*/*");
                try {
                    startActivityForResult(i, PICK_ROM);
                } catch (Exception e) {
                    // no document picker on this device: the ROM can be put in the app's folder instead
                }
            }
        });
    }

    @Override
    protected void onActivityResult(int requestCode, int resultCode, Intent data) {
        super.onActivityResult(requestCode, resultCode, data);
        if (requestCode != PICK_ROM || resultCode != RESULT_OK || data == null || data.getData() == null) return;
        final Uri uri = data.getData();
        new Thread(new Runnable() {
            @Override
            public void run() {
                String path = copyToAppStorage(uri);
                // the native side treats it like a file dropped on the window: check its hash and load it
                if (path != null) SDLActivity.onNativeDropFile(path);
            }
        }, "rom-copy").start();
    }

    /** Copies the chosen document into the app's own storage, keeping its name (saves are named after it). */
    private String copyToAppStorage(Uri uri) {
        String name = "StarTropics.nes";
        Cursor c = null;
        try {
            c = getContentResolver().query(uri, new String[] { OpenableColumns.DISPLAY_NAME }, null, null, null);
            if (c != null && c.moveToFirst()) {
                String n = c.getString(0);
                if (n != null && n.length() > 0) name = n.replace('/', '_').replace('\\', '_');
            }
        } catch (Exception e) {
            // keep the default name
        } finally {
            if (c != null) c.close();
        }
        File dir = new File(getFilesDir(), "roms");
        dir.mkdirs();
        File out = new File(dir, name);
        InputStream in = null;
        OutputStream o = null;
        try {
            in = getContentResolver().openInputStream(uri);
            if (in == null) return null;
            o = new FileOutputStream(out);
            byte[] buf = new byte[65536];
            long total = 0;
            int n;
            while ((n = in.read(buf)) > 0) {
                total += n;
                if (total > (16 << 20)) throw new IOException("larger than any NES cartridge");
                o.write(buf, 0, n);
            }
            return out.getAbsolutePath();
        } catch (Exception e) {
            out.delete();
            return null;
        } finally {
            try { if (in != null) in.close(); } catch (IOException e) { /* ignore */ }
            try { if (o != null) o.close(); } catch (IOException e) { /* ignore */ }
        }
    }

    /**
     * A RetroAchievements request from native code (on a worker thread). Returns the HTTP status
     * as 4 big-endian bytes followed by the body; status 0 means the body is an error message.
     */
    public static byte[] httpRequest(String url, byte[] post, String contentType, String userAgent, int timeoutMs) {
        HttpURLConnection c = null;
        try {
            c = (HttpURLConnection) new URL(url).openConnection();
            c.setConnectTimeout(timeoutMs);
            c.setReadTimeout(timeoutMs);
            c.setRequestProperty("User-Agent", userAgent);
            if (post != null) {
                c.setRequestMethod("POST");
                c.setDoOutput(true);
                c.setRequestProperty("Content-Type", contentType);
                c.setFixedLengthStreamingMode(post.length);
                OutputStream o = c.getOutputStream();
                o.write(post);
                o.close();
            }
            int status = c.getResponseCode();
            ByteArrayOutputStream out = new ByteArrayOutputStream();
            writeInt(out, status);
            InputStream in = status >= 400 ? c.getErrorStream() : c.getInputStream();
            if (in != null) {
                byte[] buf = new byte[8192];
                int n;
                while ((n = in.read(buf)) > 0) out.write(buf, 0, n);
                in.close();
            }
            return out.toByteArray();
        } catch (Exception e) {
            ByteArrayOutputStream err = new ByteArrayOutputStream();
            writeInt(err, 0);
            byte[] msg = (e.getMessage() != null ? e.getMessage() : e.toString()).getBytes(StandardCharsets.UTF_8);
            err.write(msg, 0, msg.length);
            return err.toByteArray();
        } finally {
            if (c != null) c.disconnect();
        }
    }

    private static void writeInt(ByteArrayOutputStream o, int v) {
        o.write(v >>> 24);
        o.write(v >>> 16);
        o.write(v >>> 8);
        o.write(v);
    }
}
