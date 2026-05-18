#pragma once

#include <mugene2/mugene2.hpp>

namespace augene2 {

using DiagnosticSeverity = mugene2::DiagnosticSeverity;
using Diagnostic = mugene2::Diagnostic;
using LocatedClip = mugene2::LocatedClip;
using TrackCompilationResult = mugene2::TrackCompilationResult;
using CompilationResult = mugene2::CompilationResult;
using SmfCompilationResult = mugene2::SmfCompilationResult;
using SourceText = mugene2::SourceText;
using DefaultMmlProfile = mugene2::DefaultMmlProfile;
using CompileOptions = mugene2::CompileOptions;
using IncludeResolver = mugene2::IncludeResolver;

inline CompilationResult compile_to_smf2clips(std::span<const SourceText> sources,
                                              const CompileOptions& options,
                                              IncludeResolver resolver = {}) {
    return mugene2::compile_to_smf2clips(sources, options, std::move(resolver));
}

inline CompilationResult compile_to_smf2clips(std::span<const SourceText> sources,
                                              IncludeResolver resolver = {}) {
    return mugene2::compile_to_smf2clips(sources, std::move(resolver));
}

inline SmfCompilationResult compile_to_smf(std::span<const SourceText> sources,
                                           const CompileOptions& options,
                                           IncludeResolver resolver = {}) {
    return mugene2::compile_to_smf(sources, options, std::move(resolver));
}

inline SmfCompilationResult compile_to_smf(std::span<const SourceText> sources,
                                           IncludeResolver resolver = {}) {
    return mugene2::compile_to_smf(sources, std::move(resolver));
}

} // namespace augene2
