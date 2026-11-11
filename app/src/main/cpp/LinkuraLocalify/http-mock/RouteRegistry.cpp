#include "RouteRegistry.hpp"

#include "MockPlatform.hpp"
#include "backend/HttpMockBackend.hpp"
#include "offline_api_mock_builtin.hpp"

#include <ctime>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>

namespace LinkuraLocal::HttpMock {
    namespace {
        using RouteTable = std::unordered_map<std::string, RegisteredRouteHandler>;

        static void RegisterStaticJson(RouteTable& routes,
                                       std::string_view path,
                                       std::string_view jsonBody,
                                       std::string_view headersText = OfflineApiMockBuiltIn::DefaultHeadersView) {
            routes.emplace(std::string(path), [jsonBody, headersText](const MockRequestContext&, HttpMockBackend&) -> std::optional<MockResponse> {
                return MockResponse{
                    std::string(jsonBody),
                    std::string(headersText),
                    200,
                    "OK (offline mock)",
                };
            });
        }

        static void RegisterBackend(RouteTable& routes,
                                    std::string_view path,
                                    RegisteredRouteHandler handler) {
            routes.emplace(std::string(path), std::move(handler));
        }

        static std::optional<MockResponse> HandleUserLogin(const MockRequestContext& request,
                                                             HttpMockBackend& backend) {
            // A clean 5.1.0 install sends an empty player_id. In preservation mode
            // that still identifies the single local save; treating it as a new
            // online account sends the client into the LinkLike ID flow and makes
            // an offline cold start impossible.
            // The client's PlayerPrefs player ID belongs to the account whose
            // cache was imported, not to this offline save; it is not echoed
            // into ProfileInfo (the iOS title label is hidden as well).
            (void)request;
            (void)backend;
            const auto& jsonBody = OfflineApiMockBuiltIn::UserLoginJsonView;
            return MockResponse{
                std::string(jsonBody),
                std::string(OfflineApiMockBuiltIn::UserLoginHeadersView),
                200,
                "OK (offline mock)",
            };
        }

        static std::optional<MockResponse> ToBackendResponse(
                std::optional<MockStoredResponse> record) {
            if (!record.has_value()) return std::nullopt;
            if (record->headersText.empty()) {
                record->headersText = std::string(OfflineApiMockBuiltIn::DefaultHeadersView);
            }
            return MockResponse{
                std::move(record->body), std::move(record->headersText),
                record->statusCode, std::move(record->statusDescription),
            };
        }

        static MockResponse JsonResponse(const nlohmann::json& body) {
            return MockResponse{body.dump(), std::string(OfflineApiMockBuiltIn::DefaultHeadersView)};
        }

        static std::string UtcNowIso() {
            const std::time_t now = std::time(nullptr);
            std::tm tm{};
            gmtime_r(&now, &tm);
            char buffer[32];
            std::strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%SZ", &tm);
            return buffer;
        }

        // Archive 応援ランキング (cheer ranking). Periods and reward tiers come
        // from the preserved Master (builtin/cheer_rankings.json); the player
        // ranking itself was server-side and is empty offline. Enum values
        // from 5.0.1 metadata: ArchiveStatus None=0 Notice=1 Open=2 Tally=3
        // End=4; MyRankStatus Ranked=1 OutOfRank=2 NotParticipated=3.
        static std::optional<MockResponse> HandleArchiveGetRankingList(const MockRequestContext& request,
                                                                       HttpMockBackend&) {
            const auto payload = nlohmann::json::parse(request.payloadJson, nullptr, false);
            const int memberId = payload.is_object() ? payload.value("member_id", 0) : 0;
            const nlohmann::json* ranking = nullptr;
            for (const auto& row : OfflineApiMockBuiltIn::CheerRankingsJsonObj()) {
                if (row.value("member_id", 0) == memberId) ranking = &row;  // sorted: keeps the latest
            }
            nlohmann::json body = {
                {"status", 0}, {"cheer_point", 0}, {"my_rank", 0}, {"my_rank_status", 3},
                {"cheer_rankings", nlohmann::json::array()}, {"cheer_rewards", nlohmann::json::array()},
                {"ranking_start_time", "2023-04-15T00:00:00Z"}, {"ranking_end_time", "2023-04-15T00:00:00Z"},
            };
            if (ranking) {
                const std::string now = UtcNowIso();
                const std::string start = ranking->value("start_time", std::string{});
                const std::string end = ranking->value("end_time", std::string{});
                body["status"] = now < start ? 1 : (now <= end ? 2 : 4);
                body["ranking_start_time"] = start;
                body["ranking_end_time"] = end;
                body["cheer_rewards"] = ranking->value("cheer_rewards", nlohmann::json::array());
            }
            return JsonResponse(body);
        }

        // With×STATION archive list: no With×STATION video was preserved, so
        // the list is empty; filters reuse the archive list's.
        static std::optional<MockResponse> HandleArchiveGetWithStationList(const MockRequestContext&,
                                                                           HttpMockBackend&) {
            const auto& archive = OfflineApiMockBuiltIn::ArchiveGetArchiveListJsonObj();
            return JsonResponse({
                {"with_station_list", nlohmann::json::array()},
                {"filterable_characters", archive.value("filterable_characters", nlohmann::json::array())},
                {"sortable_fields", archive.value("sortable_fields", nlohmann::json::array())},
            });
        }

        // Fes×LIVE archive playback polls viewer comments; none were preserved.
        static std::optional<MockResponse> HandleArchiveGetFesTimelineData(const MockRequestContext&,
                                                                           HttpMockBackend&) {
            return JsonResponse({{"timelines", nlohmann::json::array()}, {"polling_interval", 60}});
        }

        // Profile → Fan Lv. info: per-member fan levels from the offline
        // profile; season experience / point stock were server-side.
        static std::optional<MockResponse> HandleProfileGetFanLevelInfo(const MockRequestContext&,
                                                                        HttpMockBackend& backend) {
            auto profileRecord = backend.GetProfileInfoResponse();
            if (!profileRecord.has_value()) return std::nullopt;
            const auto parsed = nlohmann::json::parse(profileRecord->body, nullptr, false);
            const auto profile = parsed.is_object() ? parsed.value("profile_info", nlohmann::json::object())
                                                    : nlohmann::json::object();
            nlohmann::json list = nlohmann::json::array();
            for (const auto& level : profile.value("fan_level_list", nlohmann::json::array())) {
                list.push_back({
                    {"character_id", level.value("character_id", 0)},
                    {"member_fan_level", level.value("member_fan_level", 1)},
                    {"member_fan_experience", 0},
                    {"d_season_fan_level", level.value("d_season_fan_level", 1)},
                    {"d_season_fan_experience", 0},
                    {"earn_progress_list", nlohmann::json::array()},
                });
            }
            return JsonResponse({
                {"fan_level_info_list", std::move(list)},
                {"season_id", 0},
                {"season_fanlevel_point_stock", 0},
                {"season_fanlevel_point_stock_limit", 0},
            });
        }

        static std::optional<MockResponse> HandleHomeGetHome(const MockRequestContext&,
                                                              HttpMockBackend& backend) {
            return ToBackendResponse(backend.GetHomeResponse());
        }

        static std::optional<MockResponse> HandleMissionGetList(const MockRequestContext&,
                                                                 HttpMockBackend& backend) {
            return ToBackendResponse(backend.GetMissionListResponse());
        }

        static std::optional<MockResponse> HandleProfileGetInfo(const MockRequestContext&,
                                                                 HttpMockBackend& backend) {
            return ToBackendResponse(backend.GetProfileInfoResponse());
        }

        static std::optional<MockResponse> HandleItemExchangeGetList(const MockRequestContext& request,
                                                                       HttpMockBackend& backend) {
            return ToBackendResponse(backend.GetItemExchangeListResponse(request.payloadJson));
        }

        static std::optional<MockResponse> HandleItemExchangeSetPurchase(const MockRequestContext& request,
                                                                           HttpMockBackend& backend) {
            return ToBackendResponse(backend.ItemExchangeSetPurchase(request.payloadJson));
        }

        static std::optional<MockResponse> HandleGachaGetSeriesList(const MockRequestContext&,
                                                                     HttpMockBackend& backend) {
            return ToBackendResponse(backend.GetGachaSeriesListResponse());
        }

        static std::optional<MockResponse> HandleGachaSetPurchase(const MockRequestContext& request,
                                                                   HttpMockBackend& backend) {
            return ToBackendResponse(backend.GachaSetPurchase(request.payloadJson));
        }

        static std::optional<MockResponse> HandleGachaGetHistory(const MockRequestContext&,
                                                                  HttpMockBackend& backend) {
            return ToBackendResponse(backend.GetGachaHistoryResponse());
        }

        static std::optional<MockResponse> HandleGachaGetGuaranteePointList(const MockRequestContext& request,
                                                                             HttpMockBackend& backend) {
            return ToBackendResponse(backend.GetGachaGuaranteePointList(request.payloadJson));
        }

        static std::optional<MockResponse> HandleGachaSetGuaranteePointExchange(const MockRequestContext& request,
                                                                                 HttpMockBackend& backend) {
            return ToBackendResponse(backend.GachaSetGuaranteePointExchange(request.payloadJson));
        }

        static std::optional<MockResponse> HandleCollectionMusicList(const MockRequestContext&,
                                                                       HttpMockBackend& backend) {
            return ToBackendResponse(backend.GetCollectionMusicListResponse());
        }

        static std::optional<MockResponse> HandleCollectionStickerList(const MockRequestContext&,
                                                                         HttpMockBackend& backend) {
            return ToBackendResponse(backend.GetCollectionStickerListResponse());
        }

        static std::optional<MockResponse> HandleCollectionGallaryList(const MockRequestContext&,
                                                                         HttpMockBackend& backend) {
            return ToBackendResponse(backend.GetCollectionGallaryListResponse());
        }

        static std::optional<MockResponse> HandleArchiveDetail(const MockRequestContext& request,
                                                               HttpMockBackend& backend) {
            const auto fallbackArchiveId = HookShare::Shareable::currentArchiveId;
            auto record = backend.LookupArchiveDetailFromPayload(request.payloadJson, fallbackArchiveId);
            if (!record.has_value()) {
                const auto attemptedKey = HttpMockBackend::ExtractPayloadStringField(request.payloadJson, "archives_id");
                Log::WarnFmt("[HttpMockRouteRegistry] archive_detail not found path=%.*s archives_id=%s fallback=%s backend=%s",
                             static_cast<int>(request.path.size()),
                             request.path.data(),
                             attemptedKey.c_str(),
                             fallbackArchiveId.c_str(),
                             backend.GetStatusSummary().c_str());
                return std::nullopt;
            }

            if (record->headersText.empty()) {
                record->headersText = std::string(OfflineApiMockBuiltIn::DefaultHeadersView);
            }

            return MockResponse{
                std::move(record->body),
                std::move(record->headersText),
                record->statusCode,
                std::move(record->statusDescription),
            };
        }

        static std::optional<MockResponse> HandleCharacterInfo(const MockRequestContext& request,
                                                               HttpMockBackend& backend) {
            auto record = backend.LookupCharacterInfoFromPayload(request.payloadJson);
            if (!record.has_value()) {
                const auto attemptedKey = HttpMockBackend::ExtractPayloadIntegerFieldAsString(request.payloadJson, "character_id");
                Log::WarnFmt("[HttpMockRouteRegistry] character_info not found path=%.*s character_id=%s backend=%s",
                             static_cast<int>(request.path.size()),
                             request.path.data(),
                             attemptedKey.c_str(),
                             backend.GetStatusSummary().c_str());
                return std::nullopt;
            }

            if (record->headersText.empty()) {
                record->headersText = std::string(OfflineApiMockBuiltIn::DefaultHeadersView);
            }

            return MockResponse{
                std::move(record->body),
                std::move(record->headersText),
                record->statusCode,
                std::move(record->statusDescription),
            };
        }

        static std::optional<MockResponse> HandleCardDetail(const MockRequestContext& request,
                                                             HttpMockBackend& backend) {
            auto record = backend.LookupCardDetailFromPayload(request.payloadJson);
            if (!record.has_value()) {
                const auto attemptedKey = HttpMockBackend::ExtractPayloadStringField(request.payloadJson, "d_card_datas_id");
                Log::WarnFmt("[HttpMockRouteRegistry] card_detail not found path=%.*s d_card_datas_id=%s backend=%s",
                             static_cast<int>(request.path.size()),
                             request.path.data(),
                             attemptedKey.c_str(),
                             backend.GetStatusSummary().c_str());
                return std::nullopt;
            }

            if (record->headersText.empty()) {
                record->headersText = std::string(OfflineApiMockBuiltIn::DefaultHeadersView);
            }

            return MockResponse{
                std::move(record->body),
                std::move(record->headersText),
                record->statusCode,
                std::move(record->statusDescription),
            };
        }

        static std::optional<MockResponse> HandleCardGetList(const MockRequestContext&,
                                                              HttpMockBackend& backend) {
            auto record = backend.GetCardListResponse();
            if (!record.has_value()) {
                Log::WarnFmt("[HttpMockRouteRegistry] card get_list failed backend=%s",
                             backend.GetStatusSummary().c_str());
                return std::nullopt;
            }
            if (record->headersText.empty()) {
                record->headersText = std::string(OfflineApiMockBuiltIn::DefaultHeadersView);
            }
            return MockResponse{
                std::move(record->body), std::move(record->headersText),
                record->statusCode, std::move(record->statusDescription),
            };
        }

        static std::optional<MockResponse> HandleCheckStyleLevelUp(const MockRequestContext& request,
                                                                    HttpMockBackend& backend) {
            auto record = backend.CheckStyleLevelUp(request.payloadJson);
            if (!record.has_value()) {
                return std::nullopt;
            }
            if (record->headersText.empty()) {
                record->headersText = std::string(OfflineApiMockBuiltIn::DefaultHeadersView);
            }
            return MockResponse{
                std::move(record->body), std::move(record->headersText),
                record->statusCode, std::move(record->statusDescription),
            };
        }

        static std::optional<MockResponse> HandleItemDetail(const MockRequestContext& request,
                                                             HttpMockBackend& backend) {
            auto record = backend.LookupItemDetailFromPayload(request.payloadJson);
            if (!record.has_value()) {
                const auto attemptedKey = HttpMockBackend::ExtractPayloadStringField(request.payloadJson, "d_item_datas_id");
                Log::WarnFmt("[HttpMockRouteRegistry] item_detail not found path=%.*s d_item_datas_id=%s backend=%s",
                             static_cast<int>(request.path.size()),
                             request.path.data(),
                             attemptedKey.c_str(),
                             backend.GetStatusSummary().c_str());
                return std::nullopt;
            }

            if (record->headersText.empty()) {
                record->headersText = std::string(OfflineApiMockBuiltIn::DefaultHeadersView);
            }

            return MockResponse{
                std::move(record->body),
                std::move(record->headersText),
                record->statusCode,
                std::move(record->statusDescription),
            };
        }

        static std::optional<MockResponse> HandleItemGetList(const MockRequestContext&,
                                                              HttpMockBackend& backend) {
            auto record = backend.GetItemListResponse();
            if (!record.has_value()) {
                Log::WarnFmt("[HttpMockRouteRegistry] item get_list failed backend=%s",
                             backend.GetStatusSummary().c_str());
                return std::nullopt;
            }
            if (record->headersText.empty()) {
                record->headersText = std::string(OfflineApiMockBuiltIn::DefaultHeadersView);
            }
            return MockResponse{
                std::move(record->body), std::move(record->headersText),
                record->statusCode, std::move(record->statusDescription),
            };
        }

        static std::optional<MockResponse> HandleDeckGetList(const MockRequestContext&,
                                                                HttpMockBackend& backend) {
            auto record = backend.GetDeckListResponse();
            if (!record.has_value()) {
                Log::WarnFmt("[HttpMockRouteRegistry] deck get_list failed backend=%s",
                             backend.GetStatusSummary().c_str());
                return std::nullopt;
            }

            if (record->headersText.empty()) {
                record->headersText = std::string(OfflineApiMockBuiltIn::DefaultHeadersView);
            }

            return MockResponse{
                std::move(record->body),
                std::move(record->headersText),
                record->statusCode,
                std::move(record->statusDescription),
            };
        }

        static std::optional<MockResponse> HandleDeckModifyDeckList(const MockRequestContext& request,
                                                                     HttpMockBackend& backend) {
            auto record = backend.ModifyDeckList(request.payloadJson);
            if (!record.has_value()) {
                Log::WarnFmt("[HttpMockRouteRegistry] deck modify_deck_list failed backend=%s",
                             backend.GetStatusSummary().c_str());
                return std::nullopt;
            }

            if (record->headersText.empty()) {
                record->headersText = std::string(OfflineApiMockBuiltIn::DefaultHeadersView);
            }

            return MockResponse{
                std::move(record->body),
                std::move(record->headersText),
                record->statusCode,
                std::move(record->statusDescription),
            };
        }

        static std::optional<MockResponse> HandleMusicLearningGetMusicSelect(const MockRequestContext& request,
                                                                              HttpMockBackend& backend) {
            auto record = backend.MusicLearningGetMusicSelect(request.payloadJson);
            if (!record.has_value()) return std::nullopt;
            if (record->headersText.empty()) record->headersText = std::string(OfflineApiMockBuiltIn::DefaultHeadersView);
            return MockResponse{std::move(record->body), std::move(record->headersText), record->statusCode, std::move(record->statusDescription)};
        }

        static std::optional<MockResponse> HandleMusicLearningGetResult(const MockRequestContext& request,
                                                                         HttpMockBackend& backend) {
            auto record = backend.MusicLearningGetResult(request.payloadJson);
            if (!record.has_value()) return std::nullopt;
            if (record->headersText.empty()) record->headersText = std::string(OfflineApiMockBuiltIn::DefaultHeadersView);
            return MockResponse{std::move(record->body), std::move(record->headersText), record->statusCode, std::move(record->statusDescription)};
        }

        static std::optional<MockResponse> HandleDreamNotifyMemberReleaseConfirm(const MockRequestContext& request,
                                                                                    HttpMockBackend& backend) {
            auto record = backend.DreamNotifyMemberReleaseConfirm(request.payloadJson);
            if (!record.has_value()) return std::nullopt;
            if (record->headersText.empty()) record->headersText = std::string(OfflineApiMockBuiltIn::DefaultHeadersView);
            return MockResponse{std::move(record->body), std::move(record->headersText), record->statusCode, std::move(record->statusDescription)};
        }

        static std::optional<MockResponse> HandleDreamGetResult(const MockRequestContext& request,
                                                                 HttpMockBackend& backend) {
            auto record = backend.DreamGetResult(request.payloadJson);
            if (!record.has_value()) return std::nullopt;
            if (record->headersText.empty()) record->headersText = std::string(OfflineApiMockBuiltIn::DefaultHeadersView);
            return MockResponse{std::move(record->body), std::move(record->headersText), record->statusCode, std::move(record->statusDescription)};
        }

        static std::optional<MockResponse> HandleGradeGetQuestList(const MockRequestContext& request,
                                                                    HttpMockBackend& backend) {
            auto record = backend.GradeGetQuestList(request.payloadJson);
            if (!record.has_value()) return std::nullopt;
            if (record->headersText.empty()) record->headersText = std::string(OfflineApiMockBuiltIn::DefaultHeadersView);
            return MockResponse{std::move(record->body), std::move(record->headersText), record->statusCode, std::move(record->statusDescription)};
        }

        static std::optional<MockResponse> HandleGradeSetQuestStart(const MockRequestContext& request,
                                                                     HttpMockBackend& backend) {
            auto record = backend.GradeSetQuestStart(request.payloadJson);
            if (!record.has_value()) return std::nullopt;
            if (record->headersText.empty()) record->headersText = std::string(OfflineApiMockBuiltIn::DefaultHeadersView);
            return MockResponse{std::move(record->body), std::move(record->headersText), record->statusCode, std::move(record->statusDescription)};
        }

        static std::optional<MockResponse> HandleGradeSetQuestAction(const MockRequestContext& request,
                                                                      HttpMockBackend& backend) {
            auto record = backend.GradeSetQuestAction(request.payloadJson);
            if (!record.has_value()) return std::nullopt;
            if (record->headersText.empty()) record->headersText = std::string(OfflineApiMockBuiltIn::DefaultHeadersView);
            return MockResponse{std::move(record->body), std::move(record->headersText), record->statusCode, std::move(record->statusDescription)};
        }

        static std::optional<MockResponse> HandleGradeSetQuestAddSkill(const MockRequestContext& request,
                                                                        HttpMockBackend& backend) {
            auto record = backend.GradeSetQuestAddSkill(request.payloadJson);
            if (!record.has_value()) return std::nullopt;
            if (record->headersText.empty()) record->headersText = std::string(OfflineApiMockBuiltIn::DefaultHeadersView);
            return MockResponse{std::move(record->body), std::move(record->headersText), record->statusCode, std::move(record->statusDescription)};
        }

        static std::optional<MockResponse> HandleGradeGetStageData(const MockRequestContext& request,
                                                                    HttpMockBackend& backend) {
            auto record = backend.GradeGetStageData(request.payloadJson);
            if (!record.has_value()) return std::nullopt;
            if (record->headersText.empty()) record->headersText = std::string(OfflineApiMockBuiltIn::DefaultHeadersView);
            return MockResponse{std::move(record->body), std::move(record->headersText), record->statusCode, std::move(record->statusDescription)};
        }

        static std::optional<MockResponse> HandleGradeGetResult(const MockRequestContext& request,
                                                                 HttpMockBackend& backend) {
            auto record = backend.GradeGetResult(request.payloadJson);
            if (!record.has_value()) return std::nullopt;
            if (record->headersText.empty()) record->headersText = std::string(OfflineApiMockBuiltIn::DefaultHeadersView);
            return MockResponse{std::move(record->body), std::move(record->headersText), record->statusCode, std::move(record->statusDescription)};
        }

        static std::optional<MockResponse> HandleGradeSetQuestRetire(const MockRequestContext& request,
                                                                      HttpMockBackend& backend) {
            auto record = backend.GradeSetQuestRetire(request.payloadJson);
            if (!record.has_value()) return std::nullopt;
            if (record->headersText.empty()) record->headersText = std::string(OfflineApiMockBuiltIn::DefaultHeadersView);
            return MockResponse{std::move(record->body), std::move(record->headersText), record->statusCode, std::move(record->statusDescription)};
        }

        static std::optional<MockResponse> HandleDailyQuestStageSelect(const MockRequestContext&,
                                                                        HttpMockBackend& backend) {
            return ToBackendResponse(backend.DailyQuestStageSelect());
        }

        static std::optional<MockResponse> HandleDailyQuestStageList(const MockRequestContext& request,
                                                                     HttpMockBackend& backend) {
            auto record = backend.DailyQuestStageList(request.payloadJson);
            if (!record.has_value()) return std::nullopt;
            if (record->headersText.empty()) record->headersText = std::string(OfflineApiMockBuiltIn::DefaultHeadersView);
            return MockResponse{std::move(record->body), std::move(record->headersText), record->statusCode, std::move(record->statusDescription)};
        }

        static std::optional<MockResponse> HandleDailyQuestStageData(const MockRequestContext& request,
                                                                      HttpMockBackend& backend) {
            auto record = backend.DailyQuestStageData(request.payloadJson);
            if (!record.has_value()) return std::nullopt;
            if (record->headersText.empty()) record->headersText = std::string(OfflineApiMockBuiltIn::DefaultHeadersView);
            return MockResponse{std::move(record->body), std::move(record->headersText), record->statusCode, std::move(record->statusDescription)};
        }

        static std::optional<MockResponse> HandleQuestStageSelect(const MockRequestContext& request,
                                                                  HttpMockBackend& backend) {
            auto record = backend.QuestStageSelect(request.payloadJson);
            if (!record.has_value()) return std::nullopt;
            if (record->headersText.empty()) record->headersText = std::string(OfflineApiMockBuiltIn::DefaultHeadersView);
            return MockResponse{std::move(record->body), std::move(record->headersText), record->statusCode, std::move(record->statusDescription)};
        }

        static std::optional<MockResponse> HandleQuestStageData(const MockRequestContext& request,
                                                                 HttpMockBackend& backend) {
            auto record = backend.QuestStageData(request.payloadJson);
            if (!record.has_value()) return std::nullopt;
            if (record->headersText.empty()) record->headersText = std::string(OfflineApiMockBuiltIn::DefaultHeadersView);
            return MockResponse{std::move(record->body), std::move(record->headersText), record->statusCode, std::move(record->statusDescription)};
        }

        static std::optional<MockResponse> HandleQuestGetLiveSetting(const MockRequestContext& request,
                                                                      HttpMockBackend& backend) {
            auto record = backend.QuestGetLiveSetting(request.payloadJson);
            if (!record.has_value()) return std::nullopt;
            if (record->headersText.empty()) record->headersText = std::string(OfflineApiMockBuiltIn::DefaultHeadersView);
            return MockResponse{std::move(record->body), std::move(record->headersText), record->statusCode, std::move(record->statusDescription)};
        }

        static std::optional<MockResponse> HandleQuestSetLiveSetting(const MockRequestContext& request,
                                                                      HttpMockBackend& backend) {
            auto record = backend.QuestSetLiveSetting(request.payloadJson);
            if (!record.has_value()) return std::nullopt;
            if (record->headersText.empty()) record->headersText = std::string(OfflineApiMockBuiltIn::DefaultHeadersView);
            return MockResponse{std::move(record->body), std::move(record->headersText), record->statusCode, std::move(record->statusDescription)};
        }

        static std::optional<MockResponse> HandleQuestSetStart(const MockRequestContext& request,
                                                                HttpMockBackend& backend) {
            auto record = backend.QuestSetStart(request.payloadJson);
            if (!record.has_value()) return std::nullopt;
            if (record->headersText.empty()) record->headersText = std::string(OfflineApiMockBuiltIn::DefaultHeadersView);
            return MockResponse{std::move(record->body), std::move(record->headersText), record->statusCode, std::move(record->statusDescription)};
        }

        static std::optional<MockResponse> HandleQuestGetLiveInfo(const MockRequestContext& request,
                                                                    HttpMockBackend& backend) {
            auto record = backend.QuestGetLiveInfo(request.payloadJson);
            if (!record.has_value()) return std::nullopt;
            if (record->headersText.empty()) record->headersText = std::string(OfflineApiMockBuiltIn::DefaultHeadersView);
            return MockResponse{std::move(record->body), std::move(record->headersText), record->statusCode, std::move(record->statusDescription)};
        }

        static std::optional<MockResponse> HandleQuestSetFinish(const MockRequestContext& request,
                                                                  HttpMockBackend& backend) {
            auto record = backend.QuestSetFinish(request.payloadJson);
            if (!record.has_value()) return std::nullopt;
            if (record->headersText.empty()) record->headersText = std::string(OfflineApiMockBuiltIn::DefaultHeadersView);
            return MockResponse{std::move(record->body), std::move(record->headersText), record->statusCode, std::move(record->statusDescription)};
        }

        static std::optional<MockResponse> HandleQuestGetResult(const MockRequestContext& request,
                                                                  HttpMockBackend& backend) {
            auto record = backend.QuestGetResult(request.payloadJson);
            if (!record.has_value()) return std::nullopt;
            if (record->headersText.empty()) record->headersText = std::string(OfflineApiMockBuiltIn::DefaultHeadersView);
            return MockResponse{std::move(record->body), std::move(record->headersText), record->statusCode, std::move(record->statusDescription)};
        }

        static std::optional<MockResponse> HandleQuestSkip(const MockRequestContext& request,
                                                            HttpMockBackend& backend) {
            auto record = backend.QuestSkip(request.payloadJson);
            if (!record.has_value()) return std::nullopt;
            if (record->headersText.empty()) record->headersText = std::string(OfflineApiMockBuiltIn::DefaultHeadersView);
            return MockResponse{std::move(record->body), std::move(record->headersText), record->statusCode, std::move(record->statusDescription)};
        }

        static std::optional<MockResponse> HandleRhythmGameHome(const MockRequestContext&,
                                                                  HttpMockBackend& backend) {
            auto record = backend.GetRhythmGameHome();
            if (!record.has_value()) {
                Log::WarnFmt("[HttpMockRouteRegistry] rhythm_game/home failed backend=%s",
                             backend.GetStatusSummary().c_str());
                return std::nullopt;
            }
            if (record->headersText.empty()) {
                record->headersText = std::string(OfflineApiMockBuiltIn::DefaultHeadersView);
            }
            return MockResponse{
                std::move(record->body), std::move(record->headersText),
                record->statusCode, std::move(record->statusDescription),
            };
        }

        static std::optional<MockResponse> HandleRhythmGameSetStart(const MockRequestContext& request,
                                                                      HttpMockBackend& backend) {
            auto record = backend.RhythmGameSetStart(request.payloadJson);
            if (!record.has_value()) {
                return std::nullopt;
            }
            if (record->headersText.empty()) {
                record->headersText = std::string(OfflineApiMockBuiltIn::DefaultHeadersView);
            }
            return MockResponse{
                std::move(record->body), std::move(record->headersText),
                record->statusCode, std::move(record->statusDescription),
            };
        }

        static std::optional<MockResponse> HandleRhythmGameReceiveTotalMission(const MockRequestContext&,
                                                                                 HttpMockBackend& backend) {
            auto record = backend.RhythmGameReceiveTotalMission();
            if (!record.has_value()) {
                Log::WarnFmt("[HttpMockRouteRegistry] rhythm_game/receive_total_mission failed backend=%s",
                             backend.GetStatusSummary().c_str());
                return std::nullopt;
            }
            if (record->headersText.empty()) {
                record->headersText = std::string(OfflineApiMockBuiltIn::DefaultHeadersView);
            }
            return MockResponse{
                std::move(record->body), std::move(record->headersText),
                record->statusCode, std::move(record->statusDescription),
            };
        }

        static std::optional<MockResponse> HandleRhythmGameSetFinish(const MockRequestContext& request,
                                                                      HttpMockBackend& backend) {
            auto record = backend.RhythmGameSetFinish(request.payloadJson);
            if (!record.has_value()) {
                Log::WarnFmt("[HttpMockRouteRegistry] rhythm_game_live/set_finish failed backend=%s",
                             backend.GetStatusSummary().c_str());
                return std::nullopt;
            }
            if (record->headersText.empty()) {
                record->headersText = std::string(OfflineApiMockBuiltIn::DefaultHeadersView);
            }
            return MockResponse{
                std::move(record->body), std::move(record->headersText),
                record->statusCode, std::move(record->statusDescription),
            };
        }

        static std::optional<MockResponse> HandleRhythmGameDeckModify(const MockRequestContext& request,
                                                                       HttpMockBackend& backend) {
            auto record = backend.ModifyRhythmGameDeckList(request.payloadJson);
            if (!record.has_value()) {
                Log::WarnFmt("[HttpMockRouteRegistry] rhythm_game_deck/modify failed backend=%s",
                             backend.GetStatusSummary().c_str());
                return std::nullopt;
            }
            if (record->headersText.empty()) {
                record->headersText = std::string(OfflineApiMockBuiltIn::DefaultHeadersView);
            }
            return MockResponse{
                std::move(record->body), std::move(record->headersText),
                record->statusCode, std::move(record->statusDescription),
            };
        }

        static std::optional<MockResponse> HandleGrandPrixGetTopInfo(const MockRequestContext& request,
                                                                       HttpMockBackend& backend) {
            auto record = backend.GrandPrixGetTopInfo(request.payloadJson);
            if (!record.has_value()) return std::nullopt;
            if (record->headersText.empty()) record->headersText = std::string(OfflineApiMockBuiltIn::DefaultHeadersView);
            return MockResponse{std::move(record->body), std::move(record->headersText), record->statusCode, std::move(record->statusDescription)};
        }

        static std::optional<MockResponse> HandleGrandPrixGetStageSelect(const MockRequestContext& request,
                                                                           HttpMockBackend& backend) {
            auto record = backend.GrandPrixGetStageSelect(request.payloadJson);
            if (!record.has_value()) return std::nullopt;
            if (record->headersText.empty()) record->headersText = std::string(OfflineApiMockBuiltIn::DefaultHeadersView);
            return MockResponse{std::move(record->body), std::move(record->headersText), record->statusCode, std::move(record->statusDescription)};
        }

        static std::optional<MockResponse> HandleGrandPrixGetStageList(const MockRequestContext& request,
                                                                         HttpMockBackend& backend) {
            auto record = backend.GrandPrixGetStageList(request.payloadJson);
            if (!record.has_value()) return std::nullopt;
            if (record->headersText.empty()) record->headersText = std::string(OfflineApiMockBuiltIn::DefaultHeadersView);
            return MockResponse{std::move(record->body), std::move(record->headersText), record->statusCode, std::move(record->statusDescription)};
        }

        static std::optional<MockResponse> HandleGrandPrixGetStageData(const MockRequestContext& request,
                                                                         HttpMockBackend& backend) {
            auto record = backend.GrandPrixGetStageData(request.payloadJson);
            if (!record.has_value()) return std::nullopt;
            if (record->headersText.empty()) record->headersText = std::string(OfflineApiMockBuiltIn::DefaultHeadersView);
            return MockResponse{std::move(record->body), std::move(record->headersText), record->statusCode, std::move(record->statusDescription)};
        }

        static std::optional<MockResponse> HandleGrandPrixGetRankingList(const MockRequestContext& request,
                                                                           HttpMockBackend& backend) {
            auto record = backend.GrandPrixGetRankingList(request.payloadJson);
            if (!record.has_value()) return std::nullopt;
            if (record->headersText.empty()) record->headersText = std::string(OfflineApiMockBuiltIn::DefaultHeadersView);
            return MockResponse{std::move(record->body), std::move(record->headersText), record->statusCode, std::move(record->statusDescription)};
        }

        static std::optional<MockResponse> HandleGrandPrixGetResult(const MockRequestContext& request,
                                                                       HttpMockBackend& backend) {
            auto record = backend.GrandPrixGetResult(request.payloadJson);
            if (!record.has_value()) return std::nullopt;
            if (record->headersText.empty()) record->headersText = std::string(OfflineApiMockBuiltIn::DefaultHeadersView);
            return MockResponse{std::move(record->body), std::move(record->headersText), record->statusCode, std::move(record->statusDescription)};
        }

        static RouteTable BuildRoutes() {
            RouteTable routes;

            RegisterStaticJson(routes, "/v1/account/connect", OfflineApiMockBuiltIn::AccountConnectJsonView);
            RegisterStaticJson(routes, "/v1/user/push/devices", "null");
            RegisterBackend(routes, "/v1/user/login", HandleUserLogin);
            RegisterStaticJson(routes, "/v1/home/get_login_bonus", OfflineApiMockBuiltIn::HomeGetLoginBonusJsonView);
            RegisterBackend(routes, "/v1/home/get_home", HandleHomeGetHome);
            // These launcher screens must have a valid first response before
            // HomeMenu exposes them in offline mode. Empty player collections
            // represent a clean local save; the shop catalog comes from Shops.yaml.
            RegisterBackend(routes, "/v1/mission/get_list", HandleMissionGetList);
            RegisterBackend(routes, "/v1/mission/receive_common_mission_reward",
                            [](const MockRequestContext&, HttpMockBackend& backend) {
                                return ToBackendResponse(backend.ReceiveCommonMissionReward());
                            });
            RegisterStaticJson(routes, "/v1/present_box/get_list",
                               R"({"total_count":0,"page":1,"items":[]})");
            RegisterStaticJson(routes, "/v1/present_box/get_history",
                               R"({"total_count":0,"page":1,"items":[]})");
            RegisterStaticJson(routes, "/v1/shop/get_list",
                               OfflineApiMockBuiltIn::ShopGetListJsonView);
            RegisterBackend(routes, "/v1/item_exchange/get_list", HandleItemExchangeGetList);
            RegisterBackend(routes, "/v1/item_exchange/get_list_new", HandleItemExchangeGetList);
            RegisterBackend(routes, "/v1/item_exchange/set_purchase", HandleItemExchangeSetPurchase);
            RegisterStaticJson(routes, "/v1/circle/get_circle_top_info",
                               R"({"circle_info":null,"circle_member_list":[],"circle_ranking_info":null,"item_request_result":null,"emoji_list":[],"membership_list":[]})");
            // ID search: there are no other players offline. ResultCode enum
            // (5.0.1 metadata): Success=0, NotExists=1.
            RegisterStaticJson(routes, "/v1/follow/search_id", R"({"result_code":1})");
            RegisterStaticJson(routes, "/v1/follow/get_list",
                               R"({"follow_info_list":[],"max_follow_count":0})");
            RegisterStaticJson(routes, "/v1/follow/get_follower_list",
                               R"({"follower_info_list":[],"max_follower_count":0})");
            RegisterStaticJson(routes, "/v1/follow/recommend_list",
                               R"({"recommend_info_list":[],"max_recommend_count":0})");
            RegisterStaticJson(routes, "/v1/friend/get_list",
                               R"({"friend_info_list":[],"max_friend_count":0})");
            RegisterStaticJson(routes, "/v1/announce/list",
                               R"({"announce_list":[]})");
            RegisterBackend(routes, "/v1/gacha/get_series_list", HandleGachaGetSeriesList);
            RegisterBackend(routes, "/v1/gacha/set_purchase", HandleGachaSetPurchase);
            RegisterBackend(routes, "/v1/gacha/get_history", HandleGachaGetHistory);
            RegisterBackend(routes, "/v1/gacha/get_guarantee_point_list", HandleGachaGetGuaranteePointList);
            RegisterBackend(routes, "/v1/gacha/set_guarantee_point_exchange", HandleGachaSetGuaranteePointExchange);
            RegisterBackend(routes, "/v1/home/get_custom_setting", [](const MockRequestContext&, HttpMockBackend& backend) {
                return ToBackendResponse(backend.GetCustomSettingResponse());
            });
            RegisterStaticJson(routes, "/v1/webview/school_idol_connect_post/get_theme_list", OfflineApiMockBuiltIn::WebviewSchoolIdolConnectPostGetThemeListJsonView);
            RegisterStaticJson(routes, "/v1/follow/live_chat_group_list", OfflineApiMockBuiltIn::FollowLiveChatGroupListJsonView);
            RegisterStaticJson(routes, "/v1/archive/get_home", OfflineApiMockBuiltIn::ArchiveGetHomeJsonView);
            RegisterStaticJson(routes, "/v1/archive/get_archive_list", OfflineApiMockBuiltIn::ArchiveGetArchiveListJsonView);
            RegisterStaticJson(routes, "/v1/out_quest_live/get_quest_top", OfflineApiMockBuiltIn::OutQuestLiveGetQuestTopJsonView);
            RegisterBackend(routes, "/v1/out_quest_live/daily/get_stage_select", HandleDailyQuestStageSelect);
            RegisterBackend(routes, "/v1/out_quest_live/daily/get_recovery_challenge_count",
                            [](const MockRequestContext&, HttpMockBackend& backend) {
                                return ToBackendResponse(backend.DailyGetRecoveryChallengeCount());
                            });
            RegisterBackend(routes, "/v1/out_quest_live/daily/recovery_challenge_count",
                            [](const MockRequestContext& request, HttpMockBackend& backend) {
                                return ToBackendResponse(backend.DailyRecoveryChallengeCount(request.payloadJson));
                            });
            RegisterBackend(routes, "/v1/out_quest_live/daily/get_stage_list", HandleDailyQuestStageList);
            RegisterBackend(routes, "/v1/out_quest_live/daily/get_stage_data", HandleDailyQuestStageData);
            RegisterBackend(routes, "/v1/out_quest_live/music_learning/get_music_select", HandleMusicLearningGetMusicSelect);
            RegisterBackend(routes, "/v1/out_quest_live/music_learning/get_result", HandleMusicLearningGetResult);
            RegisterBackend(routes, "/v1/out_quest_live/dream/notify_member_release_confirm", HandleDreamNotifyMemberReleaseConfirm);
            RegisterBackend(routes, "/v1/out_quest_live/dream/get_result", HandleDreamGetResult);
            RegisterBackend(routes, "/v1/out_quest_live/grade/get_quest_list", HandleGradeGetQuestList);
            RegisterBackend(routes, "/v1/out_quest_live/grade/set_quest_start", HandleGradeSetQuestStart);
            RegisterBackend(routes, "/v1/out_quest_live/grade/set_quest_action", HandleGradeSetQuestAction);
            RegisterBackend(routes, "/v1/out_quest_live/grade/set_quest_add_skill", HandleGradeSetQuestAddSkill);
            RegisterBackend(routes, "/v1/out_quest_live/grade/get_stage_data", HandleGradeGetStageData);
            RegisterBackend(routes, "/v1/out_quest_live/grade/get_result", HandleGradeGetResult);
            RegisterBackend(routes, "/v1/out_quest_live/grade/set_quest_retire", HandleGradeSetQuestRetire);
            RegisterBackend(routes, "/v1/out_quest_live/grand_prix/get_top_info", HandleGrandPrixGetTopInfo);
            RegisterBackend(routes, "/v1/out_quest_live/grand_prix/get_stage_select", HandleGrandPrixGetStageSelect);
            RegisterBackend(routes, "/v1/out_quest_live/grand_prix/get_stage_list", HandleGrandPrixGetStageList);
            RegisterBackend(routes, "/v1/out_quest_live/grand_prix/get_stage_data", HandleGrandPrixGetStageData);
            RegisterBackend(routes, "/v1/out_quest_live/grand_prix/get_ranking_list", HandleGrandPrixGetRankingList);
            RegisterBackend(routes, "/v1/out_quest_live/grand_prix/get_result", HandleGrandPrixGetResult);
            RegisterBackend(routes, "/v1/out_quest_live/standard/get_stage_select", HandleQuestStageSelect);
            RegisterBackend(routes, "/v1/out_quest_live/standard/get_stage_data", HandleQuestStageData);
            RegisterBackend(routes, "/v1/out_quest_live/get_live_setting", HandleQuestGetLiveSetting);
            RegisterBackend(routes, "/v1/out_quest_live/set_live_setting", HandleQuestSetLiveSetting);
            RegisterBackend(routes, "/v1/in_quest_live/set_start", HandleQuestSetStart);
            RegisterBackend(routes, "/v1/in_quest_live/get_live_info", HandleQuestGetLiveInfo);
            RegisterBackend(routes, "/v1/in_quest_live/set_finish", HandleQuestSetFinish);
            RegisterBackend(routes, "/v1/out_quest_live/get_result", HandleQuestGetResult);
            RegisterBackend(routes, "/v1/in_quest_live/skip_quest", HandleQuestSkip);
            RegisterBackend(routes, "/v1/user/card/get_list", HandleCardGetList);
            RegisterBackend(routes, "/v1/user/card/get_detail", HandleCardDetail);
            RegisterBackend(routes, "/v1/user/card/check_style_level_up", HandleCheckStyleLevelUp);
            // Card growth: costs from the Master, paid from the player inventory.
            for (const char* action : {"style_level_up", "check_limit_break", "limit_break",
                                       "check_evolution", "evolution", "check_skill_level_up"}) {
                const std::string path = std::string("/v1/user/card/") + action;
                const std::string name(action);
                RegisterBackend(routes, path, [name](const MockRequestContext& request, HttpMockBackend& backend) {
                    return ToBackendResponse(backend.CardGrowth(name, request.payloadJson));
                });
            }
            RegisterBackend(routes, "/v1/user/items/get_list", HandleItemGetList);
            RegisterBackend(routes, "/v1/user/item/get_detail", HandleItemDetail);
            RegisterBackend(routes, "/v1/collection/get_character_info", HandleCharacterInfo);
            RegisterStaticJson(routes, "/v1/activity_record/get_top", OfflineApiMockBuiltIn::ActivityRecordGetTopJsonView);
            RegisterStaticJson(routes, "/v1/activity_record/play_adv_data", OfflineApiMockBuiltIn::ActivityRecordPlayAdvDataJsonView);

            RegisterBackend(routes, "/v1/archive/get_with_archive_data", HandleArchiveDetail);
            RegisterBackend(routes, "/v1/archive/get_fes_archive_data", HandleArchiveDetail);
            RegisterBackend(routes, "/v1/archive/get_ranking_list", HandleArchiveGetRankingList);
            RegisterBackend(routes, "/v1/archive/get_with_station_list", HandleArchiveGetWithStationList);
            RegisterBackend(routes, "/v1/archive/get_fes_timeline_data", HandleArchiveGetFesTimelineData);
            RegisterBackend(routes, "/v1/profile/get_fan_level_info", HandleProfileGetFanLevelInfo);

            // Chapter RANK (season pass). ChapterHomeResponse per 5.1 metadata:
            // chapter_id, rank_info{current_rank,current_point,is_max_rank},
            // category_list[{category_id,current_point,max_point,is_capped}].
            // Museum state: the final chapter (Chapters.yaml 10003) completed at
            // the top rank (ChapterRanks.yaml 6 = Rank SS); categories filled to
            // their ChapterMissionCategorys.yaml LimitPoint (2 has no limit).
            RegisterStaticJson(routes, "/v1/chapter/home",
                               R"({"chapter_id":10003,)"
                               R"("rank_info":{"current_rank":6,"current_point":10000,"is_max_rank":true},)"
                               R"("category_list":[)"
                               R"({"category_id":1,"current_point":3000,"max_point":3000,"is_capped":true},)"
                               R"({"category_id":2,"current_point":0,"max_point":0,"is_capped":false},)"
                               R"({"category_id":3,"current_point":4000,"max_point":4000,"is_capped":true},)"
                               R"({"category_id":4,"current_point":3000,"max_point":3000,"is_capped":true}]})");
            RegisterBackend(routes, "/v1/profile/get_info", HandleProfileGetInfo);
            // Profile "my designs" persist in the backend (see SetMyDesignLocked).
            const auto designRoute = [&routes](const char* path, auto call) {
                RegisterBackend(routes, path, [call](const MockRequestContext& request, HttpMockBackend& backend) {
                    return ToBackendResponse(call(backend, request.payloadJson));
                });
            };
            designRoute("/v1/profile/get_my_design_card_list",
                        [](HttpMockBackend& b, std::string_view) { return b.GetMyDesignList(false); });
            designRoute("/v1/profile/get_my_design_icon_list",
                        [](HttpMockBackend& b, std::string_view) { return b.GetMyDesignList(true); });
            designRoute("/v1/profile/set_my_design_card",
                        [](HttpMockBackend& b, std::string_view p) { return b.SetMyDesign(false, p); });
            designRoute("/v1/profile/set_my_design_icon",
                        [](HttpMockBackend& b, std::string_view p) { return b.SetMyDesign(true, p); });
            designRoute("/v1/out_quest_live/get_stamina_recovery_info",
                        [](HttpMockBackend& b, std::string_view) { return b.GetStaminaRecoveryInfo(); });
            designRoute("/v1/out_quest_live/recovery_stamina",
                        [](HttpMockBackend& b, std::string_view p) { return b.RecoverStamina(p); });
            designRoute("/v1/follow/set_friend_card",
                        [](HttpMockBackend& b, std::string_view p) { return b.SetFriendCard(p); });
            designRoute("/v1/profile/set_name",
                        [](HttpMockBackend& b, std::string_view p) { return b.SetProfileName(p); });
            designRoute("/v1/profile/set_comment",
                        [](HttpMockBackend& b, std::string_view p) { return b.SetProfileComment(p); });
            designRoute("/v1/profile/set_my_design_name",
                        [](HttpMockBackend& b, std::string_view p) { return b.SetMyDesignName(p); });
            designRoute("/v1/profile/set_profile_card",
                        [](HttpMockBackend& b, std::string_view p) { return b.SetProfileDesign(false, p); });
            designRoute("/v1/profile/set_profile_icon",
                        [](HttpMockBackend& b, std::string_view p) { return b.SetProfileDesign(true, p); });
            RegisterStaticJson(routes, "/v1/profile/get_mute_list", OfflineApiMockBuiltIn::ProfileGetMuteListJsonView);

            RegisterBackend(routes, "/v1/user/deck/get_list", HandleDeckGetList);
            RegisterBackend(routes, "/v1/user/deck/modify_deck_list", HandleDeckModifyDeckList);
            RegisterStaticJson(routes, "/v1/user/deck/notify_auto_deck", "null");
            RegisterBackend(routes, "/v1/collection/get_music_list", HandleCollectionMusicList);
            RegisterBackend(routes, "/v1/collection/get_sticker_list", HandleCollectionStickerList);
            RegisterBackend(routes, "/v1/collection/get_gallary_list", HandleCollectionGallaryList);

            RegisterStaticJson(routes, "/v1/step_up_beginner_mission/get_list", OfflineApiMockBuiltIn::StepUpBeginnerMissionGetListJsonView);

            RegisterBackend(routes, "/v1/rhythm_game/home", HandleRhythmGameHome);
            RegisterBackend(routes, "/v1/rhythm_game_live/set_start", HandleRhythmGameSetStart);
            RegisterBackend(routes, "/v1/rhythm_game_live/set_finish", HandleRhythmGameSetFinish);
            RegisterBackend(routes, "/v1/rhythm_game_deck/modify_deck_list", HandleRhythmGameDeckModify);
            RegisterBackend(routes, "/v1/rhythm_game/receive_total_mission", HandleRhythmGameReceiveTotalMission);
            RegisterStaticJson(routes, "/v1/rhythm_game/receive_class_mission", "null");
            RegisterStaticJson(routes, "/v1/rhythm_game_live/set_retire", "null");
            // Giving up a stage live: nothing to settle. What the session
            // already consumed at set_start stays consumed.
            RegisterStaticJson(routes, "/v1/in_quest_live/set_retire", "null");
            // Display notification only.
            RegisterStaticJson(routes, "/v1/activity_record/notify_month_displayed", "null");
            RegisterStaticJson(routes, "/v1/card/set_display_data", R"({"result":true})");

            return routes;
        }

        static const RouteTable& GetRoutes() {
            static const RouteTable routes = BuildRoutes();
            return routes;
        }
    }

    std::optional<MockResponse> ResolveRegisteredRoute(const MockRequestContext& request) {
        const auto& routes = GetRoutes();
        const auto it = routes.find(std::string(request.path));
        if (it == routes.end()) {
            return std::nullopt;
        }
        auto response = it->second(request, HttpMockBackend::Get());
        if (response) {
            response->headersText = HttpMockBackend::Get().ApplyLiveWalletHeaders(
                    std::move(response->headersText));
        }
        return response;
    }
}
