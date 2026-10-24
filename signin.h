#ifndef SIGNIN_H
#define SIGNIN_H

#include "redisconnection.h"
#include "signup.h"
#include <QCryptographicHash>
#include <QDialog>
#include <QMessageBox>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QString>

namespace Ui
{
class SignIn;
}
class MainWindow;
class QLabel;
class QAction;
class QShowEvent;
class QHideEvent;

class SignIn : public QDialog
{
    Q_OBJECT

  public:
    explicit SignIn(QWidget *parent = nullptr);
    ~SignIn();
    bool isSignIn = false;

  signals:
    void userLoggedIn(QString username);

  private slots:
    void on_pushButton_Back_clicked();
    void on_pushButton_SignUp_clicked();
    void on_pushButton_SignIn_clicked();

  private:
    void done(int result) override;
    void showEvent(QShowEvent *event) override;
    void hideEvent(QHideEvent *event) override;
    void setBusy(bool busy);
    void showBackendError(const QString &error);
    Ui::SignIn *ui;
    QLabel *authStatus = nullptr;
    QAction *passwordVisibility = nullptr;
    bool requestPending = false;
    quint64 requestGeneration = 0;
    QObject *requestContext = nullptr;
};

#endif
