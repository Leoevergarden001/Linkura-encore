#include "HttpMockBackend.hpp"

#include "../MockPlatform.hpp"
#include "http_mock_backend_builtin_sql.hpp"
#include "MuseumGachaCatalog.hpp"
#include "offline_api_mock_builtin.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <mutex>
#include <random>
#include <string>
#include <utility>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "sqlite3.h"

namespace LinkuraLocal::HttpMock {
    namespace {
        struct MuseumGachaSeries {
            int id;
            int exchangePointId;
            std::vector<int> featuredCards;
            std::vector<int> cards;
        };
        // 10215210 occurred in a draw that reproduced a CardAnimationPlayer
        // null-reference on the offline client. The exact missing dependency
        // has not been isolated, so exclude it from future Gacha draws until
        // its asset chain and playback can be checked. Existing ownership
        // remains intact.
        inline constexpr std::array<int, 1> kMuseumAnimationQuarantinedCards{{
            10215210,
        }};
        static bool IsMuseumAnimationQuarantined(int cardId) {
            return std::find(kMuseumAnimationQuarantinedCards.begin(),
                             kMuseumAnimationQuarantinedCards.end(), cardId) !=
                   kMuseumAnimationQuarantinedCards.end();
        }
        // Featured cards from the 5.1.0 GachaSeries/CardSeries masters.
        //
        // Gacha responses carry the base CardDatas record (the final digit is
        // 0), not the fully evolved museum-card record (final digit 4).  The
        // original animation resolves CardCoordinates and
        // CardGetMovieSettings by that base ID.  Returning the evolved ID can
        // still render a detail fallback, but it leaves animation metadata
        // null and makes consecutive draws fault before SetPhase.
        //
        // Two featured 94 records lack a CardGetMovieSettings record in the
        // preserved 5.1 cache.  Museum mode uses the matching 95 record for
        // that character instead, so every offline result has a recoverable
        // animation path.  This is a local preservation catalog, not a claim
        // about the retired server's original odds table.
        static const auto kMuseumGachaSeries = [] {
            std::array<MuseumGachaSeries, 7> series{{
                {1055003, 105503, {19415010, 19425010, 19435010, 19315020, 19525010}, {}},
                {1055002, 105502, {19215020, 19235010, 19425020, 19515010}, {}},
                {1055001, 105501, {19215010, 19225010, 19235010, 19315010, 19335010}, {}},
                {1050511, 10505, {10305010}, {}},
                {1050509, 10505, {10315390, 10325370, 10335360}, {}},
                {1050506, 10505, {10215170, 10225170, 10225180, 10235140, 10115010}, {}},
                {1050505, 10505, {10215270, 10225270, 10235240, 10205010}, {}},
            }};
            for (auto& pool : series) {
                pool.cards.reserve(kMuseumStandardRCards.size() + kMuseumStandardSRCards.size() +
                                   kMuseumStandardURCards.size() + pool.featuredCards.size());
                pool.cards.insert(pool.cards.end(), kMuseumStandardRCards.begin(), kMuseumStandardRCards.end());
                pool.cards.insert(pool.cards.end(), kMuseumStandardSRCards.begin(), kMuseumStandardSRCards.end());
                for (const int cardId : kMuseumStandardURCards) {
                    if (!IsMuseumAnimationQuarantined(cardId)) pool.cards.push_back(cardId);
                }
                pool.featuredCards.erase(
                    std::remove_if(pool.featuredCards.begin(), pool.featuredCards.end(),
                                   IsMuseumAnimationQuarantined),
                    pool.featuredCards.end());
                pool.cards.insert(pool.cards.end(), pool.featuredCards.begin(), pool.featuredCards.end());
            }
            return series;
        }();
        static int ChooseMuseumGachaCard(const MuseumGachaSeries& series, std::mt19937& rng) {
            // Museum approximation, NOT the retired server's per-card odds.
            // Tier selection prevents the catalog's large UR count from
            // turning a uniform card-ID draw into a nearly 50% UR rate.
            std::uniform_int_distribution<int> tier(0, 99);
            const auto pick = [&rng](const auto& ids) -> int {
                std::uniform_int_distribution<size_t> index(0, ids.size() - 1);
                return ids[index(rng)];
            };
            const int roll = tier(rng);
            if (roll < 80) return pick(kMuseumStandardRCards);
            if (roll < 97) return pick(kMuseumStandardSRCards);
            std::bernoulli_distribution featured(0.5);
            if (featured(rng) && !series.featuredCards.empty()) return pick(series.featuredCards);
            // Preserve the original approximate UR odds while substituting a
            // known-good preserved card for a quarantined animation record.
            const int candidate = pick(kMuseumStandardURCards);
            return IsMuseumAnimationQuarantined(candidate) ? 10315140 : candidate;
        }
        static const MuseumGachaSeries* FindMuseumGachaSeries(int id) {
            for (const auto& series : kMuseumGachaSeries) if (series.id == id) return &series;
            return nullptr;
        }
        static int PointOwnerSeriesId(const MuseumGachaSeries& series) {
            // Several graduation pools share the same ExchangePointId in Master.
            int owner = series.id;
            for (const auto& candidate : kMuseumGachaSeries) {
                if (candidate.exchangePointId == series.exchangePointId)
                    owner = std::min(owner, candidate.id);
            }
            return owner;
        }
        static std::filesystem::path GetBackendDatabasePath() {
            const auto base = LinkuraLocal::Local::GetBasePath();
            const auto configDir = base.parent_path();
            return (configDir / "HasuKikaisann.sqlite3").lexically_normal();
        }

        static std::string ExtractJsonIntegerFieldAsString(std::string_view payloadJson, std::string_view fieldName) {
            if (payloadJson.empty()) {
                return {};
            }

            const auto payload = nlohmann::json::parse(payloadJson.begin(), payloadJson.end(), nullptr, false);
            if (!payload.is_object()) {
                return {};
            }

            const auto it = payload.find(std::string(fieldName));
            if (it == payload.end()) {
                return {};
            }

            if (it->is_number_integer()) {
                return std::to_string(it->get<int64_t>());
            }
            if (it->is_string()) {
                return it->get<std::string>();
            }

            return {};
        }

        static std::string ExtractJsonStringField(std::string_view payloadJson, std::string_view fieldName) {
            if (payloadJson.empty()) {
                return {};
            }

            const auto payload = nlohmann::json::parse(payloadJson.begin(), payloadJson.end(), nullptr, false);
            if (!payload.is_object()) {
                return {};
            }

            const auto it = payload.find(std::string(fieldName));
            if (it == payload.end() || !it->is_string()) {
                return {};
            }

            return it->get<std::string>();
        }
        static const nlohmann::json kDefaultCharacterBonus = {
            {"character_id", 0},
            {"music_mastery_bonus", 0},
            {"love_correction_value", 0},
            {"music_mastery_bonus_list", nullptr},
            {"season_fan_level", 0},
        };

        static const nlohmann::json kDefaultRentalDeckData = {
            {"rental_deck_id", 0},
            {"rental_deck_cards_list", nullptr},
            {"grade_bonus_value", 0},
            {"is_released", false},
        };

        static nlohmann::json LookupSectionSkills(sqlite3* db, int stageId) {
            if (!db || stageId <= 0) return nlohmann::json::array();

            static constexpr const char* kTables[] = {
                "quest_stage",
                "daily_quest_stage",
                "dream_quest_stage",
                "grade_quest_stage",
                "grand_prix_stage",
                "learning_stage",
            };

            for (const char* table : kTables) {
                std::string sql = "SELECT section_skills_json FROM ";
                sql += table;
                sql += " WHERE stage_id = ?;";

                sqlite3_stmt* stmt = nullptr;
                if (sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr) == SQLITE_OK && stmt) {
                    sqlite3_bind_int(stmt, 1, stageId);
                    if (sqlite3_step(stmt) == SQLITE_ROW) {
                        const auto* raw = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
                        auto parsed = nlohmann::json::parse(raw ? raw : "[]", nullptr, false);
                        sqlite3_finalize(stmt);
                        if (parsed.is_array()) return parsed;
                    } else {
                        sqlite3_finalize(stmt);
                    }
                }
            }

            return nlohmann::json::array();
        }
    }

    struct HttpMockBackend::Impl {
        std::mutex mutex;
        bool initialized = false;
        bool persistentStorageAvailable = false;
        sqlite3* db = nullptr;

        ~Impl() {
            if (db) {
                sqlite3_close(db);
                db = nullptr;
            }
        }

        bool EnsureReadyLocked();
        bool EnsureMuseumCatalogLocked();
        bool BackfillMissionHistoryLocked();
        bool EnsureMuseumGachaCardsLocked();
        std::vector<int> GetOwnedMuseumGachaCardsLocked(const MuseumGachaSeries& series);
        bool EnsureQuestSessionSchemaLocked();
        bool ResetLocked();
        bool RebuildLocked();
        std::optional<MockStoredResponse> GetArchiveDetailByIdLocked(std::string_view archivesId);
        std::optional<MockStoredResponse> GetHomeResponseLocked();
        std::optional<MockStoredResponse> GetMissionListResponseLocked();
        bool RecordMissionEventLocked(std::string_view sourceKey, std::string_view kind,
                                      int amount = 1, int detail = 0);
        std::optional<nlohmann::json> BuildProfileInfoLocked();
        std::optional<MockStoredResponse> GetProfileInfoResponseLocked();
        struct CardGrowthState {
            std::string dCardDatasId;
            int baseCardId = 0;
            int styleLevel = 0;
            int limitBreakTimes = 0;
            int evolveTimes = 0;
            const nlohmann::json* growth = nullptr;
        };
        std::optional<CardGrowthState> LoadCardGrowthLocked(std::string_view payloadJson);
        int64_t ItemHoldingLocked(int itemId);
        bool ConsumeItemLocked(int itemId, int64_t amount);
        nlohmann::json UserItemListLocked(const std::vector<int>& itemIds);
        std::optional<MockStoredResponse> CheckStyleLevelUpLocked(std::string_view payloadJson);
        std::optional<MockStoredResponse> StyleLevelUpLocked(std::string_view payloadJson);
        std::optional<MockStoredResponse> CheckLimitBreakLocked(std::string_view payloadJson);
        std::optional<MockStoredResponse> LimitBreakLocked(std::string_view payloadJson);
        std::optional<MockStoredResponse> CheckEvolutionLocked(std::string_view payloadJson);
        std::optional<MockStoredResponse> EvolutionLocked(std::string_view payloadJson);
        std::optional<MockStoredResponse> CheckSkillLevelUpLocked(std::string_view payloadJson);
        bool EnsureMyDesignTableLocked();
        std::optional<MockStoredResponse> GetMyDesignListLocked(int designType);
        std::optional<MockStoredResponse> SetMyDesignLocked(int designType, std::string_view payloadJson);
        std::optional<MockStoredResponse> SetMyDesignNameLocked(std::string_view payloadJson);
        std::optional<MockStoredResponse> SetProfileDesignLocked(int designType, std::string_view payloadJson);
        struct GachaState {
            int64_t freeJewels = 0;
            int64_t paidJewels = 0;
            int64_t exchangePoint = 0;
            int64_t drawCount = 0;
        };
        std::optional<GachaState> LoadGachaStateLocked(int seriesId = 1055003);
        std::optional<MockStoredResponse> GetGachaSeriesListResponseLocked();
        std::optional<MockStoredResponse> GachaSetPurchaseLocked(std::string_view payloadJson);
        std::optional<MockStoredResponse> GetGachaHistoryResponseLocked();
        std::optional<MockStoredResponse> GetGachaGuaranteePointListLocked(std::string_view payloadJson);
        std::optional<MockStoredResponse> GachaSetGuaranteePointExchangeLocked(std::string_view payloadJson);
        std::optional<MockStoredResponse> GetCardDetailByDCardIdLocked(std::string_view dCardDatasId);
        std::optional<MockStoredResponse> GetCardListResponseLocked();
        std::optional<MockStoredResponse> GetItemDetailByDItemIdLocked(std::string_view dItemDatasId);
        std::optional<MockStoredResponse> GetItemListResponseLocked();
        std::optional<MockStoredResponse> GetItemExchangeListResponseLocked(std::string_view payloadJson);
        std::optional<MockStoredResponse> ItemExchangeSetPurchaseLocked(std::string_view payloadJson);
        std::optional<MockStoredResponse> GetCollectionListResponseLocked(
            std::string_view querySql,
            std::string_view responseListKey
        );
        std::optional<MockStoredResponse> GetCharacterInfoByIdLocked(std::string_view characterId);
        std::optional<MockStoredResponse> GetDeckListResponseLocked();
        std::optional<MockStoredResponse> ModifyDeckListLocked(std::string_view payloadJson);
        static constexpr int kQuestLiveTypeStandard = 1;
        static constexpr int kQuestLiveTypeDaily = 2;
        struct QuestLiveState {
            std::string questLiveId;
            int questLiveType = 0;
            int stageId = 0;
            int musicId = 0;
            bool isChallengeMode = false;
            nlohmann::json deckData;
            nlohmann::json characterBonus;
            std::string startTime;
            int64_t score = 0;
            std::string playReport;
            bool finished = false;
            nlohmann::json rewardList = nlohmann::json::array();
        };
        QuestLiveState currentQuestLive;

        struct GradeSquareState {
            int squareId = 0;
            int squareType = 0;
            int targetId = 0;
            int actionPointCost = 0;
            int status = 0;
            int64_t livePoint = 0;
            std::vector<int> openSquareIds;
        };
        struct GradeQuestState {
            bool active = false;
            int seriesId = 0;
            int characterId = 0;
            int generation = 0;
            int season = 0;
            int actionPoint = 0;
            int currentSquareId = 0;
            int resourceValue = 0;
            std::vector<GradeSquareState> squares;
            std::vector<int> activeAddSkillIds;
            nlohmann::json rewardsJson;
        };
        GradeQuestState gradeQuest;

        std::optional<MockStoredResponse> QuestStageSelectLocked(std::string_view payloadJson);
        std::optional<MockStoredResponse> QuestStageDataLocked(std::string_view payloadJson);
        std::optional<MockStoredResponse> QuestGetLiveSettingLocked(std::string_view payloadJson);
        std::optional<MockStoredResponse> QuestSetLiveSettingLocked(std::string_view payloadJson);
        std::optional<MockStoredResponse> QuestSetStartLocked(std::string_view payloadJson);
        std::optional<MockStoredResponse> QuestGetLiveInfoLocked(std::string_view payloadJson);
        std::optional<MockStoredResponse> QuestSetFinishLocked(std::string_view payloadJson);
        std::optional<MockStoredResponse> QuestGetResultLocked(std::string_view payloadJson);
        std::optional<MockStoredResponse> QuestSkipLocked(std::string_view payloadJson);
        bool LoadQuestSessionLocked();
        bool PersistQuestSessionLocked(std::string_view state);

        std::optional<MockStoredResponse> DailyQuestStageListLocked(std::string_view payloadJson);
        std::optional<MockStoredResponse> DailyQuestStageSelectLocked();
        int64_t CurrentStaminaLocked(std::time_t* fullAt = nullptr);
        nlohmann::json StaminaJsonLocked();
        void ConsumeStaminaLocked(int64_t amount);
        int StageStaminaCostLocked(int questLiveType, int stageId);
        int64_t GrandPrixTotalPointLocked(int grandPrixId);
        nlohmann::json DailyTicketInfoLocked();
        bool ConsumeDailyTicketsLocked(int count);
        std::optional<MockStoredResponse> DailyGetRecoveryChallengeCountLocked();
        std::optional<MockStoredResponse> DailyRecoveryChallengeCountLocked(std::string_view payloadJson);
        nlohmann::json GrantStageDropsLocked(int stageId);
        nlohmann::json StageRewardPreviewLocked(int stageId);
        std::optional<MockStoredResponse> DailyQuestStageDataLocked(std::string_view payloadJson);

        std::optional<MockStoredResponse> MusicLearningGetMusicSelectLocked(std::string_view payloadJson);
        std::optional<MockStoredResponse> MusicLearningGetResultLocked(std::string_view payloadJson);

        std::optional<MockStoredResponse> DreamNotifyMemberReleaseConfirmLocked(std::string_view payloadJson);
        std::optional<MockStoredResponse> DreamGetResultLocked(std::string_view payloadJson);

        std::optional<MockStoredResponse> GradeGetQuestListLocked(std::string_view payloadJson);
        std::optional<MockStoredResponse> GradeSetQuestStartLocked(std::string_view payloadJson);
        std::optional<MockStoredResponse> GradeSetQuestActionLocked(std::string_view payloadJson);
        std::optional<MockStoredResponse> GradeSetQuestAddSkillLocked(std::string_view payloadJson);
        std::optional<MockStoredResponse> GradeGetStageDataLocked(std::string_view payloadJson);
        std::optional<MockStoredResponse> GradeGetResultLocked(std::string_view payloadJson);
        std::optional<MockStoredResponse> GradeSetQuestRetireLocked(std::string_view payloadJson);

        std::optional<MockStoredResponse> GrandPrixGetTopInfoLocked(std::string_view payloadJson);
        std::optional<MockStoredResponse> GrandPrixGetStageSelectLocked(std::string_view payloadJson);
        std::optional<MockStoredResponse> GrandPrixGetStageListLocked(std::string_view payloadJson);
        std::optional<MockStoredResponse> GrandPrixGetStageDataLocked(std::string_view payloadJson);
        std::optional<MockStoredResponse> GrandPrixGetRankingListLocked(std::string_view payloadJson);
        std::optional<MockStoredResponse> GrandPrixGetResultLocked(std::string_view payloadJson);

        std::optional<MockStoredResponse> GetRhythmGameHomeLocked();
        std::optional<MockStoredResponse> RhythmGameSetStartLocked(std::string_view payloadJson);
        std::optional<MockStoredResponse> RhythmGameSetFinishLocked(std::string_view payloadJson);
        std::optional<MockStoredResponse> ModifyRhythmGameDeckListLocked(std::string_view payloadJson);
    };

    namespace {
        static bool ExecSql(sqlite3* db, const char* sql) {
            char* errorMessage = nullptr;
            const int rc = sqlite3_exec(db, sql, nullptr, nullptr, &errorMessage);
            if (rc == SQLITE_OK) {
                return true;
            }

            Log::ErrorFmt("[HttpMockBackend] sqlite exec failed rc=%d err=%s",
                          rc,
                          errorMessage ? errorMessage : "(null)");
            if (errorMessage) {
                sqlite3_free(errorMessage);
            }
            return false;
        }

        static bool BindText(sqlite3_stmt* stmt, int index, std::string_view value) {
            return sqlite3_bind_text(stmt, index, value.data(), static_cast<int>(value.size()), SQLITE_TRANSIENT) == SQLITE_OK;
        }

        static std::string GachaHeaders(int64_t freeJewels, int64_t paidJewels) {
            std::string headers(OfflineApiMockBuiltIn::DefaultHeadersView);
            auto replace = [&headers](std::string_view original, std::string replacement) {
                const auto pos = headers.find(original);
                if (pos != std::string::npos) {
                    headers.replace(pos, original.size(), replacement);
                }
            };
            replace("jewel_free: 0", "jewel_free: " + std::to_string(freeJewels));
            replace("jewel_paid_google: 999999", "jewel_paid_google: " + std::to_string(paidJewels));
            replace("\"gacha_status\":0", "\"gacha_status\":1");
            return headers;
        }

        static std::string GenerateUuid4() {
            static thread_local std::mt19937_64 rng(std::random_device{}());
            std::uniform_int_distribution<uint64_t> dist;
            const uint64_t hi = dist(rng);
            const uint64_t lo = dist(rng);

            uint8_t bytes[16];
            for (int i = 0; i < 8; ++i) bytes[i] = static_cast<uint8_t>(hi >> (56 - i * 8));
            for (int i = 0; i < 8; ++i) bytes[8 + i] = static_cast<uint8_t>(lo >> (56 - i * 8));

            bytes[6] = (bytes[6] & 0x0F) | 0x40;
            bytes[8] = (bytes[8] & 0x3F) | 0x80;

            char buf[37];
            snprintf(buf, sizeof(buf),
                     "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
                     bytes[0], bytes[1], bytes[2], bytes[3],
                     bytes[4], bytes[5], bytes[6], bytes[7],
                     bytes[8], bytes[9], bytes[10], bytes[11],
                     bytes[12], bytes[13], bytes[14], bytes[15]);
            return std::string(buf, 36);
        }

        static bool ExecSqlScript(sqlite3* db, const HttpMockBackendBuiltInSql::BuiltinSqlScript& script) {
            const std::string sql(script.sql);
            if (sql.empty()) {
                return true;
            }

            if (ExecSql(db, sql.c_str())) {
                return true;
            }

            Log::ErrorFmt("[HttpMockBackend] failed builtin SQL script: %.*s",
                          static_cast<int>(script.path.size()),
                          script.path.data());
            return false;
        }

        template <size_t N>
        static bool ExecBuiltInSqlScripts(sqlite3* db,
                                          const std::array<HttpMockBackendBuiltInSql::BuiltinSqlScript, N>& scripts,
                                          std::string_view groupName) {
            for (const auto& script : scripts) {
                if (!ExecSqlScript(db, script)) {
                    Log::ErrorFmt("[HttpMockBackend] builtin SQL group failed: %.*s",
                                  static_cast<int>(groupName.size()),
                                  groupName.data());
                    return false;
                }
            }
            return true;
        }

        static bool TableHasAnyRows(sqlite3* db, const char* tableName) {
            sqlite3_stmt* stmt = nullptr;
            const std::string sql = "SELECT EXISTS(SELECT 1 FROM " + std::string(tableName) + " LIMIT 1);";
            if (sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK || !stmt) {
                Log::ErrorFmt("[HttpMockBackend] sqlite prepare failed for table probe: %s", sqlite3_errmsg(db));
                if (stmt) {
                    sqlite3_finalize(stmt);
                }
                return false;
            }

            const int rc = sqlite3_step(stmt);
            const bool hasRows = (rc == SQLITE_ROW) && (sqlite3_column_int(stmt, 0) != 0);
            sqlite3_finalize(stmt);
            return hasRows;
        }

        static bool InsertCollectionSeed(sqlite3* db,
                                         const nlohmann::json& source,
                                         std::string_view listKey,
                                         std::string_view idKey,
                                         const char* sql) {
            const auto list = source.find(std::string(listKey));
            if (list == source.end() || !list->is_array()) {
                Log::ErrorFmt("[HttpMockBackend] invalid built-in collection seed: %.*s",
                              static_cast<int>(listKey.size()), listKey.data());
                return false;
            }
            sqlite3_stmt* stmt = nullptr;
            if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK || !stmt) {
                if (stmt) sqlite3_finalize(stmt);
                Log::ErrorFmt("[HttpMockBackend] collection seed prepare failed: %s", sqlite3_errmsg(db));
                return false;
            }
            for (const auto& entry : *list) {
                if (!entry.is_object() || !entry.contains(std::string(idKey)) ||
                    !entry[std::string(idKey)].is_number_integer()) {
                    sqlite3_finalize(stmt);
                    Log::ErrorFmt("[HttpMockBackend] collection seed entry lacks integer id: %.*s",
                                  static_cast<int>(idKey.size()), idKey.data());
                    return false;
                }
                sqlite3_reset(stmt);
                sqlite3_clear_bindings(stmt);
                const auto id = entry[std::string(idKey)].get<int64_t>();
                const auto json = entry.dump();
                if (sqlite3_bind_int64(stmt, 1, id) != SQLITE_OK ||
                    !BindText(stmt, 2, json) || sqlite3_step(stmt) != SQLITE_DONE) {
                    sqlite3_finalize(stmt);
                    Log::ErrorFmt("[HttpMockBackend] collection seed insert failed: %s", sqlite3_errmsg(db));
                    return false;
                }
            }
            sqlite3_finalize(stmt);
            return true;
        }

        // Growth rules from tools/generate_card_growth_catalog.py (Master data).
        const nlohmann::json& CardGrowthCatalog() {
            static const nlohmann::json catalog = OfflineApiMockBuiltIn::CardGrowthCatalogJsonObj();
            return catalog;
        }

        const nlohmann::json* FindCardGrowth(int baseCardId) {
            const auto& catalog = CardGrowthCatalog();
            if (!catalog.is_object() || !catalog.contains("cards")) return nullptr;
            const auto& cards = catalog["cards"];
            const auto it = cards.find(std::to_string(baseCardId));
            return it == cards.end() ? nullptr : &*it;
        }

        int CardGrowthMaxEvolve(const nlohmann::json& growth) {
            const auto it = growth.find("evolution");
            return it != growth.end() && it->is_array() ? static_cast<int>(it->size()) : 0;
        }

        int CardGrowthStyleCap(const nlohmann::json& growth, int evolveTimes) {
            const auto& caps = CardGrowthCatalog()["rarity_caps"];
            const auto it = caps.find(std::to_string(growth.value("rarity", 0)));
            if (it == caps.end() || !it->is_array() || it->empty()) return 0;
            const int index = std::clamp(evolveTimes, 0, static_cast<int>(it->size()) - 1);
            return (*it)[index].get<int>();
        }

        int64_t CardGrowthCumulativeStylePoint(const nlohmann::json& growth, int level) {
            const auto& levels = CardGrowthCatalog()["levels"];
            const auto it = levels.find(std::to_string(growth.value("experience_type", 0)));
            if (it == levels.end() || !it->is_array() || level < 0 ||
                level >= static_cast<int>(it->size())) return -1;
            return (*it)[level].get<int64_t>();
        }

        static std::optional<nlohmann::json> ComposeOwnedCardResponse(
                const char* responseJson,
                int styleLevel,
                int limitBreakTimes,
                bool isEvolveMax,
                int evolveTimes) {
            auto response = nlohmann::json::parse(responseJson ? responseJson : "{}", nullptr, false);
            if (!response.is_object()) return std::nullopt;
            auto card = response.find("user_card_data");
            if (card == response.end() || !card->is_object()) return std::nullopt;

            // Catalog response_json provides the verified 5.1.0 DTO shape and
            // immutable card facts. These fields are player save state and must
            // always come from museum_card_state so list/detail/deck agree.
            (*card)["style_level"] = styleLevel;
            (*card)["limit_break_times"] = limitBreakTimes;
            (*card)["is_evolve_max"] = isEvolveMax;
            // Base cards (from the gacha) can grow. An evolved card is the
            // Master id base + evolve count; its style cap follows the rarity
            // and stage. The preserved collection is fully developed and has
            // no catalog entry, so its stored values are kept.
            const int baseCardId = card->value("card_datas_id", 0);
            if (const auto* growth = FindCardGrowth(baseCardId)) {
                const int maxEvolve = CardGrowthMaxEvolve(*growth);
                const int stage = std::clamp(evolveTimes, 0, maxEvolve);
                (*card)["card_datas_id"] = baseCardId + stage;
                (*card)["max_style_level"] = CardGrowthStyleCap(*growth, stage);
                (*card)["is_evolve_max"] = stage >= maxEvolve;
                (*card)["is_evolve_possible"] = stage < maxEvolve;
            }
            return response;
        }
    }

    bool HttpMockBackend::Impl::EnsureReadyLocked() {
        if (initialized) {
            return true;
        }
        initialized = true;

        const auto dbPath = GetBackendDatabasePath();
        std::error_code ec;
        std::filesystem::create_directories(dbPath.parent_path(), ec);
        if (ec) {
            Log::WarnFmt("[HttpMockBackend] failed to create db directory: %s", ec.message().c_str());
        }

        const auto dbPathString = dbPath.string();
        const int openRc = sqlite3_open_v2(
            dbPathString.c_str(),
            &db,
            SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX,
            nullptr);
        if (openRc != SQLITE_OK || !db) {
            Log::ErrorFmt("[HttpMockBackend] sqlite open failed rc=%d path=%s", openRc, dbPathString.c_str());
            if (db) {
                sqlite3_close(db);
                db = nullptr;
            }
            persistentStorageAvailable = false;
            return false;
        }

        persistentStorageAvailable = true;

        if (!ExecBuiltInSqlScripts(db, HttpMockBackendBuiltInSql::SchemaScripts, "schema")) {
            persistentStorageAvailable = false;
            return false;
        }
        if (!EnsureQuestSessionSchemaLocked()) {
            persistentStorageAvailable = false;
            return false;
        }

        {
            bool hasColumn = false;
            sqlite3_stmt* pi = nullptr;
            if (sqlite3_prepare_v2(db, "PRAGMA table_info(grand_prix_stage);", -1, &pi, nullptr) == SQLITE_OK && pi) {
                while (sqlite3_step(pi) == SQLITE_ROW) {
                    const auto* col = reinterpret_cast<const char*>(sqlite3_column_text(pi, 1));
                    if (col && std::string(col) == "section_skills_json") { hasColumn = true; break; }
                }
                sqlite3_finalize(pi);
            }
            if (!hasColumn) {
                ExecSql(db, "DROP TABLE IF EXISTS grand_prix_stage;");
                ExecSql(db, "DROP TABLE IF EXISTS grand_prix_series;");
                ExecSql(db, "DROP TABLE IF EXISTS grand_prix;");
                ExecSql(db, "DROP TABLE IF EXISTS grand_prix_progress;");
                ExecBuiltInSqlScripts(db, HttpMockBackendBuiltInSql::SchemaScripts, "schema");
            }
        }

        const auto resetCmdPath = dbPath.parent_path() / "mock_db_reset_cmd";
        bool hasPendingReset = std::filesystem::exists(resetCmdPath, ec);
        if (hasPendingReset) {
            Log::InfoFmt("[HttpMockBackend] pending reset command found, re-seeding");
            ExecSql(db, "DELETE FROM archive_detail;");
            ExecSql(db, HttpMockBackendBuiltInSql::QueriesResetMuseumStateSqlView.data());
            ExecSql(db, "DELETE FROM card_detail;");
            ExecSql(db, "DELETE FROM collection_music;");
            ExecSql(db, "DELETE FROM collection_sticker;");
            ExecSql(db, "DELETE FROM collection_gallary;");
            ExecSql(db, "DELETE FROM character_info;");
            ExecSql(db, "DELETE FROM item;");
            ExecSql(db, "DELETE FROM deck;");
            ExecSql(db, "DELETE FROM rhythm_music_score;");
            ExecSql(db, "DELETE FROM rhythm_game_deck;");
            ExecSql(db, "DELETE FROM rhythm_runtime_session;");
            ExecSql(db, "DELETE FROM quest_runtime_session;");
            ExecSql(db, "DELETE FROM quest_stage;");
            ExecSql(db, "DELETE FROM daily_quest_stage;");
            ExecSql(db, "DELETE FROM dream_quest_stage;");
            ExecSql(db, "DELETE FROM grade_quest_season;");
            ExecSql(db, "DELETE FROM grade_quest_series;");
            ExecSql(db, "DELETE FROM grade_quest_stage;");
            ExecSql(db, "DELETE FROM grade_add_skill;");
            ExecSql(db, "DELETE FROM grade_quest_progress;");
            ExecSql(db, "DELETE FROM grand_prix_progress;");
            ExecSql(db, "DELETE FROM grand_prix;");
            ExecSql(db, "DELETE FROM grand_prix_series;");
            ExecSql(db, "DELETE FROM grand_prix_stage;");
            ExecSql(db, "DELETE FROM learning_stage;");
            ExecSql(db, "DELETE FROM music_mastery;");
            ExecSql(db, "DELETE FROM music;");
            std::filesystem::remove(resetCmdPath, ec);
        }

        if ((hasPendingReset || !TableHasAnyRows(db, "archive_detail") || !TableHasAnyRows(db, "card_detail") || !TableHasAnyRows(db, "character_info") || !TableHasAnyRows(db, "item") || !TableHasAnyRows(db, "quest_stage") || !TableHasAnyRows(db, "daily_quest_stage") || !TableHasAnyRows(db, "dream_quest_stage") || !TableHasAnyRows(db, "grade_quest_stage") || !TableHasAnyRows(db, "grade_add_skill") || !TableHasAnyRows(db, "grand_prix_stage") || !TableHasAnyRows(db, "learning_stage") || !TableHasAnyRows(db, "music_mastery") || !TableHasAnyRows(db, "music"))
            && !ExecBuiltInSqlScripts(db, HttpMockBackendBuiltInSql::SeedScripts, "seed")) {
            persistentStorageAvailable = false;
            return false;
        }

        if (!EnsureMuseumCatalogLocked()) {
            persistentStorageAvailable = false;
            return false;
        }

        if (!BackfillMissionHistoryLocked()) {
            persistentStorageAvailable = false;
            return false;
        }

        return true;
    }

    bool HttpMockBackend::Impl::BackfillMissionHistoryLocked() {
        if (!db || !ExecSql(db, "BEGIN IMMEDIATE;")) return false;
        sqlite3_stmt* insert = nullptr;
        if (sqlite3_prepare_v2(db,
                "INSERT OR IGNORE INTO offline_mission_event "
                "(source_key,event_kind,amount,detail,occurred_at) VALUES (?,'CARD_OBTAIN',?,0,?);",
                -1, &insert, nullptr) != SQLITE_OK || !insert) {
            if (insert) sqlite3_finalize(insert);
            ExecSql(db, "ROLLBACK;");
            return false;
        }
        const auto import = [this, insert](const char* sql, std::string_view prefix,
                                           bool isDraw) -> bool {
            sqlite3_stmt* rows = nullptr;
            if (sqlite3_prepare_v2(db, sql, -1, &rows, nullptr) != SQLITE_OK || !rows) {
                if (rows) sqlite3_finalize(rows);
                return false;
            }
            bool ok = true;
            while (sqlite3_step(rows) == SQLITE_ROW) {
                const auto* rawDate = reinterpret_cast<const char*>(sqlite3_column_text(rows, 1));
                const auto* rawResults = isDraw
                    ? reinterpret_cast<const char*>(sqlite3_column_text(rows, 2)) : nullptr;
                const auto parsed = isDraw
                    ? nlohmann::json::parse(rawResults ? rawResults : "[]", nullptr, false)
                    : nlohmann::json::array();
                const int amount = isDraw && parsed.is_array() ? static_cast<int>(parsed.size()) : 1;
                if (!rawDate || amount <= 0) continue;
                sqlite3_reset(insert);
                sqlite3_clear_bindings(insert);
                ok = BindText(insert, 1, std::string(prefix) + std::to_string(sqlite3_column_int64(rows, 0))) &&
                     sqlite3_bind_int(insert, 2, amount) == SQLITE_OK &&
                     BindText(insert, 3, rawDate) && sqlite3_step(insert) == SQLITE_DONE;
                if (!ok) break;
            }
            sqlite3_finalize(rows);
            return ok;
        };
        const bool ok = import(
            "SELECT draw_id, drawn_at, results_json FROM museum_gacha_history;",
            "gacha-draw:", true) && import(
            "SELECT exchange_id, exchanged_at FROM museum_gacha_exchange_history;",
            "gacha-exchange:", false);
        sqlite3_finalize(insert);
        if (!ok || !ExecSql(db, "COMMIT;")) {
            ExecSql(db, "ROLLBACK;");
            return false;
        }
        return true;
    }

    bool HttpMockBackend::Impl::EnsureMuseumCatalogLocked() {
        if (!db) return false;

        const bool needsMusic = !TableHasAnyRows(db, "collection_music");
        const bool needsSticker = !TableHasAnyRows(db, "collection_sticker");
        const bool needsGallary = !TableHasAnyRows(db, "collection_gallary");
        if (!ExecSql(db, "BEGIN IMMEDIATE;")) return false;

        auto rollback = [this]() {
            ExecSql(db, "ROLLBACK;");
            return false;
        };

        if (needsMusic &&
            !InsertCollectionSeed(db, OfflineApiMockBuiltIn::CollectionGetMusicListJsonObj(),
                                  "music_info_list", "musics_id",
                                  "INSERT OR IGNORE INTO collection_music(musics_id, response_json) VALUES (?, ?);")) {
            return rollback();
        }
        if (needsSticker &&
            !InsertCollectionSeed(db, OfflineApiMockBuiltIn::CollectionGetStickerListJsonObj(),
                                  "sticker_info_list", "stickers_id",
                                  "INSERT OR IGNORE INTO collection_sticker(stickers_id, response_json) VALUES (?, ?);")) {
            return rollback();
        }
        if (needsGallary &&
            !InsertCollectionSeed(db, OfflineApiMockBuiltIn::CollectionGetGallaryListJsonObj(),
                                  "gallary_info_list", "comics_id",
                                  "INSERT OR IGNORE INTO collection_gallary(comics_id, response_json) VALUES (?, ?);")) {
            return rollback();
        }
        if (!EnsureMuseumGachaCardsLocked()) {
            return rollback();
        }
        if (!ExecBuiltInSqlScripts(db, HttpMockBackendBuiltInSql::PlayerStateScripts, "player-state")) {
            return rollback();
        }
        return ExecSql(db, "COMMIT;") || rollback();
    }

    bool HttpMockBackend::Impl::EnsureMuseumGachaCardsLocked() {
        if (!db) return false;

        // The checked-in Museum seed contains fully developed CardDatas rows
        // for Collection.  Gacha must instead hand the original client the
        // base records, which have different MasterData animation bindings.
        // Clone the verified DTO shape from the matching developed row once;
        // only player-owned state is later written by the purchase transaction.
        constexpr const char* sourceSql =
            "SELECT response_json FROM card_detail WHERE card_datas_id=? LIMIT 1;";
        constexpr const char* insertSql =
            "INSERT OR IGNORE INTO card_detail ("
            "d_card_datas_id,response_json,card_datas_id,card_name,style_level,max_style_level,"
            "limit_break_times,max_limit_break_times,card_parameters,skill_list,character_id,"
            "generations_id,series_type,card_sort_order,character_bonus,is_evolve_possible,"
            "is_evolve_max,member_fan_level,is_limit_break,is_style_level_up,rhythm_game_skill_list"
            ") SELECT ?,?, ?,card_name,1,max_style_level,0,max_limit_break_times,card_parameters,"
            "skill_list,character_id,generations_id,series_type,card_sort_order,character_bonus,"
            "1,0,member_fan_level,0,0,rhythm_game_skill_list "
            "FROM card_detail WHERE card_datas_id=?;";

        sqlite3_stmt* source = nullptr;
        sqlite3_stmt* insert = nullptr;
        if (sqlite3_prepare_v2(db, sourceSql, -1, &source, nullptr) != SQLITE_OK ||
            sqlite3_prepare_v2(db, insertSql, -1, &insert, nullptr) != SQLITE_OK) {
            if (source) sqlite3_finalize(source);
            if (insert) sqlite3_finalize(insert);
            Log::ErrorFmt("[HttpMockBackend] gacha base-card catalog prepare failed: %s",
                          sqlite3_errmsg(db));
            return false;
        }

        for (const auto& series : kMuseumGachaSeries) {
            for (const int cardId : series.cards) {
                const int sourceCardId = cardId + 4;
                sqlite3_reset(source);
                sqlite3_clear_bindings(source);
                sqlite3_bind_int(source, 1, sourceCardId);
                if (sqlite3_step(source) != SQLITE_ROW) {
                    Log::ErrorFmt("[HttpMockBackend] gacha base card %d lacks Museum source %d",
                                  cardId, sourceCardId);
                    sqlite3_finalize(source);
                    sqlite3_finalize(insert);
                    return false;
                }
                const auto* raw = reinterpret_cast<const char*>(sqlite3_column_text(source, 0));
                auto dto = nlohmann::json::parse(raw ? raw : "{}", nullptr, false);
                if (!dto.is_object() || !dto.contains("user_card_data") ||
                    !dto["user_card_data"].is_object()) {
                    Log::ErrorFmt("[HttpMockBackend] gacha source card %d has invalid DTO", sourceCardId);
                    sqlite3_finalize(source);
                    sqlite3_finalize(insert);
                    return false;
                }

                char dCardId[37]{};
                std::snprintf(dCardId, sizeof(dCardId),
                              "00000000-0000-0000-0000-%012d", cardId);
                auto& card = dto["user_card_data"];
                card["d_card_datas_id"] = dCardId;
                card["card_datas_id"] = cardId;
                card["style_level"] = 1;
                card["limit_break_times"] = 0;
                card["is_evolve_max"] = false;
                card["is_limit_break"] = false;
                card["is_style_level_up"] = false;

                sqlite3_reset(insert);
                sqlite3_clear_bindings(insert);
                if (!BindText(insert, 1, dCardId) || !BindText(insert, 2, dto.dump()) ||
                    sqlite3_bind_int(insert, 3, cardId) != SQLITE_OK ||
                    sqlite3_bind_int(insert, 4, sourceCardId) != SQLITE_OK ||
                    sqlite3_step(insert) != SQLITE_DONE) {
                    Log::ErrorFmt("[HttpMockBackend] gacha base-card catalog insert failed: %s",
                                  sqlite3_errmsg(db));
                    sqlite3_finalize(source);
                    sqlite3_finalize(insert);
                    return false;
                }
            }
        }
        sqlite3_finalize(source);
        sqlite3_finalize(insert);
        return true;
    }

    std::vector<int> HttpMockBackend::Impl::GetOwnedMuseumGachaCardsLocked(
            const MuseumGachaSeries& series) {
        std::vector<int> cards;
        if (!db) return cards;
        sqlite3_stmt* stmt = nullptr;
        constexpr const char* sql =
            "SELECT 1 FROM card_detail INNER JOIN museum_card_ownership "
            "USING(d_card_datas_id) WHERE card_datas_id=? LIMIT 1;";
        if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK || !stmt) {
            if (stmt) sqlite3_finalize(stmt);
            return cards;
        }
        for (const int cardId : series.cards) {
            sqlite3_reset(stmt);
            sqlite3_clear_bindings(stmt);
            sqlite3_bind_int(stmt, 1, cardId);
            if (sqlite3_step(stmt) == SQLITE_ROW) cards.push_back(cardId);
        }
        sqlite3_finalize(stmt);
        return cards;
    }

    bool HttpMockBackend::Impl::EnsureQuestSessionSchemaLocked() {
        if (!db) {
            return false;
        }

        bool tableExists = false;
        bool supportsRecovered = false;
        sqlite3_stmt* stmt = nullptr;
        constexpr const char* inspectSql =
            "SELECT sql FROM sqlite_master WHERE type = 'table' AND name = 'quest_runtime_session';";
        if (sqlite3_prepare_v2(db, inspectSql, -1, &stmt, nullptr) != SQLITE_OK || !stmt) {
            Log::ErrorFmt("[HttpMockBackend] quest session schema inspection failed: %s", sqlite3_errmsg(db));
            if (stmt) sqlite3_finalize(stmt);
            return false;
        }
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            tableExists = true;
            const auto* rawSql = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
            supportsRecovered = rawSql && std::string_view(rawSql).find("RECOVERED") != std::string_view::npos;
        }
        sqlite3_finalize(stmt);
        if (!tableExists || supportsRecovered) {
            return true;
        }

        if (!ExecSql(db, "BEGIN IMMEDIATE;")) {
            return false;
        }
        const bool migrated =
            ExecSql(db, "ALTER TABLE quest_runtime_session RENAME TO quest_runtime_session_legacy;") &&
            ExecBuiltInSqlScripts(db, HttpMockBackendBuiltInSql::SchemaScripts, "schema") &&
            ExecSql(db,
                    "INSERT INTO quest_runtime_session "
                    "(singleton_id, session_id, quest_live_type, stage_id, music_id, is_challenge_mode, "
                    "state, start_payload_json, finish_payload_json, start_time, updated_at) "
                    "SELECT singleton_id, session_id, quest_live_type, stage_id, music_id, is_challenge_mode, "
                    "CASE WHEN state = 'PLAYING' THEN 'RECOVERED' ELSE state END, "
                    "start_payload_json, finish_payload_json, start_time, updated_at "
                    "FROM quest_runtime_session_legacy;") &&
            ExecSql(db, "DROP TABLE quest_runtime_session_legacy;");
        if (!migrated || !ExecSql(db, "COMMIT;")) {
            ExecSql(db, "ROLLBACK;");
            return false;
        }
        return true;
    }

    bool HttpMockBackend::Impl::ResetLocked() {
        if (!EnsureReadyLocked() || !db) {
            return false;
        }

        if (!ExecSql(db, HttpMockBackendBuiltInSql::QueriesResetMuseumStateSqlView.data())) {
            return false;
        }
        if (!ExecSql(db, "DELETE FROM offline_mission_event;") ||
            !ExecSql(db, "DELETE FROM offline_mission_award;") ||
            !ExecSql(db, "DELETE FROM offline_mission_meta;") ||
            !ExecSql(db, "DELETE FROM archive_detail;") || !ExecSql(db, "DELETE FROM card_detail;") || !ExecSql(db, "DELETE FROM character_info;") || !ExecSql(db, "DELETE FROM item;") || !ExecSql(db, "DELETE FROM deck;") || !ExecSql(db, "DELETE FROM rhythm_music_score;") || !ExecSql(db, "DELETE FROM rhythm_game_deck;") || !ExecSql(db, "DELETE FROM rhythm_runtime_session;") || !ExecSql(db, "DELETE FROM quest_runtime_session;") || !ExecSql(db, "DELETE FROM quest_stage;") || !ExecSql(db, "DELETE FROM daily_quest_stage;") || !ExecSql(db, "DELETE FROM dream_quest_stage;") || !ExecSql(db, "DELETE FROM grade_quest_season;") || !ExecSql(db, "DELETE FROM grade_quest_series;") || !ExecSql(db, "DELETE FROM grade_quest_stage;") || !ExecSql(db, "DELETE FROM grade_add_skill;") || !ExecSql(db, "DELETE FROM grade_quest_progress;") || !ExecSql(db, "DELETE FROM grand_prix_progress;") || !ExecSql(db, "DELETE FROM grand_prix;") || !ExecSql(db, "DELETE FROM grand_prix_series;") || !ExecSql(db, "DELETE FROM grand_prix_stage;") || !ExecSql(db, "DELETE FROM learning_stage;") || !ExecSql(db, "DELETE FROM music_mastery;") || !ExecSql(db, "DELETE FROM music;")) {
            return false;
        }

        return ExecBuiltInSqlScripts(db, HttpMockBackendBuiltInSql::SeedScripts, "seed") &&
               EnsureMuseumCatalogLocked() && BackfillMissionHistoryLocked();
    }

    bool HttpMockBackend::Impl::RebuildLocked() {
        if (db) {
            sqlite3_close(db);
            db = nullptr;
        }

        const auto dbPath = GetBackendDatabasePath();
        std::error_code ec;
        std::filesystem::remove(dbPath, ec);
        if (ec) {
            Log::WarnFmt("[HttpMockBackend] failed to remove db file: %s", ec.message().c_str());
        }

        initialized = false;
        persistentStorageAvailable = false;
        return EnsureReadyLocked();
    }

    std::optional<MockStoredResponse> HttpMockBackend::Impl::GetArchiveDetailByIdLocked(std::string_view archivesId) {
        if (!initialized && !EnsureReadyLocked()) {
            return std::nullopt;
        }

        if (!db || archivesId.empty()) {
            return std::nullopt;
        }

        sqlite3_stmt* stmt = nullptr;
        constexpr const char* sql =
            "SELECT response_json "
            "FROM archive_detail "
            "WHERE archives_id = ?;";
        if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK || !stmt) {
            Log::ErrorFmt("[HttpMockBackend] sqlite prepare failed for archive_detail query: %s", sqlite3_errmsg(db));
            if (stmt) sqlite3_finalize(stmt);
            return std::nullopt;
        }

        if (!BindText(stmt, 1, archivesId)) {
            Log::ErrorFmt("[HttpMockBackend] sqlite bind failed for archive_detail query: %s", sqlite3_errmsg(db));
            sqlite3_finalize(stmt);
            return std::nullopt;
        }

        const int rc = sqlite3_step(stmt);
        if (rc != SQLITE_ROW) {
            sqlite3_finalize(stmt);
            return std::nullopt;
        }

        const auto* responseJson = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        MockStoredResponse response;
        response.body = responseJson ? responseJson : "{}";
        sqlite3_finalize(stmt);
        return response;
    }

    std::optional<MockStoredResponse> HttpMockBackend::Impl::GetCardDetailByDCardIdLocked(std::string_view dCardDatasId) {
        if (!initialized && !EnsureReadyLocked()) {
            return std::nullopt;
        }

        if (!db || dCardDatasId.empty()) {
            return std::nullopt;
        }

        sqlite3_stmt* stmt = nullptr;
        constexpr const char* sql =
            "SELECT card_detail.response_json, museum_card_state.style_level, "
            "museum_card_state.limit_break_times, museum_card_state.is_evolve_max, "
            "COALESCE(offline_card_growth.evolve_times, 0) "
            "FROM card_detail "
            "INNER JOIN museum_card_ownership "
            "ON museum_card_ownership.d_card_datas_id = card_detail.d_card_datas_id "
            "INNER JOIN museum_card_state "
            "ON museum_card_state.d_card_datas_id = museum_card_ownership.d_card_datas_id "
            "LEFT JOIN offline_card_growth "
            "ON offline_card_growth.d_card_datas_id = card_detail.d_card_datas_id "
            "WHERE card_detail.d_card_datas_id = ?;";
        if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK || !stmt) {
            Log::ErrorFmt("[HttpMockBackend] sqlite prepare failed for card_detail query: %s", sqlite3_errmsg(db));
            if (stmt) sqlite3_finalize(stmt);
            return std::nullopt;
        }

        if (!BindText(stmt, 1, dCardDatasId)) {
            Log::ErrorFmt("[HttpMockBackend] sqlite bind failed for card_detail query: %s", sqlite3_errmsg(db));
            sqlite3_finalize(stmt);
            return std::nullopt;
        }

        const int rc = sqlite3_step(stmt);
        if (rc != SQLITE_ROW) {
            sqlite3_finalize(stmt);
            return std::nullopt;
        }

        const auto* responseJson = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        auto composed = ComposeOwnedCardResponse(
            responseJson,
            sqlite3_column_int(stmt, 1),
            sqlite3_column_int(stmt, 2),
            sqlite3_column_int(stmt, 3) != 0,
            sqlite3_column_int(stmt, 4));
        if (!composed) {
            Log::Error("[HttpMockBackend] invalid catalog card response while composing detail");
            sqlite3_finalize(stmt);
            return std::nullopt;
        }
        MockStoredResponse response;
        response.body = composed->dump();
        sqlite3_finalize(stmt);
        return response;
    }

    std::optional<MockStoredResponse> HttpMockBackend::Impl::GetCardListResponseLocked() {
        if (!initialized && !EnsureReadyLocked()) {
            return std::nullopt;
        }
        if (!db) {
            return std::nullopt;
        }

        sqlite3_stmt* stmt = nullptr;
        constexpr const char* sql =
            "SELECT card_detail.response_json, museum_card_state.style_level, "
            "museum_card_state.limit_break_times, museum_card_state.is_evolve_max, "
            "COALESCE(offline_card_growth.evolve_times, 0) "
            "FROM card_detail "
            "INNER JOIN museum_card_ownership "
            "ON museum_card_ownership.d_card_datas_id = card_detail.d_card_datas_id "
            "INNER JOIN museum_card_state "
            "ON museum_card_state.d_card_datas_id = museum_card_ownership.d_card_datas_id "
            "LEFT JOIN offline_card_growth "
            "ON offline_card_growth.d_card_datas_id = card_detail.d_card_datas_id "
            "ORDER BY card_detail.card_datas_id;";
        if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK || !stmt) {
            Log::ErrorFmt("[HttpMockBackend] sqlite prepare failed for card list: %s", sqlite3_errmsg(db));
            if (stmt) sqlite3_finalize(stmt);
            return std::nullopt;
        }

        nlohmann::json cards = nlohmann::json::array();
        int rc = SQLITE_ROW;
        while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
            const auto* responseJson = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
            auto parsed = ComposeOwnedCardResponse(
                responseJson,
                sqlite3_column_int(stmt, 1),
                sqlite3_column_int(stmt, 2),
                sqlite3_column_int(stmt, 3) != 0,
                sqlite3_column_int(stmt, 4));
            if (!parsed) {
                Log::ErrorFmt("[HttpMockBackend] invalid card_detail response_json while building list");
                sqlite3_finalize(stmt);
                return std::nullopt;
            }
            cards.push_back(std::move((*parsed)["user_card_data"]));
        }
        if (rc != SQLITE_DONE) {
            Log::ErrorFmt("[HttpMockBackend] sqlite card list step failed: %s", sqlite3_errmsg(db));
            sqlite3_finalize(stmt);
            return std::nullopt;
        }
        sqlite3_finalize(stmt);

        nlohmann::json result;
        result["user_card_data_list"] = std::move(cards);
        return MockStoredResponse{result.dump(), {}, 200, "OK (offline mock)"};
    }

    std::optional<HttpMockBackend::Impl::GachaState> HttpMockBackend::Impl::LoadGachaStateLocked(int seriesId) {
        if (!initialized && !EnsureReadyLocked()) return std::nullopt;
        if (!db || !FindMuseumGachaSeries(seriesId)) return std::nullopt;
        if (!ExecSql(db, "BEGIN IMMEDIATE;")) return std::nullopt;
        if (!ExecSql(db,
            "INSERT OR IGNORE INTO museum_gacha_state "
            "(series_id, free_jewels, paid_jewels, exchange_point, draw_count) "
            "VALUES (1055003, 0, 999999, 0, 0);")) {
            ExecSql(db, "ROLLBACK;");
            return std::nullopt;
        }
        const bool initializedNow = sqlite3_changes(db) == 1;
        if (initializedNow &&
            (!ExecSql(db,
                "UPDATE museum_item_inventory SET item_num=999999 WHERE d_item_datas_id="
                "(SELECT d_item_datas_id FROM item WHERE item_id=1001001 LIMIT 1);") ||
             !ExecSql(db,
                "UPDATE museum_item_inventory SET item_num=0 WHERE d_item_datas_id="
                "(SELECT d_item_datas_id FROM item WHERE item_id=1002001 LIMIT 1);"))) {
            ExecSql(db, "ROLLBACK;");
            return std::nullopt;
        }
        if (!ExecSql(db,
                "INSERT OR IGNORE INTO museum_gacha_state "
                "(series_id, free_jewels, paid_jewels, exchange_point, draw_count) "
                "VALUES (1055001, 0, 0, 0, 0), (1055002, 0, 0, 0, 0), "
                "(1050505, 0, 0, 0, 0), (1050506, 0, 0, 0, 0), "
                "(1050509, 0, 0, 0, 0), (1050511, 0, 0, 0, 0);")) {
            ExecSql(db, "ROLLBACK;");
            return std::nullopt;
        }
        if (!ExecSql(db, "COMMIT;")) {
            ExecSql(db, "ROLLBACK;");
            return std::nullopt;
        }
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(db,
                "SELECT wallet.free_jewels, wallet.paid_jewels, points.exchange_point, series.draw_count "
                "FROM museum_gacha_state AS wallet JOIN museum_gacha_state AS series "
                "ON series.series_id=? JOIN museum_gacha_state AS points ON points.series_id=? "
                "WHERE wallet.series_id=1055003;",
                -1, &stmt, nullptr) != SQLITE_OK || !stmt) {
            if (stmt) sqlite3_finalize(stmt);
            return std::nullopt;
        }
        sqlite3_bind_int(stmt, 1, seriesId);
        sqlite3_bind_int(stmt, 2, PointOwnerSeriesId(*FindMuseumGachaSeries(seriesId)));
        std::optional<GachaState> state;
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            state = GachaState{
                sqlite3_column_int64(stmt, 0),
                sqlite3_column_int64(stmt, 1),
                sqlite3_column_int64(stmt, 2),
                sqlite3_column_int64(stmt, 3),
            };
        }
        sqlite3_finalize(stmt);
        return state;
    }

    namespace {
        constexpr int kMyDesignTypeCard = 1;
        constexpr int kMyDesignTypeIcon = 2;
        constexpr int kMyDesignSlotsPerType = 10;
        std::string JsonStringField(const nlohmann::json& payload, const char* key) {
            if (!payload.is_object() || !payload.contains(key)) return {};
            const auto& value = payload[key];
            return value.is_string() ? value.get<std::string>() : value.dump();
        }
    }

    // Home and the Profile scene both read the same ProfileInfo DTO. Counts
    // come from the persistent catalog/ownership tables, everything else from
    // the checked-in Home template.
    std::optional<nlohmann::json> HttpMockBackend::Impl::BuildProfileInfoLocked() {
        const nlohmann::json home = OfflineApiMockBuiltIn::HomeGetHomeJsonObj();
        if (!home.is_object()) return std::nullopt;
        auto profile = home.value("profile_info", nlohmann::json::object());
        if (!profile.is_object()) profile = nlohmann::json::object();

        sqlite3_stmt* counts = nullptr;
        if (sqlite3_prepare_v2(db, HttpMockBackendBuiltInSql::QueriesHomeCountsSqlView.data(),
                              -1, &counts, nullptr) != SQLITE_OK || !counts) {
            if (counts) sqlite3_finalize(counts);
            Log::Error("[HttpMockBackend] home state count prepare failed");
            return std::nullopt;
        }
        if (sqlite3_step(counts) != SQLITE_ROW) {
            sqlite3_finalize(counts);
            return std::nullopt;
        }
        for (int index = 0; index < sqlite3_column_count(counts); ++index) {
            profile[sqlite3_column_name(counts, index)] = sqlite3_column_int64(counts, index);
        }
        sqlite3_finalize(counts);
        // Show the selected card/icon designs, when the player has any.
        if (EnsureMyDesignTableLocked()) {
            sqlite3_stmt* design = nullptr;
            if (sqlite3_prepare_v2(db,
                    "SELECT design_type, parts_info FROM offline_profile_my_design WHERE is_setting=1;",
                    -1, &design, nullptr) == SQLITE_OK && design) {
                while (sqlite3_step(design) == SQLITE_ROW) {
                    const auto* raw = reinterpret_cast<const char*>(sqlite3_column_text(design, 1));
                    if (!raw || !*raw) continue;
                    profile[sqlite3_column_int(design, 0) == kMyDesignTypeCard
                            ? "profile_card_parts_info" : "profile_icon_parts_info"] = raw;
                }
            }
            if (design) sqlite3_finalize(design);
        }
        return profile;
    }

    // Profile "my designs" (card and icon layouts). With an empty list the
    // client creates ten default designs per type on every Profile visit, one
    // request per frame (about 3 s of NOW LOADING), so they must persist.
    // ProfileMyDesign per 5.1 metadata: d_profile_my_designs_id,
    // my_design_type, my_design_parts_info, my_design_name, is_setting.
    // my_design_type uses 1 = card, 2 = icon (enum DesignTypeCard/Icon order).
    // Constants and JsonStringField are defined above BuildProfileInfoLocked.
    bool HttpMockBackend::Impl::EnsureMyDesignTableLocked() {
        return db && ExecSql(db,
            "CREATE TABLE IF NOT EXISTS offline_profile_my_design ("
            "d_profile_my_designs_id TEXT PRIMARY KEY, design_type INTEGER NOT NULL, "
            "parts_info TEXT NOT NULL DEFAULT '', design_name TEXT NOT NULL DEFAULT '', "
            "is_setting INTEGER NOT NULL DEFAULT 0, sort_order INTEGER NOT NULL DEFAULT 0);");
    }

    std::optional<MockStoredResponse> HttpMockBackend::Impl::GetMyDesignListLocked(int designType) {
        if (!initialized && !EnsureReadyLocked()) return std::nullopt;
        if (!EnsureMyDesignTableLocked()) return std::nullopt;
        nlohmann::json list = nlohmann::json::array();
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(db,
                "SELECT d_profile_my_designs_id, parts_info, design_name, is_setting "
                "FROM offline_profile_my_design WHERE design_type=? ORDER BY sort_order;",
                -1, &stmt, nullptr) != SQLITE_OK || !stmt) {
            if (stmt) sqlite3_finalize(stmt);
            return std::nullopt;
        }
        sqlite3_bind_int(stmt, 1, designType);
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            const auto text = [stmt](int column) {
                const auto* raw = reinterpret_cast<const char*>(sqlite3_column_text(stmt, column));
                return std::string(raw ? raw : "");
            };
            list.push_back({
                {"d_profile_my_designs_id", text(0)},
                {"my_design_type", designType},
                {"my_design_parts_info", text(1)},
                {"my_design_name", text(2)},
                {"is_setting", sqlite3_column_int(stmt, 3) != 0},
            });
        }
        sqlite3_finalize(stmt);
        return MockStoredResponse{nlohmann::json{{"my_design_list", std::move(list)}}.dump(),
                                  {}, 200, "OK (offline mock)"};
    }

    std::optional<MockStoredResponse> HttpMockBackend::Impl::SetMyDesignLocked(
            int designType, std::string_view payloadJson) {
        if (!initialized && !EnsureReadyLocked()) return std::nullopt;
        if (!EnsureMyDesignTableLocked()) return std::nullopt;
        const auto payload = nlohmann::json::parse(payloadJson.begin(), payloadJson.end(), nullptr, false);
        const char* partsKey = designType == kMyDesignTypeCard
                ? "profile_card_parts_info" : "profile_icon_parts_info";
        const std::string parts = JsonStringField(payload, partsKey);
        const std::string name = JsonStringField(payload, "my_design_name");
        const std::string id = JsonStringField(payload, "d_profile_my_designs_id");
        const auto scalar = [this](const char* sql, int type) {
            sqlite3_stmt* stmt = nullptr;
            int value = 0;
            if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) == SQLITE_OK && stmt) {
                sqlite3_bind_int(stmt, 1, type);
                if (sqlite3_step(stmt) == SQLITE_ROW) value = sqlite3_column_int(stmt, 0);
            }
            if (stmt) sqlite3_finalize(stmt);
            return value;
        };
        sqlite3_stmt* stmt = nullptr;
        if (!id.empty()) {
            if (sqlite3_prepare_v2(db,
                    "UPDATE offline_profile_my_design SET parts_info=?1, "
                    "design_name=CASE WHEN ?2='' THEN design_name ELSE ?2 END "
                    "WHERE d_profile_my_designs_id=?3 AND design_type=?4;",
                    -1, &stmt, nullptr) != SQLITE_OK || !stmt) {
                if (stmt) sqlite3_finalize(stmt);
                return std::nullopt;
            }
            BindText(stmt, 1, parts);
            BindText(stmt, 2, name);
            BindText(stmt, 3, id);
            sqlite3_bind_int(stmt, 4, designType);
        } else {
            const int count = scalar("SELECT COUNT(*) FROM offline_profile_my_design WHERE design_type=?;",
                                     designType);
            if (count >= kMyDesignSlotsPerType) {
                // All slots exist; accept without growing the table.
                return MockStoredResponse{R"({"result":true})", {}, 200, "OK (offline mock)"};
            }
            if (sqlite3_prepare_v2(db,
                    "INSERT INTO offline_profile_my_design "
                    "(d_profile_my_designs_id, design_type, parts_info, design_name, is_setting, sort_order) "
                    "VALUES (?1, ?2, ?3, ?4, ?5, ?6);",
                    -1, &stmt, nullptr) != SQLITE_OK || !stmt) {
                if (stmt) sqlite3_finalize(stmt);
                return std::nullopt;
            }
            BindText(stmt, 1, GenerateUuid4());
            sqlite3_bind_int(stmt, 2, designType);
            BindText(stmt, 3, parts);
            BindText(stmt, 4, name);
            sqlite3_bind_int(stmt, 5, count == 0 ? 1 : 0);
            sqlite3_bind_int(stmt, 6, count + 1);
        }
        const bool ok = sqlite3_step(stmt) == SQLITE_DONE;
        sqlite3_finalize(stmt);
        if (!ok) return std::nullopt;
        return MockStoredResponse{R"({"result":true})", {}, 200, "OK (offline mock)"};
    }

    std::optional<MockStoredResponse> HttpMockBackend::Impl::SetMyDesignNameLocked(std::string_view payloadJson) {
        if (!initialized && !EnsureReadyLocked()) return std::nullopt;
        if (!EnsureMyDesignTableLocked()) return std::nullopt;
        const auto payload = nlohmann::json::parse(payloadJson.begin(), payloadJson.end(), nullptr, false);
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(db,
                "UPDATE offline_profile_my_design SET design_name=? WHERE d_profile_my_designs_id=?;",
                -1, &stmt, nullptr) != SQLITE_OK || !stmt) {
            if (stmt) sqlite3_finalize(stmt);
            return std::nullopt;
        }
        BindText(stmt, 1, JsonStringField(payload, "my_design_name"));
        BindText(stmt, 2, JsonStringField(payload, "d_profile_my_designs_id"));
        const bool ok = sqlite3_step(stmt) == SQLITE_DONE;
        sqlite3_finalize(stmt);
        if (!ok) return std::nullopt;
        return MockStoredResponse{R"({"result":true})", {}, 200, "OK (offline mock)"};
    }

    std::optional<MockStoredResponse> HttpMockBackend::Impl::SetProfileDesignLocked(
            int designType, std::string_view payloadJson) {
        if (!initialized && !EnsureReadyLocked()) return std::nullopt;
        if (!EnsureMyDesignTableLocked()) return std::nullopt;
        const auto payload = nlohmann::json::parse(payloadJson.begin(), payloadJson.end(), nullptr, false);
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(db,
                "UPDATE offline_profile_my_design SET is_setting=(d_profile_my_designs_id=?1) "
                "WHERE design_type=?2;",
                -1, &stmt, nullptr) != SQLITE_OK || !stmt) {
            if (stmt) sqlite3_finalize(stmt);
            return std::nullopt;
        }
        BindText(stmt, 1, JsonStringField(payload, "d_profile_my_designs_id"));
        sqlite3_bind_int(stmt, 2, designType);
        const bool ok = sqlite3_step(stmt) == SQLITE_DONE;
        sqlite3_finalize(stmt);
        if (!ok) return std::nullopt;
        return MockStoredResponse{R"({"result":true})", {}, 200, "OK (offline mock)"};
    }

    // ---- Card growth (強化 / 解放 / 進化) ------------------------------------
    // Request/response shapes from the 5.1 metadata:
    //   check_style_level_up {d_card_datas_id} -> {is_possible,
    //       current_style_level, selectable_max_level, style_point}
    //   style_level_up {d_card_datas_id, style_level} -> {result}
    //   check_limit_break {d_card_datas_id} -> {is_possible,
    //       current_limit_break_times, selectable_limit_break_times,
    //       user_item_list[{item_id,own_num}],
    //       required_item_list[{cost_item_id1,cost_num1,..3}]}
    //   limit_break {d_card_datas_id, limit_break_step} -> {result}
    //   check_evolution {d_card_datas_id} -> {is_possible, user_item_list,
    //       required_item_list, next_max_style_level}
    //   evolution {d_card_datas_id} -> {result}
    // Costs come from the Master (card_growth_catalog.json) and are taken
    // from the player inventory; results persist in museum_card_state and
    // offline_card_growth.
    std::optional<HttpMockBackend::Impl::CardGrowthState> HttpMockBackend::Impl::LoadCardGrowthLocked(
            std::string_view payloadJson) {
        if (!initialized && !EnsureReadyLocked()) return std::nullopt;
        if (!db) return std::nullopt;
        const auto dCardId = ExtractPayloadStringField(payloadJson, "d_card_datas_id");
        if (dCardId.empty()) return std::nullopt;
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(db,
                "SELECT card_detail.card_datas_id, museum_card_state.style_level, "
                "museum_card_state.limit_break_times, COALESCE(offline_card_growth.evolve_times, 0) "
                "FROM card_detail "
                "INNER JOIN museum_card_ownership USING(d_card_datas_id) "
                "INNER JOIN museum_card_state USING(d_card_datas_id) "
                "LEFT JOIN offline_card_growth USING(d_card_datas_id) "
                "WHERE card_detail.d_card_datas_id = ?;",
                -1, &stmt, nullptr) != SQLITE_OK || !stmt) {
            if (stmt) sqlite3_finalize(stmt);
            return std::nullopt;
        }
        BindText(stmt, 1, dCardId);
        std::optional<CardGrowthState> state;
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            state = CardGrowthState{
                dCardId,
                sqlite3_column_int(stmt, 0),
                sqlite3_column_int(stmt, 1),
                sqlite3_column_int(stmt, 2),
                sqlite3_column_int(stmt, 3),
                nullptr,
            };
            state->growth = FindCardGrowth(state->baseCardId);
            if (state->growth) {
                state->evolveTimes = std::clamp(state->evolveTimes, 0, CardGrowthMaxEvolve(*state->growth));
            }
        }
        sqlite3_finalize(stmt);
        return state;
    }

    int64_t HttpMockBackend::Impl::ItemHoldingLocked(int itemId) {
        sqlite3_stmt* stmt = nullptr;
        int64_t value = 0;
        if (sqlite3_prepare_v2(db,
                "SELECT COALESCE(SUM(inventory.item_num),0) FROM item "
                "INNER JOIN museum_item_inventory AS inventory USING(d_item_datas_id) WHERE item.item_id=?;",
                -1, &stmt, nullptr) == SQLITE_OK && stmt) {
            sqlite3_bind_int(stmt, 1, itemId);
            if (sqlite3_step(stmt) == SQLITE_ROW) value = sqlite3_column_int64(stmt, 0);
        }
        if (stmt) sqlite3_finalize(stmt);
        return value;
    }

    bool HttpMockBackend::Impl::ConsumeItemLocked(int itemId, int64_t amount) {
        if (amount <= 0) return true;
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(db,
                "UPDATE museum_item_inventory SET item_num=item_num-?1 WHERE item_num>=?1 AND d_item_datas_id="
                "(SELECT d_item_datas_id FROM item WHERE item_id=?2 LIMIT 1);",
                -1, &stmt, nullptr) != SQLITE_OK || !stmt) {
            if (stmt) sqlite3_finalize(stmt);
            return false;
        }
        sqlite3_bind_int64(stmt, 1, amount);
        sqlite3_bind_int(stmt, 2, itemId);
        const bool ok = sqlite3_step(stmt) == SQLITE_DONE && sqlite3_changes(db) == 1;
        sqlite3_finalize(stmt);
        return ok;
    }

    nlohmann::json HttpMockBackend::Impl::UserItemListLocked(const std::vector<int>& itemIds) {
        nlohmann::json list = nlohmann::json::array();
        std::unordered_set<int> seen;
        for (const int itemId : itemIds) {
            if (itemId <= 0 || !seen.insert(itemId).second) continue;
            // The client's user_item_list element reads own_num (observed in
            // its deserialized check_evolution response).
            list.push_back({{"item_id", itemId}, {"own_num", ItemHoldingLocked(itemId)}});
        }
        return list;
    }

    namespace {
        nlohmann::json RequiredItemEntry(const std::vector<std::pair<int, int64_t>>& costs) {
            nlohmann::json entry = nlohmann::json::object();
            for (int index = 0; index < 3; ++index) {
                const auto key = std::to_string(index + 1);
                const bool has = index < static_cast<int>(costs.size());
                entry["cost_item_id" + key] = has ? costs[index].first : 0;
                entry["cost_num" + key] = has ? costs[index].second : 0;
            }
            return entry;
        }

        std::vector<std::pair<int, int64_t>> EvolutionCosts(const nlohmann::json& growth, int stage) {
            std::vector<std::pair<int, int64_t>> costs;
            const auto& stages = growth["evolution"];
            if (stage < 0 || stage >= static_cast<int>(stages.size())) return costs;
            for (const auto& cost : stages[stage]) {
                if (cost.is_array() && cost.size() == 2) costs.emplace_back(cost[0].get<int>(), cost[1].get<int64_t>());
            }
            return costs;
        }

        MockStoredResponse GrowthResult(bool ok, const char* reason = nullptr) {
            nlohmann::json result = {{"result", ok}};
            if (!ok && reason) result["error"] = reason;
            return MockStoredResponse{result.dump(), {}, 200, ok ? "OK (offline card growth)" : "Rejected"};
        }
    }

    std::optional<MockStoredResponse> HttpMockBackend::Impl::CheckStyleLevelUpLocked(std::string_view payloadJson) {
        const auto state = LoadCardGrowthLocked(payloadJson);
        if (!state) return std::nullopt;
        const int cap = state->growth ? CardGrowthStyleCap(*state->growth, state->evolveTimes) : state->styleLevel;
        const int pointItem = CardGrowthCatalog().value("style_point_item_id", 3004000);
        nlohmann::json result = {
            {"is_possible", state->growth != nullptr && state->styleLevel < cap},
            {"current_style_level", state->styleLevel},
            {"selectable_max_level", std::max(cap, state->styleLevel)},
            {"style_point", ItemHoldingLocked(pointItem)},
        };
        return MockStoredResponse{result.dump(), {}, 200, "OK (offline mock)"};
    }

    std::optional<MockStoredResponse> HttpMockBackend::Impl::StyleLevelUpLocked(std::string_view payloadJson) {
        const auto state = LoadCardGrowthLocked(payloadJson);
        if (!state) return std::nullopt;
        if (!state->growth) return GrowthResult(false, "card cannot grow");
        const auto payload = nlohmann::json::parse(payloadJson.begin(), payloadJson.end(), nullptr, false);
        const int target = payload.is_object() ? payload.value("style_level", 0) : 0;
        const int cap = CardGrowthStyleCap(*state->growth, state->evolveTimes);
        if (target <= state->styleLevel || target > cap) return GrowthResult(false, "invalid style level");
        const int64_t from = CardGrowthCumulativeStylePoint(*state->growth, state->styleLevel);
        const int64_t to = CardGrowthCumulativeStylePoint(*state->growth, target);
        if (from < 0 || to < from) return GrowthResult(false, "unknown level table");
        const int pointItem = CardGrowthCatalog().value("style_point_item_id", 3004000);
        if (!ExecSql(db, "BEGIN IMMEDIATE;")) return std::nullopt;
        sqlite3_stmt* stmt = nullptr;
        bool ok = ConsumeItemLocked(pointItem, to - from) &&
            sqlite3_prepare_v2(db, "UPDATE museum_card_state SET style_level=? WHERE d_card_datas_id=?;",
                               -1, &stmt, nullptr) == SQLITE_OK && stmt;
        if (ok) {
            sqlite3_bind_int(stmt, 1, target);
            BindText(stmt, 2, state->dCardDatasId);
            ok = sqlite3_step(stmt) == SQLITE_DONE && sqlite3_changes(db) == 1;
        }
        if (stmt) sqlite3_finalize(stmt);
        int characterId = 0;
        sqlite3_stmt* characterStmt = nullptr;
        if (sqlite3_prepare_v2(db, "SELECT character_id FROM card_detail WHERE d_card_datas_id=?;",
                               -1, &characterStmt, nullptr) == SQLITE_OK && characterStmt) {
            BindText(characterStmt, 1, state->dCardDatasId);
            if (sqlite3_step(characterStmt) == SQLITE_ROW)
                characterId = sqlite3_column_int(characterStmt, 0);
        }
        if (characterStmt) sqlite3_finalize(characterStmt);
        ok = ok && RecordMissionEventLocked(
            "style:" + state->dCardDatasId + ":" + std::to_string(target),
            "STYLE_LEVEL_GAIN", target - state->styleLevel, characterId);
        if (!ok || !ExecSql(db, "COMMIT;")) {
            ExecSql(db, "ROLLBACK;");
            return GrowthResult(false, "insufficient style points");
        }
        return GrowthResult(true);
    }

    std::optional<MockStoredResponse> HttpMockBackend::Impl::CheckLimitBreakLocked(std::string_view payloadJson) {
        const auto state = LoadCardGrowthLocked(payloadJson);
        if (!state) return std::nullopt;
        nlohmann::json required = nlohmann::json::array();
        std::vector<int> itemIds;
        int selectable = state->limitBreakTimes;
        if (state->growth) {
            std::map<int, int64_t> spent;
            for (const auto& step : (*state->growth)["limit_break"]) {
                const int times = step.value("times", 0);
                if (times <= state->limitBreakTimes) continue;
                const int itemId = step.value("item_id", 0);
                const int64_t num = step.value("num", 0);
                required.push_back(RequiredItemEntry({{itemId, num}}));
                itemIds.push_back(itemId);
                spent[itemId] += num;
                if (spent[itemId] <= ItemHoldingLocked(itemId) && times == selectable + 1) selectable = times;
            }
        }
        nlohmann::json result = {
            {"is_possible", selectable > state->limitBreakTimes},
            {"current_limit_break_times", state->limitBreakTimes},
            {"selectable_limit_break_times", selectable},
            {"user_item_list", UserItemListLocked(itemIds)},
            {"required_item_list", std::move(required)},
        };
        return MockStoredResponse{result.dump(), {}, 200, "OK (offline mock)"};
    }

    std::optional<MockStoredResponse> HttpMockBackend::Impl::LimitBreakLocked(std::string_view payloadJson) {
        const auto state = LoadCardGrowthLocked(payloadJson);
        if (!state) return std::nullopt;
        if (!state->growth) return GrowthResult(false, "card cannot grow");
        const auto payload = nlohmann::json::parse(payloadJson.begin(), payloadJson.end(), nullptr, false);
        const int steps = payload.is_object() ? payload.value("limit_break_step", 1) : 1;
        const int target = state->limitBreakTimes + steps;
        std::map<int, int64_t> costs;
        int maxTimes = state->limitBreakTimes;
        for (const auto& step : (*state->growth)["limit_break"]) {
            const int times = step.value("times", 0);
            maxTimes = std::max(maxTimes, times);
            if (times > state->limitBreakTimes && times <= target)
                costs[step.value("item_id", 0)] += step.value("num", 0);
        }
        if (steps < 1 || target > maxTimes) return GrowthResult(false, "invalid limit break step");
        if (!ExecSql(db, "BEGIN IMMEDIATE;")) return std::nullopt;
        bool ok = true;
        for (const auto& [itemId, num] : costs) ok = ok && ConsumeItemLocked(itemId, num);
        sqlite3_stmt* stmt = nullptr;
        ok = ok && sqlite3_prepare_v2(db,
                "UPDATE museum_card_state SET limit_break_times=? WHERE d_card_datas_id=?;",
                -1, &stmt, nullptr) == SQLITE_OK && stmt;
        if (ok) {
            sqlite3_bind_int(stmt, 1, target);
            BindText(stmt, 2, state->dCardDatasId);
            ok = sqlite3_step(stmt) == SQLITE_DONE && sqlite3_changes(db) == 1;
        }
        if (stmt) sqlite3_finalize(stmt);
        if (!ok || !ExecSql(db, "COMMIT;")) {
            ExecSql(db, "ROLLBACK;");
            return GrowthResult(false, "insufficient materials");
        }
        return GrowthResult(true);
    }

    std::optional<MockStoredResponse> HttpMockBackend::Impl::CheckEvolutionLocked(std::string_view payloadJson) {
        const auto state = LoadCardGrowthLocked(payloadJson);
        if (!state) return std::nullopt;
        nlohmann::json required = nlohmann::json::array();
        std::vector<int> itemIds;
        bool affordable = false;
        int nextCap = 0;
        const bool canEvolve = state->growth && state->evolveTimes < CardGrowthMaxEvolve(*state->growth);
        if (canEvolve) {
            const auto costs = EvolutionCosts(*state->growth, state->evolveTimes);
            required.push_back(RequiredItemEntry(costs));
            affordable = true;
            for (const auto& [itemId, num] : costs) {
                itemIds.push_back(itemId);
                affordable = affordable && ItemHoldingLocked(itemId) >= num;
            }
            nextCap = CardGrowthStyleCap(*state->growth, state->evolveTimes + 1);
        }
        nlohmann::json result = {
            {"is_possible", canEvolve && affordable},
            {"user_item_list", UserItemListLocked(itemIds)},
            {"required_item_list", std::move(required)},
            {"next_max_style_level", nextCap},
        };
        return MockStoredResponse{result.dump(), {}, 200, "OK (offline mock)"};
    }

    std::optional<MockStoredResponse> HttpMockBackend::Impl::EvolutionLocked(std::string_view payloadJson) {
        const auto state = LoadCardGrowthLocked(payloadJson);
        if (!state) return std::nullopt;
        if (!state->growth || state->evolveTimes >= CardGrowthMaxEvolve(*state->growth))
            return GrowthResult(false, "card cannot evolve");
        const auto costs = EvolutionCosts(*state->growth, state->evolveTimes);
        const int next = state->evolveTimes + 1;
        if (!ExecSql(db, "BEGIN IMMEDIATE;")) return std::nullopt;
        bool ok = true;
        for (const auto& [itemId, num] : costs) ok = ok && ConsumeItemLocked(itemId, num);
        sqlite3_stmt* stmt = nullptr;
        ok = ok && sqlite3_prepare_v2(db,
                "INSERT INTO offline_card_growth(d_card_datas_id, evolve_times) VALUES(?1, ?2) "
                "ON CONFLICT(d_card_datas_id) DO UPDATE SET evolve_times=excluded.evolve_times;",
                -1, &stmt, nullptr) == SQLITE_OK && stmt;
        if (ok) {
            BindText(stmt, 1, state->dCardDatasId);
            sqlite3_bind_int(stmt, 2, next);
            ok = sqlite3_step(stmt) == SQLITE_DONE;
        }
        if (stmt) sqlite3_finalize(stmt);
        if (ok && next >= CardGrowthMaxEvolve(*state->growth)) {
            sqlite3_stmt* maxStmt = nullptr;
            ok = sqlite3_prepare_v2(db, "UPDATE museum_card_state SET is_evolve_max=1 WHERE d_card_datas_id=?;",
                                    -1, &maxStmt, nullptr) == SQLITE_OK && maxStmt;
            if (ok) {
                BindText(maxStmt, 1, state->dCardDatasId);
                ok = sqlite3_step(maxStmt) == SQLITE_DONE;
            }
            if (maxStmt) sqlite3_finalize(maxStmt);
        }
        ok = ok && RecordMissionEventLocked(
            "evolve:" + state->dCardDatasId + ":" + std::to_string(next), "EVOLUTION");
        if (!ok || !ExecSql(db, "COMMIT;")) {
            ExecSql(db, "ROLLBACK;");
            return GrowthResult(false, "insufficient materials");
        }
        return GrowthResult(true);
    }

    std::optional<MockStoredResponse> HttpMockBackend::Impl::CheckSkillLevelUpLocked(std::string_view payloadJson) {
        // Gacha base cards carry the developed skill levels (already MAX), and
        // the preserved collection is fully developed, so no skill can level.
        const auto state = LoadCardGrowthLocked(payloadJson);
        if (!state) return std::nullopt;
        const auto payload = nlohmann::json::parse(payloadJson.begin(), payloadJson.end(), nullptr, false);
        const int skillType = payload.is_object() ? payload.value("skill_type", 0) : 0;
        int level = 0;
        auto detail = GetCardDetailByDCardIdLocked(state->dCardDatasId);
        if (detail) {
            const auto card = nlohmann::json::parse(detail->body, nullptr, false);
            if (card.is_object() && card.contains("user_card_data")) {
                for (const auto& skill : card["user_card_data"].value("skill_list", nlohmann::json::array())) {
                    if (skill.value("skill_type", -1) == skillType) level = skill.value("skill_level", 0);
                }
            }
        }
        nlohmann::json result = {
            {"is_possible", false},
            {"current_skill_level", level},
            {"selectable_max_level", level},
            {"user_item_list", nlohmann::json::array()},
            {"required_item_list", nlohmann::json::array()},
        };
        return MockStoredResponse{result.dump(), {}, 200, "OK (offline mock)"};
    }

    std::optional<MockStoredResponse> HttpMockBackend::Impl::GetProfileInfoResponseLocked() {
        if (!initialized && !EnsureReadyLocked()) return std::nullopt;
        if (!db) return std::nullopt;
        auto profile = BuildProfileInfoLocked();
        if (!profile) return std::nullopt;

        // ProfileGetInfoResponse field order recovered from the 5.1 metadata:
        // profile_info, is_own, is_mute, is_report, friend_request_status,
        // circle_invite_status, follow_status. The offline player only ever
        // views its own profile; there is no social graph to report on.
        nlohmann::json result;
        result["profile_info"] = std::move(*profile);
        result["is_own"] = true;
        result["is_mute"] = false;
        result["is_report"] = false;
        result["friend_request_status"] = 0;
        result["circle_invite_status"] = 0;
        result["follow_status"] = 0;
        const auto gachaState = LoadGachaStateLocked();
        std::string headers = gachaState
                ? GachaHeaders(gachaState->freeJewels, gachaState->paidJewels)
                : std::string{};
        return MockStoredResponse{result.dump(), std::move(headers), 200, "OK (offline mock)"};
    }

    std::optional<MockStoredResponse> HttpMockBackend::Impl::GetHomeResponseLocked() {
        if (!initialized && !EnsureReadyLocked()) return std::nullopt;
        if (!db) return std::nullopt;
        const auto gachaState = LoadGachaStateLocked();
        if (!gachaState) return std::nullopt;

        // Keep only the DTO shape/default compatibility values in the checked-in
        // JSON. Every count below is derived from the same persistent catalog and
        // player ownership tables used by the detail/list routes.
        nlohmann::json result = OfflineApiMockBuiltIn::HomeGetHomeJsonObj();
        if (!result.is_object()) return std::nullopt;
        auto profile = BuildProfileInfoLocked();
        if (!profile) return std::nullopt;
        result["profile_info"] = std::move(*profile);

        // No plan source has been fabricated.  The empty value is now a real
        // query result for the currently imported 5.1 Master subset rather than
        // a contradictory hard-coded profile assertion.
        result["plan_list"] = nlohmann::json::array();
        result["has_daily_ticket"] = false;
        result["has_grand_prix_playable_count"] = false;
        if (auto badge = result.find("highlighted_badge_info");
            badge != result.end() && badge->is_object()) {
            (*badge)["has_daily_ticket"] = false;
            (*badge)["has_grand_prix_playable_count"] = false;
            (*badge)["is_not_received_common_mission_reward"] = false;
            (*badge)["is_not_read_sticker_exchange_new_arrival"] = false;
        }
        return MockStoredResponse{
            result.dump(), GachaHeaders(gachaState->freeJewels, gachaState->paidJewels),
            200, "OK (offline mock)"};
    }

    bool HttpMockBackend::Impl::RecordMissionEventLocked(std::string_view sourceKey,
                                                         std::string_view kind,
                                                         int amount, int detail) {
        if (!db || sourceKey.empty() || kind.empty() || amount < 0) return false;
        sqlite3_stmt* stmt = nullptr;
        const char* sql = "INSERT OR IGNORE INTO offline_mission_event "
                          "(source_key,event_kind,amount,detail) VALUES (?,?,?,?);";
        if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK || !stmt) {
            if (stmt) sqlite3_finalize(stmt);
            return false;
        }
        const bool ok = BindText(stmt, 1, sourceKey) && BindText(stmt, 2, kind) &&
                        sqlite3_bind_int(stmt, 3, amount) == SQLITE_OK &&
                        sqlite3_bind_int(stmt, 4, detail) == SQLITE_OK &&
                        sqlite3_step(stmt) == SQLITE_DONE;
        sqlite3_finalize(stmt);
        return ok;
    }

    std::optional<MockStoredResponse> HttpMockBackend::Impl::GetMissionListResponseLocked() {
        if (!initialized && !EnsureReadyLocked()) return std::nullopt;
        if (!db) return std::nullopt;
        const auto& rules = OfflineApiMockBuiltIn::MissionRulesJsonObj();
        nlohmann::json result = OfflineApiMockBuiltIn::MissionGetListJsonObj();
        if (!rules.is_object() || !result.is_object() ||
            !result.contains("mission_list") || !result["mission_list"].is_array()) return std::nullopt;

        const auto scalar = [this](const std::string& sql, int detail = 0,
                                   std::string_view kind = {}) -> int64_t {
            sqlite3_stmt* stmt = nullptr;
            if (sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK || !stmt) {
                if (stmt) sqlite3_finalize(stmt);
                Log::ErrorFmt("[MissionFlow] progress query failed: %s", sqlite3_errmsg(db));
                return 0;
            }
            int index = 1;
            if (!kind.empty()) BindText(stmt, index++, kind);
            if (detail) sqlite3_bind_int(stmt, index, detail);
            const int64_t value = sqlite3_step(stmt) == SQLITE_ROW ? sqlite3_column_int64(stmt, 0) : 0;
            sqlite3_finalize(stmt);
            return value;
        };
        const auto eventCount = [&](int missionType, std::string_view kind,
                                    int detail = 0, bool distinctDetail = false) -> int64_t {
            std::string sql = distinctDetail ?
                "SELECT COUNT(DISTINCT detail) FROM offline_mission_event WHERE event_kind=?" :
                "SELECT COALESCE(SUM(amount),0) FROM offline_mission_event WHERE event_kind=?";
            if (detail) sql += " AND detail=?";
            if (missionType == 1) {
                sql += " AND date(occurred_at,'+9 hours','-4 hours')="
                       "date('now','+9 hours','-4 hours')";
            } else if (missionType == 2) {
                sql += " AND date(occurred_at,'+9 hours','-4 hours','weekday 0','-6 days')="
                       "date('now','+9 hours','-4 hours','weekday 0','-6 days')";
            }
            return scalar(sql, detail, kind);
        };

        const auto periodKey = [](int missionType) -> std::string {
            if (missionType == 3) return "main";
            // 04:00 JST is 19:00 UTC on the preceding civil day.
            std::time_t shifted = std::time(nullptr) + 5 * 3600;
            std::tm day{};
#if defined(_WIN32)
            gmtime_s(&day, &shifted);
#else
            gmtime_r(&shifted, &day);
#endif
            if (missionType == 2) {
                shifted -= ((day.tm_wday + 6) % 7) * 86400;
#if defined(_WIN32)
                gmtime_s(&day, &shifted);
#else
                gmtime_r(&shifted, &day);
#endif
            }
            char date[16]{};
            std::strftime(date, sizeof(date), "%Y-%m-%d", &day);
            return std::to_string(missionType) + ":" + date;
        };
        nlohmann::json reachedAwards = nlohmann::json::array();
        // Member Fan Lv. as shown in the profile (same ProfileInfo source).
        const auto profile = BuildProfileInfoLocked();
        const auto fanLevel = [&profile](int characterId) -> int64_t {
            if (!profile || !profile->contains("fan_level_list")) return 0;
            for (const auto& entry : (*profile)["fan_level_list"]) {
                if (entry.value("character_id", 0) == characterId) return entry.value("member_fan_level", 0);
            }
            return 0;
        };

#if defined(LINKURA_MOCK_STANDALONE)
        // The 5.0.1 (iOS) client indexes the achieve-prize list while building
        // the Daily/Weekly pages and throws on an empty one, which freezes the
        // scene change. 5.1.0 (Android) hides the Weekly/Main tabs when the
        // list is present, so it keeps the empty list.
        {
            const auto& tiers = OfflineApiMockBuiltIn::MissionAchieveRewardsJsonObj();
            for (auto& group : result["mission_list"]) {
                const auto key = std::to_string(group.value("mission_type", 0));
                if (tiers.contains(key) && group["achieve_reward_list"].empty()) {
                    group["achieve_reward_list"] = tiers[key];
                }
            }
        }
#endif
        for (auto& group : result["mission_list"]) {
            const int missionType = group.value("mission_type", 0);
            const std::string period = periodKey(missionType);
            int completed = 0;
            for (auto& mission : group["mission_list"]) {
                const int id = mission.value("mission_id", 0);
                const auto rule = rules.find(std::to_string(id));
                if (rule == rules.end() || !rule->is_object()) continue;
                const int condition = rule->value("condition_type", 0);
                const int detail = rule->value("condition_detail", 0);
                int64_t progress = 0;
                switch (condition) {
                    case 1: progress = eventCount(missionType, "QUEST_CLEAR"); break;
                    case 3: progress = eventCount(missionType, "DAILY_CLEAR"); break;
                    case 5: progress = eventCount(missionType, "DREAM_CLEAR"); break;
                    case 10:
                        progress = scalar(
                            "SELECT COALESCE(SUM(best),0) FROM (SELECT MAX(amount) AS best "
                            "FROM offline_mission_event WHERE event_kind=? GROUP BY detail)", 0, "QUEST_STAR");
                        break;
                    case 13:
                        if (detail == 1) {
                            const int64_t total = scalar(
                                "SELECT COUNT(*) FROM daily_quest_stage WHERE stage_id % 100 = 1");
                            progress = total > 0 &&
                                eventCount(3, "DAILY_CLEAR_I", 0, true) >= total ? 1 : 0;
                        }
                        break;
                    case 20:
                        progress = detail == 0 ? scalar(
                            "SELECT COUNT(DISTINCT card_detail.card_datas_id) FROM museum_card_ownership "
                            "JOIN card_detail USING(d_card_datas_id)") : scalar(
                            "SELECT COUNT(DISTINCT card_detail.card_datas_id) FROM museum_card_ownership "
                            "JOIN card_detail USING(d_card_datas_id) WHERE card_detail.character_id=?1",
                            detail);
                        break;
                    case 21: progress = eventCount(missionType, "CARD_OBTAIN"); break;
                    case 30:
                        // Main: a member's current style level total; Weekly:
                        // levels gained this week.
                        progress = (missionType == 3 && detail) ? scalar(
                            "SELECT COALESCE(SUM(museum_card_state.style_level),0) FROM museum_card_ownership "
                            "JOIN museum_card_state USING(d_card_datas_id) "
                            "JOIN card_detail USING(d_card_datas_id) WHERE card_detail.character_id=?1",
                            detail) : eventCount(missionType, "STYLE_LEVEL_GAIN", detail);
                        break;
                    case 60:
                        progress = fanLevel(detail);
                        break;
                    case 40: progress = eventCount(missionType, "EVOLUTION"); break;
                    case 901: progress = eventCount(missionType, "SHOW_CLEAR"); break;
                    // Skill upgrades, With×MEETS viewing and social actions have
                    // no local player event source yet.
                    default: break;
                }
                const int target = mission.value("condition_achieve_num", 0);
                const int shown = static_cast<int>(std::clamp<int64_t>(progress, 0, target));
                mission["achieved_num"] = shown;
                mission["is_achieved"] = target > 0 && shown >= target;
                if (target > 0 && shown >= target) {
                    ++completed;
                    reachedAwards.push_back({{"period", period}, {"id", id},
                                             {"rewards", mission.value("reward_list", nlohmann::json::array())}});
                }
            }
            for (auto& prize : group["achieve_reward_list"]) {
                prize["achieved_num"] = completed;
                const int target = prize.value("target_num", 0);
                prize["is_achieved"] = target > 0 && completed >= target;
                if (target > 0 && completed >= target) {
                    reachedAwards.push_back({
                        {"period", period}, {"id", -1000000 - missionType * 1000 - target},
                        {"rewards", prize.value("reward_list", nlohmann::json::array())}});
                }
            }
        }

        if (!ExecSql(db, "BEGIN IMMEDIATE;")) return std::nullopt;
        const auto rollback = [this]() -> std::optional<MockStoredResponse> {
            ExecSql(db, "ROLLBACK;");
            return std::nullopt;
        };
        // Bump the version whenever progress rules change: on the first list
        // after a rule change, everything already reached is baselined rather
        // than paid out (otherwise new rules would pay for Museum state).
        if (!ExecSql(db, "INSERT OR IGNORE INTO offline_mission_meta(key,value) "
                         "VALUES('award_baseline_v2','done');")) return rollback();
        const bool isBaseline = sqlite3_changes(db) == 1;
        sqlite3_stmt* award = nullptr;
        sqlite3_stmt* grant = nullptr;
        const bool prepared =
            sqlite3_prepare_v2(db,
                "INSERT OR IGNORE INTO offline_mission_award(period_key,mission_id,award_state) "
                "VALUES(?,?,?);", -1, &award, nullptr) == SQLITE_OK && award &&
            sqlite3_prepare_v2(db,
                "INSERT INTO museum_item_inventory(d_item_datas_id,item_num) "
                "SELECT d_item_datas_id,?1 FROM item WHERE item_id=?2 LIMIT 1 "
                "ON CONFLICT(d_item_datas_id) DO UPDATE SET item_num=item_num+excluded.item_num;",
                -1, &grant, nullptr) == SQLITE_OK && grant;
        if (!prepared) {
            if (award) sqlite3_finalize(award);
            if (grant) sqlite3_finalize(grant);
            return rollback();
        }
        bool ok = true;
        for (const auto& reached : reachedAwards) {
            sqlite3_reset(award);
            sqlite3_clear_bindings(award);
            ok = BindText(award, 1, reached["period"].get<std::string>()) &&
                 sqlite3_bind_int(award, 2, reached["id"].get<int>()) == SQLITE_OK &&
                 BindText(award, 3, isBaseline ? "BASELINE" : "GRANTED") &&
                 sqlite3_step(award) == SQLITE_DONE;
            if (!ok) break;
            if (sqlite3_changes(db) == 0 || isBaseline) continue;
            for (const auto& reward : reached["rewards"]) {
                // Category 1 records are achievement marks, not inventory.
                if (reward.value("reward_type", 0) != 2 &&
                    reached["id"].get<int>() > 0) continue;
                const int itemId = reward.value("item_id", 0);
                const int amount = reward.value("reward_num", 0);
                if (itemId <= 0 || amount <= 0) continue;
                sqlite3_reset(grant);
                sqlite3_clear_bindings(grant);
                ok = sqlite3_bind_int(grant, 1, amount) == SQLITE_OK &&
                     sqlite3_bind_int(grant, 2, itemId) == SQLITE_OK &&
                     sqlite3_step(grant) == SQLITE_DONE && sqlite3_changes(db) == 1;
                if (!ok) break;
                if (itemId == 1002001) {
                    sqlite3_stmt* wallet = nullptr;
                    ok = sqlite3_prepare_v2(db,
                        "UPDATE museum_gacha_state SET free_jewels=free_jewels+? "
                        "WHERE series_id=1055003;", -1, &wallet, nullptr) == SQLITE_OK && wallet;
                    if (ok) {
                        sqlite3_bind_int(wallet, 1, amount);
                        ok = sqlite3_step(wallet) == SQLITE_DONE && sqlite3_changes(db) == 1;
                    }
                    if (wallet) sqlite3_finalize(wallet);
                    if (!ok) break;
                }
            }
            if (!ok) break;
        }
        sqlite3_finalize(award);
        sqlite3_finalize(grant);
        if (!ok || !ExecSql(db, "COMMIT;")) return rollback();
        const auto wallet = LoadGachaStateLocked();
        return MockStoredResponse{
            result.dump(), wallet ? GachaHeaders(wallet->freeJewels, wallet->paidJewels) : std::string{},
            200, "OK (offline mission state)"};
    }

    std::optional<MockStoredResponse> HttpMockBackend::Impl::GetGachaSeriesListResponseLocked() {
        const auto state = LoadGachaStateLocked();
        if (!state) return std::nullopt;
        auto response = OfflineApiMockBuiltIn::GachaGetSeriesListJsonObj();
        if (!response.is_object() || !response.contains("gacha_series_list") ||
            !response["gacha_series_list"].is_array() || response["gacha_series_list"].size() != 1) {
            return std::nullopt;
        }
        const auto prototype = response["gacha_series_list"][0];
        auto& seriesList = response["gacha_series_list"];
        seriesList = nlohmann::json::array();
        for (const auto& config : kMuseumGachaSeries) {
            const auto seriesState = LoadGachaStateLocked(config.id);
            if (!seriesState) return std::nullopt;
            auto series = prototype;
            series["gacha_series_id"] = config.id;
            series["user_exchange_point_num"] = seriesState->exchangePoint;
            series["user_series_exec_count"] = seriesState->drawCount;
            series["is_single_enable"] = state->freeJewels + state->paidJewels >= 200;
            series["is_consective_enable"] = state->freeJewels + state->paidJewels >= 2000;
            // This series catalog must not depend on which cards the player
            // already owns. Advertise the complete Museum candidate set so a
            // new acquisition is represented consistently. Whether the
            // client preloads its assets from this field still needs a direct
            // client-side trace; Master metadata alone proves no asset exists.
            series["card_list"] = config.cards;
            seriesList.push_back(std::move(series));
        }
        return MockStoredResponse{
            response.dump(), GachaHeaders(state->freeJewels, state->paidJewels),
            200, "OK (offline mock)"};
    }

    std::optional<MockStoredResponse> HttpMockBackend::Impl::GachaSetPurchaseLocked(
            std::string_view payloadJson) {
        const auto payload = nlohmann::json::parse(payloadJson.begin(), payloadJson.end(), nullptr, false);
        if (!payload.is_object() || !payload.contains("gacha_series_id") ||
            !payload["gacha_series_id"].is_number_integer() ||
            !FindMuseumGachaSeries(payload["gacha_series_id"].get<int>()) ||
            !payload.contains("gacha_set_purchase_type") ||
            !payload["gacha_set_purchase_type"].is_number_integer()) {
            return MockStoredResponse{R"({"error":"unsupported offline gacha request"})", {},
                                      400, "Unsupported offline gacha request"};
        }
        const auto* series = FindMuseumGachaSeries(payload["gacha_series_id"].get<int>());
        const int purchaseType = payload["gacha_set_purchase_type"].get<int>();
        if (purchaseType != 1 && purchaseType != 2) {
            return MockStoredResponse{R"({"error":"offline gacha supports single and ten draws"})", {},
                                      400, "Unsupported offline gacha purchase type"};
        }
        const int drawCount = purchaseType == 1 ? 1 : 10;
        const int price = drawCount * 200;
        const auto state = LoadGachaStateLocked(series->id);
        if (!state) return std::nullopt;
        if (state->freeJewels + state->paidJewels < price) {
            return MockStoredResponse{R"({"error":"insufficient local SIsCa"})",
                                      GachaHeaders(state->freeJewels, state->paidJewels),
                                      400, "Insufficient local SIsCa"};
        }

        // The preserved Master identifies standard and featured candidates,
        // but not the original server-side per-card probabilities.
        static thread_local std::mt19937 rng(std::random_device{}());
        const auto spentFree = std::min<int64_t>(state->freeJewels, price);
        const auto spentPaid = price - spentFree;
        const auto freeAfter = state->freeJewels - spentFree;
        const auto paidAfter = state->paidJewels - spentPaid;
        const auto pointsAfter = state->exchangePoint + drawCount;

        std::time_t now = std::time(nullptr);
        std::tm utc{};
#if defined(_WIN32)
        gmtime_s(&utc, &now);
#else
        gmtime_r(&now, &utc);
#endif
        char timeBuffer[32]{};
        std::strftime(timeBuffer, sizeof(timeBuffer), "%Y-%m-%dT%H:%M:%SZ", &utc);

        if (!ExecSql(db, "BEGIN IMMEDIATE;")) return std::nullopt;
        auto rollback = [this]() -> std::optional<MockStoredResponse> {
            ExecSql(db, "ROLLBACK;");
            return std::nullopt;
        };
        sqlite3_stmt* card = nullptr;
        sqlite3_stmt* owned = nullptr;
        sqlite3_stmt* acquired = nullptr;
        sqlite3_stmt* obtain = nullptr;
        sqlite3_stmt* cardState = nullptr;
        if (sqlite3_prepare_v2(db, "SELECT d_card_datas_id FROM card_detail WHERE card_datas_id=? LIMIT 1;",
                               -1, &card, nullptr) != SQLITE_OK ||
            // NEW means "not yet held", so check the ownership table that the
            // card list reads. The acquisition log misses cards obtained
            // through point exchange and would mark them NEW again.
            sqlite3_prepare_v2(db, "SELECT 1 FROM museum_card_ownership WHERE d_card_datas_id=?;",
                               -1, &owned, nullptr) != SQLITE_OK ||
            sqlite3_prepare_v2(db,
                "INSERT OR IGNORE INTO museum_gacha_acquisition "
                "(card_datas_id, first_acquired_at, acquisition_source) VALUES (?, ?, 'GACHA');",
                -1, &acquired, nullptr) != SQLITE_OK ||
            sqlite3_prepare_v2(db,
                "INSERT OR IGNORE INTO museum_card_ownership "
                "(d_card_datas_id, acquired_at, acquisition_source) VALUES (?, ?, 'GACHA');",
                -1, &obtain, nullptr) != SQLITE_OK ||
            sqlite3_prepare_v2(db,
                "INSERT OR IGNORE INTO museum_card_state "
                "(d_card_datas_id, style_level, limit_break_times, is_evolve_max) VALUES (?, 1, 0, 0);",
                -1, &cardState, nullptr) != SQLITE_OK) {
            if (card) sqlite3_finalize(card);
            if (owned) sqlite3_finalize(owned);
            if (acquired) sqlite3_finalize(acquired);
            if (obtain) sqlite3_finalize(obtain);
            if (cardState) sqlite3_finalize(cardState);
            return rollback();
        }
        nlohmann::json results = nlohmann::json::array();
        bool valid = true;
        for (int index = 0; index < drawCount && valid; ++index) {
            const int cardId = ChooseMuseumGachaCard(*series, rng);
            sqlite3_reset(card);
            sqlite3_clear_bindings(card);
            sqlite3_bind_int(card, 1, cardId);
            if (sqlite3_step(card) != SQLITE_ROW) { valid = false; break; }
            const auto* rawId = reinterpret_cast<const char*>(sqlite3_column_text(card, 0));
            if (!rawId) { valid = false; break; }
            const std::string dCardId(rawId);
            sqlite3_reset(owned);
            sqlite3_clear_bindings(owned);
            BindText(owned, 1, dCardId);
            const bool isNew = sqlite3_step(owned) != SQLITE_ROW;
            sqlite3_reset(acquired);
            sqlite3_clear_bindings(acquired);
            valid = sqlite3_bind_int(acquired, 1, cardId) == SQLITE_OK &&
                    BindText(acquired, 2, timeBuffer) &&
                    sqlite3_step(acquired) == SQLITE_DONE;
            if (!valid) break;
            sqlite3_reset(obtain);
            sqlite3_clear_bindings(obtain);
            valid = BindText(obtain, 1, dCardId) && BindText(obtain, 2, timeBuffer) &&
                    sqlite3_step(obtain) == SQLITE_DONE;
            sqlite3_reset(cardState);
            sqlite3_clear_bindings(cardState);
            valid = valid && BindText(cardState, 1, dCardId) &&
                    sqlite3_step(cardState) == SQLITE_DONE;
            results.push_back({
                {"order_id", index + 1}, {"card_datas_id", cardId},
                {"is_new", isNew}, {"coin_num", 0},
                {"material_limitbreak_item_id", 0}, {"material_limitbreak_num", 0},
            });
        }
        sqlite3_finalize(card);
        sqlite3_finalize(owned);
        sqlite3_finalize(acquired);
        sqlite3_finalize(obtain);
        sqlite3_finalize(cardState);
        if (!valid) return rollback();

        sqlite3_stmt* update = nullptr;
        const char* updateSql =
            "UPDATE museum_gacha_state SET exchange_point=? WHERE series_id=?;";
        if (sqlite3_prepare_v2(db, updateSql, -1, &update, nullptr) != SQLITE_OK || !update) {
            if (update) sqlite3_finalize(update);
            return rollback();
        }
        sqlite3_bind_int64(update, 1, pointsAfter);
        sqlite3_bind_int(update, 2, PointOwnerSeriesId(*series));
        valid = sqlite3_step(update) == SQLITE_DONE && sqlite3_changes(db) == 1;
        sqlite3_finalize(update);
        if (!valid) return rollback();
        if (sqlite3_prepare_v2(db,
                "UPDATE museum_gacha_state SET draw_count=draw_count+? WHERE series_id=?;",
                -1, &update, nullptr) != SQLITE_OK || !update) {
            if (update) sqlite3_finalize(update);
            return rollback();
        }
        sqlite3_bind_int(update, 1, drawCount);
        sqlite3_bind_int(update, 2, series->id);
        valid = sqlite3_step(update) == SQLITE_DONE && sqlite3_changes(db) == 1;
        sqlite3_finalize(update);
        if (!valid) return rollback();

        sqlite3_stmt* wallet = nullptr;
        if (sqlite3_prepare_v2(db, "UPDATE museum_gacha_state SET free_jewels=?, paid_jewels=? WHERE series_id=1055003;",
                               -1, &wallet, nullptr) != SQLITE_OK || !wallet) {
            if (wallet) sqlite3_finalize(wallet);
            return rollback();
        }
        sqlite3_bind_int64(wallet, 1, freeAfter);
        sqlite3_bind_int64(wallet, 2, paidAfter);
        valid = sqlite3_step(wallet) == SQLITE_DONE && sqlite3_changes(db) == 1;
        sqlite3_finalize(wallet);
        if (!valid) return rollback();

        sqlite3_stmt* mirror = nullptr;
        if (sqlite3_prepare_v2(db,
                "UPDATE museum_item_inventory SET item_num=? WHERE d_item_datas_id="
                "(SELECT d_item_datas_id FROM item WHERE item_id=? LIMIT 1);",
                -1, &mirror, nullptr) != SQLITE_OK || !mirror) {
            if (mirror) sqlite3_finalize(mirror);
            return rollback();
        }
        for (const auto [itemId, quantity] :
             {std::pair<int, int64_t>{1001001, paidAfter}, {1002001, freeAfter}}) {
            sqlite3_reset(mirror);
            sqlite3_clear_bindings(mirror);
            sqlite3_bind_int64(mirror, 1, quantity);
            sqlite3_bind_int(mirror, 2, itemId);
            if (sqlite3_step(mirror) != SQLITE_DONE) valid = false;
        }
        sqlite3_finalize(mirror);
        if (!valid) return rollback();

        sqlite3_stmt* history = nullptr;
        if (sqlite3_prepare_v2(db,
                "INSERT INTO museum_gacha_history "
                "(series_id, drawn_at, price_type, price_num, results_json) VALUES (?, ?, 2, ?, ?);",
                -1, &history, nullptr) != SQLITE_OK || !history) {
            if (history) sqlite3_finalize(history);
            return rollback();
        }
        const auto resultsText = results.dump();
        valid = sqlite3_bind_int(history, 1, series->id) == SQLITE_OK &&
                BindText(history, 2, timeBuffer) &&
                sqlite3_bind_int(history, 3, price) == SQLITE_OK &&
                BindText(history, 4, resultsText) && sqlite3_step(history) == SQLITE_DONE;
        sqlite3_finalize(history);
        if (valid) {
            const auto drawId = sqlite3_last_insert_rowid(db);
            valid = RecordMissionEventLocked("gacha-draw:" + std::to_string(drawId),
                                             "CARD_OBTAIN", drawCount);
        }
        if (!valid || !ExecSql(db, "COMMIT;")) return rollback();

        nlohmann::json response;
        response["gacha_result_list"] = std::move(results);
        response["gacha_guarantee_point"] = {
            {"exchange_point_id", series->exchangePointId},
            {"user_guarantee_point_num_before", state->exchangePoint},
            {"user_guarantee_point_num_after", pointsAfter},
        };
        response["gacha_bonus_result_list"] = nlohmann::json::array();
        response["user_single_ticket_num"] = 0;
        response["user_consective_ticket_num"] = 0;
        response["gacha_campaign_info_list"] = nlohmann::json::array();

        // The client does not treat this as an optional notification.  After a
        // successful purchase it rebuilds its GachaResultData from the series
        // snapshot carried by this response (including the selected series'
        // new exchange-point value).  Returning an empty array happened to
        // update the header balance, but left that reconstruction path without
        // a GachaInfoData and caused OnGachaExecute to fault before it could
        // start the result animation.
        auto refreshed = OfflineApiMockBuiltIn::GachaGetSeriesListJsonObj();
        if (refreshed.is_object() && refreshed.contains("gacha_series_list") &&
            refreshed["gacha_series_list"].is_array() &&
            refreshed["gacha_series_list"].size() == 1) {
            const auto prototype = refreshed["gacha_series_list"][0];
            nlohmann::json seriesList = nlohmann::json::array();
            for (const auto& config : kMuseumGachaSeries) {
                const auto seriesState = LoadGachaStateLocked(config.id);
                if (!seriesState) continue;
                auto item = prototype;
                item["gacha_series_id"] = config.id;
                item["user_exchange_point_num"] = seriesState->exchangePoint;
                item["user_series_exec_count"] = seriesState->drawCount;
                item["is_single_enable"] = freeAfter + paidAfter >= 200;
                item["is_consective_enable"] = freeAfter + paidAfter >= 2000;
                item["card_list"] = config.cards;
                seriesList.push_back(std::move(item));
            }
            response["gacha_series_list"] = std::move(seriesList);
        } else {
            // Preserve a schema-valid fallback if a future built-in fixture is
            // changed unexpectedly; an empty list is not a valid success
            // response for this endpoint.
            response["gacha_series_list"] = nlohmann::json::array();
        }
        Log::InfoFmt("[HttpMockBackend] offline gacha purchase draws=%d points=%lld paid=%lld free=%lld",
                     drawCount, static_cast<long long>(pointsAfter),
                     static_cast<long long>(paidAfter), static_cast<long long>(freeAfter));
        return MockStoredResponse{
            response.dump(), GachaHeaders(freeAfter, paidAfter), 200, "OK (offline gacha)"};
    }

    std::optional<MockStoredResponse> HttpMockBackend::Impl::GetGachaHistoryResponseLocked() {
        const auto state = LoadGachaStateLocked();
        if (!state) return std::nullopt;
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(db,
                "SELECT drawn_at, price_type, price_num, results_json, series_id FROM museum_gacha_history "
                "ORDER BY draw_id DESC LIMIT 100;", -1, &stmt, nullptr) != SQLITE_OK || !stmt) {
            if (stmt) sqlite3_finalize(stmt);
            return std::nullopt;
        }
        nlohmann::json list = nlohmann::json::array();
        int rc = SQLITE_ROW;
        while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
            const auto* rawTime = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
            const auto* rawResults = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3));
            const auto results = nlohmann::json::parse(rawResults ? rawResults : "[]", nullptr, false);
            if (!results.is_array()) { sqlite3_finalize(stmt); return std::nullopt; }
            nlohmann::json cards = nlohmann::json::array();
            for (const auto& result : results) {
                cards.push_back({
                    {"order_id", result.value("order_id", 0)},
                    {"card_datas_id", result.value("card_datas_id", 0)},
                    {"gacha_bonus_result_history_list", nlohmann::json::array()},
                });
            }
            list.push_back({
                {"gacha_series_id", sqlite3_column_int(stmt, 4)}, {"gacha_date_time", rawTime ? rawTime : ""},
                {"gacha_price_type", sqlite3_column_int(stmt, 1)},
                {"gacha_price_num", sqlite3_column_int(stmt, 2)},
                {"gacha_result_history_info_list", std::move(cards)},
            });
        }
        sqlite3_finalize(stmt);
        if (rc != SQLITE_DONE) return std::nullopt;
        return MockStoredResponse{
            nlohmann::json{{"gacha_result_history_list", std::move(list)}}.dump(),
            GachaHeaders(state->freeJewels, state->paidJewels), 200, "OK (offline gacha)"};
    }

    std::optional<MockStoredResponse> HttpMockBackend::Impl::GetGachaGuaranteePointListLocked(
            std::string_view payloadJson) {
        const auto payload = nlohmann::json::parse(payloadJson.begin(), payloadJson.end(), nullptr, false);
        if (!payload.is_object() || !payload.contains("gacha_series_id") ||
            !payload["gacha_series_id"].is_number_integer() ||
            !payload.contains("exchange_point_id") ||
            !payload["exchange_point_id"].is_number_integer()) {
            return MockStoredResponse{R"({"error":"invalid exchange request"})", {}, 400,
                                      "Invalid exchange request"};
        }
        const auto* series = FindMuseumGachaSeries(payload["gacha_series_id"].get<int>());
        if (!series || series->exchangePointId != payload["exchange_point_id"].get<int>()) {
            return MockStoredResponse{R"({"error":"unknown exchange pool"})", {}, 400,
                                      "Unknown exchange pool"};
        }
        const auto state = LoadGachaStateLocked(series->id);
        if (!state) return std::nullopt;
        sqlite3_stmt* lookup = nullptr;
        const char* query =
            "SELECT "
            "(SELECT count(*) FROM museum_gacha_exchange_history WHERE exchange_point_rate_id=?), "
            "EXISTS(SELECT 1 FROM museum_card_ownership o JOIN card_detail c "
            "ON c.d_card_datas_id=o.d_card_datas_id WHERE c.card_datas_id=?);";
        if (sqlite3_prepare_v2(db, query, -1, &lookup, nullptr) != SQLITE_OK || !lookup) {
            if (lookup) sqlite3_finalize(lookup);
            return std::nullopt;
        }
        nlohmann::json items = nlohmann::json::array();
        bool valid = true;
        for (const auto& offer : kMuseumExchangeOffers) {
            if (offer.seriesId != series->id) continue;
            sqlite3_reset(lookup);
            sqlite3_clear_bindings(lookup);
            sqlite3_bind_int(lookup, 1, offer.rateId);
            sqlite3_bind_int(lookup, 2, offer.cardId);
            if (sqlite3_step(lookup) != SQLITE_ROW) { valid = false; break; }
            const int used = sqlite3_column_int(lookup, 0);
            items.push_back({
                {"exchange_point_rate_id", offer.rateId},
                // Unlimited offers have LimitedCount=0 in Master. The client
                // always prints this response number; cap its display value.
                {"remaining_exchange_num", offer.limit > 0 ? std::max(0, offer.limit - used) : 9999},
                {"is_having", sqlite3_column_int(lookup, 1) != 0},
            });
        }
        sqlite3_finalize(lookup);
        if (!valid) return std::nullopt;
        return MockStoredResponse{
            nlohmann::json{{"exchange_item_list", std::move(items)}}.dump(),
            GachaHeaders(state->freeJewels, state->paidJewels), 200, "OK (offline gacha exchange)"};
    }

    std::optional<MockStoredResponse> HttpMockBackend::Impl::GachaSetGuaranteePointExchangeLocked(
            std::string_view payloadJson) {
        const auto payload = nlohmann::json::parse(payloadJson.begin(), payloadJson.end(), nullptr, false);
        if (!payload.is_object() || !payload.contains("exchange_point_rate_id") ||
            !payload["exchange_point_rate_id"].is_number_integer()) {
            return MockStoredResponse{R"({"error":"invalid exchange request"})", {}, 400,
                                      "Invalid exchange request"};
        }
        const int rateId = payload["exchange_point_rate_id"].get<int>();
        const auto offer = std::find_if(kMuseumExchangeOffers.begin(), kMuseumExchangeOffers.end(),
                                        [rateId](const auto& item) { return item.rateId == rateId; });
        if (offer == kMuseumExchangeOffers.end() || !FindMuseumGachaSeries(offer->seriesId)) {
            return MockStoredResponse{R"({"error":"unknown exchange item"})", {}, 400,
                                      "Unknown exchange item"};
        }
        const auto* series = FindMuseumGachaSeries(offer->seriesId);
        const auto state = LoadGachaStateLocked(series->id);
        if (!state) return std::nullopt;
        if (state->exchangePoint < offer->price) {
            return MockStoredResponse{R"({"error":"insufficient exchange points"})",
                                      GachaHeaders(state->freeJewels, state->paidJewels), 400,
                                      "Insufficient exchange points"};
        }
        if (!ExecSql(db, "BEGIN IMMEDIATE;")) return std::nullopt;
        const auto rollback = [this]() -> std::optional<MockStoredResponse> {
            ExecSql(db, "ROLLBACK;");
            return std::nullopt;
        };
        sqlite3_stmt* lookup = nullptr;
        if (sqlite3_prepare_v2(db,
                "SELECT c.d_card_datas_id, "
                // NEW = not yet held (same rule as gacha draws).
                "EXISTS(SELECT 1 FROM museum_card_ownership o WHERE o.d_card_datas_id=c.d_card_datas_id), "
                "(SELECT count(*) FROM museum_gacha_exchange_history WHERE exchange_point_rate_id=?) "
                "FROM card_detail c WHERE c.card_datas_id=? LIMIT 1;",
                -1, &lookup, nullptr) != SQLITE_OK || !lookup) {
            if (lookup) sqlite3_finalize(lookup);
            return rollback();
        }
        sqlite3_bind_int(lookup, 1, rateId);
        sqlite3_bind_int(lookup, 2, offer->cardId);
        if (sqlite3_step(lookup) != SQLITE_ROW) {
            sqlite3_finalize(lookup);
            return rollback();
        }
        const auto* rawId = reinterpret_cast<const char*>(sqlite3_column_text(lookup, 0));
        if (!rawId) { sqlite3_finalize(lookup); return rollback(); }
        const std::string dCardId(rawId);
        const bool isNew = sqlite3_column_int(lookup, 1) == 0;
        const int used = sqlite3_column_int(lookup, 2);
        sqlite3_finalize(lookup);
        if (offer->limit > 0 && used >= offer->limit) {
            ExecSql(db, "ROLLBACK;");
            return MockStoredResponse{R"({"error":"exchange limit reached"})",
                                      GachaHeaders(state->freeJewels, state->paidJewels), 400,
                                      "Exchange limit reached"};
        }

        sqlite3_stmt* update = nullptr;
        if (sqlite3_prepare_v2(db,
                "UPDATE museum_gacha_state SET exchange_point=exchange_point-? "
                "WHERE series_id=? AND exchange_point>=?;", -1, &update, nullptr) != SQLITE_OK || !update) {
            if (update) sqlite3_finalize(update);
            return rollback();
        }
        sqlite3_bind_int(update, 1, offer->price);
        sqlite3_bind_int(update, 2, PointOwnerSeriesId(*series));
        sqlite3_bind_int(update, 3, offer->price);
        bool valid = sqlite3_step(update) == SQLITE_DONE && sqlite3_changes(db) == 1;
        sqlite3_finalize(update);
        if (!valid) return rollback();
        sqlite3_stmt* acquired = nullptr;
        if (sqlite3_prepare_v2(db,
                "INSERT OR IGNORE INTO museum_gacha_acquisition "
                "(card_datas_id, first_acquired_at, acquisition_source) "
                "VALUES (?, strftime('%Y-%m-%dT%H:%M:%SZ','now'), 'GACHA_EXCHANGE');",
                -1, &acquired, nullptr) != SQLITE_OK || !acquired) {
            if (acquired) sqlite3_finalize(acquired);
            return rollback();
        }
        sqlite3_bind_int(acquired, 1, offer->cardId);
        valid = sqlite3_step(acquired) == SQLITE_DONE;
        sqlite3_finalize(acquired);
        if (!valid) return rollback();

        sqlite3_stmt* obtain = nullptr;
        if (sqlite3_prepare_v2(db,
                "INSERT OR IGNORE INTO museum_card_ownership "
                "(d_card_datas_id, acquired_at, acquisition_source) "
                "VALUES (?, strftime('%Y-%m-%dT%H:%M:%SZ','now'), 'GACHA_EXCHANGE');",
                -1, &obtain, nullptr) != SQLITE_OK || !obtain) {
            if (obtain) sqlite3_finalize(obtain);
            return rollback();
        }
        valid = BindText(obtain, 1, dCardId) && sqlite3_step(obtain) == SQLITE_DONE;
        sqlite3_finalize(obtain);
        if (!valid) return rollback();
        sqlite3_stmt* cardState = nullptr;
        if (sqlite3_prepare_v2(db,
                "INSERT OR IGNORE INTO museum_card_state "
                "(d_card_datas_id, style_level, limit_break_times, is_evolve_max) "
                "VALUES (?, 1, 0, 0);", -1, &cardState, nullptr) != SQLITE_OK || !cardState) {
            if (cardState) sqlite3_finalize(cardState);
            return rollback();
        }
        valid = BindText(cardState, 1, dCardId) && sqlite3_step(cardState) == SQLITE_DONE;
        sqlite3_finalize(cardState);
        if (!valid) return rollback();
        sqlite3_stmt* history = nullptr;
        if (sqlite3_prepare_v2(db,
                "INSERT INTO museum_gacha_exchange_history "
                "(series_id, exchange_point_rate_id, card_datas_id, point_cost, exchanged_at) "
                "VALUES (?, ?, ?, ?, strftime('%Y-%m-%dT%H:%M:%SZ','now'));",
                -1, &history, nullptr) != SQLITE_OK || !history) {
            if (history) sqlite3_finalize(history);
            return rollback();
        }
        sqlite3_bind_int(history, 1, series->id);
        sqlite3_bind_int(history, 2, rateId);
        sqlite3_bind_int(history, 3, offer->cardId);
        sqlite3_bind_int(history, 4, offer->price);
        valid = sqlite3_step(history) == SQLITE_DONE;
        sqlite3_finalize(history);
        if (valid) {
            const auto exchangeId = sqlite3_last_insert_rowid(db);
            valid = RecordMissionEventLocked("gacha-exchange:" + std::to_string(exchangeId),
                                             "CARD_OBTAIN", offer->quantity);
        }
        if (!valid || !ExecSql(db, "COMMIT;")) return rollback();
        Log::InfoFmt("[HttpMockBackend] offline gacha exchange rate=%d card=%d points=%lld",
                     rateId, offer->cardId,
                     static_cast<long long>(state->exchangePoint - offer->price));
        return MockStoredResponse{
            nlohmann::json{{"receipt_items", nlohmann::json::array({{
                {"get_item_type", 2}, {"get_id", offer->cardId},
                {"get_num", offer->quantity}, {"is_new", isNew}
            }})}}.dump(),
            GachaHeaders(state->freeJewels, state->paidJewels), 200, "OK (offline gacha exchange)"};
    }

    std::optional<MockStoredResponse> HttpMockBackend::Impl::GetCollectionListResponseLocked(
            std::string_view querySql,
            std::string_view responseListKey) {
        if (!initialized && !EnsureReadyLocked()) return std::nullopt;
        if (!db) return std::nullopt;

        // Callers pass only embedded SQL, never request-controlled SQL text.
        const std::string sql(querySql);
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK || !stmt) {
            if (stmt) sqlite3_finalize(stmt);
            Log::ErrorFmt("[HttpMockBackend] collection list prepare failed: %s", sqlite3_errmsg(db));
            return std::nullopt;
        }
        nlohmann::json values = nlohmann::json::array();
        int rc = SQLITE_ROW;
        while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
            const auto* raw = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
            auto parsed = nlohmann::json::parse(raw ? raw : "{}", nullptr, false);
            if (!parsed.is_object()) {
                sqlite3_finalize(stmt);
                Log::Error("[HttpMockBackend] invalid collection catalog JSON");
                return std::nullopt;
            }
            if (responseListKey == "music_info_list") {
                parsed["is_mastery"] = sqlite3_column_int(stmt, 1) != 0;
                parsed["play_count"] = sqlite3_column_int64(stmt, 2);
                const auto* latest = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3));
                parsed["latest_play_time"] = latest ? latest : "0001-01-01T00:00:00Z";
            } else if (responseListKey == "sticker_info_list") {
                parsed["is_owned"] = sqlite3_column_int(stmt, 1) != 0;
                parsed["requirement_progress"] = sqlite3_column_int64(stmt, 2);
                parsed["variant_requirement_progress"] = sqlite3_column_int64(stmt, 3);
                parsed["is_available_variant"] = sqlite3_column_int(stmt, 4) != 0;
            }
            values.push_back(std::move(parsed));
        }
        sqlite3_finalize(stmt);
        if (rc != SQLITE_DONE) return std::nullopt;
        nlohmann::json result;
        result[std::string(responseListKey)] = std::move(values);
        return MockStoredResponse{result.dump(), {}, 200, "OK (offline mock)"};
    }

    std::optional<MockStoredResponse> HttpMockBackend::Impl::GetItemDetailByDItemIdLocked(std::string_view dItemDatasId) {
        if (!initialized && !EnsureReadyLocked()) {
            return std::nullopt;
        }

        if (!db || dItemDatasId.empty()) {
            return std::nullopt;
        }

        sqlite3_stmt* stmt = nullptr;
        const char* sql = HttpMockBackendBuiltInSql::QueriesItemDetailSqlView.data();
        if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK || !stmt) {
            Log::ErrorFmt("[HttpMockBackend] sqlite prepare failed for item query: %s", sqlite3_errmsg(db));
            if (stmt) sqlite3_finalize(stmt);
            return std::nullopt;
        }

        if (!BindText(stmt, 1, dItemDatasId)) {
            Log::ErrorFmt("[HttpMockBackend] sqlite bind failed for item query: %s", sqlite3_errmsg(db));
            sqlite3_finalize(stmt);
            return std::nullopt;
        }

        const int rc = sqlite3_step(stmt);
        if (rc != SQLITE_ROW) {
            sqlite3_finalize(stmt);
            return std::nullopt;
        }

        const auto* responseJson = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        auto result = nlohmann::json::parse(responseJson ? responseJson : "{}", nullptr, false);
        if (!result.is_object()) {
            sqlite3_finalize(stmt);
            return std::nullopt;
        }
        result["item_num"] = sqlite3_column_int64(stmt, 1);
        MockStoredResponse response;
        response.body = result.dump();
        sqlite3_finalize(stmt);
        return response;
    }

    std::optional<MockStoredResponse> HttpMockBackend::Impl::GetItemListResponseLocked() {
        if (!initialized && !EnsureReadyLocked()) {
            return std::nullopt;
        }
        if (!db) {
            return std::nullopt;
        }

        sqlite3_stmt* stmt = nullptr;
        const char* sql = HttpMockBackendBuiltInSql::QueriesItemListSqlView.data();
        if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK || !stmt) {
            Log::ErrorFmt("[HttpMockBackend] sqlite prepare failed for item list: %s", sqlite3_errmsg(db));
            if (stmt) sqlite3_finalize(stmt);
            return std::nullopt;
        }

        nlohmann::json categories = nlohmann::json::array();
        int currentCategory = 0;
        bool hasCategory = false;
        nlohmann::json items = nlohmann::json::array();
        auto flushCategory = [&]() {
            if (hasCategory) {
                categories.push_back({
                    {"item_category", currentCategory},
                    {"user_item_list", std::move(items)},
                });
                items = nlohmann::json::array();
            }
        };

        int rc = SQLITE_ROW;
        while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
            const int category = sqlite3_column_int(stmt, 3);
            if (!hasCategory || category != currentCategory) {
                flushCategory();
                currentCategory = category;
                hasCategory = true;
            }
            const auto* id = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
            const auto* resource = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 6));
            items.push_back({
                {"user_item_id", id ? id : ""},
                {"item_id", sqlite3_column_int64(stmt, 1)},
                {"item_type", sqlite3_column_int(stmt, 2)},
                {"rarity", sqlite3_column_int(stmt, 4)},
                {"item_num", sqlite3_column_int64(stmt, 5)},
                {"resource_file_name", resource ? resource : ""},
            });
        }
        if (rc != SQLITE_DONE) {
            Log::ErrorFmt("[HttpMockBackend] sqlite item list step failed: %s", sqlite3_errmsg(db));
            sqlite3_finalize(stmt);
            return std::nullopt;
        }
        flushCategory();
        sqlite3_finalize(stmt);

        nlohmann::json result;
        result["item_category_list"] = std::move(categories);
        return MockStoredResponse{result.dump(), {}, 200, "OK (offline mock)"};
    }

    std::optional<MockStoredResponse> HttpMockBackend::Impl::GetItemExchangeListResponseLocked(
            std::string_view payloadJson) {
        if (!initialized && !EnsureReadyLocked()) return std::nullopt;
        if (!db) return std::nullopt;
        if (!ExecSql(db, "CREATE TABLE IF NOT EXISTS offline_item_exchange_purchase ("
                         "item_exchanges_id INTEGER PRIMARY KEY, purchased_num INTEGER NOT NULL DEFAULT 0);"))
            return std::nullopt;
        const auto categoryText = ExtractJsonIntegerFieldAsString(payloadJson, "category_list_id");
        if (categoryText.empty()) return std::nullopt;
        int categoryId = 0;
        const auto parsed = std::from_chars(categoryText.data(), categoryText.data() + categoryText.size(), categoryId);
        if (parsed.ec != std::errc{} || parsed.ptr != categoryText.data() + categoryText.size()) return std::nullopt;
        const auto catalog = OfflineApiMockBuiltIn::ItemExchangeCatalogJsonObj();
        if (!catalog.is_array()) return std::nullopt;
        sqlite3_stmt* count = nullptr;
        if (sqlite3_prepare_v2(db,
                               // Holdings are player state: read the same inventory that
                               // /v1/user/items/get_list shows, not the catalog seed.
                               "SELECT COALESCE(SUM(inventory.item_num),0) FROM item INNER JOIN museum_item_inventory AS inventory "
                               "USING(d_item_datas_id) WHERE item.item_id=?;",
                               -1, &count, nullptr) != SQLITE_OK || !count) {
            if (count) sqlite3_finalize(count);
            return std::nullopt;
        }
        const auto owned = [count](int itemId) -> int64_t {
            sqlite3_reset(count);
            sqlite3_clear_bindings(count);
            sqlite3_bind_int(count, 1, itemId);
            return sqlite3_step(count) == SQLITE_ROW ? sqlite3_column_int64(count, 0) : 0;
        };
        sqlite3_stmt* history = nullptr;
        if (sqlite3_prepare_v2(db, "SELECT purchased_num FROM offline_item_exchange_purchase "
                                   "WHERE item_exchanges_id=?;", -1, &history, nullptr) != SQLITE_OK || !history) {
            if (history) sqlite3_finalize(history);
            sqlite3_finalize(count);
            return std::nullopt;
        }
        nlohmann::json entries = nlohmann::json::array();
        for (const auto& row : catalog) {
            if (row.value("category_list_id", 0) != categoryId) continue;
            sqlite3_reset(history);
            sqlite3_clear_bindings(history);
            sqlite3_bind_int(history, 1, row.value("id", 0));
            const int used = sqlite3_step(history) == SQLITE_ROW ? sqlite3_column_int(history, 0) : 0;
            const int limit = row.value("limit_num", 0);
            entries.push_back({
                {"item_exchanges_id", row.value("id", 0)},
                {"user_product_item_num", owned(row.value("product_item_id", 0))},
                {"user_material_item_num", owned(row.value("material_item_id", 0))},
                {"remaining_exchange_num", limit > 0 ? std::max(0, limit - used) : 0},
                {"is_lock", row.value("unlock_conditions", 0) != 0},
            });
        }
        sqlite3_finalize(history);
        sqlite3_finalize(count);
        nlohmann::json response = {
            {"item_exchange_list", std::move(entries)},
            {"is_convertible_limit_break_material", false},
        };
        return MockStoredResponse{response.dump(), {}, 200, "OK (offline mock)"};
    }

    std::optional<MockStoredResponse> HttpMockBackend::Impl::ItemExchangeSetPurchaseLocked(
            std::string_view payloadJson) {
        if (!initialized && !EnsureReadyLocked()) return std::nullopt;
        if (!db) return std::nullopt;
        const auto payload = nlohmann::json::parse(payloadJson.begin(), payloadJson.end(), nullptr, false);
        if (!payload.is_object() || !payload.contains("item_exchanges_id") ||
            !payload["item_exchanges_id"].is_number_integer() || !payload.contains("num") ||
            !payload["num"].is_number_integer()) {
            return MockStoredResponse{R"({"error":"invalid item exchange request"})", {}, 400,
                                      "Invalid item exchange request"};
        }
        const int64_t exchangeId64 = payload["item_exchanges_id"].get<int64_t>();
        const int64_t quantity64 = payload["num"].get<int64_t>();
        if (exchangeId64 < 1 || exchangeId64 > std::numeric_limits<int>::max() ||
            quantity64 < 1 || quantity64 > 100) {
            return MockStoredResponse{R"({"error":"invalid exchange quantity"})", {}, 400,
                                      "Invalid exchange quantity"};
        }
        const int exchangeId = static_cast<int>(exchangeId64);
        const int quantity = static_cast<int>(quantity64);
        const auto catalog = OfflineApiMockBuiltIn::ItemExchangeCatalogJsonObj();
        const nlohmann::json* offer = nullptr;
        if (catalog.is_array()) {
            for (const auto& row : catalog) {
                if (row.value("id", 0) == exchangeId) { offer = &row; break; }
            }
        }
        if (!offer || offer->value("unlock_conditions", 0) != 0) {
            return MockStoredResponse{R"({"error":"exchange item unavailable"})", {}, 400,
                                      "Exchange item unavailable"};
        }
        const int productId = offer->value("product_item_id", 0);
        const int materialId = offer->value("material_item_id", 0);
        const int productEach = offer->value("product_item_num", 0);
        const int materialEach = offer->value("material_item_num", 0);
        const int limit = offer->value("limit_num", 0);
        if (productId <= 0 || materialId <= 0 || productEach <= 0 || materialEach <= 0 ||
            productId == materialId) return std::nullopt;
        if (!ExecSql(db, "CREATE TABLE IF NOT EXISTS offline_item_exchange_purchase ("
                         "item_exchanges_id INTEGER PRIMARY KEY, purchased_num INTEGER NOT NULL DEFAULT 0);"))
            return std::nullopt;
        if (!ExecSql(db, "BEGIN IMMEDIATE;")) return std::nullopt;
        const auto rollback = [this]() -> std::optional<MockStoredResponse> {
            ExecSql(db, "ROLLBACK;");
            return std::nullopt;
        };
        const auto scalar = [this](const char* sql, int key) -> std::optional<int64_t> {
            sqlite3_stmt* stmt = nullptr;
            if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK || !stmt) {
                if (stmt) sqlite3_finalize(stmt);
                return std::nullopt;
            }
            sqlite3_bind_int(stmt, 1, key);
            const auto result = sqlite3_step(stmt) == SQLITE_ROW
                ? std::optional<int64_t>(sqlite3_column_int64(stmt, 0)) : std::nullopt;
            sqlite3_finalize(stmt);
            return result;
        };
        const auto countSql =
            "SELECT COALESCE(SUM(inventory.item_num),0) FROM item INNER JOIN museum_item_inventory AS inventory "
            "USING(d_item_datas_id) WHERE item.item_id=?;";
        const auto ownedMaterial = scalar(countSql, materialId);
        const auto ownedProduct = scalar(countSql, productId);
        const auto used = scalar("SELECT COALESCE((SELECT purchased_num FROM offline_item_exchange_purchase "
                                 "WHERE item_exchanges_id=?),0);", exchangeId);
        if (!ownedMaterial || !ownedProduct || !used) return rollback();
        const int64_t materialCost = int64_t(materialEach) * quantity;
        const int64_t productGain = int64_t(productEach) * quantity;
        if (*ownedMaterial < materialCost || (limit > 0 && *used + quantity > limit)) {
            ExecSql(db, "ROLLBACK;");
            return MockStoredResponse{R"({"error":"insufficient materials or exchange limit reached"})",
                                      {}, 400, "Exchange unavailable"};
        }
        const auto changeItem = [this](const char* sql, int64_t amount, int itemId) -> bool {
            sqlite3_stmt* stmt = nullptr;
            if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK || !stmt) {
                if (stmt) sqlite3_finalize(stmt);
                return false;
            }
            sqlite3_bind_int64(stmt, 1, amount);
            sqlite3_bind_int(stmt, 2, itemId);
            bool ok = sqlite3_step(stmt) == SQLITE_DONE && sqlite3_changes(db) == 1;
            sqlite3_finalize(stmt);
            return ok;
        };
        if (!changeItem("UPDATE museum_item_inventory SET item_num=item_num-? WHERE d_item_datas_id="
                        "(SELECT d_item_datas_id FROM item WHERE item_id=? LIMIT 1);",
                        materialCost, materialId) ||
            !changeItem("INSERT INTO museum_item_inventory(d_item_datas_id, item_num) "
                        "SELECT d_item_datas_id, ?1 FROM item WHERE item_id=?2 LIMIT 1 "
                        "ON CONFLICT(d_item_datas_id) DO UPDATE SET item_num=item_num+excluded.item_num;",
                        productGain, productId)) return rollback();
        sqlite3_stmt* history = nullptr;
        if (sqlite3_prepare_v2(db,
                "INSERT INTO offline_item_exchange_purchase(item_exchanges_id,purchased_num) "
                "VALUES(?,?) ON CONFLICT(item_exchanges_id) DO UPDATE SET "
                "purchased_num=purchased_num+excluded.purchased_num;",
                -1, &history, nullptr) != SQLITE_OK || !history) {
            if (history) sqlite3_finalize(history);
            return rollback();
        }
        sqlite3_bind_int(history, 1, exchangeId);
        sqlite3_bind_int(history, 2, quantity);
        const bool recorded = sqlite3_step(history) == SQLITE_DONE;
        sqlite3_finalize(history);
        if (!recorded || !ExecSql(db, "COMMIT;")) return rollback();
        return MockStoredResponse{nlohmann::json{
            {"user_product_item_num", *ownedProduct + productGain},
            {"user_material_item_num", *ownedMaterial - materialCost},
        }.dump(), {}, 200, "OK (offline item exchange)"};
    }

    std::optional<MockStoredResponse> HttpMockBackend::Impl::GetCharacterInfoByIdLocked(std::string_view characterId) {
        if (!initialized && !EnsureReadyLocked()) {
            return std::nullopt;
        }

        if (!db || characterId.empty()) {
            return std::nullopt;
        }

        sqlite3_stmt* stmt = nullptr;
        constexpr const char* sql =
            "SELECT response_json "
            "FROM character_info "
            "WHERE character_id = ?;";
        if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK || !stmt) {
            Log::ErrorFmt("[HttpMockBackend] sqlite prepare failed for character_info query: %s", sqlite3_errmsg(db));
            if (stmt) sqlite3_finalize(stmt);
            return std::nullopt;
        }

        if (!BindText(stmt, 1, characterId)) {
            Log::ErrorFmt("[HttpMockBackend] sqlite bind failed for character_info query: %s", sqlite3_errmsg(db));
            sqlite3_finalize(stmt);
            return std::nullopt;
        }

        const int rc = sqlite3_step(stmt);
        if (rc != SQLITE_ROW) {
            sqlite3_finalize(stmt);
            return std::nullopt;
        }

        const auto* responseJson = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        MockStoredResponse response;
        response.body = responseJson ? responseJson : "{}";
        sqlite3_finalize(stmt);
        return response;
    }

    std::optional<MockStoredResponse> HttpMockBackend::Impl::GetDeckListResponseLocked() {
        if (!initialized && !EnsureReadyLocked()) {
            return std::nullopt;
        }
        if (!db) {
            return std::nullopt;
        }

        nlohmann::json deckList = nlohmann::json::array();

        sqlite3_stmt* stmt = nullptr;
        constexpr const char* sql =
            "SELECT d_deck_datas_id, deck_name, deck_no, generations_id, ace_card, deck_cards_json "
            "FROM deck ORDER BY deck_no;";
        if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK || !stmt) {
            Log::ErrorFmt("[HttpMockBackend] sqlite prepare failed for deck list: %s", sqlite3_errmsg(db));
            if (stmt) sqlite3_finalize(stmt);
            return std::nullopt;
        }

        while (sqlite3_step(stmt) == SQLITE_ROW) {
            const auto* id = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
            const auto* name = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
            const int deckNo = sqlite3_column_int(stmt, 2);
            const int genId = sqlite3_column_int(stmt, 3);
            const auto* aceCard = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 4));
            const auto* cardsJson = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 5));

            nlohmann::json deck;
            deck["d_deck_datas_id"] = id ? id : "";
            deck["deck_name"] = name ? name : "";
            deck["deck_no"] = deckNo;
            deck["generations_id"] = genId;

            const std::string aceStr = aceCard ? aceCard : "";
            if (!aceStr.empty()) {
                deck["ace_card"] = aceStr;
            }

            auto cardsParsed = nlohmann::json::parse(cardsJson ? cardsJson : "[]", nullptr, false);
            deck["deck_cards_list"] = cardsParsed.is_array() ? cardsParsed : nlohmann::json::array();

            deckList.push_back(std::move(deck));
        }
        sqlite3_finalize(stmt);

        nlohmann::json result;
        result["deck_list"] = std::move(deckList);

        sqlite3_stmt* cardsStmt = nullptr;
        constexpr const char* cardsSql =
            "SELECT card_detail.response_json, museum_card_state.style_level, "
            "museum_card_state.limit_break_times, museum_card_state.is_evolve_max, "
            "COALESCE(offline_card_growth.evolve_times, 0) "
            "FROM card_detail "
            "INNER JOIN museum_card_ownership "
            "ON museum_card_ownership.d_card_datas_id = card_detail.d_card_datas_id "
            "INNER JOIN museum_card_state "
            "ON museum_card_state.d_card_datas_id = museum_card_ownership.d_card_datas_id "
            "LEFT JOIN offline_card_growth "
            "ON offline_card_growth.d_card_datas_id = card_detail.d_card_datas_id "
            "ORDER BY card_detail.card_datas_id;";
        if (sqlite3_prepare_v2(db, cardsSql, -1, &cardsStmt, nullptr) != SQLITE_OK || !cardsStmt) {
            if (cardsStmt) sqlite3_finalize(cardsStmt);
            return std::nullopt;
        }
        nlohmann::json userCards = nlohmann::json::array();
        int cardRc = SQLITE_ROW;
        while ((cardRc = sqlite3_step(cardsStmt)) == SQLITE_ROW) {
            const auto* raw = reinterpret_cast<const char*>(sqlite3_column_text(cardsStmt, 0));
            auto parsed = ComposeOwnedCardResponse(
                raw,
                sqlite3_column_int(cardsStmt, 1),
                sqlite3_column_int(cardsStmt, 2),
                sqlite3_column_int(cardsStmt, 3) != 0,
                sqlite3_column_int(cardsStmt, 4));
            if (!parsed) {
                sqlite3_finalize(cardsStmt);
                return std::nullopt;
            }
            userCards.push_back(std::move((*parsed)["user_card_data"]));
        }
        sqlite3_finalize(cardsStmt);
        if (cardRc != SQLITE_DONE) return std::nullopt;
        result["user_card_data_list"] = std::move(userCards);
        result["rental_deck_list"] = nlohmann::json::array();
        result["rental_card_data_list"] = nlohmann::json::array();

        MockStoredResponse response;
        response.body = result.dump();
        return response;
    }

    std::optional<MockStoredResponse> HttpMockBackend::Impl::ModifyDeckListLocked(std::string_view payloadJson) {
        if (!initialized && !EnsureReadyLocked()) {
            return std::nullopt;
        }
        if (!db || payloadJson.empty()) {
            return std::nullopt;
        }

        auto payload = nlohmann::json::parse(payloadJson.begin(), payloadJson.end(), nullptr, false);
        if (!payload.is_object() || !payload.contains("modify_deck_list") || !payload["modify_deck_list"].is_array()) {
            return std::nullopt;
        }

        nlohmann::json responseDeckList = nlohmann::json::array();

        if (!ExecSql(db, "BEGIN IMMEDIATE;")) {
            return std::nullopt;
        }

        constexpr const char* upsertSql =
            "INSERT OR REPLACE INTO deck (d_deck_datas_id, deck_name, deck_no, generations_id, ace_card, deck_cards_json) "
            "VALUES (?, ?, ?, ?, ?, ?);";

        auto failValidation = [](std::string_view reason) -> std::optional<MockStoredResponse> {
            nlohmann::json failure;
            failure["result"] = false;
            failure["error"] = std::string(reason);
            return MockStoredResponse{failure.dump(), {}, 200, "Rejected offline deck update"};
        };

        auto ownsCard = [this](std::string_view cardId) -> bool {
            sqlite3_stmt* ownership = nullptr;
            constexpr const char* ownershipSql =
                "SELECT 1 FROM museum_card_ownership WHERE d_card_datas_id = ? LIMIT 1;";
            if (sqlite3_prepare_v2(db, ownershipSql, -1, &ownership, nullptr) != SQLITE_OK || !ownership) {
                if (ownership) sqlite3_finalize(ownership);
                return false;
            }
            const bool owned = BindText(ownership, 1, cardId) && sqlite3_step(ownership) == SQLITE_ROW;
            sqlite3_finalize(ownership);
            return owned;
        };

        for (auto& deckEntry : payload["modify_deck_list"]) {
            if (!deckEntry.is_object()) {
                ExecSql(db, "ROLLBACK;");
                return failValidation("deck entry must be an object");
            }
            std::string deckId = deckEntry.value("d_deck_datas_id", std::string{});
            if (deckId.empty()) {
                deckId = GenerateUuid4();
            }

            nlohmann::json cardsList = nlohmann::json::array();
            std::unordered_set<std::string> uniqueCards;
            std::unordered_set<std::string> deckCardIds;
            if (deckEntry.contains("deck_cards_list") && deckEntry["deck_cards_list"].is_array()) {
                cardsList = deckEntry["deck_cards_list"];
                for (auto& card : cardsList) {
                    if (!card.is_object()) {
                        ExecSql(db, "ROLLBACK;");
                        return failValidation("deck card must be an object");
                    }
                    const std::string ownedCardId = card.value("d_card_datas_id", std::string{});
                    // The 5.1 client sends an unassigned slot (for example the
                    // FREE slot) as an empty card id. It is part of the deck
                    // layout, not a card reference, so persist it unchanged.
                    if (!ownedCardId.empty() && !ownsCard(ownedCardId)) {
                        ExecSql(db, "ROLLBACK;");
                        return failValidation("deck contains a card that is not owned by the Museum player");
                    }
                    if (!ownedCardId.empty() && !uniqueCards.insert(ownedCardId).second) {
                        ExecSql(db, "ROLLBACK;");
                        return failValidation("deck contains the same card more than once");
                    }
                    std::string cardId = card.value("d_deck_cards_id", std::string{});
                    if (cardId.empty()) {
                        cardId = GenerateUuid4();
                        card["d_deck_cards_id"] = cardId;
                    }
                    if (!deckCardIds.insert(cardId).second) {
                        ExecSql(db, "ROLLBACK;");
                        return failValidation("deck contains duplicate deck-card identifiers");
                    }
                }
            } else if (deckEntry.contains("deck_cards_list")) {
                ExecSql(db, "ROLLBACK;");
                return failValidation("deck_cards_list must be an array");
            }

            const std::string deckName = deckEntry.value("deck_name", std::string{});
            const int deckNo = deckEntry.value("deck_no", 0);
            const int genId = deckEntry.value("generations_id", 0);
            if (deckNo < 0) {
                ExecSql(db, "ROLLBACK;");
                return failValidation("deck_no must not be negative");
            }
            std::string aceCard;
            if (deckEntry.contains("ace_card")) {
                aceCard = deckEntry.value("ace_card", std::string{});
            } else {
                sqlite3_stmt* aceStmt = nullptr;
                constexpr const char* aceSql = "SELECT ace_card FROM deck WHERE d_deck_datas_id = ?;";
                if (sqlite3_prepare_v2(db, aceSql, -1, &aceStmt, nullptr) == SQLITE_OK && aceStmt) {
                    BindText(aceStmt, 1, deckId);
                    if (sqlite3_step(aceStmt) == SQLITE_ROW) {
                        const auto* val = reinterpret_cast<const char*>(sqlite3_column_text(aceStmt, 0));
                        if (val) aceCard = val;
                    }
                    sqlite3_finalize(aceStmt);
                }
            }
            if (!aceCard.empty() && !uniqueCards.contains(aceCard) && !deckCardIds.contains(aceCard)) {
                ExecSql(db, "ROLLBACK;");
                return failValidation("ace_card must refer to a card in the deck");
            }
            const std::string cardsJsonStr = cardsList.dump();

            sqlite3_stmt* stmt = nullptr;
            if (sqlite3_prepare_v2(db, upsertSql, -1, &stmt, nullptr) != SQLITE_OK || !stmt) {
                Log::ErrorFmt("[HttpMockBackend] sqlite prepare failed for deck upsert: %s", sqlite3_errmsg(db));
                if (stmt) sqlite3_finalize(stmt);
                ExecSql(db, "ROLLBACK;");
                return std::nullopt;
            }

            const bool bound =
                BindText(stmt, 1, deckId) &&
                BindText(stmt, 2, deckName) &&
                sqlite3_bind_int(stmt, 3, deckNo) == SQLITE_OK &&
                sqlite3_bind_int(stmt, 4, genId) == SQLITE_OK &&
                BindText(stmt, 5, aceCard) &&
                BindText(stmt, 6, cardsJsonStr);
            if (!bound) {
                Log::ErrorFmt("[HttpMockBackend] sqlite bind failed for deck upsert: %s", sqlite3_errmsg(db));
                sqlite3_finalize(stmt);
                ExecSql(db, "ROLLBACK;");
                return std::nullopt;
            }

            if (sqlite3_step(stmt) != SQLITE_DONE) {
                Log::ErrorFmt("[HttpMockBackend] sqlite step failed for deck upsert: %s", sqlite3_errmsg(db));
                sqlite3_finalize(stmt);
                ExecSql(db, "ROLLBACK;");
                return std::nullopt;
            }
            sqlite3_finalize(stmt);

            nlohmann::json responseDeck;
            responseDeck["d_deck_datas_id"] = deckId;
            responseDeck["deck_name"] = deckName;
            responseDeck["deck_no"] = deckNo;
            responseDeck["generations_id"] = genId;
            if (!aceCard.empty()) {
                responseDeck["ace_card"] = aceCard;
            }
            responseDeck["deck_cards_list"] = cardsList;

            responseDeckList.push_back(std::move(responseDeck));
        }

        if (!ExecSql(db, "COMMIT;")) {
            ExecSql(db, "ROLLBACK;");
            return std::nullopt;
        }

        nlohmann::json result;
        result["deck_list"] = std::move(responseDeckList);

        MockStoredResponse response;
        response.body = result.dump();
        return response;
    }

    std::optional<MockStoredResponse> HttpMockBackend::Impl::GetRhythmGameHomeLocked() {
        if (!initialized && !EnsureReadyLocked()) {
            return std::nullopt;
        }
        if (!db) {
            return std::nullopt;
        }

        const auto& musicListObj = OfflineApiMockBuiltIn::CollectionGetMusicListJsonObj();
        nlohmann::json musicList = nlohmann::json::array();

        if (musicListObj.contains("music_info_list") && musicListObj["music_info_list"].is_array()) {
            for (const auto& mi : musicListObj["music_info_list"]) {
                const int musicId = mi.value("musics_id", 0);

                nlohmann::json entry;
                entry["music_id"] = musicId;
                entry["high_score"] = 0;
                entry["high_score_achievement_status"] = 0;
                entry["music_mastery_level"] = 1;
                entry["music_scores"] = nlohmann::json::array();

                sqlite3_stmt* stmt = nullptr;
                constexpr const char* sql =
                    "SELECT high_score, high_score_achievement_status, "
                    "music_mastery_level, difficulty_scores_json "
                    "FROM rhythm_music_score WHERE music_id = ?;";
                if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) == SQLITE_OK && stmt) {
                    sqlite3_bind_int(stmt, 1, musicId);
                    if (sqlite3_step(stmt) == SQLITE_ROW) {
                        entry["high_score"] = sqlite3_column_int(stmt, 0);
                        entry["high_score_achievement_status"] = sqlite3_column_int(stmt, 1);
                        entry["music_mastery_level"] = sqlite3_column_int(stmt, 2);

                        const auto* dsJson = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3));
                        auto parsed = nlohmann::json::parse(dsJson ? dsJson : "[]", nullptr, false);
                        entry["music_scores"] = parsed.is_array() ? parsed : nlohmann::json::array();
                    }
                    sqlite3_finalize(stmt);
                }

                musicList.push_back(std::move(entry));
            }
        }

        nlohmann::json deckList = nlohmann::json::array();
        {
            sqlite3_stmt* stmt = nullptr;
            constexpr const char* sql =
                "SELECT rhythm_game_deck_id, name, deck_no, deck_card_list_json "
                "FROM rhythm_game_deck ORDER BY deck_no;";
            if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) == SQLITE_OK && stmt) {
                while (sqlite3_step(stmt) == SQLITE_ROW) {
                    const auto* id = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
                    const auto* name = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
                    const int deckNo = sqlite3_column_int(stmt, 2);
                    const auto* cardsJson = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3));

                    nlohmann::json deck;
                    deck["rhythm_game_deck_id"] = id ? id : "";
                    deck["name"] = name ? name : "";
                    deck["deck_no"] = deckNo;
                    auto parsed = nlohmann::json::parse(cardsJson ? cardsJson : "[]", nullptr, false);
                    deck["deck_card_list"] = parsed.is_array() ? parsed : nlohmann::json::array();
                    deckList.push_back(std::move(deck));
                }
                sqlite3_finalize(stmt);
            }
        }

        const auto& cardListObj = OfflineApiMockBuiltIn::UserCardGetListJsonObj();
        nlohmann::json cardDataList = nlohmann::json::array();
        if (cardListObj.contains("user_card_data_list")) {
            cardDataList = cardListObj["user_card_data_list"];
        }
        for (auto& card : cardDataList) {
            if (!card.contains("character_bonus") || card["character_bonus"].empty()) {
                card["character_bonus"] = kDefaultCharacterBonus;
            }
            if (!card.contains("rhythm_game_skill_list")) {
                card["rhythm_game_skill_list"] = nlohmann::json::array();
            }
        }

        const auto& profileObj = OfflineApiMockBuiltIn::HomeGetHomeJsonObj();
        nlohmann::json memberFanlevelList = nlohmann::json::array();
        if (profileObj.contains("profile_info") && profileObj["profile_info"].contains("fan_level_list")) {
            for (const auto& fl : profileObj["profile_info"]["fan_level_list"]) {
                nlohmann::json mfl;
                mfl["characters_id"] = fl.value("character_id", 0);
                mfl["member_fanlevel"] = fl.value("member_fan_level", 0);
                memberFanlevelList.push_back(std::move(mfl));
            }
        }

        int64_t totalClearCount = 0;
        int64_t totalScoreAccum = 0;
        int64_t totalHighScore = 0;
        int starTotal = 0;
        {
            sqlite3_stmt* stmt = nullptr;
            constexpr const char* sql =
                "SELECT COALESCE(SUM(clear_count),0), COALESCE(SUM(total_score_accumulated),0), "
                "COALESCE(SUM(high_score),0), high_score_achievement_status, difficulty_scores_json "
                "FROM rhythm_music_score;";
            constexpr const char* allSql =
                "SELECT high_score_achievement_status, difficulty_scores_json "
                "FROM rhythm_music_score;";
            if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) == SQLITE_OK && stmt) {
                if (sqlite3_step(stmt) == SQLITE_ROW) {
                    totalClearCount = sqlite3_column_int64(stmt, 0);
                    totalScoreAccum = sqlite3_column_int64(stmt, 1);
                    totalHighScore = sqlite3_column_int64(stmt, 2);
                }
                sqlite3_finalize(stmt);
            }
            stmt = nullptr;
            if (sqlite3_prepare_v2(db, allSql, -1, &stmt, nullptr) == SQLITE_OK && stmt) {
                while (sqlite3_step(stmt) == SQLITE_ROW) {
                    starTotal += sqlite3_column_int(stmt, 0);
                    const auto* dsJson = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
                    auto ds = nlohmann::json::parse(dsJson ? dsJson : "[]", nullptr, false);
                    if (ds.is_array()) {
                        for (const auto& d : ds) {
                            starTotal += d.value("combo_achievement_status", 0);
                            starTotal += d.value("clear_lamp", 0);
                        }
                    }
                }
                sqlite3_finalize(stmt);
            }
        }

        nlohmann::json result;
        result["music_list"] = std::move(musicList);
        result["rhythm_game_deck_list"] = std::move(deckList);
        result["card_data_list"] = std::move(cardDataList);
        result["class_mission_list"] = nlohmann::json::array({
            {{"condition_type", 1}, {"progress_num", totalClearCount}, {"received_order", 99}},
            {{"condition_type", 2}, {"progress_num", totalScoreAccum}, {"received_order", 99}},
            {{"condition_type", 3}, {"progress_num", totalHighScore}, {"received_order", 99}},
        });
        result["received_total_mission_order"] = 99;
        result["rhythm_game_star_total_count"] = starTotal;
        result["member_fanlevel_list"] = std::move(memberFanlevelList);
        result["friend_card_list"] = nlohmann::json::array();
        result["auto_play_ticket_info"] = {
            {"num", 10}, {"max", 10}, {"next_reset_time", "2023-04-15T00:00:00Z"}
        };

        const auto debugPath = GetBackendDatabasePath().parent_path() / "rhythm_game_home.json";
        {
            std::ofstream ofs(debugPath, std::ios::trunc);
            if (ofs.is_open()) {
                ofs << result.dump(2);
            }
        }

        MockStoredResponse response;
        response.body = result.dump();
        return response;
    }

    std::optional<MockStoredResponse> HttpMockBackend::Impl::RhythmGameSetStartLocked(std::string_view payloadJson) {
        if (!initialized && !EnsureReadyLocked()) {
            return std::nullopt;
        }
        if (!db || payloadJson.empty()) {
            return std::nullopt;
        }
        auto payload = nlohmann::json::parse(payloadJson.begin(), payloadJson.end(), nullptr, false);
        if (!payload.is_object()) {
            return std::nullopt;
        }
        const int musicId = payload.value("music_id", 0);
        const int difficulty = payload.value("music_score_difficulty", 1);
        if (musicId <= 0 || difficulty <= 0) {
            return std::nullopt;
        }

        sqlite3_stmt* existing = nullptr;
        constexpr const char* existingSql =
            "SELECT music_id, difficulty, state FROM rhythm_runtime_session "
            "WHERE singleton_id = 1;";
        if (sqlite3_prepare_v2(db, existingSql, -1, &existing, nullptr) == SQLITE_OK && existing) {
            if (sqlite3_step(existing) == SQLITE_ROW &&
                sqlite3_column_int(existing, 0) == musicId &&
                sqlite3_column_int(existing, 1) == difficulty) {
                const auto* rawState = reinterpret_cast<const char*>(sqlite3_column_text(existing, 2));
                const bool playing = rawState && std::string_view(rawState) == "PLAYING";
                sqlite3_finalize(existing);
                if (playing) {
                    MockStoredResponse replay;
                    replay.body = "null";
                    return replay;
                }
            } else {
                sqlite3_finalize(existing);
            }
        } else if (existing) {
            sqlite3_finalize(existing);
        }

        constexpr const char* startSql =
            "INSERT OR REPLACE INTO rhythm_runtime_session "
            "(singleton_id, session_id, music_id, difficulty, state, "
            "start_payload_json, finish_payload_json, result_json, updated_at) "
            "VALUES (1, ?, ?, ?, 'PLAYING', ?, NULL, NULL, CURRENT_TIMESTAMP);";
        sqlite3_stmt* start = nullptr;
        if (sqlite3_prepare_v2(db, startSql, -1, &start, nullptr) != SQLITE_OK || !start) {
            Log::ErrorFmt("[HttpMockBackend] rhythm session start prepare failed: %s", sqlite3_errmsg(db));
            if (start) sqlite3_finalize(start);
            return std::nullopt;
        }
        const std::string sessionId = GenerateUuid4();
        const std::string startPayload = payload.dump();
        const bool bound =
            BindText(start, 1, sessionId) &&
            sqlite3_bind_int(start, 2, musicId) == SQLITE_OK &&
            sqlite3_bind_int(start, 3, difficulty) == SQLITE_OK &&
            BindText(start, 4, startPayload);
        if (!bound || sqlite3_step(start) != SQLITE_DONE) {
            Log::ErrorFmt("[HttpMockBackend] rhythm session start failed: %s", sqlite3_errmsg(db));
            sqlite3_finalize(start);
            return std::nullopt;
        }
        sqlite3_finalize(start);
        MockStoredResponse response;
        response.body = "null";
        return response;
    }

    std::optional<MockStoredResponse> HttpMockBackend::Impl::RhythmGameSetFinishLocked(std::string_view payloadJson) {
        if (!initialized && !EnsureReadyLocked()) {
            return std::nullopt;
        }
        if (!db || payloadJson.empty()) {
            return std::nullopt;
        }

        auto payload = nlohmann::json::parse(payloadJson.begin(), payloadJson.end(), nullptr, false);
        if (!payload.is_object()) {
            return std::nullopt;
        }

        const int score = payload.value("score", 0);
        if (score < 0) {
            return std::nullopt;
        }
        int musicId = 0;
        int difficulty = 0;
        std::string missionSourceId;
        std::string sessionState;
        std::string storedResult;
        {
            sqlite3_stmt* stmt = nullptr;
            constexpr const char* sql =
                "SELECT music_id, difficulty, state, result_json, session_id "
                "FROM rhythm_runtime_session WHERE singleton_id = 1;";
            if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK || !stmt) {
                Log::ErrorFmt("[HttpMockBackend] rhythm session lookup failed: %s", sqlite3_errmsg(db));
                if (stmt) sqlite3_finalize(stmt);
                return std::nullopt;
            }
            if (sqlite3_step(stmt) != SQLITE_ROW) {
                sqlite3_finalize(stmt);
                Log::Warn("[HttpMockBackend] rhythm finish has no persisted session");
                return std::nullopt;
            }
            musicId = sqlite3_column_int(stmt, 0);
            difficulty = sqlite3_column_int(stmt, 1);
            const auto* rawState = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
            const auto* rawResult = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3));
            sessionState = rawState ? rawState : "";
            storedResult = rawResult ? rawResult : "";
            const auto* rawSessionId = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 4));
            missionSourceId = rawSessionId ? rawSessionId : "";
            sqlite3_finalize(stmt);
        }
        if (sessionState == "FINISHED" && !storedResult.empty()) {
            MockStoredResponse replay;
            replay.body = std::move(storedResult);
            return replay;
        }
        if (sessionState != "PLAYING" || musicId <= 0 || difficulty <= 0) {
            return std::nullopt;
        }
        if (!ExecSql(db, "BEGIN IMMEDIATE;")) {
            return std::nullopt;
        }

        int highScoreBefore = 0;
        int hsStatusBefore = 0;
        int masteryLevelBefore = 1;
        int clearCountBefore = 0;
        nlohmann::json diffScores = nlohmann::json::array();

        {
            sqlite3_stmt* stmt = nullptr;
            constexpr const char* sql =
                "SELECT high_score, high_score_achievement_status, "
                "music_mastery_level, clear_count, difficulty_scores_json "
                "FROM rhythm_music_score WHERE music_id = ?;";
            if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) == SQLITE_OK && stmt) {
                sqlite3_bind_int(stmt, 1, musicId);
                if (sqlite3_step(stmt) == SQLITE_ROW) {
                    highScoreBefore = sqlite3_column_int(stmt, 0);
                    hsStatusBefore = sqlite3_column_int(stmt, 1);
                    masteryLevelBefore = sqlite3_column_int(stmt, 2);
                    clearCountBefore = sqlite3_column_int(stmt, 3);
                    const auto* dsJson = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 4));
                    auto parsed = nlohmann::json::parse(dsJson ? dsJson : "[]", nullptr, false);
                    if (parsed.is_array()) diffScores = std::move(parsed);
                }
                sqlite3_finalize(stmt);
            }
        }

        int maxComboBefore = 0;
        int comboStatusBefore = 0;
        nlohmann::json* currentDiffEntry = nullptr;
        for (auto& ds : diffScores) {
            if (ds.value("difficulty", 0) == difficulty) {
                currentDiffEntry = &ds;
                maxComboBefore = ds.value("best_combo", 0);
                comboStatusBefore = ds.value("combo_achievement_status", 0);
                break;
            }
        }
        if (!currentDiffEntry) {
            diffScores.push_back({{"difficulty", difficulty}, {"best_combo", 0}, {"combo_achievement_status", 0}, {"clear_lamp", 0}});
            currentDiffEntry = &diffScores.back();
        }

        int64_t totalClearBefore = 0, totalScoreAccumBefore = 0, totalHighScoreBefore = 0;
        {
            sqlite3_stmt* stmt = nullptr;
            constexpr const char* sql =
                "SELECT COALESCE(SUM(clear_count),0), COALESCE(SUM(total_score_accumulated),0), "
                "COALESCE(SUM(high_score),0) FROM rhythm_music_score;";
            if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) == SQLITE_OK && stmt) {
                if (sqlite3_step(stmt) == SQLITE_ROW) {
                    totalClearBefore = sqlite3_column_int64(stmt, 0);
                    totalScoreAccumBefore = sqlite3_column_int64(stmt, 1);
                    totalHighScoreBefore = sqlite3_column_int64(stmt, 2);
                }
                sqlite3_finalize(stmt);
            }
        }

        int maxComboThisPlay = 0;
        if (payload.contains("notes_result_list") && payload["notes_result_list"].is_array()) {
            int currentCombo = 0;
            for (const auto& note : payload["notes_result_list"]) {
                const int judgement = note.value("judgement_result", 0);
                if (judgement >= 2) {
                    ++currentCombo;
                    if (currentCombo > maxComboThisPlay) maxComboThisPlay = currentCombo;
                } else {
                    currentCombo = 0;
                }
            }
        }

        const int highScoreAfter = std::max(highScoreBefore, score);
        const int maxComboAfter = std::max(maxComboBefore, maxComboThisPlay);

        int hsStatusAfter = 0;
        if (highScoreAfter >= 10000000) hsStatusAfter = 4;
        else if (highScoreAfter >= 3000000) hsStatusAfter = 3;
        else if (highScoreAfter >= 1500000) hsStatusAfter = 2;
        else if (highScoreAfter >= 500000) hsStatusAfter = 1;

        int comboStatusAfter = 0;
        if (maxComboAfter >= 131) comboStatusAfter = 4;
        else if (maxComboAfter >= 78) comboStatusAfter = 3;
        else if (maxComboAfter >= 52) comboStatusAfter = 2;
        else if (maxComboAfter >= 26) comboStatusAfter = 1;

        (*currentDiffEntry)["best_combo"] = maxComboAfter;
        (*currentDiffEntry)["combo_achievement_status"] = comboStatusAfter;
        (*currentDiffEntry)["clear_lamp"] = 1;

        const int masteryLevelAfter = std::min(masteryLevelBefore + 1, 50);

        const std::string diffScoresStr = diffScores.dump();
        {
            constexpr const char* upsertSql =
                "INSERT OR REPLACE INTO rhythm_music_score "
                "(music_id, high_score, high_score_achievement_status, "
                "clear_count, total_score_accumulated, music_mastery_level, difficulty_scores_json) "
                "VALUES (?, ?, ?, "
                "COALESCE((SELECT clear_count FROM rhythm_music_score WHERE music_id = ?), 0) + 1, "
                "COALESCE((SELECT total_score_accumulated FROM rhythm_music_score WHERE music_id = ?), 0) + ?, ?, ?);";
            sqlite3_stmt* stmt = nullptr;
            if (sqlite3_prepare_v2(db, upsertSql, -1, &stmt, nullptr) == SQLITE_OK && stmt) {
                sqlite3_bind_int(stmt, 1, musicId);
                sqlite3_bind_int(stmt, 2, highScoreAfter);
                sqlite3_bind_int(stmt, 3, hsStatusAfter);
                sqlite3_bind_int(stmt, 4, musicId);
                sqlite3_bind_int(stmt, 5, musicId);
                sqlite3_bind_int(stmt, 6, score);
                sqlite3_bind_int(stmt, 7, masteryLevelAfter);
                BindText(stmt, 8, diffScoresStr);
                if (sqlite3_step(stmt) != SQLITE_DONE) {
                    Log::ErrorFmt("[HttpMockBackend] rhythm_music_score upsert failed: %s", sqlite3_errmsg(db));
                    sqlite3_finalize(stmt);
                    ExecSql(db, "ROLLBACK;");
                    return std::nullopt;
                }
                sqlite3_finalize(stmt);
            } else {
                Log::ErrorFmt("[HttpMockBackend] rhythm_music_score prepare failed: %s", sqlite3_errmsg(db));
                if (stmt) sqlite3_finalize(stmt);
                ExecSql(db, "ROLLBACK;");
                return std::nullopt;
            }
        }

        // Collection history is player state and must be committed with the
        // score/session, not left at the built-in JSON's never-played defaults.
        sqlite3_stmt* collectionUpdate = nullptr;
        if (sqlite3_prepare_v2(db, HttpMockBackendBuiltInSql::QueriesRhythmCollectionFinishSqlView.data(),
                              -1, &collectionUpdate, nullptr) != SQLITE_OK || !collectionUpdate) {
            if (collectionUpdate) sqlite3_finalize(collectionUpdate);
            ExecSql(db, "ROLLBACK;");
            return std::nullopt;
        }
        const bool collectionUpdated = sqlite3_bind_int(collectionUpdate, 1, musicId) == SQLITE_OK &&
                                       sqlite3_step(collectionUpdate) == SQLITE_DONE;
        sqlite3_finalize(collectionUpdate);
        if (!collectionUpdated) {
            ExecSql(db, "ROLLBACK;");
            return std::nullopt;
        }

        const int64_t totalClearAfter = totalClearBefore + 1;
        const int64_t totalScoreAccumAfter = totalScoreAccumBefore + score;
        const int64_t totalHighScoreAfter = totalHighScoreBefore - highScoreBefore + highScoreAfter;

        int starTotalAfter = 0;
        {
            sqlite3_stmt* stmt = nullptr;
            constexpr const char* sql =
                "SELECT high_score_achievement_status, difficulty_scores_json "
                "FROM rhythm_music_score;";
            if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) == SQLITE_OK && stmt) {
                while (sqlite3_step(stmt) == SQLITE_ROW) {
                    starTotalAfter += sqlite3_column_int(stmt, 0);
                    const auto* dsJson = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
                    auto ds = nlohmann::json::parse(dsJson ? dsJson : "[]", nullptr, false);
                    if (ds.is_array()) {
                        for (const auto& d : ds) {
                            starTotalAfter += d.value("combo_achievement_status", 0);
                            starTotalAfter += d.value("clear_lamp", 0);
                        }
                    }
                }
                sqlite3_finalize(stmt);
            }
        }

        nlohmann::json result;
        result["music_mastery_level_result"] = {
            {"music_id", musicId},
            {"music_mastery_exp_before", 0},
            {"music_mastery_exp_after", 0},
            {"own_status", 0},
        };
        result["drop_reward_list"] = nlohmann::json::array();
        result["music_score_mission_result"] = {
            {"high_score_before", highScoreBefore},
            {"high_score_after", highScoreAfter},
            {"high_score_achievement_status_before", hsStatusBefore},
            {"high_score_achievement_status_after", hsStatusAfter},
            {"max_combo_before", maxComboBefore},
            {"max_combo_after", maxComboAfter},
            {"combo_achievement_status_before", comboStatusBefore},
            {"combo_achievement_status_after", comboStatusAfter},
            {"reward_list", nlohmann::json::array()},
        };
        result["clear_status"] = clearCountBefore == 0 ? 1 : 0;
        result["class_mission_progress_list"] = nlohmann::json::array({
            {{"condition_type", 1}, {"progress_before", totalClearBefore}, {"progress_after", totalClearAfter}},
            {{"condition_type", 2}, {"progress_before", totalScoreAccumBefore}, {"progress_after", totalScoreAccumAfter}},
            {{"condition_type", 3}, {"progress_before", totalHighScoreBefore}, {"progress_after", totalHighScoreAfter}},
        });
        result["auto_play_ticket_info"] = {
            {"num", 10}, {"max", 10}, {"next_reset_time", "2023-04-15T00:00:00Z"}
        };
        result["rhythm_game_star_total_count"] = starTotalAfter;
        result["applied_campaign_types"] = nlohmann::json::array();

        const std::string resultJson = result.dump();
        const std::string finishPayload = payload.dump();
        sqlite3_stmt* finish = nullptr;
        constexpr const char* finishSql =
            "UPDATE rhythm_runtime_session SET state = 'FINISHED', "
            "finish_payload_json = ?, result_json = ?, updated_at = CURRENT_TIMESTAMP "
            "WHERE singleton_id = 1 AND state = 'PLAYING';";
        if (sqlite3_prepare_v2(db, finishSql, -1, &finish, nullptr) != SQLITE_OK || !finish) {
            Log::ErrorFmt("[HttpMockBackend] rhythm session finish prepare failed: %s", sqlite3_errmsg(db));
            if (finish) sqlite3_finalize(finish);
            ExecSql(db, "ROLLBACK;");
            return std::nullopt;
        }
        const bool finishBound =
            BindText(finish, 1, finishPayload) && BindText(finish, 2, resultJson);
        if (!finishBound || sqlite3_step(finish) != SQLITE_DONE || sqlite3_changes(db) != 1) {
            Log::ErrorFmt("[HttpMockBackend] rhythm session finish failed: %s", sqlite3_errmsg(db));
            sqlite3_finalize(finish);
            ExecSql(db, "ROLLBACK;");
            return std::nullopt;
        }
        sqlite3_finalize(finish);
        // A live that runs out of mental is a failure, not a clear.
        const bool showCleared = payload.value("remain_mental", 1) > 0;
        if (showCleared && !RecordMissionEventLocked("rhythm:" + missionSourceId, "SHOW_CLEAR")) {
            ExecSql(db, "ROLLBACK;");
            return std::nullopt;
        }
        if (!ExecSql(db, "COMMIT;")) {
            ExecSql(db, "ROLLBACK;");
            return std::nullopt;
        }

        MockStoredResponse response;
        response.body = resultJson;
        return response;
    }

    std::optional<MockStoredResponse> HttpMockBackend::Impl::ModifyRhythmGameDeckListLocked(std::string_view payloadJson) {
        if (!initialized && !EnsureReadyLocked()) {
            return std::nullopt;
        }
        if (!db || payloadJson.empty()) {
            return std::nullopt;
        }

        auto payload = nlohmann::json::parse(payloadJson.begin(), payloadJson.end(), nullptr, false);
        if (!payload.is_object() || !payload.contains("modify_deck_list") || !payload["modify_deck_list"].is_array()) {
            return std::nullopt;
        }

        if (!ExecSql(db, "BEGIN IMMEDIATE;")) {
            return std::nullopt;
        }

        constexpr const char* upsertSql =
            "INSERT OR REPLACE INTO rhythm_game_deck "
            "(rhythm_game_deck_id, name, deck_no, deck_card_list_json) "
            "VALUES (?, ?, ?, ?);";

        for (auto& deckEntry : payload["modify_deck_list"]) {
            const int deckNo = deckEntry.value("deck_no", 0);

            std::string deckId;
            {
                sqlite3_stmt* stmt = nullptr;
                constexpr const char* lookupSql =
                    "SELECT rhythm_game_deck_id FROM rhythm_game_deck WHERE deck_no = ?;";
                if (sqlite3_prepare_v2(db, lookupSql, -1, &stmt, nullptr) == SQLITE_OK && stmt) {
                    sqlite3_bind_int(stmt, 1, deckNo);
                    if (sqlite3_step(stmt) == SQLITE_ROW) {
                        const auto* existingId = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
                        if (existingId) deckId = existingId;
                    }
                    sqlite3_finalize(stmt);
                }
            }
            if (deckId.empty()) {
                deckId = GenerateUuid4();
            }

            nlohmann::json cardList = nlohmann::json::array();
            if (deckEntry.contains("deck_card_list") && deckEntry["deck_card_list"].is_array()) {
                cardList = deckEntry["deck_card_list"];
                for (auto& card : cardList) {
                    std::string cardId = card.value("rhythm_game_deck_cards_id", std::string{});
                    if (cardId.empty()) {
                        card["rhythm_game_deck_cards_id"] = GenerateUuid4();
                    }
                }
            }

            const std::string deckName = deckEntry.value("name", std::string{});
            const std::string cardsJsonStr = cardList.dump();

            sqlite3_stmt* stmt = nullptr;
            if (sqlite3_prepare_v2(db, upsertSql, -1, &stmt, nullptr) != SQLITE_OK || !stmt) {
                Log::ErrorFmt("[HttpMockBackend] rhythm_game_deck prepare failed: %s", sqlite3_errmsg(db));
                if (stmt) sqlite3_finalize(stmt);
                ExecSql(db, "ROLLBACK;");
                return std::nullopt;
            }
            const bool bound =
                BindText(stmt, 1, deckId) &&
                BindText(stmt, 2, deckName) &&
                sqlite3_bind_int(stmt, 3, deckNo) == SQLITE_OK &&
                BindText(stmt, 4, cardsJsonStr);
            if (!bound || sqlite3_step(stmt) != SQLITE_DONE) {
                Log::ErrorFmt("[HttpMockBackend] rhythm_game_deck upsert failed: %s", sqlite3_errmsg(db));
                sqlite3_finalize(stmt);
                ExecSql(db, "ROLLBACK;");
                return std::nullopt;
            }
            sqlite3_finalize(stmt);
        }

        if (!ExecSql(db, "COMMIT;")) {
            ExecSql(db, "ROLLBACK;");
            return std::nullopt;
        }

        MockStoredResponse response;
        response.body = "null";
        return response;
    }

    std::optional<MockStoredResponse> HttpMockBackend::Impl::QuestStageSelectLocked(std::string_view payloadJson) {
        if (!initialized && !EnsureReadyLocked()) {
            return std::nullopt;
        }
        if (!db) {
            return std::nullopt;
        }

        auto payload = nlohmann::json::parse(payloadJson.begin(), payloadJson.end(), nullptr, false);
        const int areaId = payload.is_object() ? payload.value("area_id", 0) : 0;
        if (areaId == 0) {
            return std::nullopt;
        }

        nlohmann::json stageList = nlohmann::json::array();
        sqlite3_stmt* stmt = nullptr;
        constexpr const char* sql =
            "SELECT stage_id FROM quest_stage WHERE area_id = ? ORDER BY stage_id;";
        if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) == SQLITE_OK && stmt) {
            sqlite3_bind_int(stmt, 1, areaId);
            while (sqlite3_step(stmt) == SQLITE_ROW) {
                nlohmann::json stage;
                stage["stage_id"] = sqlite3_column_int(stmt, 0);
                stage["clear_status"] = 3;
                stage["is_lock"] = false;
                stageList.push_back(std::move(stage));
            }
            sqlite3_finalize(stmt);
        }

        nlohmann::json result;
        result["stage_list"] = std::move(stageList);
        result["user_stamina"] = StaminaJsonLocked();
        result["is_update_grade_live"] = false;

        MockStoredResponse response;
        response.body = result.dump();

        return response;
    }

    std::optional<MockStoredResponse> HttpMockBackend::Impl::QuestStageDataLocked(std::string_view payloadJson) {
        if (!initialized && !EnsureReadyLocked()) {
            return std::nullopt;
        }
        if (!db) {
            return std::nullopt;
        }

        auto payload = nlohmann::json::parse(payloadJson.begin(), payloadJson.end(), nullptr, false);
        const int areaId = payload.is_object() ? payload.value("area_id", 0) : 0;
        const int viewStageId = payload.is_object() ? payload.value("stage_id", 0) : 0;
        if (areaId == 0) {
            return std::nullopt;
        }

        nlohmann::json stageDetailList = nlohmann::json::array();
        sqlite3_stmt* stmt = nullptr;
        constexpr const char* sql =
            // QuestMusicsType 0 ("any song") stages store the generation in
            // music_id (e.g. 105), not a song. best_love_music_id must be a real
            // song: the Quest skip popup looks it up and silently fails
            // otherwise, leaving the stage UI half-locked.
            "SELECT stage_id, "
            "COALESCE((SELECT m.music_id FROM music m WHERE m.music_id = quest_stage.music_id AND m.has_score = 1), "
            "(SELECT MIN(m.music_id) FROM music m WHERE m.has_score = 1 AND m.generations_id = quest_stage.music_id), "
            "(SELECT MIN(m.music_id) FROM music m WHERE m.has_score = 1)), "
            "quest_musics_type, score1, score2, score3, gain_style_point, use_num "
            "FROM quest_stage WHERE area_id = ? ORDER BY stage_id;";
        if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) == SQLITE_OK && stmt) {
            sqlite3_bind_int(stmt, 1, areaId);
            while (sqlite3_step(stmt) == SQLITE_ROW) {
                const int stageId = sqlite3_column_int(stmt, 0);
                const int musicId = sqlite3_column_int(stmt, 1);
                const int score3Val = sqlite3_column_int(stmt, 5);

                nlohmann::json detail;
                detail["stage_id"] = stageId;
                detail["is_challenge"] = false;
                detail["is_skip"] = true;
                detail["clear_status"] = 3;
                detail["best_love_music_id"] = musicId;
                detail["stage_reward_list"] = StageRewardPreviewLocked(stageId);
                // Present in the 5.1 StageDetail DTO; a missing list arrives as
                // null and the skip dialog never opened (no request sent).
                detail["stage_challenge_reward_list"] = nlohmann::json::array();
                detail["best_love"] = score3Val;

                stageDetailList.push_back(std::move(detail));
            }
            sqlite3_finalize(stmt);
        }

        // Build music_list: for type=0 stages in this area, expand GenerationsId via music table
        nlohmann::json musicList = nlohmann::json::array();
        {
            sqlite3_stmt* genStmt = nullptr;
            constexpr const char* genSql =
                "SELECT music_id FROM quest_stage WHERE area_id = ? AND quest_musics_type = 0 AND music_id > 0 LIMIT 1;";
            int generationsId = 0;
            if (sqlite3_prepare_v2(db, genSql, -1, &genStmt, nullptr) == SQLITE_OK && genStmt) {
                sqlite3_bind_int(genStmt, 1, areaId);
                if (sqlite3_step(genStmt) == SQLITE_ROW) {
                    generationsId = sqlite3_column_int(genStmt, 0);
                }
                sqlite3_finalize(genStmt);
            }
            if (generationsId > 0) {
                sqlite3_stmt* mStmt = nullptr;
                constexpr const char* mSql =
                    "SELECT music_id FROM music WHERE generations_id = ? ORDER BY music_id;";
                if (sqlite3_prepare_v2(db, mSql, -1, &mStmt, nullptr) == SQLITE_OK && mStmt) {
                    sqlite3_bind_int(mStmt, 1, generationsId);
                    while (sqlite3_step(mStmt) == SQLITE_ROW) {
                        musicList.push_back({
                            {"m_musics_id", sqlite3_column_int(mStmt, 0)},
                            {"is_enable", true},
                        });
                    }
                    sqlite3_finalize(mStmt);
                }
            }
        }

        nlohmann::json result;
        result["view_stage_id"] = viewStageId;
        result["stage_detail_list"] = std::move(stageDetailList);
        result["user_stamina"] = StaminaJsonLocked();
        result["music_list"] = std::move(musicList);

        MockStoredResponse response;
        response.body = result.dump();

        return response;
    }

    std::optional<MockStoredResponse> HttpMockBackend::Impl::QuestGetLiveSettingLocked(std::string_view payloadJson) {
        if (!initialized && !EnsureReadyLocked()) {
            return std::nullopt;
        }
        if (!db) {
            return std::nullopt;
        }

        auto payload = nlohmann::json::parse(payloadJson.begin(), payloadJson.end(), nullptr, false);
        const int stageId = payload.is_object() ? payload.value("stage_id", 0) : 0;
        const int questLiveType = payload.is_object() ? payload.value("quest_live_type", 1) : 1;

        int musicId = 0;
        int questMusicsType = 0;
        bool foundStage = false;
        {
            sqlite3_stmt* stmt = nullptr;
            constexpr const char* sql = "SELECT music_id, quest_musics_type FROM quest_stage WHERE stage_id = ?;";
            if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) == SQLITE_OK && stmt) {
                sqlite3_bind_int(stmt, 1, stageId);
                if (sqlite3_step(stmt) == SQLITE_ROW) {
                    musicId = sqlite3_column_int(stmt, 0);
                    questMusicsType = sqlite3_column_int(stmt, 1);
                    foundStage = true;
                }
                sqlite3_finalize(stmt);
            }
        }
        if (!foundStage) {
            sqlite3_stmt* stmt = nullptr;
            constexpr const char* sql = "SELECT quest_musics_detail, quest_musics_type FROM daily_quest_stage WHERE stage_id = ?;";
            if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) == SQLITE_OK && stmt) {
                sqlite3_bind_int(stmt, 1, stageId);
                if (sqlite3_step(stmt) == SQLITE_ROW) {
                    musicId = sqlite3_column_int(stmt, 0);
                    questMusicsType = sqlite3_column_int(stmt, 1);
                    foundStage = true;
                }
                sqlite3_finalize(stmt);
            }
        }
        if (!foundStage) {
            sqlite3_stmt* stmt = nullptr;
            constexpr const char* sql = "SELECT quest_musics_detail, quest_musics_type FROM dream_quest_stage WHERE stage_id = ?;";
            if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) == SQLITE_OK && stmt) {
                sqlite3_bind_int(stmt, 1, stageId);
                if (sqlite3_step(stmt) == SQLITE_ROW) {
                    musicId = sqlite3_column_int(stmt, 0);
                    questMusicsType = sqlite3_column_int(stmt, 1);
                    foundStage = true;
                }
                sqlite3_finalize(stmt);
            }
        }
        if (!foundStage) {
            sqlite3_stmt* stmt = nullptr;
            constexpr const char* sql = "SELECT quest_musics_detail, quest_musics_type FROM grade_quest_stage WHERE stage_id = ?;";
            if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) == SQLITE_OK && stmt) {
                sqlite3_bind_int(stmt, 1, stageId);
                if (sqlite3_step(stmt) == SQLITE_ROW) {
                    musicId = sqlite3_column_int(stmt, 0);
                    questMusicsType = sqlite3_column_int(stmt, 1);
                    foundStage = true;
                }
                sqlite3_finalize(stmt);
            }
        }
        if (!foundStage) {
            sqlite3_stmt* stmt = nullptr;
            constexpr const char* sql = "SELECT music_id FROM learning_stage WHERE stage_id = ?;";
            if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) == SQLITE_OK && stmt) {
                sqlite3_bind_int(stmt, 1, stageId);
                if (sqlite3_step(stmt) == SQLITE_ROW) {
                    musicId = sqlite3_column_int(stmt, 0);
                    questMusicsType = 2;
                    foundStage = true;
                }
                sqlite3_finalize(stmt);
            }
        }

        nlohmann::json deckList = nlohmann::json::array();
        {
            sqlite3_stmt* stmt = nullptr;
            constexpr const char* sql =
                "SELECT d_deck_datas_id, deck_name, deck_no, generations_id, ace_card, deck_cards_json "
                "FROM deck ORDER BY deck_no;";
            if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) == SQLITE_OK && stmt) {
                while (sqlite3_step(stmt) == SQLITE_ROW) {
                    const auto* id = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
                    const auto* name = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
                    const int deckNo = sqlite3_column_int(stmt, 2);
                    const int genId = sqlite3_column_int(stmt, 3);
                    const auto* aceCard = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 4));
                    const auto* cardsJson = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 5));

                    nlohmann::json deck;
                    deck["d_deck_datas_id"] = id ? id : "";
                    deck["deck_name"] = name ? name : "";
                    deck["deck_no"] = deckNo;
                    deck["generations_id"] = genId;
                    const std::string aceStr = aceCard ? aceCard : "";
                    if (!aceStr.empty()) {
                        deck["ace_card"] = aceStr;
                    }
                    auto cardsParsed = nlohmann::json::parse(cardsJson ? cardsJson : "[]", nullptr, false);
                    deck["deck_cards_list"] = cardsParsed.is_array() ? cardsParsed : nlohmann::json::array();
                    deckList.push_back(std::move(deck));
                }
                sqlite3_finalize(stmt);
            }
        }

        const auto& cardListObj = OfflineApiMockBuiltIn::UserCardGetListJsonObj();
        nlohmann::json userCardDataList = nlohmann::json::array();
        if (cardListObj.contains("user_card_data_list")) {
            userCardDataList = cardListObj["user_card_data_list"];
        }

        nlohmann::json musicList = nlohmann::json::array();
        if (questMusicsType == 2 && musicId > 0) {
            musicList.push_back({{"m_musics_id", musicId}, {"is_enable", true}});
        } else if (questMusicsType == 3 && musicId > 0) {
            sqlite3_stmt* mStmt = nullptr;
            constexpr const char* mSql =
                "SELECT music_id FROM music WHERE center_character_id = ? ORDER BY music_id;";
            if (sqlite3_prepare_v2(db, mSql, -1, &mStmt, nullptr) == SQLITE_OK && mStmt) {
                sqlite3_bind_int(mStmt, 1, musicId);
                while (sqlite3_step(mStmt) == SQLITE_ROW) {
                    musicList.push_back({
                        {"m_musics_id", sqlite3_column_int(mStmt, 0)},
                        {"is_enable", true},
                    });
                }
                sqlite3_finalize(mStmt);
            }
        } else if (questMusicsType == 0 && musicId > 0) {
            sqlite3_stmt* mStmt = nullptr;
            constexpr const char* mSql =
                "SELECT music_id FROM music WHERE generations_id = ? ORDER BY music_id;";
            if (sqlite3_prepare_v2(db, mSql, -1, &mStmt, nullptr) == SQLITE_OK && mStmt) {
                sqlite3_bind_int(mStmt, 1, musicId);
                while (sqlite3_step(mStmt) == SQLITE_ROW) {
                    musicList.push_back({
                        {"m_musics_id", sqlite3_column_int(mStmt, 0)},
                        {"is_enable", true},
                    });
                }
                sqlite3_finalize(mStmt);
            }
        } else if (questMusicsType == 0 && musicId == 0) {
            sqlite3_stmt* mStmt = nullptr;
            constexpr const char* mSql = "SELECT music_id FROM music ORDER BY music_id;";
            if (sqlite3_prepare_v2(db, mSql, -1, &mStmt, nullptr) == SQLITE_OK && mStmt) {
                while (sqlite3_step(mStmt) == SQLITE_ROW) {
                    musicList.push_back({
                        {"m_musics_id", sqlite3_column_int(mStmt, 0)},
                        {"is_enable", true},
                    });
                }
                sqlite3_finalize(mStmt);
            }
        }

        nlohmann::json result;
        result["quest_live_type"] = questLiveType;
        result["stage_id"] = stageId;
        result["deck_data"] = std::move(deckList);
        result["best_love_deck"] = nlohmann::json::object();
        result["best_love_musics_id"] = musicId;
        result["user_stamina"] = StaminaJsonLocked();
        result["music_list"] = std::move(musicList);
        result["user_card_data_list"] = std::move(userCardDataList);
        result["rental_deck_data"] = nlohmann::json::array();
        result["rental_card_data_list"] = nlohmann::json::array();
        result["friend_card_list"] = nlohmann::json::array();

        MockStoredResponse response;
        response.body = result.dump();

        return response;
    }

    std::optional<MockStoredResponse> HttpMockBackend::Impl::QuestSetLiveSettingLocked(std::string_view payloadJson) {
        if (!initialized && !EnsureReadyLocked()) {
            return std::nullopt;
        }
        if (!db) {
            return std::nullopt;
        }

        auto payload = nlohmann::json::parse(payloadJson.begin(), payloadJson.end(), nullptr, false);
        if (!payload.is_object()) {
            return std::nullopt;
        }

        const int stageId = payload.value("stage_id", 0);
        const std::string deckId = payload.value("deck_id", std::string{});
        const int musicId = payload.value("music_id", 0);
        const bool isChallengeMode = payload.value("is_challenge_mode", false);
        const int questLiveType = payload.value("quest_live_type", 1);
        const int resourceValue = payload.value("resource_value", 0);

        if (questLiveType == 4 && gradeQuest.active) {
            gradeQuest.resourceValue = resourceValue;
        }

        const std::string questLiveId = std::to_string(questLiveType) + "_" + std::to_string(stageId);

        nlohmann::json deckData = nlohmann::json::object();
        if (!deckId.empty()) {
            sqlite3_stmt* stmt = nullptr;
            constexpr const char* sql =
                "SELECT d_deck_datas_id, deck_name, deck_no, generations_id, deck_cards_json "
                "FROM deck WHERE d_deck_datas_id = ?;";
            if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) == SQLITE_OK && stmt) {
                BindText(stmt, 1, deckId);
                if (sqlite3_step(stmt) == SQLITE_ROW) {
                    const auto* id = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
                    const auto* name = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
                    const int deckNo = sqlite3_column_int(stmt, 2);
                    const int genId = sqlite3_column_int(stmt, 3);
                    const auto* cardsJson = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 4));

                    deckData["d_deck_datas_id"] = id ? id : "";
                    deckData["deck_name"] = name ? name : "";
                    deckData["deck_no"] = deckNo;
                    deckData["generations_id"] = genId;
                    auto cardsParsed = nlohmann::json::parse(cardsJson ? cardsJson : "[]", nullptr, false);
                    deckData["deck_cards_list"] = cardsParsed.is_array() ? cardsParsed : nlohmann::json::array();
                }
                sqlite3_finalize(stmt);
            }
        }

        // Look up character_bonus from the first card in deck
        nlohmann::json charBonus = kDefaultCharacterBonus;
        if (deckData.contains("deck_cards_list") && deckData["deck_cards_list"].is_array()
            && !deckData["deck_cards_list"].empty()) {
            const auto& firstCard = deckData["deck_cards_list"][0];
            const std::string cardId = firstCard.value("d_card_datas_id", std::string{});
            if (!cardId.empty()) {
                sqlite3_stmt* cbStmt = nullptr;
                constexpr const char* cbSql = "SELECT character_bonus FROM card_detail WHERE d_card_datas_id = ?;";
                if (sqlite3_prepare_v2(db, cbSql, -1, &cbStmt, nullptr) == SQLITE_OK && cbStmt) {
                    BindText(cbStmt, 1, cardId);
                    if (sqlite3_step(cbStmt) == SQLITE_ROW) {
                        const auto* cbJson = reinterpret_cast<const char*>(sqlite3_column_text(cbStmt, 0));
                        auto parsed = nlohmann::json::parse(cbJson ? cbJson : "{}", nullptr, false);
                        if (parsed.is_object() && !parsed.empty()) {
                            charBonus = std::move(parsed);
                        }
                    }
                    sqlite3_finalize(cbStmt);
                }
            }
        }

        currentQuestLive = {};
        currentQuestLive.questLiveId = questLiveId;
        currentQuestLive.questLiveType = questLiveType;
        currentQuestLive.stageId = stageId;
        currentQuestLive.musicId = musicId;
        currentQuestLive.isChallengeMode = isChallengeMode;
        currentQuestLive.deckData = deckData;
        currentQuestLive.characterBonus = charBonus;

        const auto sectionSkillList = LookupSectionSkills(db, stageId);

        nlohmann::json fanLevelInfoList = nlohmann::json::array();
        {
            const auto& profileObj = OfflineApiMockBuiltIn::HomeGetHomeJsonObj();
            if (profileObj.contains("profile_info") && profileObj["profile_info"].contains("fan_level_list")) {
                for (const auto& fl : profileObj["profile_info"]["fan_level_list"]) {
                    fanLevelInfoList.push_back({
                        {"character_id", fl.value("character_id", 0)},
                        {"member_fan_level", fl.value("member_fan_level", 0)},
                    });
                }
            }
        }

        nlohmann::json result;
        result["result"] = true;
        result["quest_live_id"] = questLiveId;
        result["quest_live_type"] = questLiveType;
        result["quest_id"] = stageId;
        result["is_challenge_mode"] = isChallengeMode;
        result["music_id"] = musicId;
        result["deck_data"] = deckData;
        result["rental_deck_data"] = kDefaultRentalDeckData;
        result["character_bonus"] = charBonus;
        result["section_skill_list"] = std::move(sectionSkillList);
        result["init_hand_data"] = "";
        result["grand_prix_retry_count"] = 0;
        result["grand_prix_is_rehearsal"] = false;
        result["grand_prix_id"] = (questLiveType == 6) ? 806101 : 0;
        result["grade_retry_count"] = (questLiveType == 4) ? 1 : 0;
        {
            nlohmann::json gradeSkills = nlohmann::json::array();
            if (questLiveType == 4 && gradeQuest.active) {
                for (int id : gradeQuest.activeAddSkillIds) {
                    gradeSkills.push_back(id);
                }
            }
            result["grade_add_skill_list"] = std::move(gradeSkills);
        }
        int playCount = 0;
        int playableCount = 0;
        if (questLiveType == 6 && stageId > 0) {
            sqlite3_stmt* pcStmt = nullptr;
            constexpr const char* pcSql = "SELECT play_count FROM grand_prix_progress WHERE stage_id = ?;";
            if (sqlite3_prepare_v2(db, pcSql, -1, &pcStmt, nullptr) == SQLITE_OK && pcStmt) {
                sqlite3_bind_int(pcStmt, 1, stageId);
                if (sqlite3_step(pcStmt) == SQLITE_ROW) {
                    playCount = sqlite3_column_int(pcStmt, 0);
                }
                sqlite3_finalize(pcStmt);
            }
            playableCount = 1;
        }
        result["playable_count"] = playableCount;
        result["play_count"] = playCount;
        result["fan_level_info_list"] = std::move(fanLevelInfoList);

        MockStoredResponse response;
        response.body = result.dump();

        return response;
    }

    bool HttpMockBackend::Impl::LoadQuestSessionLocked() {
        if (!db) {
            return false;
        }

        sqlite3_stmt* stmt = nullptr;
        constexpr const char* sql =
            "SELECT session_id, quest_live_type, stage_id, music_id, is_challenge_mode, "
            "state, start_payload_json, finish_payload_json, start_time "
            "FROM quest_runtime_session WHERE singleton_id = 1;";
        if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK || !stmt) {
            Log::ErrorFmt("[HttpMockBackend] quest session lookup failed: %s", sqlite3_errmsg(db));
            if (stmt) sqlite3_finalize(stmt);
            return false;
        }

        const int rc = sqlite3_step(stmt);
        if (rc == SQLITE_DONE) {
            sqlite3_finalize(stmt);
            return true;
        }
        if (rc != SQLITE_ROW) {
            Log::ErrorFmt("[HttpMockBackend] quest session read failed: %s", sqlite3_errmsg(db));
            sqlite3_finalize(stmt);
            return false;
        }

        const auto* sessionId = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        const auto* state = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 5));
        const auto* startPayload = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 6));
        const auto* finishPayload = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 7));
        const auto* startTime = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 8));
        const bool recovered = state && std::string_view(state) == "PLAYING";
        const auto start = nlohmann::json::parse(startPayload ? startPayload : "{}", nullptr, false);
        if (!start.is_object()) {
            Log::Error("[HttpMockBackend] quest session start payload is invalid");
            sqlite3_finalize(stmt);
            return false;
        }

        currentQuestLive = {};
        currentQuestLive.questLiveId = sessionId ? sessionId : "";
        currentQuestLive.questLiveType = sqlite3_column_int(stmt, 1);
        currentQuestLive.stageId = sqlite3_column_int(stmt, 2);
        currentQuestLive.musicId = sqlite3_column_int(stmt, 3);
        currentQuestLive.isChallengeMode = sqlite3_column_int(stmt, 4) != 0;
        currentQuestLive.deckData = start.value("deck_data", nlohmann::json::object());
        currentQuestLive.characterBonus = start.value("character_bonus", nlohmann::json::object());
        currentQuestLive.startTime = startTime ? startTime : "";
        currentQuestLive.finished = state && std::string_view(state) == "FINISHED";
        if (currentQuestLive.finished && finishPayload) {
            const auto finish = nlohmann::json::parse(finishPayload, nullptr, false);
            if (finish.is_object()) {
                currentQuestLive.score = finish.value("score", int64_t(0));
                currentQuestLive.playReport = finish.value("play_report", std::string{});
            }
        }
        sqlite3_finalize(stmt);
        if (recovered && !ExecSql(db,
                                  "UPDATE quest_runtime_session SET state = 'RECOVERED', "
                                  "updated_at = CURRENT_TIMESTAMP WHERE singleton_id = 1;")) {
            return false;
        }
        return true;
    }

    bool HttpMockBackend::Impl::PersistQuestSessionLocked(std::string_view state) {
        if (!db || currentQuestLive.questLiveId.empty() || currentQuestLive.startTime.empty()) {
            return false;
        }
        if (state != "PLAYING" && state != "FINISHED") {
            return false;
        }

        const nlohmann::json startPayload = {
            {"quest_live_id", currentQuestLive.questLiveId},
            {"quest_live_type", currentQuestLive.questLiveType},
            {"stage_id", currentQuestLive.stageId},
            {"music_id", currentQuestLive.musicId},
            {"is_challenge_mode", currentQuestLive.isChallengeMode},
            {"deck_data", currentQuestLive.deckData},
            {"character_bonus", currentQuestLive.characterBonus},
        };
        const nlohmann::json finishPayload = {
            {"score", currentQuestLive.score},
            {"play_report", currentQuestLive.playReport},
        };

        const bool ownsTransaction = sqlite3_get_autocommit(db) != 0;
        if (ownsTransaction && !ExecSql(db, "BEGIN IMMEDIATE;")) {
            return false;
        }
        sqlite3_stmt* stmt = nullptr;
        constexpr const char* sql =
            "INSERT OR REPLACE INTO quest_runtime_session "
            "(singleton_id, session_id, quest_live_type, stage_id, music_id, is_challenge_mode, "
            "state, start_payload_json, finish_payload_json, start_time, updated_at) "
            "VALUES (1, ?, ?, ?, ?, ?, ?, ?, ?, ?, CURRENT_TIMESTAMP);";
        if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK || !stmt) {
            Log::ErrorFmt("[HttpMockBackend] quest session persist prepare failed: %s", sqlite3_errmsg(db));
            if (stmt) sqlite3_finalize(stmt);
            if (ownsTransaction) ExecSql(db, "ROLLBACK;");
            return false;
        }

        const bool bound =
            BindText(stmt, 1, currentQuestLive.questLiveId) &&
            sqlite3_bind_int(stmt, 2, currentQuestLive.questLiveType) == SQLITE_OK &&
            sqlite3_bind_int(stmt, 3, currentQuestLive.stageId) == SQLITE_OK &&
            sqlite3_bind_int(stmt, 4, currentQuestLive.musicId) == SQLITE_OK &&
            sqlite3_bind_int(stmt, 5, currentQuestLive.isChallengeMode ? 1 : 0) == SQLITE_OK &&
            BindText(stmt, 6, state) &&
            BindText(stmt, 7, startPayload.dump()) &&
            (state == "FINISHED"
                 ? BindText(stmt, 8, finishPayload.dump())
                 : sqlite3_bind_null(stmt, 8) == SQLITE_OK) &&
            BindText(stmt, 9, currentQuestLive.startTime);
        if (!bound || sqlite3_step(stmt) != SQLITE_DONE) {
            Log::ErrorFmt("[HttpMockBackend] quest session persist failed: %s", sqlite3_errmsg(db));
            sqlite3_finalize(stmt);
            if (ownsTransaction) ExecSql(db, "ROLLBACK;");
            return false;
        }
        sqlite3_finalize(stmt);
        if (ownsTransaction && !ExecSql(db, "COMMIT;")) {
            ExecSql(db, "ROLLBACK;");
            return false;
        }
        return true;
    }

    std::optional<MockStoredResponse> HttpMockBackend::Impl::QuestSetStartLocked(std::string_view payloadJson) {
        if (!initialized && !EnsureReadyLocked()) {
            return std::nullopt;
        }
        if (!db || payloadJson.empty()) {
            return std::nullopt;
        }
        if (currentQuestLive.questLiveId.empty() && !LoadQuestSessionLocked()) {
            return std::nullopt;
        }

        auto payload = nlohmann::json::parse(payloadJson.begin(), payloadJson.end(), nullptr, false);
        if (payload.is_object()) {
            const std::string liveId = payload.value("quest_live_id", std::string{});
            if (!liveId.empty()) {
                currentQuestLive.questLiveId = liveId;
            }
        }
        if (!currentQuestLive.startTime.empty() && !currentQuestLive.finished) {
            nlohmann::json replay;
            replay["quest_start_time"] = currentQuestLive.startTime;
            MockStoredResponse response;
            response.body = replay.dump();
            return response;
        }

        std::time_t now = std::time(nullptr);
        std::tm tmLocal{};
#if defined(_WIN32)
        localtime_s(&tmLocal, &now);
#else
        localtime_r(&now, &tmLocal);
#endif
        char buf[64]{};
        std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S+09:00", &tmLocal);
        currentQuestLive.startTime = buf;
        ConsumeStaminaLocked(StageStaminaCostLocked(currentQuestLive.questLiveType, currentQuestLive.stageId));
        currentQuestLive.finished = false;
        currentQuestLive.score = 0;
        currentQuestLive.playReport.clear();
        if (!PersistQuestSessionLocked("PLAYING")) {
            return std::nullopt;
        }

        nlohmann::json result;
        result["quest_start_time"] = currentQuestLive.startTime;

        MockStoredResponse response;
        response.body = result.dump();

        return response;
    }

    std::optional<MockStoredResponse> HttpMockBackend::Impl::QuestGetLiveInfoLocked(std::string_view payloadJson) {
        if (!initialized && !EnsureReadyLocked()) {
            return std::nullopt;
        }
        if (currentQuestLive.questLiveId.empty() && !LoadQuestSessionLocked()) {
            return std::nullopt;
        }

        const auto sectionSkillList = LookupSectionSkills(db, currentQuestLive.stageId);

        nlohmann::json fanLevelInfoList = nlohmann::json::array();
        {
            const auto& profileObj = OfflineApiMockBuiltIn::HomeGetHomeJsonObj();
            if (profileObj.contains("profile_info") && profileObj["profile_info"].contains("fan_level_list")) {
                for (const auto& fl : profileObj["profile_info"]["fan_level_list"]) {
                    fanLevelInfoList.push_back({
                        {"character_id", fl.value("character_id", 0)},
                        {"member_fan_level", fl.value("member_fan_level", 0)},
                    });
                }
            }
        }

        nlohmann::json result;
        result["result"] = true;
        result["quest_live_id"] = currentQuestLive.questLiveId;
        result["quest_live_type"] = currentQuestLive.questLiveType;
        result["quest_id"] = currentQuestLive.stageId;
        result["is_challenge_mode"] = currentQuestLive.isChallengeMode;
        result["music_id"] = currentQuestLive.musicId;
        result["deck_data"] = currentQuestLive.deckData;
        result["rental_deck_data"] = kDefaultRentalDeckData;
        result["character_bonus"] = currentQuestLive.characterBonus;
        result["section_skill_list"] = std::move(sectionSkillList);
        result["init_hand_data"] = "";
        result["grand_prix_retry_count"] = 0;
        result["grand_prix_is_rehearsal"] = false;
        result["grand_prix_id"] = (currentQuestLive.questLiveType == 6) ? 806101 : 0;
        result["grade_retry_count"] = (currentQuestLive.questLiveType == 4) ? 1 : 0;
        result["grade_add_skill_list"] = nlohmann::json::array();
        {
            int playCount = 0;
            int playableCount = 0;
            if (currentQuestLive.questLiveType == 6 && currentQuestLive.stageId > 0 && db) {
                sqlite3_stmt* pcStmt = nullptr;
                constexpr const char* pcSql = "SELECT play_count FROM grand_prix_progress WHERE stage_id = ?;";
                if (sqlite3_prepare_v2(db, pcSql, -1, &pcStmt, nullptr) == SQLITE_OK && pcStmt) {
                    sqlite3_bind_int(pcStmt, 1, currentQuestLive.stageId);
                    if (sqlite3_step(pcStmt) == SQLITE_ROW) {
                        playCount = sqlite3_column_int(pcStmt, 0);
                    }
                    sqlite3_finalize(pcStmt);
                }
                playableCount = 1;
            }
            result["playable_count"] = playableCount;
            result["play_count"] = playCount;
        }
        result["fan_level_info_list"] = std::move(fanLevelInfoList);

        MockStoredResponse response;
        response.body = result.dump();
        return response;
    }

    std::optional<MockStoredResponse> HttpMockBackend::Impl::QuestSetFinishLocked(std::string_view payloadJson) {
        if (!initialized && !EnsureReadyLocked()) {
            return std::nullopt;
        }
        if (!db || payloadJson.empty()) {
            return std::nullopt;
        }
        if (currentQuestLive.questLiveId.empty() && !LoadQuestSessionLocked()) {
            return std::nullopt;
        }
        if (currentQuestLive.finished) {
            nlohmann::json replay;
            replay["quest_live_type"] = currentQuestLive.questLiveType;
            replay["quest_result"] = true;
            replay["return_resource"] = 0;
            replay["applied_campaign_types"] = nlohmann::json::array();
            MockStoredResponse response;
            response.body = replay.dump();
            return response;
        }
        auto payload = nlohmann::json::parse(payloadJson.begin(), payloadJson.end(), nullptr, false);
        if (!payload.is_object()) {
            Log::Warn("[HttpMockBackend] quest finish payload is not a JSON object");
            return std::nullopt;
        }
        currentQuestLive.score = payload.value("score", int64_t(0));
        currentQuestLive.playReport = payload.value("play_report", std::string{});
        currentQuestLive.finished = true;
        if (!ExecSql(db, "BEGIN IMMEDIATE;")) {
            currentQuestLive.finished = false;
            return std::nullopt;
        }
        if (!PersistQuestSessionLocked("FINISHED")) {
            ExecSql(db, "ROLLBACK;");
            currentQuestLive.finished = false;
            return std::nullopt;
        }
        bool missionRecorded = true;
        // questLiveId is "<type>_<stage>" and repeats for every replay of a
        // stage; the start time makes the key unique per play while a repeated
        // finish of the same play still maps to the same key.
        const std::string missionSource =
            "quest:" + currentQuestLive.questLiveId + "@" + currentQuestLive.startTime;
        const int liveType = currentQuestLive.questLiveType;
        // "スクールアイドルステージでライブを○回クリア" counts any stage live.
        // Dream counts only when cleared (recorded in dream/get_result); Grade
        // lives can end in LIVE FAILED, which this handler cannot see.
        if (liveType == kQuestLiveTypeStandard || liveType == kQuestLiveTypeDaily ||
            liveType == 3 || liveType == 6) {
            missionRecorded = RecordMissionEventLocked(missionSource, "QUEST_CLEAR", 1,
                                                        currentQuestLive.stageId);
        }
        if (liveType == kQuestLiveTypeStandard && !currentQuestLive.isChallengeMode) {
            sqlite3_stmt* scoreStmt = nullptr;
            int stars = 0;
            if (sqlite3_prepare_v2(db,
                    "SELECT score1, score2, score3 FROM quest_stage WHERE stage_id=?;",
                    -1, &scoreStmt, nullptr) == SQLITE_OK && scoreStmt) {
                sqlite3_bind_int(scoreStmt, 1, currentQuestLive.stageId);
                if (sqlite3_step(scoreStmt) == SQLITE_ROW) {
                    for (int i = 0; i < 3; ++i) {
                        const int64_t threshold = sqlite3_column_int64(scoreStmt, i);
                        if (threshold > 0 && currentQuestLive.score >= threshold) ++stars;
                    }
                }
            }
            if (scoreStmt) sqlite3_finalize(scoreStmt);
            // detail = stage: progress uses the best stars per stage.
            missionRecorded = missionRecorded && RecordMissionEventLocked(
                missionSource + ":stars", "QUEST_STAR", stars, currentQuestLive.stageId);
        } else if (liveType == kQuestLiveTypeDaily) {
            missionRecorded = missionRecorded && RecordMissionEventLocked(
                missionSource + ":daily", "DAILY_CLEAR", 1, currentQuestLive.stageId);
            if (currentQuestLive.stageId % 100 == 1) {
                missionRecorded = missionRecorded && RecordMissionEventLocked(
                    missionSource + ":rank-i", "DAILY_CLEAR_I", 1, currentQuestLive.stageId);
            }
        }
        if (!missionRecorded) {
            ExecSql(db, "ROLLBACK;");
            currentQuestLive.finished = false;
            return std::nullopt;
        }
        currentQuestLive.rewardList = nlohmann::json::array();
        // Settled once: a repeated finish takes the replay branch above.
        if (currentQuestLive.questLiveType == kQuestLiveTypeDaily) ConsumeDailyTicketsLocked(1);
        if (currentQuestLive.questLiveType == kQuestLiveTypeDaily ||
            currentQuestLive.questLiveType == kQuestLiveTypeStandard) {
            currentQuestLive.rewardList = GrantStageDropsLocked(currentQuestLive.stageId);
        }
        if (!ExecSql(db, "COMMIT;")) {
            ExecSql(db, "ROLLBACK;");
            currentQuestLive.finished = false;
            return std::nullopt;
        }

        nlohmann::json result;
        result["quest_live_type"] = currentQuestLive.questLiveType;
        result["quest_result"] = true;
        result["return_resource"] = 0;
        result["applied_campaign_types"] = nlohmann::json::array();

        MockStoredResponse response;
        response.body = result.dump();
        return response;
    }

    std::optional<MockStoredResponse> HttpMockBackend::Impl::QuestGetResultLocked(std::string_view payloadJson) {
        if (!initialized && !EnsureReadyLocked()) {
            return std::nullopt;
        }
        if (currentQuestLive.questLiveId.empty() && !LoadQuestSessionLocked()) {
            return std::nullopt;
        }

        int gainStylePoint = 0;
        if (db && currentQuestLive.stageId > 0) {
            sqlite3_stmt* stmt = nullptr;
            constexpr const char* sql = "SELECT gain_style_point FROM quest_stage WHERE stage_id = ?;";
            if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) == SQLITE_OK && stmt) {
                sqlite3_bind_int(stmt, 1, currentQuestLive.stageId);
                if (sqlite3_step(stmt) == SQLITE_ROW) {
                    gainStylePoint = sqlite3_column_int(stmt, 0);
                }
                sqlite3_finalize(stmt);
            }
            if (gainStylePoint == 0) {
                sqlite3_stmt* dStmt = nullptr;
                constexpr const char* dSql = "SELECT gain_style_point FROM daily_quest_stage WHERE stage_id = ?;";
                if (sqlite3_prepare_v2(db, dSql, -1, &dStmt, nullptr) == SQLITE_OK && dStmt) {
                    sqlite3_bind_int(dStmt, 1, currentQuestLive.stageId);
                    if (sqlite3_step(dStmt) == SQLITE_ROW) {
                        gainStylePoint = sqlite3_column_int(dStmt, 0);
                    }
                    sqlite3_finalize(dStmt);
                }
            }
            if (gainStylePoint == 0) {
                sqlite3_stmt* lStmt = nullptr;
                constexpr const char* lSql = "SELECT gain_style_point FROM learning_stage WHERE stage_id = ?;";
                if (sqlite3_prepare_v2(db, lSql, -1, &lStmt, nullptr) == SQLITE_OK && lStmt) {
                    sqlite3_bind_int(lStmt, 1, currentQuestLive.stageId);
                    if (sqlite3_step(lStmt) == SQLITE_ROW) {
                        gainStylePoint = sqlite3_column_int(lStmt, 0);
                    }
                    sqlite3_finalize(lStmt);
                }
            }
        }

        nlohmann::json result;
        result["quest_live_type"] = currentQuestLive.questLiveType;
        result["stage_id"] = currentQuestLive.stageId;
        result["quest_result"] = true;
        result["result_love"] = currentQuestLive.score;
        result["best_love"] = currentQuestLive.score;
        result["before_best_love"] = 0;
        result["add_style_point"] = gainStylePoint;
        result["is_challenge_mode"] = currentQuestLive.isChallengeMode;
        result["music_id"] = currentQuestLive.musicId;
        result["reward_list"] = currentQuestLive.rewardList.is_array()
                ? currentQuestLive.rewardList : nlohmann::json::array();
        result["play_report"] = currentQuestLive.playReport;
        result["mastary_level_before"] = 1;
        result["mastary_level_after"] = 1;
        result["mastary_level_experience"] = 0;
        result["mastary_level_total_experience_before"] = 0;
        result["first_clear_flag"] = false;
        result["first_complete_clear_flag"] = false;
        result["is_limit_over_style_point"] = false;

        MockStoredResponse response;
        response.body = result.dump();
        return response;
    }

    std::optional<MockStoredResponse> HttpMockBackend::Impl::QuestSkipLocked(std::string_view payloadJson) {
        auto payload = nlohmann::json::parse(payloadJson.begin(), payloadJson.end(), nullptr, false);
        const int questLiveType = payload.is_object() ? payload.value("quest_live_type", 1) : 1;
        const int stageId = payload.is_object() ? payload.value("stage_id", 0) : 0;
        nlohmann::json skipRewardList = nlohmann::json::array();
        nlohmann::json totalSkipRewardList = nlohmann::json::array();
        if (questLiveType != kQuestLiveTypeDaily) {
            if (!initialized && !EnsureReadyLocked()) return std::nullopt;
            const int skips = std::max(1, payload.is_object() ? payload.value("skip_count", 1) : 1);
            ConsumeStaminaLocked(int64_t(StageStaminaCostLocked(questLiveType, stageId)) * skips);
            if (questLiveType != kQuestLiveTypeStandard)
                RecordMissionEventLocked("skip:" + GenerateUuid4(), "QUEST_CLEAR", skips, stageId);
        }
        if (questLiveType == kQuestLiveTypeDaily || questLiveType == kQuestLiveTypeStandard) {
            if (!initialized && !EnsureReadyLocked()) return std::nullopt;
            const int skipCount = std::max(1, payload.is_object() ? payload.value("skip_count", 1) : 1);
            if (questLiveType == kQuestLiveTypeDaily) ConsumeDailyTicketsLocked(skipCount);
            // Skips count toward the clear missions ("（スキップ可）").
            const std::string skipSource = "skip:" + GenerateUuid4();
            RecordMissionEventLocked(skipSource, "QUEST_CLEAR", skipCount, stageId);
            if (questLiveType == kQuestLiveTypeDaily)
                RecordMissionEventLocked(skipSource + ":daily", "DAILY_CLEAR", skipCount, stageId);
            // One SkipRewardList entry per skipped play, plus the summed total.
            std::map<int, int64_t> totals;
            for (int index = 0; index < skipCount; ++index) {
                auto rewards = GrantStageDropsLocked(stageId);
                for (const auto& reward : rewards) {
                    totals[reward.value("item_id", 0)] += reward.value("reward_num", 0);
                }
                skipRewardList.push_back({{"reward_list", std::move(rewards)}});
            }
            for (const auto& [itemId, num] : totals) {
                totalSkipRewardList.push_back({
                    {"reward_type", 1}, {"item_type", 1}, {"item_id", itemId},
                    {"item_num", num}, {"reward_num", num}, {"is_received", true},
                });
            }
        }

        int gainStylePoint = 0;
        if (db && stageId > 0) {
            sqlite3_stmt* stmt = nullptr;
            constexpr const char* sql = "SELECT gain_style_point FROM quest_stage WHERE stage_id = ?;";
            if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) == SQLITE_OK && stmt) {
                sqlite3_bind_int(stmt, 1, stageId);
                if (sqlite3_step(stmt) == SQLITE_ROW) {
                    gainStylePoint = sqlite3_column_int(stmt, 0);
                }
                sqlite3_finalize(stmt);
            }
            if (gainStylePoint == 0) {
                sqlite3_stmt* dStmt = nullptr;
                constexpr const char* dSql = "SELECT gain_style_point FROM daily_quest_stage WHERE stage_id = ?;";
                if (sqlite3_prepare_v2(db, dSql, -1, &dStmt, nullptr) == SQLITE_OK && dStmt) {
                    sqlite3_bind_int(dStmt, 1, stageId);
                    if (sqlite3_step(dStmt) == SQLITE_ROW) {
                        gainStylePoint = sqlite3_column_int(dStmt, 0);
                    }
                    sqlite3_finalize(dStmt);
                }
            }
            if (gainStylePoint == 0) {
                sqlite3_stmt* lStmt = nullptr;
                constexpr const char* lSql = "SELECT gain_style_point FROM learning_stage WHERE stage_id = ?;";
                if (sqlite3_prepare_v2(db, lSql, -1, &lStmt, nullptr) == SQLITE_OK && lStmt) {
                    sqlite3_bind_int(lStmt, 1, stageId);
                    if (sqlite3_step(lStmt) == SQLITE_ROW) {
                        gainStylePoint = sqlite3_column_int(lStmt, 0);
                    }
                    sqlite3_finalize(lStmt);
                }
            }
        }

        nlohmann::json result;
        result["quest_live_type"] = questLiveType;
        result["quest_live_id"] = "";
        result["user_stamina"] = StaminaJsonLocked();
        result["add_style_point"] = gainStylePoint;
        result["skip_reward_list"] = std::move(skipRewardList);
        result["total_skip_reward_list"] = std::move(totalSkipRewardList);
        result["is_limit_over_style_point"] = false;
        if (questLiveType == 3) {
            int masteryLevel = 50;
            int earnedExp = 250300;
            if (db && stageId > 0) {
                sqlite3_stmt* mStmt = nullptr;
                constexpr const char* mSql =
                    "SELECT mm.music_exp_level, mm.earned_music_exp "
                    "FROM learning_stage ls JOIN music_mastery mm ON ls.music_id = mm.music_id "
                    "WHERE ls.stage_id = ?;";
                if (sqlite3_prepare_v2(db, mSql, -1, &mStmt, nullptr) == SQLITE_OK && mStmt) {
                    sqlite3_bind_int(mStmt, 1, stageId);
                    if (sqlite3_step(mStmt) == SQLITE_ROW) {
                        masteryLevel = sqlite3_column_int(mStmt, 0);
                        earnedExp = sqlite3_column_int(mStmt, 1);
                    }
                    sqlite3_finalize(mStmt);
                }
            }
            result["mastery_level_before"] = masteryLevel;
            result["mastery_level_after"] = masteryLevel;
            result["mastery_level_experience"] = 0;
            result["mastery_level_total_experience_before"] = earnedExp;
        } else {
            result["mastery_level_before"] = 1;
            result["mastery_level_after"] = 1;
            result["mastery_level_experience"] = 0;
            result["mastery_level_total_experience_before"] = 0;
        }
        result["before_earned_music_exp"] = 0;
        result["earned_music_exp"] = 0;
        result["raid_stamina"] = {
            {"stamina_now", 0},
            {"stamina_recovered_time", "0001-01-01T00:00:00Z"},
        };
        result["add_once_event_point"] = 0;
        result["event_point_reward_list"] = nlohmann::json::array();
        result["event_personal_total_point"] = 0;
        result["applied_campaign_types"] = nlohmann::json::array();

        MockStoredResponse response;
        response.body = result.dump();
        return response;
    }

    // プレイPt. (user_stamina). Spent by Quest and Learning plays per
    // the stage's Master use_num; it regenerates over time at 1 point per 6
    // minutes, the interval the 5.1 client itself assumes when it shows
    // "1回復まであと" (the rate is not in the Master snapshot).
    namespace {
        constexpr int64_t kStaminaMax = 999;
        constexpr int64_t kStaminaRecoverySeconds = 360;
    }

    int64_t HttpMockBackend::Impl::CurrentStaminaLocked(std::time_t* fullAt) {
        const std::time_t now = std::time(nullptr);
        int64_t value = kStaminaMax;
        int64_t updated = now;
        if (db && ExecSql(db, "CREATE TABLE IF NOT EXISTS offline_player_stamina ("
                              "singleton_id INTEGER PRIMARY KEY CHECK(singleton_id = 1), "
                              "stamina INTEGER NOT NULL, updated_at INTEGER NOT NULL);")) {
            sqlite3_stmt* stmt = nullptr;
            if (sqlite3_prepare_v2(db, "SELECT stamina, updated_at FROM offline_player_stamina WHERE singleton_id=1;",
                                   -1, &stmt, nullptr) == SQLITE_OK && stmt) {
                if (sqlite3_step(stmt) == SQLITE_ROW) {
                    value = sqlite3_column_int64(stmt, 0);
                    updated = sqlite3_column_int64(stmt, 1);
                }
            }
            if (stmt) sqlite3_finalize(stmt);
        }
        if (value < kStaminaMax && now > updated) {
            value = std::min(kStaminaMax, value + (now - updated) / kStaminaRecoverySeconds);
        }
        if (fullAt) {
            // Partial progress toward the next point is kept by not moving updated_at.
            const int64_t missing = std::max<int64_t>(0, kStaminaMax - value);
            const int64_t elapsed = value < kStaminaMax ? (now - updated) % kStaminaRecoverySeconds : 0;
            *fullAt = now + missing * kStaminaRecoverySeconds - elapsed;
        }
        return value;
    }

    nlohmann::json HttpMockBackend::Impl::StaminaJsonLocked() {
        std::time_t fullAt = 0;
        const int64_t value = CurrentStaminaLocked(&fullAt);
        std::tm tm{};
#if defined(_WIN32)
        gmtime_s(&tm, &fullAt);
#else
        gmtime_r(&fullAt, &tm);
#endif
        char buffer[48]{};
        std::strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%S.000000000Z", &tm);
        return {{"stamina_now", value}, {"stamina_max", kStaminaMax}, {"stamina_recovery_time", buffer}};
    }

    void HttpMockBackend::Impl::ConsumeStaminaLocked(int64_t amount) {
        if (!db || amount <= 0) return;
        const std::time_t now = std::time(nullptr);
        const int64_t current = CurrentStaminaLocked();
        // Keep the partial-minute progress so regeneration stays continuous.
        sqlite3_stmt* read = nullptr;
        int64_t updated = now;
        int64_t stored = kStaminaMax;
        if (sqlite3_prepare_v2(db, "SELECT stamina, updated_at FROM offline_player_stamina WHERE singleton_id=1;",
                               -1, &read, nullptr) == SQLITE_OK && read) {
            if (sqlite3_step(read) == SQLITE_ROW) {
                stored = sqlite3_column_int64(read, 0);
                updated = sqlite3_column_int64(read, 1);
            }
        }
        if (read) sqlite3_finalize(read);
        const int64_t base = stored >= kStaminaMax
                ? now : updated + (current - stored) * kStaminaRecoverySeconds;
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(db,
                "INSERT INTO offline_player_stamina(singleton_id, stamina, updated_at) VALUES(1, ?1, ?2) "
                "ON CONFLICT(singleton_id) DO UPDATE SET stamina=excluded.stamina, updated_at=excluded.updated_at;",
                -1, &stmt, nullptr) == SQLITE_OK && stmt) {
            sqlite3_bind_int64(stmt, 1, std::max<int64_t>(0, current - amount));
            sqlite3_bind_int64(stmt, 2, current >= kStaminaMax ? now : base);
            sqlite3_step(stmt);
        }
        if (stmt) sqlite3_finalize(stmt);
    }

    int HttpMockBackend::Impl::StageStaminaCostLocked(int questLiveType, int stageId) {
        const char* sql = nullptr;
        switch (questLiveType) {
            case 1: sql = "SELECT use_num FROM quest_stage WHERE stage_id=?;"; break;
            case 3: sql = "SELECT use_num FROM learning_stage WHERE stage_id=?;"; break;
            // Daily uses tickets, Grade action Pt.; Dream's Master UseType 2 is
            // the ユメノカケラ item (not consumed in the Museum); Grand Prix is free.
            default: return 0;
        }
        sqlite3_stmt* stmt = nullptr;
        int cost = 0;
        if (db && sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) == SQLITE_OK && stmt) {
            sqlite3_bind_int(stmt, 1, stageId);
            if (sqlite3_step(stmt) == SQLITE_ROW) cost = sqlite3_column_int(stmt, 0);
        }
        if (stmt) sqlite3_finalize(stmt);
        return cost;
    }

    // Daily Live tickets: 3 per day, reset at 04:00 JST like the other daily
    // content. Usage is player state keyed by the JST business day.
    namespace {
        constexpr int kDailyTicketMax = 3;
        int64_t DailyBusinessDay(std::time_t now) {
            return (static_cast<int64_t>(now) + 9 * 3600 - 4 * 3600) / 86400;
        }
    }

    nlohmann::json HttpMockBackend::Impl::DailyTicketInfoLocked() {
        const std::time_t now = std::time(nullptr);
        const int64_t day = DailyBusinessDay(now);
        int used = 0;
        if (db && ExecSql(db, "CREATE TABLE IF NOT EXISTS offline_daily_ticket ("
                              "business_day INTEGER PRIMARY KEY, used INTEGER NOT NULL DEFAULT 0);")) {
            sqlite3_stmt* stmt = nullptr;
            if (sqlite3_prepare_v2(db, "SELECT used FROM offline_daily_ticket WHERE business_day=?;",
                                   -1, &stmt, nullptr) == SQLITE_OK && stmt) {
                sqlite3_bind_int64(stmt, 1, day);
                if (sqlite3_step(stmt) == SQLITE_ROW) used = sqlite3_column_int(stmt, 0);
            }
            if (stmt) sqlite3_finalize(stmt);
        }
        // Next 04:00 JST expressed in JST.
        const std::time_t nextResetJst = static_cast<std::time_t>((day + 1) * 86400 + 4 * 3600);
        std::tm tm{};
#if defined(_WIN32)
        gmtime_s(&tm, &nextResetJst);
#else
        gmtime_r(&nextResetJst, &tm);
#endif
        char buffer[40]{};
        std::strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%S+09:00", &tm);
        return {
            {"num", std::max(0, kDailyTicketMax - used)},
            {"max", kDailyTicketMax},
            {"next_reset_time", buffer},
        };
    }

    bool HttpMockBackend::Impl::ConsumeDailyTicketsLocked(int count) {
        if (!db || count <= 0) return false;
        if (!ExecSql(db, "CREATE TABLE IF NOT EXISTS offline_daily_ticket ("
                         "business_day INTEGER PRIMARY KEY, used INTEGER NOT NULL DEFAULT 0);"))
            return false;
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(db,
                "INSERT INTO offline_daily_ticket(business_day, used) VALUES(?1, MIN(?2, ?3)) "
                "ON CONFLICT(business_day) DO UPDATE SET used=MIN(used + ?2, ?3);",
                -1, &stmt, nullptr) != SQLITE_OK || !stmt) {
            if (stmt) sqlite3_finalize(stmt);
            return false;
        }
        sqlite3_bind_int64(stmt, 1, DailyBusinessDay(std::time(nullptr)));
        sqlite3_bind_int(stmt, 2, count);
        sqlite3_bind_int(stmt, 3, kDailyTicketMax);
        const bool ok = sqlite3_step(stmt) == SQLITE_DONE;
        sqlite3_finalize(stmt);
        return ok;
    }

    // Ticket recovery ("+" on the Daily screens). DTOs per 5.1 metadata:
    // DailyGetRecoveryChallengeCountResponse{challenge_count, recovery_count,
    // max_recovery_count, price}; DailyRecoveryChallengeCountRequest
    // {recovery_count} -> Response{challenge_count}. The Museum makes recovery
    // free (price 0) with a generous daily cap so the 3-ticket limit never
    // locks the offline player out of Daily Live.
    namespace {
        constexpr int kDailyRecoveryMax = 99;
    }

    std::optional<MockStoredResponse> HttpMockBackend::Impl::DailyGetRecoveryChallengeCountLocked() {
        if (!initialized && !EnsureReadyLocked()) return std::nullopt;
        if (!db) return std::nullopt;
        if (!ExecSql(db, "CREATE TABLE IF NOT EXISTS offline_daily_ticket_recovery ("
                         "business_day INTEGER PRIMARY KEY, recovered INTEGER NOT NULL DEFAULT 0);"))
            return std::nullopt;
        int recovered = 0;
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(db, "SELECT recovered FROM offline_daily_ticket_recovery WHERE business_day=?;",
                               -1, &stmt, nullptr) == SQLITE_OK && stmt) {
            sqlite3_bind_int64(stmt, 1, DailyBusinessDay(std::time(nullptr)));
            if (sqlite3_step(stmt) == SQLITE_ROW) recovered = sqlite3_column_int(stmt, 0);
        }
        if (stmt) sqlite3_finalize(stmt);
        const auto tickets = DailyTicketInfoLocked();
        nlohmann::json result = {
            {"challenge_count", tickets.value("num", 0)},
            {"recovery_count", recovered},
            {"max_recovery_count", kDailyRecoveryMax},
            {"price", 0},
        };
        MockStoredResponse response;
        response.body = result.dump();
        return response;
    }

    std::optional<MockStoredResponse> HttpMockBackend::Impl::DailyRecoveryChallengeCountLocked(
            std::string_view payloadJson) {
        if (!initialized && !EnsureReadyLocked()) return std::nullopt;
        if (!db) return std::nullopt;
        const auto payload = nlohmann::json::parse(payloadJson.begin(), payloadJson.end(), nullptr, false);
        const int count = payload.is_object() ? payload.value("recovery_count", 1) : 1;
        if (count < 1 || count > kDailyRecoveryMax) {
            return MockStoredResponse{R"({"error":"invalid recovery count"})", {}, 400, "Invalid recovery count"};
        }
        const int64_t day = DailyBusinessDay(std::time(nullptr));
        if (!ExecSql(db, "CREATE TABLE IF NOT EXISTS offline_daily_ticket ("
                         "business_day INTEGER PRIMARY KEY, used INTEGER NOT NULL DEFAULT 0);") ||
            !ExecSql(db, "CREATE TABLE IF NOT EXISTS offline_daily_ticket_recovery ("
                         "business_day INTEGER PRIMARY KEY, recovered INTEGER NOT NULL DEFAULT 0);") ||
            !ExecSql(db, "BEGIN IMMEDIATE;"))
            return std::nullopt;
        const auto run = [this, day, count](const char* sql) {
            sqlite3_stmt* stmt = nullptr;
            if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK || !stmt) {
                if (stmt) sqlite3_finalize(stmt);
                return false;
            }
            sqlite3_bind_int64(stmt, 1, day);
            sqlite3_bind_int(stmt, 2, count);
            const bool ok = sqlite3_step(stmt) == SQLITE_DONE;
            sqlite3_finalize(stmt);
            return ok;
        };
        // A recovered ticket offsets one used ticket for the business day.
        if (!run("INSERT INTO offline_daily_ticket(business_day, used) VALUES(?1, -?2) "
                 "ON CONFLICT(business_day) DO UPDATE SET used=used-?2;") ||
            !run("INSERT INTO offline_daily_ticket_recovery(business_day, recovered) VALUES(?1, ?2) "
                 "ON CONFLICT(business_day) DO UPDATE SET recovered=recovered+?2;") ||
            !ExecSql(db, "COMMIT;")) {
            ExecSql(db, "ROLLBACK;");
            return std::nullopt;
        }
        nlohmann::json result = {{"challenge_count", DailyTicketInfoLocked().value("num", 0)}};
        MockStoredResponse response;
        response.body = result.dump();
        return response;
    }

    // Drops for a cleared Daily stage. Which materials a stage drops comes
    // from the preserved Master (ItemSources.yaml via
    // tools/generate_daily_quest_drop_catalog.py); the server-side drop tables
    // were not preserved, so the quantity is a local Museum approximation
    // (QuestRank of the stage) and not the retired server's rate.
    nlohmann::json HttpMockBackend::Impl::GrantStageDropsLocked(int stageId) {
        nlohmann::json rewards = nlohmann::json::array();
        if (!db) return rewards;
        const auto catalog = OfflineApiMockBuiltIn::QuestDropCatalogJsonObj();
        if (!catalog.is_array()) return rewards;
        const nlohmann::json* entry = nullptr;
        for (const auto& row : catalog) {
            if (row.value("stage_id", 0) == stageId) { entry = &row; break; }
        }
        if (!entry || !entry->contains("item_ids") || !(*entry)["item_ids"].is_array()) return rewards;
        const int quantity = std::max(1, entry->value("quantity", 1));
        sqlite3_stmt* grant = nullptr;
        if (sqlite3_prepare_v2(db,
                "INSERT INTO museum_item_inventory(d_item_datas_id, item_num) "
                "SELECT d_item_datas_id, ?1 FROM item WHERE item_id=?2 LIMIT 1 "
                "ON CONFLICT(d_item_datas_id) DO UPDATE SET item_num=item_num+excluded.item_num;",
                -1, &grant, nullptr) != SQLITE_OK || !grant) {
            if (grant) sqlite3_finalize(grant);
            return rewards;
        }
        for (const auto& itemIdJson : (*entry)["item_ids"]) {
            if (!itemIdJson.is_number_integer()) continue;
            const int itemId = itemIdJson.get<int>();
            sqlite3_reset(grant);
            sqlite3_clear_bindings(grant);
            sqlite3_bind_int(grant, 1, quantity);
            sqlite3_bind_int(grant, 2, itemId);
            if (sqlite3_step(grant) != SQLITE_DONE || sqlite3_changes(db) != 1) continue;
            // Quest/skip results use the reward model with item_num (5.1
            // metadata); reward_num/item_type are kept for list consumers that
            // use the mission reward shape.
            rewards.push_back({
                {"reward_type", 1}, {"item_type", 1}, {"item_id", itemId},
                {"item_num", quantity}, {"reward_num", quantity}, {"is_received", true},
            });
        }
        sqlite3_finalize(grant);
        return rewards;
    }

    // "獲得可能アイテム": the materials a Daily/Quest stage can drop (see
    // GrantStageDropsLocked for the quantity approximation).
    nlohmann::json HttpMockBackend::Impl::StageRewardPreviewLocked(int stageId) {
        nlohmann::json stageRewards = nlohmann::json::array();
        static const auto dropCatalog = OfflineApiMockBuiltIn::QuestDropCatalogJsonObj();
        if (!dropCatalog.is_array()) return stageRewards;
        for (const auto& row : dropCatalog) {
            if (row.value("stage_id", 0) != stageId || !row.contains("item_ids")) continue;
            const int quantity = std::max(1, row.value("quantity", 1));
            for (const auto& itemId : row["item_ids"]) {
                if (!itemId.is_number_integer()) continue;
                stageRewards.push_back({
                    {"item_id", itemId.get<int>()}, {"is_received", false},
                    {"reward_type", 1}, {"item_num", quantity},
                });
            }
        }
        return stageRewards;
    }

    std::optional<MockStoredResponse> HttpMockBackend::Impl::DailyQuestStageSelectLocked() {
        if (!initialized && !EnsureReadyLocked()) return std::nullopt;
        if (!db) return std::nullopt;
        nlohmann::json result = OfflineApiMockBuiltIn::OutQuestLiveDailyGetStageSelectJsonObj();
        if (!result.is_object()) result = nlohmann::json::object();
        result["user_daily_ticket_info"] = DailyTicketInfoLocked();
        if (!result.contains("temporary_release_daily_quest_series_id_list"))
            result["temporary_release_daily_quest_series_id_list"] = nlohmann::json::array();
        MockStoredResponse response;
        response.body = result.dump();
        return response;
    }

    std::optional<MockStoredResponse> HttpMockBackend::Impl::DailyQuestStageListLocked(std::string_view payloadJson) {
        if (!initialized && !EnsureReadyLocked()) {
            return std::nullopt;
        }
        if (!db) {
            return std::nullopt;
        }

        auto payload = nlohmann::json::parse(payloadJson.begin(), payloadJson.end(), nullptr, false);
        const int questId = payload.is_object() ? payload.value("quest_id", 0) : 0;
        if (questId == 0) {
            return std::nullopt;
        }

        nlohmann::json stageList = nlohmann::json::array();
        sqlite3_stmt* stmt = nullptr;
        constexpr const char* sql =
            "SELECT stage_id FROM daily_quest_stage WHERE series_id = ? ORDER BY stage_id;";
        if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) == SQLITE_OK && stmt) {
            sqlite3_bind_int(stmt, 1, questId);
            while (sqlite3_step(stmt) == SQLITE_ROW) {
                nlohmann::json stage;
                stage["stage_id"] = sqlite3_column_int(stmt, 0);
                stage["clear_status"] = 3;
                stage["is_lock"] = false;
                stageList.push_back(std::move(stage));
            }
            sqlite3_finalize(stmt);
        }

        nlohmann::json result;
        result["user_daily_ticket_info"] = DailyTicketInfoLocked();
        result["stage_list"] = std::move(stageList);

        MockStoredResponse response;
        response.body = result.dump();
        return response;
    }

    std::optional<MockStoredResponse> HttpMockBackend::Impl::DailyQuestStageDataLocked(std::string_view payloadJson) {
        if (!initialized && !EnsureReadyLocked()) {
            return std::nullopt;
        }
        if (!db) {
            return std::nullopt;
        }

        auto payload = nlohmann::json::parse(payloadJson.begin(), payloadJson.end(), nullptr, false);
        const int stageId = payload.is_object() ? payload.value("stage_id", 0) : 0;
        if (stageId == 0) {
            return std::nullopt;
        }

        int score3Val = 0;
        {
            sqlite3_stmt* stmt = nullptr;
            constexpr const char* sql = "SELECT score3 FROM daily_quest_stage WHERE stage_id = ?;";
            if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) == SQLITE_OK && stmt) {
                sqlite3_bind_int(stmt, 1, stageId);
                if (sqlite3_step(stmt) == SQLITE_ROW) {
                    score3Val = sqlite3_column_int(stmt, 0);
                }
                sqlite3_finalize(stmt);
            }
        }

        nlohmann::json musicList = nlohmann::json::array();
        {
            sqlite3_stmt* stmt = nullptr;
            constexpr const char* sql = "SELECT music_id FROM music WHERE has_score = 1 ORDER BY music_id;";
            if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) == SQLITE_OK && stmt) {
                while (sqlite3_step(stmt) == SQLITE_ROW) {
                    musicList.push_back({
                        {"m_musics_id", sqlite3_column_int(stmt, 0)},
                        {"is_enable", true},
                    });
                }
                sqlite3_finalize(stmt);
            }
        }

        int bestLoveMusicId = 0;
        if (!musicList.empty()) {
            bestLoveMusicId = musicList[0].value("m_musics_id", 0);
        }

        nlohmann::json result;
        result["stage_id"] = stageId;
        result["clear_status"] = 3;
        result["best_love_music_id"] = bestLoveMusicId;
        result["user_daily_ticket_info"] = DailyTicketInfoLocked();
        result["stage_reward_list"] = StageRewardPreviewLocked(stageId);
        result["music_list"] = std::move(musicList);

        MockStoredResponse response;
        response.body = result.dump();
        return response;
    }

    std::optional<MockStoredResponse> HttpMockBackend::Impl::MusicLearningGetMusicSelectLocked(std::string_view payloadJson) {
        if (!initialized && !EnsureReadyLocked()) {
            return std::nullopt;
        }
        if (!db) {
            return std::nullopt;
        }

        nlohmann::json musicList = nlohmann::json::array();
        {
            sqlite3_stmt* seriesStmt = nullptr;
            constexpr const char* seriesSql =
                "SELECT ls.series_id, ls.music_id, "
                "COALESCE(mm.music_exp_level, 50), COALESCE(mm.earned_music_exp, 250300), COALESCE(mm.is_mastery, 1) "
                "FROM learning_stage ls "
                "LEFT JOIN music_mastery mm ON ls.music_id = mm.music_id "
                "GROUP BY ls.series_id ORDER BY ls.series_id;";
            if (sqlite3_prepare_v2(db, seriesSql, -1, &seriesStmt, nullptr) == SQLITE_OK && seriesStmt) {
                while (sqlite3_step(seriesStmt) == SQLITE_ROW) {
                    const int seriesId = sqlite3_column_int(seriesStmt, 0);
                    const int musicId = sqlite3_column_int(seriesStmt, 1);
                    const int musicExpLevel = sqlite3_column_int(seriesStmt, 2);
                    const int earnedMusicExp = sqlite3_column_int(seriesStmt, 3);
                    const bool isMastery = sqlite3_column_int(seriesStmt, 4) != 0;

                    nlohmann::json stageList = nlohmann::json::array();
                    sqlite3_stmt* stStmt = nullptr;
                    constexpr const char* stSql =
                        "SELECT stage_id, quest_rank, quest_level, gain_music_exp FROM learning_stage "
                        "WHERE series_id = ? ORDER BY quest_rank;";
                    if (sqlite3_prepare_v2(db, stSql, -1, &stStmt, nullptr) == SQLITE_OK && stStmt) {
                        sqlite3_bind_int(stStmt, 1, seriesId);
                        while (sqlite3_step(stStmt) == SQLITE_ROW) {
                            const int stageId = sqlite3_column_int(stStmt, 0);
                            const int questRank = sqlite3_column_int(stStmt, 1);
                            const int questLevel = sqlite3_column_int(stStmt, 2);
                            const int gainMusicExp = sqlite3_column_int(stStmt, 3);
                            stageList.push_back({
                                {"learning_live_stages_id", stageId},
                                {"quest_rank", questRank},
                                {"quest_level", questLevel},
                                {"is_lock", false},
                                {"is_clear", true},
                                {"gain_music_exp", gainMusicExp},
                                {"page", questRank <= 4 ? 1 : 2},
                            });
                        }
                        sqlite3_finalize(stStmt);
                    }

                    musicList.push_back({
                        {"learning_live_series_id", seriesId},
                        {"music_id", musicId},
                        {"earned_music_exp", earnedMusicExp},
                        {"music_exp_level", musicExpLevel},
                        {"is_mastery", isMastery},
                        {"stage_list", std::move(stageList)},
                    });
                }
                sqlite3_finalize(seriesStmt);
            }
        }

        nlohmann::json characterBonusList = nlohmann::json::array();
        {
            constexpr int characterIds[] = {1011, 1020, 1021, 1022, 1023, 1030, 1031, 1032, 1033, 1041, 1042, 1043, 1044, 1051, 1052};
            for (int charId : characterIds) {
                characterBonusList.push_back({
                    {"character_id", charId},
                    {"music_mastery_bonus", 100},
                    {"love_correction_value", 100},
                    {"music_mastery_bonus_list", nlohmann::json::array()},
                    {"season_fan_level", 0},
                });
            }
        }

        const int musicCount = static_cast<int>(musicList.size());
        nlohmann::json result;
        result["music_list"] = std::move(musicList);
        result["music_point"] = musicCount;
        result["latest_music_id"] = 0;
        result["character_bonus_list"] = std::move(characterBonusList);

        MockStoredResponse response;
        response.body = result.dump();
        return response;
    }

    std::optional<MockStoredResponse> HttpMockBackend::Impl::MusicLearningGetResultLocked(std::string_view payloadJson) {
        int masteryLevel = 50;
        int earnedMusicExp = 250300;
        bool isMastery = true;
        if (db && currentQuestLive.musicId > 0) {
            sqlite3_stmt* stmt = nullptr;
            constexpr const char* sql =
                "SELECT music_exp_level, earned_music_exp, is_mastery FROM music_mastery WHERE music_id = ?;";
            if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) == SQLITE_OK && stmt) {
                sqlite3_bind_int(stmt, 1, currentQuestLive.musicId);
                if (sqlite3_step(stmt) == SQLITE_ROW) {
                    masteryLevel = sqlite3_column_int(stmt, 0);
                    earnedMusicExp = sqlite3_column_int(stmt, 1);
                    isMastery = sqlite3_column_int(stmt, 2) != 0;
                }
                sqlite3_finalize(stmt);
            }
        }

        nlohmann::json result;
        result["stage_id"] = currentQuestLive.stageId;
        result["quest_live_type"] = 3;
        result["is_clear"] = true;
        result["is_mastery"] = isMastery;
        result["result_love"] = currentQuestLive.score;
        result["best_love"] = currentQuestLive.score;
        result["before_best_love"] = 0;
        result["music_id"] = currentQuestLive.musicId;
        result["play_report"] = currentQuestLive.playReport;
        result["mastary_level_before"] = masteryLevel;
        result["mastary_level_after"] = masteryLevel;
        result["mastary_level_experience"] = 0;
        result["mastary_level_total_experience_before"] = earnedMusicExp;
        result["style_point"] = 200;
        result["before_earned_music_exp"] = 0;
        result["earned_music_exp"] = 0;
        result["first_clear_flag"] = false;
        result["is_limit_over_style_point"] = false;
        result["is_ex_stage_open"] = true;

        MockStoredResponse response;
        response.body = result.dump();
        return response;
    }

    std::optional<MockStoredResponse> HttpMockBackend::Impl::DreamNotifyMemberReleaseConfirmLocked(std::string_view payloadJson) {
        MockStoredResponse response;
        response.body = "null";
        return response;
    }

    std::optional<MockStoredResponse> HttpMockBackend::Impl::DreamGetResultLocked(std::string_view payloadJson) {
        nlohmann::json result;
        result["stage_id"] = currentQuestLive.stageId;
        result["quest_live_type"] = 5;
        // Clear when the LOVE reaches the stage requirement (Master Score1).
        int64_t requiredLove = 0;
        bool clearedBefore = false;
        if ((initialized || EnsureReadyLocked()) && db && currentQuestLive.stageId > 0) {
            sqlite3_stmt* stmt = nullptr;
            if (sqlite3_prepare_v2(db, "SELECT score1 FROM dream_quest_stage WHERE stage_id=?;",
                                   -1, &stmt, nullptr) == SQLITE_OK && stmt) {
                sqlite3_bind_int(stmt, 1, currentQuestLive.stageId);
                if (sqlite3_step(stmt) == SQLITE_ROW) requiredLove = sqlite3_column_int64(stmt, 0);
            }
            if (stmt) sqlite3_finalize(stmt);
            ExecSql(db, "CREATE TABLE IF NOT EXISTS offline_dream_clear (stage_id INTEGER PRIMARY KEY);");
            stmt = nullptr;
            if (sqlite3_prepare_v2(db, "SELECT 1 FROM offline_dream_clear WHERE stage_id=?;",
                                   -1, &stmt, nullptr) == SQLITE_OK && stmt) {
                sqlite3_bind_int(stmt, 1, currentQuestLive.stageId);
                clearedBefore = sqlite3_step(stmt) == SQLITE_ROW;
            }
            if (stmt) sqlite3_finalize(stmt);
        }
        const bool isClear = currentQuestLive.score >= requiredLove;
        if (isClear && db && !currentQuestLive.questLiveId.empty()) {
            const std::string dreamSource =
                "dream:" + currentQuestLive.questLiveId + "@" + currentQuestLive.startTime;
            RecordMissionEventLocked(dreamSource, "DREAM_CLEAR", 1, currentQuestLive.stageId);
            RecordMissionEventLocked(dreamSource + ":live", "QUEST_CLEAR", 1, currentQuestLive.stageId);
        }
        if (isClear && !clearedBefore && db) {
            sqlite3_stmt* stmt = nullptr;
            if (sqlite3_prepare_v2(db, "INSERT OR IGNORE INTO offline_dream_clear(stage_id) VALUES(?);",
                                   -1, &stmt, nullptr) == SQLITE_OK && stmt) {
                sqlite3_bind_int(stmt, 1, currentQuestLive.stageId);
                sqlite3_step(stmt);
            }
            if (stmt) sqlite3_finalize(stmt);
        }
        result["is_clear"] = isClear;
        result["result_love"] = currentQuestLive.score;
        result["get_card_id"] = 0;
        result["music_id"] = currentQuestLive.musicId;
        result["play_report"] = currentQuestLive.playReport;
        result["first_clear_flag"] = isClear && !clearedBefore;

        MockStoredResponse response;
        response.body = result.dump();
        return response;
    }

    std::optional<MockStoredResponse> HttpMockBackend::Impl::GradeGetQuestListLocked(std::string_view payloadJson) {
        if (!initialized && !EnsureReadyLocked()) {
            return std::nullopt;
        }
        if (!db) {
            return std::nullopt;
        }

        std::unordered_map<int, int> progressMap;
        {
            sqlite3_stmt* stmt = nullptr;
            constexpr const char* sql = "SELECT series_id, clear_status FROM grade_quest_progress;";
            if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) == SQLITE_OK && stmt) {
                while (sqlite3_step(stmt) == SQLITE_ROW) {
                    progressMap[sqlite3_column_int(stmt, 0)] = sqlite3_column_int(stmt, 1);
                }
                sqlite3_finalize(stmt);
            }
        }

        nlohmann::json questSeasonList = nlohmann::json::array();
        {
            sqlite3_stmt* stmt = nullptr;
            constexpr const char* sql = "SELECT season_id, generation, season, order_id FROM grade_quest_season ORDER BY order_id;";
            if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) == SQLITE_OK && stmt) {
                while (sqlite3_step(stmt) == SQLITE_ROW) {
                    const int seasonId = sqlite3_column_int(stmt, 0);

                    nlohmann::json seriesList = nlohmann::json::array();
                    int clearNum = 0;
                    bool prevCleared = false;
                    bool isFirstSeries = true;
                    sqlite3_stmt* sStmt = nullptr;
                    constexpr const char* sSql =
                        "SELECT series_id, order_id, rewards_json FROM grade_quest_series "
                        "WHERE season_id = ? ORDER BY order_id;";
                    if (sqlite3_prepare_v2(db, sSql, -1, &sStmt, nullptr) == SQLITE_OK && sStmt) {
                        sqlite3_bind_int(sStmt, 1, seasonId);
                        while (sqlite3_step(sStmt) == SQLITE_ROW) {
                            const int seriesId = sqlite3_column_int(sStmt, 0);
                            const int orderId = sqlite3_column_int(sStmt, 1);
                            const auto* rwJson = reinterpret_cast<const char*>(sqlite3_column_text(sStmt, 2));

                            auto pit = progressMap.find(seriesId);
                            const int clearStatus = (pit != progressMap.end()) ? pit->second : 0;
                            const bool cleared = (clearStatus > 0);

                            if (isFirstSeries) {
                                prevCleared = (orderId == 1);
                                isFirstSeries = false;
                            }

                            const bool isLock = !prevCleared;

                            if (cleared) clearNum++;
                            prevCleared = cleared;

                            nlohmann::json rewardList = nlohmann::json::array();
                            auto rp = nlohmann::json::parse(rwJson ? rwJson : "[]", nullptr, false);
                            if (rp.is_array()) {
                                for (const auto& r : rp) {
                                    rewardList.push_back({
                                        {"grade_quest_rewards_id", r.value("grade_quest_rewards_id", 0)},
                                        {"is_received", cleared},
                                    });
                                }
                            }

                            seriesList.push_back({
                                {"grade_quest_series_id", seriesId},
                                {"play_status", 0},
                                {"is_lock", isLock},
                                {"clear_status", clearStatus},
                                {"reward_list", std::move(rewardList)},
                            });
                        }
                        sqlite3_finalize(sStmt);
                    }

                    const bool seasonLock = (clearNum == 0 && !seriesList.empty() &&
                                             seriesList[0].value("is_lock", false));
                    questSeasonList.push_back({
                        {"grade_quest_season_id", seasonId},
                        {"play_status", 0},
                        {"is_lock", seasonLock},
                        {"clear_num", clearNum},
                        {"quest_series_list", std::move(seriesList)},
                    });
                }
                sqlite3_finalize(stmt);
            }
        }

        nlohmann::json result;
        result["quest_season_list"] = std::move(questSeasonList);
        result["point_bonus_list"] = nlohmann::json::array();
        result["is_update_grade_live"] = false;

        MockStoredResponse response;
        response.body = result.dump();
        return response;
    }

    std::optional<MockStoredResponse> HttpMockBackend::Impl::GradeSetQuestStartLocked(std::string_view payloadJson) {
        if (!initialized && !EnsureReadyLocked()) {
            return std::nullopt;
        }
        if (!db) {
            return std::nullopt;
        }

        auto payload = nlohmann::json::parse(payloadJson.begin(), payloadJson.end(), nullptr, false);
        const int seriesId = payload.is_object() ? payload.value("grade_quest_series_id", 0) : 0;

        if (!gradeQuest.active || gradeQuest.seriesId != seriesId) {
            gradeQuest = {};
            gradeQuest.active = true;
            gradeQuest.seriesId = seriesId;

            nlohmann::json squaresParsed = nlohmann::json::array();
            {
                sqlite3_stmt* stmt = nullptr;
                constexpr const char* sql =
                    "SELECT default_action_point, squares_json, rewards_json FROM grade_quest_series WHERE series_id = ?;";
                if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) == SQLITE_OK && stmt) {
                    sqlite3_bind_int(stmt, 1, seriesId);
                    if (sqlite3_step(stmt) == SQLITE_ROW) {
                        gradeQuest.actionPoint = sqlite3_column_int(stmt, 0);
                        const auto* sqJson = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
                        auto sp = nlohmann::json::parse(sqJson ? sqJson : "[]", nullptr, false);
                        if (sp.is_array()) squaresParsed = std::move(sp);
                        const auto* rwJson = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
                        auto rp = nlohmann::json::parse(rwJson ? rwJson : "[]", nullptr, false);
                        if (rp.is_array()) gradeQuest.rewardsJson = std::move(rp);
                    }
                    sqlite3_finalize(stmt);
                }
            }
            {
                const int seasonId = seriesId / 10;
                sqlite3_stmt* stmt = nullptr;
                constexpr const char* sql = "SELECT generation, season FROM grade_quest_season WHERE season_id = ?;";
                if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) == SQLITE_OK && stmt) {
                    sqlite3_bind_int(stmt, 1, seasonId);
                    if (sqlite3_step(stmt) == SQLITE_ROW) {
                        gradeQuest.generation = sqlite3_column_int(stmt, 0);
                        gradeQuest.season = sqlite3_column_int(stmt, 1);
                        gradeQuest.characterId = gradeQuest.generation * 10 + 1;
                    }
                    sqlite3_finalize(stmt);
                }
            }

            for (const auto& sq : squaresParsed) {
                GradeSquareState ss;
                ss.squareId = sq.value("grade_quest_square_id", 0);
                ss.squareType = sq.value("square_type", 0);
                ss.targetId = sq.value("target_id", 0);
                ss.actionPointCost = sq.value("min_action_point", 0);
                if (sq.contains("open_square_ids") && sq["open_square_ids"].is_array()) {
                    for (const auto& dep : sq["open_square_ids"]) {
                        if (dep.is_number_integer()) {
                            ss.openSquareIds.push_back(dep.get<int>());
                        }
                    }
                }
                if (ss.squareType == 1) {
                    ss.status = 2;
                    gradeQuest.currentSquareId = ss.squareId;
                } else {
                    ss.status = 0;
                }
                gradeQuest.squares.push_back(ss);
            }

            for (auto& sq : gradeQuest.squares) {
                if (sq.status != 0 || sq.openSquareIds.empty()) continue;
                bool allDepsMet = true;
                for (int depId : sq.openSquareIds) {
                    bool found = false;
                    for (const auto& other : gradeQuest.squares) {
                        if (other.squareId == depId && other.status == 2) {
                            found = true;
                            break;
                        }
                    }
                    if (!found) { allDepsMet = false; break; }
                }
                if (allDepsMet) {
                    sq.status = 1;
                }
            }
        }

        nlohmann::json questSquareList = nlohmann::json::array();
        for (const auto& sq : gradeQuest.squares) {
            questSquareList.push_back({
                {"grade_quest_square_id", sq.squareId},
                {"status", sq.status},
                {"live_point", sq.livePoint},
            });
        }

        nlohmann::json rewardList = nlohmann::json::array();
        if (gradeQuest.rewardsJson.is_array()) {
            for (const auto& r : gradeQuest.rewardsJson) {
                rewardList.push_back({
                    {"grade_quest_rewards_id", r.value("grade_quest_rewards_id", 0)},
                    {"is_received", false},
                });
            }
        }

        nlohmann::json activeSkills = nlohmann::json::array();
        for (int id : gradeQuest.activeAddSkillIds) {
            activeSkills.push_back(id);
        }

        nlohmann::json result;
        result["character_id"] = gradeQuest.characterId;
        result["current_square_id"] = gradeQuest.currentSquareId;
        result["action_point"] = gradeQuest.actionPoint;
        result["quest_square_list"] = std::move(questSquareList);
        result["active_add_skill_id_list"] = std::move(activeSkills);
        result["lot_add_skill_id_list"] = nlohmann::json::array();
        result["reward_list"] = std::move(rewardList);
        {
            int bonusCount = 0;
            if (db) {
                sqlite3_stmt* bStmt = nullptr;
                constexpr const char* bSql =
                    "SELECT COUNT(*) FROM grade_quest_progress p "
                    "JOIN grade_quest_series sr ON sr.series_id = p.series_id "
                    "JOIN grade_quest_season s ON s.season_id = sr.season_id "
                    "WHERE p.bonus_cleared = 1 AND s.generation = ?;";
                if (sqlite3_prepare_v2(db, bSql, -1, &bStmt, nullptr) == SQLITE_OK && bStmt) {
                    sqlite3_bind_int(bStmt, 1, gradeQuest.generation);
                    if (sqlite3_step(bStmt) == SQLITE_ROW) {
                        bonusCount = sqlite3_column_int(bStmt, 0);
                    }
                    sqlite3_finalize(bStmt);
                }
            }
            int globalClearedCount = 0;
            {
                sqlite3_stmt* gcStmt = nullptr;
                constexpr const char* gcSql = "SELECT COUNT(*) FROM grade_quest_progress WHERE clear_status > 0;";
                if (sqlite3_prepare_v2(db, gcSql, -1, &gcStmt, nullptr) == SQLITE_OK && gcStmt) {
                    if (sqlite3_step(gcStmt) == SQLITE_ROW) {
                        globalClearedCount = sqlite3_column_int(gcStmt, 0);
                    }
                    sqlite3_finalize(gcStmt);
                }
            }
            const int gradeNum = 300 + globalClearedCount;
            nlohmann::json bonusList = nlohmann::json::array();
            if (bonusCount > 0) {
                bonusList.push_back({
                    {"bonus_type", 2}, {"target_detail", 2},
                    {"target_num", gradeQuest.generation}, {"bonus_value", bonusCount * 500},
                });
            }
            bonusList.push_back({
                {"bonus_type", 1}, {"target_detail", 0},
                {"target_num", gradeNum}, {"bonus_value", 10000},
            });
            result["point_bonus_list"] = std::move(bonusList);
        }

        MockStoredResponse response;
        response.body = result.dump();
        return response;
    }

    std::optional<MockStoredResponse> HttpMockBackend::Impl::GradeSetQuestActionLocked(std::string_view payloadJson) {
        auto payload = nlohmann::json::parse(payloadJson.begin(), payloadJson.end(), nullptr, false);
        const int squareId = payload.is_object() ? payload.value("grade_quest_square_id", 0) : 0;

        int squareType = 2;
        int targetId = 0;
        int actionCost = 0;
        for (auto& sq : gradeQuest.squares) {
            if (sq.squareId == squareId) {
                squareType = sq.squareType;
                targetId = sq.targetId;
                actionCost = sq.actionPointCost;
                sq.status = 2;
                gradeQuest.currentSquareId = squareId;
                break;
            }
        }

        gradeQuest.actionPoint = std::max(0, gradeQuest.actionPoint - actionCost);

        for (auto& sq : gradeQuest.squares) {
            if (sq.status != 0) continue;
            bool allDepsMet = true;
            for (int depId : sq.openSquareIds) {
                bool found = false;
                for (const auto& other : gradeQuest.squares) {
                    if (other.squareId == depId && other.status == 2) {
                        found = true;
                        break;
                    }
                }
                if (!found) { allDepsMet = false; break; }
            }
            if (allDepsMet && !sq.openSquareIds.empty()) {
                sq.status = 1;
            }
        }

        nlohmann::json result;
        result["action_point"] = gradeQuest.actionPoint;
        result["square_type"] = squareType;
        if (squareType == 4 && db) {
            int tier = (targetId >= 1 && targetId <= 3) ? targetId : 1;
            nlohmann::json skillList = nlohmann::json::array();
            sqlite3_stmt* stmt = nullptr;
            const char* sql = "SELECT skill_id FROM grade_add_skill WHERE tier = ? ORDER BY RANDOM() LIMIT 3";
            if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
                sqlite3_bind_int(stmt, 1, tier);
                while (sqlite3_step(stmt) == SQLITE_ROW) {
                    skillList.push_back(sqlite3_column_int(stmt, 0));
                }
                sqlite3_finalize(stmt);
            }
            if (skillList.empty()) {
                skillList = nlohmann::json::array({41004050, 41009005, 41004100});
            }
            result["add_skill_id_list"] = skillList;
        } else {
            result["add_skill_id_list"] = nullptr;
        }

        MockStoredResponse response;
        response.body = result.dump();
        return response;
    }

    std::optional<MockStoredResponse> HttpMockBackend::Impl::GradeSetQuestAddSkillLocked(std::string_view payloadJson) {
        auto payload = nlohmann::json::parse(payloadJson.begin(), payloadJson.end(), nullptr, false);
        if (payload.is_object()) {
            const int skillId = payload.value("grade_add_skills_id", 0);
            if (skillId > 0) {
                gradeQuest.activeAddSkillIds.push_back(skillId);
            }
            const int squareId = payload.value("grade_quest_square_id", 0);
            for (auto& sq : gradeQuest.squares) {
                if (sq.squareId == squareId) {
                    sq.status = 2;
                    break;
                }
            }
        }

        nlohmann::json result;
        result["action_point"] = gradeQuest.actionPoint;

        MockStoredResponse response;
        response.body = result.dump();
        return response;
    }

    std::optional<MockStoredResponse> HttpMockBackend::Impl::GradeGetStageDataLocked(std::string_view payloadJson) {
        if (!initialized && !EnsureReadyLocked()) {
            return std::nullopt;
        }
        if (!db) {
            return std::nullopt;
        }

        auto payload = nlohmann::json::parse(payloadJson.begin(), payloadJson.end(), nullptr, false);
        const int squareId = payload.is_object() ? payload.value("grade_quest_square_id", 0) : 0;

        int targetId = 0;
        int existingLivePoint = 0;
        for (const auto& sq : gradeQuest.squares) {
            if (sq.squareId == squareId) {
                targetId = sq.targetId;
                existingLivePoint = sq.livePoint;
                break;
            }
        }

        nlohmann::json musicList = nlohmann::json::array();
        if (targetId > 0) {
            sqlite3_stmt* stmt = nullptr;
            constexpr const char* sql =
                "SELECT quest_musics_type, quest_musics_detail FROM grade_quest_stage WHERE stage_id = ?;";
            if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) == SQLITE_OK && stmt) {
                sqlite3_bind_int(stmt, 1, targetId);
                if (sqlite3_step(stmt) == SQLITE_ROW) {
                    const int mType = sqlite3_column_int(stmt, 0);
                    const int mDetail = sqlite3_column_int(stmt, 1);
                    if (mType == 2 && mDetail > 0) {
                        musicList.push_back({{"m_musics_id", mDetail}, {"is_enable", true}});
                    } else if (mType == 0 && mDetail > 0) {
                        sqlite3_stmt* mStmt = nullptr;
                        constexpr const char* mSql =
                            "SELECT music_id FROM music WHERE generations_id = ? AND has_score = 1 ORDER BY music_id;";
                        if (sqlite3_prepare_v2(db, mSql, -1, &mStmt, nullptr) == SQLITE_OK && mStmt) {
                            sqlite3_bind_int(mStmt, 1, mDetail);
                            while (sqlite3_step(mStmt) == SQLITE_ROW) {
                                musicList.push_back({
                                    {"m_musics_id", sqlite3_column_int(mStmt, 0)},
                                    {"is_enable", true},
                                });
                            }
                            sqlite3_finalize(mStmt);
                        }
                    }
                }
                sqlite3_finalize(stmt);
            }
        }

        nlohmann::json result;
        result["live_point"] = existingLivePoint;
        result["music_list"] = std::move(musicList);

        MockStoredResponse response;
        response.body = result.dump();
        return response;
    }

    std::optional<MockStoredResponse> HttpMockBackend::Impl::GradeGetResultLocked(std::string_view payloadJson) {
        if (!initialized && !EnsureReadyLocked()) {
            return std::nullopt;
        }

        const int64_t score = currentQuestLive.score;
        const int64_t livePoint = score / 1000;

        int64_t requiredLivePoint = 0;
        if (db) {
            sqlite3_stmt* stmt = nullptr;
            constexpr const char* sql = "SELECT live_point FROM grade_quest_stage WHERE stage_id = ?;";
            if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) == SQLITE_OK && stmt) {
                sqlite3_bind_int(stmt, 1, currentQuestLive.stageId);
                if (sqlite3_step(stmt) == SQLITE_ROW) {
                    requiredLivePoint = sqlite3_column_int(stmt, 0);
                }
                sqlite3_finalize(stmt);
            }
        }

        gradeQuest.actionPoint = std::max(0, gradeQuest.actionPoint - gradeQuest.resourceValue);
        gradeQuest.resourceValue = 0;

        int64_t beforeLivePoint = 0;
        for (const auto& sq : gradeQuest.squares) {
            if (sq.targetId == currentQuestLive.stageId) {
                beforeLivePoint = sq.livePoint;
                break;
            }
        }
        const int64_t totalLivePoint = beforeLivePoint + livePoint;
        const bool questResult = (totalLivePoint >= requiredLivePoint);

        for (auto& sq : gradeQuest.squares) {
            if (sq.targetId == currentQuestLive.stageId) {
                sq.livePoint = totalLivePoint;
                if (questResult) {
                    sq.status = 2;
                    gradeQuest.currentSquareId = sq.squareId;
                }
                break;
            }
        }

        if (questResult) {
            for (auto& sq : gradeQuest.squares) {
                if (sq.status != 0) continue;
                bool allDepsMet = true;
                for (int depId : sq.openSquareIds) {
                    bool found = false;
                    for (const auto& other : gradeQuest.squares) {
                        if (other.squareId == depId && other.status == 2) {
                            found = true;
                            break;
                        }
                    }
                    if (!found) { allDepsMet = false; break; }
                }
                if (allDepsMet && !sq.openSquareIds.empty()) {
                    sq.status = 1;
                }
            }
        }

        bool allCleared = true;
        for (const auto& sq : gradeQuest.squares) {
            if (sq.squareType == 7 && sq.status != 2) {
                allCleared = false;
                break;
            }
        }

        nlohmann::json result;
        result["quest_live_type"] = 4;
        result["quest_stage_id"] = currentQuestLive.stageId;
        result["quest_result"] = questResult;
        result["result_love"] = score;
        result["play_report"] = currentQuestLive.playReport;
        result["add_live_point"] = livePoint;
        result["before_live_point"] = beforeLivePoint;
        result["point_bonus_list"] = nlohmann::json::array();
        result["action_point"] = gradeQuest.actionPoint;

        if (allCleared) {
            bool hasBonusSquare = false;
            for (const auto& sq : gradeQuest.squares) {
                if (sq.squareType == 6 && sq.status == 2) {
                    hasBonusSquare = true;
                    break;
                }
            }
            if (db) {
                sqlite3_stmt* pStmt = nullptr;
                constexpr const char* pSql =
                    "INSERT OR REPLACE INTO grade_quest_progress (series_id, clear_status, bonus_cleared) "
                    "VALUES (?, 2, ?);";
                if (sqlite3_prepare_v2(db, pSql, -1, &pStmt, nullptr) == SQLITE_OK && pStmt) {
                    sqlite3_bind_int(pStmt, 1, gradeQuest.seriesId);
                    sqlite3_bind_int(pStmt, 2, hasBonusSquare ? 1 : 0);
                    sqlite3_step(pStmt);
                    sqlite3_finalize(pStmt);
                }
            }

            nlohmann::json goalSquares = nlohmann::json::array();
            for (const auto& sq : gradeQuest.squares) {
                goalSquares.push_back({
                    {"grade_quest_square_id", sq.squareId},
                    {"status", sq.status},
                    {"live_point", sq.livePoint},
                });
            }
            nlohmann::json rewardList = nlohmann::json::array();
            nlohmann::json clearRewardIds = nlohmann::json::array();
            if (gradeQuest.rewardsJson.is_array()) {
                for (const auto& r : gradeQuest.rewardsJson) {
                    const int rwId = r.value("grade_quest_rewards_id", 0);
                    clearRewardIds.push_back(rwId);
                    rewardList.push_back({
                        {"grade_quest_rewards_id", rwId},
                        {"is_received", true},
                    });
                }
            }
            int globalClearedCount = 0;
            if (db) {
                sqlite3_stmt* cStmt = nullptr;
                constexpr const char* cSql = "SELECT COUNT(*) FROM grade_quest_progress WHERE clear_status > 0;";
                if (sqlite3_prepare_v2(db, cSql, -1, &cStmt, nullptr) == SQLITE_OK && cStmt) {
                    if (sqlite3_step(cStmt) == SQLITE_ROW) {
                        globalClearedCount = sqlite3_column_int(cStmt, 0);
                    }
                    sqlite3_finalize(cStmt);
                }
            }
            const int afterGradeNum = 300 + globalClearedCount;
            result["goal_clear_data"] = {
                {"character_id", gradeQuest.characterId},
                {"current_square_id", gradeQuest.currentSquareId},
                {"quest_square_list", std::move(goalSquares)},
                {"clear_grade_quest_rewards_id_list", std::move(clearRewardIds)},
                {"reward_list", std::move(rewardList)},
                {"grade_up_data", {
                    {"before_grade_num", afterGradeNum - 1},
                    {"after_grade_num", afterGradeNum},
                }},
                {"is_not_received_grade_reward", false},
            };
            gradeQuest.active = false;
        } else {
            result["goal_clear_data"] = nullptr;
        }

        MockStoredResponse response;
        response.body = result.dump();
        return response;
    }

    std::optional<MockStoredResponse> HttpMockBackend::Impl::GradeSetQuestRetireLocked(std::string_view payloadJson) {
        nlohmann::json rewardList = nlohmann::json::array();
        if (gradeQuest.rewardsJson.is_array()) {
            for (const auto& r : gradeQuest.rewardsJson) {
                rewardList.push_back({
                    {"grade_quest_rewards_id", r.value("grade_quest_rewards_id", 0)},
                    {"is_received", false},
                });
            }
        }

        nlohmann::json result;
        result["clear_grade_quest_rewards_id_list"] = nlohmann::json::array();
        result["live_point"] = 0;
        result["reward_list"] = std::move(rewardList);
        result["grade_up_data"] = nullptr;

        gradeQuest = {};

        MockStoredResponse response;
        response.body = result.dump();
        return response;
    }

    // ─── Grand Prix ───

    // Solo Grand Prix total: the sum of each quest's best daily point. There is
    // no other player, so the personal ranking is 1 once anything is scored.
    int64_t HttpMockBackend::Impl::GrandPrixTotalPointLocked(int grandPrixId) {
        sqlite3_stmt* stmt = nullptr;
        int64_t total = 0;
        if (db && sqlite3_prepare_v2(db,
                "SELECT COALESCE(SUM(best), 0) FROM ("
                "SELECT s.quest_id, MAX(COALESCE(p.daily_best_score, 0)) AS best "
                "FROM grand_prix_series gs JOIN grand_prix_stage s ON s.quest_id = gs.quest_id "
                "LEFT JOIN grand_prix_progress p ON p.stage_id = s.stage_id "
                "WHERE gs.grand_prix_id = ? GROUP BY s.quest_id);",
                -1, &stmt, nullptr) == SQLITE_OK && stmt) {
            sqlite3_bind_int(stmt, 1, grandPrixId);
            if (sqlite3_step(stmt) == SQLITE_ROW) total = sqlite3_column_int64(stmt, 0);
        }
        if (stmt) sqlite3_finalize(stmt);
        return total;
    }

    std::optional<MockStoredResponse> HttpMockBackend::Impl::GrandPrixGetTopInfoLocked(std::string_view payloadJson) {
        if (!initialized && !EnsureReadyLocked()) return std::nullopt;
        if (!db) return std::nullopt;

        constexpr int gpId = 806101;

        std::string startDate, endDate;
        int gpType = 1;
        nlohmann::json rewardsJson = nlohmann::json::array();
        {
            sqlite3_stmt* stmt = nullptr;
            constexpr const char* sql =
                "SELECT grand_prix_type, start_date, end_date, rewards_json FROM grand_prix WHERE grand_prix_id = ?;";
            if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) == SQLITE_OK && stmt) {
                sqlite3_bind_int(stmt, 1, gpId);
                if (sqlite3_step(stmt) == SQLITE_ROW) {
                    gpType = sqlite3_column_int(stmt, 0);
                    const auto* sd = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
                    const auto* ed = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
                    if (sd) startDate = sd;
                    if (ed) endDate = ed;
                    const auto* rj = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3));
                    auto rp = nlohmann::json::parse(rj ? rj : "[]", nullptr, false);
                    if (rp.is_array()) rewardsJson = std::move(rp);
                }
                sqlite3_finalize(stmt);
            }
        }

        nlohmann::json result;
        result["start_date"] = startDate;
        result["end_date"] = endDate;
        result["grand_prix_type"] = gpType;
        result["status"] = 2;
        result["search_guild_key"] = "";
        result["guild_name"] = "";
        result["guild_total_point"] = 0;
        result["guild_ranking"] = 0;
        result["within_guild_ranking"] = 0;
        const int64_t totalPoint = GrandPrixTotalPointLocked(gpId);
        result["personal_total_point"] = totalPoint;
        result["personal_ranking"] = totalPoint > 0 ? 1 : 0;
        result["get_points"] = nlohmann::json::array();
        result["personal_rewards"] = std::move(rewardsJson);

        MockStoredResponse response;
        response.body = result.dump();
        return response;
    }

    std::optional<MockStoredResponse> HttpMockBackend::Impl::GrandPrixGetStageSelectLocked(std::string_view payloadJson) {
        if (!initialized && !EnsureReadyLocked()) return std::nullopt;
        if (!db) return std::nullopt;

        constexpr int gpId = 806101;

        nlohmann::json stages = nlohmann::json::array();
        int64_t dailyTotalPoint = 0;
        int64_t bestPoint = 0;
        {
            sqlite3_stmt* stmt = nullptr;
            constexpr const char* sql =
                "SELECT quest_id FROM grand_prix_series WHERE grand_prix_id = ? ORDER BY order_id;";
            if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) == SQLITE_OK && stmt) {
                sqlite3_bind_int(stmt, 1, gpId);
                while (sqlite3_step(stmt) == SQLITE_ROW) {
                    const int questId = sqlite3_column_int(stmt, 0);

                    int64_t questHighScore = 0;
                    int64_t questDailyBest = 0;
                    int totalPlayCount = 0;
                    int dailyStageId = 0;
                    int dailyMusicId = 0;
                    {
                        sqlite3_stmt* pStmt = nullptr;
                        constexpr const char* pSql =
                            "SELECT s.stage_id, p.high_score, p.daily_best_score, p.play_count "
                            "FROM grand_prix_stage s "
                            "LEFT JOIN grand_prix_progress p ON p.stage_id = s.stage_id "
                            "WHERE s.quest_id = ? ORDER BY COALESCE(p.daily_best_score, 0) DESC;";
                        if (sqlite3_prepare_v2(db, pSql, -1, &pStmt, nullptr) == SQLITE_OK && pStmt) {
                            sqlite3_bind_int(pStmt, 1, questId);
                            bool first = true;
                            while (sqlite3_step(pStmt) == SQLITE_ROW) {
                                const int64_t hs = sqlite3_column_int64(pStmt, 1);
                                const int64_t ds = sqlite3_column_int64(pStmt, 2);
                                const int pc = sqlite3_column_int(pStmt, 3);
                                if (hs > questHighScore) questHighScore = hs;
                                if (first && ds > 0) {
                                    dailyStageId = sqlite3_column_int(pStmt, 0);
                                    questDailyBest = ds;
                                }
                                totalPlayCount += pc;
                                first = false;
                            }
                            sqlite3_finalize(pStmt);
                        }
                    }

                    dailyTotalPoint += questDailyBest;
                    if (questDailyBest > bestPoint) bestPoint = questDailyBest;

                    stages.push_back({
                        {"quest_id", questId},
                        {"daily_stage_id", dailyStageId},
                        {"daily_point", questDailyBest},
                        {"daily_music_id", dailyMusicId},
                        {"daily_rank", (questDailyBest > 0) ? 1 : 0},
                        {"high_score", questHighScore},
                        {"high_score_rank", (questHighScore > 0) ? 1 : 0},
                        {"playable_count", 1},
                        {"play_count", totalPlayCount > 0 ? 1 : 0},
                        {"play_count_bonus_value", 10000},
                    });
                }
                sqlite3_finalize(stmt);
            }
        }

        nlohmann::json result;
        result["daily_total_point"] = dailyTotalPoint;
        result["best_point"] = bestPoint;
        result["stages"] = std::move(stages);
        // quest_id 0 is the overall "合計グランプリPt." row.
        const int64_t gpTotal = GrandPrixTotalPointLocked(gpId);
        result["stage_ranking_info_list"] = nlohmann::json::array({
            {{"quest_id", 0}, {"rank_display_type", 1}, {"rank", gpTotal > 0 ? 1 : 0}, {"point", gpTotal}},
        });
        result["predicated_ranking_score_info"] = {
            {"ranking_score", gpTotal}, {"contents_type", 2},
        };

        MockStoredResponse response;
        response.body = result.dump();
        return response;
    }

    std::optional<MockStoredResponse> HttpMockBackend::Impl::GrandPrixGetStageListLocked(std::string_view payloadJson) {
        if (!initialized && !EnsureReadyLocked()) return std::nullopt;
        if (!db) return std::nullopt;

        auto payload = nlohmann::json::parse(payloadJson.begin(), payloadJson.end(), nullptr, false);
        const int questId = payload.is_object() ? payload.value("quest_id", 0) : 0;

        std::unordered_map<int, bool> clearedStages;
        {
            sqlite3_stmt* stmt = nullptr;
            constexpr const char* sql =
                "SELECT p.stage_id FROM grand_prix_progress p "
                "JOIN grand_prix_stage s ON s.stage_id = p.stage_id "
                "WHERE s.quest_id = ? AND p.play_count > 0;";
            if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) == SQLITE_OK && stmt) {
                sqlite3_bind_int(stmt, 1, questId);
                while (sqlite3_step(stmt) == SQLITE_ROW) {
                    clearedStages[sqlite3_column_int(stmt, 0)] = true;
                }
                sqlite3_finalize(stmt);
            }
        }

        nlohmann::json stageList = nlohmann::json::array();
        {
            sqlite3_stmt* stmt = nullptr;
            constexpr const char* sql =
                "SELECT stage_id, release_condition_type, release_condition_value "
                "FROM grand_prix_stage WHERE quest_id = ? ORDER BY order_id;";
            if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) == SQLITE_OK && stmt) {
                sqlite3_bind_int(stmt, 1, questId);
                while (sqlite3_step(stmt) == SQLITE_ROW) {
                    const int stageId = sqlite3_column_int(stmt, 0);
                    const int condType = sqlite3_column_int(stmt, 1);
                    const int condVal = sqlite3_column_int(stmt, 2);

                    bool isLock = false;
                    if (condType == 1) {
                        isLock = (clearedStages.find(condVal) == clearedStages.end());
                    }

                    const int clearStatus = clearedStages.count(stageId) ? 3 : 0;

                    stageList.push_back({
                        {"stage_id", stageId},
                        {"clear_status", clearStatus},
                        {"is_lock", isLock},
                        {"playable_count", 1},
                        {"play_count", clearedStages.count(stageId) ? 1 : 0},
                        {"play_count_bonus_value", 10000},
                    });
                }
                sqlite3_finalize(stmt);
            }
        }

        nlohmann::json result;
        result["stage_list"] = std::move(stageList);

        MockStoredResponse response;
        response.body = result.dump();
        return response;
    }

    std::optional<MockStoredResponse> HttpMockBackend::Impl::GrandPrixGetStageDataLocked(std::string_view payloadJson) {
        if (!initialized && !EnsureReadyLocked()) return std::nullopt;
        if (!db) return std::nullopt;

        auto payload = nlohmann::json::parse(payloadJson.begin(), payloadJson.end(), nullptr, false);
        const int stageId = payload.is_object() ? payload.value("stage_id", 0) : 0;

        int musicsType = 0;
        int musicsDetail = 0;
        {
            sqlite3_stmt* stmt = nullptr;
            constexpr const char* sql =
                "SELECT quest_musics_type, quest_musics_detail FROM grand_prix_stage WHERE stage_id = ?;";
            if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) == SQLITE_OK && stmt) {
                sqlite3_bind_int(stmt, 1, stageId);
                if (sqlite3_step(stmt) == SQLITE_ROW) {
                    musicsType = sqlite3_column_int(stmt, 0);
                    musicsDetail = sqlite3_column_int(stmt, 1);
                }
                sqlite3_finalize(stmt);
            }
        }

        nlohmann::json musicList = nlohmann::json::array();
        if (musicsType == 2 && musicsDetail > 0) {
            musicList.push_back({{"m_musics_id", musicsDetail}, {"is_enable", true}});
        } else {
            sqlite3_stmt* mStmt = nullptr;
            constexpr const char* mSql =
                "SELECT music_id FROM music WHERE has_score = 1 ORDER BY music_id;";
            if (sqlite3_prepare_v2(db, mSql, -1, &mStmt, nullptr) == SQLITE_OK && mStmt) {
                while (sqlite3_step(mStmt) == SQLITE_ROW) {
                    musicList.push_back({
                        {"m_musics_id", sqlite3_column_int(mStmt, 0)},
                        {"is_enable", true},
                    });
                }
                sqlite3_finalize(mStmt);
            }
        }

        int playCount = 0;
        {
            sqlite3_stmt* stmt = nullptr;
            constexpr const char* sql = "SELECT play_count FROM grand_prix_progress WHERE stage_id = ?;";
            if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) == SQLITE_OK && stmt) {
                sqlite3_bind_int(stmt, 1, stageId);
                if (sqlite3_step(stmt) == SQLITE_ROW) {
                    playCount = sqlite3_column_int(stmt, 0);
                }
                sqlite3_finalize(stmt);
            }
        }

        nlohmann::json result;
        result["stage_name"] = "";
        result["clear_status"] = (playCount > 0) ? 3 : 0;
        result["stage_reward_list"] = nlohmann::json::array();
        result["playable_count"] = 1;
        result["play_count"] = playCount;
        result["play_count_bonus_value"] = 10000;
        result["skip_guarantee_point"] = 0;
        result["music_list"] = std::move(musicList);
        result["is_today_playable"] = true;

        MockStoredResponse response;
        response.body = result.dump();
        return response;
    }

    std::optional<MockStoredResponse> HttpMockBackend::Impl::GrandPrixGetRankingListLocked(std::string_view payloadJson) {
        nlohmann::json result;
        result["status"] = 2;
        result["my_rank"] = 0;
        result["lowest_rank"] = 0;
        result["point_rankings"] = nlohmann::json::array();

        MockStoredResponse response;
        response.body = result.dump();
        return response;
    }

    std::optional<MockStoredResponse> HttpMockBackend::Impl::GrandPrixGetResultLocked(std::string_view payloadJson) {
        if (!initialized && !EnsureReadyLocked()) return std::nullopt;

        const int64_t score = currentQuestLive.score;
        const int stageId = currentQuestLive.stageId;
        const int musicId = currentQuestLive.musicId;
        const int64_t dailyScore = score / 1000;

        int64_t beforeHighScore = 0;
        if (db) {
            sqlite3_stmt* stmt = nullptr;
            constexpr const char* sql = "SELECT high_score FROM grand_prix_progress WHERE stage_id = ?;";
            if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) == SQLITE_OK && stmt) {
                sqlite3_bind_int(stmt, 1, stageId);
                if (sqlite3_step(stmt) == SQLITE_ROW) {
                    beforeHighScore = sqlite3_column_int64(stmt, 0);
                }
                sqlite3_finalize(stmt);
            }
        }

        if (db) {
            sqlite3_stmt* stmt = nullptr;
            constexpr const char* sql =
                "INSERT INTO grand_prix_progress (stage_id, high_score, daily_best_score, play_count) "
                "VALUES (?, ?, ?, 1) "
                "ON CONFLICT(stage_id) DO UPDATE SET "
                "high_score = MAX(high_score, excluded.high_score), "
                "daily_best_score = MAX(daily_best_score, excluded.daily_best_score), "
                "play_count = play_count + 1;";
            if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) == SQLITE_OK && stmt) {
                sqlite3_bind_int(stmt, 1, stageId);
                sqlite3_bind_int64(stmt, 2, score);
                sqlite3_bind_int64(stmt, 3, dailyScore);
                sqlite3_step(stmt);
                sqlite3_finalize(stmt);
            }
        }

        nlohmann::json result;
        result["quest_live_type"] = 6;
        result["stage_id"] = stageId;
        result["quest_result"] = true;
        result["result_love"] = score;
        result["best_love"] = std::max(score, beforeHighScore);
        result["before_best_love"] = beforeHighScore;
        result["add_style_point"] = 0;
        result["is_challenge_mode"] = false;
        result["music_id"] = musicId;
        result["reward_list"] = nlohmann::json::array();
        result["play_report"] = currentQuestLive.playReport;
        result["mastary_level_before"] = 1;
        result["mastary_level_after"] = 1;
        result["mastary_level_experience"] = 0;
        result["mastary_level_total_experience_before"] = 0;
        result["first_clear_flag"] = (beforeHighScore == 0);
        result["first_complete_clear_flag"] = (beforeHighScore == 0);
        result["is_limit_over_style_point"] = false;
        result["daily_score"] = dailyScore;
        result["before_high_score"] = beforeHighScore;
        result["is_rehearsal"] = false;
        result["is_guarantee"] = false;
        result["point_bonus_list"] = nlohmann::json::array();

        MockStoredResponse response;
        response.body = result.dump();
        return response;
    }

    HttpMockBackend& HttpMockBackend::Get() {
        static HttpMockBackend instance;
        return instance;
    }

    HttpMockBackend::HttpMockBackend()
        : impl_(std::make_unique<Impl>()) {}

    HttpMockBackend::~HttpMockBackend() = default;

    bool HttpMockBackend::Reset() {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        return impl_->ResetLocked();
    }

    bool HttpMockBackend::Rebuild() {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        return impl_->RebuildLocked();
    }

    bool HttpMockBackend::ResetAtFilesDir(const std::string& filesDir) {
        const std::string dbPath = filesDir + "/HasuKikaisann.sqlite3";

        sqlite3* db = nullptr;
        if (sqlite3_open_v2(dbPath.c_str(), &db,
                            SQLITE_OPEN_READWRITE | SQLITE_OPEN_FULLMUTEX, nullptr) != SQLITE_OK || !db) {
            if (db) sqlite3_close(db);
            Log::ErrorFmt("[HttpMockBackend] ResetAtFilesDir: open failed: %s", dbPath.c_str());
            return false;
        }

        bool ok = ExecBuiltInSqlScripts(db, HttpMockBackendBuiltInSql::SchemaScripts, "schema") &&
                  ExecSql(db, HttpMockBackendBuiltInSql::QueriesResetMuseumStateSqlView.data()) &&
                  ExecSql(db, "DELETE FROM archive_detail;") &&
                  ExecSql(db, "DELETE FROM card_detail;") &&
                  ExecSql(db, "DELETE FROM character_info;") &&
                  ExecSql(db, "DELETE FROM item;") &&
                  ExecSql(db, "DELETE FROM deck;") &&
                  ExecSql(db, "DELETE FROM rhythm_music_score;") &&
                  ExecSql(db, "DELETE FROM rhythm_game_deck;") &&
                  ExecSql(db, "DELETE FROM rhythm_runtime_session;") &&
                  ExecSql(db, "DELETE FROM quest_runtime_session;") &&
                  ExecSql(db, "DELETE FROM quest_stage;") &&
                  ExecSql(db, "DELETE FROM daily_quest_stage;") &&
                  ExecSql(db, "DELETE FROM dream_quest_stage;") &&
                  ExecSql(db, "DELETE FROM grade_quest_season;") &&
                  ExecSql(db, "DELETE FROM grade_quest_series;") &&
                  ExecSql(db, "DELETE FROM grade_quest_stage;") &&
                  ExecSql(db, "DELETE FROM grade_add_skill;") &&
                  ExecSql(db, "DELETE FROM grade_quest_progress;") &&
                  ExecSql(db, "DELETE FROM grand_prix_progress;") &&
                  ExecSql(db, "DELETE FROM grand_prix;") &&
                  ExecSql(db, "DELETE FROM grand_prix_series;") &&
                  ExecSql(db, "DELETE FROM grand_prix_stage;") &&
                  ExecSql(db, "DELETE FROM learning_stage;") &&
                  ExecSql(db, "DELETE FROM music_mastery;") &&
                  ExecSql(db, "DELETE FROM music;") &&
                  ExecBuiltInSqlScripts(db, HttpMockBackendBuiltInSql::SeedScripts, "seed");

        sqlite3_close(db);
        return ok;
    }

    bool HttpMockBackend::RebuildAtFilesDir(const std::string& filesDir) {
        const std::string dbPath = filesDir + "/HasuKikaisann.sqlite3";

        std::error_code ec;
        std::filesystem::remove(dbPath, ec);
        if (ec) {
            Log::WarnFmt("[HttpMockBackend] RebuildAtFilesDir: remove failed: %s", ec.message().c_str());
        }

        sqlite3* db = nullptr;
        if (sqlite3_open_v2(dbPath.c_str(), &db,
                            SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX, nullptr) != SQLITE_OK || !db) {
            if (db) sqlite3_close(db);
            Log::ErrorFmt("[HttpMockBackend] RebuildAtFilesDir: open failed: %s", dbPath.c_str());
            return false;
        }

        bool ok = ExecBuiltInSqlScripts(db, HttpMockBackendBuiltInSql::SchemaScripts, "schema") &&
                  ExecBuiltInSqlScripts(db, HttpMockBackendBuiltInSql::SeedScripts, "seed");

        sqlite3_close(db);
        return ok;
    }

    std::optional<MockStoredResponse> HttpMockBackend::GetArchiveDetailById(std::string_view archivesId) {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        return impl_->GetArchiveDetailByIdLocked(archivesId);
    }

    std::optional<MockStoredResponse> HttpMockBackend::LookupArchiveDetailFromPayload(std::string_view payloadJson,
                                                                                      std::string_view fallbackArchiveId) {
        auto archivesId = ExtractPayloadStringField(payloadJson, "archives_id");
        if (archivesId.empty()) {
            archivesId = std::string(fallbackArchiveId);
        }
        if (archivesId.empty()) {
            return std::nullopt;
        }
        return GetArchiveDetailById(archivesId);
    }

    bool HttpMockBackend::IsPersistentStorageAvailable() const {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        if (!impl_->initialized) {
            impl_->EnsureReadyLocked();
        }
        return impl_->persistentStorageAvailable;
    }

    std::string HttpMockBackend::GetStatusSummary() const {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        if (!impl_->initialized) {
            impl_->EnsureReadyLocked();
        }

        if (impl_->persistentStorageAvailable) {
            return "sqlite";
        }

        return "sqlite-unavailable";
    }

    std::string HttpMockBackend::ExtractPayloadStringField(std::string_view payloadJson, std::string_view fieldName) {
        return ExtractJsonStringField(payloadJson, fieldName);
    }

    std::optional<MockStoredResponse> HttpMockBackend::GetHomeResponse() {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        return impl_->GetHomeResponseLocked();
    }

    std::optional<MockStoredResponse> HttpMockBackend::GetMissionListResponse() {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        return impl_->GetMissionListResponseLocked();
    }

    // Every response carries the wallet in its headers and the client updates
    // the displayed jewel balance from whichever response arrived last. Only
    // the gacha and Home handlers used to write the live wallet; all other
    // routes sent the template value (999999), so the balance flipped between
    // screens. Rewrite the wallet lines from the persistent gacha wallet.
    std::string HttpMockBackend::ApplyLiveWalletHeaders(std::string headers) {
        if (headers.find("jewel_") == std::string::npos) return headers;
        std::lock_guard<std::mutex> lock(impl_->mutex);
        if (!impl_->initialized && !impl_->EnsureReadyLocked()) return headers;
        if (!impl_->db) return headers;
        std::optional<std::pair<int64_t, int64_t>> wallet;
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(impl_->db,
                "SELECT free_jewels, paid_jewels FROM museum_gacha_state WHERE series_id=1055003;",
                -1, &stmt, nullptr) == SQLITE_OK && stmt) {
            if (sqlite3_step(stmt) == SQLITE_ROW) {
                wallet = std::make_pair(sqlite3_column_int64(stmt, 0), sqlite3_column_int64(stmt, 1));
            }
        }
        if (stmt) sqlite3_finalize(stmt);
        if (!wallet) {
            const auto state = impl_->LoadGachaStateLocked();
            if (!state) return headers;
            wallet = std::make_pair(state->freeJewels, state->paidJewels);
        }
        const auto rewrite = [&headers](std::string_view key, int64_t value) {
            const std::string prefix = std::string(key) + ": ";
            size_t pos = 0;
            while ((pos = headers.find(prefix, pos)) != std::string::npos) {
                if (pos != 0 && headers[pos - 1] != '\n') { pos += prefix.size(); continue; }
                const size_t valueStart = pos + prefix.size();
                size_t valueEnd = headers.find_first_of("\r\n", valueStart);
                if (valueEnd == std::string::npos) valueEnd = headers.size();
                headers.replace(valueStart, valueEnd - valueStart, std::to_string(value));
                pos = valueStart;
            }
        };
        rewrite("jewel_free", wallet->first);
        rewrite("jewel_paid_google", wallet->second);
        // プレイPt. lives in the same headers (user_stamina JSON line).
        const std::string staminaKey = "user_stamina: ";
        if (const auto pos = headers.find(staminaKey);
            pos != std::string::npos && (pos == 0 || headers[pos - 1] == '\n')) {
            const size_t valueStart = pos + staminaKey.size();
            size_t valueEnd = headers.find_first_of("\r\n", valueStart);
            if (valueEnd == std::string::npos) valueEnd = headers.size();
            headers.replace(valueStart, valueEnd - valueStart, impl_->StaminaJsonLocked().dump());
        }
        return headers;
    }

    std::optional<MockStoredResponse> HttpMockBackend::GetMyDesignList(bool icon) {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        return impl_->GetMyDesignListLocked(icon ? kMyDesignTypeIcon : kMyDesignTypeCard);
    }

    std::optional<MockStoredResponse> HttpMockBackend::SetMyDesign(bool icon, std::string_view payloadJson) {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        return impl_->SetMyDesignLocked(icon ? kMyDesignTypeIcon : kMyDesignTypeCard, payloadJson);
    }

    std::optional<MockStoredResponse> HttpMockBackend::SetMyDesignName(std::string_view payloadJson) {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        return impl_->SetMyDesignNameLocked(payloadJson);
    }

    std::optional<MockStoredResponse> HttpMockBackend::SetProfileDesign(bool icon, std::string_view payloadJson) {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        return impl_->SetProfileDesignLocked(icon ? kMyDesignTypeIcon : kMyDesignTypeCard, payloadJson);
    }

    std::optional<MockStoredResponse> HttpMockBackend::GetProfileInfoResponse() {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        return impl_->GetProfileInfoResponseLocked();
    }

    std::optional<MockStoredResponse> HttpMockBackend::GetGachaSeriesListResponse() {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        return impl_->GetGachaSeriesListResponseLocked();
    }

    std::optional<MockStoredResponse> HttpMockBackend::GachaSetPurchase(std::string_view payloadJson) {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        return impl_->GachaSetPurchaseLocked(payloadJson);
    }

    std::optional<MockStoredResponse> HttpMockBackend::GetGachaHistoryResponse() {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        return impl_->GetGachaHistoryResponseLocked();
    }

    std::optional<MockStoredResponse> HttpMockBackend::GetGachaGuaranteePointList(
            std::string_view payloadJson) {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        return impl_->GetGachaGuaranteePointListLocked(payloadJson);
    }

    std::optional<MockStoredResponse> HttpMockBackend::GachaSetGuaranteePointExchange(
            std::string_view payloadJson) {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        return impl_->GachaSetGuaranteePointExchangeLocked(payloadJson);
    }

    std::optional<MockStoredResponse> HttpMockBackend::GetCardDetailByDCardId(std::string_view dCardDatasId) {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        return impl_->GetCardDetailByDCardIdLocked(dCardDatasId);
    }

    std::optional<MockStoredResponse> HttpMockBackend::GetCardListResponse() {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        return impl_->GetCardListResponseLocked();
    }

    std::optional<MockStoredResponse> HttpMockBackend::LookupCardDetailFromPayload(std::string_view payloadJson) {
        const auto dCardDatasId = ExtractPayloadStringField(payloadJson, "d_card_datas_id");
        if (dCardDatasId.empty()) {
            return std::nullopt;
        }
        return GetCardDetailByDCardId(dCardDatasId);
    }

    std::optional<MockStoredResponse> HttpMockBackend::CheckStyleLevelUp(std::string_view payloadJson) {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        return impl_->CheckStyleLevelUpLocked(payloadJson);
    }

    std::optional<MockStoredResponse> HttpMockBackend::CardGrowth(std::string_view action, std::string_view payloadJson) {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        if (action == "style_level_up") return impl_->StyleLevelUpLocked(payloadJson);
        if (action == "check_limit_break") return impl_->CheckLimitBreakLocked(payloadJson);
        if (action == "limit_break") return impl_->LimitBreakLocked(payloadJson);
        if (action == "check_evolution") return impl_->CheckEvolutionLocked(payloadJson);
        if (action == "evolution") return impl_->EvolutionLocked(payloadJson);
        if (action == "check_skill_level_up") return impl_->CheckSkillLevelUpLocked(payloadJson);
        return std::nullopt;
    }

    std::optional<MockStoredResponse> HttpMockBackend::GetItemDetailByDItemId(std::string_view dItemDatasId) {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        return impl_->GetItemDetailByDItemIdLocked(dItemDatasId);
    }

    std::optional<MockStoredResponse> HttpMockBackend::GetItemListResponse() {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        return impl_->GetItemListResponseLocked();
    }

    std::optional<MockStoredResponse> HttpMockBackend::GetItemExchangeListResponse(
            std::string_view payloadJson) {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        return impl_->GetItemExchangeListResponseLocked(payloadJson);
    }

    std::optional<MockStoredResponse> HttpMockBackend::ItemExchangeSetPurchase(
            std::string_view payloadJson) {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        return impl_->ItemExchangeSetPurchaseLocked(payloadJson);
    }

    std::optional<MockStoredResponse> HttpMockBackend::LookupItemDetailFromPayload(std::string_view payloadJson) {
        const auto dItemDatasId = ExtractPayloadStringField(payloadJson, "d_item_datas_id");
        if (dItemDatasId.empty()) {
            return std::nullopt;
        }
        return GetItemDetailByDItemId(dItemDatasId);
    }

    std::optional<MockStoredResponse> HttpMockBackend::GetCollectionMusicListResponse() {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        return impl_->GetCollectionListResponseLocked(
            HttpMockBackendBuiltInSql::QueriesCollectionMusicSqlView, "music_info_list"
        );
    }

    std::optional<MockStoredResponse> HttpMockBackend::GetCollectionStickerListResponse() {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        return impl_->GetCollectionListResponseLocked(
            HttpMockBackendBuiltInSql::QueriesCollectionStickerSqlView, "sticker_info_list"
        );
    }

    std::optional<MockStoredResponse> HttpMockBackend::GetCollectionGallaryListResponse() {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        return impl_->GetCollectionListResponseLocked(
            HttpMockBackendBuiltInSql::QueriesCollectionGallarySqlView, "gallary_info_list"
        );
    }

    std::optional<MockStoredResponse> HttpMockBackend::GetCharacterInfoById(std::string_view characterId) {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        return impl_->GetCharacterInfoByIdLocked(characterId);
    }

    std::optional<MockStoredResponse> HttpMockBackend::LookupCharacterInfoFromPayload(std::string_view payloadJson) {
        const auto characterId = ExtractPayloadIntegerFieldAsString(payloadJson, "character_id");
        if (characterId.empty()) {
            return std::nullopt;
        }
        return GetCharacterInfoById(characterId);
    }

    std::optional<MockStoredResponse> HttpMockBackend::GetDeckListResponse() {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        return impl_->GetDeckListResponseLocked();
    }

    std::optional<MockStoredResponse> HttpMockBackend::ModifyDeckList(std::string_view payloadJson) {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        return impl_->ModifyDeckListLocked(payloadJson);
    }

    std::optional<MockStoredResponse> HttpMockBackend::GetRhythmGameHome() {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        return impl_->GetRhythmGameHomeLocked();
    }

    std::optional<MockStoredResponse> HttpMockBackend::RhythmGameSetStart(std::string_view payloadJson) {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        return impl_->RhythmGameSetStartLocked(payloadJson);
    }

    std::optional<MockStoredResponse> HttpMockBackend::RhythmGameSetFinish(std::string_view payloadJson) {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        return impl_->RhythmGameSetFinishLocked(payloadJson);
    }

    std::optional<MockStoredResponse> HttpMockBackend::ModifyRhythmGameDeckList(std::string_view payloadJson) {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        return impl_->ModifyRhythmGameDeckListLocked(payloadJson);
    }

    std::optional<MockStoredResponse> HttpMockBackend::QuestStageSelect(std::string_view payloadJson) {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        return impl_->QuestStageSelectLocked(payloadJson);
    }

    std::optional<MockStoredResponse> HttpMockBackend::QuestStageData(std::string_view payloadJson) {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        return impl_->QuestStageDataLocked(payloadJson);
    }

    std::optional<MockStoredResponse> HttpMockBackend::QuestGetLiveSetting(std::string_view payloadJson) {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        return impl_->QuestGetLiveSettingLocked(payloadJson);
    }

    std::optional<MockStoredResponse> HttpMockBackend::QuestSetLiveSetting(std::string_view payloadJson) {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        return impl_->QuestSetLiveSettingLocked(payloadJson);
    }

    std::optional<MockStoredResponse> HttpMockBackend::QuestSetStart(std::string_view payloadJson) {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        return impl_->QuestSetStartLocked(payloadJson);
    }

    std::optional<MockStoredResponse> HttpMockBackend::QuestGetLiveInfo(std::string_view payloadJson) {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        return impl_->QuestGetLiveInfoLocked(payloadJson);
    }

    std::optional<MockStoredResponse> HttpMockBackend::QuestSetFinish(std::string_view payloadJson) {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        return impl_->QuestSetFinishLocked(payloadJson);
    }

    std::optional<MockStoredResponse> HttpMockBackend::QuestGetResult(std::string_view payloadJson) {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        return impl_->QuestGetResultLocked(payloadJson);
    }

    std::optional<MockStoredResponse> HttpMockBackend::QuestSkip(std::string_view payloadJson) {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        return impl_->QuestSkipLocked(payloadJson);
    }

    std::optional<MockStoredResponse> HttpMockBackend::DailyGetRecoveryChallengeCount() {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        return impl_->DailyGetRecoveryChallengeCountLocked();
    }

    std::optional<MockStoredResponse> HttpMockBackend::DailyRecoveryChallengeCount(std::string_view payloadJson) {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        return impl_->DailyRecoveryChallengeCountLocked(payloadJson);
    }

    std::optional<MockStoredResponse> HttpMockBackend::DailyQuestStageSelect() {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        return impl_->DailyQuestStageSelectLocked();
    }

    std::optional<MockStoredResponse> HttpMockBackend::DailyQuestStageList(std::string_view payloadJson) {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        return impl_->DailyQuestStageListLocked(payloadJson);
    }

    std::optional<MockStoredResponse> HttpMockBackend::DailyQuestStageData(std::string_view payloadJson) {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        return impl_->DailyQuestStageDataLocked(payloadJson);
    }

    std::optional<MockStoredResponse> HttpMockBackend::MusicLearningGetMusicSelect(std::string_view payloadJson) {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        return impl_->MusicLearningGetMusicSelectLocked(payloadJson);
    }

    std::optional<MockStoredResponse> HttpMockBackend::MusicLearningGetResult(std::string_view payloadJson) {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        return impl_->MusicLearningGetResultLocked(payloadJson);
    }

    std::optional<MockStoredResponse> HttpMockBackend::DreamNotifyMemberReleaseConfirm(std::string_view payloadJson) {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        return impl_->DreamNotifyMemberReleaseConfirmLocked(payloadJson);
    }

    std::optional<MockStoredResponse> HttpMockBackend::DreamGetResult(std::string_view payloadJson) {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        return impl_->DreamGetResultLocked(payloadJson);
    }

    std::optional<MockStoredResponse> HttpMockBackend::GradeGetQuestList(std::string_view payloadJson) {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        return impl_->GradeGetQuestListLocked(payloadJson);
    }

    std::optional<MockStoredResponse> HttpMockBackend::GradeSetQuestStart(std::string_view payloadJson) {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        return impl_->GradeSetQuestStartLocked(payloadJson);
    }

    std::optional<MockStoredResponse> HttpMockBackend::GradeSetQuestAction(std::string_view payloadJson) {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        return impl_->GradeSetQuestActionLocked(payloadJson);
    }

    std::optional<MockStoredResponse> HttpMockBackend::GradeSetQuestAddSkill(std::string_view payloadJson) {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        return impl_->GradeSetQuestAddSkillLocked(payloadJson);
    }

    std::optional<MockStoredResponse> HttpMockBackend::GradeGetStageData(std::string_view payloadJson) {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        return impl_->GradeGetStageDataLocked(payloadJson);
    }

    std::optional<MockStoredResponse> HttpMockBackend::GradeGetResult(std::string_view payloadJson) {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        return impl_->GradeGetResultLocked(payloadJson);
    }

    std::optional<MockStoredResponse> HttpMockBackend::GradeSetQuestRetire(std::string_view payloadJson) {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        return impl_->GradeSetQuestRetireLocked(payloadJson);
    }

    std::optional<MockStoredResponse> HttpMockBackend::GrandPrixGetTopInfo(std::string_view payloadJson) {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        return impl_->GrandPrixGetTopInfoLocked(payloadJson);
    }

    std::optional<MockStoredResponse> HttpMockBackend::GrandPrixGetStageSelect(std::string_view payloadJson) {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        return impl_->GrandPrixGetStageSelectLocked(payloadJson);
    }

    std::optional<MockStoredResponse> HttpMockBackend::GrandPrixGetStageList(std::string_view payloadJson) {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        return impl_->GrandPrixGetStageListLocked(payloadJson);
    }

    std::optional<MockStoredResponse> HttpMockBackend::GrandPrixGetStageData(std::string_view payloadJson) {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        return impl_->GrandPrixGetStageDataLocked(payloadJson);
    }

    std::optional<MockStoredResponse> HttpMockBackend::GrandPrixGetRankingList(std::string_view payloadJson) {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        return impl_->GrandPrixGetRankingListLocked(payloadJson);
    }

    std::optional<MockStoredResponse> HttpMockBackend::GrandPrixGetResult(std::string_view payloadJson) {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        return impl_->GrandPrixGetResultLocked(payloadJson);
    }

    std::string HttpMockBackend::ExtractPayloadIntegerFieldAsString(std::string_view payloadJson, std::string_view fieldName) {
        return ExtractJsonIntegerFieldAsString(payloadJson, fieldName);
    }
}
