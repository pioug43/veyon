/*
 * ExamModeSession.h - authenticated session envelope helpers for ExamMode
 *
 * Copyright (c) 2026 Pierrick Belledent
 */

#pragma once

#include <QByteArray>
#include <QString>
#include <QVariantMap>

namespace ExamModeSession
{

constexpr qint64 MaximumClockSkewMs = 2 * 60 * 1000;
constexpr qint64 MaximumLifetimeMs = 2 * 60 * 60 * 1000;

struct Envelope
{
	QString sessionId;
	quint64 sequence{0};
	qint64 issuedAtMs{0};
	qint64 expiresAtMs{0};
	bool highSecurity{false};
	QString signingKeyId;
	QByteArray signature;
	QStringList requiredCapabilities;
	QVariantMap externalCapabilities;
};

struct ValidationResult
{
	bool accepted{false};
	QString code;
	QString message;
};

ValidationResult validate( const Envelope& envelope, qint64 nowMs, bool allowLegacy );
QByteArray canonicalPayload( const Envelope& envelope, const QString& operation,
							 const QString& profileDigest );
bool isNewer( const Envelope& envelope, const QString& activeSessionId, quint64 lastSequence );

}
