#pragma once

#include <QString>

namespace miacode::runtime_paths {

// Better Miacode only runs with a dedicated workspace or package runtime root.
// Initialize this before startup diagnostics or any preference access.
bool initializeFromEnvironment(QString* error = nullptr);
// macOS marked bundles may bootstrap their own user-owned Better Miacode runtime.
// Other platforms retain the explicit environment/root validation above.
bool initializePackagedRuntime(QString* error = nullptr);
// Applies the same isolated identity, settings, log and temporary paths to GUI,
// disk-only CLI and worker processes after root validation.
bool configureProcessEnvironment(QString* error = nullptr);

QString rootDirectory();
QString workspaceDirectory();
QString configDirectory();
QString configFile(const char* fileName);
QString cacheDirectory();
QString dataDirectory();
QString logDirectory();
QString tempDirectory();
QString temporaryFile(const QString& fileName);
QString exportsDirectory();
QString projectDirectoryForChart(const QString& chartFilePath);

}  // namespace miacode::runtime_paths
