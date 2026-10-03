#pragma once

#include <cstdint>
#include <string>
#include <vector>
enum class UpdateCheckStatus { Disabled, Checking, UpToDate, Available, Error };
struct UpdateCheckResult {
    UpdateCheckStatus status=UpdateCheckStatus::Disabled;
    bool manual=false,mandatory=false,currentVersionSupported=true,downloadAvailable=false;
    std::string availableVersion,publishedAt,releaseNotesUrl,installerFilename,installerSha256,downloadRequestUrl,message;
    std::vector<std::string> releaseNotes;
    std::uint64_t installerSize=0;
};
UpdateCheckResult checkForAvatarUpdate(const std::string &baseUrl,const std::string &channel,const std::string &currentVersion,bool manual);
UpdateCheckResult parseAvatarUpdateManifest(const std::string &manifest,const std::string &baseUrl,const std::string &channel,const std::string &currentVersion,bool manual);
