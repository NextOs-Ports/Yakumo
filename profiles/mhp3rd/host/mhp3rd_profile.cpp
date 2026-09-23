// Everything PortableKit has to be told about Monster Hunter Portable 3rd HD
// Ver. The framework reads the rest out of the game's own executable.
//
// These values used to be spread through the host: install/game_identity.hpp
// (the release and its hashes), mhp3rd_profile.hpp (the load address, the
// 64 MiB layout and the overlay slots), executable_preparation.cpp (the "~PSP"
// tag and its key), the save-data code (the folders) and the ad hoc code (the
// product code). The hashes match config/mhp3rd_npjb40001.toml and the README.

#include "profile.hpp"

#include "camera/game_aspect.hpp"
#include "camera/game_camera.hpp"

#include "psprecomp/elf32.hpp"
#include "psprecomp/runtime.hpp"

#if defined(MHP3RD_CAMERA_HELPER_UNIT)
// Declared here rather than through generated_units.hpp, which corpora
// generated before that header existed do not have. Every generated unit has
// this signature. CMake names the unit that holds the camera's rotation helper,
// so a new partition of the code cannot hand the camera the wrong one.
namespace psprecomp {
void MHP3RD_CAMERA_HELPER_UNIT(Runtime &, AllegrexContext &);
}
#endif

namespace portablekit {
namespace {

// Overlay slots, from the executable's section table. They sit inside the load
// image's reserved BSS, and the game copies code into them at run time, so a
// jump into one stops the runtime until that overlay has its own corpus.
// The end of each slot is the start of the next one.
constexpr std::uint32_t kOverlaySlots[] = {
    0x0A001780u,  // demo_sub, game_sub
    0x0A055E80u,  // P_m*/P_v* maps
    0x0A05E600u,  // *_task mode overlays
    0x0A1BB000u,  // em*m0 monsters, result
    0x0A1EFE80u,  // em*m1
    0x0A224D00u,  // em*m2
    0x0A239780u,  // em*m3
    0x0A24E200u,  // we*player00 weapons
    0x0A25BA80u,  // we*player01
    0x0A269300u,  // we*player02
    0x0A276B80u,  // we*player03, tutorialm1
    0x0A284400u,  // P_v00 and village maps
    0x0A285200u,  // end of the load image
};

// The HD release saves under the original PSP release's product code. The
// install data is a cache the game rebuilds from the disc, so an export leaves
// it out.
constexpr SaveFolder kSaveFolders[] = {
    {"ULJM05800", "Game data", true},
    {"ULJM05800QST", "Downloaded quests", true},
    {"ULJM05800DAT", "Install data", false},
};

// The disc a player is most likely to have instead.
constexpr OtherRelease kOtherReleases[] = {
    {"ULJM05800", "This is the original PSP release of the game, which Yakumo does not support."},
};

// The game's camera, driven from PortableKit's camera input (camera/
// game_camera.hpp). The framework asks through these; the answers are the
// game's.
bool mouse_aim(float &x, float &y) {
    const auto direction = mhp3rd::camera::game_camera_mouse_aim();
    if (!direction) return false;
    x = direction->x;
    y = direction->y;
    return true;
}

constexpr CameraDriver kCamera{
    .frame = &mhp3rd::camera::game_camera_frame,
    .driving = &mhp3rd::camera::game_camera_driving,
    .aim_boost = &mhp3rd::camera::game_camera_aim_boost,
    .mouse_aim = &mouse_aim,
    .mouse_stock_turn = &mhp3rd::camera::game_camera_mouse_stock_turn,
    .degrees_per_second = &mhp3rd::camera::game_camera_degrees_per_second,
};

// Both drivers check the game's code before they write anything and stay out,
// saying why, if it is not the code they were written against. Nothing is
// hooked yet here: the camera waits until the player turns Analog camera on.
void patch_loaded_image(psprecomp::Runtime &runtime, const psprecomp::Elf32Image &) {
    (void)mhp3rd::camera::prepare_game_aspect(runtime);
#if defined(MHP3RD_CAMERA_HELPER_UNIT)
    (void)mhp3rd::camera::prepare_game_camera(runtime, &psprecomp::MHP3RD_CAMERA_HELPER_UNIT);
#endif
}

} // namespace

const GameProfile &game() {
    static const GameProfile profile{
        .app_name = "Yakumo",
        .project_name = "Yakumo",
        .env_prefix = "MHP3RD",
        // SDL_GetPrefPath("Yakumo", "MHP3rd"): where every earlier version
        // kept its settings, its saves and the prepared executable.
        .data_organization = "Yakumo",
        .data_application = "MHP3rd",

        .disc_id = "NPJB40001",
        .disc_id_display = "NPJB-40001",
        .game_title = "Monster Hunter Portable 3rd HD Ver.",
        .executable_path_on_disc = "PSP_GAME/SYSDIR/EBOOT.BIN",
        .param_sfo_path_on_disc = "PSP_GAME/PARAM.SFO",
        .release_name = "the Japanese release",
        .other_releases = kOtherReleases,
        // SHA-256 of PSP_GAME/SYSDIR/EBOOT.BIN as it is on the disc, and of
        // the executable the recompiled code was generated from.
        .encrypted_executable_sha256 = "79e25f3512d56e0f7bf5c48351d7d0d255269675ffc8811ac5599322bb66945e",
        .executable_sha256 = "55c0598436c0753b04331f8e95d406f832d9217806e3a896fed0e88b33637d8c",
        // The "~PSP" header's tag, and the key the public PSP key tables give
        // for it.
        .decryption_tag = 0xD9160BF0u,
        .decryption_key = {0x83, 0x83, 0xF1, 0x37, 0x53, 0xD0, 0xBE, 0xFC,
                           0x8D, 0xA7, 0x32, 0x52, 0x46, 0x0A, 0xC2, 0xC2},

        // The PlayStation 3 release's 64 MiB layout.
        .load_base = psprecomp::kDefaultPspUserLoadBase,
        .guest_ram_bytes = 64u * 1024u * 1024u,
        .boot_path = "disc0:/PSP_GAME/SYSDIR/EBOOT.BIN",
        .overlay_slots = kOverlaySlots,
        // The overlay libraries built before Yakumo moved onto PortableKit
        // export mhp3rd_overlay_info and mhp3rd_register_overlay.
        .legacy_overlay_symbol_prefix = "mhp3rd",

        .save_game_name = "ULJM05800",
        .save_folders = kSaveFolders,

        // The product code other players see in the announcement. Players of
        // the game's PSP release log in with the same one.
        .adhoc_product_code = "ULJM05800",

        .patch_loaded_image = &patch_loaded_image,
        .camera = &kCamera,
        .view_aspect_frame = &mhp3rd::camera::game_aspect_frame,
    };
    return profile;
}

} // namespace portablekit
