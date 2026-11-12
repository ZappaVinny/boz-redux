// Installing mods from .zip files, on every platform: unpacks into a staging folder in mods/,
// reads the mod.toml there, then (once the player agrees) swaps it in for the old copy.
#pragma once

#include "mods.h"

#include <string>
#include <vector>

struct StagedMod {
    bool ok = false;
    std::string error;      // why not, for the player
    std::string staging;    // mods/.<folder>.part, unpacked
    std::string folder;     // the folder it will have in mods/
    mod_info mod{};         // its manifest
    bool update = false;    // a mod with this id is installed (in folder)
    std::string old_version;
    std::vector<std::string> warnings;
};

// Unpacks zip_path into mods_dir/.<folder>.part. mod.toml must be at the zip's root or one
// folder down; entries outside that folder are left out, unsafe names refuse the zip.
// fallback_name names the folder when nothing else does (the zip's file name).
StagedMod mod_zip_stage(const std::string &zip_path, const std::string &mods_dir,
                        const std::string &fallback_name);

// Puts a staged mod in place (replacing the old copy); false with *error set.
bool mod_zip_commit(const StagedMod &staged, const std::string &mods_dir, std::string *error);

// Deletes a staged mod's folder.
void mod_zip_discard(const StagedMod &staged);

// Deletes an installed mod's folder; false with *error set.
bool mod_remove(const std::string &mods_dir, const std::string &folder, std::string *error);

// Removes staging and backup leftovers (.<name>.part, .<name>.old) from an interrupted install.
void mod_clean_leftovers(const std::string &mods_dir);

// A mod zip's id, name and version from its mod.toml, without unpacking; false when the file
// is not a mod zip.
bool mod_zip_peek(const std::string &zip_path, std::string *id, std::string *name,
                  std::string *version);

// Whether a zip entry name is safe to unpack (relative, no "..", no drive or device names).
bool mod_zip_safe_name(const std::string &name);
