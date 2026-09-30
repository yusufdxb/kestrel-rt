# Publication audit

Audit date: 2026-09-30. Scope: every commit on every branch and tag (`git log --all -p`), the working tree,
ignored files present on the authoring machine, and all documentation. The Phase 1 history
(`phase1-go-2026-09-30` -> `1318e51`, 13 commits) and the 13 Phase 2 commits were scanned separately.

## Method
- Pattern sweep of the full history patch text: API keys, tokens, passwords/PSKs, private keys
  (`BEGIN ... PRIVATE KEY`), GitHub tokens (`ghp_`, `github_pat`), AWS keys (`AKIA`), OpenAI-style keys,
  Wi-Fi network names, `KX_AP_PASS=` values, absolute home paths, MAC addresses, machine and host names,
  interface names, private-network IPs, USB serial numbers, and names of the author's other (private)
  projects, robots and thesis work.
- `git log --all --format=... -- esp/secrets.env esp/KestrelBridge/kx_secrets.h build` (never committed).
- Author identities: `git log --format='%an <%ae> | %cn <%ce>'` (one identity, the author's own).
- File types and sizes of every tracked file (`file --mime-encoding`, `du -b`) and of every blob in history
  (`git rev-list --objects --all | git cat-file --batch-check`).
- License inventory: THIRD_PARTY_NOTICES.md, docs/LICENSING.md.

## Findings

| # | Finding | Where | Severity | Resolution |
|---|---|---|---|---|
| 1 | Modified copy of Arduino's **unlicensed** bridge sketch (`UNOR4USBBridge.ino`, no license file or header upstream) | `esp/KestrelBridge/KestrelBridge.ino`, added in one Phase 2 commit, unchanged afterwards | **blocking** (redistribution rights not granted) | replaced by `esp/patches/KestrelBridge.ino.ed` (Kestrel lines only) applied to the user's own upstream checkout; the file is **removed from the public history** with `git filter-repo`. Only Phase 2 commits change ID; Phase 1 commits and the Phase 1 tag keep their IDs |
| 2 | ESP32-S3 Wi-Fi MAC address of the evaluation board | `backups/MANIFEST.md`, `backups/esp32_*.txt`, `docs/PHASE2_RECOVERY.md` (Phase 2 commits) | low (device identifier) | last three octets redacted in the tree and in the rewritten Phase 2 history |
| 3 | Name of the author's home Wi-Fi network (default argument; **no password**) | `scripts/esp_wifi.sh` (Phase 2 commits) | low | default removed (`KX_HOME_WIFI` now required); name replaced in the rewritten Phase 2 history |
| 4 | Names and local paths of the author's other private projects in provenance comments | `scripts/uno_swd_bootloader_entry.py`, `scripts/flash.sh` (Phase 1 history) | low (names only, no content) | reworded in the tree. **Left in Phase 1 history**, which is not rewritten |
| 5 | Local checkout paths as script defaults (`$HOME/Projects/_ref/...`) | `esp/build.sh`, `scripts/zephyr_image.sh` | low | now required environment variables (`KX_BRIDGE_REF`, `ZEPHYR_WS`) |
| 6 | Absolute path `/home/<author>/Projects/kestrel-rt/...` inside captured tool output | 41 result files (`results/*.json`, `results/phase2/*.json`, one log) | informational (reveals the author's login name, already public as author) | **kept**: result files are evidence and stay byte-identical |
| 7 | SoftAP password | only in the gitignored `esp/secrets.env` (mode 600), generated randomly by `esp/build.sh` | none in Git | never committed (checked across history) |

Not found anywhere in history: API keys, tokens, credentials, SSH or GitHub keys, Wi-Fi passwords,
NetworkManager secrets, machine IDs, host names, robot data, thesis material, ROS bags, crash dumps
containing environment data, executables, build trees, or firmware images.

## Large-file policy

| Kept out of Git (gitignored, local only) | Size | What is committed instead |
|---|---:|---|
| ESP32-S3 full-flash factory backup `backups/esp32_factory_*.bin` | 8 MB | sha256 + chip/partition identity in `backups/MANIFEST.md` |
| Raw capture streams `results/raw/` (job records, traces) | 187 MB | per-file sha256 in `results/MANIFEST.sha256`; per-run JSON summaries |
| Build outputs, fetched kernels, ESP images (`build/`) | - | exact build commands; image hashes in docs/PHASE2_RESULTS.md and CHANGELOG.md |
| ESP SoftAP secret `esp/secrets.env` | - | generator in `esp/build.sh` |

Largest committed files: `third_party/renesas/Include/R7FA4M1AB.h` (1.33 MB, vendored device header),
`results/phase2/soak_p2_udp.jsonl` (0.93 MB, the G9 soak heartbeat/metrics log, kept as compact evidence),
seven PNG figures (62 to 134 KB). Largest blob in history: the same device header. Repository size is
reported in the release handoff and in the final section below.

## Licensing
MIT for original code; vendored CMSIS headers Apache-2.0 with the license text; nothing else
redistributed. Open question documented in docs/LICENSING.md: the upstream Arduino bridge has no license,
so the fork ships as an ed script, not source. Not legal advice.

## Release checks (2026-09-30)
- Host tests (ASan/UBSan/TSan, frame fuzz, protocol fuzz): ALL HOST TESTS PASSED.
- `scripts/lint.sh`: cppcheck clean; clang-format clean **after** reformatting the Phase 2 sources. The gate
  had been reporting "0 deviations" while failing, because it counted only `warning` lines and clang-format
  reports `error`; the counter is fixed. The reformat is whitespace-only: `make APP=phase2` still produces
  the validated image `14c04cb3...58cb` byte for byte.
- Builds: `APP=benchmark`, `APP=phase2`, `APP=inversion`, FreeRTOS and superloop baselines, ESP32-S3 fork.
  The Zephyr baseline was not rebuilt (needs a west workspace).
- ESP32-S3: sketch regenerated byte for byte from upstream + ed script; a fresh compile matches the validated
  image (read back from the board) in every code and data byte (`scripts/esp_image_compare.py`).
- Markdown relative links: none broken. Every script referenced in README.md exists.
- `scripts/demo.sh` and `scripts/demo.sh --esp` ran on the board.

## Repository size
Rewritten public history after `git gc --aggressive`: 814 objects, **1.43 MiB** packed (`git count-objects -vH`);
the checked-out tree is about 5.4 MB, of which 1.33 MB is the vendored device header.

## History rewrite record
`git filter-repo --path esp/KestrelBridge/KestrelBridge.ino --invert-paths --replace-text` (MAC address
and home Wi-Fi name, finding 2 and 3). The 13 Phase 1 commits and `phase1-go-2026-09-30` -> `1318e51` are
unchanged. Every Phase 2 commit received a new ID; the private pre-rewrite history is kept by the author.
Fresh clone of the rewritten history: `make APP=phase2` gives `14c04cb3...58cb`, host tests pass, lint clean.
