#include <augene2/compiler.hpp>

#include <cctype>
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

std::string sanitizeGraphAssetName(std::string_view raw_name) {
    std::string sanitized;
    sanitized.reserve(raw_name.size());

    bool last_was_separator = false;
    for (unsigned char ch : raw_name) {
        if (std::isalnum(ch) || ch == '_' || ch == '-' || ch == '.') {
            sanitized.push_back(static_cast<char>(ch));
            last_was_separator = false;
        } else {
            if (!last_was_separator) {
                sanitized.push_back('_');
                last_was_separator = true;
            }
        }
    }

    while (!sanitized.empty() && (sanitized.front() == '.' || sanitized.front() == '_'))
        sanitized.erase(sanitized.begin());
    while (!sanitized.empty() && sanitized.back() == '_')
        sanitized.pop_back();

    if (sanitized.empty())
        sanitized = "graph";
    return sanitized;
}

std::optional<augene2::ProjectClip> extractMasterClip(const mugene2::LocatedClip& source_clip) {
    if (source_clip.smf2clip.size() < 4)
        return std::nullopt;

    augene2::ProjectClip master_clip;
    master_clip.kind = augene2::ProjectClipKind::midi2;
    master_clip.position_dctpq = source_clip.position_dctpq;
    master_clip.smf2clip.reserve(source_clip.smf2clip.size());
    master_clip.smf2clip.push_back(source_clip.smf2clip[0]);
    master_clip.smf2clip.push_back(source_clip.smf2clip[1]);
    master_clip.smf2clip.push_back(source_clip.smf2clip[2]);
    master_clip.smf2clip.push_back(source_clip.smf2clip[3]);

    bool expect_delta = true;
    uint32_t pending_delta = 0;
    bool has_master_events = false;

    auto decodeMasterEvent = [](const umppi::Ump& ump) -> std::optional<umppi::Ump> {
        if (ump.getMessageType() != umppi::MessageType::FLEX_DATA)
            return std::nullopt;

        const auto address = static_cast<uint8_t>((ump.getStatusByte() >> 4) & 0xF);
        const auto channel = ump.getChannelInGroup();
        const auto status_bank = static_cast<uint8_t>((ump.int1 >> 8) & 0xFF);
        const auto status = static_cast<uint8_t>(ump.int1 & 0xFF);

        if (address != umppi::FlexDataAddress::GROUP ||
            status_bank != umppi::FlexDataStatusBank::SETUP_AND_PERFORMANCE) {
            return std::nullopt;
        }

        if (status == umppi::FlexDataStatus::TEMPO) {
            return umppi::UmpFactory::tempo(ump.getGroup(), channel, ump.int2);
        }

        if (status == umppi::FlexDataStatus::TIME_SIGNATURE) {
            const auto numerator = static_cast<uint8_t>((ump.int2 >> 24) & 0xFF);
            const auto raw_denominator = static_cast<uint8_t>((ump.int2 >> 16) & 0xFF);
            const auto number_of_32_notes = static_cast<uint8_t>((ump.int2 >> 8) & 0xFF);
            return umppi::UmpFactory::timeSignatureDirect(
                ump.getGroup(), channel, numerator, raw_denominator, number_of_32_notes);
        }

        return std::nullopt;
    };

    for (std::size_t index = 4; index < source_clip.smf2clip.size(); ++index) {
        const auto& ump = source_clip.smf2clip[index];
        if (expect_delta) {
            if (!ump.isDeltaClockstamp())
                continue;
            pending_delta = ump.getDeltaClockstamp();
            expect_delta = false;
            continue;
        }

        if (ump.isEndOfClip())
            break;

        if (auto master_event = decodeMasterEvent(ump)) {
            master_clip.smf2clip.push_back(umppi::Ump(umppi::UmpFactory::deltaClockstamp(pending_delta)));
            master_clip.smf2clip.push_back(*master_event);
            has_master_events = true;
        }

        expect_delta = true;
    }

    if (!has_master_events)
        return std::nullopt;

    master_clip.smf2clip.push_back(umppi::Ump(umppi::UmpFactory::deltaClockstamp(0)));
    master_clip.smf2clip.push_back(umppi::UmpFactory::endOfClip());
    return master_clip;
}

std::string serializeClipIdentity(const augene2::ProjectClip& clip) {
    std::string key = std::to_string(clip.position_dctpq);
    key.push_back('|');
    for (const auto& ump : clip.smf2clip) {
        const auto bytes = ump.toBytes();
        key.append(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    }
    return key;
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

    std::set<std::string> master_clip_keys;
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
                const auto sanitized_name = sanitizeGraphAssetName(graph_asset->value);
                if (sanitized_name != graph_asset->value) {
                    result.diagnostics.push_back(ProjectDiagnostic{
                        .severity = ProjectDiagnosticSeverity::information,
                        .message = std::format(
                            "Graph asset name '{}' on {} was sanitized to '{}'.",
                            graph_asset->value,
                            track.id,
                            sanitized_name),
                    });
                }
                graph_asset->value = sanitized_name;
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
            if (auto master_clip = extractMasterClip(compiled_clip)) {
                auto clip_key = serializeClipIdentity(*master_clip);
                if (master_clip_keys.insert(std::move(clip_key)).second)
                    result.project.master_clips.push_back(std::move(*master_clip));
            }

            ProjectClip clip;
            clip.kind = ProjectClipKind::midi2;
            clip.position_dctpq = compiled_clip.position_dctpq;
            clip.smf2clip = compiled_clip.smf2clip;
            track.clips.push_back(std::move(clip));
        }

        if (!track.graph_asset_name) {
            result.diagnostics.push_back(ProjectDiagnostic{
                .severity = ProjectDiagnosticSeverity::information,
                .message = std::format(
                    "Skipping {} because no graph asset was resolved.",
                    track.id),
            });
            continue;
        }

        result.project.tracks.push_back(std::move(track));
    }

    return result;
}

} // namespace augene2
