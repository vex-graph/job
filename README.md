# job

Release builder, channel publisher, and zero-allocation C23 update resolver for the Vexgraph stack.

```bash
python3 build.py --check
python3 build.py --release 0.2.0
python3 publish.py --release 0.2.0 --channel stable
python3 publish.py --release 0.2.0 --channel stable --execute
```

## About

`hotcwap` can hot-swap dylibs at runtime, but it cannot build releases or discover new versions. `job` closes that loop without duplicating network transport: `build.py` builds versioned archives through `b`, `publish.py` hashes and publishes channel documents (`cutting-edge`, `beta`, `stable`), and `job/updates.h` resolves an update plan in pure C23. Downloading stays in `vexspoke`'s R2 net layer.

## Release loop

```text
job build      b + b.assembly.json          -> release tree, stamped once
job publish    hash + channel document      -> labelled release (stable/beta/cutting-edge)
runtime fetch  vexspoke (R2 net)            -> download, verify sha256, stage
hotcwap        MANIFEST_UPDATE / PROMOTE    -> new -> current, bump generation
hotcwap        Hot_poll                     -> dlopen new sections, swap
```

Every release cuts three artifact kinds from one version:

| Kind | Contents | Consumed by |
| :--- | :--- | :--- |
| `core` | R2–R4 dylibs (`"link": "module"`) | `hotcwap` in-process hot-swap |
| `app` | R5–R6 dylibs | `hotcwap` in-process hot-swap |
| `full` | Complete application archive | Initial install or full restart update |

## C23 resolver (`#include "job/updates.h"`)

Pure function of `(channel document, request)` using `net/json.h` over caller-owned pools with zero sockets, files, or allocations (`requiresRestart` is true only for `UPDATE_KIND_FULL`):

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

## Build and test

```bash
python3 build.py --check                                      # list repos missing module units
python3 build.py --release 0.2.0                              # build release tree + archives in dist/
python3 publish.py --release 0.2.0 --channel stable           # dry-run channel JSON
python3 publish.py --release 0.2.0 --channel stable --execute # upload + move channel pointer
```

Currently verified on `darwin-arm64` with `re/nio` (`nio.dylib`) and `job` (`job.dylib`) built into `core.tar.gz`, published to `stable.json`, and resolved end-to-end by `job/updates.c`.

## Future

- Add module (`"link": "module"`) and executable `b.assembly.json` units for R5–R6 apps to produce `app.tar.gz` and `full` archives.
- Add release signing and macOS notarization.
- Wire `vexspoke` R2 download + SHA-256 staging into `hotcwap`'s promotion ladder.

## Scope and Limitations

- **Core sections only so far**: Only `re/nio` and `job` emit module dylibs today; `app` and `full` images are not yet populated (`build.py --check` reports gaps).
- **No downloader**: Fetching bytes belongs to `vexspoke` R2 net, not `job`.
- **No signing or notarization**: Releases carry SHA-256 hashes only; cryptographic signature verification is not implemented yet.
- **Standalone image map**: Uses an explicit in-repo image map rather than reading `tools/platforms.json`.
