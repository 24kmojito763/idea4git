#pragma once

#include <QDir>
#include <QString>

namespace idea4git {

// Git porcelain paths are relative to the repository, even if a name begins with
// ':' (which QDir::filePath otherwise interprets as an absolute Qt resource path).
inline QString repositoryFilePath(const QString& root, const QString& relative) {
    return QDir::cleanPath(root + '/' + relative);
}

} // namespace idea4git
