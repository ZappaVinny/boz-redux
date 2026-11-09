package org.bozredux.client;

import android.app.Activity;
import android.content.ActivityNotFoundException;
import android.content.ClipData;
import android.content.Intent;
import android.content.pm.PackageInfo;
import android.content.pm.PackageManager;
import android.content.res.AssetManager;
import android.database.Cursor;
import android.net.Uri;
import android.os.Bundle;
import android.provider.OpenableColumns;
import android.util.Log;

import org.libsdl.app.SDLActivity;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.net.HttpURLConnection;
import java.net.URL;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.util.ArrayList;
import java.util.zip.ZipEntry;
import java.util.zip.ZipInputStream;

/**
 * The launcher: the desktop launcher's screens (src/launcher, libboz_launcher.so) on SDL, in the
 * app's main process. Its native side calls the static methods here for what only Android can
 * do: the system file picker, reading picked files, downloads, mod zips and starting the game,
 * which runs in its own process (GameActivity). The game root is the app's external files
 * folder, laid out like the desktop install: game/assets, game/gamedef, game/mods, game/saves,
 * game/client.ini.
 */
public final class LauncherActivity extends SDLActivity {
    private static final String TAG = "BozRedux";
    private static final int PICK_REQUEST = 4101;

    private static File sRoot;
    // The picker's answer for the native thread waiting in pickDocuments.
    private static final Object sPickLock = new Object();
    private static String[] sPicked;
    private static boolean sPickDone;

    @Override
    protected String[] getLibraries() {
        return new String[] {"SDL2", "boz_launcher"};
    }

    @Override
    protected String[] getArguments() {
        return new String[] {"--root", sRoot.getPath()};
    }

    @Override
    protected void onCreate(Bundle state) {
        sRoot = new File(getExternalFilesDir(null), "game");
        for (String dir : new String[] {"assets", "mods", "saves"}) {
            new File(sRoot, dir).mkdirs();
        }
        installGamedef();
        super.onCreate(state);
        // Testing: adb shell am start -n org.bozredux.client/.LauncherActivity --ez autoplay true
        //          [--es BOZ_TRACE_INPUT 1 ...] starts the game at once with those variables.
        if (getIntent().getBooleanExtra("autoplay", false)
                && new File(new File(sRoot, "assets"), "boz.s3e.unpacked").isFile()) {
            File saves = new File(sRoot, "saves");
            startGame(new String[] {"--root", sRoot.getPath(),
                    "--log", new File(sRoot, "boz-log.txt").getPath(),
                    "--run", new File(new File(sRoot, "assets"), "boz.s3e.unpacked").getPath()},
                    new String[] {"HOME", saves.getPath()});
        }
    }

    /**
     * The game definition ships inside this APK (assets/gamedef): copy it into the game root
     * once per installed version, replacing the old copy.
     */
    private void installGamedef() {
        File target = new File(sRoot, "gamedef");
        File stamp = new File(target, ".installed-from-apk");
        String version;
        try {
            PackageInfo info = getPackageManager().getPackageInfo(getPackageName(), 0);
            version = info.versionName + " " + info.lastUpdateTime;
        } catch (PackageManager.NameNotFoundException e) {
            version = "unknown";
        }
        try {
            if (stamp.isFile() && new String(Files.readAllBytes(stamp.toPath()),
                    StandardCharsets.UTF_8).equals(version)) {
                return;
            }
            AssetManager assets = getAssets();
            String[] top = assets.list("gamedef");
            if (top == null || top.length == 0) {
                return; // a build without it: the game reports the missing gamedef
            }
            deleteTree(target);
            copyAssets(assets, "gamedef", target);
            Files.write(stamp.toPath(), version.getBytes(StandardCharsets.UTF_8));
        } catch (IOException e) {
            Log.e(TAG, "cannot install the gamedef", e);
        }
    }

    private static void copyAssets(AssetManager assets, String path, File target) throws IOException {
        String[] children = assets.list(path);
        if (children != null && children.length > 0) {
            target.mkdirs();
            for (String child : children) {
                copyAssets(assets, path + "/" + child, new File(target, child));
            }
            return;
        }
        try (InputStream in = assets.open(path); OutputStream out = new FileOutputStream(target)) {
            copy(in, out);
        }
    }

    private static void deleteTree(File file) {
        File[] children = file.listFiles();
        if (children != null) {
            for (File child : children) {
                deleteTree(child);
            }
        }
        file.delete();
    }

    private static void copy(InputStream in, OutputStream out) throws IOException {
        byte[] buffer = new byte[1 << 16];
        int n;
        while ((n = in.read(buffer)) > 0) {
            out.write(buffer, 0, n);
        }
    }

    private static Activity activity() {
        return (Activity) SDLActivity.getContext();
    }

    // --- called from the native launcher (src/launcher/os_android.cpp) ---------------------

    /** Whether the system document picker exists (many TVs have none). */
    public static boolean hasDocumentPicker() {
        Intent intent = new Intent(Intent.ACTION_OPEN_DOCUMENT);
        intent.addCategory(Intent.CATEGORY_OPENABLE);
        intent.setType("*/*");
        return intent.resolveActivity(activity().getPackageManager()) != null;
    }

    /** Shows the system picker and waits for it: the picked content:// URIs, or none. */
    public static String[] pickDocuments(boolean multiple) {
        synchronized (sPickLock) {
            sPicked = new String[0];
            sPickDone = false;
        }
        final Activity activity = activity();
        activity.runOnUiThread(() -> {
            Intent intent = new Intent(Intent.ACTION_OPEN_DOCUMENT);
            intent.addCategory(Intent.CATEGORY_OPENABLE);
            intent.setType("*/*");
            intent.putExtra(Intent.EXTRA_ALLOW_MULTIPLE, multiple);
            try {
                activity.startActivityForResult(intent, PICK_REQUEST);
            } catch (ActivityNotFoundException e) {
                finishPick(new String[0]);
            }
        });
        synchronized (sPickLock) {
            while (!sPickDone) {
                try {
                    sPickLock.wait();
                } catch (InterruptedException e) {
                    return new String[0];
                }
            }
            return sPicked;
        }
    }

    private static void finishPick(String[] picked) {
        synchronized (sPickLock) {
            sPicked = picked;
            sPickDone = true;
            sPickLock.notifyAll();
        }
    }

    @Override
    protected void onActivityResult(int request, int result, Intent data) {
        super.onActivityResult(request, result, data);
        if (request != PICK_REQUEST) {
            return;
        }
        ArrayList<String> picked = new ArrayList<>();
        if (result == RESULT_OK && data != null) {
            ClipData clip = data.getClipData();
            if (clip != null) {
                for (int i = 0; i < clip.getItemCount(); ++i) {
                    picked.add(clip.getItemAt(i).getUri().toString());
                }
            } else if (data.getData() != null) {
                picked.add(data.getData().toString());
            }
        }
        finishPick(picked.toArray(new String[0]));
    }

    /** A picked file's size in bytes, or -1. */
    public static long documentSize(String uri) {
        try (Cursor cursor = activity().getContentResolver().query(Uri.parse(uri),
                new String[] {OpenableColumns.SIZE}, null, null, null)) {
            if (cursor != null && cursor.moveToFirst() && !cursor.isNull(0)) {
                return cursor.getLong(0);
            }
        } catch (RuntimeException e) {
            Log.e(TAG, "size of " + uri, e);
        }
        return -1;
    }

    public static boolean copyDocument(String uri, String to) {
        try (InputStream in = activity().getContentResolver().openInputStream(Uri.parse(uri));
             OutputStream out = new FileOutputStream(to)) {
            if (in == null) {
                return false;
            }
            copy(in, out);
            return true;
        } catch (IOException | RuntimeException e) {
            Log.e(TAG, "copy of " + uri, e);
            return false;
        }
    }

    /**
     * Unpacks a mod's .zip into modsDir: a zip of the mod's folder, or of its contents (then the
     * folder is named after the zip). Answers "ok:folder" or "error:reason".
     */
    public static String importModZip(String uri, String modsDir) {
        // Pass 1: where mod.toml sits (zip root or one folder down).
        String prefix = null;
        try (ZipInputStream zip = new ZipInputStream(
                activity().getContentResolver().openInputStream(Uri.parse(uri)))) {
            ZipEntry entry;
            while ((entry = zip.getNextEntry()) != null) {
                String name = entry.getName().replace('\\', '/');
                if (name.equals("mod.toml")) {
                    prefix = "";
                    break;
                }
                int slash = name.indexOf('/');
                if (slash > 0 && name.substring(slash + 1).equals("mod.toml")) {
                    prefix = name.substring(0, slash + 1);
                }
            }
        } catch (IOException | RuntimeException e) {
            return "error:That file is not a .zip.";
        }
        if (prefix == null) {
            return "error:That .zip has no mod.toml: it is not a BOZ Redux mod.";
        }
        String folder = prefix.isEmpty() ? zipName(uri) : prefix.substring(0, prefix.length() - 1);
        if (folder.isEmpty() || folder.startsWith(".") || folder.contains("/")) {
            return "error:That mod's folder name is not usable.";
        }
        File target = new File(modsDir, folder);
        File staging = new File(modsDir, "." + folder + ".part");
        deleteTree(staging);
        // Pass 2: unpack into a staging folder, then replace the old copy.
        try (ZipInputStream zip = new ZipInputStream(
                activity().getContentResolver().openInputStream(Uri.parse(uri)))) {
            String root = staging.getCanonicalPath() + File.separator;
            ZipEntry entry;
            while ((entry = zip.getNextEntry()) != null) {
                String name = entry.getName().replace('\\', '/');
                if (!name.startsWith(prefix)) {
                    continue;
                }
                File out = new File(staging, name.substring(prefix.length()));
                if (!out.getCanonicalPath().startsWith(root) && !out.getCanonicalPath().equals(staging.getCanonicalPath())) {
                    deleteTree(staging);
                    return "error:That .zip has unsafe file names.";
                }
                if (entry.isDirectory()) {
                    out.mkdirs();
                    continue;
                }
                out.getParentFile().mkdirs();
                try (OutputStream file = new FileOutputStream(out)) {
                    copy(zip, file);
                }
            }
        } catch (IOException | RuntimeException e) {
            deleteTree(staging);
            return "error:Could not unpack the mod: " + e.getMessage();
        }
        deleteTree(target);
        if (!staging.renameTo(target)) {
            deleteTree(staging);
            return "error:Could not install the mod.";
        }
        return "ok:" + folder;
    }

    private static String zipName(String uri) {
        String name = "mod";
        try (Cursor cursor = activity().getContentResolver().query(Uri.parse(uri),
                new String[] {OpenableColumns.DISPLAY_NAME}, null, null, null)) {
            if (cursor != null && cursor.moveToFirst() && !cursor.isNull(0)) {
                name = cursor.getString(0);
            }
        } catch (RuntimeException e) {
            // keep "mod"
        }
        if (name.toLowerCase().endsWith(".zip")) {
            name = name.substring(0, name.length() - 4);
        }
        return name.replaceAll("[^A-Za-z0-9._-]", "-");
    }

    /** Downloads url into the file to (HTTP 200 only). */
    public static boolean download(String url, String to) {
        HttpURLConnection connection = null;
        try {
            connection = (HttpURLConnection) new URL(url).openConnection();
            connection.setConnectTimeout(20000);
            connection.setReadTimeout(60000);
            connection.setInstanceFollowRedirects(true);
            if (connection.getResponseCode() != HttpURLConnection.HTTP_OK) {
                Log.e(TAG, url + ": HTTP " + connection.getResponseCode());
                return false;
            }
            try (InputStream in = connection.getInputStream(); OutputStream out = new FileOutputStream(to)) {
                copy(in, out);
            }
            return true;
        } catch (IOException | RuntimeException e) {
            Log.e(TAG, "download of " + url, e);
            return false;
        } finally {
            if (connection != null) {
                connection.disconnect();
            }
        }
    }

    /**
     * Starts the game with these arguments. env holds name, value pairs: HOME becomes the
     * game's saves folder, BOZ_* variables are set in the game's process. BOZ_* extras this
     * activity was started with (adb testing) are passed on too.
     */
    public static boolean startGame(String[] args, String[] env) {
        Activity activity = activity();
        Intent intent = new Intent(activity, GameActivity.class);
        intent.putExtra(GameActivity.EXTRA_ARGS, args);
        Bundle extras = activity.getIntent().getExtras();
        if (extras != null) {
            for (String key : extras.keySet()) {
                Object value = extras.get(key);
                if (key.startsWith("BOZ_") && value instanceof String) {
                    intent.putExtra(key, (String) value);
                }
            }
        }
        for (int i = 0; i + 1 < env.length; i += 2) {
            if (env[i].equals("HOME")) {
                intent.putExtra(GameActivity.EXTRA_SAVES, env[i + 1]);
            } else if (env[i].startsWith("BOZ_")) {
                intent.putExtra(env[i], env[i + 1]);
            }
        }
        activity.runOnUiThread(() -> activity.startActivity(intent));
        return true;
    }
}
