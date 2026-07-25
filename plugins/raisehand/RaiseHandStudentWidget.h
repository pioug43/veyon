/*
 * RaiseHandStudentWidget.h - bouton flottant « demander de l'aide »
 *
 * Copyright (c) 2026 Pierrick Belledent
 *
 * Petite fenêtre sans bordure, toujours au premier plan, affichée dans la
 * session de l'élève pendant toute la durée du mode. Volontairement minuscule
 * et déplaçable à la souris : elle ne doit pas gêner le travail en cours.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 */

#pragma once

#include <QPoint>
#include <QPointer>
#include <QWidget>

#include "Feature.h"

class QPushButton;
class VeyonWorkerInterface;

class RaiseHandStudentWidget : public QWidget
{
	Q_OBJECT
public:
	static void open( Feature::Uid featureUid, VeyonWorkerInterface* worker );
	/** L'enseignant a traité la demande : le bouton redevient disponible. */
	static void clearRequest();
	static void shutdown();

protected:
	void mousePressEvent( QMouseEvent* event ) override;
	void mouseMoveEvent( QMouseEvent* event ) override;

private:
	RaiseHandStudentWidget( Feature::Uid featureUid, VeyonWorkerInterface* worker );
	~RaiseHandStudentWidget() override = default;

	void toggleHand();
	void updateButton();
	void moveToDefaultPosition();

	static QPointer<RaiseHandStudentWidget> s_instance;

	const Feature::Uid m_featureUid;
	VeyonWorkerInterface* m_worker;

	QPushButton* m_button{nullptr};
	bool m_raised{false};
	QPoint m_dragOffset{};
};
