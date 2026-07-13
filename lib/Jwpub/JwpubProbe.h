#pragma once

#include <string>

// Phase-1 bring-up harness for JWPUB support.
//
// Runs the full "hard path" against a real .jwpub on the SD card and logs the result of
// each step, WITHOUT any UI or reader integration:
//   outer ZIP -> manifest.json -> key derivation -> inner `contents` ZIP -> SQLite `.db`
//   -> first Document.Content BLOB -> AES-128-CBC decrypt -> zlib inflate -> HTML.
//
// This exists to validate, on device, the two things that cannot be checked in this
// environment: the exact manifest field names / key-derivation recipe, and the MiniSqlite
// reader against a genuine database. A correct decrypt yields a zlib stream (first byte
// 0x78) that inflates to HTML. Call JwpubProbe::run() from a debug hook during bring-up;
// it is not wired into the normal reader flow.
namespace JwpubProbe {

// Returns true if the pipeline produced plausible inflated HTML from the first document.
// `scratchDir` must be a writable directory (e.g. "/.crosspoint") for staging the extracted
// inner ZIP and database; staged files are removed on completion.
bool run(const std::string& jwpubPath, const std::string& scratchDir = "/.crosspoint");

}  // namespace JwpubProbe
