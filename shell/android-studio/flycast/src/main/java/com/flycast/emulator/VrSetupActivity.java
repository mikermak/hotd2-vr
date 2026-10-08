// hotd2-vr: added in 2026 by mikermak for the Quest VR mode (see "git log master..hotd2-vr").
package com.flycast.emulator;

import android.app.Activity;
import android.app.ActivityManager;
import android.content.ActivityNotFoundException;
import android.content.ComponentName;
import android.content.Intent;
import android.graphics.Color;
import android.graphics.Typeface;
import android.graphics.drawable.GradientDrawable;
import android.net.Uri;
import android.os.Build;
import android.os.Bundle;
import android.os.Environment;
import android.os.Process;
import android.os.SystemClock;
import android.provider.Settings;
import android.text.TextUtils;
import android.util.TypedValue;
import android.view.Gravity;
import android.view.View;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;

import java.io.File;
import java.util.List;

/**
 * hotd2-vr: the setup panel. Shown in the headset's home when the game can't start yet: no
 * access to the files, or no game found, or more than one game to choose from. It says what's
 * missing, step by step, and starts the game in VR once it's all there.
 */
public class VrSetupActivity extends Activity
{
    /** The game picked here, for the VR activity (a path). */
    public static final String EXTRA_GAME = "hotd2vr.game";

    private static final int RED = Color.rgb(217, 32, 26), TEXT = Color.rgb(230, 230, 230),
            SOFT = Color.rgb(160, 160, 164), GOOD = Color.rgb(96, 200, 110), BACK = Color.rgb(18, 18, 20);

    private TextView accessStatus, gameStatus;
    private Button allow;
    private LinearLayout games;

    @Override
    protected void onCreate(Bundle savedInstanceState)
    {
        super.onCreate(savedInstanceState);
        LinearLayout page = new LinearLayout(this);
        page.setOrientation(LinearLayout.VERTICAL);
        page.setPadding(dp(36), dp(28), dp(36), dp(28));
        page.setBackgroundColor(BACK);

        TextView title = text("THE HOUSE OF THE DEAD 2 VR", 28, RED);
        title.setTypeface(Typeface.DEFAULT_BOLD);
        page.addView(title);
        page.addView(text("Two steps before the first game.", 17, SOFT), spaced(4));

        page.addView(heading("1.  Let the app read your files"), spaced(24));
        page.addView(accessStatus = text("", 16, SOFT), spaced(6));
        allow = button("Allow");
        allow.setOnClickListener(v -> askForAccess());
        page.addView(allow, spaced(10));

        page.addView(heading("2.  Put your game on the headset"), spaced(24));
        page.addView(gameStatus = text("", 16, SOFT), spaced(6));
        Button again = button("Look again");
        again.setOnClickListener(v -> refresh());
        page.addView(again, spaced(10));

        // a Play button per game found
        games = new LinearLayout(this);
        games.setOrientation(LinearLayout.VERTICAL);
        page.addView(games, spaced(20));

        page.addView(text("Your own copies of the games: The House of the Dead 2 (European or US version) and The "
                + "Maze of the Kings (arcade, as a MAME set: mok.zip with the folder mok next to it). Nothing of the "
                + "games comes with this app. Unofficial fan project, not affiliated with Sega.", 13, SOFT), spaced(22));

        ScrollView scroll = new ScrollView(this);
        scroll.setBackgroundColor(BACK);
        scroll.addView(page);
        setContentView(scroll);
    }

    @Override
    protected void onResume()
    {
        super.onResume();
        refresh();
    }

    private void refresh()
    {
        boolean access = VrGames.canReadSharedStorage();
        VrGames.Scan scan = VrGames.scan(this);
        boolean ready = !scan.games.isEmpty();
        if (access)
        {
            accessStatus.setText("Done.");
            accessStatus.setTextColor(GOOD);
            allow.setVisibility(View.GONE);
        }
        else
        {
            accessStatus.setText("The app looks for your game in the headset's Download folder. For that it needs "
                    + "\"All files access\": tap Allow, switch it on, and come back here.");
            accessStatus.setTextColor(SOFT);
            allow.setVisibility(View.VISIBLE);
        }
        if (ready)
        {
            StringBuilder found = new StringBuilder(scan.games.size() == 1 ? "Found: " : "Found, pick one below:");
            for (VrGames.Game g : scan.games)
                found.append(scan.games.size() == 1 ? "" : "\n").append(shortPath(g.file));
            gameStatus.setText(found);
            gameStatus.setTextColor(GOOD);
        }
        else
        {
            String how = "Copy your game to the headset's Download folder: a .chd, a .gdi or a .cue with its "
                    + ".bin files, or a .cdi. With SideQuest (Manage files), or connect the headset to a PC "
                    + "with USB, allow access in the headset, and open Quest > Internal shared storage > Download.";
            if (scan.incomplete != null)
                how = "Found " + shortPath(scan.incomplete) + ", but not the files it lists next to it: "
                        + TextUtils.join(", ", scan.missing) + ". Copy those to the same folder.";
            else if (scan.archive != null)
                how = "Found " + shortPath(scan.archive) + ": that's packed. Unpack it first (on a PC), then "
                        + "copy what comes out of it to the Download folder.";
            else if (!access)
                how += " Then allow the access above.";
            gameStatus.setText(how);
            gameStatus.setTextColor(scan.incomplete != null || scan.archive != null ? RED : SOFT);
        }
        games.removeAllViews();
        for (VrGames.Game g : scan.games)
        {
            Button play = button("Play " + g.title);
            play.setOnClickListener(v -> play(g.file));
            games.addView(play, spaced(8));
        }
    }

    private void askForAccess()
    {
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.R)
            return;
        try {
            startActivity(new Intent(Settings.ACTION_MANAGE_APP_ALL_FILES_ACCESS_PERMISSION,
                    Uri.parse("package:" + getPackageName())));
        } catch (ActivityNotFoundException e) {
            startActivity(new Intent(Settings.ACTION_MANAGE_ALL_FILES_ACCESS_PERMISSION));
        }
    }

    private void play(File game)
    {
        // A game may still be running in the app's own process (the app opened again from the
        // library while one played): the emulator has one game per process, and its task would
        // only be brought back. So that process ends, and the game picked starts in a new one.
        endGameProcess();
        // the app's launcher entry, which starts in VR
        Intent intent = new Intent(Intent.ACTION_MAIN);
        intent.setComponent(new ComponentName(this, "com.flycast.emulator.MainActivity"));
        intent.putExtra(EXTRA_GAME, game.getAbsolutePath());
        intent.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK | Intent.FLAG_ACTIVITY_CLEAR_TASK);
        startActivity(intent);
        finish();
    }

    /** Ends the app's main process (this panel runs in one of its own), and waits for it. */
    private void endGameProcess()
    {
        ActivityManager am = (ActivityManager) getSystemService(ACTIVITY_SERVICE);
        List<ActivityManager.RunningAppProcessInfo> running = am != null ? am.getRunningAppProcesses() : null;
        if (running == null)
            return;
        for (ActivityManager.RunningAppProcessInfo p : running)
        {
            if (p.pid == Process.myPid() || !getPackageName().equals(p.processName))
                continue;
            Process.killProcess(p.pid);
            for (int i = 0; i < 50 && new File("/proc/" + p.pid).exists(); i++)
                SystemClock.sleep(20);
        }
    }

    private String shortPath(File f)
    {
        String path = f.getAbsolutePath();
        String root = Environment.getExternalStorageDirectory().getAbsolutePath();
        return path.startsWith(root + "/") ? path.substring(root.length() + 1) : f.getName();
    }

    private TextView text(String s, int sp, int colour)
    {
        TextView t = new TextView(this);
        t.setText(s);
        t.setTextSize(TypedValue.COMPLEX_UNIT_SP, sp);
        t.setTextColor(colour);
        t.setLineSpacing(0, 1.15f);
        return t;
    }

    private TextView heading(String s)
    {
        TextView t = text(s, 20, TEXT);
        t.setTypeface(Typeface.DEFAULT_BOLD);
        return t;
    }

    private Button button(String s)
    {
        Button b = new Button(this);
        b.setText(s);
        b.setAllCaps(false);
        b.setTextSize(TypedValue.COMPLEX_UNIT_SP, 17);
        b.setTextColor(Color.WHITE);
        b.setGravity(Gravity.CENTER);
        b.setPadding(dp(28), dp(8), dp(28), dp(8));
        GradientDrawable shape = new GradientDrawable();
        shape.setColor(RED);
        shape.setCornerRadius(dp(8));
        b.setBackground(shape);
        return b;
    }

    private LinearLayout.LayoutParams spaced(int top)
    {
        LinearLayout.LayoutParams p = new LinearLayout.LayoutParams(LinearLayout.LayoutParams.WRAP_CONTENT,
                LinearLayout.LayoutParams.WRAP_CONTENT);
        p.topMargin = dp(top);
        return p;
    }

    private int dp(int v)
    {
        return Math.round(v * getResources().getDisplayMetrics().density);
    }
}
