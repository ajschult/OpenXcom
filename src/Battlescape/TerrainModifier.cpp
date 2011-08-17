/*
 * Copyright 2010 OpenXcom Developers.
 *
 * This file is part of OpenXcom.
 *
 * OpenXcom is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * OpenXcom is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with OpenXcom.  If not, see <http://www.gnu.org/licenses/>.
 */
#define _USE_MATH_DEFINES
#include <cmath>
#include <set>
#include "TerrainModifier.h"
#include "SDL.h"
#include "BattleAIState.h"
#include "AggroBAIState.h"
#include "../Savegame/SavedBattleGame.h"
#include "../Savegame/Tile.h"
#include "../Savegame/BattleUnit.h"
#include "../Savegame/Soldier.h"
#include "../Savegame/Alien.h"
#include "../Engine/RNG.h"
#include "../Ruleset/MapDataSet.h"
#include "../Ruleset/MapData.h"
#include "../Ruleset/RuleAlien.h"
#include "../Ruleset/RuleSoldier.h"
#include "../Resource/ResourcePack.h"

namespace OpenXcom
{

/**
 * Sets up a TerrainModifier.
 * @param save pointer to SavedBattleGame object.
 */
TerrainModifier::TerrainModifier(SavedBattleGame *save, std::vector<Uint16> *voxelData) : _save(save), _voxelData(voxelData)
{

}

/**
 * Deletes the TerrainModifier.
 */
TerrainModifier::~TerrainModifier()
{

}


/**
  * Calculate sun shading for the whole terrain.
  */
void TerrainModifier::calculateSunShading()
{
	for (int i = 0; i < _save->getWidth() * _save->getLength() * _save->getHeight(); ++i)
	{
		calculateSunShading(_save->getTiles()[i]);
	}
}

/**
  * Calculate sun shading for 1 tile. Sun comes from above and is blocked by floors or objects.
  * @param tile The tile to calculate sun shading for.
  */
void TerrainModifier::calculateSunShading(Tile *tile)
{
	const int layer = 0; // Ambient lighting layer.

	int power = 15 - _save->getGlobalShade();

	// At night/dusk sun isn't dropping shades
	if (_save->getGlobalShade() <= 5)
	{
		if (verticalBlockage(_save->getTile(Position(tile->getPosition().x, tile->getPosition().y, _save->getHeight() - 1)), tile, DT_NONE))
		{
			power-=2;
		}
	}

	tile->addLight(power, layer);
}

/**
  * Recalculate lighting for the terrain: objects,items,fire.
  */
void TerrainModifier::calculateTerrainLighting()
{
	const int layer = 1; // Static lighting layer.
	const int fireLightPower = 15; // amount of light a fire generates

	// during daytime don't calculate lighting
	if (_save->getGlobalShade() < 1)
		return;

	// reset all light to 0 first
	for (int i = 0; i < _save->getWidth() * _save->getLength() * _save->getHeight(); ++i)
	{
		_save->getTiles()[i]->resetLight(layer);
	}

	// add lighting of terrain
	for (int i = 0; i < _save->getWidth() * _save->getLength() * _save->getHeight(); ++i)
	{
		// only floors and objects can light up
		if (_save->getTiles()[i]->getMapData(MapData::O_FLOOR)
			&& _save->getTiles()[i]->getMapData(MapData::O_FLOOR)->getLightSource())
		{
			addLight(_save->getTiles()[i]->getPosition(), _save->getTiles()[i]->getMapData(MapData::O_FLOOR)->getLightSource(), layer);
		}
		if (_save->getTiles()[i]->getMapData(MapData::O_OBJECT)
			&& _save->getTiles()[i]->getMapData(MapData::O_OBJECT)->getLightSource())
		{
			addLight(_save->getTiles()[i]->getPosition(), _save->getTiles()[i]->getMapData(MapData::O_OBJECT)->getLightSource(), layer);
		}

		// fires
		if (_save->getTiles()[i]->getFire())
		{
			addLight(_save->getTiles()[i]->getPosition(), fireLightPower, layer);
		}

	}

	// todo: add lighting of items (flares)

}

/**
  * Recalculate lighting for the units.
  */
void TerrainModifier::calculateUnitLighting()
{
	const int layer = 2; // Dynamic lighting layer.
	const int personalLightPower = 15; // amount of light a unit generates

	// during daytime don't calculate lighting
	if (_save->getGlobalShade() < 1)
		return;

	// reset all light to 0 first
	for (int i = 0; i < _save->getWidth() * _save->getLength() * _save->getHeight(); ++i)
	{
		_save->getTiles()[i]->resetLight(layer);
	}

	// add lighting of soldiers
	for (std::vector<BattleUnit*>::iterator i = _save->getUnits()->begin(); i != _save->getUnits()->end(); ++i)
	{
		if ((*i)->getFaction() == FACTION_PLAYER && !(*i)->isOut())
		{
			addLight((*i)->getPosition(), personalLightPower, layer);
		}
	}

}

/**
 * Calculates line of sight of a soldier. For every visible tile fog of war is removed.
 * @param unit
 * @return true when new aliens spotted
 */
bool TerrainModifier::calculateFOV(BattleUnit *unit)
{
	// units see 90 degrees sidewards.
	int startAngle[8] = { 45, 0, -45, 270, 225, 180, 135, 90 };
	int endAngle[8] = { 135, 90, 45, 360, 315, 270, 225, 180 };

	double centerZ = (unit->getPosition().z * 2) + 1.5;
	double centerX = unit->getPosition().x + 0.5;
	double centerY = unit->getPosition().y + 0.5;
	int objectViewDistance;
	int unitViewDistance;

	// units see 90 degrees down and 60 degrees up.
	int startFi = -90;
	int endFi = 60;

	std::set<Tile*> tilesAffected;
	std::pair<std::set<Tile*>::iterator,bool> ret;


	int visibleUnitsChecksum = 0;

	if (unit->getPosition().z == 0)
	{
		startFi = 0;
	}

	// we see the tile we are standing on
	if (unit->getFaction() == FACTION_PLAYER)
		unit->getTile()->setDiscovered(true, 2);

	// calculate a visible units checksum - if it changed during this step, the soldier stops walking
	for (std::vector<BattleUnit*>::iterator i = unit->getVisibleUnits()->begin(); i != unit->getVisibleUnits()->end(); ++i)
		visibleUnitsChecksum += (*i)->getId()+1;

	unit->clearVisibleUnits();

	// raytrace up and down
	for (int fi = startFi; fi <= endFi; fi += 6)
	{
		double cos_fi = cos(fi * M_PI / 180.0);
		double sin_fi = sin(fi * M_PI / 180.0);

		// raytrace every 3 degrees makes sure we cover all tiles in a circle.
		for (int te = startAngle[unit->getDirection()]; te <= endAngle[unit->getDirection()]; te += 3)
		{
			double cos_te = cos(te * M_PI / 180.0);
			double sin_te = sin(te * M_PI / 180.0);

			Tile *origin = unit->getTile();
			double l = 0;
			double vx, vy, vz;
			int tileX, tileY, tileZ;

			// shade goes from 0 to 15 (day -> night), while the viewdistance goes from 20 to 9 (day -> night)
			objectViewDistance = 20;

			unitViewDistance = objectViewDistance;
			while (objectViewDistance > 0)
			{
				l++;
				vx = centerX + l * cos_te * cos_fi;
				vy = centerY + l * sin_te * cos_fi;
				vz = centerZ + l * sin_fi;

				tileZ = int(floor(vz / 2.0));
				tileX = int(floor(vx));
				tileY = int(floor(vy));

				objectViewDistance--;

				Tile *dest = _save->getTile(Position(tileX, tileY, tileZ));
				if (!dest) break; // out of map!

				// horizontal blockage by walls
				objectViewDistance -= horizontalBlockage(origin, dest, DT_NONE); // line of sight is all or nothing

				// vertical blockage by ceilings/floors
				objectViewDistance -= verticalBlockage(origin, dest, DT_NONE); // line of sight is all or nothing

				if (objectViewDistance > 0 && dest->getShade() < 10) // shade lower than 10 allows to see a unit
				{
					ret = tilesAffected.insert(dest); // check if we had this tile already
					if (ret.second)
					{
						if (unitViewDistance > 0)
							checkForVisibleUnits(unit, dest);

						if (unit->getFaction() == FACTION_PLAYER)
						{
							dest->setDiscovered(true, 2);
							// walls to the east or south of a visible tile, we see that too
							Tile* t = _save->getTile(Position(tileX + 1, tileY, tileZ));
							if (t) t->setDiscovered(true, 0);
							t = _save->getTile(Position(tileX, tileY - 1, tileZ));
							if (t) t->setDiscovered(true, 1);
						}
					}
				}
				if (dest->getShade() > 7) // shade higher than 7 decreases the viewdistance
				{
					objectViewDistance--;
				}
				// smoke blocks the viewdistance of units only
				unitViewDistance -= int(dest->getSmoke() / 3);
				origin = dest;
			}
		}
	}

	int newChecksum = 0;
	for (std::vector<BattleUnit*>::iterator i = unit->getVisibleUnits()->begin(); i != unit->getVisibleUnits()->end(); ++i)
		newChecksum += (*i)->getId()+1;

	return visibleUnitsChecksum < newChecksum;
}

/**
 * Check for an opposing unit on this tile
 * @param unit
 * @param tile
 */
bool TerrainModifier::checkForVisibleUnits(BattleUnit *unit, Tile *tile)
{
	BattleUnit *bu = tile->getUnit();

	if (bu == 0 || bu->isOut())
	{
		return false;
	}

	if (unit->getFaction() == FACTION_PLAYER && (bu->getFaction() == FACTION_PLAYER || bu->getFaction() == FACTION_NEUTRAL))
	{
		return false;
	}

	if (unit->getFaction() == FACTION_HOSTILE && bu->getFaction() == FACTION_HOSTILE)
	{
		return false;
	}

	Position originVoxel, targetVoxel;
	originVoxel = Position((unit->getPosition().x * 16) + 8, (unit->getPosition().y * 16) + 8, unit->getPosition().z*24);
	originVoxel.z += -tile->getTerrainLevel();
	if (unit->isKneeled())
	{
		originVoxel.z += unit->getUnit()->getKneelHeight();
	}
	else
	{
		originVoxel.z += unit->getUnit()->getStandHeight();
	}
	bool unitSeen = false;

	targetVoxel = Position((bu->getPosition().x * 16) + 8, (bu->getPosition().y * 16) + 8, bu->getPosition().z*24);
	int targetMinHeight = targetVoxel.z - _save->getTile(bu->getPosition())->getTerrainLevel();
	int targetMaxHeight;
	if (bu->isKneeled())
	{
		 targetMaxHeight = targetMinHeight + bu->getUnit()->getKneelHeight();
	}
	else
	{
		targetMaxHeight = targetMinHeight + bu->getUnit()->getStandHeight();
	}

	// scan ray from top to bottom
	for (int i = targetMaxHeight; i > targetMinHeight; i-=2)
	{
		targetVoxel.z = i;
		int test = calculateLine(originVoxel, targetVoxel, false, 0, unit);
		Position hitPosition = Position(targetVoxel.x/16, targetVoxel.y/16, targetVoxel.z/24);
		if (test == -1 || (test == 4 && bu->getPosition() == hitPosition))
		{
			unitSeen = true;
			unit->addToVisibleUnits(bu);
			break;
		}
	}

	return unitSeen;
}


/**
 * Calculates line of sight of a soldiers within range of the Position.
 * TODO: review this, because it is recalculating all soldiers.
 * @param position
 */
void TerrainModifier::calculateFOV(const Position &position)
{
	for (std::vector<BattleUnit*>::iterator i = _save->getUnits()->begin(); i != _save->getUnits()->end(); ++i)
	{
		if ((*i)->getFaction() == _save->getSide())
		{
			calculateFOV(*i);
		}
	}
}

/**
 * Checks if of the opposing faction a sniper sees this unit. The unit with the highest reaction score will be compared with the current unit's reaction score.
 * If it's higher, a shot is fired when enough time units a weapon and ammo available.
 * @param unit
 * @param action
 * @param faceToWards a unit faces towards the sniper when it shoots
 */
bool TerrainModifier::checkReactionFire(BattleUnit *unit, BattleAction *action, BattleUnit *potentialVictim, bool recalculateFOV)
{
	double highestReactionScore = 0;
	action->actor = 0;

	// reaction fire only triggered when the actioning unit is of the currently playing side
	if (unit->getFaction() != _save->getSide())
	{
		return false;
	}

	if (potentialVictim && RNG::generate(0, 4) == 1 && potentialVictim->getFaction() == FACTION_HOSTILE)
	{
		potentialVictim->lookAt(unit->getPosition());
		while (potentialVictim->getStatus() == STATUS_TURNING)
		{
			recalculateFOV = true;
			potentialVictim->turn();
		}
	}

	for (std::vector<BattleUnit*>::iterator i = _save->getUnits()->begin(); i != _save->getUnits()->end(); ++i)
	{
		int x = abs(unit->getPosition().x - (*i)->getPosition().x);
		int y = abs(unit->getPosition().y - (*i)->getPosition().y);
		int distance = int(floor(sqrt(float(x*x + y*y)) + 0.5));
		if (distance < 19 && (*i)->getFaction() != _save->getSide() && !(*i)->isOut())
		{
			if (recalculateFOV)
			{
				calculateFOV(*i);
			}
			for (std::vector<BattleUnit*>::iterator j = (*i)->getVisibleUnits()->begin(); j != (*i)->getVisibleUnits()->end(); ++j)
			{
				if ((*j) == unit && (*i)->getReactionScore() > highestReactionScore)
				{
					// I see you!
					highestReactionScore = (*i)->getReactionScore();
					action->actor = (*i);
				}
			}
		}
	}

	if (action->actor && highestReactionScore > unit->getReactionScore())
	{
		action->type = BA_SNAPSHOT;
		action->target = unit->getPosition();
		// lets try and shoot
		action->weapon = _save->getMainHandWeapon(action->actor);
		int tu = action->actor->getActionTUs(action->type, action->weapon);
		if (action->weapon && action->weapon->getAmmoItem() && action->weapon->getAmmoItem()->getAmmoQuantity())
		{
			if (action->actor->spendTimeUnits(tu, _save->getDebugMode()))
			{
				// if the target is hostile, it will aggro
				if (unit->getFaction() == FACTION_HOSTILE)
				{
					AggroBAIState *aggro = dynamic_cast<AggroBAIState*>(unit->getCurrentAIState());
					if (aggro == 0)
					{
						aggro = new AggroBAIState(_save, unit);
						unit->setAIState(aggro);
					}
					aggro->setAggroTarget(action->actor);
				}
				// if the shooter is hostile, he will aggro
				if (action->actor->getFaction() == FACTION_HOSTILE)
				{
					AggroBAIState *aggro = dynamic_cast<AggroBAIState*>(action->actor->getCurrentAIState());
					if (aggro == 0)
					{
						aggro = new AggroBAIState(_save, action->actor);
						action->actor->setAIState(aggro);
					}
					aggro->setAggroTarget(unit);
				}
				return true;
			}
		}
	}

	return false;
}

/**
 * Adds circular light pattern starting from center and loosing power with distance travelled.
 * @param center
 * @param power
 * @param layer Light is seperated in 3 layers: Ambient, Static and Dynamic.
 */
void TerrainModifier::addLight(const Position &center, int power, int layer)
{
	// only loop through the positive quadrant.
	for (int x = 0; x <= power; ++x)
	{
		for (int y = 0; y <= power; ++y)
		{
			for (int z = 0; z < _save->getHeight(); z++)
			{
				int distance = int(floor(sqrt(float(x*x + y*y)) + 0.5));

				if (_save->getTile(Position(center.x + x,center.y + y, z)))
					_save->getTile(Position(center.x + x,center.y + y, z))->addLight(power - distance, layer);

				if (_save->getTile(Position(center.x - x,center.y - y, z)))
					_save->getTile(Position(center.x - x,center.y - y, z))->addLight(power - distance, layer);

				if (_save->getTile(Position(center.x - x,center.y + y, z)))
					_save->getTile(Position(center.x - x,center.y + y, z))->addLight(power - distance, layer);

				if (_save->getTile(Position(center.x + x,center.y - y, z)))
					_save->getTile(Position(center.x + x,center.y - y, z))->addLight(power - distance, layer);
			}
		}
	}
}

/**
 * HE, smoke and fire explodes in a circular pattern on 1 level only. HE however damages floor tiles of the above level. Not the units on it.
 * HE destroys an object if its armor is lower than the explosive power, then it's HE blockage is applied for further propagation.
 * See http://www.ufopaedia.org/index.php?title=Explosions for more info.
 * @param center
 * @param power
 * @param affector
 * @param maxRadius
 */
void TerrainModifier::explode(const Position &center, int power, ItemDamageType type, int maxRadius, BattleUnit *unit)
{
	if (type == DT_AP || type == DT_PLASMA)
	{
		int part = voxelCheck(center, unit);
		if (part >= 0 && part <= 3)
		{
			// power 25% to 75%
			int rndPower = RNG::generate(power/4, (power*3)/4); //RNG::boxMuller(power, power/6)
			_save->getTile(Position(center.x/16, center.y/16, center.z/24))->damage(part, rndPower);
		}
		else if (part == 4)
		{
			// power 0 - 200%
			int rndPower = RNG::generate(0, power*2); // RNG::boxMuller(power, power/3)
			_save->getTile(Position(center.x/16, center.y/16, center.z/24))->getUnit()->damage(Position(center.x%16, center.y%16, center.z%24), rndPower);

			// conventional weapons can cause additional stun damage
			if (type == DT_AP)
			{
				_save->getTile(Position(center.x/16, center.y/16, center.z/24))->getUnit()->stun(RNG::generate(0, rndPower/4));
			}
		}
	}
	else
	{
		double centerZ = (int)(center.z / 24) + 0.5;
		double centerX = (int)(center.x / 16) + 0.5;
		double centerY = (int)(center.y / 16) + 0.5;
		int power_;
		std::set<Tile*> tilesAffected;
		std::pair<std::set<Tile*>::iterator,bool> ret;

		if (type == DT_IN)
		{
			power /= 2;
		}

		// raytrace every 3 degrees makes sure we cover all tiles in a circle.
		for (int te = 0; te <= 360; te += 3)
		{
			double cos_te = cos(te * M_PI / 180.0);
			double sin_te = sin(te * M_PI / 180.0);

			Tile *origin = _save->getTile(center);
			double l = 0;
			double vx, vy, vz;
			int tileX, tileY, tileZ;
			power_ = power + 1;

			while (power_ > 0 && l <= maxRadius)
			{
				vx = centerX + l * cos_te;
				vy = centerY + l * sin_te;
				vz = centerZ;

				tileZ = int(floor(vz));
				tileX = int(floor(vx));
				tileY = int(floor(vy));

				Tile *dest = _save->getTile(Position(tileX, tileY, tileZ));
				if (!dest) break; // out of map!

				// horizontal blockage by walls
				power_ -= horizontalBlockage(origin, dest, type);

				if (power_ > 0)
				{
					if (type == DT_HE)
					{
						// explosives do 1/2 damage to terrain and 1/2 up to 3/2 random damage to units
						dest->setExplosive(power_ / 2);
					}

					ret = tilesAffected.insert(dest); // check if we had this tile already
					if (ret.second)
					{
						if (type == DT_HE)
						{
							// power 50 - 150%
							if (dest->getUnit())
								dest->getUnit()->damage(Position(0, 0, 0), (int)(RNG::generate(power_/2.0, power_*1.5)));
							// destroy floors above
							Tile *tileAbove = _save->getTile(Position(tileX, tileY, tileZ+1));
							if ( tileAbove && tileAbove->getMapData(MapData::O_FLOOR) && power_ / 2 >= tileAbove->getMapData(MapData::O_FLOOR)->getArmor())
							{
								tileAbove->destroy(MapData::O_FLOOR);
							}
							// very slight chance (0-25%) to minor damage to units above or below explosion
							if (tileAbove->getUnit())
								tileAbove->getUnit()->damage(Position(0, 0, 0), (int)(RNG::generate(0.0, power_/4.0)));
							Tile *tileBelow = _save->getTile(Position(tileX, tileY, tileZ-1));
							if (tileBelow && !dest->getMapData(MapData::O_FLOOR) && tileBelow->getUnit())
								tileBelow->getUnit()->damage(Position(0, 0, 0), (int)(RNG::generate(0.0, power_/4.0)));
						}
						if (type == DT_SMOKE)
						{
							// smoke from explosions always stay 10 to 20 turns
							if (dest->getSmoke() < 10)
							{
								dest->addSmoke(RNG::generate(power_/10, 14));
							}
						}
						if (type == DT_IN)
						{
							if (dest->getFire() == 0)
							{
								dest->ignite();
							}
							if (dest->getUnit())
							{
								dest->getUnit()->damage(Position(0, 0, 0), RNG::generate(0, power_/3)); // immediate IN damage
								dest->getUnit()->setFire(RNG::generate(1, 5)); // catch fire and burn for 1-5 rounds
							}
						}
					}
				}
				power_ -= 10; // explosive damage decreases by 10
				origin = dest;
				l++;
			}
		}

		// now detonate the tiles affected with HE
		if (type == DT_HE)
		{
			for (std::set<Tile*>::iterator i = tilesAffected.begin(); i != tilesAffected.end(); ++i)
			{
				(*i)->detonate();
			}
		}
	}

	// recalculate line of sight (to optimise: only units in range)
	calculateFOV(center);
	calculateTerrainLighting(); // fires could have been started
}

/**
 * Chained explosions are explosions wich occur after an explosive map object is destroyed.
 * May be due a direct hit, other explosion or fire.
 * @return tile on which a explosion occured
 */
Tile *TerrainModifier::checkForChainedExplosions()
{

	for (int i = 0; i < _save->getWidth() * _save->getLength() * _save->getHeight(); ++i)
	{
		if (_save->getTiles()[i]->getExplosive())
		{
			return _save->getTiles()[i];
		}
	}
	return 0;
}

/**
 * The amount of power that is blocked going from one tile to another on a different level.
 * Can cross more than one level. Only floor tiles are taken into account.
 * @param startTile
 * @param endTile
 * @param affector
 * @return amount of blockage
 */
int TerrainModifier::verticalBlockage(Tile *startTile, Tile *endTile, ItemDamageType type)
{
	int block = 0;

	// safety check
	if (startTile == 0 || endTile == 0) return 0;

	int direction = endTile->getPosition().z - startTile->getPosition().z;
	int x = startTile->getPosition().x;
	int y = startTile->getPosition().y;

	if (direction < 0) // down
	{
		for (int z = startTile->getPosition().z; z > endTile->getPosition().z; z--)
		{
			block += blockage(_save->getTile(Position(x, y, z)), MapData::O_FLOOR, type);
		}
	}
	else if (direction > 0) // up
	{
		for (int z = startTile->getPosition().z + 1; z <= endTile->getPosition().z; z++)
		{
			block += blockage(_save->getTile(Position(x, y, z)), MapData::O_FLOOR, type);
		}
	}

	return block;
}

/**
 * The amount of power that is blocked going from one tile to another on the same level.
 * @param startTile
 * @param endTile
 * @param affector
 * @return amount of blockage
 */
int TerrainModifier::horizontalBlockage(Tile *startTile, Tile *endTile, ItemDamageType type)
{
	static const Position oneTileNorth = Position(0, 1, 0);
	static const Position oneTileEast = Position(1, 0, 0);
	static const Position oneTileSouth = Position(0, -1, 0);
	static const Position oneTileWest = Position(-1, 0, 0);

	// safety check
	if (startTile == 0 || endTile == 0) return 0;

	int direction = vectorToDirection(endTile->getPosition() - startTile->getPosition());
	if (direction == -1) return 0;

	int block = 0;

	switch(direction)
	{
	case 0:	// north
		block = blockage(startTile, MapData::O_NORTHWALL, type);
		break;
	case 1: // north east
		block = (blockage(startTile,MapData::O_NORTHWALL, type) + blockage(endTile,MapData::O_WESTWALL, type))/2
			+ (blockage(_save->getTile(startTile->getPosition() + oneTileEast),MapData::O_WESTWALL, type)
			+ blockage(_save->getTile(startTile->getPosition() + oneTileEast),MapData::O_NORTHWALL, type))/2;
		block += (blockage(_save->getTile(startTile->getPosition() + oneTileNorth),MapData::O_OBJECT, type) +
			      blockage(_save->getTile(startTile->getPosition() + oneTileEast),MapData::O_OBJECT, type))/2;
		break;
	case 2: // east
		block = blockage(endTile,MapData::O_WESTWALL, type);
		break;
	case 3: // south east
		block = (blockage(endTile,MapData::O_WESTWALL, type) + blockage(endTile,MapData::O_NORTHWALL, type))/2
			+ (blockage(_save->getTile(startTile->getPosition() + oneTileEast),MapData::O_WESTWALL, type)
			+ blockage(_save->getTile(startTile->getPosition() + oneTileSouth),MapData::O_NORTHWALL, type))/2;
		block += (blockage(_save->getTile(startTile->getPosition() + oneTileSouth),MapData::O_OBJECT, type) +
			      blockage(_save->getTile(startTile->getPosition() + oneTileEast),MapData::O_OBJECT, type))/2;
		break;
	case 4: // south
		block = blockage(endTile,MapData::O_NORTHWALL, type);
		break;
	case 5: // south west
		block = (blockage(endTile,MapData::O_NORTHWALL, type) + blockage(startTile,MapData::O_WESTWALL, type))/2
			+ (blockage(_save->getTile(startTile->getPosition() + oneTileSouth),MapData::O_WESTWALL, type)
			+ blockage(_save->getTile(startTile->getPosition() + oneTileSouth),MapData::O_NORTHWALL, type))/2;
		block += (blockage(_save->getTile(startTile->getPosition() + oneTileSouth),MapData::O_OBJECT, type) +
			      blockage(_save->getTile(startTile->getPosition() + oneTileWest),MapData::O_OBJECT, type))/2;
		break;
	case 6: // west
		block = blockage(startTile,MapData::O_WESTWALL, type);
		break;
	case 7: // north west
		block = (blockage(startTile,MapData::O_WESTWALL, type) + blockage(startTile,MapData::O_NORTHWALL, type))/2
			+ (blockage(_save->getTile(startTile->getPosition() + oneTileNorth),MapData::O_WESTWALL, type)
			+ blockage(_save->getTile(startTile->getPosition() + oneTileWest),MapData::O_NORTHWALL, type))/2;
		block += (blockage(_save->getTile(startTile->getPosition() + oneTileNorth),MapData::O_OBJECT, type) +
			      blockage(_save->getTile(startTile->getPosition() + oneTileWest),MapData::O_OBJECT, type))/2;
		break;
	}

	block += blockage(startTile, MapData::O_OBJECT, type);
	return block;
}

/*
 * The amount this certain wall or floor-part of the tile blocks.
 * @param tile
 * @param part
 * @param affector
 * @return amount of blockage
 */
int TerrainModifier::blockage(Tile *tile, const int part, ItemDamageType type)
{
	int blockage = 0;

	if (tile == 0) return 0; // probably outside the map here

	if (part == MapData::O_FLOOR && tile->getMapData(MapData::O_FLOOR))
	{
		// blockage modifiers of floors in ufo only counted for horizontal stuff, so this is kind of an experiment
		if (type == DT_HE)
			blockage += 15;
		else
			blockage += 255;
	}
	else
	{
		if (tile->getMapData(part))
			blockage += tile->getMapData(part)->getBlock(type);

		// open ufo doors are actually still closed behind the scenes
		// so a special trick is needed to see if they are open, if they are, they obviously don't block anything
		if (tile->isUfoDoorOpen(part))
			blockage = 0;
	}

	return blockage;
}



/*
 * Converts direction to a vector. Direction starts north = 0 and goes clockwise.
 * @param vector pointer to a position (which acts as a vector)
 * @return direction
 */
int TerrainModifier::vectorToDirection(const Position &vector)
{
	int x[8] = {0, 1, 1, 1, 0, -1, -1, -1};
	int y[8] = {1, 1, 0, -1, -1, -1, 0, 1};
	for (int i=0;i<9;++i)
	{
		if (x[i] == vector.x && y[i] == vector.y)
			return i;
	}
	return -1;
}

/**
 * Soldier opens a door (if any) by rightclick, or by walking through it. The unit has to face in the right direction.
 * @param unit
 * @return -1 there is no door, you can walk through.
 *          0 normal door opened, make a squeeky sound and you can walk through.
 *          1 ufo door is starting to open, make a woosh sound, don't walk through.
 *          3 ufo door is still opening, don't walk through it yet. (have patience, futuristic technology...)
 */
int TerrainModifier::unitOpensDoor(BattleUnit *unit)
{
	int door = -1;
	Tile *tile = unit->getTile();
	switch(unit->getDirection())
	{
	case 0:	// north
		door = tile->openDoor(MapData::O_NORTHWALL);
		if (door == 1)
		{
			// check for adjacent door(s)
			tile = _save->getTile(unit->getPosition() + Position(1, 0, 0));
			if (tile) tile->openDoor(MapData::O_NORTHWALL);
			tile = _save->getTile(unit->getPosition() + Position(-1, 0, 0));
			if (tile) tile->openDoor(MapData::O_NORTHWALL);
		}
		break;
	case 2: // east
		tile = _save->getTile(tile->getPosition() + Position(1, 0, 0));
		if (tile) door = tile->openDoor(MapData::O_WESTWALL);
		if (door == 1)
		{
			// check for adjacent door(s)
			tile = _save->getTile(unit->getPosition() + Position(1, -1, 0));
			if (tile) tile->openDoor(MapData::O_WESTWALL);
			tile = _save->getTile(unit->getPosition() + Position(1, 1, 0));
			if (tile) tile->openDoor(MapData::O_WESTWALL);
		}
		break;
	case 4: // south
		tile = _save->getTile(tile->getPosition() + Position(0, -1, 0));
		if (tile) door = tile->openDoor(MapData::O_NORTHWALL);
		if (door == 1)
		{
			// check for adjacent door(s)
			tile = _save->getTile(unit->getPosition() + Position(1, -1, 0));
			if (tile) tile->openDoor(MapData::O_NORTHWALL);
			tile = _save->getTile(unit->getPosition() + Position(-1, -1, 0));
			if (tile) tile->openDoor(MapData::O_NORTHWALL);
		}
		break;
	case 6: // west
		door = tile->openDoor(MapData::O_WESTWALL);
		if (door == 1)
		{
			// check for adjacent door(s)
			tile = _save->getTile(unit->getPosition() + Position(0, -1, 0));
			if (tile) tile->openDoor(MapData::O_WESTWALL);
			tile = _save->getTile(unit->getPosition() + Position(0, 1, 0));
			if (tile) tile->openDoor(MapData::O_WESTWALL);
		}
		break;
	}


	if (door == 0 || door == 1)
	{
		_save->getTerrainModifier()->calculateFOV(tile->getPosition());
	}

	return door;
}

/**
 * calculateLine. Using bresenham algorithm in 3D.
 * @param origin
 * @param target
 * @param storeTrajectory true will store the whole trajectory - otherwise it just stores the last position.
 * @return the objectnumber(0-3) or unit(4) or out of map (5) or -1(hit nothing)
 */
int TerrainModifier::calculateLine(const Position& origin, const Position& target, bool storeTrajectory, std::vector<Position> *trajectory, BattleUnit *excludeUnit)
{
	int x, x0, x1, delta_x, step_x;
    int y, y0, y1, delta_y, step_y;
    int z, z0, z1, delta_z, step_z;
    int swap_xy, swap_xz;
    int drift_xy, drift_xz;
    int cx, cy, cz;

    //start and end points
    x0 = origin.x;     x1 = target.x;
    y0 = origin.y;     y1 = target.y;
    z0 = origin.z;     z1 = target.z;

    //'steep' xy Line, make longest delta x plane
    swap_xy = abs(y1 - y0) > abs(x1 - x0);
    if (swap_xy)
	{
        std::swap(x0, y0);
        std::swap(x1, y1);
	}

    //do same for xz
    swap_xz = abs(z1 - z0) > abs(x1 - x0);
    if (swap_xz)
	{
        std::swap(x0, z0);
		std::swap(x1, z1);
	}

    //delta is Length in each plane
    delta_x = abs(x1 - x0);
    delta_y = abs(y1 - y0);
    delta_z = abs(z1 - z0);

    //drift controls when to step in 'shallow' planes
    //starting value keeps Line centred
    drift_xy  = (delta_x / 2);
    drift_xz  = (delta_x / 2);

    //direction of line
	step_x = 1;  if (x0 > x1) {  step_x = -1; }
	step_y = 1;  if (y0 > y1) {  step_y = -1; }
	step_z = 1;  if (z0 > z1) {  step_z = -1; }

    //starting point
    y = y0;
    z = z0;

    //step through longest delta (which we have swapped to x)
    for (x = x0; x != x1; x += step_x)
	{
        //copy position
        cx = x;    cy = y;    cz = z;

        //unswap (in reverse)
        if (swap_xz) std::swap(cx, cz);
        if (swap_xy) std::swap(cx, cy);

		if (storeTrajectory)
		{
			trajectory->push_back(Position(cx, cy, cz));
		}
        //passes through this point?
		int result = voxelCheck(Position(cx, cy, cz), excludeUnit);
		if (result != -1)
		{
			if (!storeTrajectory && trajectory != 0)
			{ // store the position of impact
				trajectory->push_back(Position(cx, cy, cz));
			}
			return result;
		}

        //update progress in other planes
        drift_xy = drift_xy - delta_y;
        drift_xz = drift_xz - delta_z;

        //step in y plane
        if (drift_xy < 0)
		{
            y = y + step_y;
            drift_xy = drift_xy + delta_x;
		}

        //same in z
        if (drift_xz < 0)
		{
            z = z + step_z;
            drift_xz = drift_xz + delta_x;
		}
	}

	return -1;
}

/**
 * Calculate a parabola trajectory, used for throwing items.
 * @param origin in voxelspace
 * @param target in voxelspace
 * @param storeTrajectory true will store the whole trajectory - otherwise it just stores the last position.
 * @param excludeUnit makes sure the trajectory does not hit the shooter itself
 * @param curvature how high the parabola goes: 1.0 is almost straight throw, 3.0 is a very high throw, to throw over a fence for example
 * @param accuracy is the deviation of the angles it should take into account. 1.0 is perfection.
 * @return the objectnumber(0-3) or unit(4) or out of map (5) or -1(hit nothing)
 */
int TerrainModifier::calculateParabola(const Position& origin, const Position& target, bool storeTrajectory, std::vector<Position> *trajectory, BattleUnit *excludeUnit, double curvature, double accuracy)
{
    double ro = sqrt((double)((target.x - origin.x) * (target.x - origin.x) + (target.y - origin.y) * (target.y - origin.y) + (target.z - origin.z) * (target.z - origin.z)));

    double fi = acos((double)(target.z - origin.z) / ro);
    double te = atan2((double)(target.y - origin.y), (double)(target.x - origin.x));

	fi *= accuracy;
	te *= accuracy;

    double zA = sqrt(ro)*curvature;
    double zK = 4.0 * zA / ro / ro;

    int x = origin.x;
	int y = origin.y;
	int z = origin.z;
    int i = 8;

    while (z > 0) {
        x = (int)((double)origin.x + (double)i * cos(te) * sin(fi));
        y = (int)((double)origin.y + (double)i * sin(te) * sin(fi));
        z = (int)((double)origin.z + (double)i * cos(fi) - zK * ((double)i - ro / 2.0) * ((double)i - ro / 2.0) + zA);
		if (storeTrajectory)
		{
			trajectory->push_back(Position(x, y, z));
		}
        //passes through this point?
		int result = voxelCheck(Position(x, y, z), excludeUnit);
		if (result != -1)
		{
			if (!storeTrajectory && trajectory != 0)
			{ // store the position of impact
				trajectory->push_back(Position(x, y, z));
			}
			return result;
		}
        ++i;
    }
	return -1;
}

/**
 * check if we hit a voxel.
 * @return the objectnumber(0-3) or unit(4) or out of map (5) or -1(hit nothing)
 */
int TerrainModifier::voxelCheck(const Position& voxel, BattleUnit *excludeUnit)
{

	Tile *tile = _save->getTile(Position(voxel.x/16, voxel.y/16, voxel.z/24));
	// check if we are not out of the map
	if (tile == 0)
	{
		return 5;
	}

	BattleUnit *unit = tile->getUnit();
	if (unit != 0 && unit != excludeUnit)
	{
		if ((voxel.z%24) < (unit->isKneeled()?unit->getUnit()->getKneelHeight():unit->getUnit()->getStandHeight()))
		{
			int x = 15 - voxel.x%16;
			int y = 15 - voxel.y%16;
			int idx = (unit->getUnit()->gotLoftemps() * 16) + y;
			if ((_voxelData->at(idx) & (1 << x))==(1 << x))
			{
				return 4;
			}
		}
	}

	for (int i=0; i< 4; ++i)
	{
		MapData *mp = tile->getMapData(i);
		if (tile->isUfoDoorOpen(i))
			continue;
		if (mp != 0)
		{
			int x = 15 - voxel.x%16;
			int y = 15 - voxel.y%16;
			int idx = (mp->getLoftID((voxel.z%24)/2)*16) + y;
			if ((_voxelData->at(idx) & (1 << x))==(1 << x))
			{
				return i;
			}
		}
	}
	return -1;
}

/**
 * Add item & affect with gravity.
 * @param position
 * @param item
 */
void TerrainModifier::spawnItem(const Position &position, BattleItem *item)
{
	Position p = position;

	// don't spawn anything outside of bounds
	if (_save->getTile(p) == 0)
		return;

	while (_save->getTile(p)->getMapData(MapData::O_FLOOR) == 0 && p.z > 0)
	{
		p.z--;
	}

	_save->getTile(p)->addItem(item);
}

/**
 * Close ufo doors.
 * @return whether doors are closed.
 */
int TerrainModifier::closeUfoDoors()
{
	int doorsclosed = 0;

	// prepare a list of tiles on fire/smoke & close any ufo doors
	for (int i = 0; i < _save->getWidth() * _save->getLength() * _save->getHeight(); ++i)
	{
		doorsclosed += _save->getTiles()[i]->closeUfoDoor();
	}

	return doorsclosed;
}

/**
 * New turn preparations. Like fire and smoke spreading.
 */
void TerrainModifier::prepareNewTurn()
{
	std::vector<Tile*> tilesOnFire;
	std::vector<Tile*> tilesOnSmoke;

	// prepare a list of tiles on fire/smoke
	for (int i = 0; i < _save->getWidth() * _save->getLength() * _save->getHeight(); ++i)
	{
		if (_save->getTiles()[i]->getFire() > 0)
		{
			tilesOnFire.push_back(_save->getTiles()[i]);
		}
		if (_save->getTiles()[i]->getSmoke() > 0)
		{
			tilesOnSmoke.push_back(_save->getTiles()[i]);
		}
	}

	// smoke spreads in 1 random direction, but the direction is same for all smoke
	int spreadX = RNG::generate(-1, +1);
	int spreadY = RNG::generate(-1, +1);
	for (std::vector<Tile*>::iterator i = tilesOnSmoke.begin(); i != tilesOnSmoke.end(); ++i)
	{
		int x = (*i)->getPosition().x;
		int y = (*i)->getPosition().y;
		int z = (*i)->getPosition().z;

		if ((*i)->getUnit())
		{
			// units in smoke suffer stun
			(*i)->getUnit()->stun(((*i)->getSmoke()/5)+1);
		}

		Tile *t = _save->getTile(Position(x+spreadX, y+spreadY, z));
		if (t && !t->getSmoke() && horizontalBlockage((*i), t, DT_SMOKE) == 0)
		{
			t->addSmoke((*i)->getSmoke()/2);
		}
		Tile *t2 = _save->getTile(Position(x+spreadX+spreadX, y+spreadY+spreadY, z));
		if (t && t2 && !t2->getSmoke() && horizontalBlockage(t, t2, DT_SMOKE) == 0)
		{
			t2->addSmoke((*i)->getSmoke()/4);
		}

		// smoke also spreads upwards
		t = _save->getTile(Position(x, y, z+1));
		if (t && !t->getSmoke() && verticalBlockage((*i), t, DT_SMOKE) == 0)
		{
			t->addSmoke((*i)->getSmoke()/2);
		}

		(*i)->prepareNewTurn();
	}

	for (std::vector<Tile*>::iterator i = tilesOnFire.begin(); i != tilesOnFire.end(); ++i)
	{
		if ((*i)->getUnit())
		{
			// units on a flaming tile suffer damage
			(*i)->getUnit()->damage(Position(0,0,0), RNG::generate(1,12));
			// units on a flaming tile can catch fire 33% chance
			if (RNG::generate(0,2) == 1)
			{
				(*i)->getUnit()->setFire(RNG::generate(1,5));
			}
		}

		int z = (*i)->getPosition().z;
		for (int x = (*i)->getPosition().x-1; x <= (*i)->getPosition().x+1; ++x)
		{
			for (int y = (*i)->getPosition().y-1; y <= (*i)->getPosition().y+1; ++y)
			{
				Tile *t = _save->getTile(Position(x, y, z));
				if (t && t->getFire() == 0)
				{
					// check adjacent tiles - if they have a flammability of < 255, there is a chance...
					if (horizontalBlockage((*i), t, DT_IN) == 0)
					{
						int flam = t->getFlammability();
						if (flam < 255)
						{
							double base = RNG::boxMuller(0,126);
							if (base < 0) base *= -1;

							if (flam < base)
							{
								if (RNG::generate(0, flam) < 2)
								{
									t->ignite();
								}
							}
						}
					}
				}
			}
		}
		(*i)->prepareNewTurn();
	}

	if (!tilesOnFire.empty())
	{
		calculateTerrainLighting(); // fires could have been stopped
	}

}

}
