package org.bozredux.client;

import android.content.Context;
import android.net.wifi.WifiManager;
import android.os.Bundle;
import android.system.ErrnoException;
import android.system.Os;
import android.util.Log;

import org.libsdl.app.SDLActivity;

/**
 * The game: SDL's activity running libmain.so (the desktop client built for Android) with the
 * arguments SetupActivity chose. The client's main() runs as SDL_main.
 */
public final class GameActivity extends SDLActivity {
    static final String EXTRA_ARGS = "org.bozredux.client.ARGS";
    static final String EXTRA_SAVES = "org.bozredux.client.SAVES";
    private static final String TAG = "BozRedux";

    private WifiManager.MulticastLock m_Multicast;

    @Override
    protected String[] getLibraries() {
        return new String[] {"SDL2", "SDL2_mixer", "main"};
    }

    @Override
    protected String[] getArguments() {
        String[] args = getIntent().getStringArrayExtra(EXTRA_ARGS);
        return args != null ? args : new String[0];
    }

    @Override
    protected void onCreate(Bundle state) {
        // The game keeps its saves under $HOME, as the desktop launcher sets it.
        String saves = getIntent().getStringExtra(EXTRA_SAVES);
        if (saves != null) {
            try {
                Os.setenv("HOME", saves, true);
            } catch (ErrnoException e) {
                Log.e(TAG, "cannot set HOME", e);
            }
        }
        super.onCreate(state);
        // Without the lock Android drops incoming multicast: the Local Wi-Fi lobby (mDNS) needs it.
        WifiManager wifi = (WifiManager) getApplicationContext().getSystemService(Context.WIFI_SERVICE);
        if (wifi != null) {
            m_Multicast = wifi.createMulticastLock("boz-coop-mdns");
            m_Multicast.setReferenceCounted(false);
            m_Multicast.acquire();
        }
    }

    @Override
    protected void onDestroy() {
        if (m_Multicast != null && m_Multicast.isHeld()) {
            m_Multicast.release();
        }
        boolean finishing = isFinishing();
        super.onDestroy();
        if (finishing) {
            // The next PLAY gets a fresh process: guest memory, JIT and SDL start from zero.
            android.os.Process.killProcess(android.os.Process.myPid());
        }
    }
}
