/*
 * ExamModeSession.cpp - authenticated session envelope helpers for ExamMode
 *
 * Copyright (c) 2026 Pierrick Belledent
 */

#include "ExamModeSession.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>

#include <utility>

namespace ExamModeSession
{

ValidationResult validate( const Envelope& envelope, qint64 nowMs, bool allowLegacy )
{
	const bool legacy = envelope.sessionId.isEmpty() && envelope.sequence == 0 &&
		envelope.issuedAtMs == 0 && envelope.expiresAtMs == 0;
	if( legacy )
	{
		if( allowLegacy && envelope.highSecurity == false )
		{
			return { true, QStringLiteral("LEGACY"), QString{} };
		}
		return { false, QStringLiteral("SESSION_REQUIRED"),
			QStringLiteral("A complete session envelope is required") };
	}

	static const QRegularExpression SessionIdPattern(
		QStringLiteral("^[A-Za-z0-9][A-Za-z0-9._:-]{0,127}$") );
	if( SessionIdPattern.match( envelope.sessionId ).hasMatch() == false )
	{
		return { false, QStringLiteral("INVALID_SESSION"), QStringLiteral("Invalid session identifier") };
	}
	if( envelope.sequence == 0 )
	{
		return { false, QStringLiteral("INVALID_SEQUENCE"), QStringLiteral("Sequence must be positive") };
	}
	if( envelope.issuedAtMs <= 0 || envelope.expiresAtMs <= envelope.issuedAtMs )
	{
		return { false, QStringLiteral("INVALID_TIME_WINDOW"), QStringLiteral("Invalid session time window") };
	}
	if( envelope.issuedAtMs > nowMs + MaximumClockSkewMs )
	{
		return { false, QStringLiteral("NOT_YET_VALID"), QStringLiteral("Session was issued in the future") };
	}
	if( envelope.expiresAtMs <= nowMs )
	{
		return { false, QStringLiteral("EXPIRED"), QStringLiteral("Session has expired") };
	}
	if( envelope.expiresAtMs - envelope.issuedAtMs > MaximumLifetimeMs )
	{
		return { false, QStringLiteral("LIFETIME_TOO_LONG"), QStringLiteral("Session lifetime is too long") };
	}
	if( envelope.highSecurity && ( envelope.signingKeyId.isEmpty() || envelope.signature.isEmpty() ) )
	{
		return { false, QStringLiteral("SIGNATURE_REQUIRED"),
			QStringLiteral("High-security sessions must be signed") };
	}
	return { true, QStringLiteral("VALID"), QString{} };
}


QByteArray canonicalPayload( const Envelope& envelope, const QString& operation,
							 const QString& profileDigest )
{
	QStringList required = envelope.requiredCapabilities;
	required.removeDuplicates();
	required.sort( Qt::CaseSensitive );

	QJsonArray requiredJson;
	for( const auto& capability : std::as_const( required ) )
	{
		requiredJson.append( capability );
	}

	// QJsonObject serializes keys deterministically. The protocol discriminator and
	// explicit version prevent signatures from being re-used by another feature.
	const QJsonObject object{
		{ QStringLiteral("expiresAtMs"), QString::number( envelope.expiresAtMs ) },
		{ QStringLiteral("externalCapabilities"), QJsonObject::fromVariantMap( envelope.externalCapabilities ) },
		{ QStringLiteral("highSecurity"), envelope.highSecurity },
		{ QStringLiteral("issuedAtMs"), QString::number( envelope.issuedAtMs ) },
		{ QStringLiteral("operation"), operation },
		{ QStringLiteral("profileDigest"), profileDigest.toLower() },
		{ QStringLiteral("protocol"), QStringLiteral("veyon-exammode-session") },
		{ QStringLiteral("requiredCapabilities"), requiredJson },
		{ QStringLiteral("sequence"), QString::number( envelope.sequence ) },
		{ QStringLiteral("sessionId"), envelope.sessionId },
		{ QStringLiteral("version"), 1 },
	};
	return QJsonDocument( object ).toJson( QJsonDocument::Compact );
}


bool isNewer( const Envelope& envelope, const QString& activeSessionId, quint64 lastSequence )
{
	if( envelope.sessionId.isEmpty() )
	{
		return true; // compatibility mode has no anti-replay guarantee
	}
	return activeSessionId.isEmpty() || envelope.sessionId != activeSessionId || envelope.sequence > lastSequence;
}

}
