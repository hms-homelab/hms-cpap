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
    bool tombstoneEquipmentItem(int, const std::string&) override { return false; }
