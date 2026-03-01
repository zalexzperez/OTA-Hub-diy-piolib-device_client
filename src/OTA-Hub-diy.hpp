#pragma once
#define HTTP_MAX_HEADERS 30 // GitHub sends ~28 headers back!
// SET_LOOP_TASK_STACK_SIZE(16 * 1024); // 16KB, GitHub responses are heavy

// libs
#include <Hard-Stuff-Http.hpp>
#include <Update.h>
#include <ArduinoJson.h>
#include <TimeLib.h>
#ifdef HW_MODEL_IPC
#include "ui/variants/ipc/vars.h"
#elif HW_MODEL_AIRV
#include "ui/variants/airv/vars.h"
#endif

#include <ota-github-defaults.h>
#include <ota-github-cacerts.h>

#ifndef OTA_VERSION
#define OTA_VERSION "local_development"
#endif

namespace OTA
{
#pragma region WorkingVariabls
    HardStuffHttpClient *http_ota;
    Client *underlying_client;
    String asset_id;
#pragma endregion

#pragma region UsefulStructs
    /**
     * @brief What is the condition of the update relative to the current installed firmware?
     */
    enum UpdateCondition
    {
        NO_UPDATE,       // The remote release tag matches the local OTA_VERSION (or no compatible firmware found)
        UPDATE_AVAILABLE // The remote release tag differs from the local OTA_VERSION — a legitimate update
    };

    /**
     * @brief What is the condition of the install?
     */
    enum InstallCondition
    {
        FAILED_TO_DOWNLOAD, // For whatever reason, we failed to download the update
        REDIRECT_REQUIRED,  // We'll need to follow a redirect to download the firmware.bin file. Don't forget to set to the new ca_cert!
        SUCCESS             // Success! You'll likely only see me if you've asked the installer to not restart the ESP32.
    };

    /**
     * @brief Everything necessary related to a firmware release
     */
    struct UpdateObject
    {
        UpdateCondition condition;
        String name;
        time_t published_at;
        String tag_name;
        String firmware_asset_id;
        String firmware_asset_endpoint;
        String redirect_server;

        void print(Stream *print_stream = &Serial)
        {
            const char *condition_strings[] = {"NO_UPDATE", "UPDATE_AVAILABLE"};

            print_stream->println("------------------------");
            print_stream->println("Condition: " + String(condition_strings[condition]));
            print_stream->println("name: " + name);
            print_stream->println("tag_name: " + String(tag_name));
            print_stream->println("published_at: " + http_ota->formatTimeISO8601(published_at));
            print_stream->println("firmware_asset_id: " + String(firmware_asset_id));
            print_stream->println("firmware_asset_endpoint: " + String(firmware_asset_endpoint));
            print_stream->println("------------------------");
        }
    };
#pragma endregion

#pragma region HelperFunctions
    String getMacAddress()
    {
        uint8_t baseMac[6];
        esp_read_mac(baseMac, ESP_MAC_WIFI_STA);
        char baseMacChr[18] = {0};
        sprintf(baseMacChr, "%02X:%02X:%02X:%02X:%02X:%02X", baseMac[0], baseMac[1], baseMac[2], baseMac[3], baseMac[4], baseMac[5]);
        return String(baseMacChr);
    }
#pragma endregion

#pragma region SupportFunctions
    void printFirmwareDetails(Stream *print_stream = &Serial)
    {
        print_stream->println("------------------------");
        print_stream->println("Device MAC: " + getMacAddress());
        print_stream->println("Firmware Version: " + (String)OTA_VERSION);
        print_stream->println("Firmware Compilation Date: " + (String)__DATE__ + ", " + (String)__TIME__);
        print_stream->println("------------------------");
    }

    bool isReleaseNewer(const String &remote, const String &local)
    {
        int r_major = 0, r_minor = 0, r_patch = 0;
        int l_major = 0, l_minor = 0, l_patch = 0;

        sscanf(remote.c_str(), "%d.%d.%d", &r_major, &r_minor, &r_patch);
        sscanf(local.c_str(), "%d.%d.%d", &l_major, &l_minor, &l_patch);

        if (r_major != l_major)
            return r_major > l_major;
        if (r_minor != l_minor)
            return r_minor > l_minor;
        return r_patch > l_patch;
    }

    void deinit()
    {
        if (http_ota != nullptr)
        {
            http_ota->stop();
            delete http_ota;
            http_ota = nullptr;
        }
    }

    void reinit(Client &set_underlying_client, const char *server, uint16_t port)
    {
        deinit();
        Serial.print("Server: ");
        Serial.println(server);
        underlying_client = &set_underlying_client;
        http_ota = new HardStuffHttpClient(set_underlying_client, server, port);
    }

    void init(Client &set_underlying_client)
    {
        printFirmwareDetails();
        reinit(set_underlying_client, OTAGH_SERVER, OTAGH_PORT);
    }

#pragma endregion

#pragma region CoreFunctions

    // Structure to track progress
    struct UpdateProgress
    {
        size_t totalBytes;
        size_t currentBytes;
        float percentage;
    };

    static UpdateProgress currentProgress = {0, 0, 0.0f};

    // Helper function to retrieve progress
    UpdateProgress getUpdateProgress()
    {
        return currentProgress;
    }

    // Progress callback for Update class
    void onProgress(size_t progress, size_t total)
    {
        currentProgress.currentBytes = progress;
        currentProgress.totalBytes = total;
        currentProgress.percentage = (float)progress * 100 / total;

        Serial.printf("Progress: %.2f%%\n", (float)currentProgress.percentage);
        set_var_update_percentage((int)currentProgress.percentage);
    }

    /**
     * @brief Check GitHub to see if an update is available
     *
     * @return UpdateObject that bundles all the info we'll need.
     */
    UpdateObject isUpdateAvailable()
    {
        UpdateObject return_object;
        return_object.condition = NO_UPDATE;

        // Get the response from the server
        HardStuffHttpRequest request;
        request.addHeader("Accept", "application/vnd.github+json");
#ifdef OTAGH_BEARER
        request.addHeader("Authorization", "Bearer " + String(OTAGH_BEARER)); // Used only in private repos. See the docs.
#endif

        HardStuffHttpResponse response = http_ota->getFromHTTPServer(String(OTAGH_CHECK_PATH), &request);

        if (!response.success())
        {
            Serial.printf("Failed to get update info from GitHub. Check your OTAGH_... #defines.\n");
            return return_object;
        }

        // Compile into a JSON doc
        JsonDocument release_response;
        deserializeJson(release_response, response.body);
        if (!release_response["name"].is<const char *>() || !release_response["published_at"].is<const char *>() || !release_response["assets"].is<JsonArray>() ||
            !release_response["tag_name"].is<const char *>())
        {
            Serial.println("The latest release contains no assets and/or metadata. We can't continue...");
            return return_object;
        }

        return_object.name = release_response["name"].as<String>();
        return_object.published_at = http_ota->formatTimeFromISO8601(release_response["published_at"].as<String>());
        return_object.tag_name = release_response["tag_name"].as<String>();

        // Strip leading 'v' from GitHub tag before comparing (e.g. "v1.1.1" -> "1.1.1")
        String remote_version = return_object.tag_name;
        if (remote_version.startsWith("v") || remote_version.startsWith("V"))
        {
            remote_version = remote_version.substring(1);
        }

        bool update_is_available = isReleaseNewer(remote_version, OTA_VERSION);
        Serial.printf("Remote version: %s | Local version: %s | Update: %s\n", remote_version.c_str(), OTA_VERSION, update_is_available ? "available" : "not available");

        if (!update_is_available)
        {
            return return_object; // condition is already NO_UPDATE
        }

        // Build expected firmware name based on current hardware
        String expected_firmware_name = "firmware-" + String(HW_MODEL) + "-rev" + String(HW_REVISION) + ".bin";
        Serial.printf("Looking for firmware asset: %s\n", expected_firmware_name.c_str());

        JsonArray asset_array = release_response["assets"].as<JsonArray>();
        for (JsonVariant v : asset_array)
        {
            String asset_name = v["name"].as<String>();
            Serial.printf("Found asset: %s\n", asset_name.c_str());

            if (asset_name.compareTo(expected_firmware_name) == 0)
            {
                return_object.firmware_asset_id = v["id"].as<String>();
                return_object.condition = update_is_available ? UPDATE_AVAILABLE : NO_UPDATE;
                return_object.firmware_asset_endpoint = OTAGH_BIN_PATH + return_object.firmware_asset_id;
                Serial.println("Compatible firmware found!");
                return return_object;
            }
        }

        Serial.printf("No compatible firmware found for %s rev%d\n", HW_MODEL, HW_REVISION);
        return return_object;
    }

    /**
     * @brief Download and perform an update based on the details provided in the UpdateObject file.
     *
     * @param details You'll get this from `isUpdateAvailable`
     * @param restart You can stop the updater from automatically restarting the board, say if you need to wind things down a bit...
     * @return InstallCondition Was it a success?
     */
    InstallCondition performUpdate(UpdateObject *details, bool restart = true)
    {
        Serial.println("Fetching update from: " + (details->redirect_server.isEmpty() ? String(OTAGH_SERVER) : details->redirect_server) + details->firmware_asset_endpoint);

        HardStuffHttpRequest request;
        request.addHeader("Accept", "application/octet-stream");
#ifdef OTAGH_BEARER
        request.addHeader("Authorization", "Bearer " + String(OTAGH_BEARER));
#endif

        // On GitHub this will likely return a 302 with a "location" header:
        HardStuffHttpResponse response = http_ota->getFromHTTPServer(details->firmware_asset_endpoint, &request, true);

        if (response.status_code == 302)
        {
            String URL = "";
            for (int i = 0; i < response.header_count; i++)
            {
                if (response.headers[i].key.compareTo("Location") == 0)
                {
                    URL = response.headers[i].value;
                    break;
                }
            }
            if (URL.isEmpty())
            {
                Serial.println("Redirection URL extraction error...");
                Serial.println("We can't continue.");
                return FAILED_TO_DOWNLOAD;
            }
            URL.replace("https://", "");
            URL.replace("http://", "");
            int slash_index = URL.indexOf("/");

            http_ota->stop();
            delay(1000);
            details->redirect_server = URL.substring(0, slash_index);
            details->firmware_asset_endpoint = URL.substring(slash_index);
            return REDIRECT_REQUIRED;
        }

        if (response.status_code >= 200 && response.status_code < 300)
        {
            Serial.println("firmware.bin found. Checking validity.");
            int contentLength = 0;
            bool isValidContentType = false;

            for (int i_header = 0; i_header < response.header_count; i_header++)
            {
                if (response.headers[i_header].key.compareTo("Content-Length") == 0)
                {
                    contentLength = response.headers[i_header].value.toInt();
                }
                if (response.headers[i_header].key.compareTo("Content-Type") == 0)
                {
                    String contentType = response.headers[i_header].value;
                    isValidContentType = contentType == "application/octet-stream" || contentType == "application/macbinary";
                }
            }

            if (contentLength && isValidContentType)
            {
                Serial.println("firmware.bin is good. Beginning the OTA update, this may take a while...");

                currentProgress.totalBytes = contentLength;
                currentProgress.currentBytes = 0;
                currentProgress.percentage = 0;

                Update.onProgress(onProgress);

                if (Update.begin(contentLength))
                {
                    Update.writeStream(*http_ota);
                    if (Update.end())
                    {
                        if (Update.isFinished())
                        {
                            Serial.println("OTA done!");
                            if (restart)
                            {
                                Serial.println("Reboot...");
                                ESP.restart();
                            }
                            http_ota->stop();
                            return SUCCESS;
                        }
                    }
                }

                Serial.println("------------------------------ERROR------------------------------");
                Serial.printf("    ERROR CODE: %d", Update.getError());
                Serial.println("-----------------------------------------------------------------");
            }
            else
            {
                Serial.println("Not enough space available.");
            }
        }
        else
        {
            Serial.println("There was no content in the response");
        }
        http_ota->stop();
        return FAILED_TO_DOWNLOAD;
    }

    /**
     * @brief Continue with an update (likely modified) following a 302 redirect.
     * Behaves similar to performUpdate, but is used after defining new SSL certs as needed.
     * @return InstallCondition
     */
    InstallCondition continueRedirect(UpdateObject *details, bool restart = true)
    {
        reinit(*underlying_client, details->redirect_server.c_str(), OTAGH_PORT);
        return performUpdate(details, restart);
    }
#pragma endregion
}