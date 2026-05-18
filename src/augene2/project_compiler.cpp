#include <augene2/compiler.hpp>

#include <format>
#include <utility>

namespace {

augene2::ProjectDiagnosticSeverity mapSeverity(mugene2::DiagnosticSeverity severity) {
    switch (severity) {
        case mugene2::DiagnosticSeverity::warning:
            return augene2::ProjectDiagnosticSeverity::warning;
        case mugene2::DiagnosticSeverity::information:
            return augene2::ProjectDiagnosticSeverity::information;
        case mugene2::DiagnosticSeverity::error:
        default:
            return augene2::ProjectDiagnosticSeverity::error;
    }
}

} // namespace

namespace augene2 {

ProjectCompilationResult compile_project(std::span<const mugene2::SourceText> sources,
                                         const ProjectCompileOptions& options,
                                         mugene2::IncludeResolver resolver,
                                         GraphAssetResolver graph_resolver) {
    auto mml_result = mugene2::compile_to_smf2clips(sources, options.mml_options, std::move(resolver));

    ProjectCompilationResult result;
    result.diagnostics.reserve(mml_result.diagnostics.size());
    for (const auto& diagnostic : mml_result.diagnostics) {
        result.diagnostics.push_back(ProjectDiagnostic{
            .severity = mapSeverity(diagnostic.severity),
            .source_name = diagnostic.source_name,
            .line = diagnostic.line,
            .column = diagnostic.column,
            .message = diagnostic.message,
        });
    }

    if (!mml_result.success())
        return result;

    result.project.tracks.reserve(mml_result.tracks.size());
    for (const auto& compiled_track : mml_result.tracks) {
        ProjectTrack track;
        track.id = std::format("track_{}", compiled_track.track_id);

        if (graph_resolver && !track.instrument_name.empty()) {
            auto graph_asset = graph_resolver(track.instrument_name);
            if (graph_asset) {
                track.graph_asset_name = std::move(*graph_asset);
            } else {
                result.diagnostics.push_back(ProjectDiagnostic{
                    .severity = ProjectDiagnosticSeverity::warning,
                    .message = std::format(
                        "No graph asset was resolved for INSTRUMENTNAME '{}' on {}.",
                        track.instrument_name,
                        track.id),
                });
            }
        }

        track.clips.reserve(compiled_track.clips.size());
        for (const auto& compiled_clip : compiled_track.clips) {
            ProjectClip clip;
            clip.position_dctpq = compiled_clip.position_dctpq;
            clip.smf2clip = compiled_clip.smf2clip;
            track.clips.push_back(std::move(clip));
        }

        result.project.tracks.push_back(std::move(track));
    }

    return result;
}

} // namespace augene2
