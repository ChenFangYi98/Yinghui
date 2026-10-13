#include "Fonts.h"
#include <QCoreApplication>
#include <QFontDatabase>
#include <QDir>

namespace comp {
QString initializeFonts() {
    const auto path = QDir(QCoreApplication::applicationDirPath()).filePath("fonts/NotoSansSC.ttf");
    int id = QFontDatabase::addApplicationFont(path);
    if (id >= 0) {
        auto families = QFontDatabase::applicationFontFamilies(id);
        if (!families.isEmpty()) {
            const auto family = families.first();
            for (auto script :
                 {QChar::Script_Han, QChar::Script_Hiragana, QChar::Script_Katakana, QChar::Script_Hangul})
                QFontDatabase::addApplicationFallbackFontFamily(script, family);
            return family;
        }
    }
    return "Microsoft YaHei";
}
} // namespace comp
