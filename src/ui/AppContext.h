#pragma once

namespace nixm {

class ConfigProject;
class CommandRunner;
class PackageSearch;

/// The three long-lived services every page needs. Owned by MainWindow.
struct AppContext {
    ConfigProject *project = nullptr;
    CommandRunner *runner = nullptr;
    PackageSearch *search = nullptr;
};

} // namespace nixm
