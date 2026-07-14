if(NOT WITH_WEBAPI)
	return()
endif()

if(WITH_QT6)
	find_package(Qt6 COMPONENTS HttpServer WebSockets)
else()
	find_package(Qt5HttpServer)
	find_package(Qt5WebSockets)
endif()

if(Qt6HttpServer_DIR OR Qt5HttpServer_DIR OR (Qt5Core_VERSION VERSION_GREATER 5.12.0 AND TARGET Qt5::CorePrivate AND TARGET Qt5::NetworkPrivate))

	include(BuildVeyonPlugin)

	build_veyon_plugin(webapi
		NAME WebApi
		SOURCES
		WebApiPlugin.cpp
		WebApiPlugin.h
		WebApiAuthenticationProxy.cpp
		WebApiAuthenticationProxy.h
		WebApiConfiguration.h
		WebApiConfigurationPage.cpp
		WebApiConfigurationPage.h
		WebApiConfigurationPage.ui
		WebApiConnection.cpp
		WebApiConnection.h
		WebApiController.cpp
		WebApiController.h
		WebApiHttpServer.cpp
		WebApiHttpServer.h
		webapi.qrc
		)

	set_source_files_properties(WebApiHttpServer.cpp PROPERTIES SKIP_UNITY_BUILD_INCLUSION TRUE SKIP_PRECOMPILE_HEADERS TRUE)

	if(Qt6HttpServer_DIR)
		target_link_libraries(webapi PRIVATE Qt6::HttpServer)
	elseif(Qt5HttpServer_DIR)
		target_link_libraries(webapi PRIVATE Qt5::HttpServer)
	else()
		add_subdirectory(qthttpserver)

		target_link_libraries(webapi PRIVATE qthttpserver)
	endif()

	# Pont VNC<->WebSocket pour noVNC : compilé UNIQUEMENT si le module Qt
	# WebSockets est disponible. Sinon le plugin Web API se construit sans le
	# pont (il n'est requis que sur l'hôte broker/portail) — ainsi les distros
	# dont l'image CT n'a pas Qt*WebSockets continuent de compiler.
	if(Qt6WebSockets_DIR OR Qt5WebSockets_DIR)
		target_sources(webapi PRIVATE
			WebApiVncBridge.cpp
			WebApiVncBridge.h
			WebApiWebSocketServer.cpp
			WebApiWebSocketServer.h
			)
		target_compile_definitions(webapi PRIVATE WEBAPI_HAVE_WEBSOCKETS)
		if(Qt6WebSockets_DIR)
			target_link_libraries(webapi PRIVATE Qt6::WebSockets)
		else()
			target_link_libraries(webapi PRIVATE Qt5::WebSockets)
		endif()
	endif()

else()

	message(FATAL_ERROR "WebAPI plugin is enabled (WITH_WEBAPI=ON) but no Qt HTTP server module was found. Install the Qt HttpServer development package (Qt6::HttpServer or Qt5::HttpServer), or pass -DWITH_WEBAPI=OFF to disable the plugin.")

endif()
