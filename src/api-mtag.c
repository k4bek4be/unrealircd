/************************************************************************
 *   UnrealIRCd - Unreal Internet Relay Chat Daemon - src/api-mtag.c
 *   (c) 2019- Bram Matthys and The UnrealIRCd team
 *
 *   See file AUTHORS in IRC package for additional names of
 *   the programmers. 
 *
 *   This program is free software; you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation; either version 1, or (at your option)
 *   any later version.
 *
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with this program; if not, write to the Free Software
 *   Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */

#include "unrealircd.h"

MODVAR MessageTagHandler *mtaghandlers = NULL; /**< List of message tag handlers */

void mtag_handler_init(void)
{
}

/**
 * Returns a message tag handler based on the given token name.
 *
 * @param token The message-tag token to search for.
 * @return Returns the handle to the message tag handler,
 *         or NULL if not found.
 */
MessageTagHandler *MessageTagHandlerFind(const char *token)
{
	MessageTagHandler *m;

	for (m = mtaghandlers; m; m = m->next)
	{
		if (!stricmp(token, m->name))
			return m;
	}
	return NULL;
}

/**
 * Adds a new message tag handler.
 *
 * @param module The module which owns this message-tag handler.
 * @param mreq   The details of the request such as token name, access check handler, etc.
 * @return Returns the handle to the new token if successful, otherwise NULL.
 *         The module's error code contains specific information about the
 *         error.
 */
MessageTagHandler *MessageTagHandlerAdd(Module *module, MessageTagHandlerInfo *mreq)
{
	MessageTagHandler *m;

	/* Some consistency checks to avoid a headache for module devs later on: */
	if ((mreq->flags & MTAG_HANDLER_FLAGS_NO_CAP_NEEDED) && mreq->clicap_handler)
	{
		ircd_log(LOG_ERROR, "MessageTagHandlerAdd(): .flags is set to MTAG_HANDLER_FLAGS_NO_CAP_NEEDED "
		                    "but a .clicap_handler is passed as well. These options are mutually "
		                    "exclusive, choose one or the other.");
		abort();
	} else if (!(mreq->flags & MTAG_HANDLER_FLAGS_NO_CAP_NEEDED) && !mreq->clicap_handler)
	{
		ircd_log(LOG_ERROR, "MessageTagHandlerAdd(): no .clicap_handler is passed. If the "
		                    "message tag really does not require a cap then you must "
		                    "set .flags to MTAG_HANDLER_FLAGS_NO_CAP_NEEDED");
		abort();
	}

	m = MessageTagHandlerFind(mreq->name);
	if (m)
	{
		if (m->unloaded)
		{
			m->unloaded = 0;
		} else {
			if (module)
				module->errorcode = MODERR_EXISTS;
			return NULL;
		}
	} else {
		/* New message tag handler */
		m = MyMallocEx(sizeof(MessageTagHandler));
		m->name = strdup(mreq->name);
	}
	/* Add or update the following fields: */
	m->owner = module;
	m->flags = mreq->flags;
	m->is_ok = mreq->is_ok;
	m->clicap_handler = mreq->clicap_handler;

	/* Update reverse dependency (if any) */
	if (m->clicap_handler)
		m->clicap_handler->mtag_handler = m;

	AddListItem(m, mtaghandlers);

	if (module)
	{
		ModuleObject *mobj = MyMallocEx(sizeof(ModuleObject));
		mobj->type = MOBJ_MTAG;
		mobj->object.mtag = m;
		AddListItem(mobj, module->objects);
		module->errorcode = MODERR_NOERROR;
	}

	return m;
}

void unload_mtag_handler_commit(MessageTagHandler *m)
{
	/* This is an unusual operation, I think we should log it. */
	ircd_log(LOG_ERROR, "Unloading message-tag handler for '%s'", m->name);
	sendto_realops("Unloading message-tag handler for '%s'", m->name);

	/* Remove reverse dependency, if any */
	if (m->clicap_handler)
		m->clicap_handler->mtag_handler = NULL;

	/* Destroy the object */
	DelListItem(m, mtaghandlers);
	safefree(m->name);
	MyFree(m);
}

/**
 * Removes the specified message tag handler.
 *
 * @param m The message tag handler to remove.
 */
void MessageTagHandlerDel(MessageTagHandler *m)
{
	if (loop.ircd_rehashing)
		m->unloaded = 1;
	else
		unload_mtag_handler_commit(m);

	if (m->owner)
	{
		ModuleObject *mobj;
		for (mobj = m->owner->objects; mobj; mobj = mobj->next) {
			if (mobj->type == MOBJ_MTAG && mobj->object.mtag == m)
			{
				DelListItem(mobj, m->owner->objects);
				MyFree(mobj);
				break;
			}
		}
		m->owner = NULL;
	}
}

void unload_all_unused_mtag_handlers(void)
{
	MessageTagHandler *m, *m_next;

	for (m = mtaghandlers; m; m = m_next)
	{
		m_next = m->next;
		if (m->unloaded)
			unload_mtag_handler_commit(m);
	}
}
