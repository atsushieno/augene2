#pragma once

#include <algorithm>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <umppi/umppi.hpp>

namespace augene2 {

enum class ProjectDiagnosticSeverity {
    error,
    warning,
    information,
};

struct ProjectDiagnostic {
    ProjectDiagnosticSeverity severity{ProjectDiagnosticSeverity::error};
    std::string source_name{};
    int line{0};
    int column{0};
    std::string message{};
};

struct GraphAssetName {
    std::string value{};

    [[nodiscard]] bool empty() const noexcept { return value.empty(); }
};

enum class ProjectClipKind {
    midi2,
    external,
};

struct ProjectClip {
    ProjectClipKind kind{ProjectClipKind::midi2};
    uint64_t position_dctpq{};
    std::vector<umppi::Ump> smf2clip{};
    std::string name{};
    std::string file{};
};

struct ProjectTrack {
    std::string id{};
    std::string instrument_name{};
    std::optional<GraphAssetName> graph_asset_name{};
    std::vector<ProjectClip> clips{};
};

struct Project {
    std::string title{};
    std::vector<ProjectTrack> tracks{};
};

using GraphAssetResolver = std::function<std::optional<GraphAssetName>(std::string_view instrument_name)>;

struct ProjectCompilationResult {
    std::vector<ProjectDiagnostic> diagnostics{};
    Project project{};

    [[nodiscard]] bool success() const {
        return std::none_of(diagnostics.begin(), diagnostics.end(), [](const ProjectDiagnostic& diagnostic) {
            return diagnostic.severity == ProjectDiagnosticSeverity::error;
        });
    }
};

} // namespace augene2
