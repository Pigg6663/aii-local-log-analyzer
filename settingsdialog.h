#ifndef SETTINGSDIALOG_H
#define SETTINGSDIALOG_H

#include <QDialog>

class QLineEdit;
class QSpinBox;

// 推理服务配置（QSettings 持久化，键前缀 "server/"）
struct ServerSettings {
    QString binaryPath;      // llama-server.exe 路径
    QString modelPath;       // GGUF 模型路径
    quint16 port = 8080;
    int threads = 8;
    int contextSize = 8192;
};

class SettingsDialog : public QDialog
{
    Q_OBJECT
public:
    static ServerSettings loadSettings();
    static void saveSettings(const ServerSettings &s);

    explicit SettingsDialog(QWidget *parent = nullptr);
    ServerSettings settings() const;   // 从当前控件取值

private:
    QLineEdit *m_binaryEdit;
    QLineEdit *m_modelEdit;
    QSpinBox *m_portSpin;
    QSpinBox *m_threadSpin;
    QSpinBox *m_ctxSpin;
};

#endif // SETTINGSDIALOG_H
