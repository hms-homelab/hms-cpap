#pragma once
//
// EquipmentStubs.h — no-op implementations of the SDD-004 equipment surface for
// test doubles.
//
// IDatabase declares 15 pure virtual equipment methods so that every real backend
// is forced to implement them (that is the point of the contract — a backend must
// not be able to silently skip one). The consequence is that every test double
// implementing IDatabase becomes abstract the moment the contract grows.
//
// Rather than paste 15 stubs into each double, doubles that do not care about
// equipment drop in HMS_CPAP_STUB_EQUIPMENT_METHODS. Tests that DO exercise
// equipment use a real backend (SQLite in-memory) instead of a double, so these
// stubs never stand in for behaviour under test.
//
#include "database/IDatabase.h"

#define HMS_CPAP_STUB_EQUIPMENT_METHODS                                              \
    std::vector<IDatabase::EquipmentType> listEquipmentTypes() override {            \
        return {};                                                                   \
    }                                                                                \
    std::optional<IDatabase::EquipmentType> resolveEquipmentType(                    \
        const std::string&) override {                                               \
        return std::nullopt;                                                         \
    }                                                                                \
    int addEquipmentType(const IDatabase::EquipmentType&) override { return -1; }    \
    bool updateEquipmentType(int, const IDatabase::EquipmentType&) override {         \
        return false;                                                                \
    }                                                                                \
    bool deleteEquipmentType(int) override { return false; }                         \
    std::vector<IDatabase::EquipmentProfile> listEquipmentProfiles(bool) override {   \
        return {};                                                                   \
    }                                                                                \
    std::optional<IDatabase::EquipmentProfile> getEquipmentProfile(int) override {    \
        return std::nullopt;                                                         \
    }                                                                                \
    int upsertEquipmentProfile(const IDatabase::EquipmentProfile&,                    \
                               const std::string&) override {                        \
        return -1;                                                                   \
    }                                                                                \
    bool tombstoneEquipmentProfile(int, const std::string&) override { return false; }\
    int ensureDefaultEquipmentProfile() override { return -1; }                      \
    std::vector<IDatabase::EquipmentItem> listEquipmentItems(bool) override {         \
        return {};                                                                   \
    }                                                                                \
    std::optional<IDatabase::EquipmentItem> getEquipmentItem(int) override {          \
        return std::nullopt;                                                         \
    }                                                                                \
    bool profileHasMachine(int, int) override { return false; }                      \
    int upsertEquipmentItem(const IDatabase::EquipmentItem&,                          \
                            const std::string&) override { return -1; }               \
    bool tombstoneEquipmentItem(int, const std::string&) override { return false; }  \
    /* SDD-007 cleaning. Same rationale as the equipment stubs above: these      \
       doubles exist to test collectors and ML, which never touch upkeep, and    \
       IDatabase's methods are pure so that a REAL backend cannot quietly ship   \
       a stub. That is the mistake 4.6.3 had to undo. Anything actually          \
       exercising cleaning uses a real engine via test_CleaningBackends.cpp. */  \
    std::vector<IDatabase::CleaningTaskType> listCleaningTaskTypes() override {   \
        return {};                                                               \
    }                                                                            \
    std::vector<IDatabase::CleaningTask> listCleaningTasks(int) override {        \
        return {};                                                               \
    }                                                                            \
    std::optional<IDatabase::CleaningTask> getCleaningTask(int) override {        \
        return std::nullopt;                                                     \
    }                                                                            \
    int upsertCleaningTask(const IDatabase::CleaningTask&,                        \
                           const std::string&) override { return -1; }            \
    bool tombstoneCleaningTask(int, const std::string&) override { return false; }\
    bool markCleaningTaskDone(int, const std::string&) override { return false; }

// SDD-008 sync folder ledger, in its OWN macro rather than folded into the one
// above. A double that returns nothing here reads as "no folder has ever been
// observed", which is the correct starting state and cannot fake progress --
// but it also means a folder can never settle, since settling requires
// comparing against a stored signature. Doubles that need the ledger to behave
// (the burst-orchestration mock) implement these three themselves instead of
// taking this macro. Anything storage-shaped uses a real engine via
// test_SyncFolderBackends.cpp.
#define HMS_CPAP_STUB_SYNC_FOLDER_METHODS                                        \
    std::vector<FolderLedger> listSyncFolders() override { return {}; }          \
    std::optional<FolderLedger> getSyncFolder(const std::string&) override {     \
        return std::nullopt;                                                     \
    }                                                                            \
    bool upsertSyncFolder(const FolderLedger&) override { return false; }
