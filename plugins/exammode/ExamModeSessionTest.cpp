/*
 * ExamModeSessionTest.cpp - tests for ExamMode session validation
 */

#include <QTest>

#include <algorithm>

#include "ExamModeSession.h"

class ExamModeSessionTest : public QObject
{
	Q_OBJECT
private slots:
	void acceptsFreshEnvelope();
	void rejectsReplayAndInvalidWindows();
	void canonicalPayloadIsStable();
};


void ExamModeSessionTest::acceptsFreshEnvelope()
{
	const qint64 now = 1700000000000LL;
	ExamModeSession::Envelope envelope;
	envelope.sessionId = QStringLiteral("exam-42");
	envelope.sequence = 7;
	envelope.issuedAtMs = now - 1000;
	envelope.expiresAtMs = now + 60000;
	QVERIFY( ExamModeSession::validate( envelope, now, false ).accepted );
	QVERIFY( ExamModeSession::isNewer( envelope, QStringLiteral("exam-42"), 6 ) );
}


void ExamModeSessionTest::rejectsReplayAndInvalidWindows()
{
	const qint64 now = 1700000000000LL;
	ExamModeSession::Envelope envelope;
	envelope.sessionId = QStringLiteral("exam-42");
	envelope.sequence = 7;
	envelope.issuedAtMs = now - 1000;
	envelope.expiresAtMs = now + 60000;
	QVERIFY( ExamModeSession::isNewer( envelope, QStringLiteral("exam-42"), 7 ) == false );

	envelope.issuedAtMs = now - 10 * 60 * 1000;
	envelope.expiresAtMs = now - ExamModeSession::MaximumClockSkewMs - 1;
	QCOMPARE( ExamModeSession::validate( envelope, now, false ).code, QStringLiteral("EXPIRED") );
	envelope.sessionId = QStringLiteral("bad\nsession");
	QCOMPARE( ExamModeSession::validate( envelope, now, false ).code, QStringLiteral("INVALID_SESSION") );
}


void ExamModeSessionTest::canonicalPayloadIsStable()
{
	ExamModeSession::Envelope a;
	a.sessionId = QStringLiteral("s1");
	a.sequence = 1;
	a.issuedAtMs = 1000;
	a.expiresAtMs = 2000;
	a.requiredCapabilities = { QStringLiteral("network.firewall"), QStringLiteral("process.preventLaunch") };
	a.externalCapabilities = { { QStringLiteral("screenLock"), QStringLiteral("ACTIVE") } };
	a.signature = QByteArrayLiteral("ignored");
	a.signingKeyId = QStringLiteral("ignored");
	std::reverse( a.requiredCapabilities.begin(), a.requiredCapabilities.end() );
	const auto first = ExamModeSession::canonicalPayload( a, QStringLiteral("start"), QStringLiteral("AABB") );
	a.signature = QByteArrayLiteral("different");
	a.signingKeyId = QStringLiteral("different");
	const auto second = ExamModeSession::canonicalPayload( a, QStringLiteral("start"), QStringLiteral("aabb") );
	QCOMPARE( first, second );
}

QTEST_MAIN( ExamModeSessionTest )
#include "ExamModeSessionTest.moc"
