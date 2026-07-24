/*
 * SurveyPlugin.h - declaration of SurveyPlugin class
 *
 * Réécriture pour le fork pioug43/veyon d'après l'idée de la PR upstream
 * veyon/veyon#1151 (auteur original : efraildokmeegitim). La version de la
 * PR ne relayait jamais les réponses vers le maître et affichait le dialogue
 * via le worker SYSTEM ; cette version corrige la chaîne complète et expose
 * les réponses au portail via featureStatus() (Web API).
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 */

#pragma once

#include <QHash>
#include <QMutex>

#include "FeatureProviderInterface.h"
#include "MessageContext.h"

class SurveyPlugin : public QObject, FeatureProviderInterface, PluginInterface
{
	Q_OBJECT
	Q_PLUGIN_METADATA(IID "io.veyon.Veyon.Plugins.Survey")
	Q_INTERFACES(PluginInterface FeatureProviderInterface)
public:
	enum class FeatureCommand
	{
		StartSurvey,
		StopSurvey,
		SubmitAnswer
	};

	enum class QuestionType
	{
		SingleChoice = 1,
		MultipleChoice = 2,
		ShortText = 3,
		LongText = 4,
		TrueFalse = 5
	};

	enum class Argument
	{
		Question,
		QuestionType,
		Options,
		Answer,
		SurveyId
	};

	explicit SurveyPlugin( QObject* parent = nullptr );
	~SurveyPlugin() override = default;

	Plugin::Uid uid() const override
	{
		return Plugin::Uid{ QStringLiteral("77d7f2a1-3c4d-5e6f-aaaa-111122223333") };
	}

	QVersionNumber version() const override
	{
		return QVersionNumber( 1, 0 );
	}

	QString name() const override
	{
		return QStringLiteral("Survey");
	}

	QString description() const override
	{
		return tr( "Send a question to users and collect their answers" );
	}

	QString vendor() const override
	{
		return QStringLiteral("Veyon Community / Portail VDI INSA Lyon");
	}

	QString copyright() const override
	{
		return QStringLiteral("efraildokmeegitim, Portail VDI INSA Lyon");
	}

	const FeatureList& featureList() const override
	{
		return m_features;
	}

	bool controlFeature( Feature::Uid featureUid, Operation operation, const QVariantMap& arguments,
						 const ComputerControlInterfaceList& computerControlInterfaces ) override;

	bool handleFeatureMessage( ComputerControlInterface::Pointer computerControlInterface,
							   const FeatureMessage& message ) override;

	bool handleFeatureMessage( VeyonServerInterface& server,
							   const MessageContext& messageContext,
							   const FeatureMessage& message ) override;

	bool handleFeatureMessageFromWorker( VeyonServerInterface& server,
										 const FeatureMessage& message ) override;

	bool handleFeatureMessage( VeyonWorkerInterface& worker, const FeatureMessage& message ) override;

	QVariantMap featureStatus( Feature::Uid featureUid,
							   ComputerControlInterface::Pointer computerControlInterface ) const override;

private:
	static constexpr int MaximumQuestionLength = 1000;
	static constexpr int MaximumOptionCount = 10;
	static constexpr int MaximumOptionLength = 200;
	static constexpr int MaximumAnswerLength = 4000;

	const Feature m_surveyFeature;
	const FeatureList m_features;

	// côté serveur (poste) : contexte du maître à l'origine du sondage,
	// nécessaire pour relayer la réponse du worker vers ce maître
	MessageContext m_masterContext{};

	// côté maître (webapi/portail) : dernière réponse reçue par poste
	mutable QMutex m_answersMutex;
	QHash<const ComputerControlInterface*, QVariantMap> m_answers;
};
