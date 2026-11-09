package org.bozredux.client;

import android.app.Activity;
import android.content.Intent;
import android.graphics.Color;
import android.graphics.Typeface;
import android.os.Bundle;
import android.view.Gravity;
import android.view.View;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;

import java.io.File;
import java.util.ArrayList;

/**
 * First screen (temporary, until the launcher runs on Android): shows whether the game files are
 * in place and starts the game. The game root is the app's external files folder, laid out like
 * the desktop install: game/assets (boz.s3e.unpacked and the .dz packs), game/mods,
 * game/client.ini, game/saves. No game file is part of this app.
 */
public final class SetupActivity extends Activity {
    private File m_Root;
    private TextView m_State;
    private Button m_Play;

    @Override
    protected void onCreate(Bundle state) {
        super.onCreate(state);
        m_Root = new File(getExternalFilesDir(null), "game");
        new File(m_Root, "assets").mkdirs();
        new File(m_Root, "mods").mkdirs();
        new File(m_Root, "saves").mkdirs();
        setContentView(buildLayout());
        refresh();
        // Testing: adb shell am start -n org.bozredux.client/.SetupActivity --ez autoplay true
        //          [--es BOZ_TRACE_INPUT 1 ...] starts the game at once with those variables.
        if (getIntent().getBooleanExtra("autoplay", false) && m_Play.isEnabled()) {
            play();
        }
    }

    private View buildLayout() {
        LinearLayout column = new LinearLayout(this);
        column.setOrientation(LinearLayout.VERTICAL);
        column.setPadding(dp(32), dp(24), dp(32), dp(24));
        column.setBackgroundColor(0xFF0F0F12);

        TextView title = text("BOZ REDUX", 28, 0xFFFFFFFF);
        title.setTypeface(Typeface.DEFAULT_BOLD);
        column.addView(title);
        column.addView(text("Classic Black Ops Zombies, on Android. Use your own copy of the game.", 15,
                0xFFB8B8C0));
        m_State = text("", 15, 0xFFFFFFFF);
        column.addView(space());
        column.addView(m_State);
        column.addView(space());
        m_Play = button("PLAY", v -> play());
        m_Play.setBackgroundColor(0xFFE01F26);
        column.addView(m_Play);
        column.addView(space());
        column.addView(text("Game folder: " + m_Root, 12, 0xFF808088));
        column.addView(text("Log of the last run: " + new File(m_Root, "boz-log.txt"), 12, 0xFF808088));

        ScrollView scroll = new ScrollView(this);
        scroll.setBackgroundColor(0xFF0F0F12);
        scroll.addView(column);
        return scroll;
    }

    private boolean has(String name) {
        return new File(new File(m_Root, "assets"), name).isFile();
    }

    private void refresh() {
        boolean image = has("boz.s3e.unpacked");
        boolean etc = has("blackops_etc.dz");
        boolean gles1 = has("blackops_gles1.dz");
        StringBuilder text = new StringBuilder();
        text.append(image ? "Game installed" : "Game not installed (assets/boz.s3e.unpacked)").append('\n');
        text.append(etc ? "blackops_etc.dz found" : "blackops_etc.dz missing").append('\n');
        text.append(gles1 ? "blackops_gles1.dz found" : "blackops_gles1.dz missing");
        if (!image || !etc || !gles1) {
            text.append("\n\nCopy the assets folder of a desktop BOZ Redux install into the game folder"
                    + " below (for example with adb push).");
        }
        m_State.setText(text);
        boolean ready = image && etc && gles1;
        m_Play.setEnabled(ready);
        m_Play.setAlpha(ready ? 1f : 0.4f);
        if (ready) m_Play.requestFocus(); // a TV remote or controller starts on PLAY
    }

    private void play() {
        ArrayList<String> args = new ArrayList<>();
        args.add("--root");
        args.add(m_Root.getPath());
        args.add("--log");
        args.add(new File(m_Root, "boz-log.txt").getPath());
        args.add("--run");
        args.add(new File(new File(m_Root, "assets"), "boz.s3e.unpacked").getPath());
        Intent intent = new Intent(this, GameActivity.class);
        intent.putExtra(GameActivity.EXTRA_ARGS, args.toArray(new String[0]));
        intent.putExtra(GameActivity.EXTRA_SAVES, new File(m_Root, "saves").getPath());
        Bundle extras = getIntent().getExtras();
        if (extras != null) {
            for (String key : extras.keySet()) {
                Object value = extras.get(key);
                if (key.startsWith("BOZ_") && value instanceof String) {
                    intent.putExtra(key, (String) value);
                }
            }
        }
        startActivity(intent);
    }

    @Override
    protected void onResume() {
        super.onResume();
        if (m_State != null) refresh();
    }

    private TextView text(String s, int sp, int color) {
        TextView t = new TextView(this);
        t.setText(s);
        t.setTextSize(sp);
        t.setTextColor(color);
        t.setPadding(0, dp(4), 0, dp(4));
        return t;
    }

    private Button button(String label, View.OnClickListener click) {
        Button b = new Button(this);
        b.setText(label);
        b.setAllCaps(false);
        b.setTextColor(Color.WHITE);
        b.setTextSize(20);
        b.setGravity(Gravity.CENTER);
        b.setFocusable(true);
        b.setOnClickListener(click);
        return b;
    }

    private View space() {
        View v = new View(this);
        v.setLayoutParams(new LinearLayout.LayoutParams(1, dp(16)));
        return v;
    }

    private int dp(int v) {
        return Math.round(v * getResources().getDisplayMetrics().density);
    }
}
