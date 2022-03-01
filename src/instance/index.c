#include "../enum_reflection.h"
#include "instance.h"
#include "common.h"
#ifdef WINDOWS
#include <processthreadsapi.h>
#else
#include <pthread.h>
#endif
#include <time.h>

struct IndexSession {
	struct NetSession net;
	struct IndexSession *next;
	ClientState clientState;
	struct String secret, userName, userId;
	struct PingPong tableTennis;
	struct Channels channels;
	uint8_t menu[65536];
};

struct Context {
	struct NetContext net;
	struct IndexSession *sessionList;
	struct NetKeypair keys;
};

#define update_menu(ctx, session, ...) _update_menu(ctx, session, (const char*[]){__VA_ARGS__, NULL})
static void _update_menu(struct Context *ctx, struct IndexSession *session, const char **entries) {
	uint8_t resp[65536], *resp_end;
	for(uint8_t i = 1, *it = session->menu; *it; ++i, it += *it + 1) {
		resp_end = resp;
		pkt_writeRoutingHeader(session->net.version, &resp_end, (struct RoutingHeader){i, 0, 0});
		struct PlayerDisconnected r_disconnect;
		r_disconnect.disconnectedReason = DisconnectedReason_ClientConnectionClosed;
		SERIALIZE_CUSTOM(PV_LEGACY_DEFAULT, &resp_end, InternalMessageType_PlayerDisconnected)
			pkt_writePlayerDisconnected(PV_LEGACY_DEFAULT, &resp_end, r_disconnect);
		instance_send_channeled(PV_LEGACY_DEFAULT, &session->channels, resp, resp_end - resp, DeliveryMethod_ReliableOrdered);
	}
	for(uint8_t i = 1, *it = session->menu; *entries; ++i, it += *it + 1, ++entries) {
		*it = sprintf((char*)&it[1], "%s", *entries);

		resp_end = resp;
		pkt_writeRoutingHeader(PV_LEGACY_DEFAULT, &resp_end, (struct RoutingHeader){0, 0, 0});
		struct PlayerConnected r_connected;
		r_connected.remoteConnectionId = i;
		r_connected.userId.isNull = 0;
		r_connected.userId.length = sprintf(r_connected.userId.data, "%hhx", i);
		r_connected.userName.length = *it;
		memcpy(r_connected.userName.data, &it[1], *it);
		r_connected.isConnectionOwner = 0;
		SERIALIZE_CUSTOM(PV_LEGACY_DEFAULT, &resp_end, InternalMessageType_PlayerConnected)
			pkt_writePlayerConnected(PV_LEGACY_DEFAULT, &resp_end, r_connected);
		instance_send_channeled(PV_LEGACY_DEFAULT, &session->channels, resp, resp_end - resp, DeliveryMethod_ReliableOrdered);

		resp_end = resp;
		pkt_writeRoutingHeader(PV_LEGACY_DEFAULT, &resp_end, (struct RoutingHeader){0, 0, 0});
		struct PlayerSortOrderUpdate r_sort;
		r_sort.userId = r_connected.userId;
		r_sort.sortIndex = 1;
		SERIALIZE_CUSTOM(PV_LEGACY_DEFAULT, &resp_end, InternalMessageType_PlayerSortOrderUpdate)
			pkt_writePlayerSortOrderUpdate(PV_LEGACY_DEFAULT, &resp_end, r_sort);
		instance_send_channeled(PV_LEGACY_DEFAULT, &session->channels, resp, resp_end - resp, DeliveryMethod_ReliableOrdered);

		resp_end = resp;
		pkt_writeRoutingHeader(PV_LEGACY_DEFAULT, &resp_end, (struct RoutingHeader){i, 0, 0});
		struct PlayerIdentity r_identity;
		r_identity.playerState.bloomFilter.d0 = 288266110296588352;
		r_identity.playerState.bloomFilter.d1 = 576531121051926529;
		memset(&r_identity.playerAvatar, 0, sizeof(r_identity.playerAvatar));
		r_identity.random.length = 32;
		memcpy(r_identity.random.data, NetKeypair_get_random(&ctx->keys), 32);
		r_identity.publicEncryptionKey.length = sizeof(r_identity.publicEncryptionKey.data);
		NetKeypair_write_key(&ctx->keys, &ctx->net, r_identity.publicEncryptionKey.data, &r_identity.publicEncryptionKey.length);
		SERIALIZE_CUSTOM(PV_LEGACY_DEFAULT, &resp_end, InternalMessageType_PlayerIdentity)
			pkt_writePlayerIdentity(PV_LEGACY_DEFAULT, &resp_end, r_identity);
		instance_send_channeled(PV_LEGACY_DEFAULT, &session->channels, resp, resp_end - resp, DeliveryMethod_ReliableOrdered);
	}
}

static float get_syncTime() {
	struct timespec now;
	if(clock_gettime(CLOCK_MONOTONIC, &now))
		return 0;
	return now.tv_sec + (now.tv_nsec / 1000) / 1000000.f;
}

static struct NetSession *index_onResolve(struct Context *ctx, struct SS addr, void **userdata_out) {
	for(struct IndexSession *session = ctx->sessionList; session; session = session->next)
		if(addrs_are_equal(&addr, NetSession_get_addr(&session->net)))
			return &session->net;
	return NULL;
}

static struct IndexSession *index_disconnect(struct IndexSession *session) {
	struct IndexSession *next = session->next;
	char addrstr[INET6_ADDRSTRLEN + 8];
	net_tostr(NetSession_get_addr(&session->net), addrstr);
	uprintf("disconnect %s\n", addrstr);
	net_session_free(&session->net);
	free(session);
	return next;
}

static void index_onResend(struct Context *ctx, uint32_t currentTime, uint32_t *nextTick) {
	for(struct IndexSession **sp = &ctx->sessionList; *sp;) {
		uint32_t kickTime = NetSession_get_lastKeepAlive(&(*sp)->net) + 10000;
		if(currentTime > kickTime) {
			*sp = index_disconnect(*sp);
		} else {
			if(kickTime < *nextTick)
				*nextTick = kickTime;
			struct IndexSession *session = *sp;
			flush_ack(&ctx->net, &session->net, &session->channels.ru.base.ack);
			flush_ack(&ctx->net, &session->net, &session->channels.ro.base.ack);
			for(uint_fast8_t i = 0; i < 64; ++i)
				try_resend(&ctx->net, &session->net, &session->channels.ru.base.resend[i], currentTime);
			for(uint_fast8_t i = 0; i < 64; ++i)
				try_resend(&ctx->net, &session->net, &session->channels.ro.base.resend[i], currentTime);
			try_resend(&ctx->net, &session->net, &session->channels.rs.resend, currentTime);
			net_flush_merged(&ctx->net, &session->net);
			*nextTick = currentTime + 15; // TODO: proper resend timing + clearing unneeded ack bits
			sp = &(*sp)->next;
		}
	}
}

static void handle_MenuRpc(struct Context *ctx, struct IndexSession *session, const uint8_t **data) {
	struct MenuRpcHeader rpc = pkt_readMenuRpcHeader(PV_LEGACY_DEFAULT, data);
	switch(rpc.type) {
		case MenuRpcType_SetPlayersMissingEntitlementsToLevel: uprintf("MenuRpcType_SetPlayersMissingEntitlementsToLevel not implemented\n"); abort();
		case MenuRpcType_GetIsEntitledToLevel: uprintf("MenuRpcType_GetIsEntitledToLevel not implemented\n"); abort();
		case MenuRpcType_SetIsEntitledToLevel: uprintf("MenuRpcType_SetIsEntitledToLevel not implemented\n"); abort();
		case MenuRpcType_InvalidateLevelEntitlementStatuses: uprintf("MenuRpcType_InvalidateLevelEntitlementStatuses not implemented\n"); abort();
		case MenuRpcType_SelectLevelPack: uprintf("MenuRpcType_SelectLevelPack not implemented\n"); abort();
		case MenuRpcType_SetSelectedBeatmap: uprintf("MenuRpcType_SetSelectedBeatmap not implemented\n"); abort();
		case MenuRpcType_GetSelectedBeatmap: uprintf("MenuRpcType_GetSelectedBeatmap not implemented\n"); abort();
		case MenuRpcType_RecommendBeatmap: uprintf("MenuRpcType_RecommendBeatmap not implemented\n"); abort();
		case MenuRpcType_ClearRecommendedBeatmap: uprintf("MenuRpcType_ClearRecommendedBeatmap not implemented\n"); abort();
		case MenuRpcType_GetRecommendedBeatmap: {
			pkt_readGetRecommendedBeatmap(session->net.version, data);
			struct RecommendBeatmap r_beatmap;
			r_beatmap.base.syncTime = get_syncTime();
			r_beatmap.identifier.levelID.isNull = 0;
			r_beatmap.identifier.levelID.length = sprintf(r_beatmap.identifier.levelID.data, "custom_level_00000000000000000000000000000000");
			r_beatmap.identifier.beatmapCharacteristicSerializedName.isNull = 0;
			r_beatmap.identifier.beatmapCharacteristicSerializedName.length = sprintf(r_beatmap.identifier.beatmapCharacteristicSerializedName.data, "Standard");
			r_beatmap.identifier.difficulty = BeatmapDifficulty_Easy;
			uint8_t resp[65536], *resp_end;
			for(uint8_t i = 1, *it = session->menu; *it; ++i, it += *it + 1) {
				resp_end = resp;
				pkt_writeRoutingHeader(PV_LEGACY_DEFAULT, &resp_end, (struct RoutingHeader){i, 0, 0});
				SERIALIZE_MENURPC(session->net.version, &resp_end, RecommendBeatmap, r_beatmap);
				instance_send_channeled(PV_LEGACY_DEFAULT, &session->channels, resp, resp_end - resp, DeliveryMethod_ReliableOrdered);
			}
			break;
		}
		case MenuRpcType_SetSelectedGameplayModifiers: uprintf("MenuRpcType_SetSelectedGameplayModifiers not implemented\n"); abort();
		case MenuRpcType_GetSelectedGameplayModifiers: uprintf("MenuRpcType_GetSelectedGameplayModifiers not implemented\n"); abort();
		case MenuRpcType_RecommendGameplayModifiers: {
			pkt_readRecommendGameplayModifiers(session->net.version, data);
			uint8_t resp[65536], *resp_end = resp;
			struct SetSelectedGameplayModifiers r_set;
			r_set.base.syncTime = get_syncTime();
			r_set.flags.hasValue0 = 1;
			r_set.gameplayModifiers = CLEAR_MODIFIERS;
			pkt_writeRoutingHeader(PV_LEGACY_DEFAULT, &resp_end, (struct RoutingHeader){0, 127, 0});
			SERIALIZE_MENURPC(session->net.version, &resp_end, SetSelectedGameplayModifiers, r_set);
			instance_send_channeled(PV_LEGACY_DEFAULT, &session->channels, resp, resp_end - resp, DeliveryMethod_ReliableOrdered);
			break;
		}
		case MenuRpcType_ClearRecommendedGameplayModifiers: uprintf("MenuRpcType_ClearRecommendedGameplayModifiers not implemented\n"); abort();
		case MenuRpcType_GetRecommendedGameplayModifiers: {
			pkt_readGetRecommendedGameplayModifiers(session->net.version, data);
			struct RecommendGameplayModifiers r_modifiers;
			r_modifiers.base.syncTime = get_syncTime();
			memset(&r_modifiers.gameplayModifiers, 0, sizeof(r_modifiers.gameplayModifiers));
			uint8_t resp[65536], *resp_end;
			for(uint8_t i = 1, *it = session->menu; *it; ++i, it += *it + 1) {
				resp_end = resp;
				pkt_writeRoutingHeader(PV_LEGACY_DEFAULT, &resp_end, (struct RoutingHeader){i, 0, 0});
				SERIALIZE_MENURPC(session->net.version, &resp_end, RecommendGameplayModifiers, r_modifiers);
				instance_send_channeled(PV_LEGACY_DEFAULT, &session->channels, resp, resp_end - resp, DeliveryMethod_ReliableOrdered);
			}
			break;
		}
		case MenuRpcType_LevelLoadError: uprintf("MenuRpcType_LevelLoadError not implemented\n"); abort();
		case MenuRpcType_LevelLoadSuccess: uprintf("MenuRpcType_LevelLoadSuccess not implemented\n"); abort();
		case MenuRpcType_StartLevel: uprintf("MenuRpcType_StartLevel not implemented\n"); abort();
		case MenuRpcType_GetStartedLevel: {
			pkt_readGetStartedLevel(session->net.version, data);
			uint8_t resp[65536], *resp_end = resp;
			struct SetIsStartButtonEnabled r_button;
			r_button.base.syncTime = get_syncTime();
			r_button.flags.hasValue0 = 1;
			r_button.reason = CannotStartGameReason_NoSongSelected;
			pkt_writeRoutingHeader(PV_LEGACY_DEFAULT, &resp_end, (struct RoutingHeader){0, 127, 0});
			SERIALIZE_MENURPC(session->net.version, &resp_end, SetIsStartButtonEnabled, r_button);
			instance_send_channeled(PV_LEGACY_DEFAULT, &session->channels, resp, resp_end - resp, DeliveryMethod_ReliableOrdered);
			break;
		}
		case MenuRpcType_CancelLevelStart: uprintf("MenuRpcType_CancelLevelStart not implemented\n"); abort();
		case MenuRpcType_GetMultiplayerGameState: {
			pkt_readGetMultiplayerGameState(session->net.version, data);
			uint8_t resp[65536], *resp_end = resp;
			struct SetMultiplayerGameState r_state;
			r_state.base.syncTime = get_syncTime();
			r_state.flags.hasValue0 = 1;
			r_state.lobbyState = MultiplayerGameState_Lobby;
			pkt_writeRoutingHeader(PV_LEGACY_DEFAULT, &resp_end, (struct RoutingHeader){0, 127, 0});
			SERIALIZE_MENURPC(session->net.version, &resp_end, SetMultiplayerGameState, r_state);
			instance_send_channeled(PV_LEGACY_DEFAULT, &session->channels, resp, resp_end - resp, DeliveryMethod_ReliableOrdered);
			break;
		}
		case MenuRpcType_SetMultiplayerGameState: uprintf("MenuRpcType_SetMultiplayerGameState not implemented\n"); abort();
		case MenuRpcType_GetIsReady: {
			pkt_readGetIsReady(session->net.version, data);
			struct SetIsReady r_ready;
			r_ready.base.syncTime = get_syncTime();
			r_ready.isReady = 1;
			uint8_t resp[65536], *resp_end;
			for(uint8_t i = 1, *it = session->menu; *it; ++i, it += *it + 1) {
				resp_end = resp;
				pkt_writeRoutingHeader(PV_LEGACY_DEFAULT, &resp_end, (struct RoutingHeader){i, 0, 0});
				SERIALIZE_MENURPC(session->net.version, &resp_end, SetIsReady, r_ready);
				instance_send_channeled(PV_LEGACY_DEFAULT, &session->channels, resp, resp_end - resp, DeliveryMethod_ReliableOrdered);
			}
			break;
		}
		case MenuRpcType_SetIsReady: {
			if(pkt_readSetIsReady(session->net.version, data).isReady) {
				uprintf("TODO: redirect to lobby here\n");
			}
			break;
		}
		case MenuRpcType_SetStartGameTime: uprintf("MenuRpcType_SetStartGameTime not implemented\n"); abort();
		case MenuRpcType_CancelStartGameTime: uprintf("MenuRpcType_CancelStartGameTime not implemented\n"); abort();
		case MenuRpcType_GetIsInLobby: {
			pkt_readGetIsInLobby(session->net.version, data);
			struct SetIsInLobby r_back;
			r_back.base.syncTime = get_syncTime();
			r_back.isBack = 1;
			uint8_t resp[65536], *resp_end;
			for(uint8_t i = 1, *it = session->menu; *it; ++i, it += *it + 1) {
				resp_end = resp;
				pkt_writeRoutingHeader(PV_LEGACY_DEFAULT, &resp_end, (struct RoutingHeader){i, 0, 0});
				SERIALIZE_MENURPC(session->net.version, &resp_end, SetIsInLobby, r_back);
				instance_send_channeled(PV_LEGACY_DEFAULT, &session->channels, resp, resp_end - resp, DeliveryMethod_ReliableOrdered);
			}
			break;
		}
		case MenuRpcType_SetIsInLobby: pkt_readSetIsInLobby(session->net.version, data); break;
		case MenuRpcType_GetCountdownEndTime: {
			pkt_readGetCountdownEndTime(session->net.version, data);
			uint8_t resp[65536], *resp_end = resp;
			struct SetIsStartButtonEnabled r_button;
			r_button.base.syncTime = get_syncTime();
			r_button.flags.hasValue0 = 1;
			r_button.reason = CannotStartGameReason_NoSongSelected;
			pkt_writeRoutingHeader(PV_LEGACY_DEFAULT, &resp_end, (struct RoutingHeader){0, 127, 0});
			SERIALIZE_MENURPC(session->net.version, &resp_end, SetIsStartButtonEnabled, r_button);
			instance_send_channeled(PV_LEGACY_DEFAULT, &session->channels, resp, resp_end - resp, DeliveryMethod_ReliableOrdered);
			break;
		}
		case MenuRpcType_SetCountdownEndTime: uprintf("MenuRpcType_SetCountdownEndTime not implemented\n"); abort();
		case MenuRpcType_CancelCountdown: uprintf("MenuRpcType_CancelCountdown not implemented\n"); abort();
		case MenuRpcType_GetOwnedSongPacks: uprintf("MenuRpcType_GetOwnedSongPacks not implemented\n"); abort();
		case MenuRpcType_SetOwnedSongPacks: pkt_readSetOwnedSongPacks(session->net.version, data); break;
		case MenuRpcType_RequestKickPlayer: uprintf("MenuRpcType_RequestKickPlayer not implemented\n"); abort();
		case MenuRpcType_GetPermissionConfiguration:  {
			pkt_readGetPermissionConfiguration(session->net.version, data);
			uint8_t resp[65536], *resp_end = resp;
			struct SetPermissionConfiguration r_permission;
			r_permission.base.syncTime = get_syncTime();
			r_permission.flags.hasValue0 = 1;
			r_permission.playersPermissionConfiguration.count = 1;
			r_permission.playersPermissionConfiguration.playersPermission[0].userId = session->userId;
			r_permission.playersPermissionConfiguration.playersPermission[0].isServerOwner = 1;
			r_permission.playersPermissionConfiguration.playersPermission[0].hasRecommendBeatmapsPermission = 0;
			r_permission.playersPermissionConfiguration.playersPermission[0].hasRecommendGameplayModifiersPermission = 0;
			r_permission.playersPermissionConfiguration.playersPermission[0].hasKickVotePermission = 0;
			r_permission.playersPermissionConfiguration.playersPermission[0].hasInvitePermission = 0;
			for(uint8_t *it = session->menu; *it; ++r_permission.playersPermissionConfiguration.count, it += *it + 1) {
				r_permission.playersPermissionConfiguration.playersPermission[r_permission.playersPermissionConfiguration.count].userId.isNull = 0;
				r_permission.playersPermissionConfiguration.playersPermission[r_permission.playersPermissionConfiguration.count].userId.length = sprintf(r_permission.playersPermissionConfiguration.playersPermission[r_permission.playersPermissionConfiguration.count].userId.data, "%hhx", (uint8_t)r_permission.playersPermissionConfiguration.count);
				r_permission.playersPermissionConfiguration.playersPermission[r_permission.playersPermissionConfiguration.count].isServerOwner = 0;
				r_permission.playersPermissionConfiguration.playersPermission[r_permission.playersPermissionConfiguration.count].hasRecommendBeatmapsPermission = 0;
				r_permission.playersPermissionConfiguration.playersPermission[r_permission.playersPermissionConfiguration.count].hasRecommendGameplayModifiersPermission = 0;
				r_permission.playersPermissionConfiguration.playersPermission[r_permission.playersPermissionConfiguration.count].hasKickVotePermission = 0;
				r_permission.playersPermissionConfiguration.playersPermission[r_permission.playersPermissionConfiguration.count].hasInvitePermission = 0;
			}
			pkt_writeRoutingHeader(PV_LEGACY_DEFAULT, &resp_end, (struct RoutingHeader){0, 0, 0});
			SERIALIZE_MENURPC(session->net.version, &resp_end, SetPermissionConfiguration, r_permission);
			instance_send_channeled(PV_LEGACY_DEFAULT, &session->channels, resp, resp_end - resp, DeliveryMethod_ReliableOrdered);
			break;
		}
		case MenuRpcType_SetPermissionConfiguration: uprintf("MenuRpcType_SetPermissionConfiguration not implemented\n"); abort();
		case MenuRpcType_GetIsStartButtonEnabled: uprintf("MenuRpcType_GetIsStartButtonEnabled not implemented\n"); abort();
		case MenuRpcType_SetIsStartButtonEnabled: uprintf("MenuRpcType_SetIsStartButtonEnabled not implemented\n"); abort();
		default: uprintf("BAD MENU RPC TYPE\n");
	}
}

static void handle_PlayerIdentity(struct Context *ctx, struct IndexSession *session, const uint8_t **data) {
	/*struct PlayerIdentity identity =*/ pkt_readPlayerIdentity(session->net.version, data);
	if(session->clientState != ClientState_accepted)
		return;
	session->clientState = ClientState_connected;

	uint8_t resp[65536], *resp_end = resp;
	pkt_writeRoutingHeader(PV_LEGACY_DEFAULT, &resp_end, (struct RoutingHeader){0, 0, 0});
	struct PlayerSortOrderUpdate r_sort;
	r_sort.userId = session->userId;
	r_sort.sortIndex = 0;
	SERIALIZE_CUSTOM(PV_LEGACY_DEFAULT, &resp_end, InternalMessageType_PlayerSortOrderUpdate)
		pkt_writePlayerSortOrderUpdate(PV_LEGACY_DEFAULT, &resp_end, r_sort);
	instance_send_channeled(PV_LEGACY_DEFAULT, &session->channels, resp, resp_end - resp, DeliveryMethod_ReliableOrdered);

	update_menu(ctx, session, "Room 1", "Room 2");

	resp_end = resp;
	pkt_writeRoutingHeader(PV_LEGACY_DEFAULT, &resp_end, (struct RoutingHeader){0, 127, 0});
	struct RemoteProcedureCall base;
	base.syncTime = get_syncTime();
	SERIALIZE_MENURPC(session->net.version, &resp_end, GetRecommendedBeatmap, (struct GetRecommendedBeatmap){.base = base});
	SERIALIZE_MENURPC(session->net.version, &resp_end, GetRecommendedGameplayModifiers, (struct GetRecommendedGameplayModifiers){.base = base});
	SERIALIZE_MENURPC(session->net.version, &resp_end, GetIsReady, (struct GetIsReady){.base = base});
	SERIALIZE_MENURPC(session->net.version, &resp_end, GetIsInLobby, (struct GetIsInLobby){.base = base});
	instance_send_channeled(PV_LEGACY_DEFAULT, &session->channels, resp, resp_end - resp, DeliveryMethod_ReliableOrdered);
}

static void process_Channeled(struct Context *ctx, void *room, struct IndexSession *session, const uint8_t **data, const uint8_t *end, DeliveryMethod channelId) {
	pkt_readRoutingHeader(session->net.version, data);
	while(*data < end) {
		struct SerializeHeader serial = pkt_readSerializeHeader(session->net.version, data);
		const uint8_t *sub = (*data)--;
		*data += serial.length;
		switch(serial.type) {
			case InternalMessageType_PlayerIdentity: handle_PlayerIdentity(ctx, session, &sub); break;
			case InternalMessageType_MultiplayerSession: {
				struct MultiplayerSessionMessageHeader message = pkt_readMultiplayerSessionMessageHeader(PV_LEGACY_DEFAULT, &sub);
				if(message.type != MultiplayerSessionMessageType_MenuRpc)
					continue;
				handle_MenuRpc(ctx, session, &sub);
				break;
			}
			default: continue;
		}
		if(sub != *data) {
			uprintf("BAD INTERNAL MESSAGE LENGTH (expected %u, read %zu)\n", serial.length, sub - (*data - serial.length));
			if(sub < *data) {
				uprintf("\t");
				for(const uint8_t *it = *data - serial.length; it < *data; ++it)
					uprintf("%02hhx", *it);
				uprintf("\n\t");
				for(const uint8_t *it = *data - serial.length; it < sub; ++it)
					uprintf("  ");
				uprintf("^ extra data starts here");
			}
			uprintf("\n");
		}
	}
}

static void handle_ConnectRequest(struct Context *ctx, struct IndexSession *session, const uint8_t **data) {
	struct ConnectRequest req = pkt_readConnectRequest(session->net.version, data);
	session->net.version.netVersion = req.protocolId;
	if(!(String_eq(req.secret, session->secret) && String_eq(req.userId, session->userId)))
		return;
	uint8_t resp[65536], *resp_end = resp;
	pkt_writeNetPacketHeader(PV_LEGACY_DEFAULT, &resp_end, (struct NetPacketHeader){PacketProperty_ConnectAccept, 0, 0});
	pkt_writeConnectAccept(PV_LEGACY_DEFAULT, &resp_end, (struct ConnectAccept){
		.connectTime = req.connectTime,
		.connectNum = 0,
		.reusedPeer = 0,
		.peerId = 0,
	});
	net_send_internal(&ctx->net, &session->net, resp, resp_end - resp, 1);

	if(session->clientState != ClientState_disconnected)
		return;
	session->clientState = ClientState_accepted;

	resp_end = resp;
	pkt_writeRoutingHeader(PV_LEGACY_DEFAULT, &resp_end, (struct RoutingHeader){0, 127, 0});
	struct SyncTime r_sync;
	r_sync.syncTime = get_syncTime();
	SERIALIZE_CUSTOM(PV_LEGACY_DEFAULT, &resp_end, InternalMessageType_SyncTime)
		pkt_writeSyncTime(PV_LEGACY_DEFAULT, &resp_end, r_sync);
	instance_send_channeled(PV_LEGACY_DEFAULT, &session->channels, resp, resp_end - resp, DeliveryMethod_ReliableOrdered);

	resp_end = resp;
	pkt_writeRoutingHeader(PV_LEGACY_DEFAULT, &resp_end, (struct RoutingHeader){0, 0, 0});
	struct PlayerIdentity r_identity;
	r_identity.playerState.bloomFilter.d0 = 288266110296588352;
	r_identity.playerState.bloomFilter.d1 = 576531121051926529;
	memset(&r_identity.playerAvatar, 0, sizeof(r_identity.playerAvatar));
	r_identity.random.length = 32;
	memcpy(r_identity.random.data, NetKeypair_get_random(&ctx->keys), 32);
	r_identity.publicEncryptionKey.length = sizeof(r_identity.publicEncryptionKey.data);
	NetKeypair_write_key(&ctx->keys, &ctx->net, r_identity.publicEncryptionKey.data, &r_identity.publicEncryptionKey.length);
	SERIALIZE_CUSTOM(PV_LEGACY_DEFAULT, &resp_end, InternalMessageType_PlayerIdentity)
		pkt_writePlayerIdentity(PV_LEGACY_DEFAULT, &resp_end, r_identity);
	instance_send_channeled(PV_LEGACY_DEFAULT, &session->channels, resp, resp_end - resp, DeliveryMethod_ReliableOrdered);
}

static void handle_packet(struct Context *ctx, struct IndexSession *session, const uint8_t *pkt, uint32_t len) {
	const uint8_t *data = pkt, *end = &pkt[len];
	struct NetPacketHeader header = pkt_readNetPacketHeader(PV_LEGACY_DEFAULT, &data);
	if(session->clientState == ClientState_disconnected && header.property != PacketProperty_ConnectRequest)
		return;
	if(header.isFragmented && header.property != PacketProperty_Channeled) {
		uprintf("MALFORMED HEADER\n");
		return;
	}
	switch(header.property) {
		case PacketProperty_Unreliable: data = end; break;
		case PacketProperty_Channeled: handle_Channeled((ChanneledHandler)process_Channeled, &ctx->net, &session->net, &session->channels, ctx, NULL, session, &data, end, header.isFragmented); break;
		case PacketProperty_Ack: handle_Ack(&session->net, &session->channels, &data); break;
		case PacketProperty_Ping: handle_Ping(&ctx->net, &session->net, &session->tableTennis, &data); break;
		case PacketProperty_Pong: break;
		case PacketProperty_ConnectRequest: handle_ConnectRequest(ctx, session, &data); break;
		case PacketProperty_ConnectAccept: break;
		case PacketProperty_Disconnect: data = end; break; // Client will time out after a while
		case PacketProperty_UnconnectedMessage: break;
		case PacketProperty_MtuCheck: handle_MtuCheck(&ctx->net, &session->net, &data); break;
		case PacketProperty_MtuOk: break;
		case PacketProperty_Broadcast: break;
		case PacketProperty_Merged: {
			for(uint16_t sublen; data < end; data += sublen) {
				sublen = pkt_readUint16(PV_LEGACY_DEFAULT, &data);
				const uint8_t *subdata = data;
				handle_packet(ctx, session, subdata, sublen);
			}
			break;
		}
		case PacketProperty_ShutdownOk: break;
		case PacketProperty_PeerNotFound: break;
		case PacketProperty_InvalidProtocol: break;
		case PacketProperty_NatMessage: break;
		case PacketProperty_Empty: break;
		default: uprintf("BAD PACKET PROPERTY\n");
	}
	if(data != end)
		uprintf("BAD PACKET LENGTH (expected %u, read %zu)\n", len, data - pkt);
}

#ifdef WINDOWS
static DWORD WINAPI
#else
static void*
#endif
index_handler(struct Context *ctx) {
	net_lock(&ctx->net);
	uprintf("Started Index\n");
	uint8_t buf[262144];
	memset(buf, 0, sizeof(buf));
	uint32_t len;
	struct IndexSession *session;
	const uint8_t *pkt;
	while((len = net_recv(&ctx->net, buf, sizeof(buf), (struct NetSession**)&session, &pkt, NULL))) {
		handle_packet(ctx, session, pkt, len);
	}
	net_unlock(&ctx->net);
	return 0;
}

#ifdef WINDOWS
static HANDLE index_thread = NULL;
#else
static pthread_t index_thread = 0;
#endif
static struct Context context = {{-1}, NULL};
_Bool index_init() {
	if(net_init(&context.net, 0)) {
		uprintf("net_init() failed\n");
		return 1;
	}
	context.net.user = &context;
	context.net.onResolve = index_onResolve;
	context.net.onResend = index_onResend;
	net_keypair_gen(&context.net, &context.keys);

	#ifdef WINDOWS
	index_thread = CreateThread(NULL, 0, (LPTHREAD_START_ROUTINE)index_handler, &context, 0, NULL);
	#else
	if(pthread_create(&index_thread, NULL, (void*(*)(void*))&index_handler, &context))
		index_thread = 0;
	#endif
	if(!index_thread) {
		uprintf("Index thread creation failed\n");
		return 1;
	}
	return 0;
}

void index_cleanup() {
	if(index_thread) {
		net_stop(&context.net);
		uprintf("Stopping Index\n");
		#ifdef WINDOWS
		WaitForSingleObject(index_thread, INFINITE);
		#else
		pthread_join(index_thread, NULL);
		#endif
		index_thread = 0;
		while(context.sessionList)
			context.sessionList = index_disconnect(context.sessionList);
		net_cleanup(&context.net);
	}
}

_Bool index_get_isopen(struct String *managerId_out, struct GameplayServerConfiguration *configuration) {
	configuration->maxPlayerCount = 2;
	configuration->discoveryPolicy = DiscoveryPolicy_Hidden;
	configuration->invitePolicy = InvitePolicy_NobodyCanInvite;
	configuration->gameplayServerMode = GameplayServerMode_Countdown;
	configuration->songSelectionMode = SongSelectionMode_OwnerPicks;
	configuration->gameplayServerControlSettings = GameplayServerControlSettings_None;
	return 1;
}

struct NetSession *index_create_session(struct SS addr, struct String secret, struct String userId, struct String userName) {
	struct IndexSession *session = malloc(sizeof(struct IndexSession));
	if(!session) {
		uprintf("alloc error\n");
		abort();
	}
	net_session_init(&context.net, &session->net, addr);
	session->clientState = ClientState_disconnected;
	session->secret = secret;
	session->userName = userName;
	session->userId = userId;
	instance_pingpong_init(&session->tableTennis);
	instance_channels_init(&session->channels);
	session->menu[0] = 0;
	session->next = context.sessionList;
	context.sessionList = session;

	char addrstr[INET6_ADDRSTRLEN + 8];
	net_tostr(&addr, addrstr);
	uprintf("connect %s\n", addrstr);
	return &session->net;
}

struct NetContext *index_get_net() {
	return &context.net;
}

uint32_t index_get_port() {
	struct SS addr = {sizeof(struct sockaddr_storage)};
	getsockname(net_get_sockfd(&context.net), &addr.sa, &addr.len);
	switch(addr.ss.ss_family) {
		case AF_INET: return htons(addr.in.sin_port);
		case AF_INET6: return htons(addr.in6.sin6_port);
		default:;
	}
	return 0;
}
