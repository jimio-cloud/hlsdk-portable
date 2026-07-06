/***
*
*   Half-Life deathmatch bot support.
*
****/
#pragma once
#if !defined( BOT_H )
#define BOT_H

#include "extdll.h"

extern cvar_t bot_quota;
extern cvar_t bot_skill;
extern cvar_t bot_allow_chat;

void BotRegisterCvars( void );
void BotStartFrame( void );
void BotClientDisconnect( edict_t *pEntity );
BOOL BotClientCommand( edict_t *pEntity, const char *pcmd );

#endif // BOT_H
