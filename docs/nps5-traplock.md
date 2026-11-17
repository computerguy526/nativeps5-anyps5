# nps5-traplock: host imports keep read traps (PPSA30552 load hang, deadlock #2)

Branch `nps5-traplock` (on `nps5-loadhang` 00c3b9bb03 + the two `nps5-readrace` commits). macOS only.

## The hang

Label read traps (`2cf5b41ec8`, Apple only): at a label, a pending storage image is not stored into guest memory; its
pages are made `PROT_NONE` and the first CPU access runs the resolver (`ResolveLabelTrap` -> `FlushGpuWrites` ->
`FlushForAccess` -> `StorageTexture::FlushPending` -> `writeBack` -> `writeBackLayers` -> `HostImportFor`), which stores
the image through its host import.

A host import (`importRunAt`, called with `HostImports::mutex` held) wraps the Vulkan import in
`GuestMemory::ImportWatched`, which on macOS opens a `GuestArena::HostWrite` over the import's range so the pages are
accessible and count as written. `GuestWriteWatchBeginHostWrite` first *resolves every read trap over the range*
(`withoutTraps` -> `Watch::resolve`), on the importing thread:

1. Deadlock #1 (fixed by 00c3b9bb03): the resolver collects writes, `ImportWatched` held the tracker mutex.
2. Deadlock #2 (this branch): the resolver's write-back calls `HostImportFor`, which locks `HostImports::mutex`, held by
   the same thread since `importAllocation`. Sample: draw-commit in `Mutex::lockContended` under `HostImportFor` under
   `importRunAt`.
3. Deadlock #3 (found by reading, same fix): `withoutTraps` also calls `waitForResolving`, a `sched_yield` loop that
   waits for resolvers running on *other* threads over the range. Such a resolver (a guest thread faulted on a trapped
   page) waits for `HostImports::mutex` in `HostImportFor`, which the importer holds: the importer spins forever.

The upstream intent is visible in `99af947b18`: "The driver's own import window on macOS skips it" (`OwnHostWrite` makes
`PrepareHostWrite` skip the flush). The import's own host write was never meant to flush GPU results; the read-trap
resolve inside `BeginHostWrite` is a flush that got in through the back door when traps were added later.

## Options

| | Design | Verdict |
|---|---|---|
| a | Resolve the traps over the range before taking the imports mutex (at `AcquireRegistered` / `ImportMappedRanges` / `HostImportFor`), optimistic range + retry | Not complete on its own: another thread can arm a trap (idle publish, label) between the pre-resolve and the import, so the import under the lock still needs a path that does not resolve. Needs the range computed outside the lock (it is the output of `importAllocation`: joins, step-downs, budget), a retry protocol at every entry point that can reach an import, and does nothing for #3. Most code, least safe. |
| b | The import's host write keeps the traps: open the trapped pages for the window, close them again after | **Chosen.** One new libc entry point, one call site, no lock-order change, covers #2 and #3, and restores the upstream rule that the import's own window does not flush. |
| c | Drop the imports mutex around `ImportWatched` in `importRunAt` | The function holds iterators into `state.imports`, an `ImportConstruction` in progress, budget/eviction decisions already made (`retireLeastRecent`), and its callers hold pointers into the map. Releasing the mutex in the middle invalidates all of that; each would need re-validation. Highest risk. |
| d | Make the resolver re-enter the imports mutex on the same thread | `HostImportFor` can create and retire imports while `importRunAt` is mid-change. Rejected. |

## Design (b)

libc `GuestWriteWatch` (Apple `Watch`):

- `GuestWriteWatchBeginHostWriteKeepingTraps(pointer, bytes)`: under the watch lock only. No `waitForResolving`, no
  `resolve`, no callout. Registers the range in `_hostWrites` (as `BeginHostWrite` does: no new trap can be armed over an
  open host write, collections keep the pages `Written`) and in `_trapKeepingWrites`. An untrapped page is handled as in
  `BeginHostWrite` (opened if write-protected, state `Written`). A **read-trapped page is only opened** (`mprotect` to its
  logical protection): its `ReadTrapped` bit, its write state (`Protected` / `Written`) and the `_traps` entries are
  untouched, so it does **not** read as written after the import (see "The stamp hazard").
- `untrap()`: a page whose trap is resolved or released while a trap-keeping write holds it open joins the write (open,
  `Written`), instead of being write-protected in the middle of the import.
- `GuestWriteWatchEndHostWrite`: for a trap-keeping write, pages that still carry `ReadTrapped` are closed again
  (`PROT_NONE`, one `mprotect` per run). A trap that was resolved or released during the window cleared `ReadTrapped`
  (`untrap`) and stays open.
- `protect()` (called by a collection) leaves a read-trapped page alone while a trap-keeping write holds it open;
  otherwise a collection on another thread would close the page in the middle of the import.

Driver `GuestMemory::ImportWatched`: the import window uses `ImportHostWrite` (the new entry point +
`GuestWriteWatchEndHostWrite`) instead of `GuestArena::HostWrite`. The host write hook is not called (it was already
skipped through `OwnHostWrite`). `APS5_IMPORT_RESOLVES_READ_TRAPS=1` restores the old behaviour (resolve at open, with
the tracker suspension of 00c3b9bb03) for A/B and bisecting; it brings deadlock #2 back.

### Why this is correct

- The import needs accessible pages, not their bytes. It does not read guest memory on the CPU; what the GPU later reads
  through the import is covered by the same per-use flushes (`FlushPending` over the ranges a draw reads) that cover a
  pending image without a trap, and that covered every pending image through an import before traps existed.
- The pending image stays pending behind its trap, exactly as it stays pending through an import on a platform with no
  traps. Its `_traps` entry, `readTrapped` flag and pending registration are unchanged, so every existing path that
  stores it (first CPU access, store of another image over it, eviction, explicit write-back) still does.
- Invariant kept: outside an open trap-keeping write, `ReadTrapped` in the table <=> the page is `PROT_NONE`.
- The tracker already expected read-trapped pages inside an import (`ad41d4a709`: they are left out of the page hashes
  and count as unknown), and `Collect` already reports a trapped page as written.
- Lock order: the new path takes only the watch lock (a leaf), under the tracker mutex, as `walkWrites` -> `Collect`
  already does. No GPU mutex, no imports mutex, no wait.
- Resolvers on other threads: a resolver blocked on the imports mutex continues after the import; its own stores open
  its pages (`own` in `resolveTrap`) whether `EndHostWrite` closed them or not, and its final `untrap` opens them for
  good.

### The stamp hazard (found after the first title run, fixed in the second commit)

The first version marked the trapped pages `Written` like the rest of the window. After the import, `ImportWatched`
walks the window (`walkWrites(..., &window)` -> `stampWindowRuns`): a page whose bytes hash as before the import is
stamped `ImportWindow` (possibly touched, not stored), but a page that was read-trapped when the hashes were taken is
"unknown" (`HashPages`, `ad41d4a709`) and gets a `StampKind::Cpu` **stored** stamp. With the trap resolved at the open
(the old behaviour) that stamp was followed by guest memory holding the image's results. With the trap kept, the bytes
are stale and the stamp tells the storage image that the CPU stored over its pending results ("guest memory changed
while GPU results were pending; keeping the CPU's blocks", `AccessKeptByCpu`): the pending image could lose to stale
guest memory. tl_t2 (first version, traps on) presented through the fade but the user saw gameplay dark (stale tonemap
LUT, see dark3); this is a candidate cause, not a proven one. Trapped pages are therefore not marked: nothing is
collected for them, nothing is stamped, the image stays pending exactly as before the import. Unit tests assert
`UnchangedSince` over the kept-trap range right after the import.

### Cost: a window without faults

While the import is being made (the Vulkan import calls, typically well under a millisecond per import) a CPU access by
another thread to a trapped page of the range does not fault: a read sees the bytes from before the pending image, a
store lands before the image's write-back instead of after it and is not recorded by the write watch. This is the same exposure the GPU-mutex bypass already
has (a `GpuMutex` holder opens a trapped page without storing the image, and every thread reads it unstored until the
next label; see `docs/dev/debt/libSceAgcDriver.md`, "Label read traps"), but bounded by the import instead of by the
next label. The alternative on offer was a hang.

With `APS5_NO_LABEL_READ_TRAP=1` (no trap is ever armed) the new path does what `BeginHostWrite` did: same `mprotect`s,
same page states, same `_hostWrites` entry. `APS5_TRACE_FLUSH=1` logs `[flush] import of ... keeps ... bytes of read traps`.

## Tests

- `write_watch_tests read-trap-import` (libc level): a resolver that takes a lock the "importer" holds. The trap-keeping
  write opens trapped and protected pages, calls no resolver, refuses new traps, survives a collection, reports only its
  untrapped pages as written, closes exactly the still-trapped pages at its end; the traps then resolve on the next read. Second half: a resolver in flight on another
  thread, blocked on the importer's lock, is not waited for (#3).
- `agc_write_tracking_tests --import-over-read-traps`: new `CheckImportUnderImportsLockOverReadTraps` (a resolver that
  takes a plain mutex standing in for `HostImports::mutex`; `ImportWatched(..., importMayWriteGuest = true)` under that
  mutex over armed traps, and again with a resolver in flight). Dies on its alarm (rc 142) without the fix.
  `CheckImportOpensWriteOverReadTraps` (deadlock #1) now expects the traps kept, the range `UnchangedSince` the import,
  and the traps resolved by the next read. `--import-under-imports-lock` runs the new check alone.

## Open

- libkernel DirectMemory `sceKernelQuery*` still reads the host mapping and the watch table as two steps (readrace note).
## Follow-up: nps5-fixmem (review-int5-mem)

- The window is ended by a token: `GuestWriteWatchBeginHostWriteKeepingTraps` returns it and
  `GuestWriteWatchEndHostWriteKeepingTraps(token)` ends it. `_trapKeepingWrites` is gone (one `_hostWrites` list, the
  entry says whether it keeps traps). `GuestWriteWatchEndHostWrite(pointer, bytes)` ends plain windows only, so the end
  of a plain host write over the same pages can no longer close the trapped pages under a running import.
- A CPU store to a trapped page inside the window can be recorded (opt-in). The page cannot be opened without
  write to make the store fault: Metal builds a no-copy buffer over a range that has one read-only page without an
  error, and GPU writes through that buffer then never reach the range (probe on macOS 26 / M4 Pro: all pages RW at
  creation = GPU -> CPU and CPU -> GPU both work; any page read-only at creation = GPU -> CPU lost for the whole
  buffer, CPU -> GPU still works; a `PROT_NONE` page = creation fails; protection changes after creation do not
  matter). So with `APS5_IMPORT_WINDOW_STORES=1` (default off) the watch hashes each trapped page that holds no write
  when it opens it and again at the end of the window, both times while the page is readable only, and marks a page
  whose bytes changed `Written`. The trap stays armed; the driver's import window walk then stamps the block as a CPU
  store (trapped pages are `unknown` to its own hashes), which is what a store that faulted leads to. Pages nothing
  stored to stay unmarked (the stamp hazard above). Off by default because a GPU write into such a page through a host
  import inside the window changes its bytes too and would read as a CPU store over the pending image; not measured on
  a title. Without the flag the window is as described under "Cost: a window without faults".
- `resolveTrap`: a fault on a trapped page that a trap-keeping write holds open, taken by the resolver's own thread or
  by a GPU-mutex holder, retries the access on the open page. It used to open the page with the watch's protection,
  which is read-only for an armed page, in the middle of the import.
- `GuestWriteWatchProtectKeepingTraps` + `GuestWriteWatchResolveReadTraps` for libkernel `DoMprotect`: the guest's
  mprotect resolves the read traps over its range before it locks the allocation registry and protects inside the
  mutation without calling the resolver. `GuestWriteWatchProtect` inside the mutation ran the resolver (host imports
  mutex) with the registry locked, against `ImportMappedRanges`, which holds the host imports mutex and asks the
  registry for its ranges. The nps5 runtime is not affected: it calls `GuestWriteWatchProtect` outside the registry.
- `GuestWriteWatch::ReplaceMapping`: libkernel `Unmap`, the fixed anonymous remap and `AddMapping` unregister the range
  before the host mapping is replaced. With `APS5_WATCH_RUNS=1` the driver keeps accessibility answers from the watch table
  per thread until the layout generation of their block moves; unregistering after the reset left the table (and the kept
  answers) calling pages accessible that the host had already taken away, and gave armed pages of the reset range their old
  protection back.
- `record()` moves the layout generation when it takes the host protection into the table; `Register` over an existing
  entry gives pages the host does not map protection 0.
- Tests: write_watch `unmap-order`, `host-write-store`, `host-write-token`, `protect-keeping-traps`, `layout-resync`;
  `agc_write_tracking_tests --import-over-read-traps` (`CheckImportWindowKeepsItsPages`) and `--import-window-stores`
  (`CheckImportStoreOverReadTraps`; both store tests start themselves again with the flag set).

- Any other CPU read of a trapped page made with `HostImports::mutex` held and without `GpuMutex` would hit the same
  resolver-under-imports pattern through the fault path. None was seen in the samples; `HashPages` skips trapped pages.
