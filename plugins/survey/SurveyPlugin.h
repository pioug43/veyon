/*
 * SurveyPlugin.h - declaration of SurveyPlugin class
 *
 * Réécriture pour le fork pioug43/veyon d'après l'idée de la PR upstream
 * veyon/veyon#1151 (auteur original : efraildokmeegitim). La version de la
 * PR ne relayait jamais les réponses vers le maître et affichait le dialogue
 * via le worker SYSTEM ; cette version corrige la chaîne complète, expose les
 * réponses via featureStatus() (Web API) et fournit l'interface enseignant
 * (composition de la question, suivi des réponses, export CSV).
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 */

#pragma once

#include <QHash>
#include <QMutex>
#include <QPointer>
#include <QStringList>

#include "FeatureProviderInterface.h"
#include "MessageContext.h"

class SurveyResultsWindow;

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
		return QVersionNumber( 1, 1 );
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
		return QStringLiteral("Veyon Community");
	}

	QString copyright() const override
	{
		return QStringLiteral("efraildokmeegitim, Pierrick Belledent");
	}

	const FeatureList& featureList() const override
	{
		return m_features;
	}

	bool controlFeature( Feature::Uid featureUid, Operation operation, const QVariantMap& arguments,
						 const ComputerControlInterfaceList& computerControlInterfaces ) override;

	bool startFeature( VeyonMasterInterface& master, const Feature& feature,
					   const ComputerControlInterfaceList& computerControlInterfaces ) override;

	bool stopFeature( VeyonMasterInterface& master, const Feature& feature,
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

Q_SIGNALS:
	// réponse d'un élève reçue côté maître (suivie par SurveyResultsWindow)
	void surveyAnswerReceived( ComputerControlInterface::Pointer computerControlInterface,
							   const QString& answer, const QString& answeredAt );

private:
	static constexpr int MaximumQuestionLength = 1000;
	static constexpr int MaximumOptionCount = 10;
	static constexpr int MaximumOptionLength = 200;
	static constexpr int MaximumAnswerLength = 4000;

	const Feature m_surveyFeature;
	const FeatureList m_features;

	// côté maître : fenêtre de suivi des réponses du sondage en cours
	QPointer<SurveyResultsWindow> m_resultsWindow;

	// côté serveur (poste) : contexte du maître à l'origine du sondage,
	// nécessaire pour relayer la réponse du worker vers ce maître
	MessageContext m_masterContext{};

	// côté maître : dernière réponse reçue par poste (exposée par featureStatus)
	mutable QMutex m_answersMutex;
	QHash<const ComputerControlInterface*, QVariantMap> m_answers;
};
