#include <augene2/augene2.hpp>

#include "detail/compiler_internal.hpp"

#include <array>
#include <fstream>
#include <sstream>

namespace augene2 {

namespace {

std::optional<SourceText> loadBundledResource(const char* filename) {
    std::ifstream stream(std::string(AUGENE2_RESOURCE_DIR) + "/" + filename);
    if (!stream)
        return std::nullopt;
    std::ostringstream buffer;
    buffer << stream.rdbuf();
    return SourceText{filename, buffer.str()};
}

const std::array<const char*, 4>& bundledResourceSet(DefaultMmlProfile profile) {
    static constexpr std::array<const char*, 4> midi1_bundled = {
        "default-macro.mml",
        "drum-part.mml",
        "gs-sysex.mml",
        "nrpn-gs-xg.mml",
    };
    static constexpr std::array<const char*, 4> midi2_bundled = {
        "default-macro2.mml",
        "drum-part.mml",
        "gs-sysex.mml",
        "nrpn-gs-xg.mml",
    };
    return profile == DefaultMmlProfile::midi1 ? midi1_bundled : midi2_bundled;
}

template <typename Result, typename GenerateFn>
Result compileCommon(std::span<const SourceText> sources,
                     const CompileOptions& options,
                     IncludeResolver resolver,
                     GenerateFn&& generate) {
    Result result;
    detail::DiagnosticSink diagnostics(result.diagnostics);

    if (sources.empty()) {
        diagnostics.error(detail::LineInfo{}, "No source inputs were provided.");
        return result;
    }

    for (const auto& source : sources) {
        if (source.name.empty())
            diagnostics.error(detail::LineInfo{}, "Every input source must have a name.");
        if (source.text.empty())
            diagnostics.error(detail::LineInfo{source.name, 0, 0}, "Input source text is empty.");
    }

    if (!result.success())
        return result;

    std::vector<SourceText> all_sources;
    const auto& bundled = bundledResourceSet(options.default_mml_profile);
    all_sources.reserve((options.skip_default_mml_files ? 0U : bundled.size()) + sources.size());
    if (!options.skip_default_mml_files) {
        for (const char* filename : bundled) {
            auto resource = loadBundledResource(filename);
            if (!resource) {
                diagnostics.error(detail::LineInfo{filename, 0, 0}, "Bundled resource could not be loaded.");
                return result;
            }
            all_sources.push_back(std::move(*resource));
        }
    }
    all_sources.insert(all_sources.end(), sources.begin(), sources.end());

    detail::FrontEnd front_end(diagnostics, all_sources, std::move(resolver));
    const bool front_end_ok = front_end.process();
    auto semantic_tree = detail::buildSemanticTree(front_end, diagnostics);
    const bool semantic_ok = detail::prepareSemanticTree(semantic_tree, diagnostics);
    const bool generation_ok = semantic_ok ? generate(semantic_tree, diagnostics, result) : false;
    (void) front_end_ok;
    (void) generation_ok;
    return result;
}

} // namespace

CompilationResult compile_to_smf2clips(std::span<const SourceText> sources,
                                       const CompileOptions& options,
                                       IncludeResolver resolver) {
    return compileCommon<CompilationResult>(
        sources,
        options,
        std::move(resolver),
        [](const detail::SemanticTree& semantic_tree, detail::DiagnosticSink& diagnostics, CompilationResult& result) {
            return detail::generateSmf2Clips(semantic_tree, diagnostics, result.tracks);
        });
}

CompilationResult compile_to_smf2clips(std::span<const SourceText> sources,
                                       IncludeResolver resolver) {
    return compile_to_smf2clips(sources, CompileOptions{}, std::move(resolver));
}

SmfCompilationResult compile_to_smf(std::span<const SourceText> sources,
                                    const CompileOptions& options,
                                    IncludeResolver resolver) {
    return compileCommon<SmfCompilationResult>(
        sources,
        options,
        std::move(resolver),
        [](const detail::SemanticTree& semantic_tree, detail::DiagnosticSink& diagnostics, SmfCompilationResult& result) {
            return detail::generateSmf(semantic_tree, diagnostics, result.smf);
        });
}

SmfCompilationResult compile_to_smf(std::span<const SourceText> sources,
                                    IncludeResolver resolver) {
    CompileOptions options;
    options.default_mml_profile = DefaultMmlProfile::midi1;
    return compile_to_smf(sources, options, std::move(resolver));
}

} // namespace augene2
