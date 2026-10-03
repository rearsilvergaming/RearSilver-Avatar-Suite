#pragma once

#include "update_service.hpp"

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>

enum class UpdateDownloadStatus { Idle, Downloading, Verifying, Verified, Cancelled, Error };

struct UpdateDownloadProgress {
    UpdateDownloadStatus status = UpdateDownloadStatus::Idle;
	std::uint64_t bytesTransferred = 0;
	std::uint64_t bytesTotal = 0;
	std::wstring verifiedPath;
	std::string message;
};

using UpdateDownloadCallback = std::function<void(const UpdateDownloadProgress &)>;

// Downloads and verifies one manifest-approved installer. The bearer token is
// supplied at runtime and is never persisted or written to diagnostics.
void downloadAndVerifyAvatarUpdate(const UpdateCheckResult &update, const std::string &bearerToken,
	std::atomic<bool> &cancelRequested, const UpdateDownloadCallback &callback);
bool validAvatarUpdateContentRange(const std::wstring &value,std::uint64_t expectedStart,std::uint64_t expectedTotal);

