/*
 * WebApiController.cpp - implementation of WebApiController class
 *
 * Copyright (c) 2020-2026 Tobias Junghans <tobydox@veyon.io>
 *
 * This file is part of Veyon - https://veyon.io
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public
 * License along with this program (see COPYING); if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 02111-1307, USA.
 *
 */

#include <QTcpSocket>
#include <QBuffer>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QImageWriter>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <initializer_list>
#include <limits>
#include <utility>

#include "ComputerControlInterface.h"
#include "FeatureManager.h"
#include "Filesystem.h"
#include "PlatformNetworkFunctions.h"
#include "VeyonConfiguration.h"
#include "WebApiAuthenticationProxy.h"
#include "WebApiConfiguration.h"
#include "WebApiController.h"


namespace
{

QString crashSpoolDirectory()
{
	return VeyonCore::filesystem().expandPath( VeyonCore::config().crashReportDirectory() );
}

bool isValidCrashReportId( const QString& id )
{
	// Empêche toute traversée de chemin : un id est un nom de base « crash-… »
	// sans séparateur ni point d'extension.
	static const QRegularExpression re( QStringLiteral("^crash-[A-Za-z0-9_-]+$") );
	return re.match( id ).hasMatch();
}

}


WebApiController::WebApiController( const WebApiConfiguration& configuration, QObject* parent ) :
	QObject( parent ),
	m_configuration( configuration ),
	m_connectionsLock( QReadWriteLock::Recursive )
{
	connect(&m_updateStatisticsTimer, &QTimer::timeout, this, &WebApiController::updateStatistics);
	m_updateStatisticsTimer.start(StatisticsUpdateIntervalSeconds * MillisecondsPerSecond);

	m_workerThread = new QThread(this);
	m_workerThread->setObjectName(QStringLiteral("WebApiController Worker"));
	m_workerThread->start();

	m_workerObject = new QObject;
	m_workerObject->moveToThread(m_workerThread);
	connect(m_workerThread, &QThread::finished, m_workerObject, &QObject::deleteLater);
}



WebApiController::~WebApiController()
{
	{
		QWriteLocker connectionsWriteLocker{ &m_connectionsLock };
		m_connections.clear();
	}

	// All custom connection deleters queue their work in the worker thread.
	// Wait for that work before stopping the thread and destroying its event loop.
	runInWorkerThread([] {});
	m_workerThread->quit();
	m_workerThread->wait();
	m_workerObject = nullptr;
}



WebApiController::Response WebApiController::getHostState(const Request& request, const QString& host)
{
	Q_UNUSED(request);

	m_apiTotalRequestsCounter++;

	QTcpSocket socket;
	socket.connectToHost(host, VeyonCore::config().veyonServerPort());
	if (socket.waitForConnected(3000))
	{
		return QVariantMap{{k2s(Key::State), QByteArrayLiteral("online")}};
	}

	if (VeyonCore::platform().networkFunctions().ping(host) == PlatformNetworkFunctions::PingResult::ReplyReceived)
	{
		return QVariantMap{{k2s(Key::State), QByteArrayLiteral("up")}};
	}

	return QVariantMap{{k2s(Key::State), QByteArrayLiteral("down")}};
}




WebApiController::Response WebApiController::getAuthenticationMethods( const Request& request, const QString& host )
{
	m_apiTotalRequestsCounter++;

	Q_UNUSED(request)

	WebApiConnection connection( host.isEmpty() ? QStringLiteral("localhost") : host );

	const auto proxy = new WebApiAuthenticationProxy( m_configuration );
	proxy->populateCredentials( proxy->dummyAuthenticationMethod(), {} );

	connection.controlInterface()->start( {}, ComputerControlInterface::UpdateMode::Basic, proxy );

	if( proxy->waitForAuthenticationMethods(
			m_configuration.connectionAuthenticationTimeout() * MillisecondsPerSecond ) == false )
	{
		if( proxy->protocolErrorOccurred() )
		{
			return Error::ProtocolMismatch;
		}

		vWarning() << "waiting for authentication methods timed out";
		return Error::ConnectionTimedOut;
	}

	const auto authMethodUuids = proxy->authenticationMethods();

	QVariantList methods; // clazy:exclude=inefficient-qlist
	methods.reserve( authMethodUuids.size() );

	for( const auto& authMethodUuid : authMethodUuids )
	{
		methods.append(authMethodUuid.toString(QUuid::WithoutBraces));
	}

	return QVariantMap{ { k2s(Key::Methods), methods } };
}



WebApiController::Response WebApiController::performAuthentication( const Request& request, const QString& host )
{
	m_apiTotalRequestsCounter++;

	QReadLocker connectionsReadLocker{&m_connectionsLock};

	if( m_connections.size() >= m_configuration.connectionLimit() )
	{
		return Error::ConnectionLimitReached;
	}

	const auto methodUuid = QUuid( request.data[k2s(Key::Method)].toString() );
	if( methodUuid.isNull() )
	{
		return Error::InvalidData;
	}

	auto uuid = QUuid::createUuid();
	while( m_connections.contains( uuid ) )
	{
		uuid = QUuid::createUuid();
	}

	connectionsReadLocker.unlock();

	auto proxy = new WebApiAuthenticationProxy( m_configuration );

	// create connection (including timer resources) in main thread
	auto connection = runInWorkerThread<WebApiConnectionPointer>([this, host, proxy]() {
		auto connection = new WebApiConnection{host.isEmpty() ? QStringLiteral("localhost") : host};
		connection->controlInterface()->start({}, ComputerControlInterface::UpdateMode::Basic, proxy);

		// make shared pointer destroy the connection in management thread again
		return WebApiConnectionPointer{connection,
						  [this](WebApiConnection* c) { runInWorkerThreadNonBlocking([c] { delete c; }); } };
	});

	const auto authTimeout = m_configuration.connectionAuthenticationTimeout() * MillisecondsPerSecond;
	if( proxy->waitForAuthenticationMethods( authTimeout ) == false )
	{
		vWarning() << "waiting for authentication methods timed out";
		return Error::ConnectionTimedOut;
	}

	if( proxy->protocolErrorOccurred() )
	{
		return Error::ProtocolMismatch;
	}

	if( proxy->authenticationMethods().contains( methodUuid ) == false )
	{
		return Error::AuthenticationMethodNotAvailable;
	}

	if( proxy->populateCredentials( methodUuid, request.data[k2s(Key::Credentials)].toMap() ) == false )
	{
		return Error::InvalidCredentials;
	}

	QEventLoop eventLoop;
	QTimer authenticationTimeoutTimer;

	static constexpr auto ResultAuthSucceeded = 0;
	static constexpr auto ResultAuthFailed = 1;
	static constexpr auto ResultAuthTimedOut = 2;

	connect( &authenticationTimeoutTimer, &QTimer::timeout, &eventLoop,
			 [&eventLoop]() { eventLoop.exit(ResultAuthTimedOut); } );
	connect( connection->controlInterface().data(), &ComputerControlInterface::stateChanged, &eventLoop,
			 [&connection, &eventLoop]() { // clazy:exclude=lambda-in-connect
				 switch( connection->controlInterface()->state() )
				 {
				 case ComputerControlInterface::State::AuthenticationFailed:
					 eventLoop.exit( ResultAuthFailed );
					 break;
				 case ComputerControlInterface::State::Connected:
					 eventLoop.exit( ResultAuthSucceeded );
				 default:
					 break;
				 }
			 } );

	authenticationTimeoutTimer.start( authTimeout );

	const auto result = eventLoop.exec() == ResultAuthSucceeded;

	if( result )
	{
		connection->lock();

		m_connectionsLock.lockForWrite();
		if( m_connections.size() >= m_configuration.connectionLimit() )
		{
			m_connectionsLock.unlock();
			connection->unlock();
			return Error::ConnectionLimitReached;
		}
		m_connections[uuid] = connection;
		m_connectionsLock.unlock();

		connect(connection->controlInterface().get(), &ComputerControlInterface::framebufferUpdated,
				this, &WebApiController::incrementVncFramebufferUpdatesCounter);

		const auto idleTimer = connection->idleTimer();
		const auto lifetimeTimer = connection->lifetimeTimer();

		connect( idleTimer, &QTimer::timeout, this, [this, uuid]() {
			vInfo() << "idle time exceeded for connection" << uuid;
			removeConnection(uuid);
		} );
		connect( lifetimeTimer, &QTimer::timeout, this, [this, uuid]() {
			vInfo() << "lifetime exceeded for connection" << uuid;
			removeConnection(uuid);
		} );

		const auto connectionIdleTimeout = m_configuration.connectionIdleTimeout() * MillisecondsPerSecond;
		const auto connectionLifetime = m_configuration.connectionLifetime() * MillisecondsPerHour;

		connection->unlock();

		runInWorkerThread([=] {
			idleTimer->start(connectionIdleTimeout);
			lifetimeTimer->start(connectionLifetime);
		});

		return QVariantMap{
			{ QString::fromUtf8(connectionUidHeaderFieldName().toLower()), uuid.toString(QUuid::WithoutBraces) },
			{ k2s(Key::ValidUntil), QDateTime::currentSecsSinceEpoch() + connectionLifetime / MillisecondsPerSecond }
		};
	}

	return Error::AuthenticationFailed;
}



WebApiController::Response WebApiController::closeConnection( const Request& request, const QString& host )
{
	m_apiTotalRequestsCounter++;

	Q_UNUSED(host)

	Response checkResponse{};
	if( ( checkResponse = checkConnection( request ) ).error != Error::NoError )
	{
		return checkResponse;
	}

	removeConnection(QUuid{lookupHeaderField(request, connectionUidHeaderFieldName())});

	return {};
}



WebApiController::Response WebApiController::getFramebuffer( const Request& request )
{
	m_apiTotalRequestsCounter++;

	Response checkResponse{};
	if( ( checkResponse = checkConnection( request ) ).error != Error::NoError )
	{
		return checkResponse;
	}

	const auto connection = lookupConnection( request );

	if( connection->controlInterface()->hasValidFramebuffer() == false )
	{
		return Error::FramebufferNotAvailable;
	}

	m_framebufferRequestsCounter++;

	const auto width = request.data[k2s(Key::Width)].toInt();
	const auto height = request.data[k2s(Key::Height)].toInt();
	const auto size = connection->scaledFramebufferSize( width, height );

	const auto compression = request.data[k2s(Key::Compression)].toString().toInt();
	const auto quality = request.data[k2s(Key::Quality)].toString().toInt();

	auto format = request.data[k2s(Key::Format)].toString().toUtf8();
	if( format.isEmpty() )
	{
		format = QByteArrayLiteral("png");
	}

	if( QImageWriter::supportedImageFormats().contains( format ) == false )
	{
		return Error::UnsupportedImageFormat;
	}

	const auto imageData = connection->encodedFramebufferData( size, format, compression, quality );

	if( imageData.isNull() )
	{
		return { Error::FramebufferEncodingError, connection->framebufferEncodingError() };
	}

	return imageData;
}



WebApiController::Response WebApiController::getConnectionInformation( const Request& request )
{
	m_apiTotalRequestsCounter++;

	Response checkResponse{};
	if( ( checkResponse = checkConnection( request ) ).error != Error::NoError )
	{
		return checkResponse;
	}

	const auto connection = lookupConnection( request );
	const auto controlInterface = connection->controlInterface();
	const auto framebufferSize = controlInterface->framebuffer().size();

	QVariantList screens;
	for( const auto& screen : controlInterface->screens() )
	{
		screens.append(QVariantMap{
			{ k2s(Key::Index), screen.index },
			{ k2s(Key::Name), screen.name },
			{ k2s(Key::Geometry), QVariantMap{
				{ k2s(Key::X), screen.geometry.x() },
				{ k2s(Key::Y), screen.geometry.y() },
				{ k2s(Key::Width), screen.geometry.width() },
				{ k2s(Key::Height), screen.geometry.height() },
			} },
		});
	}

	return QVariantMap{
		{ k2s(Key::State), EnumHelper::toString(controlInterface->state()) },
		{ k2s(Key::ServerVersion), EnumHelper::toString(controlInterface->serverVersion()) },
		{ k2s(Key::Framebuffer), QVariantMap{
			{ k2s(Key::Width), framebufferSize.width() },
			{ k2s(Key::Height), framebufferSize.height() },
			{ k2s(Key::Valid), controlInterface->hasValidFramebuffer() },
		} },
		{ k2s(Key::Screens), screens },
	};
}



WebApiController::Response WebApiController::sendPointerEvent( const Request& request )
{
	m_apiTotalRequestsCounter++;

	Response checkResponse{};
	if( ( checkResponse = checkConnection( request ) ).error != Error::NoError )
	{
		return checkResponse;
	}

	bool xValid = false;
	bool yValid = false;
	bool buttonMaskValid = false;
	const auto x = request.data.value(k2s(Key::X)).toInt(&xValid);
	const auto y = request.data.value(k2s(Key::Y)).toInt(&yValid);
	const auto buttonMask = request.data.value(k2s(Key::ButtonMask)).toInt(&buttonMaskValid);
	const auto connection = lookupConnection( request );
	const auto controlInterface = connection->controlInterface();
	const auto framebufferSize = controlInterface->framebuffer().size();

	if( xValid == false || yValid == false || buttonMaskValid == false ||
		x < 0 || y < 0 || x >= framebufferSize.width() || y >= framebufferSize.height() ||
		buttonMask < 0 || buttonMask > 255 )
	{
		return Error::InvalidData;
	}

	const auto vncConnection = controlInterface->vncConnection();
	if( vncConnection == nullptr || vncConnection->isConnected() == false )
	{
		return Error::ConnectionNotReady;
	}

	runInWorkerThread([controlInterface] { controlInterface->setUpdateMode(ComputerControlInterface::UpdateMode::Live); });
	vncConnection->mouseEvent(x, y, buttonMask);

	return {};
}



WebApiController::Response WebApiController::sendKeyEvent( const Request& request )
{
	m_apiTotalRequestsCounter++;

	Response checkResponse{};
	if( ( checkResponse = checkConnection( request ) ).error != Error::NoError )
	{
		return checkResponse;
	}

	bool keyCodeValid = false;
	const auto keyCode = request.data.value(k2s(Key::KeyCode)).toULongLong(&keyCodeValid);
	if( keyCodeValid == false || keyCode > std::numeric_limits<VncConnection::KeyCode>::max() ||
		request.data.contains(k2s(Key::Pressed)) == false )
	{
		return Error::InvalidData;
	}

	const auto connection = lookupConnection( request );
	const auto controlInterface = connection->controlInterface();
	const auto vncConnection = controlInterface->vncConnection();
	if( vncConnection == nullptr || vncConnection->isConnected() == false )
	{
		return Error::ConnectionNotReady;
	}

	runInWorkerThread([controlInterface] { controlInterface->setUpdateMode(ComputerControlInterface::UpdateMode::Live); });
	vncConnection->keyEvent(static_cast<VncConnection::KeyCode>(keyCode),
						request.data.value(k2s(Key::Pressed)).toBool());

	return {};
}



WebApiController::Response WebApiController::sendClipboardText( const Request& request )
{
	m_apiTotalRequestsCounter++;

	Response checkResponse{};
	if( ( checkResponse = checkConnection( request ) ).error != Error::NoError )
	{
		return checkResponse;
	}

	static constexpr auto MaximumClipboardCharacters = 1024 * 1024;
	if( request.data.contains(k2s(Key::Text)) == false ||
		request.data.value(k2s(Key::Text)).toString().size() > MaximumClipboardCharacters )
	{
		return Error::InvalidData;
	}

	const auto connection = lookupConnection( request );
	const auto vncConnection = connection->controlInterface()->vncConnection();
	if( vncConnection == nullptr || vncConnection->isConnected() == false )
	{
		return Error::ConnectionNotReady;
	}

	vncConnection->clientCut(request.data.value(k2s(Key::Text)).toString());

	return {};
}



WebApiController::Response WebApiController::startScreenBroadcast( const Request& request )
{
	m_apiTotalRequestsCounter++;

	ComputerControlInterface::Pointer source;
	ComputerControlInterfaceList targets;
	const auto lookupResponse = lookupBroadcastConnections(request, source, targets);
	if( lookupResponse.error != Error::NoError )
	{
		return lookupResponse;
	}

	const auto mode = request.data.value(k2s(Key::Mode), QStringLiteral("fullScreen")).toString();
	const Feature::Uid clientFeatureUid = mode == QStringLiteral("fullScreen") ?
		Feature::Uid{QStringLiteral("7b6231bd-eb89-45d3-af32-f70663b2f878")} :
		mode == QStringLiteral("window") ?
			Feature::Uid{QStringLiteral("ae45c3db-dc2e-4204-ae8b-374cdab8c62c")} : Feature::Uid{};

	auto demoServerHost = request.data.value(k2s(Key::Host)).toString();
	if( demoServerHost.isEmpty() )
	{
		demoServerHost = source->computer().hostName();
	}
	if( demoServerHost == QStringLiteral("localhost") || demoServerHost == QStringLiteral("127.0.0.1") ||
		demoServerHost == QStringLiteral("::1") )
	{
		return { Error::InvalidData,
			QStringLiteral("host must be an address of the teacher computer reachable by all target computers") };
	}

	bool portValid = true;
	const auto demoServerPort = request.data.contains(k2s(Key::Port)) ?
		request.data.value(k2s(Key::Port)).toInt(&portValid) : VeyonCore::config().demoServerPort();
	if( clientFeatureUid.isNull() || demoServerHost.isEmpty() || portValid == false ||
		demoServerPort < 1 || demoServerPort > 65535 )
	{
		return Error::InvalidData;
	}

	const auto token = QUuid::createUuid().toString(QUuid::WithoutBraces).toUtf8();
	const Feature::Uid serverFeatureUid{QStringLiteral("e4b6e743-1f5b-491d-9364-e091086200f4")};
	const QVariantMap serverArguments{
		{ QStringLiteral("demoAccessToken"), token },
		{ QStringLiteral("demoServerPort"), demoServerPort },
	};
	const QVariantMap clientArguments{
		{ QStringLiteral("demoAccessToken"), token },
		{ QStringLiteral("demoServerHost"), demoServerHost },
		{ QStringLiteral("demoServerPort"), demoServerPort },
	};

	runInCoreThread([&] {
		VeyonCore::featureManager().controlFeature(serverFeatureUid, FeatureProviderInterface::Operation::Start,
											  serverArguments, {source});
		VeyonCore::featureManager().controlFeature(clientFeatureUid, FeatureProviderInterface::Operation::Start,
											  clientArguments, targets);
	});

	return QVariantMap{
		{ k2s(Key::Active), true },
		{ k2s(Key::Mode), mode },
		{ k2s(Key::Token), QString::fromUtf8(token) },
		{ k2s(Key::Host), demoServerHost },
		{ k2s(Key::Port), demoServerPort },
	};
}



WebApiController::Response WebApiController::stopScreenBroadcast( const Request& request )
{
	m_apiTotalRequestsCounter++;

	ComputerControlInterface::Pointer source;
	ComputerControlInterfaceList targets;
	const auto lookupResponse = lookupBroadcastConnections(request, source, targets);
	if( lookupResponse.error != Error::NoError )
	{
		return lookupResponse;
	}

	const Feature::Uid serverFeatureUid{QStringLiteral("e4b6e743-1f5b-491d-9364-e091086200f4")};
	const Feature::Uid fullScreenClientFeatureUid{QStringLiteral("7b6231bd-eb89-45d3-af32-f70663b2f878")};
	const Feature::Uid windowClientFeatureUid{QStringLiteral("ae45c3db-dc2e-4204-ae8b-374cdab8c62c")};

	runInCoreThread([&] {
		VeyonCore::featureManager().controlFeature(fullScreenClientFeatureUid, FeatureProviderInterface::Operation::Stop,
											  {}, targets);
		VeyonCore::featureManager().controlFeature(windowClientFeatureUid, FeatureProviderInterface::Operation::Stop,
											  {}, targets);
		VeyonCore::featureManager().controlFeature(serverFeatureUid, FeatureProviderInterface::Operation::Stop,
											  {}, {source});
	});

	return QVariantMap{{ k2s(Key::Active), false }};
}



WebApiController::Response WebApiController::listFeatures( const Request& request )
{
	m_apiTotalRequestsCounter++;

	Response checkResponse{};
	if( ( checkResponse = checkConnection( request ) ).error != Error::NoError )
	{
		return checkResponse;
	}

	const auto& features = VeyonCore::featureManager().features(); // clazy:exclude=inefficient-qlist
	const auto activeFeatures = lookupConnection( request )->controlInterface()->activeFeatures();

	QVariantList featureList; // clazy:exclude=inefficient-qlist
	featureList.reserve( features.size() );

	for( const auto& feature : features )
	{
		QStringList flags;
		for( const auto& flag : std::initializer_list<std::pair<Feature::Flag, QString>>{
				 { Feature::Flag::Mode, QStringLiteral("mode") },
				 { Feature::Flag::Action, QStringLiteral("action") },
				 { Feature::Flag::Session, QStringLiteral("session") },
				 { Feature::Flag::Meta, QStringLiteral("meta") },
				 { Feature::Flag::Option, QStringLiteral("option") },
				 { Feature::Flag::Master, QStringLiteral("master") },
				 { Feature::Flag::Service, QStringLiteral("service") },
				 { Feature::Flag::Worker, QStringLiteral("worker") },
				 { Feature::Flag::Builtin, QStringLiteral("builtin") } })
		{
			if( feature.testFlag(flag.first) )
			{
				flags.append(flag.second);
			}
		}

		QVariantMap featureObject{ { k2s(Key::Name), feature.name() },
								   { k2s(Key::DisplayName), feature.displayName() },
								   { k2s(Key::DisplayNameActive), feature.displayNameActive() },
								   { k2s(Key::Description), feature.description() },
								   { k2s(Key::IconUrl), feature.iconUrl() },
								   { k2s(Key::Flags), flags },
								   { k2s(Key::Uid), feature.uid().toString(QUuid::WithoutBraces) },
								   { k2s(Key::ParentUid), feature.parentUid().toString(QUuid::WithoutBraces) },
								   { k2s(Key::Active), activeFeatures.contains(feature.uid()) } };
		featureList.append( featureObject );
	}

	return featureList;
}



WebApiController::Response WebApiController::setFeatureStatus( const Request& request, const QString& feature )
{
	m_apiTotalRequestsCounter++;

	Response checkResponse{};
	if( ( checkResponse = checkConnection( request ) ).error != Error::NoError ||
		( checkResponse = checkFeature( feature ) ).error != Error::NoError )
	{
		return checkResponse;
	}

	if( request.data.contains( k2s(Key::Active) ) == false )
	{
		return Error::InvalidData;
	}

	const auto connection = lookupConnection( request );

	const auto operation = request.data[k2s(Key::Active)].toBool() ? FeatureProviderInterface::Operation::Start
																	 : FeatureProviderInterface::Operation::Stop;
	const auto arguments = request.data[k2s(Key::Arguments)].toMap();

	runInCoreThread([&] {
		VeyonCore::featureManager().controlFeature(Feature::Uid{feature}, operation, arguments, {connection->controlInterface()});
	});

	return {};
}



WebApiController::Response WebApiController::controlFeature( const Request& request, const QString& feature )
{
	m_apiTotalRequestsCounter++;

	Response checkResponse{};
	if( ( checkResponse = checkConnection( request ) ).error != Error::NoError ||
		( checkResponse = checkFeature( feature ) ).error != Error::NoError )
	{
		return checkResponse;
	}

	const auto operationName = request.data.value(k2s(Key::Operation)).toString();
	const auto validOperation = operationName == QStringLiteral("initialize") ||
		operationName == QStringLiteral("start") || operationName == QStringLiteral("stop");
	const auto operation = operationName == QStringLiteral("initialize") ? FeatureProviderInterface::Operation::Initialize :
		operationName == QStringLiteral("start") ? FeatureProviderInterface::Operation::Start :
		FeatureProviderInterface::Operation::Stop;
	if( validOperation == false )
	{
		return Error::InvalidData;
	}

	const auto connection = lookupConnection( request );
	ComputerControlInterfaceList controlInterfaces{connection->controlInterface()};
	const auto targetValues = request.data.value(k2s(Key::TargetConnectionUids)).toList();
	if( targetValues.isEmpty() == false )
	{
		QReadLocker connectionsReadLocker{&m_connectionsLock};
		for( const auto& targetValue : targetValues )
		{
			const auto targetConnection = m_connections.value(QUuid{targetValue.toString()});
			if( targetConnection.isNull() )
			{
				return Error::InvalidConnection;
			}
			const auto targetInterface = targetConnection->controlInterface();
			if( controlInterfaces.contains(targetInterface) == false )
			{
				controlInterfaces.append(targetInterface);
			}
		}
	}
	const auto arguments = request.data.value(k2s(Key::Arguments)).toMap();
	runInCoreThread([&] {
		VeyonCore::featureManager().controlFeature(Feature::Uid{feature}, operation, arguments,
											  controlInterfaces);
	});

	return QVariantMap{
		{ k2s(Key::Feature), feature },
		{ k2s(Key::Operation), operationName },
	};
}



WebApiController::Response WebApiController::getFeatureStatus( const Request& request, const QString& feature )
{
	m_apiTotalRequestsCounter++;

	Response checkResponse{};
	if( ( checkResponse = checkConnection( request ) ).error != Error::NoError ||
		( checkResponse = checkFeature( feature ) ).error != Error::NoError )
	{
		return checkResponse;
	}

	const auto connection = lookupConnection( request );
	const auto controlInterface = connection->controlInterface();

	const auto result = controlInterface->activeFeatures().contains(Feature::Uid{feature});
	QVariantMap status;
	runInCoreThread([&] {
		status = VeyonCore::featureManager().featureStatus(Feature::Uid{feature}, controlInterface);
	});
	status.insert(k2s(Key::Active), result);
	return status;
}



WebApiController::Response WebApiController::getUserInformation( const Request& request )
{
	m_apiTotalRequestsCounter++;

	Response checkResponse{};
	if( ( checkResponse = checkConnection( request ) ).error != Error::NoError )
	{
		return checkResponse;
	}

	const auto connection = lookupConnection( request );
	const auto controlInterface = connection->controlInterface();

	const auto& userLoginName = controlInterface->userLoginName();
	auto userFullName = controlInterface->userFullName();
	if (userLoginName.isEmpty())
	{
		userFullName.clear();
	}

	return QVariantMap{
		{
			{k2s(Key::Login), userLoginName},
			{k2s(Key::FullName), userFullName}
		}
	};
}



WebApiController::Response WebApiController::getSessionInformation(const Request& request)
{
	m_apiTotalRequestsCounter++;

	Response checkResponse{};
	if((checkResponse = checkConnection(request)).error != Error::NoError)
	{
		return checkResponse;
	}

	const auto connection = lookupConnection(request);
	const auto controlInterface = connection->controlInterface();

	return QVariantMap{
		{
			{k2s(Key::SessionId), controlInterface->sessionInfo().id},
			{k2s(Key::SessionUptime), controlInterface->sessionInfo().uptime},
			{k2s(Key::SessionClientAddress), controlInterface->sessionInfo().clientAddress},
			{k2s(Key::SessionClientName), controlInterface->sessionInfo().clientName},
			{k2s(Key::SessionHostName), controlInterface->sessionInfo().hostName},
		}
	};
}



WebApiController::Response WebApiController::getCrashReports( const Request& request )
{
	m_apiTotalRequestsCounter++;

	Response checkResponse{};
	if( ( checkResponse = checkConnection( request ) ).error != Error::NoError )
	{
		return checkResponse;
	}

	QVariantList reports;
	const QDir dir( crashSpoolDirectory() );
	const auto files = dir.entryInfoList( { QStringLiteral("crash-*.json") }, QDir::Files, QDir::Time );
	reports.reserve( files.size() );

	for( const auto& fileInfo : files )
	{
		QFile f( fileInfo.absoluteFilePath() );
		if( f.open( QIODevice::ReadOnly ) == false )
		{
			continue;
		}
		const auto doc = QJsonDocument::fromJson( f.readAll() );
		f.close();
		if( doc.isObject() == false )
		{
			continue;
		}

		auto obj = doc.object();
		obj.insert( QStringLiteral("id"), fileInfo.completeBaseName() );
		obj.insert( QStringLiteral("reportSize"), fileInfo.size() );
		reports.append( obj.toVariantMap() );
	}

	return reports;
}



WebApiController::Response WebApiController::getCrashReportDump( const Request& request, const QString& id )
{
	m_apiTotalRequestsCounter++;

	Response checkResponse{};
	if( ( checkResponse = checkConnection( request ) ).error != Error::NoError )
	{
		return checkResponse;
	}

	if( isValidCrashReportId( id ) == false )
	{
		return Error::InvalidData;
	}

	const QDir dir( crashSpoolDirectory() );
	// Minidump Windows si présent, sinon trace brute Linux.
	for( const auto& extension : { QStringLiteral(".dmp"), QStringLiteral(".trace") } )
	{
		QFile f( dir.absoluteFilePath( id + extension ) );
		if( f.exists() && f.open( QIODevice::ReadOnly ) )
		{
			const auto data = f.readAll();
			f.close();
			return Response{ data };
		}
	}

	return Error::InvalidData;
}



WebApiController::Response WebApiController::deleteCrashReport( const Request& request, const QString& id )
{
	m_apiTotalRequestsCounter++;

	Response checkResponse{};
	if( ( checkResponse = checkConnection( request ) ).error != Error::NoError )
	{
		return checkResponse;
	}

	if( isValidCrashReportId( id ) == false )
	{
		return Error::InvalidData;
	}

	const QDir dir( crashSpoolDirectory() );
	const auto siblings = dir.entryInfoList( { id + QStringLiteral(".*") }, QDir::Files );
	for( const auto& sibling : siblings )
	{
		QFile::remove( sibling.absoluteFilePath() );
	}

	return QVariantMap{ { QStringLiteral("deleted"), true } };
}



QString WebApiController::getStatistics()
{
	QReadLocker connectionsLocker{&m_connectionsLock};

	return QStringLiteral("Total API requests: %1 (%2/s in the past %3 s)\n<br/>").arg(int(m_apiTotalRequestsCounter)).arg(int(m_apiTotalRequestsPerSecond)).arg(int(StatisticsUpdateIntervalSeconds)) +
			QStringLiteral("Framebuffer requests: %1 (%2/s in the past %3 s)\n<br/>").arg(int(m_framebufferRequestsCounter)).arg(int(m_framebufferRequestsPerSecond)).arg(int(StatisticsUpdateIntervalSeconds)) +
			QStringLiteral("VNC framebuffer updates: %1 (%2/s in the past %3 s)\n<br/>").arg(int(m_vncFramebufferUpdatesCounter)).arg(int(m_vncFramebufferUpdatesPerSecond)).arg(int(StatisticsUpdateIntervalSeconds)) +
			QStringLiteral("Number of client connections: %1<br/>\n").arg(m_connections.count());
}



QString WebApiController::getConnectionDetails()
{
	QReadLocker connectionsLocker{&m_connectionsLock};

	QStringList columns {QStringLiteral("Connection UUID"),
					   QStringLiteral("State"),
					   QStringLiteral("Host"),
					   QStringLiteral("User"),
					   QStringLiteral("Server version")
					  };
	QList<QStringList> rows;
	rows.reserve(m_connections.count());
	for (auto it = m_connections.constBegin(), end = m_connections.constEnd(); it != end; ++it)
	{
		const auto connection = it.value();

		rows.append({it.key().toString(QUuid::WithoutBraces),
					 EnumHelper::toString(connection->controlInterface()->state()).toHtmlEscaped(),
					 connection->controlInterface()->computer().hostName().toHtmlEscaped(),
					 connection->controlInterface()->userLoginName().toHtmlEscaped(),
					 EnumHelper::toString(connection->controlInterface()->serverVersion()).toHtmlEscaped(),
					});
	}

	const auto rowToString = [](const QStringList& row, const QString& tag) {
		return std::accumulate(row.constBegin(), row.constEnd(), QString{}, [&tag](const QString& acc, const QString& cell) -> QString {
			return acc + QStringLiteral("<%1>%2</%1>").arg(tag, cell);
		});
	};

	const auto tableHeader = QStringLiteral("<tr>%1</tr>\n").arg(rowToString(columns, QStringLiteral("th")));
	const auto tableBody = std::accumulate(rows.constBegin(), rows.constEnd(), QString{},
										   [&](const QString& acc, const QStringList& row) -> QString {
		return acc + QStringLiteral("<tr>%1</tr>\n").arg(rowToString(row, QStringLiteral("td")));
	});

	return QStringLiteral("<table border=\"1\">\n%1%2</table>\n").arg(tableHeader, tableBody);
}



WebApiController::Response WebApiController::sleep(const Request& request, const int& seconds) // clazy:exclude=function-args-by-value
{
	Q_UNUSED(request)

	m_apiTotalRequestsCounter++;
	QThread::sleep(seconds);

	return {""};
}


QString WebApiController::errorString( WebApiController::Error error )
{
	switch( error )
	{
	case Error::NoError: return {};
	case Error::InvalidData: return QStringLiteral("Invalid data");
	case Error::InvalidConnection: return QStringLiteral("Invalid connection");
	case Error::InvalidFeature: return QStringLiteral("Invalid feature");
	case Error::AuthenticationMethodNotAvailable: return QStringLiteral("Authentication method not offered by server");
	case Error::InvalidCredentials: return QStringLiteral("Invalid or incomplete credentials");
	case Error::AuthenticationFailed: return QStringLiteral("Authentication failed");
	case Error::ConnectionLimitReached: return QStringLiteral("Limit for maximum number of connections reached");
	case Error::ConnectionTimedOut: return QStringLiteral("Connection timed out");
	case Error::UnsupportedImageFormat: return QStringLiteral("Unsupported image format");
	case Error::FramebufferNotAvailable: return QStringLiteral("Framebuffer not yet available");
	case Error::FramebufferEncodingError: return QStringLiteral("Framebuffer encoding error");
	case Error::ProtocolMismatch: return QStringLiteral("Protocol mismatch error");
	case Error::ConnectionNotReady: return QStringLiteral("Connection is not ready for remote control");
	}

	return {};
}



void WebApiController::runInWorkerThread(const std::function<void()>& functor) const
{
	QMetaObject::invokeMethod(m_workerObject, functor, Qt::BlockingQueuedConnection);
}



void WebApiController::runInWorkerThreadNonBlocking(const std::function<void()>& functor) const
{
	QMetaObject::invokeMethod(m_workerObject, functor, Qt::QueuedConnection);
}



void WebApiController::runInCoreThread(const std::function<void()>& functor)
{
	const auto core = VeyonCore::instance();
	if( QThread::currentThread() == core->thread() )
	{
		functor();
		return;
	}

	QMetaObject::invokeMethod(core, functor, Qt::BlockingQueuedConnection);
}



template<class T>
T WebApiController::runInWorkerThread(const std::function<T()>& functor) const
{
	T retval{};
	QMetaObject::invokeMethod(m_workerObject, functor, Qt::BlockingQueuedConnection, &retval );
	return retval;
}



void WebApiController::removeConnection( QUuid connectionUuid )
{
	QWriteLocker connectionsWriteLocker{ &m_connectionsLock };

	// deleter functor automatically performs actual deletion in worker thread
	m_connections.remove(connectionUuid);
}



void WebApiController::incrementVncFramebufferUpdatesCounter()
{
	++m_vncFramebufferUpdatesCounter;
}



void WebApiController::updateStatistics()
{
	m_apiTotalRequestsPerSecond = (m_apiTotalRequestsCounter - m_apiTotalRequestsLast) / StatisticsUpdateIntervalSeconds;
	m_framebufferRequestsPerSecond = (m_framebufferRequestsCounter - m_framebufferRequestsLast) / StatisticsUpdateIntervalSeconds;
	m_vncFramebufferUpdatesPerSecond = (m_vncFramebufferUpdatesCounter - m_vncFramebufferUpdatesLast) / StatisticsUpdateIntervalSeconds;

	m_apiTotalRequestsLast = m_apiTotalRequestsCounter;
	m_framebufferRequestsLast = m_framebufferRequestsCounter;
	m_vncFramebufferUpdatesLast = m_vncFramebufferUpdatesCounter;
}



QByteArray WebApiController::lookupHeaderField(const Request& request, const QByteArray& fieldName)
{
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
	for (const auto& h : request.headers)
	{
		if (h.first.compare(fieldName, Qt::CaseInsensitive) == 0)
		{
			return h.second;
		}
	}
#else
	const auto fieldNameString = QString::fromUtf8(fieldName);
	if (request.headers.contains(fieldNameString))
	{
		return request.headers[fieldNameString].toByteArray();
	}

	for (auto it = request.headers.constBegin(), end = request.headers.constEnd(); it != end; ++it)
	{
		if (it.key().compare(fieldNameString, Qt::CaseInsensitive ) == 0)
		{
			return it.value().toByteArray();
		}
	}
#endif

	return {};
}



WebApiController::LockingConnectionPointer WebApiController::lookupConnection( const Request& request )
{
	QReadLocker connectionsReadLocker{&m_connectionsLock};

	return m_connections.value(QUuid{lookupHeaderField(request, connectionUidHeaderFieldName())});
}



WebApiController::Response WebApiController::lookupBroadcastConnections(
	const Request& request, ComputerControlInterface::Pointer& source, ComputerControlInterfaceList& targets )
{
	const QUuid sourceUuid{lookupHeaderField(request, connectionUidHeaderFieldName())};
	const auto targetValues = request.data.value(k2s(Key::TargetConnectionUids)).toList();
	if( sourceUuid.isNull() || targetValues.isEmpty() )
	{
		return Error::InvalidData;
	}

	QReadLocker connectionsReadLocker{&m_connectionsLock};
	const auto sourceConnection = m_connections.value(sourceUuid);
	if( sourceConnection.isNull() )
	{
		return Error::InvalidConnection;
	}
	source = sourceConnection->controlInterface();

	for( const auto& targetValue : targetValues )
	{
		const QUuid targetUuid{targetValue.toString()};
		const auto targetConnection = m_connections.value(targetUuid);
		if( targetUuid.isNull() || targetConnection.isNull() )
		{
			targets.clear();
			return Error::InvalidConnection;
		}
		if( targetUuid != sourceUuid )
		{
			const auto targetInterface = targetConnection->controlInterface();
			if( targetInterface->state() != ComputerControlInterface::State::Connected )
			{
				targets.clear();
				return Error::ConnectionNotReady;
			}
			if( targets.contains(targetInterface) == false )
			{
				targets.append(targetInterface);
			}
		}
	}

	if( targets.isEmpty() )
	{
		return Error::InvalidData;
	}

	return {};
}



WebApiController::Response WebApiController::checkConnection( const Request& request )
{
	const QUuid connectionUuid{lookupHeaderField(request, connectionUidHeaderFieldName())};

	return runInWorkerThread<WebApiController::Response>([=, this]() -> WebApiController::Response {
		m_connectionsLock.lockForRead();
		if( connectionUuid.isNull() || m_connections.contains( connectionUuid ) == false )
		{
			m_connectionsLock.unlock();
			return Error::InvalidConnection;
		}

		const auto connection = std::as_const(m_connections)[connectionUuid];
		m_connectionsLock.unlock();

		connection->lock();

		const auto idleTimer = connection->idleTimer();
		idleTimer->stop();
		idleTimer->start();

		connection->unlock();

		return {};
	} );
}



ComputerControlInterface::Pointer WebApiController::lookupConnectionByUid( const QUuid& connectionUuid )
{
	return runInWorkerThread<ComputerControlInterface::Pointer>([=, this]() -> ComputerControlInterface::Pointer {
		m_connectionsLock.lockForRead();
		if( connectionUuid.isNull() || m_connections.contains( connectionUuid ) == false )
		{
			m_connectionsLock.unlock();
			return {};
		}

		const auto connection = std::as_const(m_connections)[connectionUuid];
		m_connectionsLock.unlock();

		connection->lock();

		const auto idleTimer = connection->idleTimer();
		idleTimer->stop();
		idleTimer->start();

		connection->unlock();

		return connection->controlInterface();
	} );
}



WebApiController::Response WebApiController::checkFeature( const QString& featureUid )
{
	if( VeyonCore::featureManager().feature(Feature::Uid{featureUid}).isValid() == false )
	{
		return Error::InvalidFeature;
	}

	return {};
}
