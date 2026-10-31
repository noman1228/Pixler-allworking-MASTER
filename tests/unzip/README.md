# Upload and deferred extraction regressions

Run the extraction suite with:

```sh
python tests/unzip/run.py
```

The runner compiles the production `src/UnzipFiles.cpp` and its unmodified
header with the installed unzipLIB and ArduinoJson dependencies. Hardware
boundaries are replaced by an in-memory SD card, a controllable network state,
and fake clock/OLED functions. Python creates real ZIP fixtures in
`.pio/unzip-host-tests`; the production decompressor reads and validates them.

Coverage includes:

- Waiting for a network connection and for active uploads to close.
- One bounded 4096-byte write per poll, observable progress, and pause/resume.
- Stored and deflated XLZ, case-insensitive suffixes, matching inner/outer names,
  and mapping controller or nested entry names to the archive's `.fseq` name.
- Multiple ZIP entries, skipped directories, and complete XLZ validation before
  replacing an existing sequence.
- Corrupt deflate streams, CRC failures, invalid archives, insufficient space,
  short SD writes, and failed replacement while preserving originals and sources.
- Unsafe paths, embedded NUL names, and reserved temporary/recovery names.
- New uploads after completion, repeated archive names, mixed successful/failed
  queues, active-file protection, and ZIP callback seek/read boundaries.

Requires Python 3.9+, a C++17 compiler, and existing PlatformIO library
dependencies. On Windows, the runner can initialize an installed Visual Studio
Build Tools environment automatically. It writes build products only under
`.pio/unzip-host-tests`.

The transport/buffer suite is separate:

```sh
python tests/unzip/run_upload.py
```

The SD directory regression uses the installed, unmodified SdFat library:

```sh
python tests/unzip/sd_directory.py
```

It formats sparse RAM block devices as FAT16 and FAT32, then damages an unrelated
long-filename checksum before a valid uploaded archive. Direct pathname lookup
still finds the full archive while SdFat's ordinary iterator returns an empty
list. The production directory helper must recover subsequent valid files,
stop at the first end marker, and stop with a diagnostic on actual I/O errors.
The suite compares every sector before and after recovery to prove the scan
does not modify directory records or file contents. Build products stay under
`.pio/sd-directory-host-tests`.

Playback pause integration:

```sh
python tests/unzip/input_pause.py
```

The GUI behavior suite uses Node and jsdom installed under
`.pio/ui-verification/node_modules` (install with
`npm install --prefix .pio/ui-verification jsdom` if needed):

```sh
node tests/unzip/ui.cjs
```

These host tests do not replace flashing a controller and performing an xLights
upload over WiFi/Ethernet with its physical SD card.
