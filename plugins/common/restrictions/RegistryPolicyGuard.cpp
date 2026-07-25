/*
 * RegistryPolicyGuard.cpp - implementation of RegistryPolicyGuard
 *
 * Copyright (c) 2026 Pierrick Belledent
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 */

#include <QFile>

#include "RegistryPolicyGuard.h"
#include "VeyonCore.h"

#if defined(Q_OS_WIN)
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>

#include "ExamModeWindowsNative.h"
#endif


RegistryPolicyGuard::RegistryPolicyGuard( const QString& id, const QString& logPrefix ) :
	m_id( id ),
	m_logPrefix( logPrefix )
{
}



QString RegistryPolicyGuard::stateFile() const
{
#if defined(Q_OS_WIN)
	return QStringLiteral("C:/ProgramData/Veyon/%1-registry-state.json").arg( m_id );
#else
	return QStringLiteral("/var/lib/veyon/%1-registry-state.json").arg( m_id );
#endif
}



bool RegistryPolicyGuard::isApplied() const
{
	return QFile::exists( stateFile() );
}


#if defined(Q_OS_WIN)

bool RegistryPolicyGuard::apply( const QList<Policy>& policies )
{
	remove();		// restaure d'abord un état antérieur éventuel

	if( isApplied() )
	{
		vWarning() << m_logPrefix << ": restauration précédente incomplète; application refusée";
		return false;
	}

	if( policies.isEmpty() )
	{
		return true;
	}

	QJsonArray entries;
	for( const auto& policy : policies )
	{
		const auto previous = ExamModeWindowsNative::readRegistryValue( policy.key, policy.name );
		if( previous[QStringLiteral("ok")].toBool() == false )
		{
			vWarning() << m_logPrefix << ": sauvegarde registre impossible; transaction refusée"
					   << policy.key << policy.name;
			return false;
		}

		entries.append( QJsonObject{
			{ QStringLiteral("key"), policy.key },
			{ QStringLiteral("name"), policy.name },
			{ QStringLiteral("previous"), previous },
			{ QStringLiteral("expected"), QJsonObject{
				{ QStringLiteral("exists"), true },
				{ QStringLiteral("type"), policy.type },
				{ QStringLiteral("data"), policy.data },
			} },
		} );
	}

	const QJsonObject state{ { QStringLiteral("version"), 1 }, { QStringLiteral("entries"), entries } };

	QDir().mkpath( QFileInfo( stateFile() ).absolutePath() );
	QSaveFile file( stateFile() );
	const auto contents = QJsonDocument( state ).toJson( QJsonDocument::Compact );
	if( file.open( QIODevice::WriteOnly ) == false ||
		file.write( contents ) != contents.size() || file.commit() == false ||
		// l'état de restauration ne doit être lisible que par SYSTEM et les
		// administrateurs : il décrit exactement quoi remettre pour tout lever
		ExamModeWindowsNative::restrictFileToAdministratorsAndSystem( stateFile() ) == false )
	{
		vWarning() << m_logPrefix << ": impossible de sauvegarder l'état registre; application annulée";
		return false;
	}

	bool success = true;
	for( const auto& value : std::as_const(entries) )
	{
		const auto entry = value.toObject();
		const auto expected = entry[QStringLiteral("expected")].toObject();
		success = ExamModeWindowsNative::setRegistryValue( entry[QStringLiteral("key")].toString(),
			entry[QStringLiteral("name")].toString(), expected[QStringLiteral("type")].toString(),
			expected[QStringLiteral("data")].toString() ) && success;
	}

	if( success == false )
	{
		vWarning() << m_logPrefix << ": écriture registre partielle; restauration";
		remove();
	}

	return success;
}



void RegistryPolicyGuard::remove()
{
	if( isApplied() == false )
	{
		return;
	}

	QFile file( stateFile() );
	if( file.open( QIODevice::ReadOnly ) == false )
	{
		return;
	}

	QJsonParseError error;
	const auto document = QJsonDocument::fromJson( file.readAll(), &error );
	file.close();

	const auto entries = error.error == QJsonParseError::NoError && document.isObject()
		? document.object().value( QStringLiteral("entries") ).toArray()
		: QJsonArray{};
	if( entries.isEmpty() )
	{
		vWarning() << m_logPrefix << ": état registre illisible; fichier conservé pour diagnostic";
		return;
	}

	bool success = true;
	for( const auto& value : entries )
	{
		const auto entry = value.toObject();
		const auto key = entry[QStringLiteral("key")].toString();
		const auto name = entry[QStringLiteral("name")].toString();
		const auto previous = entry[QStringLiteral("previous")].toObject();
		const auto expected = entry[QStringLiteral("expected")].toObject();

		// Un tiers a pu modifier la politique depuis notre écriture : dans ce
		// cas on laisse son réglage en place plutôt que de l'écraser.
		const auto current = ExamModeWindowsNative::readRegistryValue( key, name );
		if( current[QStringLiteral("ok")].toBool() == false )
		{
			success = false;
			continue;
		}

		const auto expectedType = expected[QStringLiteral("type")].toString();
		const auto currentData = current[QStringLiteral("data")].toString();
		const auto expectedData = expected[QStringLiteral("data")].toString();
		const bool dataMatches = expectedType.compare( QStringLiteral("REG_DWORD"), Qt::CaseInsensitive ) == 0
			? currentData.toULongLong( nullptr, 0 ) == expectedData.toULongLong( nullptr, 0 )
			: currentData == expectedData;

		if( current[QStringLiteral("exists")].toBool() != expected[QStringLiteral("exists")].toBool() ||
			current[QStringLiteral("type")].toString().compare( expectedType, Qt::CaseInsensitive ) != 0 ||
			dataMatches == false )
		{
			continue;
		}

		if( previous[QStringLiteral("exists")].toBool() )
		{
			success = ExamModeWindowsNative::restoreRegistryValue( key, name, previous ) && success;
		}
		else
		{
			success = previous[QStringLiteral("ok")].toBool() &&
					  ExamModeWindowsNative::deleteRegistryValue( key, name ) && success;
		}
	}

	if( success )
	{
		QFile::remove( stateFile() );
	}
	else
	{
		vWarning() << m_logPrefix << ": restauration registre incomplète; nouvelle tentative au prochain démarrage";
	}
}

#else

bool RegistryPolicyGuard::apply( const QList<Policy>& policies )
{
	Q_UNUSED(policies)

	return false;
}



void RegistryPolicyGuard::remove()
{
}

#endif
