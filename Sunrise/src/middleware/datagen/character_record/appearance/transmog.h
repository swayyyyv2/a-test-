#pragma once

#include <cstdint>

#include "../../../../state/account/account_state.h"
#include "../../../../state/build_data/items/details/definition.h"

namespace sunrise::middleware::datagen::character_record::appearance::transmog {

/** Applies a saved armor art source while preserving the target item and its sockets. */
[[nodiscard]] bool set(std::uint64_t targetInstanceSoid,
                       std::uint32_t targetDefinitionHash,
                       std::uint32_t sourceDefinitionHash,
                       state::CharacterClass characterClass) noexcept;

/** Removes one saved appearance override. */
void clear(std::uint64_t targetInstanceSoid) noexcept;

/** Reads the exact override for an item/class, if one is active. */
[[nodiscard]] bool source_hash(std::uint64_t targetInstanceSoid,
                               std::uint32_t targetDefinitionHash,
                               state::CharacterClass characterClass,
                               std::uint32_t& output) noexcept;

/** Resolves the selected source to render art for the current character record. */
[[nodiscard]] bool source_detail(std::uint64_t targetInstanceSoid,
                                 std::uint32_t targetDefinitionHash,
                                 state::CharacterClass characterClass,
                                 state::build_data::items::details::Definition& output) noexcept;

} // namespace sunrise::middleware::datagen::character_record::appearance::transmog
