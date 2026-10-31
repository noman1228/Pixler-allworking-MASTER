# xLights uploads and deferred extraction

This is a selective integration into Pixler, preserving its board definitions,
custom outputs, test modes, and web interface. Upstream ESPixelStick was reviewed
at [`52d8eaf`](https://github.com/forkineye/ESPixelStick/commit/52d8eaf0eab64c413bf6f51b772e77c03ae0c28a),
including the `.xlz` filename fix. The working reference was
`H:\ESPixelStick\UNZIP_DEFERRED_CHANGES.md` and its implementation.

## Behavior

- Existing SD archives are queued at startup. Successful xLights HTTP, browser,
  and FTP archive uploads request another scan after the file has closed.
- Extraction starts only when `NetworkMgr.IsConnected()` reports WiFi or
  Ethernet connectivity and no upload is active. A lost connection or another
  upload pauses extraction until it can continue.
- `UnzipFiles::Poll()` performs at most one 4096-byte decompression read per
  main-loop pass. Network, output, FPP, and OLED polling continue between steps.
- `.xlz` must contain exactly one `.fseq` file. Its output is named after the
  uploaded archive: `Show.xlz` becomes `Show.fseq`, including when its internal
  controller filename differs or already matches. Safe nested internal paths
  are supported for `.xlz` because the output name comes from the archive.
- `.zip` supports multiple files in its root. Directory entries are skipped;
  nested output paths are rejected with a visible error. Suffix matching is
  case insensitive. Unsafe and reserved filenames are rejected.
- Output is staged in `.unzip.tmp` and checked for decompression errors, CRC,
  size, and successful SD writes before replacing its destination. `.xlz` is
  fully validated before its sequence is replaced. Multi-file `.zip` commits
  each verified entry individually; earlier successful entries remain if a
  later entry fails.
- Replacement uses `<destination>.unzip.bak` to preserve the previous file if a
  rename fails. Recovery data is retained if restoration also fails. Extraction
  requires enough free SD space to stage the new file alongside the old copy.
- Successfully processed archives are removed. Failed archives remain on the
  SD card, and the remaining queue continues. Re-uploading an archive or
  rebooting requests another attempt. Failed files are not retried in a loop.
- Extraction finishes without a reboot. The browser's previous archive reboot
  confirmation is removed. OLED start, completion, and error notices remain.
- Local sequence playback is stopped and its files closed for the extraction
  batch. Normal input processing resumes afterward, respecting other pause
  reasons such as uploads and test mode. SD removal releases this pause.

## GUI and status

`GET /XJ` includes `status.system.unzip` with:

`state`, `jobId`, `isUnzipping`, `hasPending`, `fileName`, `entryName`,
`bytesWritten`, `totalBytes`, `filesExtracted`, `archivesCompleted`,
`archivesFailed`, and `error`.

States are `idle`, `waiting`, `extracting`, `complete`, and `error`. Progress
bytes refer to the current entry; counters refer to the current batch.
Completion/error status remains until another batch is queued or the controller
reboots. The banner is outside individual tabs, so it remains visible on File
Management and other pages. The existing status poller updates visible pages
once per second, without accumulating overlapping requests.

## Upload fixes

SD filenames and in-flight upload filenames use `String`, avoiding the previous
64-character truncation and unchecked copies. Upload buffers accept chunks larger
than their capacity and use direct SD writes if no buffer is available. Final
flush and stored payload size are verified; multipart overhead is not treated as
sequence data. Failed or disconnected uploads remove their partial file.

HTTP upload start and extraction share a reservation guard. Requests that would
overwrite an actively extracted archive or destination receive HTTP 409. Different
archives can be uploaded while extraction pauses. xLights archive acknowledgment
does not parse ZIP bytes as an FSEQ header, and extraction waits until the response
has been constructed. FPP discovery reports the active network interface's IP.

### SD directory scan recovery

An investigation of the Dig-Octa upload log confirmed that `ImperialMarch.xlz`
was still present after reboot: FTP retrieved all 178,559 bytes and ZIP CRC
validation passed, despite both HTTP and FTP directory lists being empty.
Its sequence entry is `192_168_1_236_ImperialMarch.fseq` (2,582,416 bytes).

Real SdFat 2.3.1 tests reproduce these symptoms when an unrelated earlier long
filename directory entry has a damaged checksum. Named opens still succeed,
while `openNext()` stops without reporting an I/O error. The precise damaged
entry on the physical card has not been inspected.

FileMgr scans now skip consumed malformed entries with a visible warning and
continue discovering valid files. They stop at normal end markers, lack of
progress, or actual SD read failures. The scan only reads directory records;
it does not rewrite or repair them. Startup discovery can therefore find and
extract an existing valid archive after a damaged entry. FPP archive metadata
requests also return 404 instead of interpreting ZIP bytes as an FSEQ header.

## Verification

Run the host suites:

```text
python tests/unzip/run.py
python tests/unzip/run_upload.py
python tests/unzip/sd_directory.py
python tests/unzip/input_pause.py
node tests/unzip/ui.cjs
```

They compile production extraction/upload methods with real unzipLIB and real
ZIP fixtures, exercising network/upload pauses, naming, progress, corrupt archives,
short writes, failed replacement, and HTTP upload completion ordering. See
`tests/unzip/README.md` for prerequisites and GUI checks.

Build the web assets and the selected board's firmware and filesystem together.
Both firmware and filesystem must be installed to expose the new status GUI.
Physical validation still requires an xLights upload to a controller with its SD
card: test plain `.fseq`, matching/different-name `.xlz`, multiple queued archives,
network loss during extraction, and an intentionally corrupt archive. Verify both
WiFi and Ethernet as applicable. No controller has been flashed by this change.

Validated locally: 1,337 extraction assertions, 149 upload/storage assertions,
92 playback/pause assertions, and the GUI behavior suite passed. Both
`esp32_jmt` and `esp32_jmt_eth` firmware builds passed. The 327,680-byte LittleFS
image was unpacked and its GUI files compared byte-for-byte with the generated
web assets. ESLint reports 16 existing findings, versus 17 in the original script;
this change adds no lint findings.

Combined firmware/GUI packages:

- `firmware/EFU/Pixler-esp32_jmt-deferred-extraction.efu`
- `firmware/EFU/Pixler-esp32_jmt_eth-deferred-extraction.efu`

Both EFU packages were checked for their signature, network-order version and
record headers, exact firmware/filesystem payloads, and absence of trailing data.
Their SHA-256 hashes are in
`firmware/EFU/deferred-extraction-manifest.json`. The pre-existing `MyEnv.txt`
contents were restored after the build scripts appended their diagnostics.

The Dig-Octa SD scan follow-up passed 150 real-SdFat directory assertions,
161 upload/storage assertions, 1,337 extraction assertions, 92 playback/pause
assertions, and the GUI suite. `pio run -e jmt_esp32_dig_octa` passed.
The firmware-only update is
`firmware/EFU/Pixler-jmt_esp32_dig_octa-sd-upload-fix.efu`; it leaves the existing
LittleFS configuration and GUI in place. Its header, record, exact firmware
payload, and SHA-256 were verified; metadata is in
`firmware/EFU/sd-upload-fix-manifest.json`. The generated LittleFS image was also
unpacked and its 38 files compared with the generated assets, but it is not
included in this update. The retrieved archive and extracted sequence are saved
under `firmware/recovered/`. This firmware has not been flashed; physical
extraction and the specific damaged directory offset still need validation on
the controller after installation.
