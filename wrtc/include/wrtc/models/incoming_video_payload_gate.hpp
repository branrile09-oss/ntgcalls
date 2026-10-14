//
// Retinal incoming group-video payload mapping (+retinal.12).
//
// Holds incoming group-video subscriptions that arrive before the group call
// server's video payload-type table has been delivered (GroupConnection::
// createChannels), then releases them with that table; rejects them when the
// delivered table is missing or unusable. It never invents a table. Pure
// logic (standard library and MediaContent only) so it is tested on the host.
//

#pragma once
#include <algorithm>
#include <cctype>
#include <mutex>
#include <string>
#include <utility>
#include <vector>
#include <wrtc/models/media_content.hpp>

namespace wrtc {
    class IncomingVideoPayloadGate {
    public:
        enum class Outcome {
            // The table is not delivered yet: the subscription is held.
            Deferred,
            // The table is delivered and usable: create the channel with `payloadTypes`.
            Ready,
            // The table was delivered but is missing or unusable: refuse.
            Rejected
        };

        struct Subscription {
            Outcome outcome = Outcome::Deferred;
            std::vector<PayloadType> payloadTypes;
        };

        struct Delivery {
            bool usable = false;
            // Held subscriptions released by a usable table, in subscription
            // order, each with the table attached.
            std::vector<std::pair<std::string, MediaContent>> ready;
            // Endpoints of held subscriptions refused by an unusable table.
            std::vector<std::string> rejected;
        };

        // A table is usable when it names at least one real codec (not rtx) with a
        // valid dynamic payload id.
        static bool isUsable(const std::vector<PayloadType>& table) {
            return std::ranges::any_of(table, [](const PayloadType& entry) {
                if (entry.name.empty() || entry.id > 127) {
                    return false;
                }
                std::string lower = entry.name;
                std::ranges::transform(lower, lower.begin(), [](const unsigned char c) {
                    return static_cast<char>(std::tolower(c));
                });
                return lower != "rtx";
            });
        }

        Subscription subscribe(const std::string& endpoint, const MediaContent& content) {
            std::lock_guard lock(mutex);
            Subscription result;
            if (!delivered) {
                deferred.emplace_back(endpoint, content);
                result.outcome = Outcome::Deferred;
            } else if (usable) {
                result.outcome = Outcome::Ready;
                result.payloadTypes = table;
            } else {
                result.outcome = Outcome::Rejected;
            }
            return result;
        }

        Delivery deliver(const std::vector<PayloadType>& newTable) {
            std::lock_guard lock(mutex);
            delivered = true;
            usable = isUsable(newTable);
            table = usable ? newTable : std::vector<PayloadType>{};
            Delivery result;
            result.usable = usable;
            for (auto& [endpoint, content] : deferred) {
                if (usable) {
                    content.payloadTypes = table;
                    result.ready.emplace_back(endpoint, std::move(content));
                } else {
                    result.rejected.push_back(endpoint);
                }
            }
            deferred.clear();
            return result;
        }

        // Is this endpoint held, waiting for the table?
        bool contains(const std::string& endpoint) const {
            std::lock_guard lock(mutex);
            return std::ranges::any_of(deferred, [&](const auto& entry) { return entry.first == endpoint; });
        }

        // Drop a held subscription. True when one was held.
        bool remove(const std::string& endpoint) {
            std::lock_guard lock(mutex);
            const auto it = std::ranges::find_if(deferred, [&](const auto& entry) { return entry.first == endpoint; });
            if (it == deferred.end()) {
                return false;
            }
            deferred.erase(it);
            return true;
        }

    private:
        mutable std::mutex mutex;
        bool delivered = false;
        bool usable = false;
        std::vector<PayloadType> table;
        std::vector<std::pair<std::string, MediaContent>> deferred;
    };
} // wrtc
