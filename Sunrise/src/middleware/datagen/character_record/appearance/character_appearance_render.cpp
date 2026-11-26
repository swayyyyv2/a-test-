#include <algorithm>

#include "../../../../state/build_data/runtime.h"
#include "../../../../state/investment/store.h"
#include "internal.h"
#include "transmog.h"

namespace sunrise::middleware::datagen::character_record::appearance::transmog {
namespace {

namespace details = state::build_data::items::details;

[[nodiscard]] bool armor_slot(const details::Definition& detail) noexcept {
    if (!detail.equipmentSlot) return false;
    switch (*detail.equipmentSlot) {
    case 1: case 2: case 4: case 5: case 6: return true;
    default: return false;
    }
}

[[nodiscard]] bool art_for_class(const details::Definition& detail,
                                 state::CharacterClass characterClass) noexcept {
    const std::size_t slot = static_cast<std::size_t>(characterClass) + 1U;
    if (slot < detail.artArrangementIndices.size()
        && detail.artArrangementIndices[slot] != details::kUnavailableArtIndex) return true;
    const bool otherClassArt = std::any_of(detail.artArrangementIndices.begin() + 1,
                                           detail.artArrangementIndices.end(),
                                           [](std::uint16_t value) {
                                               return value != details::kUnavailableArtIndex;
                                           });
    if (otherClassArt) return false;
    return detail.gearArtIndex != details::kUnavailableArtIndex
           || detail.artArrangementIndices.front() != details::kUnavailableArtIndex;
}

[[nodiscard]] bool valid_pair(std::uint32_t targetHash,
                              std::uint32_t sourceHash,
                              state::CharacterClass characterClass,
                              details::Definition* sourceOutput = nullptr) noexcept {
    state::build_data::items::Definition targetDefinition{}, sourceDefinition{};
    details::Definition target{}, source{};
    if (!state::build_data::find_item_definition_hash(targetHash, targetDefinition)
        || !state::build_data::find_item_definition_hash(sourceHash, sourceDefinition)
        || !state::build_data::find_configured_item_detail(targetDefinition.definitionIndex, target)
        || !state::build_data::find_configured_item_detail(sourceDefinition.definitionIndex, source)
        || !armor_slot(target) || !armor_slot(source)
        || target.equipmentSlot != source.equipmentSlot
        || target.bucketId != source.bucketId
        || !art_for_class(source, characterClass)) return false;
    if (sourceOutput) *sourceOutput = source;
    return true;
}

} // namespace

bool set(std::uint64_t targetInstanceSoid,
         std::uint32_t targetDefinitionHash,
         std::uint32_t sourceDefinitionHash,
         state::CharacterClass characterClass) noexcept {
    return targetInstanceSoid != 0 && targetDefinitionHash != sourceDefinitionHash
           && valid_pair(targetDefinitionHash, sourceDefinitionHash, characterClass)
           && state::investment::store::write_armor_appearance(
                  targetInstanceSoid, targetDefinitionHash, sourceDefinitionHash,
                  static_cast<std::uint8_t>(characterClass));
}

void clear(std::uint64_t targetInstanceSoid) noexcept {
    (void)state::investment::store::clear_armor_appearance(targetInstanceSoid);
}

bool source_hash(std::uint64_t targetInstanceSoid,
                 std::uint32_t targetDefinitionHash,
                 state::CharacterClass characterClass,
                 std::uint32_t& output) noexcept {
    std::uint32_t storedTarget{}, storedSource{};
    std::uint8_t storedClass{};
    if (!state::investment::store::read_armor_appearance(
            targetInstanceSoid, storedTarget, storedSource, storedClass)
        || storedTarget != targetDefinitionHash
        || storedClass != static_cast<std::uint8_t>(characterClass)) return false;
    output = storedSource;
    return true;
}

bool source_detail(std::uint64_t targetInstanceSoid,
                   std::uint32_t targetDefinitionHash,
                   state::CharacterClass characterClass,
                   details::Definition& output) noexcept {
    std::uint32_t sourceHash = 0;
    return source_hash(targetInstanceSoid, targetDefinitionHash, characterClass, sourceHash)
           && valid_pair(targetDefinitionHash, sourceHash, characterClass, &output);
}

} // namespace sunrise::middleware::datagen::character_record::appearance::transmog

namespace sunrise::middleware::datagen::character_record::appearance {
namespace {

/**
 * The three art stages, in the order the client folds them.
 * Base stage 1, then plug stage 1, plug stage 0, base stage 2, plug stage 2. The order decides
 * which 6 keys survive, so the stages cannot be merged.
 */
enum class Stage : std::uint8_t {
    plugOnly = 0,
    base = 1,
    late = 2,
};

/** Insertion-ordered material keys. Only the first 6 reach the record, so 6 is the whole set. */
struct Fold {
    std::array<layout::MaterialPair, layout::kMaterialPairCapacity> pairs{};
    std::size_t count{};
};

/**
 * Records one material row, keeping insertion order and letting a later stage update a value.
 * A key arriving after 6 distinct keys are held can never reach the record, so it is dropped.
 * @param fold Ordered material keys.
 * @param key Material key this row names.
 * @param value Material value stored against that key.
 */
void record(Fold& fold, std::int8_t key, std::uint16_t value) noexcept {
    for (std::size_t entry = 0; entry < fold.count; ++entry) {
        if (fold.pairs[entry].key == key) {
            fold.pairs[entry].value = value;
            return;
        }
    }
    if (fold.count < fold.pairs.size()) {
        fold.pairs[fold.count].key = key;
        fold.pairs[fold.count].value = value;
        ++fold.count;
    }
}

/**
 * Folds one definition's rows for a single art stage.
 * @param detail Installed detail carrying the definition's override rows.
 * @param stage Art stage to take.
 * @param fold Ordered material keys.
 */
void fold_stage(const details::Definition& detail, Stage stage, Fold& fold) noexcept {
    const std::size_t rows = detail.renderOverrideCount < detail.renderOverrides.size()
                                 ? detail.renderOverrideCount
                                 : detail.renderOverrides.size();
    for (std::size_t entry = 0; entry < rows; ++entry) {
        const details::RenderOverride& row = detail.renderOverrides[entry];
        if (row.stage == static_cast<std::uint8_t>(stage)
            && row.key != details::kEmptyOverrideKey) {
            record(fold, row.key, row.value);
        }
    }
}

/**
 * Folds every plug lane's rows for one art stage.
 * @param equipped Effective plug lanes.
 * @param stage Art stage to take.
 * @param fold Ordered material keys.
 */
void fold_plug_stage(const Equipped& equipped, Stage stage, Fold& fold) noexcept {
    for (std::size_t lane = 0; lane < equipped.laneCount; ++lane) {
        details::Definition plug{};
        if (equipped.plugs[lane] == details::kUnavailableItemIndex
            || !state::build_data::find_configured_item_detail(equipped.plugs[lane], plug)) {
            continue;
        }
        fold_stage(plug, stage, fold);
    }
}

/**
 * Builds the 6 material pairs one equipped item publishes.
 * @param detail Base item detail.
 * @param equipped Effective plug lanes.
 * @param entry Render row receiving the pairs.
 */
void apply_material_pairs(const details::Definition& detail,
                          const Equipped& equipped,
                          layout::RenderEntry& entry) noexcept {
    Fold fold{};
    fold_stage(detail, Stage::base, fold);
    fold_plug_stage(equipped, Stage::base, fold);
    fold_plug_stage(equipped, Stage::plugOnly, fold);
    fold_stage(detail, Stage::late, fold);
    fold_plug_stage(equipped, Stage::late, fold);
    for (std::size_t pair = 0; pair < fold.count; ++pair) {
        entry.materialPairs[pair] = fold.pairs[pair];
    }
}

/** Applies plug-owned gear art and appends its class-qualified overlay arrangement. */
void apply_plug_art(const Equipped& equipped,
                    state::CharacterClass characterClass,
                    layout::RenderEntry& entry) noexcept {
    for (std::size_t lane = 0; lane < equipped.laneCount; ++lane) {
        details::Definition plug{};
        if (equipped.plugs[lane] == details::kUnavailableItemIndex
            || !state::build_data::find_configured_item_detail(equipped.plugs[lane], plug)) {
            continue;
        }
        // Shaders normally carry material rows only. Ornaments carry one or both art indices;
        // the effective plug therefore replaces only the fields it actually declares.
        if (plug.gearArtIndex != details::kUnavailableArtIndex) {
            entry.art[layout::kGearArtSlot] = plug.gearArtIndex;
        }
        const std::uint16_t overlay = select_art_arrangement(plug, characterClass);
        if (overlay != details::kUnavailableArtIndex) {
            const auto empty = std::find(
                entry.overlays.begin(), entry.overlays.end(), layout::kEmptyDefinitionIndex);
            if (empty != entry.overlays.end()) {
                *empty = overlay;
            }
        }
    }
}

} // namespace

/** Resolves one equipped instance to its detail and the plugs its sockets hold. */
bool resolve_equipped(const family4::loadout::SlottedInstance& slotted,
                      details::Definition& detail,
                      Equipped& equipped) noexcept {
    equipped = {};
    equipped.plugs.fill(details::kUnavailableItemIndex);
    equipped.equipmentSlot = slotted.equipmentSlot;
    equipped.definitionIndex = slotted.instance.baseDefinitionIndex;
    if (!state::build_data::find_configured_item_detail(slotted.instance.baseDefinitionIndex,
                                                        detail)) {
        return false;
    }
    equipped.laneCount = detail.ordinarySocketCount < equipped.plugs.size()
                             ? detail.ordinarySocketCount
                             : equipped.plugs.size();
    const auto& authored = slotted.instance.ordinarySockets.plugs;
    for (std::size_t lane = 0; lane < equipped.laneCount && lane < authored.size(); ++lane) {
        if (authored[lane].has_value()) {
            equipped.plugs[lane] = *authored[lane];
        }
    }
    return true;
}

/**
 * Resolves one socketed plug to the item row that supplies its perks and stats.
 * @param equipped Equipped base item and its visible socket plugs.
 * @param lane Zero-based ordinary socket lane.
 * @return Effective plug definition, or the unavailable sentinel for an invalid lane.
 */
std::uint16_t resolve_effective_plug(const Equipped& equipped, std::size_t lane) noexcept {
    if (lane >= equipped.laneCount || lane >= equipped.plugs.size()) {
        return details::kUnavailableItemIndex;
    }
    return state::build_data::resolve_exotic_catalyst_effect(
        equipped.definitionIndex, static_cast<std::uint8_t>(lane), equipped.plugs[lane]);
}

/** Fills each equipped render row with its instance, definition, art and material pairs. */
bool apply_render(const family4::loadout::ResolvedInstances& instances,
                  state::CharacterClass characterClass,
                  layout::Appearance& appearance) noexcept {
    for (std::size_t index = 0; index < instances.itemCount; ++index) {
        const family4::loadout::SlottedInstance& slotted = instances.items[index];
        if (slotted.equipmentSlot >= appearance.render.size()) {
            return false;
        }
        layout::RenderEntry& entry = appearance.render[slotted.equipmentSlot];
        entry.instanceSoid = slotted.instance.instanceSoid;
        entry.definitionIndex = slotted.instance.baseDefinitionIndex;
        details::Definition detail{};
        Equipped equipped{};
        if (!resolve_equipped(slotted, detail, equipped)) {
            continue;
        }
        // Both art lookups accept 0, so an item with no art block keeps the empty sentinel
        // rather than taking art row 0.
        entry.art[layout::kGearArtSlot] = detail.gearArtIndex;
        entry.art[layout::kArtArrangementSlot] = select_art_arrangement(detail, characterClass);
        details::Definition appearanceSource{};
        if (transmog::source_detail(slotted.instance.instanceSoid,
                                   detail.definitionHash,
                                   characterClass,
                                   appearanceSource)) {
            // Only render art changes; the base definition and plug banks still supply gameplay.
            entry.art[layout::kGearArtSlot] = appearanceSource.gearArtIndex;
            entry.art[layout::kArtArrangementSlot] =
                select_art_arrangement(appearanceSource, characterClass);
            apply_material_pairs(appearanceSource, equipped, entry);
        } else {
            apply_plug_art(equipped, characterClass, entry);
            apply_material_pairs(detail, equipped, entry);
        }
    }
    return true;
}

} // namespace sunrise::middleware::datagen::character_record::appearance
