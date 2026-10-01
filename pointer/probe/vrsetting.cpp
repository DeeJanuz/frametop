// Read or change one SteamVR setting through vrserver (IVRSettings). Editing steamvr.vrsettings
// while SteamVR runs doesn't stick: vrserver writes its own copy back on the next change.
// Runs as a background OpenVR client (in the dev container).
// Usage: vrsetting get SECTION KEY
//        vrsetting set-bool SECTION KEY true|false
//        vrsetting remove SECTION KEY
#include <openvr.h>

#include <cstdio>
#include <cstring>

int main(int argc, char **argv) {
    if (argc < 4 || (std::strcmp(argv[1], "set-bool") == 0 && argc < 5)) {
        std::fprintf(stderr, "usage: %s get|set-bool|remove SECTION KEY [true|false]\n", argv[0]);
        return 2;
    }
    const char *cmd = argv[1], *section = argv[2], *key = argv[3];
    vr::EVRInitError err = vr::VRInitError_None;
    vr::VR_Init(&err, vr::VRApplication_Background);
    if (err != vr::VRInitError_None) {
        std::printf("VR_Init failed: %s\n", vr::VR_GetVRInitErrorAsEnglishDescription(err));
        return 1;
    }
    vr::EVRSettingsError serr = vr::VRSettingsError_None;
    int rc = 0;
    if (!std::strcmp(cmd, "get")) {
        char s[512] = "";
        vr::VRSettings()->GetString(section, key, s, sizeof s, &serr);
        if (serr == vr::VRSettingsError_None) std::printf("%s\n", s);
        else {
            const bool b = vr::VRSettings()->GetBool(section, key, &serr);
            std::printf("%s\n", serr == vr::VRSettingsError_None ? (b ? "true" : "false") : "(unset)");
        }
    } else if (!std::strcmp(cmd, "set-bool")) {
        vr::VRSettings()->SetBool(section, key, !std::strcmp(argv[4], "true"), &serr);
    } else if (!std::strcmp(cmd, "remove")) {
        vr::VRSettings()->RemoveKeyInSection(section, key, &serr);
    } else {
        std::fprintf(stderr, "unknown command %s\n", cmd);
        rc = 2;
    }
    if (serr != vr::VRSettingsError_None) {
        std::printf("error: %s\n", vr::VRSettings()->GetSettingsErrorNameFromEnum(serr));
        rc = 1;
    }
    vr::VR_Shutdown();
    return rc;
}
