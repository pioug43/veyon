/*
 * ChatConfiguration.h - configuration values for Chat plugin
 *
 * Équivalent libre de l'addon commercial « Chat » (https://veyon.io/fr/addons/).
 * Les options sont exposées dans Veyon Configurator via ChatConfigurationPage.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 */

#pragma once

#include "VeyonConfiguration.h"
#include "Configuration/Proxy.h"

#define FOREACH_CHAT_CONFIG_PROPERTY(OP) \
	OP(ChatConfiguration, m_configuration, bool, chatEnabled, setChatEnabled, "Enabled", "Chat", true, Configuration::Property::Flag::Standard)	\
	OP(ChatConfiguration, m_configuration, bool, chatStudentsCanSendMessages, setChatStudentsCanSendMessages, "StudentsCanSendMessages", "Chat", true, Configuration::Property::Flag::Standard)	\
	OP(ChatConfiguration, m_configuration, bool, chatStudentWindowStaysOnTop, setChatStudentWindowStaysOnTop, "StudentWindowStaysOnTop", "Chat", false, Configuration::Property::Flag::Standard)	\

// clazy:excludeall=missing-qobject-macro

DECLARE_CONFIG_PROXY(ChatConfiguration, FOREACH_CHAT_CONFIG_PROPERTY)
