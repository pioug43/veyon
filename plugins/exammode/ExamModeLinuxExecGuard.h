/*
 * ExamModeLinuxExecGuard.h - kernel-mediated launch prevention on Linux
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

#include <QMutex>
#include <QSet>
#include <QStringList>
#include <QThread>

#include <atomic>

/**
 * Empêche le LANCEMENT d'exécutables sous Linux via fanotify FAN_OPEN_EXEC_PERM :
 * le noyau soumet chaque execve à une autorisation userspace. Contrairement à la
 * boucle kill, le blocage s'applique AVANT l'exécution et couvre tous les chemins
 * de lancement (shell, lanceur graphique, chemin absolu, copie portable de même
 * nom), sans dépendre du PATH ni de la coopération de l'application.
 *
 * Nécessite les privilèges root (CAP_SYS_ADMIN) et un noyau >= 5.0. Fail-safe :
 * en cas d'échec d'initialisation, startGuarding() renvoie false et l'appelant se
 * rabat sur la boucle kill ; si le processus meurt, le descripteur fanotify est
 * fermé par le noyau et les execve en attente sont autorisés (aucun blocage
 * durable de la machine). Le filtrage se fait par nom de fichier (basename) :
 * renommer un binaire contourne la règle (limite documentée).
 *
 * En dehors de Linux, toutes les méthodes sont des non-op (startGuarding renvoie
 * false) afin que le plugin compile sur toutes les plateformes.
 */
class ExamModeLinuxExecGuard : public QThread
{
	Q_OBJECT
public:
	explicit ExamModeLinuxExecGuard( QObject* parent = nullptr );
	~ExamModeLinuxExecGuard() override;

	// Démarre la surveillance. Renvoie false si fanotify est indisponible
	// (privilèges insuffisants, noyau trop ancien, plateforme non Linux).
	bool startGuarding( const QStringList& blockedBasenames );
	// Met à jour la liste des binaires bloqués sans redémarrer la surveillance.
	void updateBlocked( const QStringList& blockedBasenames );
	// Arrête proprement la surveillance et attend la fin du thread.
	void stopGuarding();
	bool refreshMounts();
	bool isActive() const { return m_active.load(); }
	bool isHealthy() const { return m_healthy.load(); }

protected:
	void run() override;

private:
	QSet<QString> blockedSnapshot();
	static QString normalizeBasename( const QString& name );

	QMutex m_mutex;
	QSet<QString> m_blocked;
	int m_fanotifyFd{-1};
	int m_stopFd{-1};
	std::atomic_bool m_active{false};
	std::atomic_bool m_healthy{false};
	std::atomic_bool m_stopping{false};
};
