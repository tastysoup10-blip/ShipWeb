#ifdef __EMSCRIPTEN__

#include "DataTransfer.h"

#include <ctime>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <emscripten.h>
#include <spdlog/spdlog.h>
#include <zip.h>

#include <ship/Context.h>
#include "soh/Notification/Notification.h"

namespace fs = std::filesystem;

static const char* kExportZipPath = "/tmp/shipweb-export.zip";
static const char* kImportZipPath = "/tmp/shipweb-import.zip";
static const char* kModImportZipPath = "/tmp/shipweb-mod-import.zip";

EM_JS(void, shipweb_download_file, (const char* path, const char* fileName), {
    const filePath = UTF8ToString(path);
    const blob = new Blob([FS.readFile(filePath)], { type: "application/zip" });
    FS.unlink(filePath);

    const url = URL.createObjectURL(blob);
    const link = document.createElement("a");
    link.href = url;
    link.download = UTF8ToString(fileName);
    document.body.appendChild(link);
    link.click();
    link.remove();
    setTimeout(() => URL.revokeObjectURL(url), 60000);
});

// Opening the file picker needs a recent click, which the menu button provides.
EM_JS(void, shipweb_pick_import_file, (const char* path), {
    const zipPath = UTF8ToString(path);
    const input = document.createElement("input");
    input.type = "file";
    input.accept = ".zip,application/zip";
    input.onchange = async () => {
        const file = input.files[0];
        if (!file) {
            return;
        }
        // confirm()/alert() block the page, which would leave the audio looping its last buffer: pause it meanwhile.
        const audio = Module["SDL2"]?.audioContext;
        const audioWasRunning = audio?.state === "running";
        if (audioWasRunning) {
            await audio.suspend();
        }
        const resumeAudio = () => audioWasRunning && audio.resume();

        if (!confirm("Importing \"" + file.name + "\" replaces the saves and settings it contains, then reloads " +
                     "the game. Continue?")) {
            resumeAudio();
            return;
        }
        FS.writeFile(zipPath, new Uint8Array(await file.arrayBuffer()));
        const imported = _ShipWeb_ImportZip();
        FS.unlink(zipPath);
        if (imported < 0) {
            alert("\"" + file.name + "\" isn't a valid ShipWeb data file. Nothing was changed.");
            resumeAudio();
            return;
        }
        // Make sure the imported files reach IndexedDB before reloading.
        shipSaveData(() => location.reload());
    };
    input.click();
});

static bool IsSafeEntryName(const std::string& name) {
    if (name.empty() || name[0] == '/' || name[0] == '\\' || name.find(':') != std::string::npos) {
        return false;
    }
    for (const auto& part : fs::path(name)) {
        if (part == "..") {
            return false;
        }
    }
    return true;
}

static bool AddDirectoryToZip(zip_t* zip, const fs::path& root) {
    std::error_code ec;
    for (auto it = fs::recursive_directory_iterator(root, ec); !ec && it != fs::recursive_directory_iterator();
         it.increment(ec)) {
        const std::string name = fs::relative(it->path(), root).generic_string();
        if (it->is_directory()) {
            if (zip_dir_add(zip, name.c_str(), ZIP_FL_ENC_UTF_8) < 0) {
                return false;
            }
        } else if (it->is_regular_file()) {
            zip_source_t* source = zip_source_file(zip, it->path().c_str(), 0, ZIP_LENGTH_TO_END);
            if (source == nullptr) {
                return false;
            }
            if (zip_file_add(zip, name.c_str(), source, ZIP_FL_ENC_UTF_8 | ZIP_FL_OVERWRITE) < 0) {
                zip_source_free(source);
                return false;
            }
        }
    }
    return !ec;
}

void ShipWeb::ExportData() {
    const fs::path dataDir = Ship::Context::GetAppDirectoryPath();

    int error = 0;
    zip_t* zip = zip_open(kExportZipPath, ZIP_CREATE | ZIP_TRUNCATE, &error);
    if (zip == nullptr) {
        SPDLOG_ERROR("Couldn't create the export zip (libzip error {})", error);
        return;
    }
    if (!AddDirectoryToZip(zip, dataDir)) {
        SPDLOG_ERROR("Couldn't add the data files to the export zip: {}", zip_strerror(zip));
        zip_discard(zip);
        return;
    }
    if (zip_close(zip) < 0) {
        SPDLOG_ERROR("Couldn't write the export zip: {}", zip_strerror(zip));
        zip_discard(zip);
        return;
    }

    char date[16];
    const std::time_t now = std::time(nullptr);
    std::strftime(date, sizeof(date), "%Y-%m-%d", std::localtime(&now));
    shipweb_download_file(kExportZipPath, ("shipweb-data-" + std::string(date) + ".zip").c_str());
    Notification::Emit({ .message = "Data exported" });
}

void ShipWeb::ImportData() {
    shipweb_pick_import_file(kImportZipPath);
}

// Opening the picker from the mod menu keeps it within a direct user gesture.
EM_JS(void, shipweb_pick_mod_zip, (const char* path), {
    const zipPath = UTF8ToString(path);
    const input = document.createElement("input");
    input.type = "file";
    input.accept = ".zip,application/zip";
    input.onchange = async () => {
        const file = input.files[0];
        if (!file) return;
        if (file.size > 128 * 1024 * 1024) {
            alert("That ZIP is too large. The current limit is 128 MB.");
            return;
        }
        if (!confirm("Import \"" + file.name + "\" as a ShipWeb asset mod? It will be stored in this browser. " +
                     "The game will reload after import. Continue?")) return;

        FS.writeFile(zipPath, new Uint8Array(await file.arrayBuffer()));
        const imported = _ShipWeb_ImportModZip();
        FS.unlink(zipPath);
        if (imported < 0) {
            alert("Couldn't import that ZIP. Use a ZIP containing .otr or .o2r mod archives.");
            return;
        }
        shipSaveData(() => location.reload());
    };
    input.click();
});

static bool IsSafeModEntryName(const std::string& name) {
    if (name.empty() || name[0] == '/' || name[0] == '\\' || name.find(':') != std::string::npos) return false;
    for (const auto& part : fs::path(name)) {
        if (part == "..") return false;
    }
    return true;
}

void ShipWeb::ImportModZip() {
    shipweb_pick_mod_zip(kModImportZipPath);
}

// Extract compatible asset archives into a fresh directory under the app's persistent /mods folder.
// The existing SoH mod scanner discovers .otr/.o2r files recursively on the next startup.
extern "C" EMSCRIPTEN_KEEPALIVE int ShipWeb_ImportModZip() {
    int error = 0;
    zip_t* zip = zip_open(kModImportZipPath, ZIP_RDONLY, &error);
    if (zip == nullptr) {
        SPDLOG_ERROR("Couldn't open mod ZIP (libzip error {})", error);
        return -1;
    }

    constexpr zip_int64_t kMaxEntries = 4096;
    constexpr zip_uint64_t kMaxFileBytes = 128ULL * 1024 * 1024;
    constexpr zip_uint64_t kMaxTotalBytes = 256ULL * 1024 * 1024;
    const zip_int64_t entryCount = zip_get_num_entries(zip, 0);
    if (entryCount <= 0 || entryCount > kMaxEntries) {
        zip_discard(zip);
        return -1;
    }

    std::vector<std::pair<std::string, std::vector<char>>> files;
    zip_uint64_t totalBytes = 0;
    bool hasAssetArchive = false;
    for (zip_int64_t i = 0; i < entryCount; i++) {
        zip_stat_t stat;
        zip_stat_init(&stat);
        if (zip_stat_index(zip, i, 0, &stat) < 0 || stat.name == nullptr) {
            zip_discard(zip);
            return -1;
        }
        const std::string name = stat.name;
        if (!name.empty() && name.back() == '/') continue;
        if (!IsSafeModEntryName(name) || stat.size > kMaxFileBytes || totalBytes > kMaxTotalBytes - stat.size) {
            zip_discard(zip);
            return -1;
        }
        const std::string extension = fs::path(name).extension().string();
        if (StringHelper::IEquals(extension, ".otr") || StringHelper::IEquals(extension, ".o2r")) {
            hasAssetArchive = true;
        }

        std::vector<char> contents(static_cast<size_t>(stat.size));
        zip_file_t* file = zip_fopen_index(zip, i, 0);
        const bool read = file != nullptr &&
            zip_fread(file, contents.data(), static_cast<zip_uint64_t>(stat.size)) == static_cast<zip_int64_t>(stat.size);
        if (file != nullptr) zip_fclose(file);
        if (!read) {
            zip_discard(zip);
            return -1;
        }
        totalBytes += stat.size;
        files.emplace_back(name, std::move(contents));
    }
    zip_discard(zip);
    if (files.empty() || !hasAssetArchive) return -1;

    const fs::path modsDir = Ship::Context::GetAppDirectoryPath() / "mods";
    const auto stamp = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    const fs::path stagingDir = modsDir / (".import-" + std::to_string(stamp));
    const fs::path finalDir = modsDir / ("imported-" + std::to_string(stamp));

    try {
        fs::create_directories(stagingDir);
        for (const auto& [name, contents] : files) {
            const fs::path target = stagingDir / fs::path(name);
            fs::create_directories(target.parent_path());
            std::ofstream out(target, std::ios::binary | std::ios::trunc);
            if (!out) throw std::runtime_error("Unable to create mod file");
            out.write(contents.data(), static_cast<std::streamsize>(contents.size()));
            if (!out) throw std::runtime_error("Unable to write mod file");
        }
        fs::rename(stagingDir, finalDir);
    } catch (const std::exception& e) {
        SPDLOG_ERROR("Mod ZIP extraction failed: {}", e.what());
        std::error_code ec;
        fs::remove_all(stagingDir, ec);
        return -1;
    }

    SPDLOG_INFO("Imported {} files into {}", files.size(), finalDir.string());
    return static_cast<int>(files.size());
}

// Called from the file picker once the chosen zip is in kImportZipPath. Returns the number of files written, or -1 if
// the zip can't be read (in which case nothing is written).
extern "C" EMSCRIPTEN_KEEPALIVE int ShipWeb_ImportZip() {
    int error = 0;
    zip_t* zip = zip_open(kImportZipPath, ZIP_RDONLY, &error);
    if (zip == nullptr) {
        SPDLOG_ERROR("Couldn't open the import zip (libzip error {})", error);
        return -1;
    }

    // Read everything first so a broken zip doesn't leave the data half replaced.
    std::vector<std::pair<std::string, std::vector<char>>> files;
    const zip_int64_t entryCount = zip_get_num_entries(zip, 0);
    for (zip_int64_t i = 0; i < entryCount; i++) {
        zip_stat_t stat;
        if (zip_stat_index(zip, i, 0, &stat) < 0) {
            zip_discard(zip);
            return -1;
        }
        const std::string name = stat.name;
        if (name.back() == '/') {
            continue;
        }
        if (!IsSafeEntryName(name)) {
            SPDLOG_WARN("Skipping unsafe path in the import zip: {}", name);
            continue;
        }
        std::vector<char> contents(stat.size);
        zip_file_t* file = zip_fopen_index(zip, i, 0);
        const bool read = file != nullptr && zip_fread(file, contents.data(), stat.size) == (zip_int64_t)stat.size;
        if (file != nullptr) {
            zip_fclose(file);
        }
        if (!read) {
            SPDLOG_ERROR("Couldn't read {} from the import zip", name);
            zip_discard(zip);
            return -1;
        }
        files.emplace_back(name, std::move(contents));
    }
    zip_discard(zip);

    if (files.empty()) {
        return -1;
    }

    const fs::path dataDir = Ship::Context::GetAppDirectoryPath();
    for (const auto& [name, contents] : files) {
        const fs::path target = dataDir / name;
        fs::create_directories(target.parent_path());
        std::ofstream out(target, std::ios::binary | std::ios::trunc);
        out.write(contents.data(), contents.size());
    }
    SPDLOG_INFO("Imported {} files", files.size());
    return (int)files.size();
}

#endif
