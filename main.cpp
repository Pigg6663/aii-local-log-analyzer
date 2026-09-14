#include "mainwindow.h"

#include <QApplication>

#include "datatypes.h"

int main(int argc, char *argv[])
{
    // 自定义类型经 queued 连接跨线程传递（信号参数按值拷贝）前必须注册，
    // 漏注册的典型症状：连接建立但槽永远收不到 + 运行时警告
    qRegisterMetaType<AnomalyFinding>("AnomalyFinding");
    qRegisterMetaType<QVector<AnomalyFinding>>("QVector<AnomalyFinding>");

    QApplication a(argc, argv);
    // QSettings 归属（Windows 下写入 HKCU\Software\aii\...）
    QCoreApplication::setOrganizationName("aii");
    QCoreApplication::setApplicationName("AI日志分析工具");
    MainWindow w;
    w.show();
    return a.exec();
}
