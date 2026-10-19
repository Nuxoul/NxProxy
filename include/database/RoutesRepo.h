#pragma once

#include "Database.h"
#include "include/database/entities/RouteProfile.h"
#include "include/database/entities/LocalRouteOverride.h"
#include <3rdparty/SQLiteCpp/include/SQLiteCpp.h>
#include <memory>
#include <mutex>
#include <map>
#include <QString>
#include <QJsonObject>
#include <QJsonDocument>
#include <QJsonArray>
#include <QMutex>

namespace Configs {
    class RoutesRepo {
    private:
        // Private connection keeps route transactions isolated from unrelated repository writes.
        mutable Database db;
        mutable std::mutex mutex;
        mutable std::map<int, std::weak_ptr<RouteProfile>> identityMap;

        QJsonObject routeRuleToJson(const RouteRule* rule) const;
        
        std::shared_ptr<RouteRule> routeRuleFromJson(const QJsonObject& json) const;
        
        QJsonObject routeProfileToJson(const RouteProfile* routeProfile) const;
        
        std::shared_ptr<RouteProfile> routeProfileFromJson(const QJsonObject& json) const;
        
        bool saveToDatabase(const RouteProfile* routeProfile, int id) const;

        // Throws, so the caller's transaction aborts.
        void saveToDatabaseInTx(const RouteProfile* routeProfile, int id) const;
        
        std::shared_ptr<RouteProfile> loadFromDatabase(int id) const;
        
        // Rules are left empty.
        std::shared_ptr<RouteProfile> routeProfileFromProfileRow(SQLite::Statement& stmt) const;
        
        // baseCol is the column index the rule name sits at.
        QJsonObject ruleJsonFromRow(SQLite::Statement& stmt, int baseCol) const;

        // Appends to byId[profileId]->Rules rather than replacing.
        void loadRulesForProfileIdsChunk(const QList<int>& profileIds, std::map<int, std::shared_ptr<RouteProfile>>& byId) const;
        
        void createTables() const;
        QList<LocalRouteOverride> loadLocalOverrides(int routeID) const;

        [[nodiscard]] bool routeRulesColumnExists(const char* columnName) const;

        [[nodiscard]] bool routeProfilesColumnExists(const char* columnName) const;

        int NewRouteProfileID() const;

    public:
        explicit RoutesRepo(Database& database);
        
        // Not saved yet: id stays -1 until AddRouteProfile.
        [[nodiscard]] static std::shared_ptr<RouteProfile> NewRouteProfile();
        
        bool AddRouteProfile(std::shared_ptr<RouteProfile>& routeProfile);
        
        std::shared_ptr<RouteProfile> GetRouteProfile(int id) const;
        
        void DeleteRouteProfile(int id);
        
        // Replaces the whole set.
        bool UpdateRouteProfiles(const QList<std::shared_ptr<RouteProfile>>& routeProfiles);
        
        QList<int> GetAllRouteProfileIds() const;

        QList<std::shared_ptr<RouteProfile>> GetAllRouteProfiles() const;
        
        // No-op unless id >= 0.
        bool Save(const std::shared_ptr<RouteProfile>& routeProfile);
        RoutingSnapshot GetRoutingSnapshot(int routeID, QString* error = nullptr) const;
        bool SetLocalOverride(int routeID, const QString& raw, simpleAction action,
                              QString* error = nullptr, const LocalRouteOverride* expected = nullptr);
        bool RemoveLocalOverride(int routeID, qint64 id, qint64 priority, QString* error = nullptr);
    };
}
