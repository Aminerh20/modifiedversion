#include "pch-il2cpp.h"
#include "_rpc.h"
#include "game.h"

RpcPetPlayer::RpcPetPlayer(PlayerControl* player)
	: Player(player)
{
}

void RpcPetPlayer::Process()
{
	auto localPlayer = *Game::pLocalPlayer;
	if (localPlayer == nullptr || Player == nullptr || Player == localPlayer ||
		!PlayerSelection(Player).has_value() || localPlayer->fields.cosmetics == nullptr ||
		localPlayer->fields.MyPhysics == nullptr) {
		return;
	}

	auto pet = localPlayer->fields.cosmetics->fields.currentPet;
	if (pet == nullptr) return;

	auto targetTransform = Component_get_transform((Component_1*)Player, NULL);
	if (targetTransform == nullptr) return;

	auto targetPosition = Transform_get_position(targetTransform, NULL);
	Vector2 petPosition = { targetPosition.x, targetPosition.y - pet->fields.yOffset * 2.f };

	PetBehaviour_SetGettingPet(pet, true, petPosition, NULL);
	auto pettingHand = CosmeticsLayer_get_PettingHand(localPlayer->fields.cosmetics, NULL);
	if (pettingHand != nullptr) PlayerPettingHand_StartPet(pettingHand, pet, NULL);
	PlayerPhysics_RpcPet(localPlayer->fields.MyPhysics, PlayerControl_GetTruePosition(localPlayer, NULL), petPosition, NULL);
}
