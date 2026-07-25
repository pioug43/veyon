/*
 * QuizPlugin.h - declaration of QuizPlugin class
 *
 * Copyright (c) 2026 Pierrick Belledent
 *
 * Questionnaire noté : l'enseignant envoie une série de questions aux postes,
 * l'élève y répond dans un dialogue séquentiel avec compte à rebours, et les
 * réponses remontent au fur et à mesure.
 *
 * Les BONNES RÉPONSES NE SONT JAMAIS ENVOYÉES AUX POSTES. Le plugin ne
 * transporte que l'énoncé et les propositions ; la correction est faite par
 * l'appelant (console ou outil d'administration) à partir des réponses
 * brutes. Envoyer le corrigé sur la machine de l'élève le rendrait lisible
 * par qui sait regarder.
 *
 * Le plugin survey couvre la question unique posée à la volée ; celui-ci
 * couvre l'exercice noté en plusieurs questions.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 */

#pragma once

#include <QHash>
#include <QMutex>
#include <QSet>

#include "FeatureProviderInterface.h"
#include "MessageContext.h"

class QuizPlugin : public QObject, FeatureProviderInterface, PluginInterface
{
	Q_OBJECT
	Q_PLUGIN_METADATA(IID "io.veyon.Veyon.Plugins.Quiz")
	Q_INTERFACES(PluginInterface FeatureProviderInterface)
public:
	enum class FeatureCommand
	{
		StartQuiz,
		StopQuiz,
		SubmitAnswer,		// poste → maître : une réponse
		QuizFinished,		// poste → maître : questionnaire terminé
	};
	Q_ENUM(FeatureCommand)

	enum class Argument
	{
		QuizId,				// QString : identifiant du questionnaire
		Questions,			// QString : tableau JSON, SANS les bonnes réponses
		DurationSeconds,	// int : 0 = pas de limite de temps
		QuestionId,			// QString : question concernée par une réponse
		Answer,				// QString : réponse brute de l'élève
		AnsweredAt,			// QString ISO-8601 (UTC)
	};
	Q_ENUM(Argument)

	explicit QuizPlugin( QObject* parent = nullptr );
	~QuizPlugin() override = default;

	Plugin::Uid uid() const override
	{
		return Plugin::Uid{ QStringLiteral("c8b41e57-2a09-4d63-9f18-70e5c3a2d641") };
	}

	QVersionNumber version() const override
	{
		return QVersionNumber( 1, 0 );
	}

	QString name() const override
	{
		return QStringLiteral("Quiz");
	}

	QString description() const override
	{
		return tr( "Send a multi-question quiz and collect the answers" );
	}

	QString vendor() const override
	{
		return QStringLiteral("Veyon Community");
	}

	QString copyright() const override
	{
		return QStringLiteral("Pierrick Belledent");
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

	bool isFeatureActive( VeyonServerInterface& server, Feature::Uid featureUid ) const override;

	QVariantMap featureStatus( Feature::Uid featureUid,
							   ComputerControlInterface::Pointer computerControlInterface ) const override;

	static constexpr int MaximumQuestions = 100;
	static constexpr int MaximumQuestionsLength = 64 * 1024;
	static constexpr int MaximumAnswerLength = 4000;
	static constexpr int MaximumDurationSeconds = 6 * 60 * 60;

private:
	const Feature m_quizFeature;
	const FeatureList m_features;

	// côté serveur (poste) : maître à l'origine du questionnaire, pour lui
	// relayer les réponses saisies dans la session utilisateur
	MessageContext m_masterContext{};
	bool m_quizActiveOnServer{false};

	// côté maître : réponses reçues par poste. Les entrées sont purgées à la
	// destruction du poste : une adresse réutilisée par une nouvelle interface
	// afficherait sinon la copie du poste précédent — inacceptable pour un
	// questionnaire noté.
	mutable QMutex m_resultsMutex;
	QHash<const ComputerControlInterface*, QVariantMap> m_results;
	QSet<const ComputerControlInterface*> m_trackedRemoteInterfaces;
};
