#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <augene2/compiler.hpp>
#include <augene2/storage.hpp>

namespace {

std::optional<std::string> readFile(const std::string& path) {
    std::ifstream stream(path);
    if (!stream)
        return std::nullopt;

    std::ostringstream buffer;
    buffer << stream.rdbuf();
    return buffer.str();
}

std::string replaceExtension(const std::string& path, const std::string& extension) {
    const auto last_dot = path.find_last_of('.');
    if (last_dot == std::string::npos)
        return path + extension;
    return path.substr(0, last_dot) + extension;
}

void printDiagnostic(const augene2::ProjectDiagnostic& diagnostic) {
    const char* severity = "error";
    switch (diagnostic.severity) {
        case augene2::ProjectDiagnosticSeverity::warning: severity = "warning"; break;
        case augene2::ProjectDiagnosticSeverity::information: severity = "information"; break;
        case augene2::ProjectDiagnosticSeverity::error: break;
    }

    if (!diagnostic.source_name.empty())
        std::cerr << diagnostic.source_name;
    else
        std::cerr << "<input>";

    std::cerr << " (" << diagnostic.line << ", " << diagnostic.column << ") : "
              << severity << ": " << diagnostic.message << '\n';
}

} // namespace

int main(int argc, char** argv) {
    augene2::ProjectCompileOptions options;
    options.mml_options.default_mml_profile = mugene2::DefaultMmlProfile::midi2;
    std::unordered_map<std::string, std::string> graph_asset_map;
    std::optional<std::string> output_path;
    std::optional<std::string> update_midi2_path;
    std::vector<std::string> input_paths;
    input_paths.reserve(static_cast<std::size_t>(argc > 1 ? argc - 1 : 0));

    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        if (arg == "--nodefault" || arg == "--skip-default-mml-files") {
            options.mml_options.skip_default_mml_files = true;
            continue;
        }
        if (arg == "--midi1-defaults") {
            options.mml_options.default_mml_profile = mugene2::DefaultMmlProfile::midi1;
            continue;
        }
        if (arg == "--midi2-defaults" || arg == "--midi2") {
            options.mml_options.default_mml_profile = mugene2::DefaultMmlProfile::midi2;
            continue;
        }
        if (arg == "--no-default-graph-asset-name") {
            options.use_instrument_name_as_graph_asset_name_by_default = false;
            continue;
        }
        if (arg == "--graph-map") {
            if (i + 1 >= argc) {
                std::cerr << "--graph-map requires instrument=asset\n";
                return 1;
            }
            std::string mapping = argv[++i];
            auto separator = mapping.find('=');
            if (separator == std::string::npos || separator == 0 || separator + 1 >= mapping.size()) {
                std::cerr << "--graph-map requires instrument=asset\n";
                return 1;
            }
            graph_asset_map.emplace(mapping.substr(0, separator), mapping.substr(separator + 1));
            continue;
        }
        if (arg == "--output") {
            if (i + 1 >= argc) {
                std::cerr << "--output requires a path\n";
                return 1;
            }
            output_path = argv[++i];
            continue;
        }
        if (arg == "--update-midi2") {
            if (i + 1 >= argc) {
                std::cerr << "--update-midi2 requires a project path\n";
                return 1;
            }
            update_midi2_path = argv[++i];
            continue;
        }
        if (arg.starts_with("--output:")) {
            output_path = std::string(arg.substr(9));
            continue;
        }
        if (arg == "--help") {
            std::cerr << "usage: augene2-cli [--midi1-defaults|--midi2-defaults] [--nodefault] "
                         "[--no-default-graph-asset-name] [--graph-map instrument=asset] "
                         "[--output path] [--update-midi2 project] [mml files]\n";
            return 0;
        }
        input_paths.emplace_back(arg);
    }

    if (input_paths.empty()) {
        std::cerr << "usage: augene2-cli [--midi1-defaults|--midi2-defaults] [--nodefault] "
                     "[--no-default-graph-asset-name] [--graph-map instrument=asset] "
                     "[--output path] [--update-midi2 project] [mml files]\n";
        return 1;
    }

    std::vector<mugene2::SourceText> sources;
    sources.reserve(input_paths.size());
    for (const auto& path : input_paths) {
        auto text = readFile(path);
        if (!text) {
            std::cerr << "failed to read: " << path << '\n';
            return 2;
        }
        sources.push_back(mugene2::SourceText{path, *text});
    }

    auto resolver = [](std::string_view including_source,
                       std::string_view requested_path) -> std::optional<mugene2::SourceText> {
        (void) including_source;
        auto text = readFile(std::string(requested_path));
        if (!text)
            return std::nullopt;
        return mugene2::SourceText{std::string(requested_path), *text};
    };

    augene2::GraphAssetResolver graph_resolver;
    if (!graph_asset_map.empty()) {
        graph_resolver = [&graph_asset_map](std::string_view instrument_name) -> std::optional<augene2::GraphAssetName> {
            auto it = graph_asset_map.find(std::string(instrument_name));
            if (it == graph_asset_map.end())
                return std::nullopt;
            return augene2::GraphAssetName{it->second};
        };
    }

    auto result = augene2::compile_project(sources, options, resolver, std::move(graph_resolver));
    for (const auto& diagnostic : result.diagnostics)
        printDiagnostic(diagnostic);

    if (!result.success())
        return 3;

    const auto resolved_output_path = output_path.value_or(replaceExtension(input_paths.back(), ".uapmd.json"));
    augene2::UapmdProjectStorage storage;
    std::string error;

    std::string saved_path = resolved_output_path;
    if (update_midi2_path) {
        auto existing_project = storage.load(*update_midi2_path, error);
        if (!existing_project) {
            std::cerr << error << '\n';
            return 4;
        }

        const auto existing_project_path = std::filesystem::path(*update_midi2_path);
        const auto existing_project_dir = std::filesystem::is_directory(existing_project_path)
            ? existing_project_path
            : existing_project_path.parent_path();
        for (const auto& track : existing_project->tracks) {
            for (const auto& clip : track.clips) {
                if (clip.kind != augene2::ProjectClipKind::midi2 || clip.file.empty())
                    continue;
                std::error_code ec;
                std::filesystem::remove(existing_project_dir / clip.file, ec);
            }
        }

        if (result.project.title.empty())
            result.project.title = existing_project->title;
        if (result.project.title.empty())
            result.project.title = input_paths.back();

        for (auto& track : existing_project->tracks) {
            std::erase_if(track.clips, [](const augene2::ProjectClip& clip) {
                return clip.kind == augene2::ProjectClipKind::midi2;
            });
        }
        std::erase_if(existing_project->tracks, [](const augene2::ProjectTrack& track) {
            return track.clips.empty();
        });
        for (auto& track : result.project.tracks)
            existing_project->tracks.push_back(std::move(track));
        result.project = std::move(*existing_project);
        saved_path = output_path.value_or(*update_midi2_path);
    } else if (result.project.title.empty()) {
        result.project.title = input_paths.back();
    }

    if (!storage.save(result.project, saved_path, error)) {
        std::cerr << error << '\n';
        return 5;
    }

    std::cout << "Compiled project with " << result.project.tracks.size() << " track(s)\n";
    for (const auto& track : result.project.tracks) {
        std::cout << track.id;
        if (!track.instrument_name.empty())
            std::cout << " instrument=" << track.instrument_name;
        if (track.graph_asset_name)
            std::cout << " graph_asset=" << track.graph_asset_name->value;
        std::cout << " clips=" << track.clips.size() << '\n';
    }
    std::cout << "Saved project to " << saved_path << '\n';
    return 0;
}
