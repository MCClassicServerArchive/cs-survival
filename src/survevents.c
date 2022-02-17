#include <core.h>
#include <world.h>
#include <event.h>
#include <client.h>
#include <protocol.h>
#include <csmath.h>
#include "survdata.h"
#include "survevents.h"
#include "survgui.h"
#include "survhacks.h"
#include "survinv.h"
#include "survdmg.h"
#include "survbrk.h"
#include "survfs.h"

static cs_bool Survival_OnHandshake(void *param) {
	onHandshakeDone *a = (onHandshakeDone *)param;
	if(!Client_GetExtVer(a->client, EXT_HACKCTRL) ||
	!Client_GetExtVer(a->client, EXT_MESSAGETYPE) ||
	!Client_GetExtVer(a->client, EXT_PLAYERCLICK) ||
	!Client_GetExtVer(a->client, EXT_HELDBLOCK)) {
		Client_Kick(a->client, "Your client doesn't support necessary CPE extensions.");
		return false;
	}

	return SurvData_Create(a->client);
}

static void Survival_OnSpawn(void *param) {
	Client *cl = (Client *)param;
	SrvData *data = SurvData_Get(cl);
	if(data) {
		if(!SurvFS_LoadPlayerData(data))
			Client_Chat(cl, MESSAGE_TYPE_CHAT, "&cLooks like your survival saved data is corrupted.");
		SurvGui_DrawAll(data);
		SurvHacks_Update(data);
		SurvInv_Init(data);
	}
}

static void Survival_OnDespawn(void *param) {
	SrvData *data = SurvData_Get((Client *)param);
	if(data) SurvFS_SavePlayerData(data);
}

static cs_bool Survival_OnBlockPlace(void *param) {
	onBlockPlace *a = (onBlockPlace *)param;
	SrvData *data = SurvData_Get(a->client);
	if(!data || data->godMode) return true;

	cs_byte mode = a->mode;
	BlockID id = a->id;

	if(mode == 0x00) {
		Client_Kick(a->client, "Hacked client detected.");
		return false;
	}

	if(mode == 0x01 && SurvInv_Take(data, id, 1)) {
		if(SurvInv_Get(data, id) < 1) {
			SurvGui_DrawBlockInfo(data, 0);
			return true;
		}
		SurvGui_DrawBlockInfo(data, id);
		return true;
	}

	return false;
}

static void Survival_OnHeldChange(void *param) {
	onHeldBlockChange *a = (onHeldBlockChange *)param;
	SrvData *data = SurvData_Get(a->client);
	if(data && !data->godMode)
		SurvGui_DrawBlockInfo(data, a->curr);
}

static void Survival_OnTick(void *param) {
	cs_int32 delta = *(cs_int32 *)param;
	for(ClientID i = 0; i < MAX_CLIENTS; i++) {
		SrvData *data = SurvData_GetByID(i);
		if(data) {
			if(data->breakStarted)
				SurvBrk_Tick(data, delta);
			SurvHacks_Test(data);
		}
	}
}

static void Survival_OnMove(void *param) {
	Client *client = (Client *)param;
	SrvData *data = SurvData_Get(client);
	if(!data || data->godMode) return;

	Vec ppos;
	cs_float falldamage;
	if(Client_GetPosition(client, &ppos, NULL)) {
		switch(Client_GetStandBlock(client)) {
			case BLOCK_AIR:
				if(!data->freeFall) {
					data->fallStart = ppos.y;
					data->freeFall = true;
				}
				break;
			default:
				if(data->freeFall) {
					falldamage = (data->fallStart - ppos.y) / 2.0f;
					data->freeFall = false;
					if(falldamage > 1.0f && Client_GetFluidLevel(client, NULL) < 1)
						SurvDmg_Hurt(data, NULL, (cs_byte)falldamage);
				}
				break;
		}
	}
}

static void Survival_OnClick(void *param) {
	onPlayerClick *a = (onPlayerClick *)param;
	if(a->button != 0) return;

	SrvData *data = SurvData_Get(a->client);
	if(!data || data->godMode) return;

	if(a->action == 1) {
		SurvBrk_Stop(data);
		return;
	}

	Vec playerpos;
	if(!Client_GetPosition(a->client, &playerpos, NULL)) {
		Client_Kick(a->client, "Internal error");
		return;
	}

	Vec knockback = {0.0f, 0.0f, 0.0f};
	SVec *blockPos = &a->tgpos;
	Client *target = Client_GetByID(a->tgid);
	SrvData *dataTg = NULL;
	if(target) dataTg = SurvData_Get(target);
	cs_float dist_max = Client_GetClickDistanceInBlocks(a->client);

	float dist_entity = 32768.0f;
	float dist_block = 32768.0f;

	if(!Vec_IsInvalid(blockPos)) {
		Vec blockcenter;
		blockcenter.x = blockPos->x + 0.5f;
		blockcenter.y = blockPos->y - 0.5f;
		blockcenter.z = blockPos->z + 0.5f;
		dist_block = Math_Distance(&blockcenter, &playerpos);
		if(dist_block - dist_max > 1.5f) goto hackdetected;
	}

	if(target) {
		Vec tgcampos;
		if(Client_GetPosition(target, &tgcampos, NULL)) {
			knockback.x = -(playerpos.x - tgcampos.x) * 350.0f;
			knockback.y = -(playerpos.y - tgcampos.y) * 350.0f;
			knockback.z = -(playerpos.z - tgcampos.z) * 350.0f;
			dist_entity = Math_Distance(&tgcampos, &playerpos);
			if(dist_entity > dist_max) dist_entity = 32768.0f;
		}
	}

	if(data->breakStarted && !SVec_Compare(&data->lastClick, blockPos)) {
		SurvBrk_Stop(data);
		return;
	}

	if(dist_block < dist_entity && dist_block < dist_max) {
		if(!data->breakStarted) {
			BlockID bid = World_GetBlock(Client_GetWorld(a->client), blockPos);
			if(bid > BLOCK_AIR) SurvBrk_Start(data, bid);
		}
		data->lastClick = *blockPos;
	} else if(dist_entity < dist_block && dist_entity < dist_max) {
		if(data->breakStarted) {
			SurvBrk_Stop(data);
			return;
		}
		if(data->pvpMode && dataTg->pvpMode) {
			if(!dataTg->godMode) {
				dataTg->hackScore = 0;
				SurvDmg_Hurt(dataTg, data, 1);
				Client_SetVelocity(target, &knockback, true);
			}
		} else {
			if(!data->pvpMode)
				Client_Chat(a->client, 0, "Enable pvp mode (/pvp) first.");
		}
	}

	return;
	hackdetected:
	Client_Kick(a->client, "Click hack detected!");
}

EventRegBunch events[] = {
	{'v', EVT_ONTICK, (void *)Survival_OnTick},
	{'v', EVT_ONSPAWN, (void *)Survival_OnSpawn},
	{'v', EVT_ONDESPAWN, (void *)Survival_OnDespawn},
	{'v', EVT_ONHELDBLOCKCHNG, (void *)Survival_OnHeldChange},
	{'b', EVT_ONBLOCKPLACE, (void *)Survival_OnBlockPlace},
	{'v', EVT_ONMOVE, (void *)Survival_OnMove},
	{'v', EVT_ONDISCONNECT, (void *)SurvData_Free},
	{'b', EVT_ONHANDSHAKEDONE, (void *)Survival_OnHandshake},
	{'v', EVT_ONCLICK, (void *)Survival_OnClick},
	{0, 0, NULL}
};

void SurvEvents_Init(void) {
	Event_RegisterBunch(events);
}
