#include <augene2/compiler.hpp>

#include <cctype>
#include <filesystem>
#include <format>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <choc/audio/choc_MIDIFile.h>

namespace {

struct ImportedClip {
    mugene2::LocatedClip data{};
    std::string name{};
};

struct ImportedTrack {
    uint32_t track_id{};
    std::string name{};
    std::string instrument_name{};
    std::optional<std::string> graph_binding_key{};
    std::string graph_binding_source{"binding key"};
    std::vector<ImportedClip> clips{};
};

struct ImportedProjectData {
    std::vector<ImportedTrack> tracks{};
    std::vector<augene2::ProjectClip> master_clips{};
    std::string title{};
    std::vector<augene2::ProjectDiagnostic> diagnostics{};
};

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

std::string decodeFakeMidiMetaText(const std::vector<umppi::Ump>& message, uint8_t expected_meta_type) {
    const auto bytes = umppi::UmpRetriever::getSysex8Data(message);
    if (bytes.size() < 8)
        return {};
    if (bytes[0] != 0 || bytes[1] != 0 || bytes[2] != 0 || bytes[3] != 0)
        return {};
    if (bytes[4] != 0xFF || bytes[5] != 0xFF || bytes[6] != 0xFF)
        return {};
    if (bytes[7] != expected_meta_type)
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

std::vector<std::string> extractMetadataTexts(const mugene2::TrackCompilationResult& track,
                                              uint8_t midi_meta_type,
                                              uint8_t flex_metadata_status,
                                              std::string_view unknown_prefix = {}) {
    std::vector<std::string> texts;

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
                    auto text = decodeFakeMidiMetaText(current_sysex8, midi_meta_type);
                    if (!text.empty())
                        texts.push_back(std::move(text));
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
                    status == flex_metadata_status) {
                    current_flex.push_back(ump);
                    if (format == 0 || format == 3) {
                        auto text = decodeFlexDataText(current_flex);
                        if (!unknown_prefix.empty())
                            text = stripKnownUnknownMetadataTag(text, unknown_prefix);
                        if (!text.empty())
                            texts.push_back(std::move(text));
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

    return texts;
}

std::vector<std::string> extractMetadataTexts(const mugene2::LocatedClip& clip,
                                              uint8_t midi_meta_type,
                                              uint8_t flex_metadata_status,
                                              std::string_view unknown_prefix = {}) {
    mugene2::TrackCompilationResult track;
    track.clips.push_back(clip);
    return extractMetadataTexts(track, midi_meta_type, flex_metadata_status, unknown_prefix);
}

std::vector<std::string> extractInstrumentNames(const mugene2::TrackCompilationResult& track) {
    return extractMetadataTexts(
        track,
        umppi::MidiMetaType::INSTRUMENT_NAME,
        umppi::MetadataTextStatus::UNKNOWN,
        "InstrumentName:");
}

std::vector<std::string> extractTrackNames(const mugene2::TrackCompilationResult& track) {
    return extractMetadataTexts(
        track,
        umppi::MidiMetaType::TRACK_NAME,
        umppi::MetadataTextStatus::MIDI_CLIP_NAME);
}

std::vector<std::string> extractTrackNames(const mugene2::LocatedClip& clip) {
    return extractMetadataTexts(
        clip,
        umppi::MidiMetaType::TRACK_NAME,
        umppi::MetadataTextStatus::MIDI_CLIP_NAME);
}

std::vector<std::string> extractProgramKeys(const mugene2::TrackCompilationResult& track) {
    std::vector<std::string> keys;
    for (const auto& clip : track.clips) {
        bool expect_delta = true;
        for (const auto& ump : clip.smf2clip) {
            if (expect_delta) {
                expect_delta = !ump.isDeltaClockstamp();
                continue;
            }

            if (ump.isEndOfClip())
                break;

            if (ump.getMessageType() == umppi::MessageType::MIDI1 &&
                ump.getStatusCode() == umppi::MidiChannelStatus::PROGRAM) {
                keys.push_back(std::format("ch{}-p{}", ump.getChannelInGroup() + 1, ump.getMidi1Program()));
            }
            expect_delta = true;
        }
    }
    return keys;
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

void appendClipMessage(std::vector<umppi::Ump>& destination,
                       uint32_t event_delta,
                       const std::vector<umppi::Ump>& umps) {
    bool first_packet = true;
    for (const auto& ump : umps) {
        destination.push_back(umppi::Ump(umppi::UmpFactory::deltaClockstamp(first_packet ? event_delta : 0)));
        destination.push_back(ump);
        first_packet = false;
    }
}

bool hasChannelMessages(std::span<const ImportedClip> clips) {
    for (const auto& clip : clips)
        for (const auto& ump : clip.data.smf2clip)
            if (ump.getMessageType() == umppi::MessageType::MIDI1 ||
                ump.getMessageType() == umppi::MessageType::MIDI2)
                return true;
    return false;
}

void appendTrackMessage(std::vector<umppi::Ump>& destination,
                        uint32_t event_delta,
                        const choc::midi::LongMessage& message) {
    if (message.isSysex()) {
        std::vector<uint8_t> sysex7(message.data() + 1, message.data() + message.length());
        appendClipMessage(destination, event_delta, umppi::UmpFactory::sysex7(0, sysex7));
        return;
    }

    if (message.isShortMessage()) {
        if (message.data()[0] >= 0xF0) {
            destination.push_back(umppi::Ump(umppi::UmpFactory::deltaClockstamp(event_delta)));
            destination.emplace_back(umppi::UmpFactory::systemMessage(
                0,
                message.data()[0],
                message.length() > 1 ? message.data()[1] : 0,
                message.length() > 2 ? message.data()[2] : 0));
        } else {
            destination.push_back(umppi::Ump(umppi::UmpFactory::deltaClockstamp(event_delta)));
            destination.emplace_back(umppi::UmpFactory::midi1Message(
                0,
                static_cast<uint8_t>(message.data()[0] & 0xF0),
                static_cast<uint8_t>(message.data()[0] & 0x0F),
                message.length() > 1 ? message.data()[1] : 0,
                message.length() > 2 ? message.data()[2] : 0));
        }
    }
}

std::optional<umppi::Ump> tryConvertMasterMetaEvent(const choc::midi::LongMessage& message) {
    if (message.isMetaEventOfType(0x51)) {
        const auto data = message.getMetaEventData();
        if (data.length() != 3)
            return std::nullopt;

        uint32_t tempo_microseconds = static_cast<uint8_t>(data[0]);
        tempo_microseconds = (tempo_microseconds << 8) | static_cast<uint8_t>(data[1]);
        tempo_microseconds = (tempo_microseconds << 8) | static_cast<uint8_t>(data[2]);
        return umppi::UmpFactory::tempo(0, 0, tempo_microseconds * 100);
    }

    if (message.isMetaEventOfType(0x58)) {
        const auto data = message.getMetaEventData();
        if (data.length() < 4)
            return std::nullopt;
        return umppi::UmpFactory::timeSignatureDirect(
            0,
            0,
            static_cast<uint8_t>(data[0]),
            static_cast<uint8_t>(data[1]),
            static_cast<uint8_t>(data[3]));
    }

    return std::nullopt;
}

std::optional<std::string> selectUniqueValue(std::span<const std::string> values,
                                             std::string_view label,
                                             std::string_view track_id,
                                             std::vector<augene2::ProjectDiagnostic>& diagnostics) {
    if (values.empty())
        return std::nullopt;

    const auto selected = values.back();
    std::set<std::string> distinct(values.begin(), values.end());
    if (distinct.size() > 1) {
        diagnostics.push_back(augene2::ProjectDiagnostic{
            .severity = augene2::ProjectDiagnosticSeverity::warning,
            .message = std::format(
                "Multiple {} values were found on {}. Using the last value '{}'.",
                label,
                track_id,
                selected),
        });
    }
    return selected;
}

void appendImportedTrack(augene2::ProjectCompilationResult& result,
                         const ImportedTrack& imported_track,
                         const augene2::ProjectCompileOptions& options,
                         const augene2::GraphAssetResolver& graph_resolver) {
    augene2::ProjectTrack track;
    track.id = std::format("track_{}", imported_track.track_id);
    track.name = imported_track.name;
    track.instrument_name = imported_track.instrument_name;

    if (imported_track.graph_binding_key) {
        std::optional<augene2::GraphAssetName> graph_asset;
        if (graph_resolver) {
            graph_asset = graph_resolver(*imported_track.graph_binding_key);
        } else if (options.use_instrument_name_as_graph_asset_name_by_default) {
            graph_asset = augene2::GraphAssetName{*imported_track.graph_binding_key};
        }

        if (graph_asset) {
            const auto sanitized_name = sanitizeGraphAssetName(graph_asset->value);
            if (sanitized_name != graph_asset->value) {
                result.diagnostics.push_back(augene2::ProjectDiagnostic{
                    .severity = augene2::ProjectDiagnosticSeverity::information,
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
            result.diagnostics.push_back(augene2::ProjectDiagnostic{
                .severity = augene2::ProjectDiagnosticSeverity::warning,
                .message = std::format(
                    "No graph asset was resolved for {} '{}' on {}.",
                    imported_track.graph_binding_source,
                    *imported_track.graph_binding_key,
                    track.id),
                });
        }
    } else if (options.keep_tracks_without_graph_asset) {
        result.diagnostics.push_back(augene2::ProjectDiagnostic{
            .severity = augene2::ProjectDiagnosticSeverity::warning,
            .message = std::format(
                "No graph binding key could be derived for {}.",
                track.id),
        });
    }

    track.clips.reserve(imported_track.clips.size());
    for (const auto& compiled_clip : imported_track.clips) {
        augene2::ProjectClip clip;
        clip.kind = augene2::ProjectClipKind::midi2;
        clip.position_dctpq = compiled_clip.data.position_dctpq;
        clip.smf2clip = compiled_clip.data.smf2clip;
        clip.name = compiled_clip.name;
        track.clips.push_back(std::move(clip));
    }

    if (!track.graph_asset_name && !options.keep_tracks_without_graph_asset) {
        result.diagnostics.push_back(augene2::ProjectDiagnostic{
            .severity = augene2::ProjectDiagnosticSeverity::information,
            .message = std::format(
                "Skipping {} because no graph asset was resolved.",
                track.id),
        });
        return;
    }

    result.project.tracks.push_back(std::move(track));
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

ImportedProjectData importSmfProject(std::string_view source_name,
                                     std::span<const uint8_t> smf_data) {
    ImportedProjectData imported;

    choc::midi::File midi_file;
    try {
        midi_file.load(smf_data.data(), smf_data.size());
    } catch (const std::exception& ex) {
        imported.diagnostics.push_back(augene2::ProjectDiagnostic{
            .severity = augene2::ProjectDiagnosticSeverity::error,
            .source_name = std::string(source_name),
            .message = std::format("Failed to parse SMF input: {}.", ex.what()),
        });
        return imported;
    }

    if (midi_file.timeFormat <= 0) {
        imported.diagnostics.push_back(augene2::ProjectDiagnostic{
            .severity = augene2::ProjectDiagnosticSeverity::error,
            .source_name = std::string(source_name),
            .message = "SMPTE-based MIDI timing is not supported yet.",
        });
        return imported;
    }

    struct MasterEvent {
        uint32_t tick{};
        umppi::Ump ump{};
    };

    std::vector<MasterEvent> master_events;
    uint32_t emitted_track_id = 1;

    for (const auto& midi_track : midi_file.tracks) {
        std::vector<std::string> track_names;
        std::vector<std::string> instrument_names;
        std::vector<std::string> program_keys;
        std::set<std::string> distinct_program_keys;

        ImportedClip clip;
        clip.data.position_dctpq = 0;
        clip.data.smf2clip.push_back(umppi::Ump(umppi::UmpFactory::deltaClockstamp(0)));
        clip.data.smf2clip.push_back(umppi::Ump(umppi::UmpFactory::dctpq(static_cast<uint16_t>(midi_file.timeFormat))));
        clip.data.smf2clip.push_back(umppi::Ump(umppi::UmpFactory::deltaClockstamp(0)));
        clip.data.smf2clip.push_back(umppi::UmpFactory::startOfClip());

        uint32_t current_tick = 0;
        bool has_track_events = false;
        bool has_channel_messages = false;

        for (const auto& event : midi_track.events) {
            const auto& message = event.message;
            if (message.isMetaEvent()) {
                const auto data = message.getMetaEventData();
                if (message.isMetaEventOfType(0x03) && !data.empty())
                    track_names.emplace_back(data);
                else if (message.isMetaEventOfType(0x04) && !data.empty())
                    instrument_names.emplace_back(data);

                if (auto master_event = tryConvertMasterMetaEvent(message)) {
                    master_events.push_back(MasterEvent{event.tickPosition, *master_event});
                }
                continue;
            }

            const auto event_delta = event.tickPosition - current_tick;
            appendTrackMessage(clip.data.smf2clip, event_delta, message);
            current_tick = event.tickPosition;
            has_channel_messages = has_channel_messages || (message.isShortMessage() && message.data()[0] < 0xF0);

            if (message.isShortMessage() && message.isProgramChange()) {
                const auto key = std::format("ch{}-p{}", message.getChannel1to16(), message.getProgramChangeNumber());
                program_keys.push_back(key);
                distinct_program_keys.insert(key);
            }

            has_track_events = true;
        }

        if (!has_track_events) {
            if (imported.title.empty() && !track_names.empty())
                imported.title = track_names.front();
            continue;
        }

        if (!has_channel_messages) {
            if (imported.title.empty() && !track_names.empty())
                imported.title = track_names.front();
            continue;
        }

        clip.data.smf2clip.push_back(umppi::Ump(umppi::UmpFactory::deltaClockstamp(0)));
        clip.data.smf2clip.push_back(umppi::UmpFactory::endOfClip());

        if (auto clip_name = selectUniqueValue(
                extractTrackNames(clip.data), "clip-name meta", std::format("track_{}", emitted_track_id), imported.diagnostics)) {
            clip.name = *clip_name;
        } else if (!track_names.empty()) {
            clip.name = track_names.back();
        }

        ImportedTrack imported_track;
        imported_track.track_id = emitted_track_id++;
        imported_track.clips.push_back(std::move(clip));

        const auto track_id = std::format("track_{}", imported_track.track_id);
        if (auto track_name = selectUniqueValue(track_names, "track-name meta", track_id, imported.diagnostics))
            imported_track.name = *track_name;
        if (auto instrument_name = selectUniqueValue(instrument_names, "instrument-name meta", track_id, imported.diagnostics))
            imported_track.instrument_name = *instrument_name;

        if (!imported_track.instrument_name.empty()) {
            imported_track.graph_binding_key = imported_track.instrument_name;
            imported_track.graph_binding_source = "instrument-name meta";
        } else if (!imported_track.name.empty()) {
            imported_track.graph_binding_key = imported_track.name;
            imported_track.graph_binding_source = "track-name meta";
        } else if (!program_keys.empty()) {
            imported_track.graph_binding_key = program_keys.back();
            imported_track.graph_binding_source = "program key";
            if (distinct_program_keys.size() > 1) {
                imported.diagnostics.push_back(augene2::ProjectDiagnostic{
                    .severity = augene2::ProjectDiagnosticSeverity::warning,
                    .message = std::format(
                        "Multiple program keys were found on {}. Using the last value '{}'.",
                        track_id,
                        *imported_track.graph_binding_key),
                });
            }
        }

        imported.tracks.push_back(std::move(imported_track));
    }

    if (!master_events.empty()) {
        std::stable_sort(master_events.begin(), master_events.end(), [](const MasterEvent& left, const MasterEvent& right) {
            return left.tick < right.tick;
        });

        augene2::ProjectClip master_clip;
        master_clip.kind = augene2::ProjectClipKind::midi2;
        master_clip.position_dctpq = 0;
        master_clip.smf2clip.push_back(umppi::Ump(umppi::UmpFactory::deltaClockstamp(0)));
        master_clip.smf2clip.push_back(umppi::Ump(umppi::UmpFactory::dctpq(static_cast<uint16_t>(midi_file.timeFormat))));
        master_clip.smf2clip.push_back(umppi::Ump(umppi::UmpFactory::deltaClockstamp(0)));
        master_clip.smf2clip.push_back(umppi::UmpFactory::startOfClip());

        uint32_t current_tick = 0;
        for (const auto& event : master_events) {
            master_clip.smf2clip.push_back(umppi::Ump(umppi::UmpFactory::deltaClockstamp(event.tick - current_tick)));
            master_clip.smf2clip.push_back(event.ump);
            current_tick = event.tick;
        }
        master_clip.smf2clip.push_back(umppi::Ump(umppi::UmpFactory::deltaClockstamp(0)));
        master_clip.smf2clip.push_back(umppi::UmpFactory::endOfClip());
        imported.master_clips.push_back(std::move(master_clip));
    }

    if (imported.title.empty())
        imported.title = std::filesystem::path(source_name).stem().string();
    return imported;
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
        const auto instrument_names = extractInstrumentNames(compiled_track);
        const auto track_names = extractTrackNames(compiled_track);
        const auto program_keys = extractProgramKeys(compiled_track);
        ImportedTrack imported_track;
        imported_track.track_id = compiled_track.track_id;
        imported_track.clips.reserve(compiled_track.clips.size());
        for (const auto& compiled_clip : compiled_track.clips) {
            ImportedClip clip;
            clip.data = compiled_clip;
            if (auto clip_name = selectUniqueValue(
                    extractTrackNames(compiled_clip), "MIDI_CLIP_NAME", std::format("track_{}", compiled_track.track_id),
                    result.diagnostics)) {
                clip.name = *clip_name;
            }
            imported_track.clips.push_back(std::move(clip));
        }

        for (const auto& compiled_clip : compiled_track.clips) {
            if (auto master_clip = extractMasterClip(compiled_clip)) {
                auto clip_key = serializeClipIdentity(*master_clip);
                if (master_clip_keys.insert(std::move(clip_key)).second)
                    result.project.master_clips.push_back(std::move(*master_clip));
            }
        }

        if (!hasChannelMessages(imported_track.clips))
            continue;

        const auto track_id = std::format("track_{}", compiled_track.track_id);
        if (auto instrument_name = selectUniqueValue(instrument_names, "INSTRUMENTNAME", track_id, result.diagnostics)) {
            imported_track.instrument_name = *instrument_name;
            imported_track.graph_binding_key = *instrument_name;
            imported_track.graph_binding_source = "INSTRUMENTNAME";
        } else if (auto track_name = selectUniqueValue(track_names, "TRACKNAME", track_id, result.diagnostics)) {
            imported_track.name = *track_name;
            imported_track.graph_binding_key = *track_name;
            imported_track.graph_binding_source = "TRACKNAME";
        } else if (auto program_key = selectUniqueValue(program_keys, "program key", track_id, result.diagnostics)) {
            imported_track.graph_binding_key = *program_key;
            imported_track.graph_binding_source = "program key";
        }

        if (!imported_track.name.empty()) {
            for (auto& clip : imported_track.clips) {
                if (clip.name.empty())
                    clip.name = imported_track.name;
            }
        }

        appendImportedTrack(result, imported_track, options, graph_resolver);
    }

    return result;
}

ProjectCompilationResult compile_project_from_smf(std::string_view source_name,
                                                  std::span<const uint8_t> smf_data,
                                                  const ProjectCompileOptions& options,
                                                  GraphAssetResolver graph_resolver) {
    auto imported = importSmfProject(source_name, smf_data);

    ProjectCompilationResult result;
    result.diagnostics = std::move(imported.diagnostics);
    if (!result.success())
        return result;

    result.project.title = std::move(imported.title);
    result.project.master_clips = std::move(imported.master_clips);
    result.project.tracks.reserve(imported.tracks.size());

    ProjectCompileOptions effective_options = options;
    effective_options.keep_tracks_without_graph_asset = true;
    for (const auto& imported_track : imported.tracks)
        appendImportedTrack(result, imported_track, effective_options, graph_resolver);

    return result;
}

} // namespace augene2
