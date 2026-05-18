#include <cstdlib>
#include <fstream>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include <mugene2/mugene2.hpp>

namespace {

std::optional<std::string> readFile(const std::string& path) {
    std::ifstream stream(path);
    if (!stream)
        return std::nullopt;

    std::ostringstream buffer;
    buffer << stream.rdbuf();
    return buffer.str();
}

void printDiagnostic(const mugene2::Diagnostic& diagnostic) {
    const char* severity = "error";
    switch (diagnostic.severity) {
        case mugene2::DiagnosticSeverity::warning: severity = "warning"; break;
        case mugene2::DiagnosticSeverity::information: severity = "information"; break;
        case mugene2::DiagnosticSeverity::error: break;
    }

    if (!diagnostic.source_name.empty())
        std::cerr << diagnostic.source_name;
    else
        std::cerr << "<input>";

    std::cerr << " (" << diagnostic.line << ", " << diagnostic.column << ") : "
              << severity << ": " << diagnostic.message << '\n';
}

std::string replaceExtension(const std::string& path, const std::string& extension) {
    const auto last_dot = path.find_last_of('.');
    if (last_dot == std::string::npos)
        return path + extension;
    return path.substr(0, last_dot) + extension;
}

} // namespace

int main(int argc, char** argv) {
    mugene2::CompileOptions options;
    options.default_mml_profile = mugene2::DefaultMmlProfile::midi1;
    bool midi2 = false;
    std::optional<std::string> output_path;
    std::vector<std::string> input_paths;
    input_paths.reserve(static_cast<std::size_t>(argc > 1 ? argc - 1 : 0));

    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        if (arg == "--nodefault" || arg == "--skip-default-mml-files") {
            options.skip_default_mml_files = true;
            continue;
        }
        if (arg == "--midi2" || arg == "--midi2x" || arg == "--midi2-defaults") {
            midi2 = true;
            options.default_mml_profile = mugene2::DefaultMmlProfile::midi2;
            continue;
        }
        if (arg == "--midi1-defaults") {
            midi2 = false;
            options.default_mml_profile = mugene2::DefaultMmlProfile::midi1;
            continue;
        }
        if (arg == "--smf-out") {
            if (i + 1 >= argc) {
                std::cerr << "--smf-out requires a path\n";
                return 1;
            }
            output_path = argv[++i];
            continue;
        }
        if (arg.starts_with("--output:")) {
            output_path = std::string(arg.substr(9));
            continue;
        }
        if (arg == "--help") {
            std::cerr << "usage: mugene2-cli [--midi2|--midi2x] [--nodefault] [--output:path] [mml files]\n";
            return 0;
        }
        if (arg == "--verbose" || arg == "--disable-running-status" || arg.starts_with("--encoding:")) {
            continue;
        }
        input_paths.emplace_back(arg);
    }

    if (input_paths.empty()) {
        std::cerr << "usage: mugene2-cli [--midi2|--midi2x] [--nodefault] [--output:path] [mml files]\n";
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

    if (!midi2) {
        auto result = mugene2::compile_to_smf(sources, options, resolver);
        for (const auto& diagnostic : result.diagnostics)
            printDiagnostic(diagnostic);

        if (!result.success())
            return 3;

        const auto resolved_output_path = output_path.value_or(replaceExtension(input_paths.back(), ".mid"));
        std::ofstream out(resolved_output_path, std::ios::binary);
        if (!out) {
            std::cerr << "failed to open output: " << resolved_output_path << '\n';
            return 4;
        }
        out.write(reinterpret_cast<const char*>(result.smf.data()), static_cast<std::streamsize>(result.smf.size()));
        std::cout << "Written SMF file ... " << resolved_output_path << '\n';
        return 0;
    }

    std::cerr << "MIDI 2 CLI file output is not implemented yet.\n";
    return 5;
}
