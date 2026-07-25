/*
 * RaiseHandMasterWindow.cpp - file des demandes d'aide côté enseignant
 *
 * Copyright (c) 2026 Pierrick Belledent
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 */

#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QTreeWidget>
#include <QVBoxLayout>

#include "RaiseHandMasterWindow.h"
#include "RaiseHandPlugin.h"

namespace
{

// pointeur du poste, pour retrouver la ligne correspondante
constexpr auto TargetPointerRole = Qt::UserRole;

}


RaiseHandMasterWindow::RaiseHandMasterWindow( RaiseHandPlugin* plugin, QWidget* parent ) :
	QWidget( parent, Qt::Window ),
	m_plugin( plugin )
{
	setWindowTitle( tr( "Help requests" ) );
	setAttribute( Qt::WA_DeleteOnClose );
	resize( 420, 320 );

	auto* const layout = new QVBoxLayout( this );

	m_summary = new QLabel( this );
	layout->addWidget( m_summary );

	m_requests = new QTreeWidget( this );
	m_requests->setColumnCount( 2 );
	m_requests->setHeaderLabels( { tr( "Computer" ), tr( "Since" ) } );
	m_requests->setRootIsDecorated( false );
	m_requests->setSelectionMode( QAbstractItemView::SingleSelection );
	layout->addWidget( m_requests );

	auto* const buttonLayout = new QHBoxLayout;

	m_clearButton = new QPushButton( tr( "Mark as handled" ), this );
	connect( m_clearButton, &QPushButton::clicked, this, &RaiseHandMasterWindow::clearSelectedRequest );
	buttonLayout->addWidget( m_clearButton );

	buttonLayout->addStretch();

	auto* const closeButton = new QPushButton( tr( "Close" ), this );
	connect( closeButton, &QPushButton::clicked, this, &QWidget::close );
	buttonLayout->addWidget( closeButton );

	layout->addLayout( buttonLayout );

	connect( m_plugin, &RaiseHandPlugin::handRaised, this, &RaiseHandMasterWindow::addRequest );
	connect( m_plugin, &RaiseHandPlugin::handLowered, this, &RaiseHandMasterWindow::removeRequest );

	updateSummary();
}



void RaiseHandMasterWindow::addRequest( ComputerControlInterface::Pointer computerControlInterface,
										const QDateTime& timestamp )
{
	if( computerControlInterface.isNull() )
	{
		return;
	}

	const auto pointer = reinterpret_cast<quintptr>( computerControlInterface.data() );

	// une demande déjà en file n'est pas dupliquée (le poste peut renvoyer son
	// état après une reconnexion) : on conserve l'heure d'origine
	for( int index = 0; index < m_requests->topLevelItemCount(); ++index )
	{
		if( m_requests->topLevelItem( index )->data( 0, TargetPointerRole ).value<quintptr>() == pointer )
		{
			return;
		}
	}

	auto* const item = new QTreeWidgetItem( m_requests, {
		displayName( computerControlInterface.data() ),
		timestamp.toLocalTime().toString( QStringLiteral("HH:mm:ss") ),
	} );
	item->setData( 0, TargetPointerRole, QVariant::fromValue( pointer ) );
	m_targets.insert( pointer, computerControlInterface );

	if( m_requests->currentItem() == nullptr )
	{
		m_requests->setCurrentItem( item );
	}

	updateSummary();

	show();
	raise();
}



void RaiseHandMasterWindow::removeRequest( ComputerControlInterface::Pointer computerControlInterface )
{
	if( computerControlInterface.isNull() )
	{
		return;
	}

	const auto pointer = reinterpret_cast<quintptr>( computerControlInterface.data() );

	for( int index = 0; index < m_requests->topLevelItemCount(); ++index )
	{
		if( m_requests->topLevelItem( index )->data( 0, TargetPointerRole ).value<quintptr>() == pointer )
		{
			delete m_requests->takeTopLevelItem( index );
			break;
		}
	}

	m_targets.remove( pointer );

	updateSummary();
}



void RaiseHandMasterWindow::clearSelectedRequest()
{
	auto* const item = m_requests->currentItem();
	if( item == nullptr )
	{
		return;
	}

	const auto pointer = item->data( 0, TargetPointerRole ).value<quintptr>();
	const auto target = m_targets.value( pointer );
	if( target.isNull() )
	{
		return;
	}

	// Réarme le bouton de l'élève ; la ligne quitte la file immédiatement, sans
	// attendre une confirmation du poste (qui peut être injoignable).
	m_plugin->clearRequest( target );
	removeRequest( target );
}



void RaiseHandMasterWindow::updateSummary()
{
	const auto count = m_requests->topLevelItemCount();

	m_summary->setText( count > 0
		? tr( "%1 user(s) waiting for help — oldest request first." ).arg( count )
		: tr( "No pending help request." ) );

	m_clearButton->setEnabled( count > 0 );
}



QString RaiseHandMasterWindow::displayName( const ComputerControlInterface* computerControlInterface )
{
	if( computerControlInterface == nullptr )
	{
		return {};
	}

	auto name = computerControlInterface->computerName();

	auto user = computerControlInterface->userFullName();
	if( user.isEmpty() )
	{
		user = computerControlInterface->userLoginName();
	}

	if( user.isEmpty() == false )
	{
		name += QStringLiteral(" (%1)").arg( user );
	}

	return name;
}
