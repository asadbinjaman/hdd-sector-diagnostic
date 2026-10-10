# HDD Sector Diagnostic 
>>>A hobby project



A conservative native Windows physical-drive scanner. The current implementation is a **read-only baseline**: it identifies disks, performs a sequential read scan, records reads taking more than 10 ms, adaptively retries sectors within failed multi-sector reads, retests slow LBAs, and writes append-only CSV/text logs with a resume checkpoint.

> **Important scope note:** destructive write verification, actual ATA/SCSI block reassignment, SMART attribute decoding, and openSeaChest integration are not implemented in this baseline. `--destructive` performs additional safety checks but deliberately does not issue writes. It is not represented as a complete six-pass destructive/remediation implementation. This is intentional: raw-drive writes and remapping must be implemented and tested against the exact drive/controller protocol before use.

## Build

Requirements: Windows 10/11, Visual Studio Code and MinGW-w64 14.0.0 [This build was compiled with GCC 16.1.0 and packaged on 2026-07-30.].

From a Developer PowerShell in this directory:

```powershell
cmake -S . -B build -A x64
cmake --build build --config Release
```

Executable: `hdddiag.exe`.

## Usage

Run PowerShell or Command Prompt **as Administrator**.

List accessible physical drives and reported identity:

```powershell
.\hdddiag.exe --list
```

Read-only scan (logs must be on a different physical disk):

```powershell
.\hdddiag.exe --scan 2 --logs E:\hdddiag-logs
```

The program prints model, serial, capacity, and logical sector size before scanning. Confirm the disk number carefully; never assume `PhysicalDrive2` is the intended disk.

The `--destructive` flag is intentionally not a working write-test feature in this baseline. It verifies target identity and fails closed if the system-disk mapping or log-disk mapping cannot be established, but then records that destructive testing is not implemented and does not write to the disk.

## Output files

- `pass1_slow.csv`: `timestamp,LBA,sector_count,latency_ms` (the timestamp is prepended by the common logger; records are append-only).
- `io_errors.csv`: read failures and isolated retry failures with Win32 error code and elapsed time.
- `pass2_retest.csv`: results of retesting slow LBAs.
- `weak_sectors.log`: slow/error sectors on retest.
- `bad_blocks.log`: reserved for verified future write-test results; this baseline does not populate real bad-block findings.
- `weak_blocks.log`: final retest of LBA records found in `bad_blocks.log`.
- `scan_state.txt`: periodic `next_lba=` checkpoints. Existing logs are preserved when resuming.

## Safety and interpretation

- The scanner requests administrator access and defaults to read-only I/O.
- The Windows system disk is refused for destructive mode; uncertainty is treated as unsafe.
- The log destination must be on a different physical disk, and volume-to-disk mapping must be verifiable.
- Slow reads are not proof of bad sectors. Latency is affected by queueing, caching, power management, USB bridges, controller retries, and concurrent workloads.
- A failed read is distinct from a slow successful read. The tool records both separately.
- This baseline uses 4 KiB-sized requests aligned to the reported logical sector size, with a single-sector retry when a larger request fails. It is not a substitute for vendor diagnostics.
- Logs can contain repeated observations after a resume; preserve them as history rather than treating duplicate entries as new unique sectors.
- Raw device identity strings may be unavailable or altered by USB/SATA bridges. Do not proceed if model/serial cannot reliably identify the target.

## Planned safe extension points

1. Add SMART health/attribute collection via documented ATA pass-through or a trusted vendor tool, with controller compatibility reported explicitly.
2. Integrate openSeaChest as a separately invoked, version-checked maintenance executable. Capture exact command, exit code, stdout/stderr, and before/after device health. Never translate a command being issued into a claim that remapping succeeded.
3. Implement destructive verification only after a separate explicit opt-in, full backup acknowledgement, exact device identity checks, system-disk exclusion, and a tested sector-aligned write/read/compare loop. A write pass should first preserve enough state to resume safely, and should log every write, read, and mismatch independently.
4. Physical remapping should only be reported as requested/supported/verified when the device protocol and a post-operation read or vendor-reported defect-list change provide evidence. Most consumer HDDs do not expose a generic safe OS-level “remap this LBA” API; do not issue guessed IOCTLs.

## Known limitations

- Windows overlapped offsets are used with a synchronous handle; supported on standard Windows disk handles, but driver-specific behavior can vary.
- The 10 ms threshold is a screening heuristic, not a universal drive-health criterion.
- The state checkpoint is intentionally simple. It allows resuming the main scan but does not provide a transactional SQLite database or guarantee recovery from every abrupt power loss.
- SMART and openSeaChest are not automatically run by this baseline.
