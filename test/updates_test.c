#include "job/updates.h"

#include <stdio.h>
#include <string.h>

static int failures = 0;
static int checks = 0;

// Plans carry length-delimited views into the document (net/json.h keeps
// unescaped strings in place), so equality is length plus bytes.
static bool same(const char *view, u32 length, const char *expected)
{
    return view != nullptr && length == (u32) strlen(expected) &&
           strncmp(view, expected, length) == 0;
}

static void check(bool condition, const char *label)
{
    ++checks;
    if (!condition)
    {
        printf("FAIL %s\n", label);
        ++failures;
    }
}

static const char *DOCUMENT =
    "{"
    "  \"channel\": \"stable\","
    "  \"release\": \"1.4.0\","
    "  \"core\": {"
    "    \"darwin-arm64\": { \"version\": \"1.4.0\", \"url\": \"https://x/core.dylib\","
    "                       \"sha256\": \"aa\", \"size\": 10 }"
    "  },"
    "  \"app\": {"
    "    \"darwin-arm64\": { \"version\": \"1.4.0\", \"url\": \"https://x/app.dylib\","
    "                       \"sha256\": \"bb\", \"size\": 20 }"
    "  },"
    "  \"applications\": {"
    "    \"semicolon\": {"
    "      \"darwin-arm64\": {"
    "        \"app\": { \"version\": \"1.4.0\", \"url\": \"https://x/semicolon.app.zip\","
    "                 \"sha256\": \"cc\", \"size\": 30 }"
    "      }"
    "    }"
    "  }"
    "}";

static UpdateRequest requestOf(UpdateKind kind, const char *current)
{
    UpdateRequest request = {0};
    request.kind = kind;
    request.channel = "stable";
    request.platform = "darwin-arm64";
    request.current = current;
    return request;
}

int main(int argc, char **argv)
{
    static JsonNode nodes[256];
    static char scratch[2048];

    UpdateStore store = UpdateStore(nodes, 256, scratch, 2048);
    check(UpdateStore_parse(&store, DOCUMENT), "parses a channel document");

    UpdateRequest core = requestOf(UPDATE_KIND_CORE, "1.3.0");
    UpdatePlan plan = UpdateStore_resolve(&store, &core);
    check(plan.available, "core is newer than 1.3.0");
    check(plan.kind == UPDATE_KIND_CORE, "core keeps its kind");
    check(same(plan.version, plan.versionLength, "1.4.0"), "core version");
    check(plan.size == 10, "core size");
    check(!plan.requiresRestart, "a core swap needs no restart");

    UpdateRequest installed = requestOf(UPDATE_KIND_CORE, "1.4.0");
    check(!UpdateStore_resolve(&store, &installed).available, "same version is not an update");

    UpdateRequest ahead = requestOf(UPDATE_KIND_CORE, "1.5.0");
    check(!UpdateStore_resolve(&store, &ahead).available, "never proposes a downgrade");

    UpdateRequest fresh = requestOf(UPDATE_KIND_CORE, nullptr);
    check(UpdateStore_resolve(&store, &fresh).available, "a first install always has a target");

    UpdateRequest other = requestOf(UPDATE_KIND_CORE, "1.3.0");
    other.platform = "windows-x86_64";
    check(!UpdateStore_resolve(&store, &other).available, "an absent platform has no plan");

    UpdateRequest full = requestOf(UPDATE_KIND_FULL, "1.3.0");
    full.application = "semicolon";
    UpdatePlan fullPlan = UpdateStore_resolve(&store, &full);
    check(fullPlan.available, "full application resolves");
    check(fullPlan.requiresRestart, "a full application needs a restart");
    check(same(fullPlan.version, fullPlan.versionLength, "1.4.0"), "full version");
    check(fullPlan.url != nullptr &&
              strstr(fullPlan.url, "semicolon.app.zip") != nullptr,
          "darwin defaults to the .app archive");

    UpdateRequest missing = requestOf(UPDATE_KIND_FULL, "1.3.0");
    missing.application = "primeagen";
    check(!UpdateStore_resolve(&store, &missing).available, "an absent application has no plan");

    UpdateStore empty = UpdateStore_zero();
    check(!UpdateStore_parse(&empty, ""), "an empty document fails closed");
    check(!UpdateStore_resolve(&empty, &core).available, "an unparsed store offers nothing");

    check(strcmp(UpdateKind_title(UPDATE_KIND_FULL), "full") == 0, "kind title");

    // Integration: the document publish.py produced must resolve here, or the
    // C finder and the Python publisher have drifted.
    if (argc > 1)
    {
        static char liveDocument[8192];
        FILE *file = fopen(argv[1], "rb");
        check(file != nullptr, "published channel opens");
        if (file != nullptr)
        {
            bytes read = fread(liveDocument, 1, sizeof(liveDocument) - 1, file);
            liveDocument[read] = '\0';
            fclose(file);

            static JsonNode liveNodes[512];
            static char liveScratch[4096];
            UpdateStore live = UpdateStore(liveNodes, 512, liveScratch, 4096);
            check(UpdateStore_parse(&live, liveDocument), "published channel parses");

            UpdateRequest liveCore = requestOf(UPDATE_KIND_CORE, nullptr);
            UpdatePlan livePlan = UpdateStore_resolve(&live, &liveCore);
            check(livePlan.available, "published channel offers core");
            check(same(livePlan.version, livePlan.versionLength, "0.1.0"),
                  "published core version");
            check(livePlan.url != nullptr && livePlan.urlLength > 0, "published core url");
            check(livePlan.size > 0, "published core size");
        }
    }

    if (failures == 0)
    {
        printf("job/updates: %d checks passed\n", checks);
        return 0;
    }
    printf("job/updates: %d of %d check(s) failed\n", failures, checks);
    return 1;
}
