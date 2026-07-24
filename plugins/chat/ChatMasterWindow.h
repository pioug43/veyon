/*
 * ChatMasterWindow.h - fenêtre de conversation côté enseignant
 *
 * Les postes sont regroupés par pool VDI (= « emplacement » Veyon, champ
 * location du Computer, alimenté par le VDI manager). La sélection d'un
 * pool diffuse à tous ses postes, la racine « All computers » à tout le monde.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 */

#pragma once

#include <QDateTime>
#include <QHash>
#include <QVector>
#include <QWidget>

#include "ComputerControlInterface.h"

class ChatPlugin;
class QTreeWidgetItem;

namespace Ui { class ChatMasterWindow; }

class ChatMasterWindow : public QWidget
{
	Q_OBJECT
public:
	explicit ChatMasterWindow( ChatPlugin* plugin, QWidget* parent = nullptr );
	~ChatMasterWindow() override;

	void addTargets( const ComputerControlInterfaceList& computerControlInterfaces );
	void removeTargets( const ComputerControlInterfaceList& computerControlInterfaces );

protected:
	void closeEvent( QCloseEvent* event ) override;

private Q_SLOTS:
	void appendStudentMessage( ComputerControlInterface::Pointer computerControlInterface,
							   const QString& text, const QDateTime& timestamp );

private:
	struct Entry
	{
		ComputerControlInterface::Pointer target; // nul pour une diffusion
		QString pool;                             // non vide : diffusion à ce pool
		bool fromTeacher;
		QString sender;
		QString text;
		QDateTime timestamp;
	};

	enum class SelectionKind
	{
		All,
		Pool,
		Computer
	};

	struct Selection
	{
		SelectionKind kind{SelectionKind::All};
		QString pool;
		ComputerControlInterface::Pointer target;
	};

	void sendMessage();
	void rebuildView();
	Selection currentSelection() const;
	bool entryVisible( const Entry& entry, const Selection& selection ) const;
	void markUnread( QTreeWidgetItem* item );

	QTreeWidgetItem* poolItem( const QString& pool );

	static QString poolOf( const ComputerControlInterface::Pointer& computerControlInterface );
	static QString displayName( const ComputerControlInterface* computerControlInterface );

	Ui::ChatMasterWindow* ui;
	ChatPlugin* m_plugin;

	QTreeWidgetItem* m_allComputersItem;
	QHash<const ComputerControlInterface*, QTreeWidgetItem*> m_computerItems;

	ComputerControlInterfaceList m_targets;
	QVector<Entry> m_log;
};
