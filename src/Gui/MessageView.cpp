/* Copyright (C) 2006 - 2012 Jan Kundrát <jkt@flaska.net>

   This file is part of the Trojita Qt IMAP e-mail client,
   http://trojita.flaska.net/

   This program is free software; you can redistribute it and/or
   modify it under the terms of the GNU General Public
   License as published by the Free Software Foundation; either
   version 2 of the License, or the version 3 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
   General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; see the file COPYING.  If not, write to
   the Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
   Boston, MA 02110-1301, USA.
*/
#include <QDebug>
#include <QDesktopServices>
#include <QHeaderView>
#include <QKeyEvent>
#include <QLabel>
#include <QTextDocument>
#include <QTimer>
#include <QUrl>
#if QT_VERSION >= QT_VERSION_CHECK(5, 0, 0)
#include <QUrlQuery>
#endif
#include <QVBoxLayout>
#include <QtWebKit/QWebHistory>

#include "MessageView.h"
#include "AbstractPartWidget.h"
#include "EmbeddedWebView.h"
#include "ExternalElementsWidget.h"
#include "PartWidgetFactory.h"
#include "TagListWidget.h"
#include "UserAgentWebPage.h"
#include "Window.h"

#include "Imap/Model/MailboxTree.h"
#include "Imap/Model/MsgListModel.h"
#include "Imap/Network/MsgPartNetAccessManager.h"

namespace Gui
{

MessageView::MessageView(QWidget *parent): QWidget(parent)
{
    QPalette pal = palette();
    pal.setColor(backgroundRole(), palette().color(QPalette::Active, QPalette::Base));
    pal.setColor(foregroundRole(), palette().color(QPalette::Active, QPalette::Text));
    setPalette(pal);
    setAutoFillBackground(true);
    netAccess = new Imap::Network::MsgPartNetAccessManager(this);
    connect(netAccess, SIGNAL(requestingExternal(QUrl)), this, SLOT(externalsRequested(QUrl)));
    factory = new PartWidgetFactory(netAccess, this);

    emptyView = new EmbeddedWebView(this, new QNetworkAccessManager(this));
    emptyView->setFixedSize(450,300);
    QMetaObject::invokeMethod(emptyView, "handlePageLoadFinished", Qt::QueuedConnection, Q_ARG(bool, true));
    emptyView->setPage(new UserAgentWebPage(emptyView));
    emptyView->installEventFilter(this);
    emptyView->setAutoFillBackground(false);

    viewer = emptyView;

    //BEGIN create header section

    headerSection = new QWidget(this);

    // we create a dummy header, pass it through the style and the use it's color roles so we
    // know what headers in general look like in the system
    QHeaderView helpingHeader(Qt::Horizontal);
    helpingHeader.ensurePolished();
    pal = headerSection->palette();
    pal.setColor(headerSection->backgroundRole(), palette().color(QPalette::Active, helpingHeader.backgroundRole()));
    pal.setColor(headerSection->foregroundRole(), palette().color(QPalette::Active, helpingHeader.foregroundRole()));
    headerSection->setPalette(pal);
    headerSection->setAutoFillBackground(true);

    // the actual mail header
    header = new QLabel(headerSection);
    header->setBackgroundRole(helpingHeader.backgroundRole());
    header->setForegroundRole(helpingHeader.foregroundRole());
    header->setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::LinksAccessibleByMouse);
    header->setIndent(5);
    header->setWordWrap(true);
    connect(header, SIGNAL(linkHovered(QString)), this, SLOT(linkInTitleHovered(QString)));
    connect(header, SIGNAL(linkActivated(QString)), this, SLOT(headerLinkActivated(QString)));

    // the tag bar
    tags = new TagListWidget(headerSection);
    tags->setBackgroundRole(helpingHeader.backgroundRole());
    tags->setForegroundRole(helpingHeader.foregroundRole());
    tags->hide();
    connect(tags, SIGNAL(tagAdded(QString)), this, SLOT(newLabelAction(QString)));
    connect(tags, SIGNAL(tagRemoved(QString)), this, SLOT(deleteLabelAction(QString)));

    // whether we allow to load external elements
    externalElements = new ExternalElementsWidget(this);
    externalElements->hide();
    connect(externalElements, SIGNAL(loadingEnabled()), this, SLOT(externalsEnabled()));

    // layout the header
    layout = new QVBoxLayout(headerSection);
    layout->addWidget(header, 1);
    layout->addWidget(tags, 3);
    layout->addWidget(externalElements, 1);

    //END create header section

    //BEGIN layout the message

    layout = new QVBoxLayout(this);
    layout->setSpacing(0);
    layout->setContentsMargins(0,0,0,0);

    layout->addWidget(headerSection, 1);

    headerSection->hide();

    // put the actual messages into an extra horizontal view
    // this allows us easy usage of the trailing stretch and also to indent the message a bit
    QHBoxLayout *hLayout = new QHBoxLayout;
    hLayout->setContentsMargins(6,6,6,0);
    hLayout->addWidget(viewer);
    static_cast<QVBoxLayout*>(layout)->addLayout(hLayout, 1);
    // add a strong stretch to squeeze header and message to the top
    // possibly passing a large stretch factor to the message could be enough...
    layout->addStretch(1000);

    //END layout the message

    // make the layout used to add messages our new horizontal layout
    layout = hLayout;

    markAsReadTimer = new QTimer(this);
    markAsReadTimer->setSingleShot(true);
    connect(markAsReadTimer, SIGNAL(timeout()), this, SLOT(markAsRead()));
}

MessageView::~MessageView()
{
    // Redmine #496 -- the default order of destruction starts with our QNAM subclass which in turn takes care of all pending
    // QNetworkReply instances created by that manager. When the destruction goes to the WebKit objects, they try to disconnect
    // from the network replies which are however gone already. We can mitigate that by simply making sure that the destruction
    // starts with the QWebView subclasses and only after that proceeds to the QNAM. Qt's default order leads to segfaults here.
    if (viewer != emptyView) {
        delete viewer;
    }
    delete emptyView;

    delete factory;
}

void MessageView::setEmpty()
{
    markAsReadTimer->stop();
    header->setText(QString());
    headerSection->hide();
    message = QModelIndex();
    disconnect(this, SLOT(handleDataChanged(QModelIndex,QModelIndex)));
    tags->hide();
    if (viewer != emptyView) {
        layout->removeWidget(viewer);
        viewer->deleteLater();
        viewer = emptyView;
        viewer->show();
        layout->addWidget(viewer);
        emit messageChanged();
    }
}

void MessageView::setMessage(const QModelIndex &index)
{
    // first, let's get a real model
    QModelIndex messageIndex;
    const Imap::Mailbox::Model *constModel = 0;
    Imap::Mailbox::TreeItem *item = Imap::Mailbox::Model::realTreeItem(index, &constModel, &messageIndex);
    Q_ASSERT(item); // Make sure it's a message
    Q_ASSERT(messageIndex.isValid());
    Imap::Mailbox::Model *realModel = const_cast<Imap::Mailbox::Model *>(constModel);
    Q_ASSERT(realModel);

    if (!messageIndex.data(Imap::Mailbox::RoleIsFetched).toBool()) {
        qDebug() << "Attempted to load a message that hasn't been synced yet";
        setEmpty();
        connect(realModel, SIGNAL(dataChanged(QModelIndex,QModelIndex)), this, SLOT(handleDataChanged(QModelIndex,QModelIndex)));
        message = messageIndex;
        return;
    }

    QModelIndex rootPartIndex = messageIndex.child(0, 0);

    headerSection->show();
    if (message != messageIndex) {
        emptyView->hide();
        layout->removeWidget(viewer);
        if (viewer != emptyView) {
            viewer->setParent(0);
            viewer->deleteLater();
        }
        message = messageIndex;
        netAccess->setExternalsEnabled(false);
        externalElements->hide();

        netAccess->setModelMessage(message);

        viewer = factory->create(rootPartIndex);
        viewer->setParent(this);
        layout->addWidget(viewer);
        viewer->show();
        header->setText(headerText());

        tags->show();
        tags->setTagList(messageIndex.data(Imap::Mailbox::RoleMessageFlags).toStringList());
        disconnect(this, SLOT(handleDataChanged(QModelIndex,QModelIndex)));
        connect(realModel, SIGNAL(dataChanged(QModelIndex,QModelIndex)), this, SLOT(handleDataChanged(QModelIndex,QModelIndex)));

        emit messageChanged();

        // We want to propagate the QWheelEvent to upper layers
        viewer->installEventFilter(this);
    }

    if (realModel->isNetworkAvailable())
        markAsReadTimer->start(200); // FIXME: make this configurable
}

void MessageView::markAsRead()
{
    if (!message.isValid())
        return;
    Imap::Mailbox::Model *model = const_cast<Imap::Mailbox::Model *>(dynamic_cast<const Imap::Mailbox::Model *>(message.model()));
    Q_ASSERT(model);
    if (!model->isNetworkAvailable())
        return;
    if (!message.data(Imap::Mailbox::RoleMessageIsMarkedRead).toBool())
        model->markMessagesRead(QModelIndexList() << message, Imap::Mailbox::FLAG_ADD);
}

bool MessageView::eventFilter(QObject *object, QEvent *event)
{
    if (event->type() == QEvent::Wheel) {
        MessageView::event(event);
        return true;
    } else if (event->type() == QEvent::KeyPress || event->type() == QEvent::KeyRelease) {
        QKeyEvent *keyEvent = static_cast<QKeyEvent *>(event);
        switch (keyEvent->key()) {
        case Qt::Key_Left:
        case Qt::Key_Right:
        case Qt::Key_Up:
        case Qt::Key_Down:
        case Qt::Key_PageUp:
        case Qt::Key_PageDown:
        case Qt::Key_Home:
        case Qt::Key_End:
            MessageView::event(event);
            return true;
        default:
            return QObject::eventFilter(object, event);
        }
    } else {
        return QObject::eventFilter(object, event);
    }
}

Imap::Message::Envelope MessageView::envelope() const
{
    // Accessing the envelope via QVariant is just too much work here; it's way easier to just get the raw pointer
    Imap::Mailbox::Model *model = dynamic_cast<Imap::Mailbox::Model *>(const_cast<QAbstractItemModel *>(message.model()));
    Imap::Mailbox::TreeItemMessage *messagePtr = dynamic_cast<Imap::Mailbox::TreeItemMessage *>(static_cast<Imap::Mailbox::TreeItem *>(message.internalPointer()));
    return messagePtr->envelope(model);
}

QString MessageView::headerText()
{
    if (!message.isValid())
        return QString();

    const Imap::Message::Envelope &e = envelope();

    QString res;
    if (!e.from.isEmpty())
        res += tr("<b>From:</b>&nbsp;%1<br/>").arg(Imap::Message::MailAddress::prettyList(e.from, Imap::Message::MailAddress::FORMAT_CLICKABLE));
    if (!e.to.isEmpty())
        res += tr("<b>To:</b>&nbsp;%1<br/>").arg(Imap::Message::MailAddress::prettyList(e.to, Imap::Message::MailAddress::FORMAT_CLICKABLE));
    if (!e.cc.isEmpty())
        res += tr("<b>Cc:</b>&nbsp;%1<br/>").arg(Imap::Message::MailAddress::prettyList(e.cc, Imap::Message::MailAddress::FORMAT_CLICKABLE));
    if (!e.bcc.isEmpty())
        res += tr("<b>Bcc:</b>&nbsp;%1<br/>").arg(Imap::Message::MailAddress::prettyList(e.bcc, Imap::Message::MailAddress::FORMAT_CLICKABLE));
#if QT_VERSION < QT_VERSION_CHECK(5, 0, 0)
    res += tr("<b>Subject:</b>&nbsp;%1").arg(Qt::escape(e.subject));
#else
    res += tr("<b>Subject:</b>&nbsp;%1").arg(e.subject.toHtmlEscaped());
#endif
    if (e.date.isValid())
        res += tr("<br/><b>Date:</b>&nbsp;%1").arg(e.date.toLocalTime().toString(Qt::SystemLocaleLongDate));
    return res;
}

QString MessageView::quoteText() const
{
    if (const AbstractPartWidget *w = dynamic_cast<const AbstractPartWidget *>(viewer)) {
        QString quote = w->quoteMe();
        quote.replace('\n', "\n> ");
        const Imap::Message::Envelope &e = envelope();
        QString sender;
        if (!e.from.isEmpty())
            sender = e.from.at(0).name;
        if (e.from.isEmpty())
            sender = tr("you");
        quote.prepend(tr("On %1 %2 wrote:\n\n").arg(e.date.toLocalTime().toString(Qt::SystemLocaleLongDate)).arg(sender));
        return quote;
    }
    return QString();
}

void MessageView::reply(MainWindow *mainWindow, ReplyMode mode)
{
    if (!message.isValid())
        return;

    const Imap::Message::Envelope &e = envelope();

    QList<QPair<Imap::Mailbox::MessageComposer::RecipientKind,QString> > recipients;
    for (QList<Imap::Message::MailAddress>::const_iterator it = e.from.begin(); it != e.from.end(); ++it) {
        recipients << qMakePair(Imap::Mailbox::MessageComposer::Recipient_To, QString::fromUtf8("%1@%2").arg(it->mailbox, it->host));
    }
    if (mode == REPLY_ALL) {
        for (QList<Imap::Message::MailAddress>::const_iterator it = e.to.begin(); it != e.to.end(); ++it) {
            recipients << qMakePair(Imap::Mailbox::MessageComposer::Recipient_Cc, QString::fromUtf8("%1@%2").arg(it->mailbox, it->host));
        }
        for (QList<Imap::Message::MailAddress>::const_iterator it = e.cc.begin(); it != e.cc.end(); ++it) {
            recipients << qMakePair(Imap::Mailbox::MessageComposer::Recipient_To, QString::fromUtf8("%1@%2").arg(it->mailbox, it->host));
        }
    }
    mainWindow->invokeComposeDialog(replySubject(e.subject), quoteText(), recipients, e.messageId);
}

QString MessageView::replySubject(const QString &subject)
{
    if (!subject.startsWith(tr("Re:")))
        return tr("Re: ") + subject;
    else
        return subject;
}

void MessageView::externalsRequested(const QUrl &url)
{
    Q_UNUSED(url);
    externalElements->show();
}

void MessageView::externalsEnabled()
{
    netAccess->setExternalsEnabled(true);
    externalElements->hide();
    AbstractPartWidget *w = dynamic_cast<AbstractPartWidget *>(viewer);
    if (w)
        w->reloadContents();
}

void MessageView::linkInTitleHovered(const QString &target)
{
    if (target.isEmpty()) {
        header->setToolTip(QString());
        return;
    }

    QUrl url(target);
#if QT_VERSION < QT_VERSION_CHECK(5, 0, 0)
    QString niceName = url.queryItemValue(QLatin1String("X-Trojita-DisplayName"));
    if (niceName.isEmpty())
        header->setToolTip(QString::fromAscii("%1@%2").arg(
                               Qt::escape(url.userName()), Qt::escape(url.host())));
    else
        header->setToolTip(QString::fromAscii("<p style='white-space:pre'>%1 &lt;%2@%3&gt;</p>").arg(
                               Qt::escape(niceName), Qt::escape(url.userName()), Qt::escape(url.host())));
#else
    QUrlQuery q(url);
    QString niceName = q.queryItemValue(QLatin1String("X-Trojita-DisplayName"));
    if (niceName.isEmpty())
        header->setToolTip(QString::fromUtf8("%1@%2").arg(
                               url.userName().toHtmlEscaped(), url.host().toHtmlEscaped()));
    else
        header->setToolTip(QString::fromUtf8("<p style='white-space:pre'>%1 &lt;%2@%3&gt;</p>").arg(
                               niceName.toHtmlEscaped(), url.userName().toHtmlEscaped(), url.host().toHtmlEscaped()));
#endif
}

void MessageView::newLabelAction(const QString &tag)
{
    if (!message.isValid())
        return;

    Imap::Mailbox::Model *model = dynamic_cast<Imap::Mailbox::Model *>(const_cast<QAbstractItemModel *>(message.model()));
    model->setMessageFlags(QModelIndexList() << message, tag, Imap::Mailbox::FLAG_ADD);
}

void MessageView::deleteLabelAction(const QString &tag)
{
    if (!message.isValid())
        return;

    Imap::Mailbox::Model *model = dynamic_cast<Imap::Mailbox::Model *>(const_cast<QAbstractItemModel *>(message.model()));
    model->setMessageFlags(QModelIndexList() << message, tag, Imap::Mailbox::FLAG_REMOVE);
}

void MessageView::handleDataChanged(const QModelIndex &topLeft, const QModelIndex &bottomRight)
{
    Q_ASSERT(topLeft.row() == bottomRight.row() && topLeft.parent() == bottomRight.parent());
    if (topLeft == message) {
        if (viewer == emptyView && message.data(Imap::Mailbox::RoleIsFetched).toBool()) {
            qDebug() << "got it!";
            setEmpty();
            setMessage(topLeft);
        }
        tags->setTagList(message.data(Imap::Mailbox::RoleMessageFlags).toStringList());
    }
}

void MessageView::setHomepageUrl(const QUrl &homepage)
{
    emptyView->load(homepage);
}

void MessageView::showEvent(QShowEvent *se)
{
    QWidget::showEvent(se);
    // The Oxygen style reset the attribute - since we're gonna cause an update() here anyway, it's
    // a good moment to stress that "we know better, Hugo ;-)" -- Thomas
    setAutoFillBackground(true);
}

void MessageView::headerLinkActivated(QString s)
{
    // Trojita is registered to handle any mailto: URL
    QDesktopServices::openUrl(QUrl(s));
}

}



