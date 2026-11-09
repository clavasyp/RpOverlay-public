#pragma once

#include <QWidget>
#include <QString>
#include <QPoint>

class QLineEdit;
class QLabel;
class QPushButton;
class LicenseClient;

// ActivationWindow — ОТДЕЛЬНОЕ окно активации по лицензионному ключу.
//
// Поток (координируется в main.cpp по сигналам AuthClient):
//   • нет валидного токена (AuthClient::needKey) → это окно показывается ПЕРВЫМ (сам оверлей скрыт);
//   • юзер вставляет ключ → onSubmit → LicenseClient::activate → (main) acceptTokens → authorized;
//   • при authorized main прячет это окно и показывает оверлей;
//   • выход из аккаунта в настройках оверлея → токены чистятся → needKey → это окно снова поверх.
//
// Окно самодостаточно (frameless-карточка по центру, always-on-top, перетаскиваемое). Закрытие
// (✕ / Alt+F4) завершает приложение — до активации это единственное окно.
class ActivationWindow : public QWidget {
    Q_OBJECT
public:
    explicit ActivationWindow(LicenseClient *license, QWidget *parent = nullptr);

    void showCentered();    // по центру экрана + поднять + фокус в поле ключа
    void resetForInput();   // сбросить busy/ошибку, разблокировать поле (перед повторным показом)

public slots:
    // Показать ошибку активации (код→человеч. текст готовит LicenseClient/AuthClient). Снимает busy.
    void showError(const QString &code, const QString &human);

protected:
    void mousePressEvent(QMouseEvent *e) override;
    void mouseMoveEvent(QMouseEvent *e) override;
    void closeEvent(QCloseEvent *e) override;   // закрытие окна = выход из приложения

private slots:
    void onSubmit();        // нажата «Активировать» / Enter в поле

private:
    void setBusy(bool busy);

    LicenseClient *license_ = nullptr;
    QLineEdit   *keyEdit_     = nullptr;
    QLabel      *errorLbl_    = nullptr;
    QLabel      *statusLbl_   = nullptr;
    QPushButton *activateBtn_ = nullptr;
    QPoint       dragOffset_;
};
