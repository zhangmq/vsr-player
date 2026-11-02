// dump_keys.cpp — 把键位表打印成 JSON（T1 静态检查的输入）
//
// 为什么不用 python 解析 Keymap.cpp：序列规范化是 Qt 的语义（"z" 与 "Z" 解析
// 结果相同、Backtab 是 Shift+Tab、Ctrl++ 的键值编码了 Shift…），用正则抄一遍
// 只会得到"看起来对"的结论。这里直接编译真实的 Keymap.cpp，用 Qt 自己的
// QKeySequence 解析，输出给 python 只做断言。
//
// 构建（由 tests/hotkeys/check_keys.py 调用）：
//   g++ -std=c++20 -I src/client $(pkg-config --cflags Qt6Gui Qt6Core) \
//       src/client/Keymap.cpp tests/hotkeys/dump_keys.cpp \
//       -o <tmp>/dump-keys $(pkg-config --libs Qt6Gui Qt6Core)
#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <cstdio>

#include "Keymap.h"

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    const QStringList errs = keymap::selfCheck();
    for (const QString &e : errs)
        fprintf(stderr, "selfcheck: %s\n", qUtf8Printable(e));

    QJsonArray arr;
    for (const QVariant &v : keymap::dump())
        arr.append(QJsonObject::fromVariantMap(v.toMap()));
    const QByteArray json = QJsonDocument(arr).toJson(QJsonDocument::Compact);
    fwrite(json.constData(), 1, size_t(json.size()), stdout);
    fputc('\n', stdout);
    return errs.isEmpty() ? 0 : 1;
}
