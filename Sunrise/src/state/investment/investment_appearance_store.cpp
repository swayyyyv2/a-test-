#include "store_internal.h"

namespace sunrise::state::investment::store {

bool write_armor_appearance(std::uint64_t targetInstanceSoid,
                            std::uint32_t targetDefinitionHash,
                            std::uint32_t sourceDefinitionHash,
                            std::uint8_t characterClass) noexcept {
    if (targetInstanceSoid == 0 || targetDefinitionHash == 0 || sourceDefinitionHash == 0
        || characterClass > 2) return false;
    Transaction transaction;
    Statement row("INSERT INTO armor_appearance_overrides VALUES (?,?,?,?) "
                  "ON CONFLICT(target_instance_soid) DO UPDATE SET "
                  "target_definition_hash=excluded.target_definition_hash,"
                  "source_definition_hash=excluded.source_definition_hash,"
                  "character_class=excluded.character_class");
    return transaction.ready()
           && row.write(targetInstanceSoid, targetDefinitionHash, sourceDefinitionHash,
                        characterClass)
           && transaction.commit();
}

bool read_armor_appearance(std::uint64_t targetInstanceSoid,
                           std::uint32_t& targetDefinitionHash,
                           std::uint32_t& sourceDefinitionHash,
                           std::uint8_t& characterClass) noexcept {
    const std::lock_guard lock(g_mutex);
    Statement row("SELECT target_definition_hash,source_definition_hash,character_class "
                  "FROM armor_appearance_overrides WHERE target_instance_soid=?");
    return row.parameters(targetInstanceSoid) && row.step() == SQLITE_ROW
           && row.columns(targetDefinitionHash, sourceDefinitionHash, characterClass);
}

bool clear_armor_appearance(std::uint64_t targetInstanceSoid) noexcept {
    Transaction transaction;
    Statement row("DELETE FROM armor_appearance_overrides WHERE target_instance_soid=?");
    return transaction.ready() && row.write(targetInstanceSoid) && transaction.commit();
}

bool write_appearance_pose(std::uint8_t characterSlot, std::uint8_t pose) noexcept {
    if (characterSlot > 2 || pose > 2) return false;
    Transaction transaction;
    Statement row("INSERT INTO appearance_preview_pose VALUES (?,?) "
                  "ON CONFLICT(character_slot) DO UPDATE SET pose=excluded.pose");
    return transaction.ready() && row.write(characterSlot, pose) && transaction.commit();
}

bool read_appearance_pose(std::uint8_t characterSlot, std::uint8_t& pose) noexcept {
    const std::lock_guard lock(g_mutex);
    Statement row("SELECT pose FROM appearance_preview_pose WHERE character_slot=?");
    return row.parameters(characterSlot) && row.step() == SQLITE_ROW && row.column(0, pose);
}

} // namespace sunrise::state::investment::store
