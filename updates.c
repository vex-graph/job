// job/updates.c — the update finder.
//
// A resolver, not a transport. It reads a channel document that someone else
// fetched and answers one question: is there a newer artifact for this
// channel, platform and kind, and where does it live. Version comparison is
// numeric per dot-separated segment, so "1.10.0" is newer than "1.9.9" and a
// tag suffix never decides an update.

#include "vex.h"
#include "job/updates.h"

#include <stdio.h>
#include <string.h>


;;DEFINITION
/**
 * ============================================================================
 * DEFINITION: UpdateStore
 * ============================================================================
 * The update finder: a pure function of (channel document, request). Parsing
 * uses net/json.h with caller-owned pools, so a resolver costs no allocation
 * and a document that does not fit simply fails closed. Every returned string
 * is a view into the parsed document; the caller owns both the document and
 * the storage it was parsed into, and must keep them alive while a plan is in
 * use. The resolver never downloads, never writes, and never picks a version
 * that is not strictly newer than the installed one.
 * ============================================================================
 */

static const JsonRef UPDATE_NO_NODE = -1;
#define UPDATE_VERSION_PARTS 4u

static inline UpdateKind UpdateRequest_kind(const UpdateRequest *self) { return (*self).kind; }
static inline const char *UpdateRequest_platform(const UpdateRequest *self) { return (*self).platform; }
static inline const char *UpdateRequest_current(const UpdateRequest *self) { return (*self).current; }
static inline const char *UpdateRequest_application(const UpdateRequest *self) { return (*self).application; }
static inline const char *UpdateRequest_format(const UpdateRequest *self) { return (*self).format; }

static inline JsonNode *UpdateStore_nodes(const UpdateStore *self) { return (*self).nodes; }
static inline u32 UpdateStore_nodeCapacity(const UpdateStore *self) { return (*self).nodeCapacity; }
static inline char *UpdateStore_scratch(const UpdateStore *self) { return (*self).scratch; }
static inline u32 UpdateStore_scratchCapacity(const UpdateStore *self) { return (*self).scratchCapacity; }
static inline const JsonDoc *UpdateStore_document(const UpdateStore *self) { return &(*self).document; }
static inline bool UpdateStore_parsed(const UpdateStore *self) { return (*self).parsed; }

static inline fn UpdateStore_setDocument(UpdateStore *self, JsonDoc document) { (*self).document = document; }
static inline fn UpdateStore_setParsed(UpdateStore *self, bool parsed) { (*self).parsed = parsed; }

// Reads up to four numeric segments. A suffix after the numbers ("1.4.0-beta")
// is skipped, so a tag never decides an update; a missing segment reads as 0,
// so "1.2" equals "1.2.0".
static fn UpdateStore_versionParts(const char *text, bytes length,
                                     u64 parts[UPDATE_VERSION_PARTS])
{
    bytes index = 0;
    for (bytes part = 0; part < UPDATE_VERSION_PARTS; ++part)
    {
        u64 value = 0;
        while (index < length && text[index] >= '0' && text[index] <= '9')
        {
            value = value * 10u + (u64) (text[index] - '0');
            ++index;
        }
        parts[part] = value;
        while (index < length && text[index] != '.')
            ++index;
        if (index < length && text[index] == '.')
            ++index;
    }
}

// Compares a document view against a caller's C string.
static i32 UpdateStore_compareVersion(const char *left, bytes leftLength, const char *right)
{
    u64 a[UPDATE_VERSION_PARTS] = {0};
    u64 b[UPDATE_VERSION_PARTS] = {0};
    if (left == nullptr || right == nullptr)
        return 0;

    UpdateStore_versionParts(left, leftLength, a);
    UpdateStore_versionParts(right, strlen(right), b);

    for (bytes part = 0; part < UPDATE_VERSION_PARTS; ++part)
    {
        if (a[part] != b[part])
            return a[part] < b[part] ? -1 : 1;
    }
    return 0;
}

// The archive a platform ships by default when the request names no format.
static const char *UpdateStore_defaultFormat(const char *platform)
{
    if (platform == nullptr)
        return "tar.gz";
    if (strncmp(platform, "darwin", 6) == 0)
        return "app";
    if (strncmp(platform, "windows", 7) == 0)
        return "exe";
    return "tar.gz";
}

// Where the artifact lives in the document. Built into a bounded stack buffer:
// a channel document is a small, fixed vocabulary of names, not user data.
static JsonRef UpdateStore_artifact(const UpdateStore *self, const UpdateRequest *request)
{
    char path[192];
    switch (UpdateRequest_kind(request))
    {
        case UPDATE_KIND_CORE:
            snprintf(path, sizeof(path), "core.%s", UpdateRequest_platform(request));
            break;
        case UPDATE_KIND_APP:
            snprintf(path, sizeof(path), "app.%s", UpdateRequest_platform(request));
            break;
        case UPDATE_KIND_FULL:
        {
            const char *application = UpdateRequest_application(request);
            if (application == nullptr)
                return UPDATE_NO_NODE;
            const char *platform = UpdateRequest_platform(request);
            const char *format = UpdateRequest_format(request) != nullptr
                                     ? UpdateRequest_format(request)
                                     : UpdateStore_defaultFormat(platform);
            snprintf(path, sizeof(path), "applications.%s.%s.%s",
                     application, platform, format);
            break;
        }
        default:
            return UPDATE_NO_NODE;
    }
    const JsonDoc *document = UpdateStore_document(self);
    return Json_path(document, Json_root(document), path);
}

UpdateStore UpdateStore_4(JsonNode *nodes, u32 nodeCapacity,
                          char *scratch, u32 scratchCapacity)
{
    UpdateStore store = {0};
    store.nodes = nodes;
    store.nodeCapacity = nodeCapacity;
    store.scratch = scratch;
    store.scratchCapacity = scratchCapacity;
    store.parsed = false;
    return store;
}

bool UpdateStore_parse(UpdateStore *self, const char *text)
{
    UpdateStore_setParsed(self, false);
    if (text == nullptr || UpdateStore_nodes(self) == nullptr || UpdateStore_nodeCapacity(self) == 0)
        return false;

    JsonDoc document = {0};
    bool ok = Json_parse(&document, UpdateStore_nodes(self), UpdateStore_nodeCapacity(self),
                         UpdateStore_scratch(self), UpdateStore_scratchCapacity(self), text);
    UpdateStore_setDocument(self, document);
    UpdateStore_setParsed(self, ok);
    return ok;
}

UpdatePlan UpdateStore_resolve(const UpdateStore *self, const UpdateRequest *request)
{
    UpdatePlan plan = {0};
    if (request == nullptr || !UpdateStore_parsed(self))
        return plan;

    UpdateKind kind = UpdateRequest_kind(request);
    plan.kind = kind;
    plan.requiresRestart = kind == UPDATE_KIND_FULL;

    JsonRef artifact = UpdateStore_artifact(self, request);
    if (artifact == UPDATE_NO_NODE)
        return plan;

    const JsonDoc *document = UpdateStore_document(self);
    JsonStringView version = Json_getString(document, Json_member(document, artifact, "version"));
    JsonStringView url = Json_getString(document, Json_member(document, artifact, "url"));
    JsonStringView sha256 = Json_getString(document, Json_member(document, artifact, "sha256"));
    if (version.data == nullptr || url.data == nullptr || sha256.data == nullptr)
        return plan;

    // A first install always has something to do; an installed client only
    // moves forward.
    const char *current = UpdateRequest_current(request);
    if (current != nullptr &&
        UpdateStore_compareVersion(version.data, version.length, current) <= 0)
        return plan;

    plan.available = true;
    plan.version = version.data;
    plan.versionLength = version.length;
    plan.url = url.data;
    plan.urlLength = url.length;
    plan.sha256 = sha256.data;
    plan.sha256Length = sha256.length;
    plan.size = (u64) Json_getNumber(document, Json_member(document, artifact, "size"), 0);
    return plan;
}

const char *UpdateKind_title(UpdateKind kind)
{
    switch (kind)
    {
        case UPDATE_KIND_CORE:
            return "core";
        case UPDATE_KIND_APP:
            return "app";
        case UPDATE_KIND_FULL:
            return "full";
        default:
            return "unknown";
    }
}
