#include "ActivationWindow.h"
#include "LicenseClient.h"
#include "Config.h"
#include "StealthGuard.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QGuiApplication>
#include <QScreen>
#include <QMouseEvent>
#include <QCloseEvent>
#include <QDesktopServices>
#include <QUrl>
#include <QApplication>

ActivationWindow::ActivationWindow(LicenseClient *license, QWidget *parent)
    : QWidget(parent)
    , license_(license)
{
    setWindowTitle(QStringLiteral("Активация RP Overlay"));
    // Frameless-карточка по центру, поверх всего. По умолчанию БЕЗ Qt::Tool — окно видно в
    // панели задач, чтобы юзер мог его найти/свернуть до активации. В «режиме невидимка»
    // (тестер-билд) добавляем Qt::Tool — прячем из таскбара/Alt-Tab/списка окон OBS.
    Qt::WindowFlags actFlags = Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint;
    if (stealth::masterOn())
        actFlags |= Qt::Tool;
    setWindowFlags(actFlags);
    setAttribute(Qt::WA_TranslucentBackground, false);
    setFixedWidth(420);

    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(28, 22, 28, 24);
    root->setSpacing(14);

    // Шапка: заголовок + кнопка закрытия (✕ = выход из приложения).
    auto *head = new QHBoxLayout();
    auto *title = new QLabel(QStringLiteral("Активация RP Overlay"), this);
    title->setObjectName(QStringLiteral("actTitle"));
    head->addWidget(title, 1);
    auto *closeBtn = new QPushButton(QStringLiteral("✕"), this);
    closeBtn->setObjectName(QStringLiteral("actClose"));
    closeBtn->setFixedSize(26, 26);
    closeBtn->setCursor(Qt::PointingHandCursor);
    connect(closeBtn, &QPushButton::clicked, qApp, &QApplication::quit);
    head->addWidget(closeBtn, 0, Qt::AlignTop);
    root->addLayout(head);

    auto *subtitle = new QLabel(
        QStringLiteral("Вставьте лицензионный ключ. Взять его можно в боте: /test → «🔑 Мой ключ»."),
        this);
    subtitle->setObjectName(QStringLiteral("actSubtitle"));
    subtitle->setWordWrap(true);
    root->addWidget(subtitle);

    keyEdit_ = new QLineEdit(this);
    keyEdit_->setObjectName(QStringLiteral("actKeyEdit"));
    keyEdit_->setPlaceholderText(QStringLiteral("RPOV-XXXXX-XXXXX-XXXXX-XXXXX"));
    keyEdit_->setAlignment(Qt::AlignCenter);
    keyEdit_->setMaxLength(40);
    connect(keyEdit_, &QLineEdit::returnPressed, this, &ActivationWindow::onSubmit);
    root->addWidget(keyEdit_);

    errorLbl_ = new QLabel(this);
    errorLbl_->setObjectName(QStringLiteral("actError"));
    errorLbl_->setWordWrap(true);
    errorLbl_->setVisible(false);
    root->addWidget(errorLbl_);

    activateBtn_ = new QPushButton(QStringLiteral("Активировать"), this);
    activateBtn_->setObjectName(QStringLiteral("actActivate"));
    activateBtn_->setCursor(Qt::PointingHandCursor);
    connect(activateBtn_, &QPushButton::clicked, this, &ActivationWindow::onSubmit);
    root->addWidget(activateBtn_);

    auto *botBtn = new QPushButton(QStringLiteral("Открыть бота"), this);
    botBtn->setObjectName(QStringLiteral("actBot"));
    botBtn->setCursor(Qt::PointingHandCursor);
    connect(botBtn, &QPushButton::clicked, this, [] {
        const QString bot = cfg().botUsername;
        if (!bot.isEmpty())
            QDesktopServices::openUrl(QUrl(QStringLiteral("https://t.me/%1").arg(bot)));
    });
    root->addWidget(botBtn);

    statusLbl_ = new QLabel(this);
    statusLbl_->setObjectName(QStringLiteral("actStatus"));
    statusLbl_->setWordWrap(true);
    statusLbl_->setAlignment(Qt::AlignCenter);
    root->addWidget(statusLbl_);

    // Тёмная crimson-карточка (самодостаточный стиль, не зависит от темы оверлея).
    setStyleSheet(QStringLiteral(
        "ActivationWindow { background: #15151c; border: 1px solid rgba(232,28,90,0.45);"
        "  border-radius: 14px; }"
        "QLabel#actTitle { color: #f2f2f6; font-family: 'Segoe UI'; font-size: 17px; font-weight: 700; }"
        "QLabel#actSubtitle { color: #b9b9c4; font-family: 'Segoe UI'; font-size: 12px; }"
        "QLabel#actStatus { color: #9a9aa6; font-family: 'Segoe UI'; font-size: 11px; }"
        "QLabel#actError { color: #e06c75; font-family: 'Segoe UI'; font-size: 12px; }"
        "QLineEdit#actKeyEdit { background: #0e0e14; color: #f2f2f6; border: 1px solid rgba(255,255,255,0.12);"
        "  border-radius: 8px; padding: 10px; font-family: 'Consolas','Segoe UI'; font-size: 14px;"
        "  letter-spacing: 1px; }"
        "QLineEdit#actKeyEdit:focus { border: 1px solid #e81c5a; }"
        "QPushButton#actActivate { background: #e81c5a; color: #ffffff; border: none; border-radius: 8px;"
        "  padding: 11px; font-family: 'Segoe UI'; font-size: 14px; font-weight: 600; }"
        "QPushButton#actActivate:hover { background: #ff2e6c; }"
        "QPushButton#actActivate:disabled { background: #6b2740; color: #c9c9d0; }"
        "QPushButton#actBot { background: transparent; color: #c9c9d4; border: 1px solid rgba(255,255,255,0.16);"
        "  border-radius: 8px; padding: 9px; font-family: 'Segoe UI'; font-size: 12px; }"
        "QPushButton#actBot:hover { border-color: #e81c5a; color: #ffffff; }"
        "QPushButton#actClose { background: transparent; color: #b9b9c4; border: none; font-size: 15px; }"
        "QPushButton#actClose:hover { color: #ffffff; }"));
}

void ActivationWindow::showCentered()
{
    if (QScreen *scr = QGuiApplication::primaryScreen()) {
        adjustSize();
        const QRect g = scr->availableGeometry();
        move(g.center().x() - width() / 2, g.center().y() - height() / 2);
    }
    show();
    raise();
    activateWindow();
    if (keyEdit_)
        keyEdit_->setFocus();
}

void ActivationWindow::resetForInput()
{
    setBusy(false);
    if (errorLbl_)  { errorLbl_->clear(); errorLbl_->setVisible(false); }
    if (statusLbl_) statusLbl_->clear();
    if (keyEdit_)   keyEdit_->setFocus();
}

void ActivationWindow::setBusy(bool busy)
{
    if (activateBtn_) activateBtn_->setEnabled(!busy);
    if (keyEdit_)     keyEdit_->setEnabled(!busy);
}

void ActivationWindow::onSubmit()
{
    if (!keyEdit_)
        return;
    const QString key = keyEdit_->text().trimmed();
    if (key.isEmpty())
        return;
    if (!LicenseClient::validateFormat(key)) {
        showError(QStringLiteral("invalid_key_format"),
                  QStringLiteral("Неверный формат ключа. Проверьте и повторите."));
        return;
    }
    if (errorLbl_) errorLbl_->setVisible(false);
    setBusy(true);
    if (statusLbl_) statusLbl_->setText(QStringLiteral("Активирую…"));
    if (license_)
        license_->activate(key);
    else
        showError(QStringLiteral("nolic"), QStringLiteral("Внутренняя ошибка: нет LicenseClient."));
}

void ActivationWindow::showError(const QString &code, const QString &human)
{
    Q_UNUSED(code);
    setBusy(false);
    if (statusLbl_) statusLbl_->clear();
    if (errorLbl_) {
        errorLbl_->setText(human);
        errorLbl_->setVisible(true);
    }
    adjustSize();
}

void ActivationWindow::mousePressEvent(QMouseEvent *e)
{
    if (e->button() == Qt::LeftButton)
        dragOffset_ = e->globalPosition().toPoint() - frameGeometry().topLeft();
}

void ActivationWindow::mouseMoveEvent(QMouseEvent *e)
{
    if (e->buttons() & Qt::LeftButton)
        move(e->globalPosition().toPoint() - dragOffset_);
}

void ActivationWindow::closeEvent(QCloseEvent *e)
{
    // Закрытие окна активации = выход из приложения (до активации это единственное окно;
    // app.setQuitOnLastWindowClosed(false), поэтому гасим явно).
    e->accept();
    qApp->quit();
}
