// Lock the issue-#11 preload fences without loading or patching QEMU.
// The binary-patch path is a single-build lab tool: this suite proves the
// build-id allowlist refuses mismatches, the validation opt-in policy, and
// that the portrait dimension hacks stay behind an explicit guest profile.
#define SVGA3_PRELOAD_TEST
#include "../src/qemu_svga3d_preload.cpp"
#include <cstdio>
#include <cstring>

static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); ++failures; } } while (0)

// Must stay byte-identical to the single allowlisted lab build entry.
static const unsigned char kGoodBuildId[20] = {
    0x2e,0x87,0x0e,0x40,0x0e,0x16,0x92,0xf5,
    0xf5,0xa5,0x4d,0xa8,0x97,0xd1,0xdc,0x15,
    0x2b,0x18,0x72,0x78
};

int main() {
    // 1. Build-id allowlist: exact match only.
    CHECK(preload_build_id_allowed(kGoodBuildId, sizeof(kGoodBuildId)));
    unsigned char flipped[20];
    memcpy(flipped, kGoodBuildId, sizeof(flipped));
    flipped[19] ^= 0xff;
    CHECK(!preload_build_id_allowed(flipped, sizeof(flipped)));
    unsigned char zeroes[20] = {0};
    CHECK(!preload_build_id_allowed(zeroes, sizeof(zeroes)));
    CHECK(!preload_build_id_allowed(kGoodBuildId, 19));   // truncated
    CHECK(!preload_build_id_allowed(kGoodBuildId, 21));   // overlong
    CHECK(!preload_build_id_allowed(nullptr, 0));

    // 2. Build-id formatter: stable hex for refusal diagnostics.
    char hex[64];
    CHECK(preload_format_build_id(kGoodBuildId, sizeof(kGoodBuildId), hex, sizeof(hex)));
    CHECK(strcmp(hex, "2e870e400e1692f5f5a54da897d1dc152b187278") == 0);
    CHECK(!preload_format_build_id(kGoodBuildId, sizeof(kGoodBuildId), hex, 40)); // too small

    CHECK(!preload_format_build_id(kGoodBuildId, SIZE_MAX, hex, sizeof(hex)));

    // 3. Validation policy: explicit env wins; debug builds validate.
    setenv("SVGA3_VLKN_VALIDATE", "1", 1);
    CHECK(preload_validation_requested());
    setenv("SVGA3_VLKN_VALIDATE", "0", 1);
    CHECK(!preload_validation_requested());
    unsetenv("SVGA3_VLKN_VALIDATE");
#ifdef NDEBUG
    CHECK(!preload_validation_requested());
#else
    CHECK(preload_validation_requested());
#endif

    // 4. Portrait dimension hacks stay behind the explicit guest profile.
    unsetenv("SVGA3_VLKN_GUEST_PROFILE");
    CHECK(!portrait_profile_enabled());
    setenv("SVGA3_VLKN_GUEST_PROFILE", "playbook-portrait", 1);
    CHECK(portrait_profile_enabled());
    setenv("SVGA3_VLKN_GUEST_PROFILE", "landscape", 1);
    CHECK(!portrait_profile_enabled());
    setenv("SVGA3_VLKN_GUEST_PROFILE", "", 1);
    CHECK(!portrait_profile_enabled());

    if (failures == 0) printf("ALL PRELOAD FENCE CHECKS PASSED\n");
    return failures ? 1 : 0;
}
