# job

The update system for the vex stack: it builds a release, publishes it to a
channel, and resolves what a running application should move to.

Hotcwap can swap a dylib at runtime. It cannot produce one, and it has no way
to know a new one exists. That is what this repository is for.

## Three kinds of artifact, one release

| kind | contents | consumed by |
|---|---|---|
| `core` | dylibs from R2–R4 | hotcwap, in-process |
| `app` | dylibs from R5–R6 | hotcwap, in-process |
| `full` | a complete application | the shell, on first install |

All three are cut from **one release and one version**. A fresh download and a
hot-swapped install must end at the same version, or the stack ships two
different products under one number.

`core` and `app` are hot-swappable: replace the dylibs, keep running. `full` is
not — the process is what would be replaced — so it is the install artifact,
and installing for the first time and updating a full application are the same
path.

## The loop

```
job build      b + b.assembly.json          → release tree, stamped once
job publish    hash + channel document      → a labelled release
runtime fetch  (vexspoke, R2 net)           → download, verify sha256, stage
hotcwap        MANIFEST_UPDATE / PROMOTE    → new → current, bump generation
hotcwap        Hot_poll                     → dlopen the new sections, swap
```

Each arrow belongs to a different tier on purpose. The **download belongs to
vexsppoke's net layer** — a fetch here would be a second transport beside it.
`job` builds, publishes, and resolves; it never moves bytes.

## The resolver is C

`#include "job/updates.h"` — a pure function of (channel document, request):

```c
UpdateStore store = UpdateStore(nodes, 256, scratch, 2048);
UpdateStore_parse(&store, document);

UpdateRequest request = {0};
request.kind = UPDATE_KIND_CORE;
request.platform = "darwin-arm64";
request.current = installedVersion;      // nullptr on a first install

UpdatePlan plan = UpdateStore_resolve(&store, &request);
// plan.available, plan.version, plan.url, plan.sha256, plan.requiresRestart
```

No sockets, no files, no allocation: parsing uses `net/json.h` with
caller-owned pools, and every returned string is a length-delimited view into
the document the caller supplied. A resolver that also downloaded would be a
second transport, which is the tier rule this ecosystem keeps.

`requiresRestart` is true only for `UPDATE_KIND_FULL`.

## A section

One directory holding a `b.assembly.json` whose build declares:

```json
"link": "module"
```

`b` emits `-dynamiclib -fPIC -undefined dynamic_lookup` and writes
`<territory>/.b/out/<name>.dylib`. Undefined symbols resolve against the host
at load time — that is what makes a section swappable and keeps it from
carrying a copy of its neighbors.

## Channels

A channel is a mutable label over immutable releases. Publishing `0.4.2`
never changes it; moving `stable` to it is one edit.

```
cutting-edge   every build, may break
beta           promoted after it survives
stable         the default
```

`publish.py` writes the document the C resolver reads:

```json
{
  "channel": "stable",
  "release": "0.1.0",
  "core": { "darwin-arm64": { "version": "0.1.0", "url": "...", "sha256": "...", "size": 10830 } },
  "app": {},
  "applications": {
    "semicolon": { "darwin-arm64": { "tar.gz": { "version": "...", "url": "...", "sha256": "..." } } }
  }
}
```

## Commands

```sh
python3 build.py --check                   # which repositories still lack a unit
python3 build.py --release 0.2.0           # build the release tree + archives
python3 publish.py --release 0.2.0 --channel stable          # write the channel (dry)
python3 publish.py --release 0.2.0 --channel stable --execute # upload + move the pointer
```

`dist/` is generated and ignored.

## Current State

**Implemented and proven:** `build.py`, `publish.py`, and `job/updates.c`.
A release built for `darwin-arm64` produced `core.tar.gz` (10,830 bytes,
sha256 `3d4c188f…`), `publish.py` wrote `stable.json`, and the C resolver
parsed that exact file and returned a plan — the publisher and the finder are
tested against each other, not against copies.

**Sections present:** `re/nio` (→ `nio.dylib`) and `job` itself (→ `job.dylib`).

**Not yet present:** the `app` image and `full` applications. No R5–R6
repository declares a module unit, and no application declares an executable
unit outside its tests. `build.py --check` names every gap; it does not invent
an artifact that does not build.

## Scope and Limitations

- `build.py` builds through `b`; it never compiles or links anything itself.
- No downloader here, by design. That is R2 net work in vexspoke.
- No signing or notarization. A release is bytes and a hash; trust is the
  transport's problem until a signature step exists.
- Release assets are flat on GitHub: `core.tar.gz`, `app.tar.gz`,
  `<app>.tar.gz`. The document carries the URL, so hosting can move without a
  client change.
- The image map is explicit rather than read from `tools/platforms.json`, so
  this repository stands alone without the tools layer.
