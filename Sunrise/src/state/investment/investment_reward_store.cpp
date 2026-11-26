#include "store_internal.h"

namespace sunrise::state::investment::store {
namespace {

/** A reward is owned by the character that was selected when it was earned. */
int selected() noexcept {
    for (std::size_t slot = 0; slot < g_session.selected.size(); ++slot) {
        if (g_session.selected[slot]) {
            return static_cast<int>(slot);
        }
    }
    return -1;
}

} // namespace

/** Reward ownership and quantities are committed before presentation is attempted. */
bool enqueue_reward(std::uint32_t definitionHash,
                    std::int32_t quantity,
                    std::uint8_t kind) noexcept {
    const std::lock_guard lock(g_mutex);
    const int owner = selected();
    if (owner < 0 || kind > 2) {
        return false;
    }
    Statement row("INSERT INTO pending_rewards(character_slot,kind,definition_hash,quantity) "
                  "VALUES (?,?,?,?)");
    return row.write(owner, kind, definitionHash, quantity);
}

/** A receipt and both earned rows share a savepoint; the receipt stays after pickup. */
MissionRewardStatus enqueue_mission_reward_once(std::uint64_t sessionId,
                                                std::uint64_t playerKey,
                                                std::uint64_t attemptGeneration,
                                                std::uint8_t stage,
                                                std::uint32_t weaponHash,
                                                std::uint32_t currencyHash,
                                                std::int32_t currencyQuantity) noexcept {
    if (sessionId == 0 || playerKey == 0 || attemptGeneration == 0 || stage < 1 || stage > 7
        || weaponHash == 0 || (currencyHash != 0 && currencyQuantity <= 0)) {
        return MissionRewardStatus::failed;
    }
    Transaction transaction;
    if (!transaction.ready()) {
        return MissionRewardStatus::failed;
    }
    const int owner = selected();
    if (owner < 0) {
        return MissionRewardStatus::failed;
    }
    Statement receipt("INSERT OR IGNORE INTO mission_reward_receipts("
                      "session_id,player_key,attempt_generation,stage) VALUES (?,?,?,?)");
    if (!receipt.write(sessionId, playerKey, attemptGeneration, stage)) {
        return MissionRewardStatus::failed;
    }
    if (sqlite3_changes(g_database) == 0) {
        return transaction.commit() ? MissionRewardStatus::alreadyQueued
                                    : MissionRewardStatus::failed;
    }
    Statement reward("INSERT INTO pending_rewards(character_slot,kind,definition_hash,quantity) "
                     "VALUES (?,?,?,?)");
    if (!reward.write(owner, 2, weaponHash, 1)
        || (currencyHash != 0 && !reward.write(owner, 1, currencyHash, currencyQuantity))
        || !transaction.commit()) {
        return MissionRewardStatus::failed;
    }
    return MissionRewardStatus::queued;
}

/** Same receipt rule as the single-weapon form, for a stage that earns several rows. */
MissionRewardStatus enqueue_mission_rewards_once(std::uint64_t sessionId,
                                                 std::uint64_t playerKey,
                                                 std::uint64_t attemptGeneration,
                                                 std::uint8_t stage,
                                                 std::span<const MissionRewardRow> rows) noexcept {
    if (sessionId == 0 || playerKey == 0 || attemptGeneration == 0 || stage < 1 || stage > 7
        || rows.empty() || rows.size() > 8) {
        return MissionRewardStatus::failed;
    }
    for (const MissionRewardRow& row : rows) {
        if (row.kind > 2 || row.definitionHash == 0 || row.quantity <= 0) {
            return MissionRewardStatus::failed;
        }
    }
    Transaction transaction;
    if (!transaction.ready()) {
        return MissionRewardStatus::failed;
    }
    const int owner = selected();
    if (owner < 0) {
        return MissionRewardStatus::failed;
    }
    Statement receipt("INSERT OR IGNORE INTO mission_reward_receipts("
                      "session_id,player_key,attempt_generation,stage) VALUES (?,?,?,?)");
    if (!receipt.write(sessionId, playerKey, attemptGeneration, stage)) {
        return MissionRewardStatus::failed;
    }
    if (sqlite3_changes(g_database) == 0) {
        return transaction.commit() ? MissionRewardStatus::alreadyQueued
                                    : MissionRewardStatus::failed;
    }
    Statement reward("INSERT INTO pending_rewards(character_slot,kind,definition_hash,quantity) "
                     "VALUES (?,?,?,?)");
    for (const MissionRewardRow& row : rows) {
        if (!reward.write(owner, row.kind, row.definitionHash, row.quantity)) {
            return MissionRewardStatus::failed;
        }
    }
    return transaction.commit() ? MissionRewardStatus::queued : MissionRewardStatus::failed;
}


/** Other characters' earned rewards remain queued; only a clean SQL step proves no row follows. */
bool next_reward(PendingReward& output, std::uint64_t afterId, bool& readFailed) noexcept {
    const std::lock_guard lock(g_mutex);
    output = {};
    Statement row("SELECT id,definition_hash,quantity,kind FROM pending_rewards "
                  "WHERE character_slot=? AND id>? ORDER BY id LIMIT 1");
    const int step = row.parameters(selected(), afterId) ? row.step() : SQLITE_ERROR;
    const bool found = step == SQLITE_ROW
                       && row.columns(output.id, output.definitionHash, output.quantity, output.kind);
    readFailed = !found && step != SQLITE_DONE;
    return found;
}

/** The inventory grant and deletion of this reward must share one outer transaction. */
bool complete_reward(std::uint64_t id) noexcept {
    const std::lock_guard lock(g_mutex);
    Statement row("DELETE FROM pending_rewards WHERE id=? AND character_slot=?");
    return row.write(id, selected()) && sqlite3_changes(g_database) == 1;
}

} // namespace sunrise::state::investment::store
