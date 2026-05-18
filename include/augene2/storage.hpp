#pragma once

#include <memory>
#include <string>
#include <filesystem>

#include "project.hpp"

namespace augene2 {

class ProjectStorage {
public:
    virtual ~ProjectStorage() = default;

    virtual bool save(const Project& project,
                      const std::filesystem::path& path,
                      std::string& error) = 0;
    virtual std::unique_ptr<Project> load(const std::filesystem::path& path,
                                          std::string& error) = 0;
};

class UapmdProjectStorage : public ProjectStorage {
public:
    bool save(const Project& project,
              const std::filesystem::path& path,
              std::string& error) override;
    std::unique_ptr<Project> load(const std::filesystem::path& path,
                                  std::string& error) override;
};

} // namespace augene2
