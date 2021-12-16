/*
 * This file is part of EasyRPG Player.
 *
 * EasyRPG Player is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * EasyRPG Player is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with EasyRPG Player. If not, see <http://www.gnu.org/licenses/>.
 */

// Headers
#include "game_player.h"
#include "async_handler.h"
#include "game_actor.h"
#include "game_map.h"
#include "game_message.h"
#include "game_party.h"
#include "game_system.h"
#include "game_screen.h"
#include "game_pictures.h"
#include "input.h"
#include "main_data.h"
#include "player.h"
#include "util_macro.h"
#include "game_switches.h"
#include "output.h"
#include "rand.h"
#include "utils.h"
#include <lcf/reader_util.h>
#include <lcf/scope_guard.h>
#include "scene_battle.h"
#include "scene_menu.h"
#include <lcf/rpg/savetarget.h>
#include <algorithm>
#include <cmath>

Game_Player::Game_Player(): Game_PlayerBase(Player)
{
	SetDirection(lcf::rpg::EventPage::Direction_down);
	SetMoveSpeed(4);
	SetAnimationType(lcf::rpg::EventPage::AnimType_non_continuous);
}

void Game_Player::SetSaveData(lcf::rpg::SavePartyLocation save)
{
	*data() = std::move(save);

	SanitizeData("Party");

	// RPG_RT will always reset the hero graphic on loading a save, even if
	// a move route changed the graphic.
	ResetGraphic();
}

lcf::rpg::SavePartyLocation Game_Player::GetSaveData() const {
	return *data();
}

int Game_Player::GetScreenZ(bool apply_shift) const {
	// Player is always slightly above events
	// (and always on "same layer as hero" obviously)
	return Game_Character::GetScreenZ(apply_shift) + 1;
}

void Game_Player::ReserveTeleport(int map_id, int x, int y, int direction, TeleportTarget::Type tt) {
	teleport_target = TeleportTarget(map_id, x, y, direction, tt);

	FileRequestAsync* request = Game_Map::RequestMap(map_id);
	request->SetImportantFile(true);
	request->Start();
}

void Game_Player::ReserveTeleport(const lcf::rpg::SaveTarget& target) {
	int map_id = target.map_id;

	if (Game_Map::GetMapType(target.map_id) == lcf::rpg::TreeMap::MapType_area) {
		// Area: Obtain the map the area belongs to
		map_id = Game_Map::GetParentId(target.map_id);
	}

	ReserveTeleport(map_id, target.map_x, target.map_y, Down, TeleportTarget::eSkillTeleport);

	if (target.switch_on) {
		Main_Data::game_switches->Set(target.switch_id, true);
		Game_Map::SetNeedRefresh(true);
	}
}

void Game_Player::PerformTeleport() {
	assert(IsPendingTeleport());
	if (!IsPendingTeleport()) {
		return;
	}

	if (teleport_target.GetMapId() <= 0) {
		Output::Error("Invalid Teleport map id! mapid={} x={} y={} d={}", teleport_target.GetMapId(),
				teleport_target.GetX(), teleport_target.GetY(), teleport_target.GetDirection());
	}

	const auto map_changed = (GetMapId() != teleport_target.GetMapId());
	MoveTo(teleport_target.GetMapId(), teleport_target.GetX(), teleport_target.GetY());


	if (teleport_target.GetDirection() >= 0) {
		SetDirection(teleport_target.GetDirection());
		UpdateFacing();
	}

	if (map_changed && teleport_target.GetType() != TeleportTarget::eAsyncQuickTeleport) {
		Main_Data::game_screen->OnMapChange();
		Main_Data::game_pictures->OnMapChange();
		Game_Map::GetInterpreter().OnMapChange();
	}

	ResetTeleportTarget();
}

void Game_Player::MoveTo(int map_id, int x, int y) {
	const auto map_changed = (GetMapId() != map_id);

	Game_Character::MoveTo(map_id, x, y);
	SetEncounterSteps(0);
	SetMenuCalling(false);

	auto* vehicle = GetVehicle();
	if (vehicle) {
		// RPG_RT doesn't check the aboard flag for this one
		vehicle->MoveTo(map_id, x, y);
	}

	if (map_changed) {
		// FIXME: Assert map pre-loaded in cache.

		// pan_state does not reset when you change maps.
		data()->pan_speed = lcf::rpg::SavePartyLocation::kPanSpeedDefault;
		data()->pan_finish_x = lcf::rpg::SavePartyLocation::kPanXDefault;
		data()->pan_finish_y = lcf::rpg::SavePartyLocation::kPanYDefault;
		data()->pan_current_x = lcf::rpg::SavePartyLocation::kPanXDefault;
		data()->pan_current_y = lcf::rpg::SavePartyLocation::kPanYDefault;

		ResetAnimation();

		auto map = Game_Map::loadMapFile(GetMapId());

		Game_Map::Setup(std::move(map));
		Game_Map::PlayBgm();

		// This Fixes an RPG_RT bug where the jumping flag doesn't get reset
		// if you change maps during a jump
		SetJumping(false);
	} else {
		Game_Map::SetPositionX(GetSpriteX() - GetPanX());
		Game_Map::SetPositionY(GetSpriteY() - GetPanY());
	}

	ResetGraphic();
}

bool Game_Player::MakeWay(int from_x, int from_y, int to_x, int to_y) {
	if (IsAboard()) {
		return GetVehicle()->MakeWay(from_x, from_y, to_x, to_y);
	}

	return Game_Character::MakeWay(from_x, from_y, to_x, to_y);
}

void Game_Player::MoveRouteSetSpriteGraphic(std::string sprite_name, int index) {
	auto* vh = GetVehicle();
	if (vh) {
		vh->MoveRouteSetSpriteGraphic(std::move(sprite_name), index);
	} else {
		Game_Character::MoveRouteSetSpriteGraphic(std::move(sprite_name), index);
	}
}

void Game_Player::UpdateScroll(int amount, bool was_jumping) {
	if (IsPanLocked()) {
		return;
	}

	auto dx = (GetX() * SCREEN_TILE_SIZE) - Game_Map::GetPositionX() - GetPanX();
	auto dy = (GetY() * SCREEN_TILE_SIZE) - Game_Map::GetPositionY() - GetPanY();

	const auto w = Game_Map::GetWidth() * SCREEN_TILE_SIZE;
	const auto h = Game_Map::GetHeight() * SCREEN_TILE_SIZE;

	dx = Utils::PositiveModulo(dx + w / 2, w) - w / 2;
	dy = Utils::PositiveModulo(dy + h / 2, h) - h / 2;

	const auto sx = Utils::Signum(dx);
	const auto sy = Utils::Signum(dy);

	if (was_jumping) {
		const auto jdx = sx * std::abs(GetX() - GetBeginJumpX());
		const auto jdy = sy * std::abs(GetY() - GetBeginJumpY());

		Game_Map::Scroll(amount * jdx, amount * jdy);

		if (!IsJumping()) {
			// RPG does this to fix rounding errors?
			const auto x = SCREEN_TILE_SIZE * Utils::RoundTo<int>(Game_Map::GetPositionX() / static_cast<double>(SCREEN_TILE_SIZE));
			const auto y = SCREEN_TILE_SIZE * Utils::RoundTo<int>(Game_Map::GetPositionY() / static_cast<double>(SCREEN_TILE_SIZE));

			// RPG_RT does adjust map position, but not panorama!
			Game_Map::SetPositionX(x, false);
			Game_Map::SetPositionY(y, false);
		}
		return;
	}

	int move_sx = 0;
	int move_sy = 0;
	const auto d = GetDirection();
	if (sy < 0 && (d == Up || d == UpRight || d == UpLeft)) {
		move_sy = sy;
	}
	if (sy > 0 && (d == Down || d == DownRight || d == DownLeft)) {
		move_sy = sy;
	}
	if (sx > 0 && (d == Right || d == UpRight || d == DownRight)) {
		move_sx = sx;
	}
	if (sx < 0 && (d == Left || d == UpLeft || d == DownLeft)) {
		move_sx = sx;
	}

	Game_Map::Scroll(move_sx * amount, move_sy * amount);
}

bool Game_Player::UpdateAirship() {
	auto* vehicle = GetVehicle();

	// RPG_RT doesn't check vehicle, but we have to as we don't have another way to fetch it.
	// Also in vanilla RPG_RT it's impossible for the hero to fly without the airship.
	if (vehicle && vehicle->IsFlying()) {
		if (vehicle->AnimateAscentDescent()) {
			if (!vehicle->IsFlying()) {
				// If we landed, them disembark
				Main_Data::game_player->SetFlying(vehicle->IsFlying());
				data()->aboard = false;
				SetFacing(Down);
				data()->vehicle = 0;
				SetMoveSpeed(data()->preboard_move_speed);

				Main_Data::game_system->BgmPlay(Main_Data::game_system->GetBeforeVehicleMusic());
			}

			return true;
		}
	}
	return false;
}

void Game_Player::UpdateNextMovementAction() {
	if (UpdateAirship()) {
		return;
	}

	UpdateMoveRoute(data()->move_route_index, data()->move_route, true);

	if (Game_Map::GetInterpreter().IsRunning()) {
		SetMenuCalling(false);
		return;
	}

	if(IsPaused() || IsMoveRouteOverwritten() || Game_Message::IsMessageActive()) {
		return;
	}

	if (IsEncounterCalling()) {
		SetMenuCalling(false);
		SetEncounterCalling(false);

		BattleArgs args;
		if (Game_Map::PrepareEncounter(args)) {
			Scene::instance->SetRequestedScene(Scene_Battle::Create(std::move(args)));
			return;
		}
	}

	if (IsMenuCalling()) {
		SetMenuCalling(false);

		ResetAnimation();
		Main_Data::game_system->SePlay(Main_Data::game_system->GetSystemSE(Main_Data::game_system->SFX_Decision));
		Scene::instance->SetRequestedScene(std::make_shared<Scene_Menu>());
		return;
	}

	CheckEventTriggerHere({ lcf::rpg::EventPage::Trigger_collision }, false);

	if (Game_Map::IsAnyEventStarting()) {
		return;
	}

	int move_dir = -1;
	switch (Input::getDir4()) {
		case 2:
			move_dir = Down;
			break;
		case 4:
			move_dir = Left;
			break;
		case 6:
			move_dir = Right;
			break;
		case 8:
			move_dir = Up;
			break;
	}
	if (move_dir >= 0) {
		SetThrough((Player::debug_flag && Input::IsPressed(Input::DEBUG_THROUGH)) || data()->route_through);
		Move(move_dir);
		ResetThrough();
		if (IsStopping()) {
			int front_x = Game_Map::XwithDirection(GetX(), GetDirection());
			int front_y = Game_Map::YwithDirection(GetY(), GetDirection());
			CheckEventTriggerThere({lcf::rpg::EventPage::Trigger_touched, lcf::rpg::EventPage::Trigger_collision}, front_x, front_y, false);
		}
	}

	if (IsStopping()) {
		if (Input::IsTriggered(Input::DECISION)) {
			if (!GetOnOffVehicle()) {
				CheckActionEvent();
			}
		}
		return;
	}

	Main_Data::game_party->IncSteps();
	if (Main_Data::game_party->ApplyStateDamage()) {
		Main_Data::game_screen->FlashMapStepDamage();
	}
	UpdateEncounterSteps();
}

void Game_Player::UpdateMovement(int amount) {
	const bool was_jumping = IsJumping();

	Game_Character::UpdateMovement(amount);

	UpdateScroll(amount, was_jumping);

	if (!IsMoveRouteOverwritten() && IsStopping()) {
		TriggerSet triggers = { lcf::rpg::EventPage::Trigger_touched, lcf::rpg::EventPage::Trigger_collision };
		CheckEventTriggerHere(triggers, false);
	}
}

void Game_Player::Update() {
	Game_Character::Update();

	if (IsStopping()) {
		if (data()->boarding) {
			// Boarding completed
			data()->aboard = true;
			data()->boarding = false;
			// Note: RPG_RT ignores the lock_facing flag here!
			SetFacing(Left);

			auto* vehicle = GetVehicle();
			SetMoveSpeed(vehicle->GetMoveSpeed());
		}
		if (data()->unboarding) {
			// Unboarding completed
			data()->unboarding = false;
		}
	}

	auto* vehicle = GetVehicle();

	if (IsAboard() && vehicle) {
		vehicle->SyncWithRider(this);
	}

	UpdatePan();

	// ESC-Menu calling
	if (Main_Data::game_system->GetAllowMenu()
			&& !Game_Message::IsMessageActive()
			&& !Game_Map::GetInterpreter().IsRunning())
	{
		if (Input::IsTriggered(Input::CANCEL)) {
			SetMenuCalling(true);
		}
	}
}

bool Game_Player::CheckActionEvent() {
	if (IsFlying()) {
		return false;
	}

	bool result = false;
	int front_x = Game_Map::XwithDirection(GetX(), GetDirection());
	int front_y = Game_Map::YwithDirection(GetY(), GetDirection());

	result |= CheckEventTriggerThere({lcf::rpg::EventPage::Trigger_touched, lcf::rpg::EventPage::Trigger_collision}, front_x, front_y, true);
	result |= CheckEventTriggerHere({lcf::rpg::EventPage::Trigger_action}, true);

	// Counter tile loop stops only if you talk to an action event.
	bool got_action = CheckEventTriggerThere({lcf::rpg::EventPage::Trigger_action}, front_x, front_y, true);
	// RPG_RT allows maximum of 3 counter tiles
	for (int i = 0; !got_action && i < 3; ++i) {
		if (!Game_Map::IsCounter(front_x, front_y)) {
			break;
		}

		front_x = Game_Map::XwithDirection(front_x, GetDirection());
		front_y = Game_Map::YwithDirection(front_y, GetDirection());

		got_action |= CheckEventTriggerThere({lcf::rpg::EventPage::Trigger_action}, front_x, front_y, true);
	}
	return result || got_action;
}

bool Game_Player::CheckEventTriggerHere(TriggerSet triggers, bool triggered_by_decision_key) {
	if (InAirship()) {
		return false;
	}

	bool result = false;

	for (auto& ev: Game_Map::GetEvents()) {
		const auto trigger = ev.GetTrigger();
		if (ev.IsActive()
				&& ev.GetX() == GetX()
				&& ev.GetY() == GetY()
				&& ev.GetLayer() != lcf::rpg::EventPage::Layers_same
				&& trigger >= 0
				&& triggers[trigger]) {
			SetEncounterCalling(false);
			result |= ev.ScheduleForegroundExecution(triggered_by_decision_key, true);
		}
	}
	return result;
}

bool Game_Player::CheckEventTriggerThere(TriggerSet triggers, int x, int y, bool triggered_by_decision_key) {
	if (InAirship()) {
		return false;
	}
	bool result = false;

	for (auto& ev : Game_Map::GetEvents()) {
		const auto trigger = ev.GetTrigger();
		if (ev.IsActive()
				&& ev.GetX() == x
				&& ev.GetY() == y
				&& ev.GetLayer() == lcf::rpg::EventPage::Layers_same
				&& trigger >= 0
				&& triggers[trigger]) {
			SetEncounterCalling(false);
			result |= ev.ScheduleForegroundExecution(triggered_by_decision_key, true);
		}
	}
	return result;
}

void Game_Player::ResetGraphic() {

	auto* actor = Main_Data::game_party->GetActor(0);
	if (actor == nullptr) {
		SetSpriteGraphic("", 0);
		SetTransparency(0);
		return;
	}

	SetSpriteGraphic(ToString(actor->GetSpriteName()), actor->GetSpriteIndex());
	SetTransparency(actor->GetSpriteTransparency());
}

bool Game_Player::GetOnOffVehicle() {
	if (IsDirectionDiagonal(GetDirection())) {
		SetDirection(GetFacing());
	}

	return IsAboard()
		? GetOffVehicle()
		: GetOnVehicle();
}

bool Game_Player::GetOnVehicle() {
	assert(!IsDirectionDiagonal(GetDirection()));
	assert(!IsAboard());

	auto* vehicle = Game_Map::GetVehicle(Game_Vehicle::Airship);

	if (vehicle->IsInPosition(GetX(), GetY()) && IsStopping() && vehicle->IsStopping()) {
		data()->vehicle = Game_Vehicle::Airship;
		data()->aboard = true;

		// Note: RPG_RT ignores the lock_facing flag here!
		SetFacing(Left);

		data()->preboard_move_speed = GetMoveSpeed();
		SetMoveSpeed(vehicle->GetMoveSpeed());
		vehicle->StartAscent();
		Main_Data::game_player->SetFlying(vehicle->IsFlying());
	} else {
		const auto front_x = Game_Map::XwithDirection(GetX(), GetDirection());
		const auto front_y = Game_Map::YwithDirection(GetY(), GetDirection());

		vehicle = Game_Map::GetVehicle(Game_Vehicle::Ship);
		if (!vehicle->IsInPosition(front_x, front_y)) {
			vehicle = Game_Map::GetVehicle(Game_Vehicle::Boat);
			if (!vehicle->IsInPosition(front_x, front_y)) {
				return false;
			}
		}

		if (!Game_Map::CanEmbarkShip(*this, front_x, front_y)) {
			return false;
		}

		SetThrough(true);
		Move(GetDirection());
		// FIXME: RPG_RT resets through to route_through || not visible?
		ResetThrough();

		data()->vehicle = vehicle->GetVehicleType();
		data()->preboard_move_speed = GetMoveSpeed();
		data()->boarding = true;
	}

	Main_Data::game_system->SetBeforeVehicleMusic(Main_Data::game_system->GetCurrentBGM());
	Main_Data::game_system->BgmPlay(vehicle->GetBGM());
	return true;
}

bool Game_Player::GetOffVehicle() {
	assert(!IsDirectionDiagonal(GetDirection()));
	assert(IsAboard());

	auto* vehicle = GetVehicle();
	if (!vehicle) {
		return false;
	}

	if (InAirship()) {
		if (vehicle->IsAscendingOrDescending()) {
			return false;
		}

		// Note: RPG_RT ignores the lock_facing flag here!
		SetFacing(Left);
		vehicle->StartDescent();
		return true;
	}

	const auto front_x = Game_Map::XwithDirection(GetX(), GetDirection());
	const auto front_y = Game_Map::YwithDirection(GetY(), GetDirection());

	if (!Game_Map::CanDisembarkShip(*this, front_x, front_y)) {
		return false;
	}

	vehicle->SetDefaultDirection();
	data()->aboard = false;
	SetMoveSpeed(data()->preboard_move_speed);
	data()->unboarding = true;

	SetThrough(true);
	Move(GetDirection());
	ResetThrough();

	data()->vehicle = 0;
	Main_Data::game_system->BgmPlay(Main_Data::game_system->GetBeforeVehicleMusic());

	return true;
}

void Game_Player::ForceGetOffVehicle() {
	if (!IsAboard()) {
		return;
	}

	auto* vehicle = GetVehicle();
	vehicle->ForceLand();
	vehicle->SetDefaultDirection();

	data()->flying = false;
	data()->aboard = false;
	SetMoveSpeed(data()->preboard_move_speed);
	data()->unboarding = true;
	data()->vehicle = 0;
	Main_Data::game_system->BgmPlay(Main_Data::game_system->GetBeforeVehicleMusic());
}

bool Game_Player::InVehicle() const {
	return data()->vehicle > 0;
}

bool Game_Player::InAirship() const {
	return data()->vehicle == Game_Vehicle::Airship;
}

Game_Vehicle* Game_Player::GetVehicle() const {
	return Game_Map::GetVehicle((Game_Vehicle::Type) data()->vehicle);
}

bool Game_Player::Move(int dir) {
	if (!IsStopping()) {
		return true;
	}

	Game_Character::Move(dir);
	if (IsStopping()) {
		return false;
	}

	if (InAirship()) {
		return true;
	}

	int terrain_id = Game_Map::GetTerrainTag(GetX(), GetY());
	const auto* terrain = lcf::ReaderUtil::GetElement(lcf::Data::terrains, terrain_id);
	bool red_flash = false;

	if (terrain) {
		if (terrain->damage != 0) {
			for (auto hero : Main_Data::game_party->GetActors()) {
				if (terrain->damage < 0 || !hero->PreventsTerrainDamage()) {
					if (terrain->damage > 0) {
						red_flash = true;
					}
					hero->ChangeHp(-terrain->damage, false);
				}
			}
		}
		if (!terrain->on_damage_se || red_flash) {
			Main_Data::game_system->SePlay(terrain->footstep);
		}
	} else {
		Output::Warning("Player BeginMove: Invalid terrain ID {} at ({}, {})", terrain_id, GetX(), GetY());
	}

	if (red_flash) {
		Main_Data::game_screen->FlashMapStepDamage();
	}

	return true;
}

bool Game_Player::IsAboard() const {
	return data()->aboard;
}

bool Game_Player::IsBoardingOrUnboarding() const {
	return data()->boarding || data()->unboarding;
}

void Game_Player::UpdateEncounterSteps() {
	if (Player::debug_flag && Input::IsPressed(Input::DEBUG_THROUGH)) {
		return;
	}

	if(IsFlying()) {
		return;
	}

	const auto encounter_rate = Game_Map::GetEncounterRate();

	if (encounter_rate <= 0) {
		SetEncounterSteps(0);
		return;
	}

	int x = GetX();
	int y = GetY();

	const auto* terrain = lcf::ReaderUtil::GetElement(lcf::Data::terrains, Game_Map::GetTerrainTag(x,y));
	if (!terrain) {
		Output::Warning("UpdateEncounterSteps: Invalid terrain at ({}, {})", x, y);
		return;
	}

	data()->encounter_steps += terrain->encounter_rate;

	struct Row {
		int ratio;
		float pmod;
	};

#if 1
	static constexpr Row enc_table[] = {
		{ 0, 0.0625},
		{ 20, 0.125 },
		{ 40, 0.25 },
		{ 60, 0.5 },
		{ 100, 2.0 },
		{ 140, 4.0 },
		{ 160, 8.0 },
		{ 180, 16.0 },
		{ INT_MAX, 16.0 }
	};
#else
	//Old versions of RM2k used this table.
	//Left here for posterity.
	static constexpr Row enc_table[] = {
		{ 0, 0.5 },
		{ 20, 2.0 / 3.0 },
		{ 50, 5.0 / 6.0 },
		{ 100, 6.0 / 5.0 },
		{ 200, 3.0 / 2.0 },
		{ INT_MAX, 3.0 / 2.0 }
	};
#endif
	const auto ratio = GetEncounterSteps() / encounter_rate;

	auto& idx = last_encounter_idx;
	while (ratio > enc_table[idx+1].ratio) {
		++idx;
	}
	const auto& row = enc_table[idx];

	const auto pmod = row.pmod;
	const auto p = (1.0f / float(encounter_rate)) * pmod * (float(terrain->encounter_rate) / 100.0f);

	if (!Rand::PercentChance(p)) {
		return;
	}

	SetEncounterSteps(0);
	SetEncounterCalling(true);
}

void Game_Player::SetEncounterSteps(int steps) {
	last_encounter_idx = 0;
	data()->encounter_steps = steps;
}

void Game_Player::LockPan() {
	data()->pan_state = lcf::rpg::SavePartyLocation::PanState_fixed;
}

void Game_Player::UnlockPan() {
	data()->pan_state = lcf::rpg::SavePartyLocation::PanState_follow;
}

void Game_Player::StartPan(int direction, int distance, int speed) {
	distance *= SCREEN_TILE_SIZE;

	if (direction == PanUp) {
		int new_pan = data()->pan_finish_y + distance;
		data()->pan_finish_y = new_pan;
	} else if (direction == PanRight) {
		int new_pan = data()->pan_finish_x - distance;
		data()->pan_finish_x = new_pan;
	} else if (direction == PanDown) {
		int new_pan = data()->pan_finish_y - distance;
		data()->pan_finish_y = new_pan;
	} else if (direction == PanLeft) {
		int new_pan = data()->pan_finish_x + distance;
		data()->pan_finish_x = new_pan;
	}

	data()->pan_speed = 2 << speed;
}

void Game_Player::ResetPan(int speed) {
	data()->pan_finish_x = lcf::rpg::SavePartyLocation::kPanXDefault;
	data()->pan_finish_y = lcf::rpg::SavePartyLocation::kPanYDefault;
	data()->pan_speed = 2 << speed;
}

int Game_Player::GetPanWait() {
	const auto distance = std::max(
			std::abs(data()->pan_current_x - data()->pan_finish_x),
			std::abs(data()->pan_current_y - data()->pan_finish_y));
	const auto speed = data()->pan_speed;
	assert(speed > 0);
	return distance / speed + (distance % speed != 0);
}

void Game_Player::UpdatePan() {
	if (!IsPanActive())
		return;

	const int step = data()->pan_speed;
	const int pan_remain_x = data()->pan_current_x - data()->pan_finish_x;
	const int pan_remain_y = data()->pan_current_y - data()->pan_finish_y;

	int dx = std::min(step, std::abs(pan_remain_x));
	dx = pan_remain_x >= 0 ? dx : -dx;
	int dy = std::min(step, std::abs(pan_remain_y));
	dy = pan_remain_y >= 0 ? dy : -dy;

	int screen_x = Game_Map::GetPositionX();
	int screen_y = Game_Map::GetPositionY();

	Game_Map::AddScreenX(screen_x, dx);
	Game_Map::AddScreenY(screen_y, dy);

	// If we hit the edge of the map before pan finishes.
	if (dx == 0 && dy == 0) {
		return;
	}

	Game_Map::Scroll(dx, dy);

	data()->pan_current_x -= dx;
	data()->pan_current_y -= dy;
}

