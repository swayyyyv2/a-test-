#include <array>
#include <limits>
#include <memory>
#include <new>
#include <span>
#include <utility>

#include "../../core/logging/log.h"
#include "../../core/runtime/server_clock.h"
#include "../../middleware/crypto/random_bytes.h"
#include "../../state/build_data/runtime.h"
#include "../../state/investment/store_internal.h"
#include "internal.h"

namespace sunrise::server::bap {
namespace {

/** Logged for a saved row whose hash is not in the item table. */
constexpr std::uint16_t kUnresolvedItemIndex = (std::numeric_limits<std::uint16_t>::max)();

/** How a saved reward reaches the inventory; used to pick rewards this build can grant. */
enum class ItemRoute : std::uint8_t { unavailable, quest, profile, reward };

/**
 * Classifies an installed item by its quest setup and bucket. Upstream folded this into
 * prepare_item_reward; the scripted boss rewards still pick from pools by route.
 */
[[nodiscard]] ItemRoute item_route(std::uint16_t itemIndex) noexcept {
    namespace data = state::build_data;
    data::items::Definition item{};
    if (!data::find_item_definition_index(itemIndex, item)) {
        return ItemRoute::unavailable;
    }
    if (item.questInitialization.scope != data::items::QuestInitialization::Scope::none) {
        return ItemRoute::quest;
    }
    data::items::details::Definition detail{};
    data::inventory::buckets::Descriptor bucket{};
    if (!data::find_configured_item_detail(itemIndex, detail)
        || !data::find_inventory_bucket_descriptor(item.bucketId, bucket)) {
        return ItemRoute::reward;
    }
    if (bucket.arraySelector == data::inventory::buckets::ArraySelector::profile
        && detail.instancedDefinitionState
               == data::items::details::InstancedDefinitionState::stackable) {
        return ItemRoute::profile;
    }
    return bucket.arraySelector == data::inventory::buckets::ArraySelector::character
               ? ItemRoute::reward
               : ItemRoute::unavailable;
}

/** Outcome of one settlement attempt. */
enum class WorldRewardSettlement : std::uint8_t { granted, retired, kept };

/**
 * Grants and retires one earned reward in one transaction, or retires one State rules out.
 * The item resolves through its reward row, which also starts a placed quest item's quest.
 * @param request Saved reward of the selected character.
 * @return Granted, retired as ungrantable, or kept for a later attempt.
 */
WorldRewardSettlement commit_world_reward(const WorldRewardRequest& request) noexcept {
    state::investment::store::Transaction transaction;
    if (!transaction.ready()) {
        report_reward_refusal("world_settle", request.itemDefinitionIndex, "transaction");
        return WorldRewardSettlement::kept;
    }
    const std::unique_ptr<state::PendingRecordRewardGrant> grant(
        new (std::nothrow) state::PendingRecordRewardGrant);
    std::uint64_t seed = 0;
    const bool seeded = middleware::crypto::random::draw_seed(seed);
    const char* reason = seeded ? "reward_allocation" : "random_source";
    auto preparation = state::RewardPreparation::notReady;
    if (seeded && grant) {
        preparation = state::prepare_item_reward(request.itemDefinitionIndex,
                                                 static_cast<std::uint32_t>(request.quantity),
                                                 seed,
                                                 core::runtime::server_clock_seconds(),
                                                 *grant,
                                                 &reason);
    }
    const bool unresolvable = preparation == state::RewardPreparation::unresolvable;
    bool committed = preparation == state::RewardPreparation::prepared;
    if (unresolvable) {
        return retire_world_reward(transaction, request, reason) ? WorldRewardSettlement::retired
                                                                 : WorldRewardSettlement::kept;
    }
    if (committed && request.kind == WorldRewardKind::menagerieWeapon) {
        std::size_t rolledLanes = 0;
        reason = "menagerie_socket_roll";
        committed = state::roll_menagerie_reward_sockets(
            request.itemDefinitionIndex, seed, *grant, rolledLanes);
        if (committed) {
            core::log::writef(core::log::Channel::server,
                              core::log::Level::info,
                              "ev=menagerie_reward stage=socket_roll lanes=%u",
                              static_cast<unsigned>(rolledLanes));
        }
    }
    if (committed) {
        reason = "inventory_commit";
        committed = state::commit_record_reward(*grant);
    }
    // The grant and the row's deletion share the savepoint, so a failed deletion undoes the grant.
    if (committed) {
        reason = "queue_commit";
        committed = complete_world_reward(request.id) && transaction.commit();
    }
    if (!committed) {
        report_reward_refusal("world_settle", request.itemDefinitionIndex, reason);
        return WorldRewardSettlement::kept;
    }
    return WorldRewardSettlement::granted;
}

/**
 * Settles each saved reward of the selected character in turn. A reward that must wait stays saved
 * and steps aside, so later rewards still take their turn.
 * @return True when any reward was granted.
 */
bool settle_saved_world_rewards() noexcept {
    bool granted = false;
    WorldRewardRequest request;
    std::uint64_t afterId = 0;
    while (current_world_reward(request, afterId, nullptr)) {
        const WorldRewardSettlement settlement = commit_world_reward(request);
        granted = granted || settlement == WorldRewardSettlement::granted;
        if (settlement == WorldRewardSettlement::kept) {
            afterId = request.id;
        }
    }
    return granted;
}

/** Saves the reward under its stable definition hash and earning character. */
bool enqueue_world_reward(std::uint16_t definitionIndex,
                          std::int32_t quantity,
                          WorldRewardKind kind = WorldRewardKind::item) noexcept {
    state::build_data::items::Definition definition;
    if (!state::build_data::find_item_definition_index(definitionIndex, definition)
        || !state::investment::store::enqueue_reward(
            definition.definitionHash, quantity, static_cast<std::uint8_t>(kind))) {
        return false;
    }
    if (!has_active_family4_peer()) {
        settle_world_reward();
    }
    return true;
}

} // namespace

/** Logs one refused reward stage with its native index and a specific reason. */
void report_reward_refusal(const char* stage, std::uint16_t index, const char* reason) noexcept {
    core::log::writef(core::log::Channel::server,
                      core::log::Level::warn,
                      "ev=reward stage=%s index=%u result=refused reason=%s",
                      stage,
                      static_cast<unsigned>(index),
                      reason != nullptr ? reason : "unknown");
}

/** Saves one item reward before its pickup presentation is queued. */
bool arm_world_item_acquisition(std::uint16_t itemDefinitionIndex) noexcept {
    return enqueue_world_reward(itemDefinitionIndex, 1);
}

/** Saves a positive quantity of one item before its pickup presentation is queued. */
bool arm_world_profile_item_acquisition(std::uint16_t itemDefinitionIndex,
                                        std::int32_t quantity) noexcept {
    return quantity > 0
           && enqueue_world_reward(itemDefinitionIndex, quantity, WorldRewardKind::profileItem);
}

/** Each accepted clear enters the same durable acquisition queue as native world loot. */
MenagerieRewardStatus arm_menagerie_boss_reward(std::uint64_t sessionId,
                                               std::uint64_t playerKey,
                                               std::uint64_t attemptGeneration,
                                               std::uint8_t stage) noexcept {
    constexpr std::array<std::uint32_t, 23> kLeviathanWeapons{
        0x433F5A7D, // Midnight Coup
        0xC63AD712, // Inaugural Address
        0x95576728, // Ghost Primus
        0xDC0DA337, // Sins of the Past
        0xE8DF3495, // Conspirator
        0xEBB55C1D, // Mob Justice
        0xC98208A4, // Alone as a god
        0x3CAE8F97, // It Stared Back
        0xA160A265, // Zenith of Your Kind
        0xE7A3AB5A, // I Am Alive
        0x7C40A73B, // Last of the Legion
        0xFF962AE5, // The Emperor's Envy
        0x4CB14828, // Gahlran's Right Hand
        0x59318DBF, // Bane of Sorrow
        0x8B5C6795, // Calusea Noblesse
        0xE6290630, // Emperor's Courtesy
        0x90D42801, // Austringer
        0xF9CC8D28, // Beloved
        0x61E4CE23, // Fixed Odds
        0xF5D4B4B7, // The Epicurean
        0xAE018294, // Imperial Decree
        0x04B699ED, // Drang (Baroque)
        0x0A61F5E1, // CALUS Mini-Tool
    };
    constexpr std::array<std::pair<std::uint32_t, std::int32_t>, 2> kCurrency{
        std::pair{0x61ECF2B8U, 25}, // Imperials
        std::pair{0x59B8B955U, 1},  // Emperor Calus Token
    };
    if (sessionId == 0 || playerKey == 0 || attemptGeneration == 0 || stage < 1 || stage > 7) {
        return MenagerieRewardStatus::failed;
    }
    std::uint32_t randomValue = 0;
    if (!middleware::crypto::random::fill(std::as_writable_bytes(std::span(&randomValue, 1)))) {
        return MenagerieRewardStatus::failed;
    }
    std::uint32_t selectedHash = 0;
    for (std::size_t offset = 0; offset < kLeviathanWeapons.size(); ++offset) {
        const auto hash = kLeviathanWeapons[(randomValue + offset) % kLeviathanWeapons.size()];
        state::build_data::items::Definition definition{};
        if (state::build_data::find_item_definition_hash(hash, definition)
            && item_route(definition.definitionIndex) == ItemRoute::reward) {
            selectedHash = hash;
            break;
        }
    }
    if (selectedHash == 0) {
        core::log::write(core::log::Channel::server, core::log::Level::warn,
                         "ev=menagerie_reward result=refused reason=weapon_catalog");
        return MenagerieRewardStatus::failed;
    }
    std::uint32_t currencyHash = 0;
    std::int32_t currencyQuantity = 0;
    for (const auto [hash, quantity] : kCurrency) {
        state::build_data::items::Definition currency{};
        if (state::build_data::find_item_definition_hash(hash, currency)
            && item_route(currency.definitionIndex) == ItemRoute::profile) {
            currencyHash = hash;
            currencyQuantity = quantity;
            break;
        }
    }
    const auto status = state::investment::store::enqueue_mission_reward_once(
        sessionId, playerKey, attemptGeneration, stage, selectedHash, currencyHash,
        currencyQuantity);
    if (status == state::investment::store::MissionRewardStatus::failed) {
        return MenagerieRewardStatus::failed;
    }
    if (status == state::investment::store::MissionRewardStatus::alreadyQueued) {
        return MenagerieRewardStatus::alreadyQueued;
    }
    core::log::writef(core::log::Channel::server, core::log::Level::info,
                      "ev=menagerie_reward stage=%u weapon=0x%08X currency=0x%08X quantity=%d result=queued",
                      static_cast<unsigned>(stage), static_cast<unsigned>(selectedHash),
                      static_cast<unsigned>(currencyHash), currencyQuantity);
    if (!has_active_family4_peer()) {
        settle_world_reward();
    }
    return MenagerieRewardStatus::queued;
}

namespace {

/**
 * Dry-runs the grant the settle path will make, without committing anything. A row the settle
 * path would retire as unresolvable never pays, so it is skipped here and the next one is tried.
 * Deferred (for example a full bucket) still counts as grantable: the row waits and pays later.
 */
bool grantable(std::uint32_t hash, std::uint16_t index, bool rolledWeapon, bool strict) noexcept {
    const std::unique_ptr<state::PendingRecordRewardGrant> grant(
        new (std::nothrow) state::PendingRecordRewardGrant);
    std::uint64_t seed = 0;
    if (!grant
        || !middleware::crypto::random::fill(std::as_writable_bytes(std::span(&seed, 1)))) {
        return true; // Nothing learned; let the queue decide as before.
    }
    const char* reason = "unresolvable";
    const auto preparation = state::prepare_item_reward(
        index, 1, seed, core::runtime::server_clock_seconds(), *grant, &reason);
    // Strict: grantable right now. Otherwise deferred (for example a full bucket) also counts.
    bool ok = strict ? preparation == state::RewardPreparation::prepared
                     : preparation != state::RewardPreparation::unresolvable;
    if (ok && rolledWeapon && preparation == state::RewardPreparation::prepared) {
        std::size_t lanes = 0;
        if (!state::roll_menagerie_reward_sockets(index, seed, *grant, lanes)) {
            ok = false;
            reason = "socket_roll";
        }
    }
    if (!ok) {
        core::log::writef(core::log::Channel::server, core::log::Level::info,
                          "ev=reward_probe hash=0x%08X result=skipped reason=%s",
                          static_cast<unsigned>(hash), reason != nullptr ? reason : "unknown");
    }
    return ok;
}

/**
 * First hash in the list that this build installs on the reward route, from a random start.
 * A weapon must also have configured socket detail: the rolled-weapon grant needs it, and a row
 * that can never roll would stay at the head of the queue.
 */
std::uint32_t pick_installed_pass(std::span<const std::uint32_t> pool,
                                  std::uint32_t start,
                                  ItemRoute route,
                                  bool rolledWeapon,
                                  bool strict) noexcept {
    for (std::size_t offset = 0; offset < pool.size(); ++offset) {
        const auto hash = pool[(start + offset) % pool.size()];
        state::build_data::items::Definition definition{};
        if (!state::build_data::find_item_definition_hash(hash, definition)
            || item_route(definition.definitionIndex) != route) {
            continue;
        }
        if (rolledWeapon) {
            state::build_data::items::details::Definition detail{};
            if (!state::build_data::find_configured_item_detail(definition.definitionIndex, detail)
                || detail.definitionHash != hash) {
                continue;
            }
        }
        if (!grantable(hash, definition.definitionIndex, rolledWeapon, strict)) {
            continue;
        }
        return hash;
    }
    return 0;
}

/** Prefers an item that can be granted right now; falls back to one that can wait. */
std::uint32_t pick_installed(std::span<const std::uint32_t> pool,
                             std::uint32_t start,
                             ItemRoute route,
                             bool rolledWeapon = false) noexcept {
    const auto now = pick_installed_pass(pool, start, route, rolledWeapon, true);
    return now != 0 ? now : pick_installed_pass(pool, start, route, rolledWeapon, false);
}

/** Red War weapons this build is known to grant; used when a whole themed pool is refused. */
constexpr std::array<std::uint32_t, 20> kFallbackWeapons{
    0x74D68F77, 0x61FFEE96, 0xE042B104, 0x092D8A05, 0xDC0DA337, 0x71F46BCF, 0x433F5A7D,
    0x0F17792A, 0xF8D186CA, 0x7FE1600F, 0xE5BCDD0D, 0x888BCBCA, 0x5A7B7C16, 0x4CFC0D09,
    0x560331F8, 0x56FD56F8, 0x7948D9AA, 0x7DF07066, 0xB9BFD2AC, 0xD0DFED30,
};

/** A grantable weapon from the pool, or from the fallback list when none in the pool is. */
std::uint32_t pick_weapon(std::span<const std::uint32_t> pool, std::uint32_t start) noexcept {
    const auto weapon = pick_installed(pool, start, ItemRoute::reward, true);
    if (weapon != 0) {
        return weapon;
    }
    core::log::write(core::log::Channel::server, core::log::Level::warn,
                     "ev=reward_probe result=fallback reason=pool_ungrantable");
    return pick_installed(kFallbackWeapons, start, ItemRoute::reward, true);
}

/** Class byte of the selected character: 0 Titan, 1 Hunter, 2 Warlock; 0xFF when none. */
std::uint8_t selected_character_class() noexcept {
    const state::AccountState account = state::bound_account_snapshot();
    for (std::size_t index = 0; index < account.characterCount; ++index) {
        if (account.characters[index].selected) {
            return static_cast<std::uint8_t>(account.characters[index].characterClass);
        }
    }
    return 0xFF;
}

} // namespace

/**
 * Prophecy boss rush (activity 0xF74050EE). Stages 1-4 are the four rooms, 5 is the full clear.
 * Every stage queues one weapon as a rolled weapon row (kind 2, which runs the socket roll that
 * Menagerie weapons use) and one armor piece for the selected class. The full clear adds a second
 * weapon. Hashes come from this build's item name table and are re-checked against the installed
 * catalog here, so an entry the build lacks is skipped rather than queued.
 */
ProphecyRewardStatus arm_prophecy_boss_reward(std::uint64_t sessionId,
                                              std::uint64_t playerKey,
                                              std::uint64_t attemptGeneration,
                                              std::uint8_t stage) noexcept {
    // Trials of the Nine weapons, which are also the Prophecy dungeon's weapons.
    constexpr std::array<std::uint32_t, 17> kWeapons{
        0x71D1159E, // Prosecutor
        0x46C9415E, // Relentless
        0x7CDE3A31, // Adjudicator
        0x70028208, // A Sudden Death
        0x1367082A, // Darkest Before
        0x6B26D5A2, // Judgment
        0xA9E5E269, // Judgment (second row)
        0x0548FEB9, // The End
        0x6CCE80CD, // The End (second row)
        0xE5B7F045, // A Swift Verdict
        0x0A6B4FA6, // The Long Walk
        0x599F42F0, // The Last Breath
        0x7B773DA5, // A Cold Sweat
        0x95FC9D4D, // Purpose
        0xEB7191BE, // Motion to Suppress
        0xCC2DEB2A, // Motion to Compel
        0x3CB94ECF, // Motion to Vacate
    };
    // Titan: Annihilating and Crushing, with the Crushing CODA pieces.
    constexpr std::array<std::uint32_t, 20> kTitanArmor{
        0x73005222, 0x8E873D98, // Annihilating Helm
        0x6F0B5211, 0xB454F2F3, // Annihilating Guard
        0x2EC65D37, 0x37ADEC39, // Annihilating Plate
        0x846BA96B, 0xCC3F5C0D, // Annihilating Greaves
        0x73005223, 0x8E873D99, // Crushing Helm
        0x6F0B5210, 0xB454F2F2, // Crushing Guard
        0x2EC65D36, 0x37ADEC38, // Crushing Plate
        0x846BA96A, 0xCC3F5C0C, // Crushing Greaves
        0xECFE1083,             // Crushing Helm (CODA)
        0x30CB8752,             // Crushing Guard (CODA)
        0x99390E16,             // Crushing Plate (CODA)
        0x48B5F0EC,             // Crushing Greaves (CODA)
    };
    // Hunter: Flowing and Floating, the Flowing CODA pieces, and both sets' cloaks.
    constexpr std::array<std::uint32_t, 23> kHunterArmor{
        0x17D7E727, 0xBA671B2F, // Flowing Cowl
        0x386166B6, 0x9D737DAE, // Flowing Grips
        0x20AE40C2, 0x2E17E67A, // Flowing Vest
        0x32E972A8, 0x80A4E310, // Flowing Boots
        0xB74E76ED,             // Flowing Cowl (CODA)
        0xF9FC785C,             // Flowing Grips (CODA)
        0x1E48A144,             // Flowing Vest (CODA)
        0xBC125FE6,             // Flowing Boots (CODA)
        0x0FEE86EF, 0x42C2D518, // Flowing Dance
        0x17D7E726, 0xBA671B2E, // Floating Cowl
        0x386166B7, 0x9D737DAF, // Floating Grips
        0x20AE40C3, 0x2E17E67B, // Floating Vest
        0x32E972A9, 0x80A4E311, // Floating Boots
        0xE80F0CF2,             // Floating Dance
    };
    // Warlock: Channeling and Focusing, with the Channeling CODA pieces.
    constexpr std::array<std::uint32_t, 20> kWarlockArmor{
        0x28ECCB2C, 0xB0B1CB40, // Channeling Cowl
        0x1F9F0511, 0xF8FECFE5, // Channeling Wraps
        0x0457443B, 0x3529D2C7, // Channeling Robes
        0xF4645C67, 0xFC41DCE3, // Channeling Treads
        0x9B25385E,             // Channeling Cowl (CODA)
        0x53DAC58F,             // Channeling Wraps (CODA)
        0x627EA6E9,             // Channeling Robes (CODA)
        0x78935C31,             // Channeling Treads (CODA)
        0x28ECCB2D, 0xB0B1CB41, // Focusing Cowl
        0x1F9F0510, 0xF8FECFE4, // Focusing Wraps
        0x0457443A, 0x3529D2C6, // Focusing Robes
        0xF4645C66, 0xFC41DCE2, // Focusing Boots
    };
    if (sessionId == 0 || playerKey == 0 || attemptGeneration == 0 || stage < 1 || stage > 5) {
        return ProphecyRewardStatus::failed;
    }
    std::array<std::uint32_t, 4> random{};
    if (!middleware::crypto::random::fill(std::as_writable_bytes(std::span(random)))) {
        return ProphecyRewardStatus::failed;
    }
    std::array<state::investment::store::MissionRewardRow, 4> rows{};
    std::size_t rowCount = 0;
    const auto weapon = pick_weapon(kWeapons, random[0]);
    if (weapon == 0) {
        core::log::write(core::log::Channel::server, core::log::Level::warn,
                         "ev=prophecy_reward result=refused reason=weapon_catalog");
        return ProphecyRewardStatus::failed;
    }
    rows[rowCount++] = {static_cast<std::uint8_t>(WorldRewardKind::menagerieWeapon), weapon, 1};
    std::uint32_t bonus = 0;
    if (stage == 5) {
        // A different weapon for the full clear when the pool allows it.
        bonus = pick_weapon(kWeapons, random[1]);
        if (bonus == weapon) {
            bonus = pick_weapon(kWeapons, random[1] + 1U);
        }
        if (bonus != 0) {
            rows[rowCount++] = {static_cast<std::uint8_t>(WorldRewardKind::menagerieWeapon),
                                bonus, 1};
        }
    }
    std::span<const std::uint32_t> armorPool;
    switch (selected_character_class()) {
    case 0: armorPool = kTitanArmor; break;
    case 1: armorPool = kHunterArmor; break;
    case 2: armorPool = kWarlockArmor; break;
    default: break;
    }
    std::uint32_t armor = 0;
    if (!armorPool.empty()) {
        armor = pick_installed(armorPool, random[2], ItemRoute::reward);
        if (armor != 0) {
            rows[rowCount++] = {static_cast<std::uint8_t>(WorldRewardKind::item), armor, 1};
        }
    }
    // Kell Echo exclusive: "Verdict of the Nine", a Studio clone of Graviton Lance (mod
    // dev.ender.prophecy-ultra-bosses). One in kNineExoticOdds Kell clears. It is granted as a
    // plain item row so its exotic perks stay fixed; skipped when the mod is not installed.
    constexpr std::uint32_t kKellStage = 4;
    constexpr std::uint32_t kNineExoticOdds = 4;
    constexpr std::array<std::uint32_t, 1> kNineExotic{0xBDA2AEDD};
    std::uint32_t exotic = 0;
    if (stage == kKellStage && random[3] % kNineExoticOdds == 0) {
        exotic = pick_installed(kNineExotic, 0, ItemRoute::reward);
        if (exotic != 0) {
            rows[rowCount++] = {static_cast<std::uint8_t>(WorldRewardKind::item), exotic, 1};
        } else {
            core::log::write(core::log::Channel::server, core::log::Level::warn,
                             "ev=prophecy_reward stage=4 exotic=skipped reason=not_installed");
        }
    }
    const auto status = state::investment::store::enqueue_mission_rewards_once(
        sessionId, playerKey, attemptGeneration, stage, std::span(rows.data(), rowCount));
    if (status == state::investment::store::MissionRewardStatus::failed) {
        return ProphecyRewardStatus::failed;
    }
    if (status == state::investment::store::MissionRewardStatus::alreadyQueued) {
        return ProphecyRewardStatus::alreadyQueued;
    }
    core::log::writef(core::log::Channel::server, core::log::Level::info,
                      "ev=prophecy_reward stage=%u weapon=0x%08X bonus=0x%08X armor=0x%08X exotic=0x%08X result=queued",
                      static_cast<unsigned>(stage), static_cast<unsigned>(weapon),
                      static_cast<unsigned>(bonus),
                      static_cast<unsigned>(armor),
                      static_cast<unsigned>(exotic));
    if (!has_active_family4_peer()) {
        settle_world_reward();
    }
    return ProphecyRewardStatus::queued;
}

/** A full Tangled Web set for the active class, atomically queued once per Skolas clear. */
NightmareHuntRewardStatus arm_nightmare_hunt_reward(std::uint64_t sessionId,
                                                     std::uint64_t playerKey,
                                                     std::uint64_t attemptGeneration) noexcept {
    constexpr std::array<std::array<std::uint32_t, 2>, 5> kTitan{{
        {0x800C61B3, 0xB1C4003C}, // Helm
        {0x028436B5, 0x06643D42}, // Gauntlets
        {0x98BC332B, 0xF43D3826}, // Plate
        {0x6075ED97, 0x803B6DFC}, // Greaves
        {0x207E168F, 0x36C9ADC2}, // Mark
    }};
    constexpr std::array<std::array<std::uint32_t, 2>, 5> kHunter{{
        {0xB67F0E2F, 0xFE067308}, // Mask
        {0x347959B6, 0xD71F8F99}, // Grips
        {0x07CB6F7A, 0x9DDD98FF}, // Vest
        {0x7CBCD610, 0x83813C8B}, // Strides
        {0x017EDC0E, 0xF2D7424B}, // Cloak
    }};
    constexpr std::array<std::array<std::uint32_t, 2>, 5> kWarlock{{
        {0x20FBCD62, 0x632FEC61}, // Hood
        {0x95218F78, 0xF7252A4B}, // Gloves
        {0x3DA3DE90, 0x9F6A0005}, // Robes
        {0x20061FB2, 0x05184C55}, // Boots
        {0x4F4E7658, 0xFDB18ECD}, // Bond
    }};
    if (sessionId == 0 || playerKey == 0 || attemptGeneration == 0) {
        return NightmareHuntRewardStatus::failed;
    }
    const std::array<std::array<std::uint32_t, 2>, 5>* armor = nullptr;
    switch (selected_character_class()) {
    case 0: armor = &kTitan; break;
    case 1: armor = &kHunter; break;
    case 2: armor = &kWarlock; break;
    default: return NightmareHuntRewardStatus::failed;
    }
    std::array<state::investment::store::MissionRewardRow, 5> rows{};
    for (std::size_t slot = 0; slot < armor->size(); ++slot) {
        const std::uint32_t hash = pick_installed((*armor)[slot], 0,
                                                  ItemRoute::reward);
        if (hash == 0) {
            core::log::writef(core::log::Channel::server, core::log::Level::warn,
                              "ev=nightmare_hunt_reward result=refused reason=armor_catalog slot=%u",
                              static_cast<unsigned>(slot));
            return NightmareHuntRewardStatus::failed;
        }
        rows[slot] = {static_cast<std::uint8_t>(WorldRewardKind::item), hash, 1};
    }
    constexpr std::uint8_t kSkolasClearStage = 1;
    const auto status = state::investment::store::enqueue_mission_rewards_once(
        sessionId, playerKey, attemptGeneration, kSkolasClearStage, rows);
    if (status == state::investment::store::MissionRewardStatus::failed) {
        return NightmareHuntRewardStatus::failed;
    }
    if (status == state::investment::store::MissionRewardStatus::alreadyQueued) {
        return NightmareHuntRewardStatus::alreadyQueued;
    }
    core::log::write(core::log::Channel::server, core::log::Level::info,
                     "ev=nightmare_hunt_reward set=tangled_web result=queued");
    if (!has_active_family4_peer()) {
        settle_world_reward();
    }
    return NightmareHuntRewardStatus::queued;
}

/**
 * One finished scripted event run. The receipt reuses the mission receipt table unchanged: stage 7
 * marks an event, and the attempt column holds attempt * 4096 + run, so every run pays once and
 * a retry or a reload of the same run is alreadyQueued.
 */
EventRewardStatus arm_event_reward(std::uint64_t sessionId,
                                   std::uint64_t playerKey,
                                   std::uint64_t attemptGeneration,
                                   std::uint16_t run,
                                   std::uint8_t tier,
                                   std::span<const std::uint32_t> weapons,
                                   std::span<const std::uint32_t> items) noexcept {
    constexpr std::uint64_t kRunSpace = 4096;
    constexpr std::uint8_t kEventStage = 7;
    constexpr std::uint64_t kLargestAttempt =
        static_cast<std::uint64_t>((std::numeric_limits<std::int64_t>::max)()) / kRunSpace - 1U;
    if (sessionId == 0 || playerKey == 0 || attemptGeneration == 0
        || attemptGeneration > kLargestAttempt || run < 1 || run >= kRunSpace || tier < 1
        || tier > 3 || weapons.empty() || weapons.size() > 32 || items.size() > 32) {
        return EventRewardStatus::failed;
    }
    std::array<std::uint32_t, 3> random{};
    if (!middleware::crypto::random::fill(std::as_writable_bytes(std::span(random)))) {
        return EventRewardStatus::failed;
    }
    std::array<state::investment::store::MissionRewardRow, 3> rows{};
    std::size_t rowCount = 0;
    const auto weapon = pick_weapon(weapons, random[0]);
    if (weapon == 0) {
        core::log::write(core::log::Channel::server, core::log::Level::warn,
                         "ev=event_reward result=refused reason=no_installed_weapon");
        return EventRewardStatus::failed;
    }
    rows[rowCount++] = {static_cast<std::uint8_t>(WorldRewardKind::menagerieWeapon), weapon, 1};
    std::uint32_t bonus = 0;
    if (tier >= 2) {
        bonus = pick_weapon(weapons, random[1]);
        if (bonus == weapon && weapons.size() > 1) {
            bonus = pick_weapon(weapons, random[1] + 1U);
        }
        if (bonus != 0) {
            rows[rowCount++] = {static_cast<std::uint8_t>(WorldRewardKind::menagerieWeapon),
                                bonus, 1};
        }
    }
    std::uint32_t item = 0;
    if (!items.empty()) {
        item = pick_installed(items, random[2], ItemRoute::reward);
        if (item != 0) {
            rows[rowCount++] = {static_cast<std::uint8_t>(WorldRewardKind::item), item, 1};
        }
    }
    const auto status = state::investment::store::enqueue_mission_rewards_once(
        sessionId, playerKey, attemptGeneration * kRunSpace + run, kEventStage,
        std::span(rows.data(), rowCount));
    if (status == state::investment::store::MissionRewardStatus::failed) {
        return EventRewardStatus::failed;
    }
    if (status == state::investment::store::MissionRewardStatus::alreadyQueued) {
        return EventRewardStatus::alreadyQueued;
    }
    core::log::writef(core::log::Channel::server, core::log::Level::info,
                      "ev=event_reward run=%u tier=%u weapon=0x%08X bonus=0x%08X item=0x%08X "
                      "result=queued",
                      static_cast<unsigned>(run), static_cast<unsigned>(tier),
                      static_cast<unsigned>(weapon), static_cast<unsigned>(bonus),
                      static_cast<unsigned>(item));
    if (!has_active_family4_peer()) {
        settle_world_reward();
    }
    return EventRewardStatus::queued;
}


/**
 * Restores the selected character's next reward, retiring rows that can never be granted.
 * @param request Receives the reward; cleared when none is ready.
 * @return False when no reward follows afterId or the item table is not published yet.
 */
bool current_world_reward(WorldRewardRequest& request,
                          std::uint64_t afterId,
                          bool* exhausted) noexcept {
    if (exhausted != nullptr) {
        *exhausted = false;
    }
    state::investment::store::PendingReward saved;
    bool interrupted = false;
    bool readFailed = false;
    while (state::investment::store::next_reward(saved, afterId, readFailed)) {
        request = {};
        request.id = saved.id;
        request.definitionHash = saved.definitionHash;
        request.quantity = saved.quantity;
        request.kind = saved.kind <= 2 ? static_cast<WorldRewardKind>(saved.kind)
                                       : WorldRewardKind::item;
        const bool itemsPublished = state::build_data::item_definitions_ready();
        state::build_data::items::Definition definition;
        const char* verdict = "reward_quantity";
        if (state::build_data::find_item_definition_hash(saved.definitionHash, definition)) {
            request.itemDefinitionIndex = definition.definitionIndex;
            if (request.quantity > 0) {
                return true;
            }
        } else if (!itemsPublished || !state::build_data::item_definitions_ready()) {
            // An unpublished item table proves nothing; retry later.
            interrupted = true;
            break;
        } else {
            request.itemDefinitionIndex = kUnresolvedItemIndex;
            verdict = "item_definition";
        }
        state::investment::store::Transaction transaction;
        if (!transaction.ready()) {
            report_reward_refusal("world_retire", request.itemDefinitionIndex, "transaction");
            interrupted = true;
            break;
        }
        if (!retire_world_reward(transaction, request, verdict)) {
            interrupted = true;
            break;
        }
    }
    if (readFailed) {
        report_reward_refusal("world_read", kUnresolvedItemIndex, "store_read");
        interrupted = true;
    }
    if (exhausted != nullptr) {
        *exhausted = !interrupted;
    }
    request = {};
    return false;
}

/** Acknowledges exactly the reward being granted or retired. */
bool complete_world_reward(std::uint64_t id) noexcept {
    return state::investment::store::complete_reward(id);
}

/**
 * Deletes the row on the caller's open savepoint, commits, and logs why.
 * @param transaction Open savepoint the deletion commits on.
 * @param reason Static label of the verdict that rules the reward out.
 * @return False when the row stays saved.
 */
bool retire_world_reward(state::investment::store::Transaction& transaction,
                         const WorldRewardRequest& request,
                         const char* reason) noexcept {
    const bool retired = complete_world_reward(request.id) && transaction.commit();
    core::log::writef(core::log::Channel::server,
                      core::log::Level::warn,
                      "ev=reward stage=world_retire id=%llu hash=0x%08X index=%u quantity=%d "
                      "result=%s reason=%s",
                      static_cast<unsigned long long>(request.id),
                      static_cast<unsigned>(request.definitionHash),
                      static_cast<unsigned>(request.itemDefinitionIndex),
                      static_cast<int>(request.quantity),
                      retired ? "retired" : "kept",
                      reason != nullptr ? reason : "unknown");
    return retired;
}

/** A granted reward reaches every peer through the account image. */
void settle_world_reward() noexcept {
    if (settle_saved_world_rewards()) {
        arm_account_resync_everywhere();
    }
}

/** Shutdown grants or retires every reward it can; other characters' rewards stay saved. */
void drain_world_rewards() noexcept {
    settle_saved_world_rewards();
}

} // namespace sunrise::server::bap
