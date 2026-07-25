/*
 * CourseRestrictionsPlugin.cpp - implementation of CourseRestrictionsPlugin
 *
 * Copyright (c) 2026 Pierrick Belledent
 *
 * Voir CourseRestrictionsPlugin.h pour la description générale.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 */

#include <QDateTime>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMessageBox>
#include <QMutexLocker>
#include <QTimer>

#include "ComputerControlInterface.h"
#include "CourseRestrictionsConfigurationPage.h"
#include "CourseRestrictionsPlugin.h"
#include "FeatureMessage.h"
#include "VeyonConfiguration.h"
#include "VeyonMasterInterface.h"
#include "VeyonServerInterface.h"

namespace
{

/** Fichier d'état du mode examen : sa présence (non expirée) signifie qu'un
 *  examen est en cours sur ce poste et que le mode cours doit s'effacer. */
QString examModeActiveStateFile()
{
#if defined(Q_OS_WIN)
	return QStringLiteral("C:/ProgramData/Veyon/exammode-active-state.json");
#else
	return QStringLiteral("/var/lib/veyon/exammode-active-state.json");
#endif
}

}



CourseRestrictionsPlugin::CourseRestrictionsPlugin( QObject* parent ) :
	QObject( parent ),
	m_configuration( &VeyonCore::config() ),
	m_courseRestrictionsFeature( QStringLiteral("CourseRestrictions"),
								 Feature::Flag::Mode | Feature::Flag::AllComponents,
								 Feature::Uid( "9f2c4b81-6d35-4e7a-8c19-5b3ea0d7f264" ),
								 Feature::Uid(),
								 tr( "Course restrictions" ), tr( "Lift restrictions" ),
								 tr( "Block the applications and websites of a profile on all computers. "
									 "Blocked applications are closed and cannot be started again while "
									 "the restrictions are active." ) ),
	m_enforcer( RestrictionEnforcer::Scope{ QStringLiteral("courserestrictions"),
											QStringLiteral("CourseRestrictions") } )
{
	updateFeatures();

	if( isEndpointComponent() )
	{
		// Filet de sécurité au démarrage du poste : un arrêt brutal pendant un
		// cours a pu laisser des clés IFEO, des politiques navigateur ou une
		// section hosts en place. Contrairement au mode examen, il n'y a rien à
		// reprendre : les restrictions de cours sont volontairement éphémères.
		m_enforcer.cleanupResidualState();

		m_enforceTimer = new QTimer( this );
		connect( m_enforceTimer, &QTimer::timeout, this, &CourseRestrictionsPlugin::enforceTick );

		m_arbitrationTimer = new QTimer( this );
		connect( m_arbitrationTimer, &QTimer::timeout,
				 this, &CourseRestrictionsPlugin::checkExamModeArbitration );
	}
}



CourseRestrictionsPlugin::~CourseRestrictionsPlugin()
{
	if( isEndpointComponent() && m_active )
	{
		stopEnforcement();
	}
}



bool CourseRestrictionsPlugin::isEndpointComponent()
{
	return VeyonCore::component() == VeyonCore::Component::Service ||
		   VeyonCore::component() == VeyonCore::Component::Server;
}



QList<CourseRestrictionsProfile> CourseRestrictionsPlugin::configuredProfiles() const
{
	QList<CourseRestrictionsProfile> profiles;

	const auto configuredValues = m_configuration.courseRestrictionsProfiles();
	for( const auto& value : configuredValues )
	{
		const CourseRestrictionsProfile profile{ value.toObject() };
		if( profile.isValid() )
		{
			profiles.append( profile );
		}
	}

	return profiles;
}



/**
 * Chaque profil configuré devient une sous-fonction du bouton « Restrictions de
 * cours » (même principe que les applications prédéfinies de desktopservices).
 * La console n'affiche alors qu'un menu déroulant de profils.
 */
void CourseRestrictionsPlugin::updateFeatures()
{
	m_profileFeatures.clear();

	const auto profiles = configuredProfiles();
	for( const auto& profile : profiles )
	{
		m_profileFeatures.append( Feature( m_courseRestrictionsFeature.name(),
										   Feature::Flag::Mode | Feature::Flag::Master,
										   profile.uid(), m_courseRestrictionsFeature.uid(),
										   profile.name(), {},
										   tr( "Apply the restrictions profile \"%1\"" ).arg( profile.name() ) ) );
	}

	m_features = FeatureList{ m_courseRestrictionsFeature } + m_profileFeatures;
}



QVariantMap CourseRestrictionsPlugin::profileArguments( const CourseRestrictionsProfile& profile )
{
	return {
		{ argToString( Argument::ProfileId ), profile.uid().toString() },
		{ argToString( Argument::ProfileName ), profile.name() },
		{ argToString( Argument::BlockedApplications ), profile.blockedApplications() },
		{ argToString( Argument::Websites ), profile.websites() },
		{ argToString( Argument::WebsiteMode ), profile.websiteMode() },
	};
}



Feature::Uid CourseRestrictionsPlugin::metaFeature( Feature::Uid featureUid ) const
{
	for( const auto& feature : m_profileFeatures )
	{
		if( feature.uid() == featureUid )
		{
			return m_courseRestrictionsFeature.uid();
		}
	}

	return FeatureProviderInterface::metaFeature( featureUid );
}



bool CourseRestrictionsPlugin::controlFeature( Feature::Uid featureUid, Operation operation,
											   const QVariantMap& arguments,
											   const ComputerControlInterfaceList& computerControlInterfaces )
{
	if( hasFeature( featureUid ) == false )
	{
		return false;
	}

	// Les sous-fonctions « profil » n'existent que côté maître : les postes ne
	// connaissent que la fonctionnalité parente.
	const auto targetFeatureUid = m_courseRestrictionsFeature.uid();

	auto targetInterfaces = computerControlInterfaces;
	targetInterfaces.removeLocalHostInterfaces();		// ne jamais se restreindre soi-même

	if( operation == Operation::Stop )
	{
		sendFeatureMessage( FeatureMessage{ targetFeatureUid, FeatureCommand::StopRestrictions },
							targetInterfaces );
		return true;
	}

	if( operation != Operation::Start )
	{
		return false;
	}

	if( m_configuration.courseRestrictionsEnabled() == false )
	{
		vWarning() << "CourseRestrictions: fonctionnalité désactivée dans la configuration";
		return false;
	}

	// Un profil peut être décrit de deux manières :
	//  - forme simple    : blockedApplications + websites + websiteMode
	//  - forme structurée: processRules + urlRules + urlDefaultAction
	// TOUJOURS convertir vers un type concret : un QVariant invalide (clé absente)
	// sérialisé dans le FeatureMessage fait rejeter la map entière par
	// VariantStream::checkVariant côté poste.
	FeatureMessage message{ targetFeatureUid, FeatureCommand::StartRestrictions };
	message.addArgument( Argument::ProfileId,
						 arguments.value( argToString( Argument::ProfileId ) ).toString() )
		.addArgument( Argument::ProfileName,
					  arguments.value( argToString( Argument::ProfileName ) ).toString()
						  .left( MaximumProfileNameLength ) )
		.addArgument( Argument::BlockedApplications,
					  arguments.value( argToString( Argument::BlockedApplications ) ).toStringList() )
		.addArgument( Argument::Websites,
					  arguments.value( argToString( Argument::Websites ) ).toStringList() )
		.addArgument( Argument::WebsiteMode,
					  arguments.value( argToString( Argument::WebsiteMode ),
									   QStringLiteral("block") ).toString() )
		.addArgument( Argument::ProcessRules,
					  arguments.value( argToString( Argument::ProcessRules ) ).toList() )
		.addArgument( Argument::UrlRules,
					  arguments.value( argToString( Argument::UrlRules ) ).toList() )
		.addArgument( Argument::UrlDefaultAction,
					  arguments.value( argToString( Argument::UrlDefaultAction ),
									   QStringLiteral("allow") ).toString() );

	{
		QMutexLocker locker( &m_remoteStatusMutex );
		for( const auto& controlInterface : std::as_const(targetInterfaces) )
		{
			auto* const rawInterface = controlInterface.data();
			if( m_trackedRemoteInterfaces.contains( rawInterface ) == false )
			{
				m_trackedRemoteInterfaces.insert( rawInterface );
				connect( rawInterface, &QObject::destroyed, this, [this, rawInterface]() {
					QMutexLocker statusLocker( &m_remoteStatusMutex );
					m_remoteStatuses.remove( rawInterface );
					m_trackedRemoteInterfaces.remove( rawInterface );
				} );
			}
			m_remoteStatuses.insert( rawInterface, QVariantMap{
				{ QStringLiteral("status"), QStringLiteral("PENDING") },
				{ QStringLiteral("timestamp"), QDateTime::currentMSecsSinceEpoch() },
			} );
		}
	}

	sendFeatureMessage( message, targetInterfaces );

	return true;
}



bool CourseRestrictionsPlugin::startFeature( VeyonMasterInterface& master, const Feature& feature,
											 const ComputerControlInterfaceList& computerControlInterfaces )
{
	if( hasFeature( feature.uid() ) == false )
	{
		return false;
	}

	if( m_configuration.courseRestrictionsEnabled() == false )
	{
		QMessageBox::information( master.mainWindow(), tr( "Course restrictions" ),
								  tr( "The course restrictions feature is disabled in the Veyon configuration." ) );
		return true;
	}

	const auto profiles = configuredProfiles();

	// Le bouton parent n'est cliquable directement que lorsqu'aucun profil n'est
	// configuré (sinon la console n'affiche qu'un menu déroulant de profils).
	if( feature.uid() == m_courseRestrictionsFeature.uid() )
	{
		QMessageBox::information( master.mainWindow(), tr( "Course restrictions" ),
								  profiles.isEmpty() ?
									  tr( "No restrictions profile has been configured yet. "
										  "Profiles are defined in the Veyon Configurator, "
										  "page \"Course restrictions\"." ) :
									  tr( "Choose a restrictions profile from the menu of this button." ) );
		return true;
	}

	for( const auto& profile : profiles )
	{
		if( Feature::Uid{ profile.uid() } == feature.uid() )
		{
			return controlFeature( m_courseRestrictionsFeature.uid(), Operation::Start,
								   profileArguments( profile ), computerControlInterfaces );
		}
	}

	return false;
}



bool CourseRestrictionsPlugin::stopFeature( VeyonMasterInterface& master, const Feature& feature,
											const ComputerControlInterfaceList& computerControlInterfaces )
{
	Q_UNUSED(master)

	// stopAllFeatures() appelle stopFeature() pour CHAQUE fonctionnalité : on ne
	// réagit qu'à la fonctionnalité parente afin de n'émettre qu'un seul message
	// de levée, quel que soit le nombre de profils configurés.
	if( feature.uid() != m_courseRestrictionsFeature.uid() )
	{
		return false;
	}

	return controlFeature( m_courseRestrictionsFeature.uid(), Operation::Stop, {},
						   computerControlInterfaces );
}



bool CourseRestrictionsPlugin::handleFeatureMessage( VeyonServerInterface& server,
													 const MessageContext& messageContext,
													 const FeatureMessage& message )
{
	if( message.featureUid() != m_courseRestrictionsFeature.uid() )
	{
		return false;
	}

	// Comme pour le mode examen, c'est le composant Server du poste — assez
	// privilégié pour agir sur le système — qui applique les restrictions.
	switch( message.command<FeatureCommand>() )
	{
	case FeatureCommand::StartRestrictions:
		startEnforcement( message );
		break;

	case FeatureCommand::StopRestrictions:
		stopEnforcement();
		break;

	default:
		return false;
	}

	server.sendFeatureMessageReply( messageContext, statusMessage() );

	return true;
}



bool CourseRestrictionsPlugin::handleFeatureMessage( VeyonWorkerInterface& worker, const FeatureMessage& message )
{
	Q_UNUSED(worker)
	Q_UNUSED(message)

	// aucune interface utilisateur côté élève : les restrictions sont silencieuses
	// (l'enseignant prévient sa classe ; un message peut être envoyé via TextMessage)
	return false;
}



bool CourseRestrictionsPlugin::handleFeatureMessage( ComputerControlInterface::Pointer computerControlInterface,
													 const FeatureMessage& message )
{
	if( message.featureUid() != m_courseRestrictionsFeature.uid() ||
		message.command<FeatureCommand>() != FeatureCommand::RestrictionsStatus )
	{
		return false;
	}

	const QVariantMap status{
		{ QStringLiteral("status"), message.argument( Argument::Status ) },
		{ QStringLiteral("profileId"), message.argument( Argument::ProfileId ) },
		{ QStringLiteral("profileName"), message.argument( Argument::ProfileName ) },
		{ QStringLiteral("errorCode"), message.argument( Argument::ErrorCode ) },
		{ QStringLiteral("errorMessage"), message.argument( Argument::ErrorMessage ) },
		{ QStringLiteral("backendResults"), message.argument( Argument::BackendResults ) },
		{ QStringLiteral("appliedAt"), message.argument( Argument::AppliedAt ) },
		{ QStringLiteral("timestamp"), message.argument( Argument::Timestamp ) },
	};

	QMutexLocker locker( &m_remoteStatusMutex );
	auto* const rawInterface = computerControlInterface.data();
	if( m_trackedRemoteInterfaces.contains( rawInterface ) == false )
	{
		m_trackedRemoteInterfaces.insert( rawInterface );
		connect( rawInterface, &QObject::destroyed, this, [this, rawInterface]() {
			QMutexLocker statusLocker( &m_remoteStatusMutex );
			m_remoteStatuses.remove( rawInterface );
			m_trackedRemoteInterfaces.remove( rawInterface );
		} );
	}
	m_remoteStatuses.insert( rawInterface, status );

	return true;
}



void CourseRestrictionsPlugin::sendAsyncFeatureMessages( VeyonServerInterface& server,
														 const MessageContext& messageContext )
{
	if( isEndpointComponent() == false )
	{
		return;
	}

	auto* const ioDevice = messageContext.ioDevice();
	if( ioDevice == nullptr )
	{
		return;
	}

	// sendAsyncFeatureMessages() est invoqué pour CHAQUE message serveur proxifié.
	// N'émettre que lorsque le statut a réellement changé POUR CE CLIENT : émettre
	// à chaque trame injecterait un message dans le flux RFB et le
	// désynchroniserait (écran noir, connexion qui bat) — cf. exammode.
	static const char* const sentVersionProperty = "CourseRestrictionsStatusVersion";
	if( ioDevice->property( sentVersionProperty ).toULongLong() == m_statusVersion )
	{
		return;
	}

	if( server.sendFeatureMessageReply( messageContext, statusMessage() ) )
	{
		ioDevice->setProperty( sentVersionProperty, m_statusVersion );
	}
}



bool CourseRestrictionsPlugin::isFeatureActive( VeyonServerInterface& server, Feature::Uid featureUid ) const
{
	Q_UNUSED(server)

	return featureUid == m_courseRestrictionsFeature.uid() && m_active;
}



QVariantMap CourseRestrictionsPlugin::featureStatus( Feature::Uid featureUid,
													 ComputerControlInterface::Pointer computerControlInterface ) const
{
	if( featureUid != m_courseRestrictionsFeature.uid() )
	{
		return {};
	}

	QMutexLocker locker( &m_remoteStatusMutex );
	return m_remoteStatuses.value( computerControlInterface.data(), QVariantMap{
		{ QStringLiteral("status"), QStringLiteral("UNKNOWN") },
	} );
}



ConfigurationPage* CourseRestrictionsPlugin::createConfigurationPage()
{
	return new CourseRestrictionsConfigurationPage( m_configuration );
}



/**
 * Applique le profil reçu. Le mode examen reste prioritaire : si un examen est
 * en cours sur ce poste, la demande est refusée sans rien modifier — les deux
 * plugins écriraient sinon dans les mêmes clés de registre (politiques
 * navigateur) et se restaureraient mutuellement de travers.
 */
bool CourseRestrictionsPlugin::startEnforcement( const FeatureMessage& message )
{
	if( isEndpointComponent() == false )
	{
		return false;
	}

	if( m_configuration.courseRestrictionsEnabled() == false )
	{
		setStatus( QStringLiteral("REJECTED"), QStringLiteral("FEATURE_DISABLED"),
				   QStringLiteral("Course restrictions are disabled on this computer") );
		return false;
	}

	if( examModeActive() )
	{
		vWarning() << "CourseRestrictions: mode examen actif; restrictions de cours refusées";
		setStatus( QStringLiteral("REJECTED"), QStringLiteral("EXAM_MODE_ACTIVE"),
				   QStringLiteral("An exam is in progress on this computer") );
		return false;
	}

	m_profileId = message.argument( Argument::ProfileId ).toString();
	m_profileName = message.argument( Argument::ProfileName ).toString().left( MaximumProfileNameLength );

	QString platform;
#if defined(Q_OS_WIN)
	platform = QStringLiteral("windows");
#elif defined(Q_OS_MACOS) || defined(Q_OS_MAC)
	platform = QStringLiteral("macos");
#else
	platform = QStringLiteral("linux");
#endif

	RestrictionEnforcer::Policy policy;

	// forme structurée : règles typées par système d'exploitation
	const auto processRules = message.argument( Argument::ProcessRules ).toList();
	if( processRules.isEmpty() == false )
	{
		const auto resolved = RestrictionRules::resolveProcessRules( processRules, platform );
		policy.terminateApplications = resolved.terminateApplications;
		policy.preventLaunchApplications = resolved.preventLaunchApplications;
	}

	// forme simple : une seule liste, fermée ET empêchée de redémarrer
	const auto simpleApplications = RestrictionRules::normalizeApplications(
		message.argument( Argument::BlockedApplications ).toStringList() );
	for( const auto& application : simpleApplications )
	{
		if( policy.terminateApplications.contains( application ) == false )
		{
			policy.terminateApplications.append( application );
		}
		if( policy.preventLaunchApplications.contains( application ) == false )
		{
			policy.preventLaunchApplications.append( application );
		}
	}

	const auto urlRules = message.argument( Argument::UrlRules ).toList();
	if( urlRules.isEmpty() == false )
	{
		policy.urlRules = RestrictionRules::normalizeUrlRules( urlRules );
	}

	bool validDefaultAction = true;
	const auto defaultAction = message.argument( Argument::UrlDefaultAction ).toString();
	if( defaultAction == QStringLiteral("block") )
	{
		policy.defaultUrlAction = RestrictionRules::RuleAction::Block;
	}
	else if( defaultAction.isEmpty() == false && defaultAction != QStringLiteral("allow") )
	{
		validDefaultAction = false;
	}

	if( validDefaultAction == false )
	{
		setStatus( QStringLiteral("REJECTED"), QStringLiteral("INVALID_URL_DEFAULT_ACTION"),
				   QStringLiteral("urlDefaultAction must be \"allow\" or \"block\"") );
		return false;
	}

	// forme simple : liste noire ou liste blanche de domaines
	const auto domains = RestrictionRules::normalizeDomains(
		message.argument( Argument::Websites ).toStringList() );
	const auto websiteMode = message.argument( Argument::WebsiteMode ).toString();
	if( domains.isEmpty() == false )
	{
		if( websiteMode == QStringLiteral("allow") )
		{
			// liste blanche : tout est bloqué sauf ces domaines (backend PAC)
			policy.defaultUrlAction = RestrictionRules::RuleAction::Block;
			for( const auto& domain : domains )
			{
				policy.urlRules.append( { RestrictionRules::RuleAction::Allow, domain, false } );
			}
		}
		else
		{
			policy.blockedDomains = domains;
			for( const auto& domain : domains )
			{
				policy.urlRules.append( { RestrictionRules::RuleAction::Block, domain, false } );
			}
		}
	}

	const bool applied = m_enforcer.apply( policy );

	m_active = true;
	m_backendResults = m_enforcer.backendResults();
	m_appliedAt = QDateTime::currentDateTimeUtc().toString( Qt::ISODate );

	if( m_enforceTimer )
	{
		m_enforceTimer->start( EnforceIntervalMs );
	}
	if( m_arbitrationTimer )
	{
		m_arbitrationTimer->start( ArbitrationIntervalMs );
	}

	if( applied )
	{
		setStatus( QStringLiteral("APPLIED") );
	}
	else
	{
		// Application partielle : ce qui a pu être posé l'est resté (et reste
		// restaurable). L'appelant voit le détail par backend.
		setStatus( QStringLiteral("DEGRADED"), QStringLiteral("BACKEND_PARTIALLY_APPLIED"),
				   QStringLiteral("One or more restriction backends could not be applied") );
	}

	return applied;
}



void CourseRestrictionsPlugin::stopEnforcement()
{
	if( m_enforceTimer )
	{
		m_enforceTimer->stop();
	}
	if( m_arbitrationTimer )
	{
		m_arbitrationTimer->stop();
	}

	if( isEndpointComponent() )
	{
		m_enforcer.remove();
	}

	m_active = false;
	m_profileId.clear();
	m_profileName.clear();
	m_appliedAt.clear();
	m_backendResults.clear();

	setStatus( QStringLiteral("IDLE") );
}



void CourseRestrictionsPlugin::enforceTick()
{
	if( m_active == false )
	{
		return;
	}

	m_enforcer.terminateBlockedProcesses();
}



bool CourseRestrictionsPlugin::examModeActive() const
{
	QFile file( examModeActiveStateFile() );
	if( file.open( QIODevice::ReadOnly ) == false )
	{
		return false;
	}

	const auto document = QJsonDocument::fromJson( file.readAll() );
	if( document.isObject() == false )
	{
		return false;
	}

	// le mode examen persiste une échéance : un fichier périmé (poste redémarré
	// pendant un examen terminé) ne doit pas bloquer les restrictions de cours
	const auto expiresAtMs = document.object().value(
		QStringLiteral("expiresAtMs") ).toVariant().toLongLong();

	return expiresAtMs > QDateTime::currentMSecsSinceEpoch();
}



/**
 * Le mode examen peut démarrer alors que des restrictions de cours sont déjà en
 * place. Comme il est prioritaire et qu'il va reprendre la main sur les mêmes
 * politiques navigateur, on s'efface proprement dès qu'on le détecte.
 */
void CourseRestrictionsPlugin::checkExamModeArbitration()
{
	if( m_active == false || examModeActive() == false )
	{
		return;
	}

	vInfo() << "CourseRestrictions: mode examen détecté; levée des restrictions de cours";

	stopEnforcement();
	setStatus( QStringLiteral("REJECTED"), QStringLiteral("EXAM_MODE_ACTIVE"),
			   QStringLiteral("Course restrictions were lifted because an exam started") );
}



void CourseRestrictionsPlugin::setStatus( const QString& status, const QString& errorCode,
										  const QString& errorMessage )
{
	m_status = status;
	m_errorCode = errorCode;
	m_errorMessage = errorMessage;

	++m_statusVersion;
}



FeatureMessage CourseRestrictionsPlugin::statusMessage() const
{
	FeatureMessage message{ m_courseRestrictionsFeature.uid(), FeatureCommand::RestrictionsStatus };

	// types concrets uniquement (cf. VariantStream::checkVariant)
	message.addArgument( Argument::Status, m_status )
		.addArgument( Argument::ProfileId, m_profileId )
		.addArgument( Argument::ProfileName, m_profileName )
		.addArgument( Argument::ErrorCode, m_errorCode )
		.addArgument( Argument::ErrorMessage, m_errorMessage )
		.addArgument( Argument::BackendResults, m_backendResults )
		.addArgument( Argument::AppliedAt, m_appliedAt )
		.addArgument( Argument::Timestamp, QDateTime::currentMSecsSinceEpoch() );

	// FeatureMessage a un constructeur de copie explicite : pas de copy-init ici
	return FeatureMessage{ message };
}


IMPLEMENT_CONFIG_PROXY(CourseRestrictionsConfiguration)
