/*
 * TimerPlugin.cpp - implementation of TimerPlugin class
 *
 * Copyright (c) 2026 Pierrick Belledent
 *
 * Voir TimerPlugin.h pour la description générale.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 */

#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QLineEdit>
#include <QSpinBox>
#include <QTimer>

#include "ComputerControlInterface.h"
#include "FeatureManager.h"
#include "FeatureWorkerManager.h"
#include "TimerPlugin.h"
#include "TimerStudentWidget.h"
#include "VeyonMasterInterface.h"
#include "VeyonServerInterface.h"

namespace
{

// fonctionnalité ScreenLock, réutilisée pour l'action « verrouiller » à
// l'expiration plutôt que d'implémenter un second verrouillage
const Feature::Uid ScreenLockFeatureUid{ QStringLiteral("ccb535a2-1d24-4cc1-a709-8b47d2b2ac79") };

}


TimerPlugin::TimerPlugin( QObject* parent ) :
	QObject( parent ),
	m_timerFeature( QStringLiteral("Timer"),
					Feature::Flag::Mode | Feature::Flag::AllComponents,
					Feature::Uid( "5e0a83c6-1d7f-4b92-8a45-c9f306e21b74" ),
					Feature::Uid(),
					tr( "Timer" ), tr( "Stop timer" ),
					tr( "Display a countdown on all computers, optionally locking "
						"the screens when the time is up." ),
					QStringLiteral(":/core/media-playback-start.png") ),
	m_features( { m_timerFeature } )
{
}



bool TimerPlugin::controlFeature( Feature::Uid featureUid, Operation operation,
								  const QVariantMap& arguments,
								  const ComputerControlInterfaceList& computerControlInterfaces )
{
	if( featureUid != m_timerFeature.uid() )
	{
		return false;
	}

	auto targetInterfaces = computerControlInterfaces;
	targetInterfaces.removeLocalHostInterfaces();		// pas de décompte chez l'enseignant

	if( operation == Operation::Stop )
	{
		if( m_expiryTimer )
		{
			m_expiryTimer->stop();
		}
		m_expiryTargets.clear();

		sendFeatureMessage( FeatureMessage{ featureUid, FeatureCommand::StopTimer }, targetInterfaces );
		return true;
	}

	if( operation != Operation::Start )
	{
		return false;
	}

	const auto seconds = qBound( MinimumSeconds,
								 arguments.value( argToString( Argument::Seconds ) ).toInt(),
								 MaximumSeconds );
	const auto label = arguments.value( argToString( Argument::Label ) ).toString()
						   .trimmed().left( MaximumLabelLength );
	const auto fullscreen = arguments.value( argToString( Argument::Fullscreen ) ).toBool();

	FeatureMessage message{ featureUid, FeatureCommand::StartTimer };
	// types concrets uniquement : un QVariant invalide ferait rejeter la map
	// entière côté poste (cf. VariantStream::checkVariant)
	message.addArgument( Argument::Seconds, seconds )
		.addArgument( Argument::Label, label )
		.addArgument( Argument::Fullscreen, fullscreen );

	sendFeatureMessage( message, targetInterfaces );

	// L'action d'expiration est tenue par le maître : les postes ne font
	// qu'afficher le décompte, ce qui évite deux propriétaires du verrouillage.
	if( m_expiryAction != ExpiryAction::None )
	{
		if( m_expiryTimer == nullptr )
		{
			m_expiryTimer = new QTimer( this );
			m_expiryTimer->setSingleShot( true );
			connect( m_expiryTimer, &QTimer::timeout, this, &TimerPlugin::triggerExpiryAction );
		}

		m_expiryTargets = targetInterfaces;
		m_expiryTimer->start( seconds * 1000 );
	}

	return true;
}



bool TimerPlugin::startFeature( VeyonMasterInterface& master, const Feature& feature,
								const ComputerControlInterfaceList& computerControlInterfaces )
{
	if( feature.uid() != m_timerFeature.uid() )
	{
		return false;
	}

	QDialog dialog( master.mainWindow() );
	dialog.setWindowTitle( tr( "Timer" ) );

	auto* const layout = new QFormLayout( &dialog );

	auto* const minutes = new QSpinBox( &dialog );
	minutes->setRange( 1, MaximumSeconds / 60 );
	minutes->setValue( 10 );
	minutes->setSuffix( tr( " min" ) );
	layout->addRow( tr( "Duration:" ), minutes );

	auto* const label = new QLineEdit( &dialog );
	label->setMaxLength( MaximumLabelLength );
	label->setPlaceholderText( tr( "e.g. End of the exercise" ) );
	layout->addRow( tr( "Caption:" ), label );

	auto* const fullscreen = new QCheckBox( tr( "Show in full screen" ), &dialog );
	layout->addRow( QString{}, fullscreen );

	auto* const expiry = new QComboBox( &dialog );
	expiry->addItem( tr( "Do nothing" ), int(ExpiryAction::None) );
	expiry->addItem( tr( "Lock the screens" ), int(ExpiryAction::Lock) );
	layout->addRow( tr( "When time is up:" ), expiry );

	auto* const buttons = new QDialogButtonBox( QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog );
	connect( buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept );
	connect( buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject );
	layout->addRow( buttons );

	if( dialog.exec() != QDialog::Accepted )
	{
		return true;
	}

	m_expiryAction = static_cast<ExpiryAction>( expiry->currentData().toInt() );

	return controlFeature( m_timerFeature.uid(), Operation::Start, {
		{ argToString( Argument::Seconds ), minutes->value() * 60 },
		{ argToString( Argument::Label ), label->text() },
		{ argToString( Argument::Fullscreen ), fullscreen->isChecked() },
	}, computerControlInterfaces );
}



bool TimerPlugin::stopFeature( VeyonMasterInterface& master, const Feature& feature,
							   const ComputerControlInterfaceList& computerControlInterfaces )
{
	Q_UNUSED(master)

	if( feature.uid() != m_timerFeature.uid() )
	{
		return false;
	}

	return controlFeature( m_timerFeature.uid(), Operation::Stop, {}, computerControlInterfaces );
}



/** Échéance atteinte : déclenche l'action choisie sur les postes concernés. */
void TimerPlugin::triggerExpiryAction()
{
	if( m_expiryAction != ExpiryAction::Lock || m_expiryTargets.isEmpty() )
	{
		return;
	}

	VeyonCore::featureManager().controlFeature( ScreenLockFeatureUid, Operation::Start, {},
												m_expiryTargets );
}



bool TimerPlugin::handleFeatureMessage( VeyonServerInterface& server,
										const MessageContext& messageContext,
										const FeatureMessage& message )
{
	Q_UNUSED(messageContext)

	if( message.featureUid() != m_timerFeature.uid() )
	{
		return false;
	}

	switch( message.command<FeatureCommand>() )
	{
	case FeatureCommand::StartTimer:
		m_timerActiveOnServer = true;
		break;

	case FeatureCommand::StopTimer:
		m_timerActiveOnServer = false;
		break;

	default:
		return false;
	}

	// le décompte est une interface utilisateur : il doit s'afficher dans la
	// session de l'utilisateur, pas via le worker SYSTEM (session 0)
	server.featureWorkerManager().sendMessageToUnmanagedSessionWorker( message );

	return true;
}



bool TimerPlugin::handleFeatureMessage( VeyonWorkerInterface& worker, const FeatureMessage& message )
{
	Q_UNUSED(worker)

	if( message.featureUid() != m_timerFeature.uid() )
	{
		return false;
	}

	switch( message.command<FeatureCommand>() )
	{
	case FeatureCommand::StartTimer:
		TimerStudentWidget::open( message.argument( Argument::Seconds ).toInt(),
								  message.argument( Argument::Label ).toString(),
								  message.argument( Argument::Fullscreen ).toBool() );
		return true;

	case FeatureCommand::StopTimer:
		TimerStudentWidget::shutdown();
		return true;

	default:
		break;
	}

	return false;
}



bool TimerPlugin::isFeatureActive( VeyonServerInterface& server, Feature::Uid featureUid ) const
{
	Q_UNUSED(server)

	return featureUid == m_timerFeature.uid() && m_timerActiveOnServer;
}
