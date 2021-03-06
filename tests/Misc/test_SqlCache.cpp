/* Copyright (C) 2006 - 2014 Jan Kundrát <jkt@flaska.net>

   This file is part of the Trojita Qt IMAP e-mail client,
   http://trojita.flaska.net/

   This program is free software; you can redistribute it and/or
   modify it under the terms of the GNU General Public License as
   published by the Free Software Foundation; either version 2 of
   the License or (at your option) version 3 or any later version
   accepted by the membership of KDE e.V. (or its successor approved
   by the membership of KDE e.V.), which shall act as a proxy
   defined in Section 14 of version 3 of the license.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include <QTest>
#include "test_SqlCache.h"
#include "Imap/Model/SQLCache.h"

Q_DECLARE_METATYPE(QList<Imap::Mailbox::MailboxMetadata>)

namespace QTest {
template<>
char *toString(const QList<Imap::Mailbox::MailboxMetadata> &items)
{
    QString buf;
    QDebug dbg(&buf);
    dbg << "[";
    Q_FOREACH(const Imap::Mailbox::MailboxMetadata &metadata, items) {
        dbg << "MailboxMetadata(" << metadata.mailbox << metadata.separator << metadata.flags << "),";
    }
    dbg << "]";
    return qstrdup(buf.toUtf8().constData());
}
}

void TestSqlCache::initTestCase()
{
    errorLog.clear();
    cache = std::make_shared<Imap::Mailbox::SQLCache>();
    cache->setErrorHandler([this](const QString &e) { this->errorLog.push_back(e); });
    QCOMPARE(cache->open(QLatin1String("meh"), QLatin1String(":memory:")), true);
}

void TestSqlCache::cleanupTestCase()
{
}

#define CHECK_CACHE_ERRORS \
    if (!errorLog.empty()) { \
        QCOMPARE(errorLog[0], QString()); \
    }

void TestSqlCache::testMailboxOperation()
{
    using namespace Imap::Mailbox;

    QCOMPARE(cache->childMailboxesFresh(QString()), false);
    CHECK_CACHE_ERRORS;
    QCOMPARE(cache->childMailboxesFresh(QLatin1String("foo")), false);
    CHECK_CACHE_ERRORS;

    QList<MailboxMetadata> toplevel;
    toplevel << MailboxMetadata(QStringLiteral("INBOX"), QStringLiteral("."), QStringList());
    toplevel << MailboxMetadata(QStringLiteral("a"), QString(), QStringList());
    toplevel << MailboxMetadata(QStringLiteral("b"), QString(), QStringList() << QStringLiteral("\\HASNOCHILDREN"));
    toplevel << MailboxMetadata(QStringLiteral("c"), QString(), QStringList() << QStringLiteral("\\HASCHILDREN"));

    cache->setChildMailboxes(QString(), toplevel);
    CHECK_CACHE_ERRORS;
    QCOMPARE(cache->childMailboxes(QString()), toplevel);
    CHECK_CACHE_ERRORS;

    // Check that the cache really removes items
    toplevel.takeLast();
    cache->setChildMailboxes(QString(), toplevel);
    CHECK_CACHE_ERRORS;
    QCOMPARE(cache->childMailboxes(QString()), toplevel);
    CHECK_CACHE_ERRORS;

    QVERIFY(errorLog.empty());
}

QTEST_GUILESS_MAIN(TestSqlCache)
