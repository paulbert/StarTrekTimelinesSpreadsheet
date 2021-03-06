#include "VoyageCalculator.h"
#include <cmath>
#include <algorithm>
#include <unordered_map>
#include <map>

#ifdef max
#undef max // cstdlib included in json.hpp
#endif

using json = nlohmann::json;

namespace VoyageTools
{
#ifdef DEBUG
Log log(true /*enabled*/);
#else
Log log(true /*enabled*/);
#endif

constexpr unsigned int ANTIMATTER_FOR_SKILL_MATCH = 25;
constexpr size_t MIN_SCAN_DEPTH = 2;
constexpr size_t MAX_SCAN_DEPTH = 10; // sanity

// Various fields used in the duration estimation code
constexpr unsigned int ticksPerCycle = 28;
constexpr unsigned int secondsPerTick = 20;
constexpr unsigned int cycleSeconds = ticksPerCycle * secondsPerTick;
constexpr float cyclesPerHour = 60 * 60 / (float)cycleSeconds;
constexpr unsigned int hazPerCycle = 6;
constexpr float activityPerCycle = 18;
constexpr float dilemmasPerHour = 0.5f;
constexpr float hazPerHour = hazPerCycle * cyclesPerHour - dilemmasPerHour;
constexpr unsigned int hazSkillPerHour = 1250;
constexpr unsigned int hazAmPass = 5;
constexpr unsigned int hazAmFail = 30;
constexpr float activityAmPerHour = activityPerCycle * cyclesPerHour;
constexpr unsigned int minPerHour = 60;
constexpr float psChance = 0.35f;
constexpr float ssChance = 0.25f;
constexpr float osChance = 0.1f;
constexpr unsigned int dilPerMin = 5;

unsigned int VoyageCalculator::computeScore(const Crew& crew, size_t skill, size_t trait) const noexcept
{
	if (crew.skills[skill] == 0)
		return 0;

	unsigned int score = 0;
	for (size_t iSkill = 0; iSkill < SKILL_COUNT; ++iSkill)
	{
		unsigned int skillScore = crew.skills[iSkill];
		if (iSkill == primarySkill)
		{
			skillScore = lround(skillScore * config_skillPrimaryMultiplier);
		}
		else if (iSkill == secondarySkill)
		{
			skillScore = lround(skillScore * config_skillSecondaryMultiplier);
		}
		else if (iSkill == skill)
		{
			skillScore = lround(skillScore * config_skillMatchingMultiplier);
		}

		score += skillScore;
	};

	if (crew.traits.find(trait) != crew.traits.end())
	{
		score += config_traitScoreBoost;
	}

	return score;
}

VoyageCalculator::VoyageCalculator(const char* jsonInput) noexcept :
	j(json::parse(jsonInput)), shipAntiMatter(j["shipAM"]),
		config_skillPrimaryMultiplier(j["skillPrimaryMultiplier"]),
		config_skillSecondaryMultiplier(j["skillSecondaryMultiplier"]),
		config_skillMatchingMultiplier(j["skillMatchingMultiplier"]),
		config_traitScoreBoost(j["traitScoreBoost"]),
		config_includeAwayCrew(j["includeAwayCrew"]),
		config_includeFrozenCrew(j["includeFrozenCrew"])
{
	std::map<std::string, size_t> skillMap;
	skillMap.insert({"command_skill",0});
	skillMap.insert({"science_skill",1});
	skillMap.insert({"security_skill",2});
	skillMap.insert({"engineering_skill",3});
	skillMap.insert({"diplomacy_skill",4});
	skillMap.insert({"medicine_skill",5});

	primarySkillName = j["voyage_skills"]["primary_skill"];
	secondarySkillName = j["voyage_skills"]["secondary_skill"];

	primarySkill = skillMap[primarySkillName];
	secondarySkill = skillMap[secondarySkillName];

	assert(SLOT_COUNT == j["voyage_crew_slots"].size());

	size_t traitId = 0;
	std::unordered_map<std::string, size_t> traitMap;

	auto fGetTrait = [&](const std::string &trait) {
		auto iTrait = traitMap.find(trait);
		if (iTrait == traitMap.end())
		{
			iTrait = traitMap.insert({trait, traitId++}).first;
		}
		return iTrait->second;
	};

	for (const auto &crew : j["crew"])
	{
		if (!config_includeFrozenCrew && crew["frozen"] != 0)
			continue;

		if (!config_includeAwayCrew && crew["active_id"] != 0)
			continue;

		Crew c;
		c.id = crew["id"];
		c.name = crew["name"];
		for (const auto &skill : skillMap)
		{
			c.skillMaxProfs[skill.second] = crew[skill.first]["max"].get<int16_t>();
			c.skillMinProfs[skill.second] = crew[skill.first]["min"].get<int16_t>();
			c.skills[skill.second] = crew[skill.first]["core"].get<int16_t>()
				+ (c.skillMaxProfs[skill.second] + c.skillMinProfs[skill.second]) / 2;
		}

		for (const std::string &trait : crew["traits"])
		{
			c.traits.emplace(fGetTrait(trait));
		}

		log << c.name << " " << c.skills[0] << " " << c.skills[1] << " " << c.skills[2] << " "
			<< c.skills[3] << " " << c.skills[4] << " " << c.skills[5] << " " << /*c.traits <<*/ std::endl;

		roster.emplace_back(std::move(c));
	}

	for (size_t iSlot = 0; iSlot < SLOT_COUNT; iSlot++)
	{
		slotNames[iSlot] = j["voyage_crew_slots"][iSlot]["name"].get<std::string>();
		slotSkillNames[iSlot] = j["voyage_crew_slots"][iSlot]["skill"].get<std::string>().c_str();
		slotSkills[iSlot] = skillMap[slotSkillNames[iSlot]];
		slotTraits[iSlot] = fGetTrait(j["voyage_crew_slots"][iSlot]["trait"].get<std::string>());
	}

	log << "encountered " << traitId << " traits" << std::endl;

	sortedRoster.setSearchDepth(j["search_depth"]);

	for (size_t iSlot = 0; iSlot < SLOT_COUNT; iSlot++)
	{
		auto &slotRoster = sortedRoster.slotRosters[iSlot];
		slotRoster.resize(roster.size());
		for (size_t iCrew = 0; iCrew < roster.size(); ++iCrew)
		{
			slotRoster[iCrew] = roster[iCrew];
			slotRoster[iCrew].original = &roster[iCrew];

			slotRoster[iCrew].score = computeScore(slotRoster[iCrew], slotSkills[iSlot], slotTraits[iSlot]);
		}
		std::sort(slotRoster.begin(), slotRoster.end(),
			[&](const Crew &left, const Crew &right) {
			return (left.score > right.score);
		});

		this->slotRoster[iSlot] = &sortedRoster.slotRosters[iSlot];
	}
}

void VoyageCalculator::calculate() noexcept
{
	// find the nth highest crew score
	std::vector<unsigned int> slotCrewScores;
	for (size_t iSlot = 0; iSlot < SLOT_COUNT; ++iSlot)
	{
		const std::vector<Crew> *slot = slotRoster[iSlot];
		for (const Crew &crew : *slot)
		{
			slotCrewScores.emplace_back(crew.score);
		}
	}

	std::sort(slotCrewScores.begin(), slotCrewScores.end(), std::greater<unsigned int>());

	unsigned int minScore = slotCrewScores[std::min(slotCrewScores.size() - 1, sortedRoster.depth * SLOT_COUNT)];
	size_t minDepth = MIN_SCAN_DEPTH; // ?

	// find the deepest slot
	size_t deepSlot = 0;
	size_t maxDepth = 0;
	for (size_t iSlot = 0; iSlot < SLOT_COUNT; ++iSlot)
	{
		log << slotSkillNames[iSlot] << std::endl;
		size_t iCrew;
		for (iCrew = 0; iCrew < slotRoster[iSlot]->size(); ++iCrew)
		{
			const auto &crew = slotRoster[iSlot]->at(iCrew);
			if (iCrew >= minDepth && crew.score < minScore)
			{
				break;
			}
			log << "  " << crew.score << " - " << crew.name  << std::endl;
		}
		log << std::endl;

		if (iCrew > maxDepth) {
			deepSlot = iSlot;
			maxDepth = iCrew;
		}
	}

	// initialize depth vectors
	considered.resize(maxDepth);
	for (const Crew &crew : roster) {
		crew.considered.resize(maxDepth, false);
	}

	log << "minScore " << minScore << std::endl;
	log << "primary " << primarySkillName << "(" << primarySkill << ")" << std::endl;
	log << "secondary " << secondarySkillName << "(" << secondarySkill << ")" << std::endl;
	
	{ Timer voyageCalcTime{"actual calc"};
		for (size_t iMinDepth = minDepth; iMinDepth < MAX_SCAN_DEPTH; ++iMinDepth)
		{
			log << "depth " << iMinDepth << std::endl;
			fillSlot(0, minScore, iMinDepth, deepSlot);
			threadPool.joinAll();
			if (bestscore > 0)
				break;
		}
	}
}

void VoyageCalculator::fillSlot(size_t iSlot, unsigned int minScore, size_t minDepth, size_t seedSlot, size_t thread) noexcept
{
	size_t slot;
	if (iSlot == 0) {
		slot = seedSlot;
	} else if (iSlot == seedSlot) {
		slot = 0;
	} else {
		slot = iSlot;
	}

	for (size_t iCrew = 0; iCrew < slotRoster[slot]->size(); ++iCrew)
	{
		const auto &crew = slotRoster[slot]->at(iCrew);
		if (iCrew >= minDepth && minScore > crew.score)
		{
			break;
		}

		if (slot == seedSlot) {
			thread = iCrew;
		} else 
		if (crew.original->considered[thread])
			continue;

		considered[thread][slot] = &crew;
		crew.original->considered[thread] = true;

		if (slot < SLOT_COUNT - 1)
		{
			auto fRecurse = [=]{fillSlot(iSlot + 1, minScore, minDepth, seedSlot, thread);};
			if (slot == seedSlot) {
				threadPool.add(fRecurse);
			} else {
				fRecurse();
			}
		}
		else
		{
			auto crewToConsider = considered[thread];
			// we have a complete crew complement, compute score
			float score = calculateDuration(crewToConsider);

			if (score > bestscore)
			{
				std::lock_guard<std::mutex> guard(calcMutex);
				if (score > bestscore) { // check again within the lock to resolve race condition
					log << "new best found: " << score << std::endl;
					// sanity
					for (size_t i = 0; i < crewToConsider.size(); ++i) {
						for (size_t j = i+1; j < crewToConsider.size(); ++j) {
							if (crewToConsider[i]->original == crewToConsider[j]->original) {
								log << "ERROR - DUPE CREW IN RESULT" << std::endl;
							}
						}
					}
					bestconsidered = crewToConsider;
					bestscore = score;
					progressUpdate(bestconsidered, bestscore);
					calculateDuration(crewToConsider, true); // debug
				}
			}
		}

		if (slot != seedSlot)
			crew.original->considered[thread] = false;
	}
}

float VoyageCalculator::calculateDuration(const std::array<const Crew *, SLOT_COUNT> &complement, bool debug) noexcept
{
	unsigned int shipAM = shipAntiMatter;
	Crew totals;
	totals.skills.fill(0);

	std::array<unsigned int, SKILL_COUNT> totalProfRange;
	for (size_t iSkill = 0; iSkill < SKILL_COUNT; ++iSkill)
	{
		totalProfRange[iSkill] = 0;
	}
	unsigned int totalSkill = 0;

	for (size_t iSlot = 0; iSlot < SLOT_COUNT; ++iSlot)
	{
		const auto &crew = complement[iSlot];

		// NOTE: this is not how the game client displays totals
		//	the game client seems to add all profs first, then divide by 2,
		//	which is slightly more precise.
		for (size_t iSkill = 0; iSkill < SKILL_COUNT; ++iSkill)
		{
			totals.skills[iSkill] += crew->skills[iSkill];
			totalProfRange[iSkill] += crew->skillMaxProfs[iSkill] - crew->skillMinProfs[iSkill];
		}

		if (crew->traits.find(slotTraits[iSlot]) != crew->traits.end())
		{
			shipAM += ANTIMATTER_FOR_SKILL_MATCH;
		}
	}

	for (size_t iSkill = 0; iSkill < SKILL_COUNT; ++iSkill)
	{
		totalSkill += totals.skills[iSkill];
	}

	if (debug)
	{
		log << shipAM << " "
			<< totals.skills[0] << " " << totals.skills[1] << " " << totals.skills[2] << " "
			<< totals.skills[3] << " " << totals.skills[4] << " " << totals.skills[5] << std::endl;
	}

	unsigned int PrimarySkill = totals.skills[primarySkill];
	unsigned int SecondarySkill = totals.skills[secondarySkill];
	unsigned int MaxSkill = 0;

	std::array<float, SKILL_COUNT> hazSkillVariance;
	for (size_t iSkill = 0; iSkill < SKILL_COUNT; ++iSkill)
	{
		hazSkillVariance[iSkill] = ((float)totalProfRange[iSkill]) / 2 / totals.skills[iSkill];
		if (totals.skills[iSkill] > MaxSkill)
			MaxSkill = totals.skills[iSkill];
	}

	// Code translated from Chewable C++'s JS implementation from https://codepen.io/somnivore/pen/Nabyzw
	// TODO: make this prettier

	//let maxExtends = 100000
	unsigned int maxExtends = 0; // we only care about the first one atm

	if (debug)
	{
		log << "primary skill prof variance: " << hazSkillVariance[primarySkill] << std::endl;
	}

	unsigned int elapsedHours = 0; // TODO: deal with this later
	unsigned int elapsedHazSkill = elapsedHours * hazSkillPerHour;

	MaxSkill = std::max((unsigned int)0, MaxSkill - elapsedHazSkill);
	float endVoySkill = MaxSkill * (1 + hazSkillVariance[primarySkill]);

	const std::array<unsigned int, SKILL_COUNT> &skills = totals.skills;
	std::array<float, 6> skillChances;
	skillChances.fill(osChance);

	for (size_t iSkill = 0; iSkill < skills.size(); iSkill++)
	{
		if (iSkill == primarySkill)
		{
			skillChances[iSkill] = psChance;
			if (debug)
			{
				log << "pri: " << skills[iSkill] << std::endl;
			}
		}
		else if (iSkill == secondarySkill)
		{
			skillChances[iSkill] = ssChance;
			if (debug)
			{
				log << "sec: " << skills[iSkill] << std::endl;
			}
		}
	}

	float totalRefillCost = 0;
	float voyTime = 0;
	for (size_t extend = 0; extend <= maxExtends; extend++)
	{
		// converging loop - refine calculation based on voyage time every iteration
		unsigned int tries = 0;
		for (;;)
		{
			tries++;
			if (tries == 100)
			{
				log << "something went wrong!" << std::endl;
				//console.error("Something went wrong! Check your inputs.")
				break;
			}

			//test.text += Math.floor(endVoySkill) + " "
			float am = (float)(shipAM + shipAM * extend);
			for (size_t iSkill = 0; iSkill < SKILL_COUNT; iSkill++)
			{
				unsigned int skill = skills[iSkill];
				skill = std::max((unsigned int)0, skill - elapsedHazSkill);
				float chance = skillChances[iSkill];

				// skill amount for 100% pass
				float passSkill = std::min(endVoySkill, skill*(1 - hazSkillVariance[iSkill]));

				// skill amount for RNG pass
				// (compute passing proportion of triangular RNG area - integral of x)
				float skillRngRange = skill * hazSkillVariance[iSkill] * 2;
				float lostRngProportion = 0;
				if (skillRngRange > 0)
				{ // avoid division by 0
					lostRngProportion = std::max(0.0f, std::min(1.0f, (skill*(1 + hazSkillVariance[iSkill]) - endVoySkill) / skillRngRange));
				}
				float skillPassRngProportion = 1 - lostRngProportion * lostRngProportion;
				passSkill += skillRngRange * skillPassRngProportion / 2;

				// am gained for passing hazards
				am += passSkill * chance / hazSkillPerHour * hazPerHour * hazAmPass;

				// skill amount for 100% hazard fail
				float failSkill = std::max(0.0f, endVoySkill - skill * (1 + hazSkillVariance[iSkill]));
				// skill amount for RNG fail
				float skillFailRngProportion = (1 - lostRngProportion)*(1 - lostRngProportion);
				failSkill += skillRngRange * skillFailRngProportion / 2;

				// am lost for failing hazards
				am -= failSkill * chance / hazSkillPerHour * hazPerHour * hazAmFail;
			}

			float amLeft = am - endVoySkill / hazSkillPerHour * activityAmPerHour;
			float timeLeft = amLeft / (hazPerHour*hazAmFail + activityAmPerHour);

			voyTime = endVoySkill / hazSkillPerHour + timeLeft + elapsedHours;

			if (std::abs(timeLeft) > 0.001f)
			{
				endVoySkill = (voyTime - elapsedHours)*hazSkillPerHour;
				continue;
			}
			else
			{
				break;
			}
		}

		// compute other results
	/*	float safeTime = voyTime*0.95f;
		float saferTime = voyTime*0.90f;
		float refillTime = shipAM / (hazPerHour*hazAmFail + activityAmPerHour);
		float refillCost = std::ceil(voyTime*60/dilPerMin);*/

		// display results
		/*if (extend < 3)
		{
			//window['result'+extend].value = timeToString(voyTime)
			//window['safeResult'+extend].value = timeToString(safeTime)
			//window['saferResult'+extend].value = timeToString(saferTime)
			if (extend > 0)
				window['refillCostResult'+extend].value = totalRefillCost
			//test.text = MaxSkill*(1+hazSkillVariance)/hazSkillPerHour
			// the threshold here is just a guess
			if (MaxSkill/hazSkillPerHour > voyTime) {
				let tp = Math.floor(voyTime*hazSkillPerHour)
				// TODO: warn somehow
				//setWarning(extend, "Your highest skill is too high by about " + Math.floor(MaxSkill - voyTime*hazSkillPerHour) + ". To maximize voyage time, redistribute more like this: " + tp + "/" + tp + "/" + tp/4 + "/" + tp/4 + "/" + tp/4 + "/" + tp/4 + ".")
			}
		}

		totalRefillCost += refillCost

		if (voyTime >= 20) {
			//test.text += "hi"
			// TODO: show 20 hr?
			//window['20hrdil'].value = totalRefillCost
			//window['20hrrefills'].value = extend
			break
		}*/

		return voyTime;
	}

	return 0;
}

} // namespace VoyageTools