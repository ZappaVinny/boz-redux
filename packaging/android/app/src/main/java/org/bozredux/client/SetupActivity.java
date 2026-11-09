package org.bozredux.client;

import android.app.Activity;
import android.content.Intent;
import android.graphics.Color;
import android.graphics.Typeface;
import android.os.Bundle;
import android.text.InputFilter;
import android.text.InputType;
import android.view.Gravity;
import android.view.View;
import android.widget.Button;
import android.widget.EditText;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;

import java.io.File;
import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.util.ArrayList;
import java.util.List;

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
    private EditText m_Name;

    /** The game's limit for a player name (s3e_config.c valid_player_name). */
    private static final int NAME_MAX = 13;

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
        column.addView(text("Player name (shown to the others in co-op)", 15, 0xFFFFFFFF));
        m_Name = new EditText(this);
        m_Name.setSingleLine(true);
        m_Name.setInputType(InputType.TYPE_CLASS_TEXT | InputType.TYPE_TEXT_FLAG_NO_SUGGESTIONS);
        m_Name.setTextColor(0xFFFFFFFF);
        m_Name.setHintTextColor(0xFF808088);
        m_Name.setHint("Letters, digits, space - _ . (up to 13)");
        // the characters the game accepts, as the desktop launcher filters them
        m_Name.setFilters(new InputFilter[] {new InputFilter.LengthFilter(NAME_MAX),
                (source, start, end, dest, dstart, dend) -> {
                    StringBuilder kept = new StringBuilder();
                    for (int i = start; i < end; ++i) {
                        char c = source.charAt(i);
                        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')
                                || c == ' ' || c == '-' || c == '_' || c == '.') {
                            kept.append(c);
                        }
                    }
                    return kept.length() == end - start ? null : kept;
                }});
        m_Name.setText(readName());
        column.addView(m_Name);
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

    /** client.ini [player] name, as the desktop launcher reads it. */
    private String readName() {
        File ini = new File(m_Root, "client.ini");
        if (!ini.isFile()) return "";
        try {
            boolean player = false;
            for (String line : Files.readAllLines(ini.toPath(), StandardCharsets.UTF_8)) {
                String t = line.trim();
                if (t.startsWith("[")) {
                    player = t.equals("[player]");
                } else if (player && t.startsWith("name") && t.contains("=")) {
                    String key = t.substring(0, t.indexOf('=')).trim();
                    if (key.equals("name")) return t.substring(t.indexOf('=') + 1).trim();
                }
            }
        } catch (IOException e) {
            // no name yet
        }
        return "";
    }

    /** Writes client.ini [player] name in place, keeping the rest of the file (comments too). */
    private void writeName(String name) {
        File ini = new File(m_Root, "client.ini");
        List<String> lines = new ArrayList<>();
        try {
            if (ini.isFile()) lines = new ArrayList<>(Files.readAllLines(ini.toPath(), StandardCharsets.UTF_8));
        } catch (IOException e) {
            lines = new ArrayList<>();
        }
        // [player] starts at section; its entries run until the next section header (end).
        int section = -1, end = -1, existing = -1;
        for (int i = 0; i < lines.size(); ++i) {
            String t = lines.get(i).trim();
            if (t.startsWith("[")) {
                if (section >= 0 && end < 0) end = i;
                if (t.equals("[player]")) {
                    section = i;
                    end = -1;
                }
            } else if (section >= 0 && end < 0 && t.contains("=")
                    && t.substring(0, t.indexOf('=')).trim().equals("name")) {
                existing = i;
            }
        }
        String entry = "name = " + name;
        if (existing >= 0) {
            lines.set(existing, entry);
        } else if (section >= 0) {
            lines.add(end >= 0 ? end : lines.size(), entry);
        } else {
            lines.add("");
            lines.add("[player]");
            lines.add(entry);
        }
        try {
            Files.write(ini.toPath(), lines, StandardCharsets.UTF_8);
        } catch (IOException e) {
            m_State.setText("Cannot save the player name: " + e.getMessage());
        }
    }

    private void play() {
        writeName(m_Name.getText().toString().trim());
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
