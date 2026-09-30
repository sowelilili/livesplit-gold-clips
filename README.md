# LiveSplit Gold Clips

An OBS Studio plugin for Windows. Whenever you get a gold split in LiveSplit
(a segment faster than your best), it saves a video clip of that segment, with
a couple of seconds extra on each side, from OBS's replay buffer.

Clips go to:

    <OBS recording folder>/Golds/<Game>/<Category>/<Split> <segment time>.mkv

for example `Golds/Ratchet & Clank 2/Any%/Oozla 1m23.45s.mkv`. The file type
follows your recording format. If a name is taken, ` (2)`, ` (3)`, ... is added.

## Requirements

- OBS Studio 32 on Windows (64-bit).
- The replay buffer enabled: Settings > Output > Replay Buffer. Its
  **Maximum Replay Time** must cover your longest segment plus about 10 seconds
  (lead, tail and saving time). The buffer is kept in memory, roughly
  bitrate x time: at 10 Mbps, 5 minutes is about 375 MB. The plugin starts the
  replay buffer by itself when a run starts.
- LiveSplit's TCP server running on port 16834: right-click LiveSplit >
  Control > Start TCP Server. Keep the "Best Segments" comparison enabled in
  LiveSplit (it is by default).
- ffmpeg on PATH (for example `winget install Gyan.FFmpeg`), or its full path
  in the settings.

## Install

1. Build: in Git Bash, run `tools/fetch-obs-headers.sh` once, then `./build.sh`.
2. Close OBS.
3. In PowerShell, run `./install.ps1`. It copies the plugin to
   `C:\ProgramData\obs-studio\plugins\livesplit-gold-clips\bin\64bit\`.

## Settings

Tools > LiveSplit Gold Clips settings opens the settings file
(`%APPDATA%\obs-studio\plugin_config\livesplit-gold-clips\config.ini`).
Changes apply when the next run starts. Write `key=value` with no spaces
around `=`, and use forward slashes in paths.

| Key | Default | Meaning |
| --- | --- | --- |
| `host` | `127.0.0.1` | LiveSplit's TCP server address |
| `port` | `16834` | LiveSplit's TCP server port |
| `poll_ms` | `25` | How often to check LiveSplit, in milliseconds |
| `lead_seconds` | `2.0` | Video kept before the segment starts |
| `tail_seconds` | `2.0` | Video kept after the split |
| `save_margin_seconds` | `1.0` | Extra wait before saving the replay buffer |
| `folder` | `Golds` | Folder in the recording folder for the clips |
| `ffmpeg` | `ffmpeg` | The ffmpeg program |
| `start_replay_buffer` | `true` | Start the replay buffer when a run starts |
| `keep_full_replay` | `false` | Keep the whole replay file after the clips are cut |
| `clip_unknown_best` | `true` | Count splits without a best segment yet (such as on a first run) as golds |

The plugin logs what it does to the OBS log (Help > Log Files), with lines
starting with `[gold-clips]`.

## Limits

- Clips start on the last keyframe before the segment, so they can begin up
  to one keyframe interval early. Set the keyframe interval to 1-2 seconds
  (Advanced output mode, recording encoder); "0" or auto can mean several
  seconds.
- A segment longer than the replay buffer is clipped starting late, from the
  oldest video the buffer still has. The same happens to the first segment
  of a run if the plugin had to start the replay buffer at that run's start.
- The segment right after a skipped split is never clipped, because it has no
  segment time.
- Not checked: two splits within one check (25 ms) and the split after them,
  splits made while LiveSplit was not reachable, and the segment in progress
  when OBS connects to LiveSplit.
- Each save briefly writes the whole replay buffer to disk; the plugin deletes
  that file after cutting the clips. If OBS's "Automatically remux to mp4" is
  on and you record to MKV, OBS also makes an .mp4 copy of each save, which the
  plugin leaves alone.

## Tests

`tests/run.sh` builds everything and runs the tests (needs Python 3, ffmpeg and
ffprobe). They use a fake LiveSplit server and a fake OBS, never the real ones.
