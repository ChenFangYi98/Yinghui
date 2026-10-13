#pragma once
#include "AIProvider.h"
namespace comp {
class OwnedComfy {
  public:
    static QString root();
    static bool available(const QString& model = "qwen-image-21");
    static QString python();
    static void verifyPack(AICancel canceled = {}, AIText progress = {});
    static QUrl acquire(AICancel canceled = {}, AIText progress = {});
    static QString token();
    static void interrupt();
    static void releaseMemory();
};
} // namespace comp
