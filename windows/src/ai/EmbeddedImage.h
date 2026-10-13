#pragma once
#include "AIProvider.h"
namespace comp {
class EmbeddedImage {
  public:
    static QString root();
    static bool available();
    static AIProfile profile();
    static void probe();
    static QImage run(const AIImageRequest& request, AICancel canceled = {}, AIText progress = {});
};
} // namespace comp
