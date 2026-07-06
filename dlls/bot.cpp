/***
*
*   Half-Life deathmatch bot support.
*
****/
#include "extdll.h"
#include "util.h"
#include "cbase.h"
#include "player.h"
#include "client.h"
#include "gamerules.h"
#include "game.h"
#include "bot.h"
#include "weapons.h"
#include "usercmd.h"

cvar_t bot_quota = { "bot_quota", "0", FCVAR_SERVER };
cvar_t bot_skill = { "bot_skill", "3", FCVAR_SERVER };
cvar_t bot_allow_chat = { "bot_allow_chat", "1", FCVAR_SERVER };

namespace
{
constexpr int MAX_BOTS = 32;
constexpr float BOT_THINK_INTERVAL = 0.05f;
constexpr float BOT_ITEM_SCAN_INTERVAL = 0.35f;
constexpr float BOT_REACTION_BASE = 0.34f;

template <typename T>
T BotClamp( T value, T minValue, T maxValue )
{
	return value < minValue ? minValue : ( value > maxValue ? maxValue : value );
}

struct BotBrain
{
	edict_t *edict = NULL;
	float nextThink = 0;
	float nextItemScan = 0;
	float nextChat = 0;
	float firstSeenEnemy = 0;
	float lastStrafeChange = 0;
	float stuckCheckTime = 0;
	Vector stuckOrigin = g_vecZero;
	Vector roamGoal = g_vecZero;
	EHANDLE enemy;
	EHANDLE itemGoal;
	float strafe = 0;
	float aggression = 0.75f;
};

BotBrain g_Bots[MAX_BOTS];
int g_BotSequence = 0;

const char *const BOT_NAMES[] =
{
	"Lambda Reaper", "Gluon Goblin", "Tau Cannon", "Crossfire", "Bounce Pad",
	"Vortigaunt Jr", "Satchel Sage", "Crowbar Hero", "Hazard Course", "Anomalous"
};

void BotSay( BotBrain &bot, const char *text )
{
	if( bot_allow_chat.value == 0 || bot.nextChat > gpGlobals->time )
		return;

	bot.nextChat = gpGlobals->time + RANDOM_FLOAT( 18.0f, 40.0f );
	SERVER_COMMAND( UTIL_VarArgs( "say \"%s: %s\"\n", STRING( bot.edict->v.netname ), text ) );
}

int BotCount()
{
	int count = 0;
	for( int i = 0; i < MAX_BOTS; ++i )
		if( g_Bots[i].edict && !g_Bots[i].edict->free )
			++count;
	return count;
}

BotBrain *FindBot( edict_t *edict )
{
	for( int i = 0; i < MAX_BOTS; ++i )
		if( g_Bots[i].edict == edict )
			return &g_Bots[i];
	return NULL;
}

bool IsAlivePlayer( CBaseEntity *entity )
{
	return entity && entity->IsPlayer() && entity->IsAlive() && !FBitSet( entity->pev->flags, FL_NOTARGET );
}

bool CanSee( edict_t *self, CBaseEntity *target )
{
	TraceResult tr;
	UTIL_TraceLine( self->v.origin + self->v.view_ofs, target->EyePosition(), dont_ignore_monsters, self, &tr );
	return tr.flFraction >= 0.98f || tr.pHit == target->edict();
}

CBaseEntity *ChooseEnemy( BotBrain &bot )
{
	CBaseEntity *best = NULL;
	float bestScore = 999999999.0f;
	CBaseEntity *player = NULL;
	while( ( player = UTIL_FindEntityByClassname( player, "player" ) ) != NULL )
	{
		if( player->edict() == bot.edict || !IsAlivePlayer( player ) )
			continue;

		CBaseEntity *self = CBaseEntity::Instance( bot.edict );
		if( g_pGameRules->PlayerRelationship( self, player ) == GR_TEAMMATE )
			continue;

		const float distance = ( player->pev->origin - bot.edict->v.origin ).Length();
		if( distance > 4096.0f || !CanSee( bot.edict, player ) )
			continue;

		const float score = distance - player->pev->frags * 24.0f + player->pev->health * 2.0f;
		if( score < bestScore )
		{
			bestScore = score;
			best = player;
		}
	}
	return best;
}

bool IsUsefulPickup( CBaseEntity *entity )
{
	if( !entity || entity->pev->effects & EF_NODRAW || entity->pev->solid == SOLID_NOT )
		return false;

	const char *classname = STRING( entity->pev->classname );
	return !strncmp( classname, "weapon_", 7 ) || !strncmp( classname, "ammo_", 5 ) ||
		FStrEq( classname, "item_healthkit" ) || FStrEq( classname, "item_battery" ) || FStrEq( classname, "item_longjump" );
}

CBaseEntity *ChooseItem( BotBrain &bot )
{
	CBaseEntity *best = NULL;
	float bestScore = 999999999.0f;
	CBaseEntity *entity = NULL;
	while( ( entity = UTIL_FindEntityInSphere( entity, bot.edict->v.origin, 1600.0f ) ) != NULL )
	{
		if( !IsUsefulPickup( entity ) )
			continue;

		TraceResult tr;
		UTIL_TraceLine( bot.edict->v.origin + bot.edict->v.view_ofs, entity->Center(), ignore_monsters, bot.edict, &tr );
		if( tr.flFraction < 0.75f )
			continue;

		float score = ( entity->pev->origin - bot.edict->v.origin ).Length();
		const char *classname = STRING( entity->pev->classname );
		if( !strncmp( classname, "weapon_", 7 ) )
			score -= 450.0f;
		else if( FStrEq( classname, "item_healthkit" ) && bot.edict->v.health < 65 )
			score -= 350.0f;
		else if( FStrEq( classname, "item_battery" ) && bot.edict->v.armorvalue < 45 )
			score -= 250.0f;

		if( score < bestScore )
		{
			bestScore = score;
			best = entity;
		}
	}
	return best;
}

void PickCombatWeapon( CBasePlayer *player, float distance )
{
	if( distance > 850 && player->HasNamedPlayerItem( "weapon_gauss" ) ) player->SelectItem( "weapon_gauss" );
	else if( distance > 650 && player->HasNamedPlayerItem( "weapon_crossbow" ) ) player->SelectItem( "weapon_crossbow" );
	else if( distance > 450 && player->HasNamedPlayerItem( "weapon_rpg" ) ) player->SelectItem( "weapon_rpg" );
	else if( distance > 300 && player->HasNamedPlayerItem( "weapon_9mmAR" ) ) player->SelectItem( "weapon_9mmAR" );
	else if( distance < 280 && player->HasNamedPlayerItem( "weapon_shotgun" ) ) player->SelectItem( "weapon_shotgun" );
	else if( player->HasNamedPlayerItem( "weapon_357" ) ) player->SelectItem( "weapon_357" );
	else if( player->HasNamedPlayerItem( "weapon_9mmhandgun" ) ) player->SelectItem( "weapon_9mmhandgun" );
}

void RunBot( BotBrain &bot )
{
	if( !bot.edict || bot.edict->free || !bot.edict->pvPrivateData )
		return;

	CBasePlayer *player = GetClassPtr( (CBasePlayer *)&bot.edict->v );
	if( !player->IsAlive() )
	{
		g_engfuncs.pfnRunPlayerMove( bot.edict, bot.edict->v.v_angle, 0, 0, 0, IN_ATTACK, 0, 50 );
		return;
	}

	if( bot.nextItemScan <= gpGlobals->time )
	{
		bot.nextItemScan = gpGlobals->time + BOT_ITEM_SCAN_INTERVAL;
		bot.itemGoal = ChooseItem( bot );
	}

	CBaseEntity *enemy = ChooseEnemy( bot );
	if( enemy != bot.enemy )
	{
		bot.enemy = enemy;
		bot.firstSeenEnemy = gpGlobals->time;
		if( enemy && RANDOM_LONG( 0, 7 ) == 0 )
			BotSay( bot, "target acquired" );
	}

	Vector goal;
	bool attacking = false;
	unsigned short buttons = 0;
	float forward = 420.0f;
	float side = bot.strafe;

	if( enemy )
	{
		goal = enemy->EyePosition();
		Vector toEnemy = goal - ( bot.edict->v.origin + bot.edict->v.view_ofs );
		float distance = toEnemy.Length();
		PickCombatWeapon( player, distance );

		const float skill = BotClamp( bot_skill.value, 0.0f, 5.0f );
		const float reaction = Q_max( 0.03f, BOT_REACTION_BASE - skill * 0.055f );
		attacking = gpGlobals->time - bot.firstSeenEnemy >= reaction;
		forward = distance < 240.0f ? -220.0f : ( distance > 900.0f ? 420.0f : 120.0f );

		if( bot.lastStrafeChange <= gpGlobals->time )
		{
			bot.lastStrafeChange = gpGlobals->time + RANDOM_FLOAT( 0.45f, 1.15f );
			bot.strafe = RANDOM_LONG( 0, 1 ) ? 360.0f : -360.0f;
		}
		side = bot.strafe;
		if( attacking )
			buttons |= IN_ATTACK;
		if( RANDOM_FLOAT( 0.0f, 1.0f ) < 0.035f + skill * 0.01f )
			buttons |= IN_JUMP;
	}
	else if( bot.itemGoal )
	{
		goal = bot.itemGoal->Center();
	}
	else
	{
		if( ( bot.roamGoal - bot.edict->v.origin ).Length() < 96.0f || bot.roamGoal.Length() == 0 )
		{
			bot.roamGoal = bot.edict->v.origin + Vector( RANDOM_FLOAT( -900, 900 ), RANDOM_FLOAT( -900, 900 ), RANDOM_FLOAT( -64, 128 ) );
		}
		goal = bot.roamGoal;
	}

	Vector aimDir = goal - ( bot.edict->v.origin + bot.edict->v.view_ofs );
	Vector viewAngles = UTIL_VecToAngles( aimDir );
	viewAngles.x = -viewAngles.x;
	viewAngles.z = 0;

	if( bot.stuckCheckTime <= gpGlobals->time )
	{
		if( ( bot.edict->v.origin - bot.stuckOrigin ).Length() < 24.0f )
			buttons |= IN_JUMP;
		bot.stuckOrigin = bot.edict->v.origin;
		bot.stuckCheckTime = gpGlobals->time + 0.65f;
	}

	bot.edict->v.v_angle = viewAngles;
	bot.edict->v.angles = viewAngles;
	g_engfuncs.pfnRunPlayerMove( bot.edict, viewAngles, forward, side, 0, buttons, 0, 50 );
}

bool AddBot()
{
	BotBrain *slot = NULL;
	for( int i = 0; i < MAX_BOTS; ++i )
	{
		if( !g_Bots[i].edict || g_Bots[i].edict->free )
		{
			slot = &g_Bots[i];
			break;
		}
	}
	if( !slot )
		return false;

	const char *baseName = BOT_NAMES[g_BotSequence % ( sizeof( BOT_NAMES ) / sizeof( BOT_NAMES[0] ) )];
	edict_t *edict = g_engfuncs.pfnCreateFakeClient( UTIL_VarArgs( "%s %d", baseName, g_BotSequence + 1 ) );
	if( FNullEnt( edict ) )
		return false;

	edict->v.flags |= FL_FAKECLIENT;
	ClientPutInServer( edict );
	*slot = BotBrain();
	slot->edict = edict;
	slot->aggression = RANDOM_FLOAT( 0.55f, 1.0f );
	++g_BotSequence;
	return true;
}

void KickOneBot()
{
	for( int i = MAX_BOTS - 1; i >= 0; --i )
	{
		if( g_Bots[i].edict && !g_Bots[i].edict->free )
		{
			ClientDisconnect( g_Bots[i].edict );
			REMOVE_ENTITY( g_Bots[i].edict );
			g_Bots[i] = BotBrain();
			return;
		}
	}
}
}

void BotRegisterCvars( void )
{
	CVAR_REGISTER( &bot_quota );
	CVAR_REGISTER( &bot_skill );
	CVAR_REGISTER( &bot_allow_chat );
}

void BotStartFrame( void )
{
	if( !g_pGameRules || !g_pGameRules->IsDeathmatch() )
		return;

	int quota = BotClamp( (int)bot_quota.value, 0, MAX_BOTS );
	while( BotCount() < quota && AddBot() ) {}
	while( BotCount() > quota ) KickOneBot();

	for( int i = 0; i < MAX_BOTS; ++i )
	{
		if( g_Bots[i].edict && g_Bots[i].nextThink <= gpGlobals->time )
		{
			g_Bots[i].nextThink = gpGlobals->time + BOT_THINK_INTERVAL;
			RunBot( g_Bots[i] );
		}
	}
}

void BotClientDisconnect( edict_t *pEntity )
{
	BotBrain *bot = FindBot( pEntity );
	if( bot )
		*bot = BotBrain();
}

BOOL BotClientCommand( edict_t *pEntity, const char *pcmd )
{
	if( FStrEq( pcmd, "bot_add" ) )
	{
		if( AddBot() )
			CVAR_SET_STRING( "bot_quota", UTIL_dtos1( BotCount() ) );
		return TRUE;
	}
	if( FStrEq( pcmd, "bot_kick" ) )
	{
		KickOneBot();
		CVAR_SET_STRING( "bot_quota", UTIL_dtos1( BotCount() ) );
		return TRUE;
	}
	if( FStrEq( pcmd, "bot_count" ) )
	{
		CLIENT_PRINTF( pEntity, print_console, UTIL_VarArgs( "%d Half-Life deathmatch bots active\n", BotCount() ) );
		return TRUE;
	}
	return FALSE;
}
