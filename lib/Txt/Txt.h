#pragma once

#include <HalStorage.h>

#include <memory>
#include <string>

class Txt {
  std::string filepath;
  std::string cacheBasePath;
  std::string cachePath;
  // Identity path used for navigation/recents. Defaults to filepath, but a wrapper format
  // (e.g. JWPUB, which renders a generated content.txt) can point this at the original book
  // so "back" returns to the book's folder rather than the cache.
  std::string bookPath;
  bool loaded = false;
  size_t fileSize = 0;

 public:
  explicit Txt(std::string path, std::string cacheBasePath, std::string bookPath = "");

  bool load();
  [[nodiscard]] const std::string& getPath() const { return filepath; }
  [[nodiscard]] const std::string& getBookPath() const { return bookPath; }
  [[nodiscard]] const std::string& getCachePath() const { return cachePath; }
  [[nodiscard]] std::string getTitle() const;
  [[nodiscard]] size_t getFileSize() const { return fileSize; }

  void setupCacheDir() const;
  bool clearCache() const;

  // Cover image support - looks for cover.bmp/jpg/jpeg/png in same folder as txt file
  [[nodiscard]] std::string getCoverBmpPath() const;
  [[nodiscard]] bool generateCoverBmp() const;
  [[nodiscard]] std::string findCoverImage() const;

  // Read content from file
  [[nodiscard]] bool readContent(uint8_t* buffer, size_t offset, size_t length) const;
};
