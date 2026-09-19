#include "dialogs/ProFeatureDialog.h"

#include <QMessageBox>
#include <QString>
#include <QWidget>

void ProFeatureDialog::show(QWidget *parent, const QString &featureName)
{
    QMessageBox::information(parent,
        QStringLiteral("Pro版限定機能"),
        QStringLiteral("「%1」はPro版限定の機能です。\n\n"
                        "Pro版のライセンスをお持ちの場合は、ライセンスファイルを\n"
                        "%2\n"
                        "に配置してからアプリを再起動してください。")
            .arg(featureName)
#ifdef Q_OS_WIN
            .arg(QStringLiteral("%APPDATA%\\pwxwx\\Tiepolo\\license.tiepololicense"))
#else
            .arg(QStringLiteral("アプリのデータディレクトリ内のlicense.tiepololicense"))
#endif
    );
}
