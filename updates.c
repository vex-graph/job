// job/updates.c — the update finder.
//
// A resolver, not a transport. It reads a channel document that someone else
// fetched and answers one question: is there a newer artifact for this
// channel, platform and kind, and where does it live. Version comparison is
// numeric per dot-separated segment, so "1.10.0" is newer than "1.9.9" and a
// tag suffix never decides an update.

#include "job/updates.h"

#include <stdio.h>
#include <string.h>

#include "annotation/annotation.h"

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

// Reads up to four numeric segments. A suffix after the numbers ("1.4.0-beta")
// is skipped, so a tag never decides an update; a missing segment reads as 0,
// so "1.2" equals "1.2.0".
static void UpdateStore_versionParts(const char *text, bytes length,
                                     unsigned long parts[UPDATE_VERSION_PARTS])
{
    bytes index = 0;
    for (bytes part = 0; part < UPDATE_VERSION_PARTS; ++part)
    {
        unsigned long value = 0;
        while (index < length && text[index] >= '0' && text[index] <= '9')
        {
            value = value * 10u + (unsigned long) (text[index] - '0');
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
static int UpdateStore_compareVersion(const char *left, bytes leftLength, const char *right)
{
    unsigned long a[UPDATE_VERSION_PARTS] = {0};
    unsigned long b[UPDATE_VERSION_PARTS] = {0};
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
    switch ((*request).kind)
    {
        case UPDATE_KIND_CORE:
            snprintf(path, sizeof(path), "core.%s", (*request).platform);
            break;
        case UPDATE_KIND_APP:
            snprintf(path, sizeof(path), "app.%s", (*request).platform);
            break;
        case UPDATE_KIND_FULL:
        {
            if ((*request).application == nullptr)
                return UPDATE_NO_NODE;
            const char *format = (*request).format != nullptr
                                     ? (*request).format
                                     : UpdateStore_defaultFormat((*request).platform);
            snprintf(path, sizeof(path), "applications.%s.%s.%s",
                     (*request).application, (*request).platform, format);
            break;
        }
        default:
            return UPDATE_NO_NODE;
    }
    return Json_path(&(*self).document, Json_root(&(*self).document), path);
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
    (*self).parsed = false;
    if (text == nullptr || (*self).nodes == nullptr || (*self).nodeCapacity == 0)
        return false;

    JsonDoc document = {0};
    bool ok = Json_parse(&document, (*self).nodes, (*self).nodeCapacity,
                         (*self).scratch, (*self).scratchCapacity, text);
    (*self).document = document;
    (*self).parsed = ok;
    return ok;
}

UpdatePlan UpdateStore_resolve(const UpdateStore *self, const UpdateRequest *request)
{
    UpdatePlan plan = {0};
    if (request == nullptr || !(*self).parsed)
        return plan;

    plan.kind = (*request).kind;
    plan.requiresRestart = (*request).kind == UPDATE_KIND_FULL;

    JsonRef artifact = UpdateStore_artifact(self, request);
    if (artifact == UPDATE_NO_NODE)
        return plan;

    const JsonDoc *document = &(*self).document;
    JsonStringView version = Json_getString(document, Json_member(document, artifact, "version"));
    JsonStringView url = Json_getString(document, Json_member(document, artifact, "url"));
    JsonStringView sha256 = Json_getString(document, Json_member(document, artifact, "sha256"));
    if (version.data == nullptr || url.data == nullptr || sha256.data == nullptr)
        return plan;

    // A first install always has something to do; an installed client only
    // moves forward.
    if ((*request).current != nullptr &&
        UpdateStore_compareVersion(version.data, version.length, (*request).current) <= 0)
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
