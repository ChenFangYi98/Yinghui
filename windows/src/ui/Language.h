#pragma once
#include <QString>
#include <QCoreApplication>
namespace comp {
inline QString text(const char* chinese, const char* english) {
    return QCoreApplication::instance()->property("english").toBool() ? QString::fromUtf8(english)
                                                                      : QString::fromUtf8(chinese);
}
} // namespace comp
