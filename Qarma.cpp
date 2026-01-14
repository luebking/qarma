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

#include "Qarma.h"

#include <QAction>
#include <QBoxLayout>
#include <QCalendarWidget>
#include <QCheckBox>
#include <QColorDialog>
#include <QComboBox>
#include <QCryptographicHash>
#include <QDate>
#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include <QDBusInterface>
#include <QDialogButtonBox>
#include <QEvent>
#include <QFileDialog>
#include <QFontDialog>
#include <QFormLayout>
#include <QIcon>
#include <QImageReader>
#include <QInputDialog>
#include <QLabel>
#include <QLocale>
#include <QLineEdit>
#include <QMessageBox>
#include <QProcess>
#include <QProgressDialog>
#include <QPropertyAnimation>
#include <QProxyStyle>
#include <QPushButton>
#include <QScreen>
#include <QScrollBar>
#include <QSettings>
#include <QSlider>
#include <QSocketNotifier>
#include <QSplitter>
#include <QStandardPaths>
#include <QStringBuilder>
#include <QStringList>
#include <QTextBrowser>
#include <QTimer>
#include <QTimerEvent>
#include <QTreeWidget>
#include <QTreeWidgetItem>

#if QT_VERSION >= 0x050000
// this is to hack access to the --title parameter in Qt5
#include <QWindow>
#endif

#include <QtDebug>

#include <cfloat>

#ifdef Q_OS_UNIX
#include <signal.h>
#include <unistd.h>
#endif

#define QARMA_VERSION "1.1.0"
#define ZENITY_VERSION "4.2.1"


#if (QT_VERSION >= QT_VERSION_CHECK(5, 14, 0))
    #define SKIP_EMPTY Qt::SkipEmptyParts
#else
    #define SKIP_EMPTY QString::SkipEmptyParts
#endif

class InputGuard : public QObject
{
public:
    InputGuard() : QObject(), m_guardedWidget(NULL), m_checkTimer(0) {}
    static void watch(QWidget *w) {
#if QT_VERSION >= 0x050000
        if (qApp->platformName() == "wayland")
            return;
#endif
        if (!s_instance)
            s_instance = new InputGuard;
        w->installEventFilter(s_instance);
    }
protected:
    bool eventFilter(QObject *o, QEvent *e) {
        QWidget *w = static_cast<QWidget*>(o);
        switch (e->type()) {
            case QEvent::FocusIn:
            case QEvent::WindowActivate:
                if (hasActiveFocus(w))
                    guard(w);
                break;
            case QEvent::FocusOut:
            case QEvent::WindowDeactivate:
                if (w == m_guardedWidget && !hasActiveFocus(w))
                    unguard(w);
                break;
            default:
                break;
        }
        return false;
    }
    void timerEvent(QTimerEvent *te) {
        if (te->timerId() == m_checkTimer && m_guardedWidget)
            check(m_guardedWidget);
        else
            QObject::timerEvent(te);
    }
private:
    bool check(QWidget *w) {
#if QT_VERSION >= 0x050000
        // try to re-grab
        if (!w->window()->windowHandle()->setKeyboardGrabEnabled(true))
            w->releaseKeyboard();
#endif
        if (w == QWidget::keyboardGrabber()) {
            w->setPalette(QPalette());
            return true;
        }
        w->setPalette(QPalette(Qt::white, Qt::red, Qt::white, Qt::black, Qt::gray,
                               Qt::white, Qt::white, Qt::red, Qt::red));
        return false;
    }
    void guard(QWidget *w) {
        w->grabKeyboard();
        if (check(w))
            m_guardedWidget = w;
        if (!m_checkTimer)
            m_checkTimer = startTimer(500);
    }
    bool hasActiveFocus(QWidget *w) {
        return w == QApplication::focusWidget() && w->isActiveWindow();
    }
    void unguard(QWidget *w) {
        Q_ASSERT(m_guardedWidget == w);
        killTimer(m_checkTimer);
        m_checkTimer = 0;
        m_guardedWidget = NULL;
        w->releaseKeyboard();
    }
private:
    QWidget *m_guardedWidget;
    int m_checkTimer;
    static InputGuard *s_instance;
};

InputGuard *InputGuard::s_instance = NULL;

#ifdef WS_X11
#if (QT_VERSION >= QT_VERSION_CHECK(6, 0, 0))
#include <QGuiApplication>
#else
#include <QX11Info>
#endif
#include <X11/Xlib.h>
#endif

typedef QPair<QString, QString> Help;
typedef QList<Help> HelpList;
typedef QPair<QString, HelpList> CategoryHelp;
typedef QMap<QString, CategoryHelp> HelpDict;


Qarma::Qarma(int &argc, char **argv) : QApplication(argc, argv)
, m_modal(false)
, m_selectableLabel(false)
, m_popup(false)
, m_parentWindow(0)
, m_timeout(0)
, m_notificationId(0)
, m_dialog(NULL)
, m_type(Invalid)
{
    m_pos = QPoint(INT_MAX, INT_MAX); // invalid
    m_size = QSize(0,0); // so we can reasonably use isNull …
    QStringList argList = QCoreApplication::arguments(); // arguments() is slow
    const QString binary = argList.at(0);
    m_zenity = binary.endsWith("zenity");
    // make canonical list
    QStringList args;
    if (argList.at(0).endsWith("-askpass")) {
        argList.removeFirst();
        args << "--title" << tr("Enter Password") << "--password" << "--prompt" << argList.join(' ');
    } else {
        for (int i = 1; i < argList.count(); ++i) {
            if (argList.at(i).startsWith("--")) {
                int split = argList.at(i).indexOf('=');
                if (split > -1) {
                    args << argList.at(i).left(split) << argList.at(i).mid(split+1);
                } else {
                    args << argList.at(i);
                }
            } else {
                args << argList.at(i);
            }
        }
    }
    argList.clear();

    if (!readGeneral(args))
        return;

    // set application class
    if (!m_class.isNull())
        QCoreApplication::setApplicationName(m_class);

    // set application name
    if (!m_name.isNull())
        // Qt intercepts "--name name" but fails to process "--name=name"
        // Workaround this by setting the RESOURCE_NAME environment variable
        // which ends up being used to populate the WM_CLASS name on X11.
        qputenv("RESOURCE_NAME", m_name.toLocal8Bit());

    char error = 1;
    foreach (const QString &arg, args) {
        if (arg == "--calendar") {
            m_type = Calendar;
            error = showCalendar(args);
        } else if (arg == "--entry") {
            m_type = Entry;
            error = showEntry(args);
        } else if (arg == "--error") {
            m_type = Error;
            error = showMessage(args, 'e');
        } else if (arg == "--info") {
            m_type = Info;
            error = showMessage(args, 'i');
        } else if (arg == "--file-selection") {
            m_type = FileSelection;
            error = showFileSelection(args);
        } else if (arg == "--list") {
            m_type = List;
            error = showList(args);
        } else if (arg == "--notification") {
            m_type = Notification;
            error = showNotification(args);
        } else if (arg == "--progress") {
            m_type = Progress;
            error = showProgress(args);
        } else if (arg == "--question") {
            m_type = Question;
            error = showMessage(args, 'q');
        } else if (arg == "--warning") {
            m_type = Warning;
            error = showMessage(args, 'w');
        } else if (arg == "--scale") {
            m_type = Scale;
            error = showScale(args);
        } else if (arg == "--text-info") {
            m_type = TextInfo;
            error = showText(args);
        } else if (arg == "--color-selection") {
            m_type = ColorSelection;
            error = showColorSelection(args);
        } else if (arg == "--font-selection") {
            m_type = FontSelection;
            error = showFontSelection(args);
        } else if (arg == "--password") {
            m_type = Password;
            error = showPassword(args);
        } else if (arg == "--forms") {
            m_type = Forms;
            error = showForms(args);
        } else if (arg == "--dzen") {
            m_type = Dzen;
            error = showDzen(args);
        } else if (arg == "--version") {
            printf("%s\n", m_zenity ? ZENITY_VERSION : QARMA_VERSION);
            exit(0);
        }
        if (error != 1) {
            break;
        }
    }

    if (error) {
        QMetaObject::invokeMethod(this, "quit", Qt::QueuedConnection);
        return;
    }

    if (m_dialog) {
#if QT_VERSION >= 0x050000
        /*  Stage #1 one of "Setting a window title should be easy but Qt5 is dumb"
            Qt5 sucks away "--title foo" but not "--title=foo" and then stumbles
            over itself when trying to set it
            So #1 we seek to get ourselfs access to the window title to get some
            control over it

            since it's set on showing the first QWindow, we just create one here
            and copy the title
        */
        /// @todo: remove once this is fixed in Qt5 - ie. "never"
        const bool qt5title = m_caption.isNull(); // otherwise we read it in the general options
        if (qt5title) {
            QWindow *w = new QWindow;
            w->setVisible(true);
            m_caption = w->title();
            delete w;
            m_dialog->setWindowTitle("");
        }
        m_dialog->setWindowTitle(m_caption);
        // so much for stage one, see below for more on Qt5 being dumb...
#endif
        // close on ctrl+return in addition to ctrl+enter
        QAction *shortAccept = new QAction(m_dialog);
        m_dialog->addAction(shortAccept);
        QList<QKeySequence> cuts;
        cuts << QKeySequence(Qt::CTRL|Qt::Key_Return) << QKeySequence(Qt::CTRL|Qt::Key_Enter);
        shortAccept->setShortcuts(cuts);
        connect (shortAccept, SIGNAL(triggered()), m_dialog, SLOT(accept()));

        // workaround for #21 - since QWidget is now merely bitrot, QDialog closes,
        // but does not reject on the escape key (unlike announced in the specific section of the API)
        QAction *shortReject = new QAction(m_dialog);
        m_dialog->addAction(shortReject);
        shortReject->setShortcut(QKeySequence(Qt::Key_Escape));
        connect (shortReject, SIGNAL(triggered()), m_dialog, SLOT(reject()));

        if (!m_size.isNull()) {
            m_dialog->adjustSize();
            QSize sz = m_dialog->size();
            if (m_size.width() > 0)
                sz.setWidth(m_size.width());
            if (m_size.height() > 0)
                sz.setHeight(m_size.height());
            m_dialog->resize(sz);
        }
        if (m_pos.x() < INT_MAX) {
            QRect desktop = QGuiApplication::screens().at(0)->availableGeometry();
            if (m_pos.x() < 0)
                m_pos.rx() = desktop.right() + m_pos.x() - m_dialog->width();
            if (m_pos.y() < 0)
                m_pos.ry() = desktop.bottom() + m_pos.y() - m_dialog->height();
            m_dialog->move(m_pos);
        }
        m_dialog->setWindowModality(m_modal ? Qt::ApplicationModal : Qt::NonModal);

        /*  Stage #2 one of "Setting a window title should be easy but Qt5 is dumb"
            Errhemmm... Fuck! This! Shit!
            Not only is Qt5 too dumb to set the window title by parameter, if one
            does it, at least on X11 Qt5 doesn't set the property on the platform
            window *before* mapping it, but *while* - causing an IPC race condition
            with the WMs that will pick up the "old" title, but also miss the property
            update because it can happen while they're configuring the window.
            Awesome.
            Because we can't retroactively preceed the mapping, we at least need to
            make sure that the title gets updated after the window is mapped and
            configured - what we can only guess witha timer... *grrrrr*
            Since setting the same title is idempotent and skipped in ::setWindowTitle
            (what would be great if the entire thing wasn't broken to begin with)
            We need two steps to first clear and second reset the title - maximzing
            the server roundtrips...
            But hey, abstract and wayland and QML... FUCK! THIS! SHIT! *grrrrrrrr*
        */
        // We do this after the dialog creation, because the setup can take varying times
        if (!(m_caption.isNull() || binary.endsWith(m_caption)))
            QTimer::singleShot(10, this, [=]() {m_dialog->setWindowTitle(""); m_dialog->setWindowTitle(m_caption);});
        // so much for setting the window title - despite Qt trying to do it by itself

        if (!m_icon.isNull())
            m_dialog->setWindowIcon(QIcon(m_icon));
        QDialogButtonBox *box = m_dialog->findChild<QDialogButtonBox*>();
        if (box && !m_ok.isNull()) {
            if (QPushButton *btn = box->button(QDialogButtonBox::Ok)) {
                btn->setText(m_ok);
                if (m_ok.isEmpty())
                    btn->hide();
            }
        }
        if (box && !m_cancel.isNull()) {
            if (QPushButton *btn = box->button(QDialogButtonBox::Cancel)) {
                btn->setText(m_cancel);
                if (m_cancel.isEmpty())
                    btn->hide();
            }
        }
        if (m_parentWindow) {
#ifdef WS_X11
            m_dialog->setAttribute(Qt::WA_X11BypassTransientForHint);
            Display *dpy;
#if (QT_VERSION >= QT_VERSION_CHECK(6, 0, 0))
            dpy = QGuiApplication::nativeInterface<QNativeInterface::QX11Application>()->display();
#else
            dpy = QX11Info::display();
#endif
            XSetTransientForHint(dpy, m_dialog->winId(), m_parentWindow);
#endif
        }
    }
}

bool Qarma::error(const QString message)
{
    printf("Error: %s", qPrintable(message));
    QMetaObject::invokeMethod(this, "quitOnError", Qt::QueuedConnection);
    return true;
}

#define IF_IS(_TYPE_) if (const _TYPE_ *t = qobject_cast<const _TYPE_*>(w))

static QString value(const QWidget *w, const QString &pattern)
{
    if (!w)
        return QString();

    IF_IS(QLineEdit) {
        return t->text();
    } else IF_IS(QTreeWidget) {
        QString s;
        foreach (QTreeWidgetItem *item, t->selectedItems()) {
            for (int i = 0; i < t->columnCount(); ++i)
                s += item->text(i);
        }
        return s;
    } else IF_IS(QComboBox) {
        return t->currentText();
    } else IF_IS(QCalendarWidget) {
        if (pattern.isNull())
            return QLocale::system().toString(t->selectedDate(), QLocale::ShortFormat);
        return t->selectedDate().toString(pattern);
    } else IF_IS(QCheckBox) {
        return t->isChecked() ? "true" : "false";
    }
    return QString();
}

void Qarma::dialogFinished(int status)
{
    if (m_type == FileSelection) {
        QFileDialog *dlg = static_cast<QFileDialog*>(sender());
        QVariantList l;
        for (int i = 0; i < dlg->sidebarUrls().count(); ++i)
            l << dlg->sidebarUrls().at(i);
        QSettings settings("qarma");
        settings.setValue("Bookmarks", l);
        settings.setValue("FileDetails", dlg->viewMode() == QFileDialog::Detail);
    }

    if (!(status == QDialog::Accepted || status == QMessageBox::Ok || status == QMessageBox::Yes)) {
#ifdef Q_OS_UNIX
        if (sender()->property("qarma_autokill_parent").toBool()) {
            ::kill(getppid(), 15);
        }
#endif
        exit(1);
        return;
    }

    switch (m_type) {
        case Question:
        case Warning:
        case Info:
        case Error:
        case Progress:
        case Notification:
            break;
        case Calendar: {
            QString format = sender()->property("qarma_date_format").toString();
            QDate date = sender()->findChild<QCalendarWidget*>()->selectedDate();
            if (format.isEmpty())
                printf("%s\n", qPrintable(QLocale::system().toString(date, QLocale::ShortFormat)));
            else
                printf("%s\n", qPrintable(date.toString(format)));
            break;
        }
        case Entry: {
            QInputDialog *dlg = static_cast<QInputDialog*>(sender());
            if (dlg->inputMode() == QInputDialog::DoubleInput) {
                printf("%s\n", qPrintable(QLocale::c().toString(dlg->doubleValue(), 'f', 2)));
            } else if (dlg->inputMode() == QInputDialog::IntInput) {
                printf("%d\n", dlg->intValue());
            } else {
                printf("%s\n", qPrintable(dlg->textValue()));
            }
            break;
        }
        case Password: {
            QLineEdit   *username = sender()->findChild<QLineEdit*>("qarma_username"),
                        *password = sender()->findChild<QLineEdit*>("qarma_password");
            QString result;
            if (username)
                result = username->text() + '|';
            if (password)
                result += password->text();
            printf("%s\n", qPrintable(result));
            break;
        }
        case FileSelection: {
            QStringList files = static_cast<QFileDialog*>(sender())->selectedFiles();
            printf("%s\n", qPrintable(files.join(sender()->property("qarma_separator").toString())));
            break;
        }
        case ColorSelection: {
            QColorDialog *dlg = static_cast<QColorDialog*>(sender());
            printf("%s\n", qPrintable(dlg->selectedColor().name()));
            QVariantList l;
            for (int i = 0; i < dlg->customCount(); ++i)
                l << dlg->customColor(i).rgba();
            QSettings("qarma").setValue("CustomPalette", l);
            break;
        }
        case FontSelection: {
            QFontDialog *dlg = static_cast<QFontDialog*>(sender());
            QFont fnt = dlg->selectedFont();
            int size = fnt.pointSize();
            if (size < 0)
                size = fnt.pixelSize();

            // crude mapping of Qt's random enum to xft's random category
            QString weight = "medium";
            if (fnt.weight() < 35) weight = "light";
            else if (fnt.weight() > 85) weight = "black";
            else if (fnt.weight() > 70) weight = "bold";
            else if (fnt.weight() > 60) weight = "demibold";

            QString slant = "roman";
            if (fnt.style() == QFont::StyleItalic) slant = "italic";
            else if (fnt.style() == QFont::StyleOblique) slant = "oblique";

            QString font = sender()->property("qarma_fontpattern").toString();
            font = font.arg(fnt.family()).arg(size).arg(weight).arg(slant);
            printf("%s\n", qPrintable(font));
            break;
        }
        case TextInfo: {
            QTextEdit *te = sender()->findChild<QTextEdit*>();
            if (te && !te->isReadOnly()) {
                printf("%s\n", qPrintable(te->toPlainText()));
            }
            break;
        }
        case Scale: {
            QSlider *sld = sender()->findChild<QSlider*>();
            if (sld) {
                printf("%s\n", qPrintable(QString::number(sld->value())));
            }
            break;
        }
        case List: {
            QTreeWidget *tw = sender()->findChild<QTreeWidget*>();
            QStringList result;
            if (tw) {
                QVariant v = sender()->property("qarma_print_column");
                int column = v.toString() == "ALL" ? -1 : v.toInt();
                if (column > tw->columnCount())
                    column = -1;
                else if (column > 0)
                    --column;
                bool done(false);
                auto text = [=](const QTreeWidgetItem *twi, int col, int offset) {
                    if (col > -1)
                        return (col < offset) ? QString() : twi->text(col);
                    QString s;
                    for (int i = offset; i < tw->columnCount()-1; ++i)
                        s += twi->text(i) + '\t';
                    s += twi->text(tw->columnCount()-1);
                    return s;
                };
                foreach (const QTreeWidgetItem *twi, tw->selectedItems()) {
                    done = true;
                    result << text(twi, column, 0);
                }
                if (!done) { // checkable
                    for (int i = 0; i < tw->topLevelItemCount(); ++i) {
                        const QTreeWidgetItem *twi = tw->topLevelItem(i);
                        if (twi->checkState(0) == Qt::Checked)
                            result << text(twi, column, 1);
                    }
                }
            }
            printf("%s\n", qPrintable(result.join(sender()->property("qarma_separator").toString())));
            break;
        }
        case Forms: {
            QFormLayout *fl = sender()->findChild<QFormLayout*>();
            QStringList result;
            QString format = sender()->property("qarma_date_format").toString();
            for (int i = 0; i < fl->count(); ++i) {
                if (QLayoutItem *li = fl->itemAt(i, QFormLayout::FieldRole))
                    result << value(li->widget(), format);
            }
            printf("%s\n", qPrintable(result.join(sender()->property("qarma_separator").toString())));
            break;
        }
        default:
            qDebug() << "unhandled output" << m_type;
            break;
    }
    exit (0);
}

void Qarma::quitOnError()
{
    exit(1);
}

#define NEXT_ARG QString((++i < args.count()) ? args.at(i) : QString())
#define WARN_UNKNOWN_ARG(_KNOWN_) if (args.at(i).startsWith("--") && args.at(i) != _KNOWN_) qDebug() << "unspecific argument" << args.at(i);
#define SHOW_DIALOG m_dialog = dlg; connect(dlg, SIGNAL(finished(int)), SLOT(dialogFinished(int))); dlg->show();
#define READ_INT(_V_, _TYPE_, _ERROR_) bool ok; const int _V_ = NEXT_ARG.to##_TYPE_(&ok); if (!ok) return !error(_ERROR_)

bool Qarma::readGeneral(QStringList &args) {
    QStringList remains;
    for (int i = 0; i < args.count(); ++i) {
        if (args.at(i) == "--title") {
            m_caption = NEXT_ARG;
        } else if (args.at(i) == "--window-icon") {
            m_icon = NEXT_ARG;
        } else if (args.at(i) == "--width") {
            READ_INT(w, UInt, "--width must be followed by a positive number");
            m_size.setWidth(w);
        } else if (args.at(i) == "--height") {
            READ_INT(h, UInt, "--height must be followed by a positive number");
            m_size.setHeight(h);
        } else if (args.at(i) == "--pos") {
            QString pos = NEXT_ARG;
            QRegularExpressionMatch m = QRegularExpression("([+-]*[0-9]+)([+-][0-9]+)?").match(pos);
            if (m.lastCapturedIndex() > 0 && m.lastCapturedIndex() < 3) {
                m_pos.setX(m.captured(1).toInt());
                m_pos.setY(m.lastCapturedIndex() == 2 ? m.captured(2).toInt() : 0);
            } else {
                return !error("--pos must be followed by a position [+-]x[(+-)y]");
            }
        } else if (args.at(i) == "--timeout") {
            READ_INT(t, UInt, "--timeout must be followed by a positive number");
            QTimer::singleShot(t*1000, this, SLOT(quit()));
        } else if (args.at(i) == "--ok-label") {
            m_ok = NEXT_ARG;
        } else if (args.at(i) == "--cancel-label") {
            m_cancel = NEXT_ARG;
        } else if (args.at(i) == "--modal") {
            m_modal = true;
        } else if (args.at(i) == "--popup") {
            m_popup = true;
        } else if (args.at(i) == "--attach") {
            READ_INT(w, UInt, "--attach must be followed by a positive number");
            m_parentWindow = w;
        } else if (args.at(i) == "--class") {
            m_class = NEXT_ARG;
        } else if (args.at(i) == "--name") {
            m_name = NEXT_ARG;
        } else {
            remains << args.at(i);
        }
    }
    args = remains;
    return true;
}

#define NEW_DIALOG QDialog *dlg = new QDialog; QVBoxLayout *vl = new QVBoxLayout(dlg); if (m_popup) dlg->setWindowFlags(Qt::Popup);
#define FINISH_DIALOG(_BTNS_)   QDialogButtonBox *btns = new QDialogButtonBox(_BTNS_, Qt::Horizontal, dlg);\
                                vl->addWidget(btns);\
                                connect(btns, SIGNAL(accepted()), dlg, SLOT(accept()));\
                                connect(btns, SIGNAL(rejected()), dlg, SLOT(reject()));

char Qarma::showCalendar(const QStringList &args)
{
    NEW_DIALOG

    QDate date = QDate::currentDate();
    int d,m,y;
    date.getDate(&y, &m, &d);
    bool ok;
    for (int i = 0; i < args.count(); ++i) {
        if (args.at(i) == "--text") {
            vl->addWidget(new QLabel(NEXT_ARG, dlg));
        } else if (args.at(i) == "--day") {
            d = NEXT_ARG.toUInt(&ok);
            if (!ok)
                return !error("--day must be followed by a positive number");
        } else if (args.at(i) == "--month") {
            m = NEXT_ARG.toUInt(&ok);
            if (!ok)
                return !error("--month must be followed by a positive number");
        } else if (args.at(i) == "--year") {
            y = NEXT_ARG.toUInt(&ok);
            if (!ok)
                return !error("--year must be followed by a positive number");
        } else if (args.at(i) == "--date-format") {
            dlg->setProperty("qarma_date_format", NEXT_ARG);
        } else { WARN_UNKNOWN_ARG("--calendar") }
    }
    date.setDate(y, m, d);

    QCalendarWidget *cal = new QCalendarWidget(dlg);
    cal->setSelectedDate(date);
    vl->addWidget(cal);
    connect(cal, SIGNAL(activated(const QDate&)), dlg, SLOT(accept()));
    if (!m_popup) {
        FINISH_DIALOG(QDialogButtonBox::Ok|QDialogButtonBox::Cancel);
    }
    SHOW_DIALOG
    return 0;
}

char Qarma::showEntry(const QStringList &args)
{
    QInputDialog *dlg = new QInputDialog;
    for (int i = 0; i < args.count(); ++i) {
        if (args.at(i) == "--text")
            dlg->setLabelText(labelText(NEXT_ARG));
        else if (args.at(i) == "--entry-text")
            dlg->setTextValue(NEXT_ARG);
        else if (args.at(i) == "--hide-text")
            dlg->setTextEchoMode(QLineEdit::Password);
        else if (args.at(i) == "--values") {
            dlg->setComboBoxItems(NEXT_ARG.split('|'));
            dlg->setComboBoxEditable(true);
        } else if (args.at(i) == "--int") {
            dlg->setInputMode(QInputDialog::IntInput);
            dlg->setIntRange(INT_MIN, INT_MAX);
            dlg->setIntValue(NEXT_ARG.toInt());
        } else if (args.at(i) == "--float") {
            dlg->setInputMode(QInputDialog::DoubleInput);
            dlg->setDoubleRange(DBL_MIN, DBL_MAX);
            dlg->setDoubleValue(NEXT_ARG.toDouble());
        }
        else { WARN_UNKNOWN_ARG("--entry") }
    }
    SHOW_DIALOG

    return 0;
}

char Qarma::showPassword(const QStringList &args)
{
    NEW_DIALOG

    QLineEdit *username(NULL), *password(NULL);
    QString prompt = tr("Enter password");
    for (int i = 0; i < args.count(); ++i) {
        if (args.at(i) == "--username") {
            vl->addWidget(new QLabel(tr("Enter username"), dlg));
            vl->addWidget(username = new QLineEdit(dlg));
            username->setObjectName("qarma_username");
            break;
        } else if (args.at(i) == "--prompt") {
            prompt = NEXT_ARG;
        } { WARN_UNKNOWN_ARG("--password") }
    }

    vl->addWidget(new QLabel(prompt, dlg));
    vl->addWidget(password = new QLineEdit(dlg));
    password->setObjectName("qarma_password");
    password->setEchoMode(QLineEdit::Password);

    InputGuard::watch(password);

    if (username)
        username->setFocus(Qt::OtherFocusReason);
    else
        password->setFocus(Qt::OtherFocusReason);

    FINISH_DIALOG(QDialogButtonBox::Ok|QDialogButtonBox::Cancel);
    SHOW_DIALOG
    return 0;
}

char Qarma::showMessage(const QStringList &args, char type)
{
    QMessageBox *dlg = new QMessageBox;
    dlg->setStandardButtons((type == 'q') ? QMessageBox::Yes|QMessageBox::No : QMessageBox::Ok);
    dlg->setDefaultButton(QMessageBox::Ok);

    bool wrap = true, html = true;
    for (int i = 0; i < args.count(); ++i) {
        if (args.at(i) == "--text")
            dlg->setText(html ? labelText(NEXT_ARG) : NEXT_ARG);
        else if (args.at(i) == "--icon-name")
            dlg->setIconPixmap(QIcon(NEXT_ARG).pixmap(64));
        else if (args.at(i) == "--no-wrap")
            wrap = false;
        else if (args.at(i) == "--ellipsize")
            wrap = true;
        else if (args.at(i) == "--no-markup")
            html = false;
        else if (args.at(i) == "--default-cancel")
            dlg->setDefaultButton(QMessageBox::Cancel);
        else if (args.at(i) == "--selectable-labels")
            m_selectableLabel = true;
        else if (args.at(i).startsWith("--") && args.at(i) != "--info" && args.at(i) != "--question" &&
                                                args.at(i) != "--warning" && args.at(i) != "--error")
            qDebug() << "unspecific argument" << args.at(i);
    }
    QLabel *msgLabel = dlg->findChild<QLabel*>("qt_msgbox_label");
    if (msgLabel) {
        // this is pointless because QMessageBox fucks around with that when applying its size constraints, see below
//        msgLabel->setWordWrap(wrap);
        msgLabel->setTextFormat(html ? Qt::RichText : Qt::PlainText);
        if (m_selectableLabel)
            msgLabel->setTextInteractionFlags(msgLabel->textInteractionFlags()|Qt::TextSelectableByMouse);
    }
    if (dlg->iconPixmap().isNull())
        dlg->setIcon(type == 'w' ? QMessageBox::Warning :
                   (type == 'q' ? QMessageBox::Question :
                   (type == 'e' ? QMessageBox::Critical : QMessageBox::Information)));
    SHOW_DIALOG
    // **** grrrrrrrr *****
    // https://github.com/luebking/qarma/issues/62
    // https://runebook.dev/en/docs/qt/qmessagebox/resizeEvent
    // the suggested spacer however doesn't work, but Qt allows us to fix the size after the show event
    //
    // still a lousy hack which might run into windowmanager related problems
    if (!(wrap && m_size.isNull()) && msgLabel) {
        if (m_size.width() > 0 && dlg->width() <= 500) {
            // QMessageBox unconditionally disables wrapping for dialogs < 500px width, so don't try to shrink those
            m_size.setWidth(qMax(m_size.width(), dlg->width()));
        }
        // figure the dimensions of the dialog if the label wasn't there
        QLabel *icnLabel = dlg->findChild<QLabel*>("qt_msgboxex_icon_label");
        QSize delta = msgLabel->size();
        // we need to account for the icon left of the label - if that's taller than the label
        // it dictates the labels effective height
        if (icnLabel)
            delta.setHeight(qMax(delta.height(), icnLabel->height()));
        // delta is now the virtual label size, subtract if from the dialog
        delta = dlg->size() - delta;
        QRect r;
        QMargins marge = msgLabel->contentsMargins() + QMargins(4,4,4,4);
        if (!wrap) {
            r = msgLabel->fontMetrics().boundingRect(msgLabel->text());
        } else if (m_size.width() < 1 || m_size.height() < 1) {
            // likewise we can apply unilateral --width/--height
            r = QRect(0,0,QWIDGETSIZE_MAX,QWIDGETSIZE_MAX);
            if (m_size.width() > 0)
                r.setWidth(m_size.width() - delta.width());
            r = msgLabel->fontMetrics().boundingRect(r, Qt::TextWordWrap, msgLabel->text());
            if (m_size.height() > 0) {
                // for only fixed height, calculate the line count and overguess the necessary columns
                r.setHeight(m_size.height() - delta.height());
                int lines = qMax(1, r.height()/msgLabel->fontMetrics().lineSpacing());
                r.setWidth(3*r.width()/(2*lines)); // generous 50% overhead to account for word wrapping, better the dialog is a bit wider than missing text
            }
        } else { // or completely fixed size
            r.setSize(m_size);
            delta = QSize(0,0);
            marge = QMargins();
        }
        m_size = QSize(0,0); // reset so the global size adjustment doesn't apply
        r.setWidth(r.width() + marge.left()+marge.right());
        r.setHeight(r.height() + marge.top()+marge.bottom());
        // if there's an icon, our new label, regardless of the widths sufficient for one unwrapped line, gets at least its height
        if (icnLabel)
            r.setHeight(qMax(r.height(), icnLabel->height()));
        dlg->setFixedSize(delta + r.size());
        // here's the catch - the WM migth think the pre-showing size is mandatory
        // (probably race condition between window mapping and the size fix and the WM handling client messages)
        // so we briefly wait (100ms is a complete random time) and set the current size again to get the WM up to speed
        for (int ms = 2; ms < 150; ms*=2) {
            QTimer::singleShot(ms, this, [=]() {dlg->setFixedSize(delta + r.size());});
        }
    }
    return 0;
}

QPixmap thumbnail(const QString &path, uint size)
{
    size = qMin(size, 1024u);
    QImage thumb;
    QImageReader thumbReader;
    thumbReader.setFileName(path);
    if (!thumbReader.canRead())
        return QPixmap();

    thumbReader.setQuality(50);
    QSize sz = thumbReader.size();
    QSize origSz = sz;
    bool skipThumbnail = sz.width()*sz.height() < 1920*1200+1;

    if (skipThumbnail) {
        sz.scale(QSize(size,size), Qt::KeepAspectRatio);
        thumbReader.setScaledSize(sz);
    } else {
        QFileInfo info(path);
        QString canonicalPath = info.canonicalFilePath();
        if (canonicalPath.isEmpty())
            canonicalPath = info.absoluteFilePath();
        QUrl url = QUrl::fromLocalFile(canonicalPath);
        QCryptographicHash md5(QCryptographicHash::Md5);
        md5.addData(QFile::encodeName(url.adjusted(QUrl::RemovePassword).url()));

        QString folder;
        uint tSize;
        if (size <= 128) {
            tSize = 128; folder = "normal/";
        } else if (size <= 256) {
            tSize = 256; folder = "large/";
        } else if (size <= 512) {
            tSize = 512; folder = "x-large/";
        } else {
            tSize = 1024; folder = "xx-large/";
        }
        Q_UNUSED(tSize);

        const QString thumbPath = QStandardPaths::writableLocation(QStandardPaths::GenericCacheLocation) +
                                                                    QLatin1String("/thumbnails/") + folder +
                                                                    QString::fromLatin1(md5.result().toHex()) + QStringLiteral(".png");
        QFileInfo tInfo(thumbPath);
        if (tInfo.exists() && info.metadataChangeTime() <= tInfo.lastModified() && info.lastModified() <= tInfo.lastModified()) {
            thumbReader.setFileName(thumbPath);
            sz.scale(QSize(size,size), Qt::KeepAspectRatio);
            thumbReader.setScaledSize(sz);
            if (thumbReader.read(&thumb)) {
                int w = thumb.text("Thumb::Image::Width").toInt();
                int h = thumb.text("Thumb::Image::Height").toInt();
                if (origSz == QSize(w, h))
                    return QPixmap::fromImage(thumb);
            }
        }
        thumbReader.setFileName(path);
//        sz.scale(QSize(tSize,tSize), Qt::KeepAspectRatio); // in case we'll ever store the thumbnail
        sz.scale(QSize(size,size), Qt::KeepAspectRatio);
        thumbReader.setScaledSize(sz);
    }
    if (!thumbReader.read(&thumb))
        return QPixmap();
//    if (skipThumbnail)
        return QPixmap::fromImage(thumb);
}

class DblClckStyle : public QProxyStyle
{
  public:
    int styleHint(StyleHint hint, const QStyleOption *option = nullptr,
                  const QWidget *widget = nullptr, QStyleHintReturn *returnData = nullptr) const override
    {
        if (hint == QStyle::SH_ItemView_ActivateItemOnSingleClick)
            return false;
        return QProxyStyle::styleHint(hint, option, widget, returnData);
    }
};

char Qarma::showFileSelection(const QStringList &args)
{
    QFileDialog *dlg = new QFileDialog;
    QSettings settings("qarma");
    dlg->setViewMode(settings.value("FileDetails", false).toBool() ? QFileDialog::Detail : QFileDialog::List);
    dlg->setFileMode(QFileDialog::ExistingFile);
    dlg->setOption(QFileDialog::DontConfirmOverwrite, false);
    dlg->setProperty("qarma_separator", "|");
    QVariantList l = settings.value("Bookmarks").toList();
    QList<QUrl> bookmarks;
    for (int i = 0; i < l.count(); ++i)
        bookmarks << l.at(i).toUrl();
    if (!bookmarks.isEmpty())
        dlg->setSidebarUrls(bookmarks);
    QStringList mimeFilters;
    for (int i = 0; i < args.count(); ++i) {
        if (args.at(i) == "--filename") {
            QString path = NEXT_ARG;
            if (path.endsWith("/."))
                dlg->setDirectory(path);
            else
                dlg->selectFile(path);
        }
        else if (args.at(i) == "--multiple")
            dlg->setFileMode(QFileDialog::ExistingFiles);
        else if (args.at(i) == "--directory") {
            dlg->setFileMode(QFileDialog::Directory);
            dlg->setOption(QFileDialog::ShowDirsOnly);
        } else if (args.at(i) == "--save") {
            dlg->setFileMode(QFileDialog::AnyFile);
            dlg->setAcceptMode(QFileDialog::AcceptSave);
        }
        else if (args.at(i) == "--separator")
            dlg->setProperty("qarma_separator", NEXT_ARG);
        else if (args.at(i) == "--confirm-overwrite")
            dlg->setOption(QFileDialog::DontConfirmOverwrite);
        else if (args.at(i) == "--file-filter") {
            QString mimeFilter = NEXT_ARG;
            const int idx = mimeFilter.indexOf('|');
            if (idx > -1)
                mimeFilter = mimeFilter.left(idx).trimmed() + " (" + mimeFilter.mid(idx+1).trimmed() + ")";
            mimeFilters << mimeFilter;
        } else if (args.at(i) == "--preview-images") {
            READ_INT(size, UInt, "--preview-images must be followed by a positive number for the thumbnail size");
            dlg->setOption(QFileDialog::DontUseNativeDialog);
            if (QSplitter *splitter = dlg->findChild<QSplitter*>()) {
                qApp->setStyle(new DblClckStyle);
                QLabel *preview = new QLabel(splitter);
                splitter->addWidget(preview);
                connect(dlg, &QFileDialog::currentChanged, [=](const QString &path) {
                    preview->setPixmap(thumbnail(path, size));
                });
            }
        }
        else { WARN_UNKNOWN_ARG("--file-selection") }
    }
    dlg->setNameFilters(mimeFilters);
    SHOW_DIALOG
    return 0;
}

void Qarma::toggleItems(QTreeWidgetItem *item, int column)
{
    if (column)
        return; // not the checkmark

    static bool recursion = false;
    if (recursion)
        return;

    recursion = true;
    QTreeWidget *tw = item->treeWidget();
    for (int i = 0; i < tw->topLevelItemCount(); ++i) {
        QTreeWidgetItem *twi = tw->topLevelItem(i);
        if (twi != item)
            twi->setCheckState(0, Qt::Unchecked);
    }
    recursion = false;
}

static void addItems(QTreeWidget *tw, QStringList &values, bool editable, bool checkable, bool icons)
{
    for (int i = 0; i < values.count(); ) {
        QStringList itemValues;
        for (int j = 0; j < tw->columnCount(); ++j) {
            itemValues << values.at(i++);
            if (i == values.count())
                break;
        }
        QTreeWidgetItem *item = new QTreeWidgetItem(tw, itemValues);
        Qt::ItemFlags flags = item->flags();
        if (editable)
            flags |= Qt::ItemIsEditable;
        if (checkable) {
            flags |= Qt::ItemIsUserCheckable;
            item->setCheckState(0, QVariant(item->text(0)).toBool() ? Qt::Checked : Qt::Unchecked);
        }
        if (icons)
            item->setIcon(0, QPixmap(item->text(0)));
        if (checkable || icons) {
            item->setData(0, Qt::EditRole, item->text(0));
            item->setText(0, QString());
        }
        item->setFlags(flags);
        tw->addTopLevelItem(item);
    }
}

char Qarma::showList(const QStringList &args)
{
    NEW_DIALOG

    QLabel *lbl;
    vl->addWidget(lbl = new QLabel(dlg));

    QTreeWidget *tw;
    vl->addWidget(tw = new QTreeWidget(dlg));
    tw->setSelectionBehavior(QAbstractItemView::SelectRows);
    tw->setSelectionMode(QAbstractItemView::SingleSelection);
    tw->setRootIsDecorated(false);
    tw->setAllColumnsShowFocus(true);

    bool editable(false), checkable(false), exclusive(false), icons(false), ok, needFilter(true);
    QStringList columns;
    QStringList values;
    QList<int> hiddenCols;
    dlg->setProperty("qarma_separator", "|");
    for (int i = 0; i < args.count(); ++i) {
        if (args.at(i) == "--text")
            lbl->setText(labelText(NEXT_ARG));
        else if (args.at(i) == "--multiple")
            tw->setSelectionMode(QAbstractItemView::ExtendedSelection);
        else if (args.at(i) == "--column") {
            columns << NEXT_ARG;
        } else if (args.at(i) == "--editable")
            editable = true;
        else if (args.at(i) == "--hide-header")
            tw->setHeaderHidden(true);
        else if (args.at(i) == "--separator")
            dlg->setProperty("qarma_separator", NEXT_ARG);
        else if (args.at(i) == "--hide-column") {
            int v = NEXT_ARG.toInt(&ok);
            if (ok)
                hiddenCols << v-1;
        } else if (args.at(i) == "--print-column") {
            dlg->setProperty("qarma_print_column", NEXT_ARG);
        } else if (args.at(i) == "--checklist") {
            tw->setSelectionMode(QAbstractItemView::NoSelection);
            tw->setAllColumnsShowFocus(false);
            checkable = true;
        } else if (args.at(i) == "--radiolist") {
            tw->setSelectionMode(QAbstractItemView::NoSelection);
            tw->setAllColumnsShowFocus(false);
            checkable = true;
            exclusive = true;
        } else if (args.at(i) == "--imagelist") {
            icons = true;
        } else if (args.at(i) == "--mid-search") {
            if (needFilter) {
                needFilter = false;
                QLineEdit *filter;
                vl->addWidget(filter = new QLineEdit(dlg));
                filter->setPlaceholderText(tr("Filter"));
                connect (filter, &QLineEdit::textChanged, this, [=](const QString &match){
                    for (int i = 0; i < tw->topLevelItemCount(); ++i)
                        tw->topLevelItem(i)->setHidden(!tw->topLevelItem(i)->text(0).contains(match, Qt::CaseInsensitive));
                });
            }
        } else if (args.at(i) != "--list") {
            values << args.at(i);
        }
    }
    if (values.isEmpty())
        listenToStdIn();

    if (checkable) {
        tw->setCurrentItem(nullptr);
        editable = false;
    }

    tw->setProperty("qarma_list_flags", int(editable | checkable << 1 | icons << 2));

    int columnCount = qMax(columns.count(), 1);
    tw->setColumnCount(columnCount);
    tw->setHeaderLabels(columns);
    foreach (const int &i, hiddenCols)
        tw->setColumnHidden(i, true);

    addItems(tw, values, editable, checkable, icons);

    if (exclusive) {
        connect (tw, SIGNAL(itemChanged(QTreeWidgetItem*, int)), SLOT(toggleItems(QTreeWidgetItem*, int)));
    }
    for (int i = 0; i < columns.count(); ++i)
        tw->resizeColumnToContents(i);

    FINISH_DIALOG(QDialogButtonBox::Ok|QDialogButtonBox::Cancel);
    SHOW_DIALOG
    return 0;
}

void Qarma::notify(const QString message, bool noClose)
{
    if (QDBusConnection::sessionBus().interface()->isServiceRegistered("org.freedesktop.Notifications")) {
        QDBusInterface notifications("org.freedesktop.Notifications", "/org/freedesktop/Notifications", "org.freedesktop.Notifications");
        const QString summary = (message.length() < 32) ? message : message.left(25) + "...";
        QVariantMap hintMap;
        QStringList hintList = m_notificationHints.split(':');
        for (int i = 0; i < hintList.count() - 1; i+=2)
            hintMap.insert(hintList.at(i), hintList.at(i+1));
        QDBusMessage msg = notifications.call("Notify", "Qarma", m_notificationId, "dialog-information", summary, message,
                                              QStringList() /*actions*/, hintMap, m_timeout);
        if (msg.arguments().count())
            m_notificationId = msg.arguments().at(0).toUInt();
        return;
    }

    QMessageBox *dlg = static_cast<QMessageBox*>(m_dialog);
    if (!dlg) {
        dlg = new QMessageBox;
        dlg->setIcon(QMessageBox::Information);
        dlg->setStandardButtons(noClose ? QMessageBox::NoButton : QMessageBox::Ok);
        dlg->setWindowFlags(Qt::ToolTip);
        dlg->setWindowOpacity(0.8);
        if (QLabel *l = dlg->findChild<QLabel*>("qt_msgbox_label")) {
            l->setWordWrap(true);
            if (m_selectableLabel)
                l->setTextInteractionFlags(l->textInteractionFlags()|Qt::TextSelectableByMouse);
        }
    }
    dlg->setText(labelText(message));
    SHOW_DIALOG
    dlg->adjustSize();
    dlg->move(QGuiApplication::screens().at(0)->availableGeometry().topRight() - QPoint(dlg->width() + 20, -20));
}

char Qarma::showNotification(const QStringList &args)
{
    QString message;
    bool listening(false);
    for (int i = 0; i < args.count(); ++i) {
        if (args.at(i) == "--text") {
            message = NEXT_ARG;
        } else if (args.at(i) == "--listen") {
            listening = true;
            listenToStdIn();
        } else if (args.at(i) == "--hint") {
            m_notificationHints = NEXT_ARG;
        } else if (args.at(i) == "--selectable-labels") {
            m_selectableLabel = true;
        } else { WARN_UNKNOWN_ARG("--notification") }
    }
    if (!message.isEmpty())
        notify(message, listening);
    if (!(listening || m_dialog))
        QMetaObject::invokeMethod(this, "quit", Qt::QueuedConnection);
    return 0;
}

static QFile *gs_stdin = 0;

void Qarma::finishProgress()
{
    Q_ASSERT(m_type == Progress);
    QProgressDialog *dlg = static_cast<QProgressDialog*>(m_dialog);
    if (dlg->property("qarma_autoclose").toBool())
        QTimer::singleShot(250, this, SLOT(quit()));
    else {
        dlg->setRange(0, 101);
        dlg->setValue(100);
        disconnect (dlg, SIGNAL(canceled()), dlg, SLOT(reject()));
        connect (dlg, SIGNAL(canceled()), dlg, SLOT(accept()));
        dlg->setCancelButtonText(m_ok.isNull() ? tr("Ok") : m_ok);
        if (QPushButton *btn = dlg->findChild<QPushButton*>())
            btn->show();
    }
}

void Qarma::readStdIn()
{
    if (!gs_stdin->isOpen())
        return;
    QSocketNotifier *notifier = qobject_cast<QSocketNotifier*>(sender());
    if (notifier)
        notifier->setEnabled(false);

    QByteArray ba = (m_type == TextInfo || m_type == List) ? gs_stdin->readAll() : gs_stdin->readLine();
    if (ba.isEmpty() && notifier) {
        gs_stdin->close();
//         gs_stdin->deleteLater(); // hello segfault...
//         gs_stdin = NULL;
        notifier->deleteLater();
        if (m_type == Progress)
            finishProgress();
        return;
    }

    static QString cachedText;
    QString newText = QString::fromLocal8Bit(ba);
    if (newText.isEmpty() && cachedText.isEmpty()) {
        if (notifier)
            notifier->setEnabled(true);
        return;
    }

    QStringList input;
    if (m_type != TextInfo) {
        if (newText.endsWith('\n'))
            newText.resize(newText.length()-1);
        input = newText.split('\n');
    }
    if (m_type == Progress) {
        QProgressDialog *dlg = static_cast<QProgressDialog*>(m_dialog);

        const int oldValue = dlg->value();
        bool ok;
        foreach (QString line, input) {
            if (line.startsWith('#')) {
                dlg->setLabelText(labelText(line.mid(1)));
            } else {
                static const QRegularExpression nondigit("[^0-9]");
                int u = line.section(nondigit,0,0).toInt(&ok);
                if (ok)
                    dlg->setValue(qMin(100,u));
            }
        }

        if (dlg->value() == 100) {
            finishProgress();
        } else if (oldValue == 100) {
            disconnect (dlg, SIGNAL(canceled()), dlg, SLOT(accept()));
            connect (dlg, SIGNAL(canceled()), dlg, SLOT(reject()));
            dlg->setCancelButtonText(m_cancel.isNull() ? tr("Cancel") : m_cancel);
        } else if (dlg->property("qarma_eta").toBool()) {
            static QDateTime starttime;
            if (starttime.isNull()) {
                starttime = QDateTime::currentDateTime();
            } else if (dlg->value() > 0) {
                const qint64 secs = starttime.secsTo(QDateTime::currentDateTime());
                QString eta = QTime(0,0,0).addSecs(100 * secs / dlg->value() - secs).toString();
                foreach (QWidget *w, dlg->findChildren<QWidget*>())
                    w->setToolTip(eta);
            }
        }
    } else if (m_type == TextInfo) {
        if (QTextEdit *te = m_dialog->findChild<QTextEdit*>()) {
            cachedText += newText;
            static QPropertyAnimation *animator = NULL;
            if (!animator || animator->state() != QPropertyAnimation::Running) {
                const int oldValue = te->verticalScrollBar() ? te->verticalScrollBar()->value() : 0;
                if (te->property("qarma_html").toBool())
                    te->setHtml(te->toHtml() + cachedText);
                else
                    te->setPlainText(te->toPlainText() + cachedText);
                cachedText.clear();
                if (te->verticalScrollBar() && te->property("qarma_autoscroll").toBool()) {
                    te->verticalScrollBar()->setValue(oldValue);
                    if (!animator) {
                        animator = new QPropertyAnimation(te->verticalScrollBar(), "value", this);
                        animator->setEasingCurve(QEasingCurve::InOutCubic);
                        connect(animator, SIGNAL(finished()), SLOT(readStdIn()));
                    }
                    const int diff = te->verticalScrollBar()->maximum() - oldValue;
                    if (diff > 0) {
                        animator->setDuration(qMin(qMax(200, diff), 2500));
                        animator->setEndValue(te->verticalScrollBar()->maximum());
                        animator->start();
                    }
                }
            }
        }
    } else if (m_type == Notification) {
        bool userNeedsHelp = true;
        foreach (QString line, input) {
            int split = line.indexOf(':');
            if (split < 0)
                continue;
            if (line.left(split) == "icon") {
                userNeedsHelp = false;
                // TODO: some icon filename, seems gnome specific and i've no idea how to handle this atm.
                qWarning("'icon' command not yet supported - if you know what this is supposed to do, please file a bug");
            } else if (line.left(split) == "message" || line.left(split) == "tooltip") {
                userNeedsHelp = false;
                notify(line.mid(split+1));
            } else if (line.left(split) == "visible") {
                userNeedsHelp = false;
                if (m_dialog)
                    m_dialog->setVisible(line.mid(split+1).trimmed().compare("false", Qt::CaseInsensitive) &&
                                         line.mid(split+1).trimmed().compare("0", Qt::CaseInsensitive));
                else
                    qWarning("'visible' command only supported for failsafe dialog notification");
            } else if (line.left(split) == "hints") {
                m_notificationHints = line.mid(split+1);
            }
        }
        if (userNeedsHelp)
            qDebug() << "icon: <filename>\nmessage: <UTF-8 encoded text>\ntooltip: <UTF-8 encoded text>\nvisible: <true|false>";
    } else if (m_type == List) {
        if (QTreeWidget *tw = m_dialog->findChild<QTreeWidget*>()) {
            const int twflags = tw->property("qarma_list_flags").toInt();
            addItems(tw, input, twflags & 1, twflags & 1<<1, twflags & 1<<2);
        }
    } else if (m_type == Dzen) {
        QLabel *header = m_dialog->findChild<QLabel*>("header");
        QLabel *body = m_dialog->findChild<QLabel*>("body");
        if (body) {
            const int lines = body->property("lines").toInt();
            const bool unified = m_dialog->property("unified").toBool();
            for (int i = unified ? 0 : 1; i < lines; ++i)
                input.append(QString::fromLocal8Bit(gs_stdin->readLine()));
//            qDebug() << input;
            if (header->text().isEmpty() || unified) {
//                qDebug() << header->text().isEmpty() << m_dialog->property("unified").toBool() << "set header text";
                header->setText(input.takeFirst());
            }
            if (!input.isEmpty()) {
//                qDebug() << "have more input";
                QStringList oldBody = body->text().split('\n');
//                qDebug() << oldBody;
                oldBody.append(input);
//                qDebug() << "=>" << oldBody;
                const int idx = qMax(0, oldBody.count() - lines);
//                qDebug() << "==>" << oldBody.mid(idx);
                body->setText(oldBody.mid(idx).join('\n'));
            }
        } else {
//            qDebug() << "just set header";
            header->setText(input.constLast());
        }
    }
    if (notifier)
        notifier->setEnabled(true);
    QCoreApplication::processEvents();
}

void Qarma::listenToStdIn()
{
    if (gs_stdin)
        return;
    gs_stdin = new QFile;
    if (gs_stdin->open(stdin, QIODevice::ReadOnly)) {
        QSocketNotifier *snr = new QSocketNotifier(gs_stdin->handle(), QSocketNotifier::Read, gs_stdin);
        connect (snr, SIGNAL(activated(int)), SLOT(readStdIn()));
    } else {
        delete gs_stdin;
        gs_stdin = NULL;
    }
}

char Qarma::showProgress(const QStringList &args)
{
    QProgressDialog *dlg = new QProgressDialog;
    dlg->setRange(0, 101);
    for (int i = 0; i < args.count(); ++i) {
        if (args.at(i) == "--text")
            dlg->setLabelText(labelText(NEXT_ARG));
        else if (args.at(i) == "--percentage")
            dlg->setValue(NEXT_ARG.toUInt());
        else if (args.at(i) == "--pulsate")
            dlg->setRange(0,0);
        else if (args.at(i) == "--auto-close")
            dlg->setProperty("qarma_autoclose", true);
        else if (args.at(i) == "--auto-kill")
            dlg->setProperty("qarma_autokill_parent", true);
        else if (args.at(i) == "--no-cancel") {
            if (QPushButton *btn = dlg->findChild<QPushButton*>())
                btn->hide();
        } else if (args.at(i) == "--time-remaining") {
            dlg->setProperty("qarma_eta", true);
        }
        else { WARN_UNKNOWN_ARG("--progress") }
    }

    listenToStdIn();
    if (dlg->maximum() == 0) { // pulsate, quit as stdin closes
        connect (gs_stdin, SIGNAL(aboutToClose()), this, SLOT(finishProgress()));
    }

    if (!m_cancel.isNull())
        dlg->setCancelButtonText(m_cancel);
    connect (dlg, SIGNAL(canceled()), dlg, SLOT(reject()));
    SHOW_DIALOG
    return 0;
}

void Qarma::printInteger(int v)
{
    printf("%d\n", v);
}

char Qarma::showScale(const QStringList &args)
{
    NEW_DIALOG

    QHBoxLayout *hl = new QHBoxLayout;
    QLabel *lbl, *val;
    QSlider *sld;

    vl->addWidget(lbl = new QLabel("Enter a value", dlg));
    vl->addLayout(hl);
    hl->addWidget(sld = new QSlider(Qt::Horizontal, dlg));
    hl->addWidget(val = new QLabel(dlg));
    connect (sld, SIGNAL(valueChanged(int)), val, SLOT(setNum(int)));

    FINISH_DIALOG(QDialogButtonBox::Ok|QDialogButtonBox::Cancel);

    sld->setRange(0,100);
    val->setNum(0);

    bool ok;
    for (int i = 0; i < args.count(); ++i) {
        if (args.at(i) == "--text")
            lbl->setText(labelText(NEXT_ARG));
        else if (args.at(i) == "--value")
            sld->setValue(NEXT_ARG.toInt());
        else if (args.at(i) == "--min-value") {
            int v = NEXT_ARG.toInt(&ok);
            if (ok)
                sld->setMinimum(v);
        } else if (args.at(i) == "--max-value") {
            int v = NEXT_ARG.toInt(&ok);
            if (ok)
                sld->setMaximum(v);
        } else if (args.at(i) == "--step") {
            int u = NEXT_ARG.toInt(&ok);
            if (ok)
                sld->setSingleStep(u);
        } else if (args.at(i) == "--print-partial") {
            connect (sld, SIGNAL(valueChanged(int)), SLOT(printInteger(int)));
        } else if (args.at(i) == "--hide-value") {
            val->hide();
        } else { WARN_UNKNOWN_ARG("--scale") }
    }
    SHOW_DIALOG
    return 0;
}

char Qarma::showText(const QStringList &args)
{
    NEW_DIALOG

    QTextBrowser *te;
    vl->addWidget(te = new QTextBrowser(dlg));
    te->setReadOnly(true);
    te->setOpenExternalLinks(true);
    QCheckBox *cb(NULL);

    QString filename;
    bool html(false), plain(false), onlyMarkup(false), url(false);
    for (int i = 0; i < args.count(); ++i) {
        if (args.at(i) == "--filename") {
            filename = NEXT_ARG;
        } else if (args.at(i) == "--url") {
            filename = NEXT_ARG;
            url = true;
        }
        else if (args.at(i) == "--editable")
            te->setReadOnly(false);
        else if (args.at(i) == "--font") {
            te->setFont(QFont(NEXT_ARG));
        } else if (args.at(i) == "--checkbox") {
            vl->addWidget(cb = new QCheckBox(NEXT_ARG, dlg));
        } else if (args.at(i) == "--auto-scroll") {
            te->setProperty("qarma_autoscroll", true);
        } else if (args.at(i) == "--html") {
            html = true;
            te->setProperty("qarma_html", true);
        } else if (args.at(i) == "--plain") {
            plain = true;
        } else if (args.at(i) == "--no-interaction") {
            onlyMarkup = true;
        } else { WARN_UNKNOWN_ARG("--text-info") }
    }

    if (html) {
        te->setReadOnly(true);
        te->setTextInteractionFlags(onlyMarkup ? Qt::TextSelectableByMouse : Qt::TextBrowserInteraction);
    }
    if (te->isReadOnly()) {
        QPalette pal = te->viewport()->palette();
        for (int i = 0; i < 3; ++i) { // Disabled, Active, Inactive, Normal
            QPalette::ColorGroup cg = (QPalette::ColorGroup)i;
            pal.setColor(cg, QPalette::Base, pal.color(cg, QPalette::Window));
            pal.setColor(cg, QPalette::Text, pal.color(cg, QPalette::WindowText));
        }
        te->viewport()->setPalette(pal);
        te->viewport()->setAutoFillBackground(false);
        te->setFrameStyle(QFrame::NoFrame);
    }

    if (filename.isNull()) {
        listenToStdIn();
    } else if (url) {
        QProcess *curl = new QProcess;
        connect(curl, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), [=]() {
            te->setText(QString::fromLocal8Bit(curl->readAllStandardOutput()));
            delete curl;
        });
        curl->start("curl", QStringList() << "-L" << "-s" << filename);
    } else {
        QFile file(filename);
        if (file.open(QIODevice::ReadOnly)) {
            if (html)
                te->setHtml(QString::fromLocal8Bit(file.readAll()));
            else if (plain)
                te->setPlainText(QString::fromLocal8Bit(file.readAll()));
            else
                te->setText(QString::fromLocal8Bit(file.readAll()));
            file.close();
        }
    }

    FINISH_DIALOG(QDialogButtonBox::Ok|QDialogButtonBox::Cancel);

    if (cb) {
        QPushButton *btn = btns->button(QDialogButtonBox::Ok);
        btn->setEnabled(false);
        connect(cb, SIGNAL(toggled(bool)), btn, SLOT(setEnabled(bool)));
    }

    SHOW_DIALOG
    return 0;
}

char Qarma::showColorSelection(const QStringList &args)
{
    QColorDialog *dlg = new QColorDialog;
    QVariantList l = QSettings("qarma").value("CustomPalette").toList();
    for (int i = 0; i < l.count() && i < dlg->customCount(); ++i)
        dlg->setCustomColor(i, QColor(l.at(i).toUInt()));
    for (int i = 0; i < args.count(); ++i) {
        if (args.at(i) == "--color") {
            dlg->setCurrentColor(QColor(NEXT_ARG));
        } else if (args.at(i) == "--show-palette") {
            qWarning("The show-palette parameter is not supported by qarma. Sorry.");
            void(0);
        } else if (args.at(i) == "--custom-palette") {
            if (i+1 < args.count()) {
                QString path = NEXT_ARG;
                QFile file(path);
                if (file.open(QIODevice::ReadOnly)) {
                    QStringList pal = QString::fromLocal8Bit(file.readAll()).split('\n');
                    int r, g, b; bool ok; int idx = 0;
                    for (const QString &line : pal) {
                        if (idx > 47) break; // sorry :(
                        QStringList color = line.split(QRegularExpression("\\s+"), SKIP_EMPTY);
                        if (color.count() < 3) continue;
                        r = color.at(0).toInt(&ok); if (!ok) continue;
                        g = color.at(1).toInt(&ok); if (!ok) continue;
                        b = color.at(2).toInt(&ok); if (!ok) continue;
                        dlg->setStandardColor(idx++, QColor(r,g,b));
                    }
                    while (idx < 48) {
                        dlg->setStandardColor(idx++, Qt::black);
                    }
                    file.close();
                } else {
                    qWarning("Cannot read %s", path.toLocal8Bit().constData());
                }
            } else {
                qWarning("You have to provide a gimp palette (*.gpl)");
            }
        }{ WARN_UNKNOWN_ARG("--color-selection") }
    }
    SHOW_DIALOG
    return 0;
}

char Qarma::showFontSelection(const QStringList &args)
{
    QFontDialog *dlg = new QFontDialog;
    QString pattern = "%1-%2:%3:%4";
    QString sample = "The quick brown fox jumps over the lazy dog";
    for (int i = 0; i < args.count(); ++i) {
        if (args.at(i) == "--type") {
            QStringList types = NEXT_ARG.split(',');
            QFontDialog::FontDialogOptions opts;
            for (const QString &type : types) {
                if (type == "vector")   opts |= QFontDialog::ScalableFonts;
                if (type == "bitmap")   opts |= QFontDialog::NonScalableFonts;
                if (type == "fixed")    opts |= QFontDialog::MonospacedFonts;
                if (type == "variable") opts |= QFontDialog::ProportionalFonts;
            }
            if (opts) // https://bugreports.qt.io/browse/QTBUG-93473
                dlg->setOptions(opts);
            dlg->setCurrentFont(QFont()); // also works around the bug :P
        } else if (args.at(i) == "--pattern") {
            pattern = NEXT_ARG;
            if (!pattern.contains("%1"))
                qWarning("The output pattern doesn't include a placeholder for the font name...");
        } else if (args.at(i) == "--sample") {
            sample = NEXT_ARG;
        } { WARN_UNKNOWN_ARG("--font-selection") }
    }
    if (QLineEdit *smpl = dlg->findChild<QLineEdit*>("qt_fontDialog_sampleEdit"))
        smpl->setText(sample);
    dlg->setProperty("qarma_fontpattern", pattern);
    SHOW_DIALOG
    return 0;
}

static void buildList(QTreeWidget **tree, QStringList &values, QStringList &columns, bool &showHeader)
{
    QTreeWidget *tw = *tree;

    if (!tw)
        return;

    int columnCount = columns.count();
    tw->setHeaderHidden(!showHeader);
    if (columns.count()) {
        tw->setColumnCount(columns.count());
        tw->setHeaderLabels(columns);
    } else {
        columnCount = 1;
    }


    for (int i = 0; i < values.count(); ) {
        QStringList itemValues;
        for (int j = 0; j < columnCount; ++j) {
            itemValues << values.at(i++);
            if (i == values.count())
                break;
        }
        tw->addTopLevelItem(new QTreeWidgetItem(tw, itemValues));
    }

    for (int i = 0; i < columns.count(); ++i)
        tw->resizeColumnToContents(i);

    values.clear();
    columns.clear();
    showHeader = false;
    *tree = NULL;
}

char Qarma::showForms(const QStringList &args)
{
    NEW_DIALOG
    dlg->setProperty("qarma_separator", "|");

    QLabel *label;
    vl->addWidget(label = new QLabel(dlg));
    QFont fnt = label->font();
    fnt.setBold(true);
    label->setFont(fnt);

    QFormLayout *fl;
    vl->addLayout(fl = new QFormLayout);

    QTreeWidget *lastList = NULL;
    QStringList lastListValues, lastListColumns, lastComboValues;
    bool lastListHeader(false);
    QComboBox *lastCombo = NULL;
    QLineEdit *lastEntry = nullptr;
    QString lastEntryValue;
    for (int i = 0; i < args.count(); ++i) {
        if (args.at(i) == "--add-entry") {
            fl->addRow(NEXT_ARG, lastEntry = new QLineEdit(dlg));
            lastEntry->setText(lastEntryValue);
            lastEntry->setPlaceholderText(lastEntryValue);
        } else if (args.at(i) == "--entry-value") {
            lastEntryValue = NEXT_ARG;
            if (lastEntry) {
                lastEntry->setText(lastEntryValue);
                lastEntry->setPlaceholderText(lastEntryValue);
                lastEntryValue.clear();
                lastEntry = nullptr;
            }
        } else if (args.at(i) == "--add-password") {
            QLineEdit *le;
            fl->addRow(NEXT_ARG, le = new QLineEdit(dlg));
            le->setEchoMode(QLineEdit::Password);
        } else if (args.at(i) == "--add-calendar") {
            fl->addRow(NEXT_ARG, new QCalendarWidget(dlg));
        } else if (args.at(i) == "--add-list") {
            buildList(&lastList, lastListValues, lastListColumns, lastListHeader);
            fl->addRow(NEXT_ARG, lastList = new QTreeWidget(dlg));
        } else if (args.at(i) == "--list-values") {
            lastListValues = NEXT_ARG.split('|');
        } else if (args.at(i) == "--column-values") {
            lastListColumns = NEXT_ARG.split('|');
        } else if (args.at(i) == "--add-combo") {
            fl->addRow(NEXT_ARG, lastCombo = new QComboBox(dlg));
            lastCombo->addItems(lastComboValues);
            lastComboValues.clear();
        } else if (args.at(i) == "--combo-values") {
            lastComboValues = NEXT_ARG.split('|');
            if (lastCombo) {
                lastCombo->addItems(lastComboValues);
                lastComboValues.clear();
                lastCombo = NULL;
            }
        } else if (args.at(i) == "--show-header") {
            lastListHeader = true;
        } else if (args.at(i) == "--text") {
            label->setText(labelText(NEXT_ARG));
        } else if (args.at(i) == "--separator") {
            dlg->setProperty("qarma_separator", NEXT_ARG);
        } else if (args.at(i) == "--forms-date-format") {
            dlg->setProperty("qarma_date_format", NEXT_ARG);
        } else if (args.at(i) == "--add-checkbox") {
            fl->addRow(new QCheckBox(NEXT_ARG, dlg));
        } else { WARN_UNKNOWN_ARG("--forms") }
    }
    buildList(&lastList, lastListValues, lastListColumns, lastListHeader);


    FINISH_DIALOG(QDialogButtonBox::Ok|QDialogButtonBox::Cancel);
    SHOW_DIALOG
    return 0;
}

static QFont xftFont(const QString &pattern)
{
    QFont font;
    if (pattern.isEmpty())
        return font;
    /*
    plausible subset of https://keithp.com/~keithp/render/Xft.tutorial 
    <family>-<size>:<name>=<value>...
    */
    const QStringList fields = pattern.split(':');
    bool ok = false;
    int size = -1;
    const int split = fields.at(0).lastIndexOf('-');
    if (split > 0) size = fields.at(0).mid(split + 1).toUInt(&ok);
    font = ok ? QFont(fields.at(0).mid(0, split), size) : QFont(fields.at(0));
    
    for (int i = 1; i < fields.count(); ++i) {
        if (fields.at(i) == "light") font.setWeight(QFont::Light);
        else if (fields.at(i) == "medium") font.setWeight(QFont::Medium);
        else if (fields.at(i) == "demibold") font.setWeight(QFont::DemiBold);
        else if (fields.at(i) == "bold") font.setWeight(QFont::Bold);
        else if (fields.at(i) == "black") font.setWeight(QFont::Black);
        else if (fields.at(i) == "roman") font.setItalic(false);
        else if (fields.at(i) == "italic") font.setItalic(true);
        else if (fields.at(i) == "oblique") font.setItalic(true);
        else if (fields.at(i).contains('=')) {
            QStringList field = fields.at(i).split('=');
            if (field.count() < 2) continue;
            if (field.at(0) == "family") font.setFamily(field.at(1));
            else if (field.at(0) == "weight") font.setWeight(QFont::Weight(field.at(1).toUInt()));
            else if (field.at(0) == "size") font.setPointSizeF(field.at(1).toFloat());
            else if (field.at(0) == "pixelsize") font.setPixelSize(field.at(1).toUInt());
            else if (field.at(0) == "slant") {
                if (field.at(0) == "italic" || field.at(0) == "oblique")
                    font.setItalic(true);
            }
        }
    }
    return font;
}

char Qarma::showDzen(const QStringList &args)
{
//    m_popup = true;
    NEW_DIALOG
    vl->setContentsMargins(0, 0, 0, 0);
    QLabel *header, *body;
    vl->addWidget(header = new QLabel(dlg));
    header->setObjectName("header");
    header->setAlignment(Qt::AlignCenter);
    header->setMargin(6);
    vl->addWidget(body = new QLabel(dlg));
    body->setObjectName("body");
    body->setMargin(8);
   
    
    QPalette pal = dlg->palette();
    if (m_pos.x() == INT_MAX)
        m_pos = QPoint(0,0);

    int suicide = 0;

    for (int i = 0; i < args.count(); ++i) {
        if (args.at(i) == "-fg") {
            QColor c(NEXT_ARG);
            for (int i = 0; i < 3; ++i) { // Disabled, Active, Inactive, Normal
                QPalette::ColorGroup cg = (QPalette::ColorGroup)i;
                pal.setColor(cg, QPalette::Text, c);
                pal.setColor(cg, QPalette::WindowText, c);
            }
        } else if (args.at(i) == "-bg") {
            QColor c(NEXT_ARG);
            for (int i = 0; i < 3; ++i) { // Disabled, Active, Inactive, Normal
                QPalette::ColorGroup cg = (QPalette::ColorGroup)i;
                pal.setColor(cg, QPalette::Base, c);
                pal.setColor(cg, QPalette::Window, c);
            }
        } else if (args.at(i) == "-fn") {
            dlg->setFont(xftFont(NEXT_ARG));
        } else if (args.at(i) == "-ta" || args.at(i) == "-sa") {
            QLabel *label = args.at(i) == "-sa" ? body : header;
            QString align = NEXT_ARG;
            if (align.startsWith('l', Qt::CaseInsensitive))
                label->setAlignment(Qt::AlignLeft|Qt::AlignVCenter);
            else if (align.startsWith('c', Qt::CaseInsensitive))
                label->setAlignment(Qt::AlignCenter);
            else if (align.startsWith('r', Qt::CaseInsensitive))
                label->setAlignment(Qt::AlignRight|Qt::AlignVCenter);
        } else if (args.at(i) == "-l") {
            READ_INT(lines, UInt, "-l(ines) expects a positive integer as next value");
            body->setProperty("lines", lines);
        } else if (args.at(i) == "-x") {
            READ_INT(x, Int, "-x expects a positive integer as next value");
            m_pos.setX(x);
        } else if (args.at(i) == "-y") {
            READ_INT(y, Int, "-y expects a positive integer as next value");
            m_pos.setY(y);
        } else if (args.at(i) == "-w") {
            READ_INT(w, UInt, "-w expects a positive integer as next value");
            m_size.setWidth(w);
        } else if (args.at(i) == "-p") {
            suicide = -1;
            if (i+1 < args.count()) {
                bool ok;
                suicide = QString(args.at(i+1)).toUInt(&ok);
                if (ok)
                    ++i;
                else
                    suicide = -1;
            }
        } else if (args.at(i) == "-u") {
            dlg->setProperty("unified", true);
        } else { WARN_UNKNOWN_ARG("--dzen") }
    }
    if (!body->property("lines").isValid())
        delete body;
    dlg->setPalette(pal);
//       62     -tw     title window width
//       65     -e      events and actions, see (2)
//       66     -m      menu mode, see (3)
//       72     -h      line height (default: fontheight + 2 pixels)
//       74     -xs     number of Xinerama screen
    
    listenToStdIn();
    if (suicide > -1)
        connect (gs_stdin, &QFile::aboutToClose, this, [=](){
                    QTimer::singleShot(suicide*1000, this, SLOT(quit()));
                });
    dlg->setWindowFlags(Qt::BypassWindowManagerHint);
    SHOW_DIALOG
    return 0;
}

QString Qarma::labelText(const QString &s) const
{
    // zenity uses pango markup, https://developer.gnome.org/pygtk/stable/pango-markup-language.html
    // This near-html-subset isn't really compatible w/ Qt's html subset and we end up
    // w/ a weird mix of ASCII escape codes and html tags
    // the below is NOT a perfect translation

    // known "caveats"
    // pango termiantes the string for "\0" (we do not - atm)
    // pango inserts some control char for "\f", but that's not reasonably handled by gtk label (so it's ignored here)
    if (m_zenity) {
        QString r = s;
        // First replace backslashes with alarms to avoid false positives below.
        // The alarm is treated as invalid and not supported by zenity/pango either
        r.replace("\\\\", "\a") \
         .replace("\\n", "<br>").replace("\\t", "&nbsp;&nbsp;&nbsp;") \
         .replace("\\r", "<br>");
        int idx = 0;
        while (true) {
            static const QRegularExpression octet("\\\\([0-9]{1,3})");
            idx = r.indexOf(octet, idx);
            if (idx < 0)
                break;
            int sz = 0;
            while (sz < 3 && r.at(idx+sz+1).isDigit())
                ++sz;
            r.replace(idx, sz+1, QChar(r.mid(idx+1, sz).toUInt(nullptr, 8)));
        }
        r.remove("\\").replace(("\a"), "\\");
        return r;
    }
    return s;
}


void Qarma::printHelp(const QString &category)
{
    static HelpDict helpDict;
    if (helpDict.isEmpty()) {
        helpDict["help"] = CategoryHelp(tr("Help options"), HelpList() <<
                            Help("-h, --help", tr("Show help options")) <<
                            Help("--help-all", tr("Show all help options")) <<
                            Help("--help-general", tr("Show general options")) <<
                            Help("--help-calendar", tr("Show calendar options")) <<
                            Help("--help-entry", tr("Show text entry options")) <<
                            Help("--help-error", tr("Show error options")) <<
                            Help("--help-info", tr("Show info options")) <<
                            Help("--help-file-selection", tr("Show file selection options")) <<
                            Help("--help-list", tr("Show list options")) <<
                            Help("--help-notification", tr("Show notification icon options")) <<
                            Help("--help-progress", tr("Show progress options")) <<
                            Help("--help-question", tr("Show question options")) <<
                            Help("--help-warning", tr("Show warning options")) <<
                            Help("--help-scale", tr("Show scale options")) <<
                            Help("--help-text-info", tr("Show text information options")) <<
                            Help("--help-color-selection", tr("Show color selection options")) <<
                            Help("--help-font-selection", tr("Show font selection options")) <<
                            Help("--help-password", tr("Show password dialog options")) <<
                            Help("--help-forms", tr("Show forms dialog options")) <<
                            Help("--help-dzen", tr("Show dzen options")) <<
                            Help("--help-misc", tr("Show miscellaneous options")) <<
                            Help("--help-qt", tr("Show Qt Options")));
        helpDict["general"] = CategoryHelp(tr("General options"), HelpList() <<
                            Help("--title=TITLE", tr("Set the dialog title")) <<
                            Help("--window-icon=ICONPATH", tr("Set the window icon")) <<
                            Help("--width=WIDTH", tr("Set the width") + tr(" (not entirely deterministic for message dialogs)")) <<
                            Help("--height=HEIGHT", tr("Set the height") + tr(" (not entirely deterministic for message dialogs)")) <<
                            Help("--pos=[+-]x[(+-)y]", "QARMA ONLY! " + tr("Set the position")) <<
                            Help("--timeout=TIMEOUT", tr("Set dialog timeout in seconds")) <<
                            Help("--ok-label=TEXT", tr("Sets the label of the Ok button")) <<
                            Help("--cancel-label=TEXT", tr("Sets the label of the Cancel button")) <<
                            Help("--modal", tr("Set the modal hint")) <<
                            Help("--popup", "QARMA ONLY! " + tr("Open dialog as unframed and trasient popup window")) <<
                            Help("--attach=WINDOW", tr("Set the parent window to attach to")));
        helpDict["calendar"] = CategoryHelp(tr("Calendar options"), HelpList() <<
                            Help("--text=TEXT", tr("Set the dialog text")) <<
                            Help("--day=DAY", tr("Set the calendar day")) <<
                            Help("--month=MONTH", tr("Set the calendar month")) <<
                            Help("--year=YEAR", tr("Set the calendar year")) <<
                            Help("--timeout=TIMEOUT", tr("Set dialog timeout in seconds")) <<
                            Help("--date-format=PATTERN", tr("Set the format for the returned date")));
        helpDict["entry"] = CategoryHelp(tr("Text entry options"), HelpList() <<
                            Help("--text=TEXT", tr("Set the dialog text")) <<
                            Help("--entry-text=TEXT", tr("Set the entry text")) <<
                            Help("--hide-text", tr("Hide the entry text")) <<
                            Help("--values=v1|v2|v3|...", "QARMA ONLY! " + tr("Offer preset values to pick from")) <<
                            Help("--int=integer", "QARMA ONLY! " + tr("Integer input only, preset given value")) <<
                            Help("--float=floating_point", "QARMA ONLY! " + tr("Floating point input only, preset given value")));
        helpDict["error"] = CategoryHelp(tr("Error options"), HelpList() <<
                            Help("--text=TEXT", tr("Set the dialog text")) <<
                            Help("--icon-name=ICON-NAME", tr("Set the dialog icon")) <<
                            Help("--no-wrap", tr("Do not enable text wrapping")) <<
                            Help("--no-markup", tr("Do not enable html markup")) <<
                            Help("--ellipsize", tr("Do wrap text, zenity has a rather special problem here")) <<
                            Help("--selectable-labels", "QARMA ONLY! " + tr("Allow to select text for copy and paste")));
        helpDict["info"] = CategoryHelp(tr("Info options"), HelpList() <<
                            Help("--text=TEXT", tr("Set the dialog text")) <<
                            Help("--icon-name=ICON-NAME", tr("Set the dialog icon")) <<
                            Help("--no-wrap", tr("Do not enable text wrapping")) <<
                            Help("--no-markup", tr("Do not enable html markup")) <<
                            Help("--ellipsize", tr("Do wrap text, zenity has a rather special problem here")) <<
                            Help("--selectable-labels", "QARMA ONLY! " + tr("Allow to select text for copy and paste")));
        helpDict["file-selection"] = CategoryHelp(tr("File selection options"), HelpList() <<
                            Help("--filename=FILENAME", tr("Set the filename")) <<
                            Help("--multiple", tr("Allow multiple files to be selected")) <<
                            Help("--directory", tr("Activate directory-only selection")) <<
                            Help("--save", tr("Activate save mode")) <<
                            Help("--separator=SEPARATOR", tr("Set output separator character")) <<
                            Help("--confirm-overwrite", tr("Confirm file selection if filename already exists")) <<
                            Help("--file-filter=NAME | PATTERN1 PATTERN2 ...", tr("Sets a filename filter")) <<
                            Help("--preview-images=SIZE", "QARMA ONLY! " + tr("Show image thumbnails of SIZE")));
        helpDict["list"] = CategoryHelp(tr("List Command\n  %1 --list [Options] [Item1 ...]\nList Options").arg(applicationName()), HelpList() <<
                            Help("--text=TEXT", tr("Set the dialog text")) <<
                            Help("--column=COLUMN", tr("Set the column header")) <<
                            Help("--checklist", tr("Use check boxes for first column")) <<
                            Help("--radiolist", tr("Use radio buttons for first column")) <<
                            Help("--imagelist", tr("Use an image for first column")) <<
                            Help("--separator=SEPARATOR", tr("Set output separator character")) <<
                            Help("--multiple", tr("Allow multiple rows to be selected")) <<
                            Help("--editable", tr("Allow changes to text")) <<
                            Help("--print-column=NUMBER", tr("Print a specific column (Default is 1. 'ALL' can be used to print all columns)")) <<
                            Help("--hide-column=NUMBER", tr("Hide a specific column")) <<
                            Help("--hide-header", tr("Hides the column headers")) <<
                            Help("--mid-search", tr("Change list default search function searching for text in the middle, not on the beginning")));
        helpDict["notification"] = CategoryHelp(tr("Notification icon options"), HelpList() <<
                            Help("--text=TEXT", tr("Set the dialog text")) <<
                            Help("--listen", tr("Listen for commands on stdin")) <<
                            Help("--hint=TEXT", tr("Set the notification hints")) <<
                            Help("--selectable-labels", "QARMA ONLY! " + tr("Allow to select text for copy and paste")));
        helpDict["progress"] = CategoryHelp(tr("Progress options"), HelpList() <<
                            Help("--text=TEXT", tr("Set the dialog text")) <<
                            Help("--percentage=PERCENTAGE", tr("Set initial percentage")) <<
                            Help("--pulsate", tr("Pulsate progress bar")) <<
                            Help("--auto-close", tr("Dismiss the dialog when 100% has been reached")) <<
                            Help("--auto-kill", tr("Kill parent process if Cancel button is pressed")) <<
                            Help("--no-cancel", tr("Hide Cancel button")));
        helpDict["question"] = CategoryHelp(tr("Question options"), HelpList() <<
                            Help("--text=TEXT", tr("Set the dialog text")) <<
                            Help("--icon-name=ICON-NAME", tr("Set the dialog icon")) <<
                            Help("--no-wrap", tr("Do not enable text wrapping")) <<
                            Help("--no-markup", tr("Do not enable html markup")) <<
                            Help("--default-cancel", tr("Give cancel button focus by default")) <<
                            Help("--ellipsize", tr("Do wrap text, zenity has a rather special problem here")) <<
                            Help("--selectable-labels", "QARMA ONLY! " + tr("Allow to select text for copy and paste")));
        helpDict["warning"] = CategoryHelp(tr("Warning options"), HelpList() <<
                            Help("--text=TEXT", tr("Set the dialog text")) <<
                            Help("--icon-name=ICON-NAME", tr("Set the dialog icon")) <<
                            Help("--no-wrap", tr("Do not enable text wrapping")) <<
                            Help("--no-markup", tr("Do not enable html markup")) <<
                            Help("--ellipsize", tr("Do wrap text, zenity has a rather special problem here")) <<
                            Help("--selectable-labels", "QARMA ONLY! " + tr("Allow to select text for copy and paste")));
        helpDict["scale"] = CategoryHelp(tr("Scale options"), HelpList() <<
                            Help("--text=TEXT", tr("Set the dialog text")) <<
                            Help("--value=VALUE", tr("Set initial value")) <<
                            Help("--min-value=VALUE", tr("Set minimum value")) <<
                            Help("--max-value=VALUE", tr("Set maximum value")) <<
                            Help("--step=VALUE", tr("Set step size")) <<
                            Help("--print-partial", tr("Print partial values")) <<
                            Help("--hide-value", tr("Hide value")));
        helpDict["text-info"] = CategoryHelp(tr("Text information options"), HelpList() <<
                            Help("--filename=FILENAME", tr("Open file")) <<
                            Help("--editable", tr("Allow changes to text")) <<
                            Help("--font=TEXT", tr("Set the text font")) <<
                            Help("--checkbox=TEXT", tr("Enable an I read and agree checkbox")) <<
                            Help("--plain", "QARMA ONLY! " + tr("Force plain text, zenity default limitation")) <<
                            Help("--html", tr("Enable HTML support")) <<
                            Help("--no-interaction", tr("Do not enable user interaction with the WebView. Only works if you use --html option")) <<
                            Help("--url=URL", "REQUIRES CURL BINARY! " + tr("Set an URL instead of a file. Only works if you use --html option")) <<
                            Help("--auto-scroll", tr("Auto scroll the text to the end. Only when text is captured from stdin")));
        helpDict["color-selection"] = CategoryHelp(tr("Color selection options"), HelpList() <<
                            Help("--color=VALUE", tr("Set the color")) <<
                            Help("--show-palette", tr("Show the palette")) <<
                            Help("--custom-palette=path/to/some.gpl",  "QARMA ONLY! " + tr("Load a custom GPL for standard colors")));
        helpDict["font-selection"] = CategoryHelp(tr("Font selection options"), HelpList() <<
                            Help("--type=[vector][,bitmap][,fixed][,variable]", tr("Filter fonts (default: all)")) <<
                            Help("--pattern=%1-%2:%3:%4", tr("Output pattern, %1: Name, %2: Size, %3: weight, %4: slant")) <<
                            Help("--sample=TEXT", tr("Sample text, defaults to the foxdogthing")));
        helpDict["password"] = CategoryHelp(tr("Password dialog options"), HelpList() <<
                            Help("--username", tr("Display the username option")) <<
                            Help("--prompt=TEXT", "QARMA ONLY! " + tr("The prompt for the user")));
        helpDict["forms"] = CategoryHelp(tr("Forms dialog options"), HelpList() <<
                            Help("--add-entry=Field name", tr("Add a new Entry in forms dialog")) <<
                            Help("--entry-value=TEXT", "QARMA ONLY! " + tr("Preset text for the last entry")) <<
                            Help("--add-password=Field name", tr("Add a new Password Entry in forms dialog")) <<
                            Help("--add-calendar=Calendar field name", tr("Add a new Calendar in forms dialog")) <<
                            Help("--add-list=List field and header name", tr("Add a new List in forms dialog")) <<
                            Help("--list-values=List of values separated by |", tr("List of values for List")) <<
                            Help("--column-values=List of values separated by |", tr("List of values for columns")) <<
                            Help("--add-combo=Combo box field name", tr("Add a new combo box in forms dialog")) <<
                            Help("--combo-values=List of values separated by |", tr("List of values for combo box")) <<
                            Help("--show-header", tr("Show the columns header")) <<
                            Help("--text=TEXT", tr("Set the dialog text")) <<
                            Help("--separator=SEPARATOR", tr("Set output separator character")) <<
                            Help("--forms-date-format=PATTERN", tr("Set the format for the returned date")) <<
                            Help("--add-checkbox=Checkbox label", "QARMA ONLY! " + tr("Add a new Checkbox forms dialog")));
        helpDict["dzen"] =  CategoryHelp(tr("Dzen options"), HelpList() <<
                            Help("-fg <color>", tr("Set foreground color. Either as symbolic name (e.g. red, darkgreen, etc.) or as #rrggbb hex-value.")) <<
                            Help("-bg <color>", tr("Set background color (same format as -fg).")) <<
                            Help("-fn <font>", tr("Set font (using the format of xlsfonts and xfontsel).")) <<
                            Help("-ta <l|c|r>", tr("Set alignement of title window content: l(eft), c(center) or r(ight).")) <<
                            Help("-sa <l|c|r>", tr("Set alignment of slave window (see -ta ).")) <<
                            Help("-l <lines>", tr("Number of lines to display in slave window.")) <<
                            Help("-u", tr("Update contents of title and slave window simultaneously.")) <<
                            Help("-p [<timeout>]", tr("Persist EOF (optional timeout in seconds).")) <<
                            Help("-x <pixels>", tr("Set x position on the screen.")) <<
                            Help("-y <pixels>", tr("Set y position on the screen.")) <<
                            Help("-w <pixels>", tr("width")));
        helpDict["misc"] = CategoryHelp(tr("Miscellaneous options"), HelpList() <<
                            Help("--about", tr("About Qarma")) <<
                            Help("--version", tr("Print version")));
        helpDict["qt"] = CategoryHelp(tr("Qt options"), HelpList() <<
                            Help("-platform <platformName[:options]>", tr("specifies the Qt Platform Abstraction (QPA) plugin")) <<
                            Help("-platformpluginpath <path>", tr("specifies the path to platform plugins")) <<
                            Help("-platformtheme <platformTheme>", tr("specifies the platform theme")) <<
                            Help("-plugin <plugin>", tr("specifies additional plugins to load. Can appear multiple times")) <<
                            Help("-qwindowgeometry <WxH+X+Y>", tr("specifies window geometry for the main window")) <<
                            Help("-qwindowicon <icon>", tr("sets the default window icon")) <<
                            Help("-qwindowtitle <title>", tr("sets the title of the first window")) <<
                            Help("-reverse", tr("sets the application's layout direction to Qt::RightToLeft")) <<
                            Help("-display <hostname:screen_number>", tr("switches displays on X11")) <<
                            Help("-geometry <WxH+X+Y>", tr("same as -qwindowgeometry")));
        helpDict["application"] = CategoryHelp(tr("Application Options"), HelpList() <<
                            Help("--calendar", tr("Display calendar dialog")) <<
                            Help("--entry", tr("Display text entry dialog")) <<
                            Help("--error", tr("Display error dialog")) <<
                            Help("--info", tr("Display info dialog")) <<
                            Help("--file-selection", tr("Display file selection dialog")) <<
                            Help("--list", tr("Display list dialog")) <<
                            Help("--notification", tr("Display notification")) <<
                            Help("--progress", tr("Display progress indication dialog")) <<
                            Help("--question", tr("Display question dialog")) <<
                            Help("--warning", tr("Display warning dialog")) <<
                            Help("--scale", tr("Display scale dialog")) <<
                            Help("--text-info", tr("Display text information dialog")) <<
                            Help("--color-selection", tr("Display color selection dialog")) <<
                            Help("--font-selection", "QARMA ONLY! " + tr("Display font selection dialog")) <<
                            Help("--password", tr("Display password dialog")) <<
                            Help("--forms", tr("Display forms dialog")) <<
                            Help("--dzen", "QARMA ONLY! " + tr("Somewhat dzen compatible label")) <<
                            Help("--display=DISPLAY", tr("X display to use")) <<
                            Help("--class=CLASS", tr("Program class as used by the window manager")) <<
                            Help("--name=NAME", tr("Program name as used by the window manager")));
    }

    if (category == "all") {
        foreach(const CategoryHelp &el, helpDict) {
            printf("%s\n", qPrintable(el.first));
            foreach (const Help &help, el.second)
                printf("  %-53s%s\n", qPrintable(help.first), qPrintable(help.second));
            printf("\n");
        }
        return;
    }

    HelpDict::const_iterator it = helpDict.constEnd();
    if (!category.isEmpty())
        it = helpDict.find(category);

    if (it == helpDict.constEnd()) {
        printf("Usage:\n  %s [OPTION ...]\n\n", qPrintable(applicationName()));
        printHelp("help");
        printHelp("application");
        return;
    }

    printf("%s\n", qPrintable(it->first));
    foreach (const Help &help, it->second)
        printf("  %-53s%s\n", qPrintable(help.first), qPrintable(help.second));
    printf("\n");
}

int main (int argc, char **argv)
{
    if (argc > 0)
        QCoreApplication::setApplicationName(argv[0]); // autoset my ass…
    if (argc < 2) {
        Qarma::printHelp();
        return 1;
    }

    bool helpMission = false;
    for (int i = 1; i < argc; ++i) {
        const QString arg(argv[i]);
        if (arg == "-h" || arg.startsWith("--help")) {
            helpMission = true;
            Qarma::printHelp(arg.mid(7)); // "--help-"
        }
    }

    if (helpMission) {
        return 0;
    }

    Qarma d(argc, argv);
    return d.exec();
}
