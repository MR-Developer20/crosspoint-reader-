#include "JwpubManifest.h"

#include <ArduinoJson.h>
#include <Logging.h>

bool JwpubManifest::parse(const char* json) {
  if (json == nullptr) {
    return false;
  }

  // Filter so the document holds only the handful of scalar fields we need, not the large
  // `images` / table-of-contents arrays that also live in the manifest.
  JsonDocument filter;
  filter["name"] = true;
  filter["hash"] = true;
  filter["contentFormat"] = true;
  JsonObject pubFilter = filter["publication"].to<JsonObject>();
  pubFilter["title"] = true;
  pubFilter["symbol"] = true;
  pubFilter["year"] = true;
  pubFilter["issueTagNumber"] = true;
  pubFilter["language"] = true;

  JsonDocument doc;
  const DeserializationError error = deserializeJson(doc, json, DeserializationOption::Filter(filter));
  if (error) {
    LOG_ERR("JWMANIFEST", "JSON parse failed: %s", error.c_str());
    return false;
  }

  name = doc["name"].as<std::string>();
  hash = doc["hash"].as<std::string>();
  contentFormat = doc["contentFormat"].as<std::string>();

  JsonObjectConst pub = doc["publication"];
  if (pub.isNull()) {
    LOG_ERR("JWMANIFEST", "no publication object in manifest");
    return false;
  }
  title = pub["title"].as<std::string>();
  symbol = pub["symbol"].as<std::string>();
  year = pub["year"].as<int>();
  issueTagNumber = pub["issueTagNumber"] | 0;                 // absent -> 0
  mepsLanguageIndex = pub["language"] | -1;                   // absent -> -1 (invalid)

  LOG_DBG("JWMANIFEST", "symbol=%s year=%d issue=%d lang=%d fmt=%s", symbol.c_str(), year, issueTagNumber,
          mepsLanguageIndex, contentFormat.c_str());
  return true;
}
