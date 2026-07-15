/*
 * CrashHandler.h - process-wide crash capture writing rich reports to a spool
 *
 * Copyright (c) 2026 Pierrick Belledent
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

#pragma once

#include <QString>

// Installe des gestionnaires de crash à l'échelle du processus. En cas de
// crash (exception non gérée sous Windows, signal fatal sous POSIX), un
// rapport riche (JSON + minidump Windows / backtrace Linux) est écrit dans
// spoolDir pour être collecté plus tard par le portail via la Web API.
namespace CrashHandler
{

// componentName : nom lisible du processus (Server, Worker, Service, ...),
// intégré au rapport. logDirectory : dossier des journaux Veyon (une queue
// du journal courant est jointe au rapport). Idempotent (1re installation
// seulement). À appeler tôt (au chargement du plugin).
void install( const QString& spoolDir, const QString& componentName, const QString& logDirectory );

}
