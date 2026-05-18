#include <augene2/storage.hpp>

#include <algorithm>
#include <format>
#include <fstream>
#include <optional>
#include <sstream>
#include <system_error>
#include <vector>

#include <choc/text/choc_JSON.h>

namespace augene2 {

namespace {

std::optional<std::string> tryReadTextFile(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream)
        return std::nullopt;

    std::ostringstream buffer;
    buffer << stream.rdbuf();
    return buffer.str();
}

bool readClipFile(const std::filesystem::path& path,
                  std::vector<umppi::Ump>& smf2clip,
                  std::string& error) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        error = std::format("Failed to open clip input '{}'.", path.string());
        return false;
    }

    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    if (!input.good() && !input.eof()) {
        error = std::format("Failed while reading clip input '{}'.", path.string());
        return false;
    }

    try {
        smf2clip = umppi::Ump::fromBytes(bytes);
        return true;
    } catch (const std::exception& ex) {
        error = std::format("Failed to parse MIDI2 clip '{}': {}.", path.string(), ex.what());
        return false;
    }
}

bool writeClipFile(const ProjectClip& clip,
                   const std::filesystem::path& path,
                   std::string& error) {
    std::ofstream out(path, std::ios::binary);
    if (!out) {
        error = std::format("Failed to open clip output '{}'.", path.string());
        return false;
    }

    for (const auto& ump : clip.smf2clip) {
        const auto bytes = ump.toBytes();
        out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        if (!out) {
            error = std::format("Failed while writing clip output '{}'.", path.string());
            return false;
        }
    }
    return true;
}

std::string clipKindToString(ProjectClipKind kind) {
    switch (kind) {
        case ProjectClipKind::external:
            return "external";
        case ProjectClipKind::midi2:
        default:
            return "midi2";
    }
}

ProjectClipKind clipKindFromJson(const choc::value::ValueView& clip_json) {
    if (clip_json.hasObjectMember("kind")) {
        const auto kind = std::string(clip_json["kind"].getString());
        if (kind == "external" || kind == "audio")
            return ProjectClipKind::external;
        return ProjectClipKind::midi2;
    }

    if (clip_json.hasObjectMember("file")) {
        const auto file = std::string(clip_json["file"].getString());
        if (file.ends_with(".midi2"))
            return ProjectClipKind::midi2;
    }

    return ProjectClipKind::external;
}

std::optional<GraphAssetName> graphAssetNameFromGraphFile(std::string_view graph_file) {
    constexpr std::string_view prefix = "graphs/";
    constexpr std::string_view suffix = ".graph.json";
    if (!graph_file.starts_with(prefix) || !graph_file.ends_with(suffix))
        return std::nullopt;
    const auto name = graph_file.substr(prefix.size(), graph_file.size() - prefix.size() - suffix.size());
    if (name.empty())
        return std::nullopt;
    return GraphAssetName{std::string(name)};
}

std::filesystem::path resolveProjectFilePath(const std::filesystem::path& path,
                                             bool must_exist,
                                             std::string& error) {
    std::error_code ec;
    const bool is_directory = std::filesystem::is_directory(path, ec);
    if (!ec && !is_directory)
        return path;

    if (ec) {
        error = std::format("Failed to inspect '{}': {}.", path.string(), ec.message());
        return {};
    }

    std::vector<std::filesystem::path> candidates;
    for (const auto& entry : std::filesystem::directory_iterator(path, ec)) {
        if (ec) {
            error = std::format("Failed to inspect directory '{}': {}.", path.string(), ec.message());
            return {};
        }
        if (!entry.is_regular_file())
            continue;
        const auto filename = entry.path().filename().string();
        if (filename.ends_with(".uapmd.json")) {
            candidates.push_back(entry.path());
            continue;
        }
        if (entry.path().extension() == ".json")
            candidates.push_back(entry.path());
    }

    if (candidates.empty()) {
        if (must_exist)
            error = std::format("No project JSON file was found in directory '{}'.", path.string());
        return {};
    }

    std::ranges::sort(candidates);
    return candidates.front();
}

choc::value::Value saveProjectToJson(const Project& project) {
    auto root = choc::value::createObject("AugeneProject");
    root.addMember("format", "uapmd-project");
    root.addMember("generator", "augene2");
    root.addMember("title", project.title);

    auto tracks_array = choc::value::createEmptyArray();
    for (const auto& track : project.tracks) {
        auto track_json = choc::value::createObject("AugeneTrack");
        track_json.addMember("id", track.id);
        track_json.addMember("instrument_name", track.instrument_name);
        if (track.graph_asset_name)
            track_json.addMember("graph_file", std::format("graphs/{}.graph.json", track.graph_asset_name->value));

        auto clips_array = choc::value::createEmptyArray();
        std::size_t clip_ordinal = 0;
        std::size_t midi2_index = 0;
        for (const auto& clip : track.clips) {
            ++clip_ordinal;
            auto clip_json = choc::value::createObject("AugeneClip");
            clip_json.addMember("kind", clipKindToString(clip.kind));
            clip_json.addMember("position_dctpq", clip.position_dctpq);
            clip_json.addMember("name", clip.name);

            std::string clip_file = clip.file;
            if (clip.kind == ProjectClipKind::midi2) {
                if (clip_file.empty())
                    clip_file = std::format("clips/{}_clip_{}.midi2", track.id, ++midi2_index);
                else
                    ++midi2_index;
            }

            clip_json.addMember("id", std::format("{}_clip_{}", track.id, clip_ordinal));
            clip_json.addMember("file", clip_file);
            clips_array.addArrayElement(clip_json);
        }

        track_json.addMember("clips", clips_array);
        tracks_array.addArrayElement(track_json);
    }

    root.addMember("tracks", tracks_array);
    return root;
}

bool writeGeneratedMidi2Files(const Project& project,
                              const std::filesystem::path& project_dir,
                              std::string& error) {
    const auto clips_dir = project_dir / "clips";
    std::error_code ec;
    std::filesystem::create_directories(clips_dir, ec);
    if (ec) {
        error = std::format("Failed to create clips directory '{}': {}.", clips_dir.string(), ec.message());
        return false;
    }

    for (const auto& track : project.tracks) {
        std::size_t midi2_index = 0;
        for (const auto& clip : track.clips) {
            if (clip.kind != ProjectClipKind::midi2)
                continue;
            const auto clip_file = clip.file.empty()
                ? std::format("clips/{}_clip_{}.midi2", track.id, ++midi2_index)
                : clip.file;
            const auto clip_path = project_dir / clip_file;
            std::filesystem::create_directories(clip_path.parent_path(), ec);
            if (ec) {
                error = std::format("Failed to create clip directory '{}': {}.", clip_path.parent_path().string(), ec.message());
                return false;
            }
            if (!writeClipFile(clip, clip_path, error))
                return false;
        }
    }

    return true;
}

} // namespace

bool UapmdProjectStorage::save(const Project& project,
                               const std::filesystem::path& path,
                               std::string& error) {
    const auto project_path = resolveProjectFilePath(path, false, error);
    const auto effective_project_path = project_path.empty() ? path : project_path;
    const auto project_dir = effective_project_path.parent_path().empty()
        ? std::filesystem::path(".")
        : effective_project_path.parent_path();

    if (!writeGeneratedMidi2Files(project, project_dir, error))
        return false;

    const auto root = saveProjectToJson(project);
    std::ofstream out(effective_project_path, std::ios::binary);
    if (!out) {
        error = std::format("Failed to open project output '{}'.", effective_project_path.string());
        return false;
    }

    const auto text = choc::json::toString(root, true);
    out.write(text.data(), static_cast<std::streamsize>(text.size()));
    out << '\n';
    if (!out) {
        error = std::format("Failed while writing project output '{}'.", effective_project_path.string());
        return false;
    }
    return true;
}

std::unique_ptr<Project> UapmdProjectStorage::load(const std::filesystem::path& path,
                                                   std::string& error) {
    if (path.extension() == ".uapmdz") {
        error = std::format("Loading '{}' is not implemented yet. Extract the project directory first.", path.string());
        return {};
    }

    const auto project_path = resolveProjectFilePath(path, true, error);
    if (project_path.empty())
        return {};

    auto text = tryReadTextFile(project_path);
    if (!text) {
        error = std::format("Failed to read project input '{}'.", project_path.string());
        return {};
    }

    choc::value::Value root;
    try {
        root = choc::json::parse(*text);
    } catch (const std::exception& ex) {
        error = std::format("Failed to parse project input '{}': {}.", project_path.string(), ex.what());
        return {};
    }

    auto project = std::make_unique<Project>();
    if (root.hasObjectMember("title"))
        project->title = std::string(root["title"].getString());

    if (!root.hasObjectMember("tracks") || !root["tracks"].isArray()) {
        error = std::format("Project '{}' does not contain a valid tracks array.", project_path.string());
        return {};
    }

    const auto project_dir = project_path.parent_path().empty()
        ? std::filesystem::path(".")
        : project_path.parent_path();

    for (const auto& track_json : root["tracks"]) {
        if (!track_json.isObject())
            continue;

        ProjectTrack track;
        if (track_json.hasObjectMember("id"))
            track.id = std::string(track_json["id"].getString());
        if (track_json.hasObjectMember("instrument_name"))
            track.instrument_name = std::string(track_json["instrument_name"].getString());
        if (track_json.hasObjectMember("graph_file"))
            track.graph_asset_name = graphAssetNameFromGraphFile(track_json["graph_file"].getString());

        if (track_json.hasObjectMember("clips") && track_json["clips"].isArray()) {
            for (const auto& clip_json : track_json["clips"]) {
                if (!clip_json.isObject())
                    continue;

                ProjectClip clip;
                clip.kind = clipKindFromJson(clip_json);
                if (clip_json.hasObjectMember("position_dctpq"))
                    clip.position_dctpq = clip_json["position_dctpq"].getWithDefault<int64_t>(0);
                if (clip_json.hasObjectMember("name"))
                    clip.name = std::string(clip_json["name"].getString());
                if (clip_json.hasObjectMember("file"))
                    clip.file = std::string(clip_json["file"].getString());

                if (clip.kind == ProjectClipKind::midi2 && !clip.file.empty()) {
                    if (!readClipFile(project_dir / clip.file, clip.smf2clip, error))
                        return {};
                }

                track.clips.push_back(std::move(clip));
            }
        }

        project->tracks.push_back(std::move(track));
    }

    return project;
}

} // namespace augene2
