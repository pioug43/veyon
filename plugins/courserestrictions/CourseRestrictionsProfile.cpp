/*
 * CourseRestrictionsProfile.cpp - implementation of CourseRestrictionsProfile
 *
 * Copyright (c) 2026 Pierrick Belledent
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 */

#include <QRegularExpression>

#include "CourseRestrictionsProfile.h"

CourseRestrictionsProfile::CourseRestrictionsProfile( const QJsonObject& jsonObject ) :
	m_uid( jsonObject.value( QStringLiteral("Uid") ).toString() ),
	m_name( jsonObject.value( QStringLiteral("Name") ).toString() ),
	m_blockedApplications( splitList( jsonObject.value( QStringLiteral("BlockedApplications") ).toString() ) ),
	m_websites( splitList( jsonObject.value( QStringLiteral("Websites") ).toString() ) ),
	m_websiteMode( jsonObject.value( QStringLiteral("WebsiteMode") ).toString() )
{
	if( m_websiteMode != QStringLiteral("allow") )
	{
		m_websiteMode = QStringLiteral("block");
	}
}



QJsonObject CourseRestrictionsProfile::toJson() const
{
	return {
		{ QStringLiteral("Uid"), m_uid.toString() },
		{ QStringLiteral("Name"), m_name },
		{ QStringLiteral("BlockedApplications"), joinList( m_blockedApplications ) },
		{ QStringLiteral("Websites"), joinList( m_websites ) },
		{ QStringLiteral("WebsiteMode"), m_websiteMode },
	};
}



CourseRestrictionsProfile CourseRestrictionsProfile::fromValues( const QString& name,
																 const QString& blockedApplications,
																 const QString& websites,
																 const QString& websiteMode, Uid uid )
{
	CourseRestrictionsProfile profile;
	profile.m_uid = uid.isNull() ? QUuid::createUuid() : uid;
	profile.m_name = name.trimmed();
	profile.m_blockedApplications = splitList( blockedApplications );
	profile.m_websites = splitList( websites );
	profile.m_websiteMode = websiteMode == QStringLiteral("allow") ?
		QStringLiteral("allow") : QStringLiteral("block");

	return profile;
}



QStringList CourseRestrictionsProfile::splitList( const QString& value )
{
	QStringList values;

	// on tolère le point-virgule, la virgule et le retour à la ligne comme
	// séparateurs : les listes sont saisies à la main par les enseignants
	const auto parts = value.split( QRegularExpression( QStringLiteral("[;,\\s]+") ), Qt::SkipEmptyParts );
	for( const auto& part : parts )
	{
		const auto trimmed = part.trimmed();
		if( trimmed.isEmpty() == false && values.contains( trimmed ) == false )
		{
			values.append( trimmed );
		}
	}

	return values;
}



QString CourseRestrictionsProfile::joinList( const QStringList& values )
{
	return values.join( QStringLiteral("; ") );
}
