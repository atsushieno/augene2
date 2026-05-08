#pragma once

#include <algorithm>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <umppi/umppi.hpp>

namespace augene2 {

enum class DiagnosticSeverity {
    error,
    warning,
    information,
};

struct Diagnostic {
    DiagnosticSeverity severity{DiagnosticSeverity::error};
    std::string source_name{};
    int line{0};
    int column{0};
    std::string message{};
};

struct LocatedClip {
    uint64_t position_dctpq{};
    std::vector<umppi::Ump> smf2clip{};
};

struct TrackCompilationResult {
    uint32_t track_id{};
    std::vector<LocatedClip> clips{};
};

struct CompilationResult {
    std::vector<Diagnostic> diagnostics{};
    std::vector<TrackCompilationResult> tracks{};

    [[nodiscard]] bool success() const {
        return std::none_of(diagnostics.begin(), diagnostics.end(), [](const Diagnostic& diagnostic) {
            return diagnostic.severity == DiagnosticSeverity::error;
        });
    }
};

struct SmfCompilationResult {
    std::vector<Diagnostic> diagnostics{};
    std::vector<uint8_t> smf{};

    [[nodiscard]] bool success() const {
        return std::none_of(diagnostics.begin(), diagnostics.end(), [](const Diagnostic& diagnostic) {
            return diagnostic.severity == DiagnosticSeverity::error;
        });
    }
};

struct SourceText {
    std::string name{};
    std::string text{};
};

enum class DefaultMmlProfile {
    midi1,
    midi2,
};

struct CompileOptions {
    bool skip_default_mml_files{false};
    DefaultMmlProfile default_mml_profile{DefaultMmlProfile::midi2};
};

using IncludeResolver = std::function<std::optional<SourceText>(
    std::string_view including_source,
    std::string_view requested_path)>;

CompilationResult compile_to_smf2clips(std::span<const SourceText> sources,
                                       const CompileOptions& options,
                                       IncludeResolver resolver = {});

CompilationResult compile_to_smf2clips(std::span<const SourceText> sources,
                                       IncludeResolver resolver = {});

SmfCompilationResult compile_to_smf(std::span<const SourceText> sources,
                                    const CompileOptions& options,
                                    IncludeResolver resolver = {});

SmfCompilationResult compile_to_smf(std::span<const SourceText> sources,
                                    IncludeResolver resolver = {});

} // namespace augene2
