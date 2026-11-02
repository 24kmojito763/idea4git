#pragma once

#include <QHash>
#include <QTranslator>

namespace idea4git {

// Only the built-in Chinese UI needs these standard Qt/editor messages. No
// external .qm files or language-pack/plugin loader are involved.
class BuiltinChineseTranslator final : public QTranslator {
  public:
    using QTranslator::QTranslator;
    bool isEmpty() const override { return false; }
    QString translate(const char*, const char* source, const char* = nullptr,
                      int = -1) const override {
        static const QHash<QString, QString> messages = {
            {"OK", "确定"},
            {"Save", "保存"},
            {"Cancel", "取消"},
            {"Close", "关闭"},
            {"Yes", "是"},
            {"No", "否"},
            {"Discard", "不保存"},
            {"Open", "打开"},
            {"Abort", "中止"},
            {"Retry", "重试"},
            {"Ignore", "忽略"},
            {"Apply", "应用"},
            {"Reset", "重置"},
            {"Help", "帮助"},
            {"Save All", "全部保存"},
            {"This preview supports text files up to 5 MiB.",
             "初版支持编辑不超过 5 MiB 的文本文件。"},
            {"Binary files cannot be edited.", "无法在文本编辑器中编辑二进制文件。"},
            {"Only UTF-8 files can be edited safely in this version.",
             "初版仅支持安全编辑 UTF-8 编码的文件。"},
            {"File changed on disk. Reload it or explicitly confirm overwriting.",
             "文件已被外部程序修改，请重新打开或明确确认覆盖。"}};
        QString key = QString::fromUtf8(source);
        key.remove('&');
        return messages.value(key);
    }
};

} // namespace idea4git
