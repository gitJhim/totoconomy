#include <algorithm>
#include <econ/econ.hpp>
#include <tuple>
namespace econ {
Json choose(const ShoppingView &v) {
    Json evidence = {{"actor", v.actor},
                     {"day", v.day},
                     {"policy", "lowest_perceived_cost-1"},
                     {"product", v.product},
                     {"knowledge", v.observations},
                     {"constraints",
                      {{"cash", v.cash},
                       {"budget", v.budget},
                       {"time_minutes", v.time_minutes},
                       {"quantity", v.quantity},
                       {"time_value", v.time_value}}},
                     {"alternatives", Json::array()},
                     {"exclusions", Json::array()},
                     {"chosen", nullptr},
                     {"reason", "NO_FEASIBLE_OPTION"}};
    std::optional<std::tuple<Amount, std::string>> best;
    for (const auto &shop : v.shop_ids) {
        auto it = std::find_if(v.observations.begin(), v.observations.end(),
                               [&](const auto &o) { return o.shop == shop && o.product == v.product; });
        Json candidate = {{"shop", shop}};
        std::string reason;
        if (it == v.observations.end())
            reason = "UNKNOWN_SHOP";
        else {
            auto time = v.travel.at(shop);
            auto cost = mul(it->price, v.quantity);
            candidate["perceived_price"] = it->price;
            candidate["perceived_stock"] = it->stock;
            candidate["observed_day"] = it->day;
            candidate["travel_minutes"] = time;
            if (it->stock < v.quantity)
                reason = "OUT_OF_STOCK";
            else if (cost > v.cash)
                reason = "INSUFFICIENT_CASH";
            else if (cost > v.budget)
                reason = "BUDGET_LIMIT";
            else if (time > v.time_minutes)
                reason = "TIME_LIMIT";
            else {
                auto score = add(cost, mul(time, v.time_value));
                candidate["score"] = score;
                evidence["alternatives"].push_back(candidate);
                auto key = std::make_tuple(score, shop);
                if (!best || key < *best)
                    best = key;
            }
        }
        if (!reason.empty()) {
            candidate["reason"] = reason;
            evidence["exclusions"].push_back(candidate);
        }
    }
    if (best) {
        evidence["chosen"] = std::get<1>(*best);
        evidence["reason"] = "SELECTED_LOWEST_PERCEIVED_COST";
    }
    return evidence;
}
} // namespace econ
