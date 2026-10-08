// hotd2-vr: added in 2026 by mikermak for the Quest VR mode (see "git log master..hotd2-vr").
package com.flycast.emulator;

import android.content.Context;
import android.os.Build;
import android.os.Environment;

import java.io.BufferedReader;
import java.io.File;
import java.io.FileReader;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;
import java.util.Locale;

/**
 * hotd2-vr: finding the player's own games on the headset. First the app's own folders
 * (files/games, internal and on the shared storage), then, with "All files access", the
 * Download folder and the rest of the shared storage. A .cue or .gdi only counts when the
 * tracks it lists are next to it; a packed game (.zip, .7z) is noticed to tell the player to
 * unpack it. Arcade (NAOMI) games are MAME sets: the game's zip, with its GD-ROM image in a
 * folder of the same name next to it.
 */
public final class VrGames
{
    private VrGames() {}

    /** A game that can start. */
    public static final class Game
    {
        public final String title;
        public final File file;

        Game(String title, File file)
        {
            this.title = title;
            this.file = file;
        }
    }

    public static final class Scan
    {
        /** The games found, the best first (The House of the Dead by its name). */
        public final List<Game> games = new ArrayList<>();
        /** The game to start when there's one, or null. */
        public File game;
        /** A disc image whose tracks aren't all there (when there's no good one), and what's missing. */
        public File incomplete;
        public final List<String> missing = new ArrayList<>();
        /** A packed game, when there's no disc image. */
        public File archive;
    }

    /** The arcade sets the VR mod knows: MAME name, title, and the GD-ROM image next to it. */
    private static final String[][] ARCADE = {
        { "mok", "The Maze of the Kings", "gds-0022" },
    };

    private static final String[] IMAGES = { ".cue", ".gdi", ".chd", ".cdi" };
    private static final String[] ARCHIVES = { ".zip", ".7z", ".rar" };
    private static final List<String> SKIP = Arrays.asList("android", "dcim", "oculus", "movies", "pictures",
            "music", "podcasts", "ringtones", "alarms", "notifications", "audiobooks", "recordings");

    /** The app may look in the headset's shared storage (Download and the rest). */
    public static boolean canReadSharedStorage()
    {
        return Build.VERSION.SDK_INT < Build.VERSION_CODES.R || Environment.isExternalStorageManager();
    }

    public static Scan scan(Context context)
    {
        Scan scan = new Scan();
        List<File> found = new ArrayList<>();
        list(new File(context.getFilesDir(), "games"), 1, found);
        File external = context.getExternalFilesDir(null);
        if (external != null)
            list(new File(external, "games"), 1, found);
        if (canReadSharedStorage())
        {
            File root = Environment.getExternalStorageDirectory();
            list(new File(root, Environment.DIRECTORY_DOWNLOADS), 3, found);
            List<File> elsewhere = new ArrayList<>();
            list(root, 2, elsewhere);
            for (File f : elsewhere)
                if (!found.contains(f))
                    found.add(f);
        }
        collect(found, scan);
        if (!scan.games.isEmpty())
            scan.game = scan.games.get(0).file;
        return scan;
    }

    private static boolean endsWith(String name, String[] extensions)
    {
        String lower = name.toLowerCase(Locale.ROOT);
        for (String ext : extensions)
            if (lower.endsWith(ext))
                return true;
        return false;
    }

    private static void list(File dir, int depth, List<File> out)
    {
        File[] files = dir.listFiles();
        if (files == null)
            return;
        Arrays.sort(files);
        for (File f : files)
        {
            String name = f.getName();
            if (name.startsWith("."))
                continue;
            if (f.isDirectory())
            {
                if (depth > 0 && !SKIP.contains(name.toLowerCase(Locale.ROOT)))
                    list(f, depth - 1, out);
            }
            else if (f.length() > 0 && (endsWith(name, IMAGES) || endsWith(name, ARCHIVES)))
                out.add(f);
        }
    }

    /** The House of the Dead by its name, else whatever there is. */
    private static int score(File f)
    {
        String name = f.getName().toLowerCase(Locale.ROOT);
        return name.contains("house of the dead") || name.contains("hotd") ? 1 : 0;
    }

    /** An arcade set's title, when it is one the mod knows and its GD-ROM is next to it. */
    private static String arcadeTitle(File zip)
    {
        String name = zip.getName().toLowerCase(Locale.ROOT);
        for (String[] set : ARCADE)
        {
            if (!name.equals(set[0] + ".zip") && !name.equals(set[0] + ".7z"))
                continue;
            File dir = new File(zip.getParentFile(), set[0]);
            for (String ext : new String[] { ".chd", ".cue", ".gdi" })
                if (new File(dir, set[2] + ext).isFile())
                    return set[1];
        }
        return null;
    }

    /** A disc image's title: its file name without the extension and the tags in brackets. */
    private static String discTitle(File f)
    {
        String name = f.getName();
        int dot = name.lastIndexOf('.');
        if (dot > 0)
            name = name.substring(0, dot);
        name = name.replaceAll("\\s*[\\(\\[][^\\)\\]]*[\\)\\]]", "").trim();
        // "House of the Dead 2, The" -> "The House of the Dead 2"
        if (name.endsWith(", The"))
            name = "The " + name.substring(0, name.length() - 5);
        return name.isEmpty() ? f.getName() : name;
    }

    private static void collect(List<File> found, Scan scan)
    {
        List<File> discs = new ArrayList<>();
        for (File f : found)
        {
            if (endsWith(f.getName(), ARCHIVES))
            {
                String title = arcadeTitle(f);
                if (title != null)
                    scan.games.add(new Game(title, f));
                else if (scan.archive == null || score(f) > score(scan.archive))
                    scan.archive = f;
                continue;
            }
            // the GD-ROM image of an arcade set isn't a game of its own
            File parent = f.getParentFile();
            if (parent != null && isArcadeFolder(parent))
                continue;
            List<String> missing = missingTracks(f);
            if (!missing.isEmpty())
            {
                if (scan.incomplete == null || score(f) > score(scan.incomplete))
                {
                    scan.incomplete = f;
                    scan.missing.clear();
                    scan.missing.addAll(missing);
                }
                continue;
            }
            discs.add(f);
        }
        // The House of the Dead first, then the rest
        discs.sort((a, b) -> score(b) - score(a));
        List<Game> games = new ArrayList<>();
        for (File f : discs)
            games.add(new Game(discTitle(f), f));
        games.addAll(scan.games);
        scan.games.clear();
        scan.games.addAll(games);
        if (!scan.games.isEmpty())
            scan.archive = null;
    }

    private static boolean isArcadeFolder(File dir)
    {
        for (String[] set : ARCADE)
            if (dir.getName().equalsIgnoreCase(set[0]))
                return true;
        return false;
    }

    /** The files a .cue or .gdi lists that aren't next to it (letter case aside). */
    static List<String> missingTracks(File image)
    {
        List<String> missing = new ArrayList<>();
        String lower = image.getName().toLowerCase(Locale.ROOT);
        boolean cue = lower.endsWith(".cue");
        if (!cue && !lower.endsWith(".gdi"))
            return missing;
        File dir = image.getParentFile();
        String[] names = dir != null ? dir.list() : null;
        try (BufferedReader in = new BufferedReader(new FileReader(image)))
        {
            String line;
            boolean first = true;
            while ((line = in.readLine()) != null)
            {
                line = line.trim();
                String track = null;
                if (cue)
                {
                    if (line.toUpperCase(Locale.ROOT).startsWith("FILE "))
                        track = fileName(line.substring(5));
                }
                else if (!first && !line.isEmpty())
                {
                    // number, lba, type, sector size, file name (quoted when it has spaces), offset
                    String[] parts = line.split("\\s+", 5);
                    if (parts.length == 5)
                        track = fileName(parts[4]);
                }
                first = false;
                if (track == null || track.isEmpty())
                    continue;
                boolean there = new File(dir, track).isFile();
                if (!there && names != null)
                    for (String n : names)
                        if (n.equalsIgnoreCase(track))
                            there = true;
                if (!there)
                    missing.add(track);
            }
        }
        catch (Exception e) {
            // unreadable: let the emulator have a go
        }
        return missing;
    }

    /** A file name at the start of the rest of a line, quoted or not. */
    private static String fileName(String rest)
    {
        rest = rest.trim();
        if (rest.startsWith("\""))
        {
            int end = rest.indexOf('"', 1);
            return end > 0 ? rest.substring(1, end) : rest.substring(1);
        }
        int space = rest.indexOf(' ');
        return space > 0 ? rest.substring(0, space) : rest;
    }
}
