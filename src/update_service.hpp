#pragma once

#include <string>

struct UpdateFetchResult {
    bool manual = false;
    bool succeeded = false;
    std::string body;
    std::string error;
};

UpdateFetchResult fetchUpdateManifest(const std::string &baseUrl,
                                      const std::string &channel,
                                      bool manual);
