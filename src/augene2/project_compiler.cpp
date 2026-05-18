#include <augene2/compiler.hpp>

#include <format>
#include <set>
#include <string>
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

std::string decodeFakeMidiMetaText(const std::vector<umppi::Ump>& message) {
    const auto bytes = umppi::UmpRetriever::getSysex8Data(message);
    if (bytes.size() < 8)
        return {};
    if (bytes[0] != 0 || bytes[1] != 0 || bytes[2] != 0 || bytes[3] != 0)
        return {};
    if (bytes[4] != 0xFF || bytes[5] != 0xFF || bytes[6] != 0xFF)
        return {};
    if (bytes[7] != umppi::MidiMetaType::INSTRUMENT_NAME)
        return {};

    return std::string(bytes.begin() + 8, bytes.end());
}

std::string decodeFlexDataText(const std::vector<umppi::Ump>& message) {
    std::vector<uint8_t> bytes;
    bytes.reserve(message.size() * 12);

    for (const auto& ump : message) {
        for (const uint32_t word : {ump.int2, ump.int3, ump.int4}) {
            bytes.push_back(static_cast<uint8_t>((word >> 24) & 0xFF));
            bytes.push_back(static_cast<uint8_t>((word >> 16) & 0xFF));
            bytes.push_back(static_cast<uint8_t>((word >> 8) & 0xFF));
            bytes.push_back(static_cast<uint8_t>(word & 0xFF));
        }
    }

    while (!bytes.empty() && bytes.back() == 0)
        bytes.pop_back();
    return std::string(bytes.begin(), bytes.end());
}

std::string stripKnownUnknownMetadataTag(std::string_view text, std::string_view prefix) {
    if (!text.starts_with(prefix))
        return {};
    auto value = text.substr(prefix.size());
    while (!value.empty() && value.front() == ' ')
        value.remove_prefix(1);
    return std::string(value);
}

std::vector<std::string> extractInstrumentNames(const mugene2::TrackCompilationResult& track) {
    std::vector<std::string> names;

    for (const auto& clip : track.clips) {
        std::vector<umppi::Ump> current_sysex8;
        std::vector<umppi::Ump> current_flex;
        for (const auto& ump : clip.smf2clip) {
            if (ump.isStartOfClip() || ump.isEndOfClip() || ump.isDeltaClockstamp() || ump.isDCTPQ())
                continue;

            if (ump.getMessageType() == umppi::MessageType::SYSEX8_MDS) {
                current_sysex8.push_back(ump);
                const auto chunk_status = ump.getBinaryChunkStatus();
                if (chunk_status == umppi::BinaryChunkStatus::COMPLETE_PACKET ||
                    chunk_status == umppi::BinaryChunkStatus::END) {
                    auto text = decodeFakeMidiMetaText(current_sysex8);
                    if (!text.empty())
                        names.push_back(std::move(text));
                    current_sysex8.clear();
                }
                current_flex.clear();
                continue;
            }

            if (ump.getMessageType() == umppi::MessageType::FLEX_DATA) {
                const auto status_bank = static_cast<uint8_t>((ump.int1 >> 8) & 0xFF);
                const auto status = static_cast<uint8_t>(ump.int1 & 0xFF);
                const auto format = static_cast<uint8_t>((ump.int1 >> 22) & 0x3);
                if (status_bank == umppi::FlexDataStatusBank::METADATA_TEXT &&
                    status == umppi::MetadataTextStatus::UNKNOWN) {
                    current_flex.push_back(ump);
                    if (format == 0 || format == 3) {
                        auto text = decodeFlexDataText(current_flex);
                        auto instrument_name = stripKnownUnknownMetadataTag(text, "InstrumentName:");
                        if (!instrument_name.empty())
                            names.push_back(std::move(instrument_name));
                        current_flex.clear();
                    }
                } else {
                    current_flex.clear();
                }
                current_sysex8.clear();
                continue;
            }

            current_sysex8.clear();
            current_flex.clear();
        }
    }

    return names;
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
        const auto instrument_names = extractInstrumentNames(compiled_track);
        if (!instrument_names.empty()) {
            track.instrument_name = instrument_names.back();

            std::set<std::string> distinct_names(instrument_names.begin(), instrument_names.end());
            if (distinct_names.size() > 1) {
                result.diagnostics.push_back(ProjectDiagnostic{
                    .severity = ProjectDiagnosticSeverity::warning,
                    .message = std::format(
                        "Multiple INSTRUMENTNAME values were found on {}. Using the last value '{}'.",
                        track.id,
                        track.instrument_name),
                });
            }
        }

        if (!track.instrument_name.empty()) {
            std::optional<GraphAssetName> graph_asset;
            if (graph_resolver) {
                graph_asset = graph_resolver(track.instrument_name);
            } else if (options.use_instrument_name_as_graph_asset_name_by_default) {
                graph_asset = GraphAssetName{track.instrument_name};
            }

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
            clip.kind = ProjectClipKind::midi2;
            clip.position_dctpq = compiled_clip.position_dctpq;
            clip.smf2clip = compiled_clip.smf2clip;
            track.clips.push_back(std::move(clip));
        }

        result.project.tracks.push_back(std::move(track));
    }

    return result;
}

} // namespace augene2
