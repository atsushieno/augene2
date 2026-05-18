#include <cstdlib>
#include <fstream>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include <augene2/compiler.hpp>

namespace {

std::optional<std::string> readFile(const std::string& path) {
    std::ifstream stream(path);
    if (!stream)
        return std::nullopt;

    std::ostringstream buffer;
    buffer << stream.rdbuf();
    return buffer.str();
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
        if (arg == "--help") {
            std::cerr << "usage: augene2-cli [--midi1-defaults|--midi2-defaults] [--nodefault] [mml files]\n";
            return 0;
        }
        input_paths.emplace_back(arg);
    }

    if (input_paths.empty()) {
        std::cerr << "usage: augene2-cli [--midi1-defaults|--midi2-defaults] [--nodefault] [mml files]\n";
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

    auto result = augene2::compile_project(sources, options, resolver, {});
    for (const auto& diagnostic : result.diagnostics)
        printDiagnostic(diagnostic);

    if (!result.success())
        return 3;

    std::cout << "Compiled project with " << result.project.tracks.size() << " track(s)\n";
    return 0;
}
