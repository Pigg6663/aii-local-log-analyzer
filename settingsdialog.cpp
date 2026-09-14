#include "settingsdialog.h"

#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLineEdit>
#include <QPushButton>
#include <QSettings>
#include <QSpinBox>
#include <QVBoxLayout>
#include <QWidget>

namespace {

// 路径输入框 + "浏览…"按钮的组合行
QWidget *withBrowse(QLineEdit *edit, QPushButton *btn)
{
    auto *w = new QWidget;
    auto *h = new QHBoxLayout(w);
    h->setContentsMargins(0, 0, 0, 0);
    h->addWidget(edit, 1);
    h->addWidget(btn);
    return w;
}

} // namespace

ServerSettings SettingsDialog::loadSettings()
{
    QSettings s;
    ServerSettings cfg;
    cfg.binaryPath  = s.value("server/binary").toString();
    cfg.modelPath   = s.value("server/model").toString();
    cfg.port        = s.value("server/port", 8080).toUInt();
    cfg.threads     = s.value("server/threads", 8).toInt();
    cfg.contextSize = s.value("server/contextSize", 8192).toInt();
    return cfg;
}

void SettingsDialog::saveSettings(const ServerSettings &cfg)
{
    QSettings s;
    s.setValue("server/binary", cfg.binaryPath);
    s.setValue("server/model", cfg.modelPath);
    s.setValue("server/port", cfg.port);
    s.setValue("server/threads", cfg.threads);
    s.setValue("server/contextSize", cfg.contextSize);
}

SettingsDialog::SettingsDialog(QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle("设置");
    const ServerSettings cfg = loadSettings();

    m_binaryEdit = new QLineEdit(cfg.binaryPath, this);
    auto *browseBin = new QPushButton("浏览…", this);
    connect(browseBin, &QPushButton::clicked, this, [this] {
        const QString p = QFileDialog::getOpenFileName(
            this, "选择 llama-server.exe", m_binaryEdit->text(),
            "可执行文件 (*.exe);;所有文件 (*)");
        if (!p.isEmpty())
            m_binaryEdit->setText(p);
    });

    m_modelEdit = new QLineEdit(cfg.modelPath, this);
    auto *browseModel = new QPushButton("浏览…", this);
    connect(browseModel, &QPushButton::clicked, this, [this] {
        const QString p = QFileDialog::getOpenFileName(
            this, "选择 GGUF 模型", m_modelEdit->text(),
            "GGUF 模型 (*.gguf);;所有文件 (*)");
        if (!p.isEmpty())
            m_modelEdit->setText(p);
    });

    m_portSpin = new QSpinBox(this);
    m_portSpin->setRange(1024, 65535);
    m_portSpin->setValue(cfg.port);

    m_threadSpin = new QSpinBox(this);
    m_threadSpin->setRange(1, 128);
    m_threadSpin->setValue(cfg.threads);

    m_ctxSpin = new QSpinBox(this);
    m_ctxSpin->setRange(512, 65536);
    m_ctxSpin->setValue(cfg.contextSize);

    auto *form = new QFormLayout;
    form->addRow("llama-server.exe", withBrowse(m_binaryEdit, browseBin));
    form->addRow("GGUF 模型", withBrowse(m_modelEdit, browseModel));
    form->addRow("端口", m_portSpin);
    form->addRow("线程数", m_threadSpin);
    form->addRow("上下文大小", m_ctxSpin);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    auto *v = new QVBoxLayout(this);
    v->addLayout(form);
    v->addWidget(buttons);
    resize(560, sizeHint().height());
}

ServerSettings SettingsDialog::settings() const
{
    ServerSettings cfg;
    cfg.binaryPath  = m_binaryEdit->text().trimmed();
    cfg.modelPath   = m_modelEdit->text().trimmed();
    cfg.port        = m_portSpin->value();
    cfg.threads     = m_threadSpin->value();
    cfg.contextSize = m_ctxSpin->value();
    return cfg;
}
