/*
 * RaiseHandMasterWindow.h - file des demandes d'aide côté enseignant
 *
 * Copyright (c) 2026 Pierrick Belledent
 *
 * Liste les postes ayant demandé de l'aide, dans l'ordre d'arrivée — c'est
 * l'ordre qui compte pour l'enseignant, pas la disposition de la salle. Une
 * demande traitée disparaît de la file et réarme le bouton de l'élève.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 */

#pragma once

#include <QDateTime>
#include <QHash>
#include <QWidget>

#include "ComputerControlInterface.h"

class QLabel;
class QPushButton;
class QTreeWidget;
class RaiseHandPlugin;

class RaiseHandMasterWindow : public QWidget
{
	Q_OBJECT
public:
	explicit RaiseHandMasterWindow( RaiseHandPlugin* plugin, QWidget* parent = nullptr );
	~RaiseHandMasterWindow() override = default;

private Q_SLOTS:
	void addRequest( ComputerControlInterface::Pointer computerControlInterface,
					 const QDateTime& timestamp );
	void removeRequest( ComputerControlInterface::Pointer computerControlInterface );

private:
	void clearSelectedRequest();
	void updateSummary();

	static QString displayName( const ComputerControlInterface* computerControlInterface );

	RaiseHandPlugin* m_plugin;

	// Conserve le pointeur PARTAGÉ de chaque poste en file : on ne peut pas le
	// reconstruire depuis un pointeur brut (QSharedPointer — cela créerait un
	// second propriétaire et une double libération).
	QHash<quintptr, ComputerControlInterface::Pointer> m_targets;

	QTreeWidget* m_requests{nullptr};
	QLabel* m_summary{nullptr};
	QPushButton* m_clearButton{nullptr};
};
