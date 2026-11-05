// job/updates.h — the update finder: a pure resolver over a channel document.
//
// The document arrives as text; a plan comes out. There is no socket here and
// no file here: the caller fetches the channel document (vexsppoke's net
// layer), hands it over once, and asks as many questions as it likes. Every
// string in the plan is a view INTO that document, so the document must
// outlive every plan taken from it.

#ifndef JOB_UPDATES_H
#define JOB_UPDATES_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "c23/constructor.h"
#include "net/json.h"

// Which artifact the caller is asking about. CORE and APP are hot-swappable
// dylib images; FULL is a complete application laid down by the shell, which
// is why only FULL ever requires a restart.
typedef enum UpdateKind {
    UPDATE_KIND_UNKNOWN = 0,
    UPDATE_KIND_CORE,
    UPDATE_KIND_APP,
    UPDATE_KIND_FULL,
} UpdateKind;

// What the caller knows about itself. `current` is the installed version, or
// nullptr on a first install. `application` names the app for FULL only.
// `format` picks the archive for FULL ("app", "exe", "tar.gz"); nullptr takes
// the platform default.
typedef struct UpdateRequest {
    UpdateKind kind;
    const char *channel;
    const char *platform;
    const char *current;
    const char *application;
    const char *format;
} UpdateRequest;

// The answer. `available` is false when the channel is missing, malformed, or
// not newer than `current` — a resolver never proposes a downgrade.
//
// The string fields are VIEWS into the channel document and are
// length-delimited, not NUL-terminated (net/json.h keeps unescaped strings in
// place). Use the matching length field; copy through a bounded Str when a
// terminator is required.
typedef struct UpdatePlan {
    bool available;
    UpdateKind kind;
    bool requiresRestart;
    const char *version;
    uint32_t versionLength;
    const char *url;
    uint32_t urlLength;
    const char *sha256;
    uint32_t sha256Length;
    uint64_t size;
} UpdatePlan;

// Caller-owned storage for one parsed document. No allocation of its own: the
// node pool and scratch arena are the embedder's, sized for its channel class.
typedef struct UpdateStore {
    JsonNode *nodes;
    uint32_t nodeCapacity;
    char *scratch;
    uint32_t scratchCapacity;
    JsonDoc document;
    bool parsed;
} UpdateStore;

UpdateStore UpdateStore_4(JsonNode *nodes, uint32_t nodeCapacity,
                          char *scratch, uint32_t scratchCapacity);
#define UpdateStore(...) CONSTRUCTOR_DISPATCH(UpdateStore, __VA_ARGS__)
#define UpdateStore_zero() UpdateStore_4(nullptr, 0, nullptr, 0)

// Parse one channel document. Returns false and leaves the store unresolved on
// any syntax error, pool exhaustion, or an empty document.
bool UpdateStore_parse(UpdateStore *self, const char *text);

// Answer for one request. Returns a zeroed plan (available == false) when the
// store was never parsed or the request names no artifact it can find.
UpdatePlan UpdateStore_resolve(const UpdateStore *self, const UpdateRequest *request);

// Stable names for logs and toString projections.
const char *UpdateKind_title(UpdateKind kind);

#endif
