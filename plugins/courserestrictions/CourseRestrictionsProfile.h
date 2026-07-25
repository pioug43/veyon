/*
 * CourseRestrictionsProfile.h - data class representing a course restrictions profile
 *
 * Copyright (c) 2026 Pierrick Belledent
 *
 * Un profil décrit ce qui est interdit pendant un cours : applications à
 * fermer/empêcher et sites web à bloquer (ou, à l'inverse, seuls sites
 * autorisés). Les profils sont stockés dans la configuration Veyon et
 * apparaissent comme sous-fonctions du bouton « Restrictions de cours ».
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 */

#pragma once

#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <QUuid>

class CourseRestrictionsProfile
{
public:
	using Uid = QUuid;

	CourseRestrictionsProfile() = default;
	explicit CourseRestrictionsProfile( const QJsonObject& jsonObject );

	const Uid& uid() const { return m_uid; }
	const QString& name() const { return m_name; }
	const QStringList& blockedApplications() const { return m_blockedApplications; }
	const QStringList& websites() const { return m_websites; }

	/** "block" : liste noire de sites. "allow" : liste blanche (Windows/PAC uniquement). */
	const QString& websiteMode() const { return m_websiteMode; }

	bool isValid() const { return m_uid.isNull() == false && m_name.isEmpty() == false; }

	QJsonObject toJson() const;

	static CourseRestrictionsProfile fromValues( const QString& name, const QString& blockedApplications,
												 const QString& websites, const QString& websiteMode,
												 Uid uid = {} );

	/** Découpe une saisie « firefox.exe; steam.exe » en liste normalisée. */
	static QStringList splitList( const QString& value );
	static QString joinList( const QStringList& values );

private:
	Uid m_uid{};
	QString m_name{};
	QStringList m_blockedApplications{};
	QStringList m_websites{};
	QString m_websiteMode{QStringLiteral("block")};
};
