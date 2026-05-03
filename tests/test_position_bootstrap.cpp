#include "doctest/doctest.h"

#include "common/position_bootstrap.h"
#include "common/token_inventory.h"
#include "common/types.h"

TEST_SUITE("PositionBootstrap") {

TEST_CASE("bootstrap_positions returns 0 when no condition IDs") {
    lt::TokenInventory inv;
    std::vector<std::string> empty;
    CHECK(lt::bootstrap_positions("0xabc", empty, inv) == 0);
}

TEST_CASE("bootstrap_positions returns 0 when address empty") {
    lt::TokenInventory inv;
    std::vector<std::string> ids = {"0xcondition123"};
    CHECK(lt::bootstrap_positions("", ids, inv) == 0);
}

}  // TEST_SUITE
