#pragma once

#include <cstdint>
#include <string>
#include <vector>

// Streaming CommonMark-core -> XHTML converter. Feeds lib/Markdown/Markdown.cpp, which
// packages the result as a synthetic EPUB so a .md file renders through the existing EPUB
// engine (lib/Epub) instead of a second layout engine. Reads the source and writes the
// output in small bounded chunks; never holds the whole document in RAM.
//
// This is a practical subset of CommonMark, not a full implementation. Simplifications,
// all chosen to keep the parser single-pass and bounded-memory while still covering the
// overwhelming majority of real-world Markdown:
//
//  * Headings are ATX only (`# Heading`). Setext headings (`Heading\n=====`) are not
//    recognized — the underline just renders as its own line/thematic-break — because
//    recognizing them needs one line of lookback after already having streamed the
//    preceding paragraph text out.
//  * No raw HTML passthrough: a literal '<', '>' or '&' in text is escaped and shown as-is
//    rather than interpreted as a tag.
//  * Blockquotes, list items, and code blocks don't nest inside each other — whichever
//    block type a line matches wins, and item/quote *content* is single-paragraph inline
//    text (no nested lists/quotes/code within an item).
//  * List nesting depth is derived from indentation width (~2 spaces per level), not a
//    full marker-alignment algorithm, and is capped at 4 levels.
//  * Ordered-list numbers are always auto-incremented from 1 at the start of each list run;
//    an explicit starting number in the source (e.g. "5. foo") is not honored.
//  * Inline emphasis/strong/code spans must open and close within the same source line —
//    they don't span a soft line break inside a paragraph.
//  * Only inline links/images are supported (`[text](url)`, `![alt](src)`); reference-style
//    links are not. Link targets are dropped (shown as plain text) since there's no browser
//    to follow them; images are resolved and embedded only when they're a local relative
//    JPEG/PNG file next to the source document.
class MarkdownToXhtml {
 public:
  struct Heading {
    uint8_t level;  // 1-6
    std::string text;
    std::string anchorId;  // "h0", "h1", ... in document order
  };

  // A locally-embeddable image referenced by the document.
  struct ImageRef {
    std::string sdPath;        // full path to the source image on SD
    std::string zipEntryName;  // "OEBPS/images/imgN.ext" — matches the <img src> emitted
  };

  struct Result {
    bool ok = false;
    uint32_t crc32 = 0;   // of the generated XHTML written to dstPath
    uint32_t size = 0;    // bytes written to dstPath
    std::string firstH1;  // raw text of the first top-level heading, empty if none
    std::vector<Heading> headings;
    std::vector<ImageRef> images;
  };

  // Converts `srcPath` (a .md file already on SD) into a complete XHTML document written to
  // `dstPath` (created/overwritten).
  static Result convert(const std::string& srcPath, const std::string& dstPath);
};
