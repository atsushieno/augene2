#include <augene2/storage.hpp>

namespace augene2 {

bool UapmdProjectStorage::save(const Project& project,
                               const std::filesystem::path& path,
                               std::string& error) {
    (void) project;
    (void) path;
    error = "UAPMD project save is not implemented yet.";
    return false;
}

std::unique_ptr<Project> UapmdProjectStorage::load(const std::filesystem::path& path,
                                                   std::string& error) {
    (void) path;
    error = "UAPMD project load is not implemented yet.";
    return {};
}

} // namespace augene2
