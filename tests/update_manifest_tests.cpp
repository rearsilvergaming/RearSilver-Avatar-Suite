#include "update_service.hpp"
#include <cstdlib>
#include <iostream>
#include <string>

namespace {
const std::string valid = R"({"schema":1,"product":"rearsilver-avatar-suite","channel":"owner-build","platform":"windows-x64","version":"1.0.0-owner.6","minimum_supported_version":"1.0.0-owner.5","minimum_updater_schema":1,"mandatory":false,"published_at":"2026-10-03T12:00:00Z","installer":{"filename":"RearSilver-Avatar-Suite-Owner-1.0.0-owner.6-Setup.exe","size":1234,"sha256":"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa","download_request_url":"https://staging.example/v1/download/owner-build/1.0.0-owner.6/windows-x64"},"release_notes":["Test update"],"release_notes_url":""})";
void require(bool condition,const char*message){if(!condition){std::cerr<<message<<'\n';std::exit(1);}}
}

int main(){
 const auto good=parseAvatarUpdateManifest(valid,"https://staging.example/","Owner Build","1.0.0-owner.5",true);
 require(good.status==UpdateCheckStatus::Available&&good.downloadAvailable,"valid manifest was rejected");
 require(parseAvatarUpdateManifest("{","https://staging.example/","Owner Build","1.0.0-owner.5",false).status==UpdateCheckStatus::Error,"malformed JSON was accepted");
 auto missing=valid;missing.replace(missing.find("\"product\""),9,"\"missing\"");
 require(parseAvatarUpdateManifest(missing,"https://staging.example/","Owner Build","1.0.0-owner.5",false).status==UpdateCheckStatus::Error,"missing required property was accepted");
 auto wrongType=valid;wrongType.replace(wrongType.find("\"schema\":1"),10,"\"schema\":\"1\"");
 require(parseAvatarUpdateManifest(wrongType,"https://staging.example/","Owner Build","1.0.0-owner.5",false).status==UpdateCheckStatus::Error,"wrong property type was accepted");
 auto wrongOrigin=valid;wrongOrigin.replace(wrongOrigin.find("https://staging.example/v1/download"),36,"https://other.example/v1/download");
 require(!parseAvatarUpdateManifest(wrongOrigin,"https://staging.example/","Owner Build","1.0.0-owner.5",false).downloadAvailable,"cross-origin download was accepted");
 auto duplicate=valid;duplicate.insert(1,"\"schema\":2,");
 require(parseAvatarUpdateManifest(duplicate,"https://staging.example/","Owner Build","1.0.0-owner.5",false).status==UpdateCheckStatus::Available,"Windows.Data.Json duplicate handling changed");
 std::cout<<"update manifest tests passed\n";
}
