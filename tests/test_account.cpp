#include <doctest/doctest.h>

#include <cstdio>
#include <fstream>

#include "common/account.h"

using namespace lt;

TEST_SUITE("Account") {
    TEST_CASE("load valid account file") {
        const char* path = "test_account_temp.json";
        {
            std::ofstream f(path);
            f << R"({
                "name": "Test Account",
                "private_key": "abcdef1234567890abcdef1234567890abcdef1234567890abcdef1234567890",
                "address": "0x1234567890abcdef1234567890abcdef12345678",
                "owner_uuid": "550e8400-e29b-41d4-a716-446655440000",
                "api_key": "my-api-key",
                "api_secret": "bXktYXBpLXNlY3JldA==",
                "api_passphrase": "my-passphrase"
            })";
        }

        auto result = load_account(path);
        CHECK(result.ok());
        CHECK(result.value.name == "Test Account");
        CHECK(result.value.private_key == "abcdef1234567890abcdef1234567890abcdef1234567890abcdef1234567890");
        CHECK(result.value.address == "0x1234567890abcdef1234567890abcdef12345678");
        CHECK(result.value.owner_uuid == "550e8400-e29b-41d4-a716-446655440000");
        CHECK(result.value.api_key == "my-api-key");
        CHECK(result.value.api_secret == "bXktYXBpLXNlY3JldA==");
        CHECK(result.value.api_passphrase == "my-passphrase");

        std::remove(path);
    }

    TEST_CASE("missing required field returns JSON_MISSING_FIELD") {
        const char* path = "test_account_missing.json";

        SUBCASE("missing name") {
            std::ofstream f(path);
            f << R"({
                "private_key": "abc",
                "address": "0x123",
                "owner_uuid": "uuid",
                "api_key": "key",
                "api_secret": "secret",
                "api_passphrase": "pass"
            })";
            f.close();
            auto result = load_account(path);
            CHECK(!result.ok());
            CHECK(result.error == ErrorCode::JSON_MISSING_FIELD);
        }

        SUBCASE("missing private_key") {
            std::ofstream f(path);
            f << R"({
                "name": "Test",
                "address": "0x123",
                "owner_uuid": "uuid",
                "api_key": "key",
                "api_secret": "secret",
                "api_passphrase": "pass"
            })";
            f.close();
            auto result = load_account(path);
            CHECK(!result.ok());
            CHECK(result.error == ErrorCode::JSON_MISSING_FIELD);
        }

        SUBCASE("missing address") {
            std::ofstream f(path);
            f << R"({
                "name": "Test",
                "private_key": "abc",
                "owner_uuid": "uuid",
                "api_key": "key",
                "api_secret": "secret"
            })";
            f.close();
            auto result = load_account(path);
            CHECK(!result.ok());
            CHECK(result.error == ErrorCode::JSON_MISSING_FIELD);
        }

        std::remove(path);
    }

    TEST_CASE("missing file returns CONFIG_FILE_NOT_FOUND") {
        auto result = load_account("nonexistent_account_file.json");
        CHECK(!result.ok());
        CHECK(result.error == ErrorCode::CONFIG_FILE_NOT_FOUND);
    }

    TEST_CASE("malformed JSON returns CONFIG_PARSE_ERROR") {
        const char* path = "test_account_malformed.json";
        {
            std::ofstream f(path);
            f << "{ not valid json }}}";
        }

        auto result = load_account(path);
        CHECK(!result.ok());
        CHECK(result.error == ErrorCode::CONFIG_PARSE_ERROR);

        std::remove(path);
    }

    TEST_CASE("minimal account file loads with empty optional fields") {
        const char* path = "test_account_minimal.json";
        {
            std::ofstream f(path);
            f << R"({
                "name": "Minimal",
                "private_key": "abcdef1234567890abcdef1234567890abcdef1234567890abcdef1234567890",
                "address": "0x1234567890abcdef1234567890abcdef12345678"
            })";
        }

        auto result = load_account(path);
        CHECK(result.ok());
        CHECK(result.value.name == "Minimal");
        CHECK(result.value.private_key == "abcdef1234567890abcdef1234567890abcdef1234567890abcdef1234567890");
        CHECK(result.value.address == "0x1234567890abcdef1234567890abcdef12345678");
        CHECK(result.value.owner_uuid.empty());
        CHECK(result.value.api_key.empty());
        CHECK(result.value.api_secret.empty());
        CHECK(result.value.api_passphrase.empty());

        std::remove(path);
    }

    TEST_CASE("save_account writes and reloads correctly") {
        const char* path = "test_account_save.json";

        AccountInfo info;
        info.name = "SaveTest";
        info.private_key = "aabbccdd";
        info.address = "0xAABB";
        info.api_key = "key123";
        info.api_secret = "secret456";
        info.api_passphrase = "pass789";

        auto err = save_account(path, info);
        CHECK(err == ErrorCode::OK);

        auto result = load_account(path);
        CHECK(result.ok());
        CHECK(result.value.name == "SaveTest");
        CHECK(result.value.private_key == "aabbccdd");
        CHECK(result.value.address == "0xAABB");
        CHECK(result.value.api_key == "key123");
        CHECK(result.value.api_secret == "secret456");
        CHECK(result.value.api_passphrase == "pass789");
        CHECK(result.value.owner_uuid.empty());

        std::remove(path);
    }

    TEST_CASE("save_account omits empty optional fields") {
        const char* path = "test_account_save_minimal.json";

        AccountInfo info;
        info.name = "MinSave";
        info.private_key = "1234";
        info.address = "0x5678";
        // Leave optional fields empty

        auto err = save_account(path, info);
        CHECK(err == ErrorCode::OK);

        auto result = load_account(path);
        CHECK(result.ok());
        CHECK(result.value.name == "MinSave");
        CHECK(result.value.api_key.empty());
        CHECK(result.value.api_secret.empty());
        CHECK(result.value.api_passphrase.empty());
        CHECK(result.value.owner_uuid.empty());

        std::remove(path);
    }

    TEST_CASE("clear_secrets zeroes sensitive fields") {
        AccountInfo info;
        info.name = "Test";
        info.private_key = "abcdef1234567890";
        info.api_secret = "secret_data_here";
        info.address = "0x1234";

        info.clear_secrets();

        CHECK(info.private_key.empty());
        CHECK(info.api_secret.empty());
        // Non-sensitive fields unchanged
        CHECK(info.name == "Test");
        CHECK(info.address == "0x1234");
    }
}
