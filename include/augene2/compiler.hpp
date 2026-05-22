#pragma once

#include <cstdint>
#include <span>
#include <string_view>

#include <mugene2/mugene2.hpp>

#include "project.hpp"

namespace augene2 {

struct ProjectCompileOptions {
    mugene2::CompileOptions mml_options{};
    bool use_instrument_name_as_graph_asset_name_by_default{true};
    bool keep_tracks_without_graph_asset{false};
};

ProjectCompilationResult compile_project(std::span<const mugene2::SourceText> sources,
                                         const ProjectCompileOptions& options = {},
                                         mugene2::IncludeResolver resolver = {},
                                         GraphAssetResolver graph_resolver = {});

ProjectCompilationResult compile_project_from_smf(std::string_view source_name,
                                                  std::span<const uint8_t> smf_data,
                                                  const ProjectCompileOptions& options = {},
                                                  GraphAssetResolver graph_resolver = {});

} // namespace augene2
