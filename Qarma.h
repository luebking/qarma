/*
 *   Qarma - a Zenity clone for Qt4 and Qt5
 *   Copyright 2014 by Thomas Lübking <thomas.luebking@gmail.com>
 *
 *   This program is free software; you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License version 2
 *
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details
 *
 *   You should have received a copy of the GNU General Public
 *   License along with this program; if not, write to the
 *   Free Software Foundation, Inc.,
 *   51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 */

#ifndef QARMA_H
#define QARMA_H

class QDialog;
class QTreeWidgetItem;

#include <QApplication>
#include <QPair>

class Qarma : public QApplication
{
    Q_OBJECT
public:
    Qarma(int &argc, char **argv);
    enum Type { Invalid, Calendar, Entry, Error, Info, FileSelection, List, Notification, Progress, Question, Warning,
                Scale, TextInfo, ColorSelection, Password, Forms };
    static void printHelp(const QString &category = QString());
private:
    char showCalendar(const QStringList &args);
    char showEntry(const QStringList &args);
    char showPassword(const QStringList &args);

    char showMessage(const QStringList &args, char type);

    char showFileSelection(const QStringList &args);
    char showList(const QStringList &args);
    char showNotification(const QStringList &args);
    char showProgress(const QStringList &args);
    char showScale(const QStringList &args);
    char showText(const QStringList &args);
    char showColorSelection(const QStringList &args);
    char showForms(const QStringList &args);
    bool readGeneral(QStringList &args);
    bool error(const QString& message);
    void listenToStdIn();
    void notify(const QString& message, bool noClose = false);

    QString labelText(const QString &s) const; // m_zenity requires \n and \t interpretation in html.
private slots:
    void dialogFinished(int status);
    void printInteger(int v);
    void quitOnError();
    void readStdIn();
    void toggleItems(QTreeWidgetItem *item, int column);
    void finishProgress();
private:
    bool m_helpMission, m_modal, m_zenity, m_selectableLabel;
    QString m_caption, m_icon, m_ok, m_cancel, m_notificationHints;
    QSize m_size;
    int m_parentWindow, m_timeout;
    uint m_notificationId;
    QDialog *m_dialog;
    Type m_type;
};

#endif //QARMA_H