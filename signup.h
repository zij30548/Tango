#ifndef SIGNUP_H
#define SIGNUP_H

#include "redisconnection.h"
#include <QButtonGroup>
#include <QCryptographicHash>
#include <QDialog>
#include <QMessageBox>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QUuid>

namespace Ui
{
class SignUp;
}
class QLabel;
class QAction;
class QShowEvent;
class QHideEvent;
class QPushButton;

class SignUp : public QDialog
{
    Q_OBJECT

  public:
    explicit SignUp(QWidget *parent = nullptr);
    ~SignUp();

  private slots:
    void on_pushButton_Back_clicked();

    void on_pushButton_SignUp_clicked();

  private:
    void done(int result) override;
    void showEvent(QShowEvent *event) override;
    void hideEvent(QHideEvent *event) override;
    void setBusy(bool busy);
    void showBackendError(const QString &error);
    void loadRegistrationPolicy();
    void updateRegistrationControls();
    void cancelRegistrationPolicy();
    Ui::SignUp *ui;
    QLabel *authStatus = nullptr;
    QAction *passwordVisibility = nullptr;
    QAction *confirmPasswordVisibility = nullptr;
    bool requestPending = false;
    quint64 requestGeneration = 0;
    QObject *requestContext = nullptr;
    QObject *policyContext = nullptr;
    QPushButton *retryPolicy = nullptr;
    bool policyReady = false;
    bool policyLoading = false;
    quint64 policyGeneration = 0;
    QString policyServiceIdentity;
    QString registrationMode;
    int passwordMinimum = 15;
    int passwordMaximum = 128;
    int usernameMinimum = 2;
    int usernameMaximum = 32;
};

#endif // SIGNUP_H
