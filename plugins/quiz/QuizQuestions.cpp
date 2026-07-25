/*
 * QuizQuestions.cpp - normalisation des questions envoyées aux postes
 *
 * Copyright (c) 2026 Pierrick Belledent
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 */

#include <QJsonObject>

#include "QuizQuestions.h"


QJsonArray QuizQuestions::sanitize( const QJsonArray& questions, int maximumQuestions )
{
	QJsonArray sanitized;

	for( const auto& value : questions )
	{
		if( value.isObject() == false || sanitized.count() >= maximumQuestions )
		{
			continue;
		}

		const auto question = value.toObject();
		const auto text = question.value( QStringLiteral("text") ).toString().trimmed();
		if( text.isEmpty() )
		{
			continue;
		}

		auto type = question.value( QStringLiteral("type") ).toString();
		if( type != QStringLiteral("multiple") && type != QStringLiteral("text") )
		{
			type = QStringLiteral("single");
		}

		QJsonArray options;
		const auto rawOptions = question.value( QStringLiteral("options") ).toArray();
		for( const auto& option : rawOptions )
		{
			const auto label = option.toString().trimmed();
			if( label.isEmpty() == false )
			{
				options.append( label );
			}
		}

		// une question à choix sans proposition n'est pas affichable
		if( type != QStringLiteral("text") && options.count() < 2 )
		{
			continue;
		}

		auto id = question.value( QStringLiteral("id") ).toString().trimmed().left( 64 );
		if( id.isEmpty() )
		{
			id = QString::number( sanitized.count() + 1 );
		}

		sanitized.append( QJsonObject{
			{ QStringLiteral("id"), id },
			{ QStringLiteral("type"), type },
			{ QStringLiteral("text"), text },
			{ QStringLiteral("options"), options },
		} );
	}

	return sanitized;
}
