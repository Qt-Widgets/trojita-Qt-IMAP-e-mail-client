#include "MessageComposer.h"
#include <QCoreApplication>
#include <QMimeData>
#include <QUuid>
#include "Imap/Encoders.h"
#include "Imap/Model/ComposerAttachments.h"
#include "Imap/Model/Model.h"
#include "Imap/Model/Utils.h"

namespace Imap {
namespace Mailbox {

MessageComposer::MessageComposer(Model *model, QObject *parent) :
    QAbstractListModel(parent), m_model(model)
{
}

MessageComposer::~MessageComposer()
{
    qDeleteAll(m_attachments);
}

int MessageComposer::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : m_attachments.size();
}

QVariant MessageComposer::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.column() != 0 || index.row() < 0 || index.row() >= m_attachments.size())
        return QVariant();

    switch (role) {
    case Qt::DisplayRole:
        return m_attachments[index.row()]->caption();
    case Qt::ToolTipRole:
        return m_attachments[index.row()]->tooltip();
    }
    return QVariant();
}

Qt::DropActions MessageComposer::supportedDropActions() const
{
    return Qt::CopyAction | Qt::MoveAction;
}

Qt::ItemFlags MessageComposer::flags(const QModelIndex &index) const
{
    Qt::ItemFlags f = QAbstractListModel::flags(index);

    if (index.isValid()) {
        f |= Qt::ItemIsDragEnabled;
    }
    f |= Qt::ItemIsDropEnabled;
    return f;
}

bool MessageComposer::dropMimeData(const QMimeData *data, Qt::DropAction action, int row, int column, const QModelIndex &parent)
{
    if (action == Qt::IgnoreAction)
        return true;

    if (column > 0)
        return false;

    if (!m_model)
        return false;

    static QString xTrojitaMessageList = QLatin1String("application/x-trojita-message-list");
    static QString xTrojitaImapPart = QLatin1String("application/x-trojita-imap-part");

    if (data->hasFormat(xTrojitaMessageList)) {
        QByteArray encodedData = data->data(xTrojitaMessageList);
        QDataStream stream(&encodedData, QIODevice::ReadOnly);

        Q_ASSERT(!stream.atEnd());
        QString mailbox;
        uint uidValidity;
        QList<uint> uids;
        stream >> mailbox >> uidValidity >> uids;
        Q_ASSERT(stream.atEnd());

        TreeItemMailbox *mboxPtr = m_model->findMailboxByName(mailbox);
        if (!mboxPtr) {
            qDebug() << "drag-and-drop: mailbox not found";
            return false;
        }

        if (uids.size() < 1)
            return false;

        beginInsertRows(QModelIndex(), m_attachments.size(), m_attachments.size() + uids.size() - 1);
        Q_FOREACH(const uint uid, uids) {
            m_attachments << new ImapMessageAttachmentItem(m_model, mailbox, uidValidity, uid);
        }
        endInsertRows();

        return true;

    } else if (data->hasFormat(xTrojitaImapPart)) {
        QByteArray encodedData = data->data(xTrojitaImapPart);
        QDataStream stream(&encodedData, QIODevice::ReadOnly);
        Q_ASSERT(!stream.atEnd());
        QString mailbox;
        uint uidValidity;
        uint uid;
        QString pathToPart;
        stream >> mailbox >> uidValidity >> uid >> pathToPart;
        Q_ASSERT(stream.atEnd());

        TreeItemMailbox *mboxPtr = m_model->findMailboxByName(mailbox);
        if (!mboxPtr) {
            qDebug() << "drag-and-drop: mailbox not found";
            return false;
        }

        if (!uidValidity || !uid || pathToPart.isEmpty()) {
            qDebug() << "drag-and-drop: invalid data";
            return false;
        }

        beginInsertRows(QModelIndex(), m_attachments.size(), m_attachments.size());
        m_attachments << new ImapPartAttachmentItem(m_model, mailbox, uidValidity, uid, pathToPart);
        endInsertRows();

        return true;

    } else {
        return false;
    }
}

QStringList MessageComposer::mimeTypes() const
{
    return QStringList() << QLatin1String("application/x-trojita-message-list") << QLatin1String("application/x-trojita-imap-part");
}

void MessageComposer::setFrom(const Message::MailAddress &from)
{
    m_from = from;
}

void MessageComposer::setRecipients(const QList<QPair<RecipientKind, Message::MailAddress> > &recipients)
{
    m_recipients = recipients;
}

void MessageComposer::setInReplyTo(const QByteArray &inReplyTo)
{
    m_inReplyTo = inReplyTo;
}

void MessageComposer::setTimestamp(const QDateTime &timestamp)
{
    m_timestamp = timestamp;
}

void MessageComposer::setSubject(const QString &subject)
{
    m_subject = subject;
}

void MessageComposer::setText(const QString &text)
{
    m_text = text;
}

bool MessageComposer::isReadyForSerialization() const
{
    return true;
}

QByteArray MessageComposer::generateMessageId(const Imap::Message::MailAddress &sender)
{
    if (sender.host.isEmpty()) {
        // There's no usable domain, let's just bail out of here
        return QByteArray();
    }
    return QUuid::createUuid()
#if QT_VERSION >= 0x040800
            .toByteArray()
#else
            .toString().toAscii()
#endif
            .replace("{", "").replace("}", "") + "@" + sender.host.toAscii();
}

/** @short Generate a random enough MIME boundary */
QByteArray MessageComposer::generateMimeBoundary()
{
    // Usage of "=_" is recommended by RFC2045 as it's guaranteed to never occur in a quoted-printable source
    return QByteArray("trojita=_") + QUuid::createUuid()
#if QT_VERSION >= 0x040800
            .toByteArray()
#else
            .toString().toAscii()
#endif
            .replace("{", "").replace("}", "");
}

QByteArray MessageComposer::encodeHeaderField(const QString &text)
{
    /* This encodes an "unstructured" header field */
    /* FIXME: Don't apply RFC2047 if it isn't needed */
    return Imap::encodeRFC2047String(text);
}

bool MessageComposer::asRawMessage(QIODevice *target) const
{
    // The From header
    target->write(QByteArray("From: ").append(m_from.asMailHeader()).append("\r\n"));

    // All recipients
    QByteArray recipientHeaders;
    for (QList<QPair<RecipientKind,Imap::Message::MailAddress> >::const_iterator it = m_recipients.begin();
         it != m_recipients.end(); ++it) {
        switch(it->first) {
        case Recipient_To:
            recipientHeaders.append("To: ").append(it->second.asMailHeader()).append("\r\n");
            break;
        case Recipient_Cc:
            recipientHeaders.append("Cc: ").append(it->second.asMailHeader()).append("\r\n");
            break;
        case Recipient_Bcc:
            break;
        }
    }
    target->write(recipientHeaders);

    // Other message metadata
    target->write(QByteArray("Subject: ").append(encodeHeaderField(m_subject)).append("\r\n").
            append("Date: ").append(Imap::dateTimeToRfc2822(m_timestamp)).append("\r\n").
            append("User-Agent: ").append(
                QString::fromAscii("%1/%2; %3")
                .arg(qApp->applicationName(), qApp->applicationVersion(), Imap::Mailbox::systemPlatformVersion()).toAscii()
                ).append("\r\n").
            append("MIME-Version: 1.0\r\n"));
    QByteArray messageId = generateMessageId(m_from);
    if (!messageId.isEmpty()) {
        target->write(QByteArray("Message-ID: <").append(messageId).append(">\r\n"));
    }
    if (!m_inReplyTo.isEmpty()) {
        target->write(QByteArray("In-Reply-To: ").append(m_inReplyTo).append("\r\n"));
    }

    const bool hasAttachments = !m_attachments.isEmpty();

    // We don't bother with checking that our boundary is not present in the individual parts. That's arguably wrong,
    // but we don't have much choice if we ever plan to use CATENATE.  It also looks like this is exactly how other MUAs
    // oeprate as well, so let's just join the universal dontcareism here.
    QByteArray boundary(generateMimeBoundary());

    if (hasAttachments) {
        target->write("Content-Type: multipart/mixed;\r\n\tboundary=\"" + boundary + "\"\r\n"
                      "\r\nThis is a multipart/mixed message in MIME format.\r\n\r\n"
                      "--" + boundary + "\r\n");
    }

    target->write("Content-Type: text/plain; charset=utf-8\r\n"
                  "Content-Transfer-Encoding: quoted-printable\r\n"
                  "\r\n");
    target->write(Imap::quotedPrintableEncode(m_text.toUtf8()));

    if (hasAttachments) {
        Q_FOREACH(const AttachmentItem *attachment, m_attachments) {
            // FIXME: this assert can fail very, *very* easily when it comes to IMAP-based attachments...
            if (!attachment->isAvailable())
                return false;
            target->write("\r\n--" + boundary + "\r\n"
                          "Content-Type: " + attachment->mimeType() + "\r\n");
            target->write(attachment->contentDispositionHeader());

            AttachmentItem::ContentTransferEncoding cte = attachment->suggestedCTE();
            switch (cte) {
            case AttachmentItem::CTE_BASE64:
                target->write("Content-Transfer-Encoding: base64\r\n");
                break;
            case AttachmentItem::CTE_7BIT:
                target->write("Content-Transfer-Encoding: 7bit\r\n");
                break;
            case AttachmentItem::CTE_8BIT:
                target->write("Content-Transfer-Encoding: 8bit\r\n");
                break;
            case AttachmentItem::CTE_BINARY:
                target->write("Content-Transfer-Encoding: binary\r\n");
                break;
            }

            target->write("\r\n");

            QSharedPointer<QIODevice> io = attachment->rawData();
            if (!io)
                return false;
            while (!io->atEnd()) {
                switch (cte) {
                case AttachmentItem::CTE_BASE64:
                    // Base64 maps 6bit chunks into a single byte. Output shall have no more than 76 characters per line
                    // (not counting the CRLF pair).
                    target->write(io->read(76*6/8).toBase64() + "\r\n");
                    break;
                default:
                    target->write(io->readAll());
                }
            }
        }
        target->write("\r\n--" + boundary + "--\r\n");
    }
    return true;
}

QDateTime MessageComposer::timestamp() const
{
    return m_timestamp;
}

QByteArray MessageComposer::rawFromAddress() const
{
    return m_from.asSMTPMailbox();
}

QList<QByteArray> MessageComposer::rawRecipientAddresses() const
{
    QList<QByteArray> res;

    for (QList<QPair<RecipientKind,Imap::Message::MailAddress> >::const_iterator it = m_recipients.begin();
         it != m_recipients.end(); ++it) {
        res << it->second.asSMTPMailbox();
    }

    return res;
}

void MessageComposer::addFileAttachment(const QString &path)
{
    beginInsertRows(QModelIndex(), m_attachments.size(), m_attachments.size());
    m_attachments << new FileAttachmentItem(path);
    endInsertRows();
}

void MessageComposer::removeAttachment(const QModelIndex &index)
{
    if (!index.isValid() || index.column() != 0 || index.row() < 0 || index.row() >= m_attachments.size())
        return;

    beginRemoveRows(QModelIndex(), index.row(), index.row());
    delete m_attachments.takeAt(index.row());
    endRemoveRows();
}

}
}