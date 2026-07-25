/*
 * SurveyMasterDialog.h - composition d'un sondage côté enseignant
 *
 * Copyright (c) 2026 Pierrick Belledent
 *
 * Saisie de la question, de son type et — pour les questions à choix — de la
 * liste des réponses proposées (une par ligne). Le dialogue produit la carte
 * d'arguments attendue par SurveyPlugin::controlFeature().
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 */

#pragma once

#include <QDialog>
#include <QVariantMap>

namespace Ui { class SurveyMasterDialog; }

class SurveyMasterDialog : public QDialog
{
	Q_OBJECT
public:
	explicit SurveyMasterDialog( QWidget* parent = nullptr );
	~SurveyMasterDialog() override;

	/** Arguments prêts pour controlFeature() : question, questionType, options. */
	QVariantMap surveyArguments() const;

	QString question() const;
	int questionType() const;
	QStringList options() const;

private:
	void updateOptionsState();

	void accept() override;

	Ui::SurveyMasterDialog* ui;
};
