#ifndef _PUL_FRIEND_ROOM_CPUS_
#define _PUL_FRIEND_ROOM_CPUS_

#include <MarioKartWii/Race/RaceData.hpp>
#include <MarioKartWii/Mii/Mii.hpp>
#include <MarioKartWii/AI/AIManager.hpp>
#include <Network/PacketExpansion.hpp>

namespace Pulsar {
namespace Network {

void PrepareFriendRoomCPUs(Racedata* racedata);
void FinalizeFriendRoomCPUs();
void ActivateFriendRoomCPUTransport(bool isHost);
bool IsFriendRoomCPUTransportActive();
bool IsFriendRoomCPU(u8 playerId);
Mii* GetFriendRoomCPUDisplayMii(u8 playerId);

bool WriteFriendRoomCPUState(PulRH1* packet);
void ReadFriendRoomCPUState(const PulRH1* packet, u32 packetSize, u8 senderAid);

void UpdateFriendRoomCPUs(AI::Manager* manager);

}  // namespace Network
}  // namespace Pulsar

#endif
